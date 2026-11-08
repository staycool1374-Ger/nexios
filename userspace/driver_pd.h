#ifndef NEXIOS_DRIVER_PD_H
#define NEXIOS_DRIVER_PD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ipc.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    DRV_OP_BIND = 0,
    DRV_OP_READ = 1,
    DRV_OP_WRITE = 2,
    DRV_OP_POLL = 3,
    DRV_OP_MODE_SET = 4,
    DRV_OP_PRESENT = 5,
    DRV_OP_MAP_SHM = 6,
    /* Console PD (issue #51): raw status-page mapping + render mode. */
    DRV_OP_MAP_STATUS = 7,
    DRV_OP_CONSOLE_MODE = 8,
    /* Console PD scanout grant as a frame pin (RAM-backed scanouts —
     * SYS_MMIO_MAP refuses RAM, SYS_FRAME_MAP refuses kernel pages, so
     * the kind follows the backing; see Framebuffer::scanout_ram_backed).
     * Same payload shape as DRV_OP_BIND, chunk index in `flags`. */
    DRV_OP_BIND_FRAME = 9,
    DRV_OP_INJECT_FAULT = 240,
} drv_op_t;

struct DriverRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

static void drv_assert_shape(void) {
    char check[sizeof(struct DriverRpc) <= 48 ? 1 : -1];
    (void)check;
}

static inline int drv_send(unsigned long dest, unsigned long type,
                           const void *data, unsigned long size) {
    return ipc_send(dest, type, data, size, 0);
}

static inline int drv_recv(void *buf, unsigned long max_size,
                           unsigned long timeout_ticks) {
    return ipc_recv_timeout(buf, max_size, timeout_ticks);
}

/* Serial TX SHM ring (issue #51 step-c-prime, mirrors the kernel
 * SharedRingHeader layout byte-for-byte — see src/kernel/ipc/shm_ring.hpp;
 * keep in sync: magic/version/capacity/element_size + 64 B-aligned
 * head/tail + 64 B-aligned drain_hold (v2 quiesce hook). Element:
 * {uint8 len, char data[7]}. */
#define SERIAL_TX_RING_ESZ 8
#define SERIAL_TX_RING_MAGIC 0x4E455852494E47UL
#define SERIAL_TX_RING_VERSION 2
#define SERIAL_TX_NOTIFY 0x5458

/* Console PD bounded-render contract (issue #51). The text ring reuses the
 * SharedRingHeader wire (magic/version/capacity/element_size + 64 B-aligned
 * head/tail/drain_hold) with the same {uint8 len, char data[7]} element.
 * Render is bounded: at most CONSOLE_CELLS_PER_PASS glyph cells per drain
 * pass (a 80x4 band), so one pass can never monopolise the PD. The status
 * page is raw shared memory (no header) the kernel reads directly: the
 * exact bound proof is pass_max_cells <= CONSOLE_CELLS_PER_PASS. */
#define CONSOLE_RING_MAGIC SERIAL_TX_RING_MAGIC
#define CONSOLE_RING_VERSION SERIAL_TX_RING_VERSION
#define CONSOLE_RING_ESZ 8
#define CONSOLE_CELLS_PER_PASS 320
#define CONSOLE_MAX_COLS 160
#define CONSOLE_MAX_ROWS 60
/* Scanout chunking: the user map primitives hand out one fixed region
 * per cap (CONFIG_USER_MMIO_REGION_SIZE for MMIO, CONFIG_USER_SHM_REGION_SIZE
 * for frames), so a full scanout is granted as consecutive chunks. BIND /
 * BIND_FRAME carry the chunk index in `flags`. Keep in sync with the kernel
 * (static_assert in src/kernel/task/boot_sdf.hpp: kConsoleMmioChunk /
 * kConsoleFrameChunk). */
#define CONSOLE_MMIO_CHUNK 0x200000UL
#define CONSOLE_FRAME_CHUNK 0x200000UL
#define CONSOLE_MAX_SCANOUT_CHUNKS 4
#define CONSOLE_FG 0x00C8C8C8
#define CONSOLE_BG 0x00000000

struct console_status {
    volatile uint64_t rendered_cells; /* total glyph cells drawn */
    volatile uint64_t pass_max_cells; /* max cells drawn in one pass */
    volatile uint64_t passes;         /* drain passes that drew */
    volatile uint64_t rejected;       /* malformed RPC/element count */
    volatile uint64_t dropped_full;   /* ring-full drops (kernel-side) */
    volatile uint64_t mode_w;
    volatile uint64_t mode_h;
    volatile uint64_t pitch;
    volatile uint64_t bpp;
    volatile uint64_t cols;
    volatile uint64_t rows;
    volatile uint64_t cursor_row;
    volatile uint64_t scanout_mapped;
    volatile uint64_t status_mapped;
    volatile uint64_t ring_mapped;
};

#endif
