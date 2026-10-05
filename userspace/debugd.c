/*
 * NexIOS RTOS — GDB debug daemon target runtime (issue #295)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * debugd — userspace GDB remote stub (issue #295, spec §13 v1).
 *
 * Single-threaded RSP loop over /dev/tty (O_RAWTTY: no CR mapping, no
 * keyboard merge — any byte mangling invalidates RSP framing, issue
 * #295/O1) driving the kernel debug selectors (frozen numbers,
 * src/libc/syscall.h). Conformance bar: tools/debugd host oracle.
 *
 * Modelled on userspace/watchdogd.c: statics only, bounded everything,
 * no blocking primitive without a deadline. Differences:
 * - ZERO console output, ever (not even a startup banner): the serial
 *   line carries RSP framing from the first byte a host GDB may be
 *   listening; Logger info/warn already mute on live sessions (#232).
 * - Targets arrive via IPC grants minted by `runelf --debug` (§13.6:
 *   the loop never pid-scans). One active target (refuse-EBUSY on a
 *   second concurrent grant, spec Q5).
 * - EAGAIN discipline (§3): runnable targets arm the deferred stop and
 *   ask for retry — the controller re-polls instead of touching frames.
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <syscall.h>
#include <ipc.h>

/* Debug syscall numbers (frozen, src/libc/syscall.h). */
#define DBG_ATTACH 86
#define DBG_READ_REGS 87
#define DBG_WRITE_REGS 88
#define DBG_READ_MEM 89
#define DBG_WRITE_MEM 90

/* Attach selectors (spec §9.2 grounding). */
#define DBG_SEL_DETACH 0
#define DBG_SEL_CLAIM 1
#define DBG_SEL_POLL 2
#define DBG_SEL_BP_INSERT 3
#define DBG_SEL_BP_CLEAR 4
#define DBG_SEL_STEP 5
#define DBG_SEL_CONT 6
#define DBG_SEL_STOP 9

/* Shell -> debugd grant delivery (mirrors WDOG_SUPERVISE 300-302). */
#define DEBUG_TARGET 310

/* Stop kinds, packet ceiling, blob size, and the pure protocol core
 * (checksums, hex codec, stop replies) live in the shared header so
 * the host conformance tests pin the same wire decisions. */
#include "debugd_proto.h"

/* Linux errno numerics the kernel returns negated (cf. sel handlers). */
#define NEG_EPERM ((unsigned long)-1)
#define NEG_ESRCH ((unsigned long)-3)
#define NEG_EBADF ((unsigned long)-9)
#define NEG_EAGAIN ((unsigned long)-11)
#define NEG_EFAULT ((unsigned long)-14)
#define NEG_EINVAL ((unsigned long)-22)
/* Bounded waits (ticks at 1 kHz). */
#define RSP_BYTE_TIMEOUT_TICKS 1000
#define RSP_ACK_TIMEOUT_TICKS 1000
#define RSP_POLL_DEADLINE_TICKS 2000
#define RSP_ACK_RETRIES 3
/* Session watchdog: no RSP byte for this long with a live handle means
 * the host went away — detach and release the console (the shell mutes
 * while a session is live; an abandoned mute would brick the console
 * until reboot). Ticks at 1 kHz. */
#define DBG_SESSION_IDLE_TICKS 3600000

struct DbgStopEvent {
    uint64_t target_id;
    uint64_t target_gen;
    uint64_t kind;
    uint64_t fault_num;
    uint64_t fault_addr;
    uint64_t snap_state;
    uint64_t snap_prio;
    uint64_t snap_budget;
};

static int g_tty_fd = -1;
static unsigned long g_handle = 0;
static unsigned long g_target_pid = 0;
static unsigned long g_continue_outstanding = 0;
static unsigned long g_last_stop_kind = 0;
/* Tick of the last RSP byte in either direction: proof the host is
 * alive (session watchdog below). */
