/*
 * NexIOS RTOS — debugd pure protocol core, shared target/host (issue #295)
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
 * Pure RSP protocol core shared by the target loop (userspace/debugd.c)
 * and the host conformance tests (tools/debugd/tests/test_cproto.c):
 * checksums, hex codec, m-packet parsing, stop-reply mapping. Zero
 * syscall dependencies — freestanding-clean on both sides. The host
 * oracle (test_loop.cpp, C++) stays the normative §13 suite; this
 * header pins the C loop to the same wire decisions.
 */

#ifndef NEXIOS_DEBUGD_PROTO_H
#define NEXIOS_DEBUGD_PROTO_H

/* Stop kinds (kernel/debug/debug_stop.hpp). */
#define DBG_STOP_BREAKPOINT 1
#define DBG_STOP_STEP 2
#define DBG_STOP_FAULT 3
#define DBG_STOP_DEATH 4

/* RSP packet ceiling (qSupported PacketSize we advertise). */
#define DBG_RSP_MAX_PACKET 1024

/* Register-blob size follows the kernel arch-dependent GDB layout
 * (debug_regs.cpp: 164 x86_64 / 272 aarch64 / 264 riscv64), GDB order.
 * Compiler predefines only -- no build-system dependency. */
#if defined(__aarch64__)
#define DBG_BLOB_BYTES 272
#elif defined(__riscv)
#define DBG_BLOB_BYTES 264
#else
#define DBG_BLOB_BYTES 164
#endif

static unsigned dbg_hex_val(char c) {
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (unsigned)(c - 'A' + 10);
    return 16;
}

/* RSP checksum: sum mod 256, two lowercase hex digits. */
static void dbg_rsp_checksum(const char *data, unsigned long len,
                             char out[2]) {
    static const char hex[] = "0123456789abcdef";
    unsigned long sum = 0;
    unsigned long i = 0;

    for (i = 0; i < len; ++i)
        sum += (unsigned char)data[i];
    out[0] = hex[(sum >> 4) & 0xF];
    out[1] = hex[sum & 0xF];
}

static void dbg_hex_encode(const unsigned char *in, unsigned long len,
                           char *out) {
    static const char hex[] = "0123456789abcdef";
    unsigned long i = 0;

    for (i = 0; i < len; ++i) {
        out[2 * i] = hex[(in[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[in[i] & 0xF];
    }
    out[2 * len] = '\0';
}

/* Returns decoded length, or (unsigned long)-1 on bad hex. */
static unsigned long dbg_hex_decode(const char *in, unsigned long hexlen,
                                    unsigned char *out, unsigned long cap) {
    unsigned long i = 0;

    if ((hexlen & 1) != 0)
        return (unsigned long)-1;
    if (hexlen / 2 > cap)
        return (unsigned long)-1;
    for (i = 0; i < hexlen; i += 2) {
        unsigned hi = dbg_hex_val(in[i]);
        unsigned lo = dbg_hex_val(in[i + 1]);

        if (hi > 15 || lo > 15)
            return (unsigned long)-1;
        out[i / 2] = (unsigned char)((hi << 4) | lo);
    }
    return hexlen / 2;
}

/* Parse "AA,LL" hex pair (m command). Returns 0 on success. */
static int dbg_parse_addr_len(const char *p, unsigned long *addr,
                              unsigned long *len) {
    unsigned long a = 0;
    unsigned long l = 0;

    while (*p && *p != ',') {
        unsigned v = dbg_hex_val(*p++);

        if (v > 15)
            return -1;
        a = (a << 4) | v;
    }
    if (*p != ',')
        return -1;
    ++p;
    while (*p) {
        unsigned v = dbg_hex_val(*p++);

        if (v > 15)
            return -1;
        l = (l << 4) | v;
    }
    *addr = a;
    *len = l;
    return 0;
}

/* Map a stop event kind to an RSP stop reply. Single thread model
 * (§13/Q1): thread 1, consistent with qC. Returns reply length. */
static unsigned long dbg_stop_reply_for_kind(unsigned long kind,
                                             char *out) {
    unsigned long pos = 0;
    const char *sig = "T05";

    if (kind == DBG_STOP_DEATH) {
        out[0] = 'W';
        out[1] = '0';
        out[2] = '0';
        out[3] = '\0';
        return 3;
    }
    if (kind == DBG_STOP_FAULT)
        sig = "T0b";
    out[pos++] = sig[0];
    out[pos++] = sig[1];
    out[pos++] = sig[2];
    out[pos++] = 't';
    out[pos++] = 'h';
    out[pos++] = 'r';
    out[pos++] = 'e';
    out[pos++] = 'a';
    out[pos++] = 'd';
    out[pos++] = ':';
    out[pos++] = '1';
    out[pos++] = ';';
    out[pos] = '\0';
    return pos;
}

#endif /* NEXIOS_DEBUGD_PROTO_H */
