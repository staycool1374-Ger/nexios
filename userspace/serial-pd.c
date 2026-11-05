/* Driver PD: serial (issue #48 strangler step a + issue #51 step-c TX flip).
 *
 * BIND conveys the PD's UART cap (arg0 = encoded MmioCap handle,
 * arg1 = phys base, arg2 = size, flags = cap count); the PD maps it once
 * (x86: SYS_IOPORT_GRANT + raw outb/inb; aarch64/riscv64: SYS_MMIO_MAP +
 * volatile MMIO) and serves polled DRV_OP_WRITE. Replies go to
 * req.owner_id (the send_sync requester queue — reply-to-1 is a dead
 * letter, issue #441 pattern). No logging on the serve path: the PD must
 * never block on a lock its kernel client may hold (§11.1).
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include "driver_pd.h"

static uint64_t g_my_id;
static unsigned long g_uart_handle = 0;
static unsigned long g_uart_phys = 0;
static unsigned long g_uart_va = 0;
static int g_uart_mapped = 0;

/* 16550 LSR layout (x86 COM1 + riscv64 virt UART): register offsets from
 * the UART base conveyed by the kernel SDF row (arch::device COM1/UART0
 * literals stay kernel-owned; only spec-fixed offsets live here). */
#define UART_THR_OFF 0
#define UART_LSR_OFF 5
#define UART_LSR_THRE 0x20
/* PL011 layout (aarch64): DR + busy-wait on FR.TXFF. */
#define PL011_DR_OFF 0x00
#define PL011_FR_OFF 0x18
#define PL011_FR_TXFF 0x20
/* Bounded LSR poll (never spins forever; fail-closed -EAGAIN on stall). */
#define UART_POLL_BOUND 100000

#if defined(__x86_64__)
static inline void uart_outb(unsigned long port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"((uint16_t)port));
}
static inline unsigned char uart_inb(unsigned long port) {
    unsigned char val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"((uint16_t)port));
    return val;
}
#endif

static int uart_map(unsigned long handle, unsigned long phys,
                    unsigned long size) {
    if (g_uart_mapped && handle == g_uart_handle)
        return 0; /* idempotent re-BIND (epoch re-derive), no double grant */
    if (g_uart_mapped)
        return -1; /* handle changed under us: fail closed, never double-map */
#if defined(__x86_64__)
    (void)phys;
    if (size == 0)
        return -1;
    long rc = __syscall5(SYS_IOPORT_GRANT, (long)handle, (long)g_uart_phys,
                         (long)size, 0);
    if (rc != 0)
        return -1;
#else
    (void)phys;
    (void)size;
    long va = __syscall5(SYS_MMIO_MAP, (long)handle, 0, 0, 0);
    if (va == 0 || va == -1)
        return -1;
    g_uart_va = (unsigned long)va;
#endif
    g_uart_handle = handle;
    g_uart_mapped = 1;
    return 0;
}

static int uart_putc(unsigned char ch) {
#if defined(__x86_64__)
    unsigned long bound = UART_POLL_BOUND;
    while (!(uart_inb(g_uart_phys + UART_LSR_OFF) & UART_LSR_THRE)) {
        if (--bound == 0)
            return -1;
    }
    if (ch == '\n') {
        bound = UART_POLL_BOUND;
        while (!(uart_inb(g_uart_phys + UART_LSR_OFF) & UART_LSR_THRE)) {
            if (--bound == 0)
                return -1;
        }
        uart_outb(g_uart_phys + UART_THR_OFF, '\r');
    }
    uart_outb(g_uart_phys + UART_THR_OFF, ch);
    return 0;
#elif defined(__aarch64__)
    volatile unsigned int *fr =
        (volatile unsigned int *)(g_uart_va + PL011_FR_OFF);
    volatile unsigned int *dr =
        (volatile unsigned int *)(g_uart_va + PL011_DR_OFF);
    unsigned long bound = UART_POLL_BOUND;
    while ((*fr & PL011_FR_TXFF) != 0) {
        if (--bound == 0)
            return -1;
    }
    *dr = ch;
    return 0;
#else
    volatile unsigned char *lsr =
        (volatile unsigned char *)(g_uart_va + UART_LSR_OFF);
    volatile unsigned char *thr =
        (volatile unsigned char *)(g_uart_va + UART_THR_OFF);
    unsigned long bound = UART_POLL_BOUND;
    while (!(*lsr & UART_LSR_THRE)) {
        if (--bound == 0)
            return -1;
    }
    if (ch == '\n') {
        bound = UART_POLL_BOUND;
        while (!(*lsr & UART_LSR_THRE)) {
            if (--bound == 0)
                return -1;
        }
        *thr = '\r';
    }
    *thr = ch;
    return 0;
#endif
}

int main(void) {
    drv_assert_shape();
    g_my_id = (uint64_t)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = drv_recv(rbuf, sizeof(rbuf), 0);
        if (rc < 0)
            continue; // fail-closed: no reply on transport error
        struct DriverRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; // torn/stale payload: kernel type must match body op

        struct DriverRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        /* Reply target: the requester (owner_id), never PID 1 — a
         * send_sync client waits on its own queue (issues #51/#441). */
        unsigned long dest = req.owner_id != 0 ? req.owner_id : 1;
        switch (req.op) {
        case DRV_OP_BIND:
            if (req.flags < 1) {
                rep.status = (uint16_t)(uint16_t)-22;
                break;
            }
            g_uart_phys = (unsigned long)req.arg1;
            if (uart_map((unsigned long)req.arg0, g_uart_phys,
                         (unsigned long)req.arg2) != 0) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            rep.arg0 = (uint64_t)1;
            break;
        case DRV_OP_WRITE:
            if (!g_uart_mapped) {
                rep.status = (uint16_t)(uint16_t)-38;
                break;
            }
            if (uart_putc((unsigned char)req.arg0) != 0) {
                rep.status = (uint16_t)(uint16_t)-11;
                break;
            }
            rep.arg0 = (uint64_t)1;
            break;
        default:
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        ipc_send(dest, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