static unsigned long g_last_byte_tick = 0;

/* Round up: 164 is not a multiple of 8, and the kernel copies
 * debug_blob_bytes() (164/272/264 by arch) here. */
static unsigned long g_reg_qwords[(DBG_BLOB_BYTES + 7) / 8];
static unsigned char g_mem_buf[1024];
static struct DbgStopEvent g_stop_ev;
static char g_packet[DBG_RSP_MAX_PACKET + 1];
/* Reply ceiling: worst case is the hex of a full g_mem_buf read
 * (2*1024+1); the register-blob hex (2*272+1 on aarch64) and every
 * control reply fit inside the same bound. */
static char g_reply[2 * sizeof(g_mem_buf) + 64];

static unsigned long dbg_ticks(void) {
    return (unsigned long)__syscall5(5, 0, 0, 0, 0);
}

static void dbg_yield(void) {
    __syscall5(0, 0, 0, 0, 0);
}

static unsigned long dbg_attach(unsigned long sel, unsigned long a0,
                                unsigned long a1) {
    return (unsigned long)__syscall5(DBG_ATTACH, (long)sel, (long)a0,
                                     (long)a1, 0);
}

/* RSP checksum: sum mod 256, two lowercase hex digits. */

static int transport_write_all(const char *data, unsigned long len) {
    unsigned long off = 0;

    while (off < len) {
        long r = write(g_tty_fd, data + off, (size_t)(len - off));

        if (r <= 0)
            return -1;
        off += (unsigned long)r;
    }
    return 0;
}

/* Bounded single-byte read: 0 = byte stored, -1 = timeout/error.
 * Every received byte feeds the session watchdog (host alive). */
static int transport_read_byte(char *out, unsigned long timeout_ticks) {
    unsigned long deadline = dbg_ticks() + timeout_ticks;
    char c = 0;

    for (;;) {
        long r = read(g_tty_fd, &c, 1);

        if (r == 1) {
            *out = c;
            g_last_byte_tick = dbg_ticks();
            return 0;
        }
        if ((long)(dbg_ticks() - deadline) >= 0)
            return -1;
        dbg_yield();
    }
}

/* Send a packet, waiting for host ACK with bounded retransmits (§13.2:
 * sender retransmits on `-` or timeout; give up after RSP_ACK_RETRIES
 * rather than hanging the loop forever). Returns 0 if ACKed. */
static int rsp_send_packet(const char *data, unsigned long len) {
    char csum[2];
    int attempt = 0;

    /* Send ceiling is the reply buffer, not the advertised PacketSize:
     * a full 1024-byte `m` read builds a 2048-char hex reply, and
     * refusing it here would leave the host hanging with no reply at
     * all (audit #295: fail-closed liveness). RSP framing is
     * self-delimiting ($...#cs); GDB sizes its requests by PacketSize
     * and accepts longer replies. Every caller builds at most
     * 2*sizeof(g_mem_buf)+1 bytes, inside g_reply. */
    if (len > sizeof(g_reply) - 1)
        return -1;
    dbg_rsp_checksum(data, len, csum);
    for (attempt = 0; attempt < RSP_ACK_RETRIES; ++attempt) {
        char c = 0;

        if (transport_write_all("$", 1) != 0)
            return -1;
        if (transport_write_all(data, len) != 0)
            return -1;
        if (transport_write_all("#", 1) != 0)
            return -1;
        if (transport_write_all(csum, 2) != 0)
            return -1;
        if (transport_read_byte(&c, RSP_ACK_TIMEOUT_TICKS) != 0)
            continue;
        if (c == '+')
            return 0;
        /* `-` or anything else: retransmit. */
    }
    return -1;
}

static int rsp_send_empty(void) {
    return rsp_send_packet("", 0);
}

static int rsp_send_ack(void) {
    return transport_write_all("+", 1);
}

