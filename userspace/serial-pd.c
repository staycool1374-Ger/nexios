/* Driver PD: serial (issue #48 strangler step a + issue #51 step-c TX flip
 * + step-c-prime SHM-ring bulk TX).
 *
 * BIND conveys the PD's UART cap (arg0 = encoded MmioCap handle,
 * arg1 = phys base, arg2 = size, flags = cap count); the PD maps it once
 * (x86: SYS_IOPORT_GRANT + raw outb/inb; aarch64/riscv64: SYS_MMIO_MAP +
 * volatile MMIO) and serves polled DRV_OP_WRITE. MAP_SHM conveys the TX
 * ring FrameCap (arg0 = encoded handle, arg1 = page count, arg2 = element
 * size); the PD SYS_FRAME_MAPs it once and drains ring→UART every loop
 * iteration (1-tick recv poll when mapped, block-forever when not — zero
 * idle overhead before the channel is live). Replies go to req.owner_id
 * (the send_sync requester queue — reply-to-1 is a dead letter, issue
 * #441 pattern). No logging on the serve path: the PD must never block
 * on a lock its kernel client may hold (§11.1).
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

/* TX SHM ring consumer (issue #51 step-c-prime). Layout mirrors the
 * kernel SharedRingHeader (shm_ring.hpp): u64 magic/version/capacity/
 * element_size, then 64 B-aligned head (producer) and tail (consumer),
 * then 64 B-aligned drain_hold (kernel test quiesce hook, nonzero =
 * skip pops). All shared fields are read with acquire semantics; the
 * tail publish uses release after the payload copy (SPSC protocol). */
struct tx_ring_header {
    uint64_t magic;
    uint64_t version;
    uint64_t capacity;
    uint64_t element_size;
    uint8_t pad1[32];
    volatile uint64_t head;
    uint8_t pad2[56];
    volatile uint64_t tail;
    uint8_t pad3[56];
    volatile uint64_t drain_hold;
    uint8_t pad4[56];
};

static unsigned long g_tx_handle = 0;
static unsigned long g_tx_va = 0;
static unsigned long g_tx_pages = 0;
static int g_tx_mapped = 0;

/* Forward: UART byte emitter defined below (per-arch). */
static int uart_putc(unsigned char ch);

static struct tx_ring_header *tx_hdr(void) {
    return (struct tx_ring_header *)g_tx_va;
}

static int tx_valid(void) {
    struct tx_ring_header *h = tx_hdr();
    uint64_t magic = __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE);
    uint64_t version = __atomic_load_n(&h->version, __ATOMIC_ACQUIRE);
    uint64_t cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&h->element_size, __ATOMIC_ACQUIRE);
    if (magic != SERIAL_TX_RING_MAGIC || version != SERIAL_TX_RING_VERSION)
        return 0;
    if (cap == 0 || (cap & (cap - 1)) != 0)
        return 0; /* capacity must be a nonzero power of two (wrap mask) */
    if (esz != SERIAL_TX_RING_ESZ)
        return 0;
    return 1;
}

/* Pop one element into dst (SERIAL_TX_RING_ESZ bytes). Returns 1 on
 * data, 0 on empty/invalid. */
static int tx_pop(unsigned char *dst) {
    struct tx_ring_header *h = tx_hdr();
    uint64_t cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&h->element_size, __ATOMIC_ACQUIRE);
    if (cap == 0 || esz == 0)
        return 0;
    uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return 0;
    unsigned char *base = (unsigned char *)g_tx_va + 4096;
    unsigned long off = (unsigned long)(((tail / esz) * esz) & (cap - 1));
    unsigned char *src = base + off;
    for (unsigned long i = 0; i < esz; i++)
        dst[i] = src[i];
    __atomic_store_n(&h->tail, tail + esz, __ATOMIC_RELEASE);
    return 1;
}

/* Drain the ring to UART, preserving order. Stops (fail-closed) on the
 * first UART stall, leaving the remainder for the next loop iteration;
 * bytes already popped were already emitted, so no duplication. Skips
 * entirely while the kernel test quiesce hook holds the drain (exact
 * producer drop accounting — issue #51 QE finding). */
static void tx_drain(void) {
    unsigned char elem[SERIAL_TX_RING_ESZ];
    if (!g_tx_mapped)
        return; /* no ring yet: header reads would fault (NULL VA) */
    if (__atomic_load_n(&tx_hdr()->drain_hold, __ATOMIC_ACQUIRE) != 0)
        return; /* quiesced: leave everything for the release */
    while (tx_pop(elem)) {
        unsigned long n = elem[0];
        if (n > SERIAL_TX_RING_ESZ - 1)
            n = SERIAL_TX_RING_ESZ - 1; /* hostile len: clamp, never OOB */
        for (unsigned long i = 0; i < n; i++) {
            if (uart_putc(elem[1 + i]) != 0)
                return;
        }
    }
}

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
        tx_drain();
        /* 1-tick poll once the ring is live (bounded drain latency for
         * both RPCs and TX bytes); block forever before MAP (zero idle
         * overhead — the NOTIFY is a latency optimization, the poll is
         * the liveness backstop, so a lost notify costs one tick). */
        int rc = drv_recv(rbuf, sizeof(rbuf), g_tx_mapped ? 1 : 0);
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
        case DRV_OP_MAP_SHM: {
            unsigned long pages = (unsigned long)req.arg1;
            /* Wire contract: fixed 8 B elements (kernel
             * kSerialTxRingElemSize); anything else is a version skew —
             * fail closed, never map a ring we cannot parse. */
            if ((unsigned long)req.arg2 != SERIAL_TX_RING_ESZ || pages < 2) {
                rep.status = (uint16_t)(uint16_t)-22;
                break;
            }
            if (g_tx_mapped && (unsigned long)req.arg0 == g_tx_handle) {
                rep.arg0 = (uint64_t)1; /* idempotent re-MAP */
                break;
            }
            if (g_tx_mapped) {
                /* New handle under us (restart re-derive): drop the old
                 * mapping before taking the new one — never double-map. */
                __syscall5(SYS_FRAME_UNMAP, (long)g_tx_va, 0, 0, 0);
                g_tx_mapped = 0;
                g_tx_handle = 0;
                g_tx_va = 0;
                g_tx_pages = 0;
            }
            long va = __syscall5(SYS_FRAME_MAP, (long)req.arg0, 0, 0, 0);
            if (va == 0 || va == -1) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            g_tx_va = (unsigned long)va;
            if (!tx_valid()) {
                /* Torn/hostile header: unmap, never consume. The frames
                 * stay composed; a later re-MAP may succeed. */
                __syscall5(SYS_FRAME_UNMAP, (long)g_tx_va, 0, 0, 0);
                g_tx_va = 0;
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            g_tx_handle = (unsigned long)req.arg0;
            g_tx_pages = pages;
            g_tx_mapped = 1;
            rep.arg0 = (uint64_t)1;
            break;
        }
        default:
            rep.status = (uint16_t)(uint16_t)-38;
            break;
        }
        ipc_send(dest, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