static int rsp_send_nack(void) {
    return transport_write_all("-", 1);
}

/* Ensure the target is stopped, then report why (§13: GDB `?` must
 * observe a real stop, never an assumed one). Arms sel9 (no-op when
 * already parked), then polls bounded. Falls back to TRAP. */
static int dbg_poll_stop(unsigned long timeout_ticks) {
    unsigned long deadline = dbg_ticks() + timeout_ticks;

    for (;;) {
        unsigned long r = (unsigned long)__syscall5(
            DBG_ATTACH, DBG_SEL_POLL, (long)g_handle,
            (long)&g_stop_ev, 0);

        if (r == 0)
            return 0;
        if ((long)(dbg_ticks() - deadline) >= 0)
            return -1;
        dbg_yield();
    }
}

/* Ensure the target is stopped, then report why (§13: GDB `?` must
 * observe a real stop, never an assumed one). Arms sel9 (no-op when
 * already parked), then polls bounded. Falls back to TRAP. */
static int handle_query_stop(void) {
    unsigned long r = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    dbg_attach(DBG_SEL_STOP, g_handle, 0);
    if (dbg_poll_stop(RSP_POLL_DEADLINE_TICKS) == 0) {
        g_last_stop_kind = g_stop_ev.kind;
        g_continue_outstanding = 0;
        r = dbg_stop_reply_for_kind(g_stop_ev.kind, g_reply);
    } else {
        g_last_stop_kind = DBG_STOP_BREAKPOINT;
        r = dbg_stop_reply_for_kind(DBG_STOP_BREAKPOINT, g_reply);
    }
    return rsp_send_packet(g_reply, r);
}

static int handle_read_regs(void) {
    unsigned long r = 0;
    unsigned long tries = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    /* EAGAIN discipline (§3): runnable targets arm the deferred stop;
     * re-poll bounded instead of touching frames. */
    for (tries = 0; tries < 4; ++tries) {
        r = (unsigned long)__syscall5(DBG_READ_REGS, (long)g_handle,
                                      (long)g_reg_qwords, 0, 0);
        if (r != NEG_EAGAIN)
            break;
        dbg_yield();
    }
    if (r != 0)
        return rsp_send_packet("E01", 3);
    dbg_hex_encode((const unsigned char *)g_reg_qwords, DBG_BLOB_BYTES,
               g_reply);
    return rsp_send_packet(g_reply, 2 * DBG_BLOB_BYTES);
}

static int handle_write_regs(const char *hex, unsigned long hexlen) {
    unsigned long n = 0;
    unsigned long r = 0;
    unsigned long tries = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    n = dbg_hex_decode(hex, hexlen, (unsigned char *)g_reg_qwords,
                   sizeof(g_reg_qwords));
    if (n != DBG_BLOB_BYTES)
        return rsp_send_packet("E01", 3);
    for (tries = 0; tries < 4; ++tries) {
        r = (unsigned long)__syscall5(DBG_WRITE_REGS, (long)g_handle,
                                      (long)g_reg_qwords, 0, 0);
        if (r != NEG_EAGAIN)
            break;
        dbg_yield();
    }
    if (r != 0)
        return rsp_send_packet("E01", 3);
    return rsp_send_packet("OK", 2);
}

static int handle_read_mem(const char *args) {
    unsigned long addr = 0;
    unsigned long len = 0;
    unsigned long r = 0;
    unsigned long tries = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    if (dbg_parse_addr_len(args, &addr, &len) != 0 || len > sizeof(g_mem_buf))
        return rsp_send_packet("E01", 3);
    for (tries = 0; tries < 4; ++tries) {
        r = (unsigned long)__syscall5(DBG_READ_MEM, (long)g_handle,
                                      (long)addr, (long)len,
                                      (long)g_mem_buf);
        if (r != NEG_EAGAIN)
            break;
        dbg_yield();
    }
    /* Partial counts: the kernel returns bytes read; EFAULT only when
     * nothing at all was readable. */
    if (r == NEG_EFAULT || r == NEG_EINVAL || r > (unsigned long)len)
        return rsp_send_packet("E01", 3);
    if (r == NEG_EAGAIN)
        return rsp_send_packet("E01", 3);
    dbg_hex_encode(g_mem_buf, r, g_reply);
    return rsp_send_packet(g_reply, 2 * r);
}

static int handle_write_mem(const char *args) {
    unsigned long addr = 0;
    unsigned long hexlen = 0;
    unsigned long n = 0;
    unsigned long r = 0;
    unsigned long tries = 0;
    const char *colon = args;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    while (*colon && *colon != ':')
        ++colon;
    if (*colon != ':')
        return rsp_send_packet("E01", 3);
    {
        char lenbuf[24];
        unsigned long l = 0;

        /* Length field between ',' and ':' — re-parse bounded. */
        const char *comma = args;

        while (*comma && *comma != ',')
            ++comma;
        if (*comma != ',' || (unsigned long)(colon - comma - 1) >=
                                 sizeof(lenbuf))
            return rsp_send_packet("E01", 3);
        for (l = 0; comma + 1 + l < colon; ++l)
            lenbuf[l] = comma[1 + l];
        lenbuf[l] = '\0';
        {
            unsigned long v = 0;
            unsigned long k = 0;

            for (k = 0; lenbuf[k]; ++k) {
                unsigned d = dbg_hex_val(lenbuf[k]);

                if (d > 15)
                    return rsp_send_packet("E01", 3);
                v = (v << 4) | d;
                /* Bound the accumulator (audit #295 S3): lenbuf holds
                 * at most 23 digits, so v can wrap past any later
                 * range check; any length unsatisfiable from a bounded
                 * packet fails closed here instead. */
                if (v > sizeof(g_mem_buf))
                    return rsp_send_packet("E01", 3);
            }
            hexlen = 2 * v;
        }
    }
    /* Address part re-parse (before ','). */
    {
        unsigned long a = 0;
        const char *p = args;

        while (*p && *p != ',') {
            unsigned d = dbg_hex_val(*p++);

            if (d > 15)
                return rsp_send_packet("E01", 3);
            a = (a << 4) | d;
        }
        addr = a;
    }
    n = 0;
    {
        /* Declared length must fit the actual post-colon bytes (audit
         * #295 S3): g_packet is NUL-terminated, so anything past the
         * terminator is an over-read of adjacent statics (rejected
         * E01 below, nothing exfiltrated — but never read it). */
        unsigned long avail = 0;
        const char *eol = colon + 1;
        while (*eol++)
            ++avail;
        if (hexlen > avail)
            return rsp_send_packet("E01", 3);
    }
    n = dbg_hex_decode(colon + 1, hexlen, g_mem_buf, sizeof(g_mem_buf));
    if (n == (unsigned long)-1)
        return rsp_send_packet("E01", 3);
    for (tries = 0; tries < 4; ++tries) {
        r = (unsigned long)__syscall5(DBG_WRITE_MEM, (long)g_handle,
                                      (long)addr, (long)n,
                                      (long)g_mem_buf);
        if (r != NEG_EAGAIN)
            break;
        dbg_yield();
    }
    if (r != n)
        return rsp_send_packet("E01", 3);
    return rsp_send_packet("OK", 2);
}

static int handle_break(const char *args, int insert) {
    unsigned long addr = 0;
    unsigned long len = 0;
    unsigned long r = 0;
    unsigned long sel = insert ? (unsigned long)DBG_SEL_BP_INSERT
                               : (unsigned long)DBG_SEL_BP_CLEAR;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    /* "Z0,addr,kind" — kind ignored (software breakpoints only, v1). */
    {
        const char *p = args;
        unsigned long a = 0;

        while (*p && *p != ',') {
            unsigned d = dbg_hex_val(*p++);

            if (d > 15)
                return rsp_send_packet("E01", 3);
            a = (a << 4) | d;
        }
        addr = a;
        len = 0;
    }
    r = dbg_attach(sel, g_handle, addr);
    if (r != 0)
        return rsp_send_packet("E01", 3);
    return rsp_send_packet("OK", 2);
}

/* Continue: sel6 resumes (RUNNING target = no-op success); the stop
 * reply arrives asynchronously when the target parks. No packet now. */
static int handle_continue(void) {
    unsigned long r = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    r = dbg_attach(DBG_SEL_CONT, g_handle, 0);
    if (r != 0)
        return rsp_send_packet("E01", 3);
    g_continue_outstanding = 1;
    return 0;
}

/* Step: sel5 on a parked target steps synchronously-ish (bounded poll
 * for the step stop); on a runnable target it arms + EAGAIN, so poll
 * for the resulting park the same way. */
static int handle_step(void) {
    unsigned long r = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    r = dbg_attach(DBG_SEL_STEP, g_handle, 0);
    if (r != 0 && r != NEG_EAGAIN)
        return rsp_send_packet("E01", 3);
    if (dbg_poll_stop(RSP_POLL_DEADLINE_TICKS) != 0)
        return rsp_send_packet("E01", 3);
    g_last_stop_kind = g_stop_ev.kind;
    g_continue_outstanding = 0;
    r = dbg_stop_reply_for_kind(g_stop_ev.kind, g_reply);
    return rsp_send_packet(g_reply, r);
}

static int handle_detach(int kill_target) {
    unsigned long r = 0;

    if (g_handle == 0)
        return rsp_send_packet("E01", 3);
    r = dbg_attach(DBG_SEL_DETACH, g_handle, 0);
    /* Raw-pid kill is safe within a live session window: task ids are
     * monotonic (no reuse while live), so a stale g_target_pid
     * (admission denied, target already reaped) names a dead task and
     * the kill is a harmless ESRCH — it cannot alias a live unrelated
     * task. (Snapshot restore can rewind the id counter, but skip-live
     * assignment never hands an in-use id to a new task, so the window
     * where this runs stays safe. Audit #295 S3.) */
    if (kill_target && g_target_pid != 0)
        sys_kill((pid_t)g_target_pid, SIGKILL);
    g_handle = 0;
    g_target_pid = 0;
    g_continue_outstanding = 0;
    g_last_stop_kind = 0;
    if (r != 0)
        return rsp_send_packet("E01", 3);
    return rsp_send_packet("OK", 2);
}

/* Ctrl-C (§13.3): stop injection only while a continue is outstanding;
 * idle heartbeat otherwise. Never a global attach/broadcast/detach. */
static void handle_ctrl_c(void) {
    if (g_continue_outstanding && g_handle != 0)
        dbg_attach(DBG_SEL_STOP, g_handle, 0);
}

static int handle_query(const char *packet) {
    if (packet[1] == 'C') /* qC: current thread */
        return rsp_send_packet("QC1", 3);
    if (packet[1] == 'f') /* qfThreadInfo: single thread */
        return rsp_send_packet("m1", 2);
    if (packet[1] == 's') /* qsThreadInfo: end of list */
        return rsp_send_packet("l", 1);
    if (packet[1] == 'S') {
        /* qSupported: minimal viable set; no-ack NOT offered (host
         * keeps ACKing, which our retransmit loop relies on). */
        static const char qsup[] =
            "PacketSize=1024;qXfer:features:read-;"
            "QStartNoAckMode-;swbreak+;hwbreak-;"
            "vContSupported-";
        return rsp_send_packet(qsup, sizeof(qsup) - 1);
    }
    return rsp_send_empty();
}

/* Dispatch one validated packet body (without $...#cs). */
static int dispatch_packet(const char *p) {
    switch (p[0]) {
    case '?':
        return handle_query_stop();
    case 'g':
        return handle_read_regs();
    case 'G':
        return handle_write_regs(p + 1, strlen(p + 1));
    case 'm':
        return handle_read_mem(p + 1);
    case 'M':
        return handle_write_mem(p + 1);
    case 'Z':
        if (p[1] == '0')
            return handle_break(p + 2, 1);
        return rsp_send_empty();
    case 'z':
        if (p[1] == '0')
            return handle_break(p + 2, 0);
        return rsp_send_empty();
    case 'c':
        return handle_continue();
    case 's':
        return handle_step();
    case 'D':
        return handle_detach(0);
    case 'k':
        return handle_detach(1);
    case 'H':
        /* Hg/Hc thread select: single-thread model, always OK. */
        return rsp_send_packet("OK", 2);
    case 'q':
        return handle_query(p);
    case '!':
        /* Extended-remote: decline, stay in basic remote mode. */
        return rsp_send_empty();
    default:
        return rsp_send_empty();
    }
}

/* One grant-delivery poll with a 1-tick bound (never block: the byte
 * loop must stay live for UART FIFO service — RSP retries cover any
 * overrun, corruption is impossible by checksum).
 *
 * sys_receive (nr 2) returns msg.type and copies only msg.data into
 * the buffer — never sender_id, never the header. The shell's grant
 * carries data[0] = target pid, data[1] = binding handle (16-byte
 * payload); decode little-endian u64s from the payload, never from a
 * kernel-Message-shaped struct (the old DbgGrantMsg overlay read the
 * handle from the priority slot and always armed handle 0). The grant
 * is one-way: the shell sends NONBLOCK and never waits, and the
 * sender id is not delivered, so no reply is addressed anywhere. */
static int poll_grants(void) {
    unsigned char payload[16];
    unsigned long target_pid = 0;
    unsigned long handle = 0;
    long r = 0;

    r = __syscall5(2, 0, (long)payload, (long)sizeof(payload), 1);
    if (r < 0)
        return 0;
    if (r != DEBUG_TARGET)
        return 0;
    if (g_handle != 0)
        return 0;
    memcpy(&target_pid, payload, 8);
    memcpy(&handle, payload + 8, 8);
    if (handle == 0)
        return 0;
    g_handle = handle;
    g_target_pid = target_pid;
    g_continue_outstanding = 0;
    g_last_stop_kind = 0;
    return 1;
}

/* Read one validated RSP packet into g_packet (NUL-terminated body).
 * Returns body length, or (unsigned long)-1 on timeout/framing error.
 * Ctrl-C bytes arriving mid-stream inject a stop and are skipped. */
static unsigned long read_packet(void) {
    char c = 0;
    unsigned long len = 0;
    unsigned long sum = 0;
    unsigned long want = 0;

    for (;;) {
        if (transport_read_byte(&c, RSP_BYTE_TIMEOUT_TICKS) != 0)
            return (unsigned long)-1;
        if (c == 0x03) {
            handle_ctrl_c();
            continue;
        }
        if (c == '$')
            break;
        /* Leading ACKs/junk (e.g. host '+'): ignore. */
    }
    sum = 0;
    while (len < DBG_RSP_MAX_PACKET) {
        if (transport_read_byte(&c, RSP_BYTE_TIMEOUT_TICKS) != 0)
            return (unsigned long)-1;
        if (c == 0x03) {
            handle_ctrl_c();
            continue;
        }
        if (c == '#')
            break;
        g_packet[len++] = c;
        sum += (unsigned char)c;
    }
    if (len >= DBG_RSP_MAX_PACKET)
        return (unsigned long)-1;
    g_packet[len] = '\0';
    {
        char hi = 0;
        char lo = 0;
        unsigned got = 0;

        if (transport_read_byte(&hi, RSP_BYTE_TIMEOUT_TICKS) != 0)
            return (unsigned long)-1;
        if (transport_read_byte(&lo, RSP_BYTE_TIMEOUT_TICKS) != 0)
            return (unsigned long)-1;
        if (dbg_hex_val(hi) > 15 || dbg_hex_val(lo) > 15)
            return (unsigned long)-1;
        got = (dbg_hex_val(hi) << 4) | dbg_hex_val(lo);
        if ((sum & 0xFF) != got) {
            rsp_send_nack();
            return (unsigned long)-1;
        }
    }
    (void)want;
    rsp_send_ack();
    return len;
}

/* Async stop check: while a continue is outstanding, poll sel2 without
 * blocking the byte loop; on a stop event, emit the stop packet now. */
static void poll_async_stop(void) {
    unsigned long r = 0;
    unsigned long rl = 0;

    if (!g_continue_outstanding || g_handle == 0)
        return;
    r = (unsigned long)__syscall5(DBG_ATTACH, DBG_SEL_POLL, (long)g_handle,
                                  (long)&g_stop_ev, 0);
    if (r != 0)
        return;
    g_last_stop_kind = g_stop_ev.kind;
    g_continue_outstanding = 0;
    rl = dbg_stop_reply_for_kind(g_stop_ev.kind, g_reply);
    rsp_send_packet(g_reply, rl);
}

/* Grant-arm bound before open (ticks at 1 kHz): the shell sends the
 * grant promptly after spawn; a missing grant means the launch failed
 * closed on the shell side (its slot was undone) — exit rather than
 * orphan-spin. */
#define GRANT_ARM_TIMEOUT_TICKS 30000

int main(void) {
    /* Arm BEFORE open (send_sync cross-talk dodge, issue #295): the
     * open() authorizes via send_sync to vfsd, and send_sync pops the
     * oldest queued message as its "reply" without sender matching —
     * a still-queued grant (16 bytes < sizeof(Reply)) would fail that
     * authorize AND eat the grant. Consuming the grant first (plain
     * recv-poll, no authorize involved) leaves the inbox grant-free,
     * so the open authorize can only pop vfsd's own reply. Post-arm
     * senders to this inbox do not exist (one grant ever; vfsd replies
     * pair 1:1 with authorizes), so the order is deterministic. The
     * principled kernel fix (reply matching in send_sync) is tracked
     * separately; this ordering is correct with or without it. */
    g_last_byte_tick = dbg_ticks();
    {
        unsigned long deadline = dbg_ticks() + GRANT_ARM_TIMEOUT_TICKS;
        while (g_handle == 0 &&
               (long)(dbg_ticks() - deadline) < 0) {
            poll_grants();
            dbg_yield();
        }
        if (g_handle == 0)
            return 1;
    }
    /* No console output, ever: the serial line is RSP framing from the
     * first byte (see file header). */
    g_tty_fd = open("/dev/tty", O_RDWR | O_RAWTTY);
    if (g_tty_fd < 0)
        return 1;

    g_last_byte_tick = dbg_ticks();
    for (;;) {
        unsigned long len = 0;

        /* Grants first (§13.6 discovery): a DEBUG_TARGET grant arms the
         * session the RSP side then drives. Bounded IPC poll keeps the
         * byte loop live (watchdogd bounded-receive precedent). */
        poll_grants();
        /* Async stops from an outstanding continue go out unprompted. */
        poll_async_stop();
        /* Session watchdog: RSP traffic proves the host is alive; an
         * hour of silence with a live handle means it went away —
         * detach so the shell gets its console back. */
        if (g_handle != 0 &&
            (long)(dbg_ticks() - g_last_byte_tick) >
                (long)DBG_SESSION_IDLE_TICKS) {
            handle_detach(0);
            continue;
        }
        len = read_packet();
        if (len == (unsigned long)-1)
            continue;
        poll_grants();
        dispatch_packet(g_packet);
    }

    return 0;
}
