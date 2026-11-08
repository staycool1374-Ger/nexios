/* Driver PD: framebuffer / console (issue #48 device caps + issue #51
 * console emulation policy).
 *
 * Ownership split (docs/specs/driver-pds.md): #48 owns the device caps,
 * #51 owns the emulation policy. This PD therefore receives the scanout
 * as a memory-type MmioCap and renders text from a kernel-fed SPSC ring —
 * no ambient framebuffer authority, no kernel pixel loops.
 *
 * Wire (all bounded, no allocation, no locks across IPC):
 *   BIND            arg0 = scanout MmioCap handle, arg1 = phys,
 *                   arg2 = byte span; PD maps it once (SYS_MMIO_MAP).
 *   CONSOLE_MODE    arg0 = pitch, arg1 = width, arg2 = height,
 *                   flags = bpp; recomputes the clamped text grid.
 *   MAP_SHM         arg0 = text-ring FrameCap handle, arg1 = pages,
 *                   arg2 = element size; PD SYS_FRAME_MAPs it once.
 *   MAP_STATUS      arg0 = raw status FrameCap handle, arg1 = pages;
 *                   PD publishes render counters there for kernel tests.
 *   PRESENT         bounded ack (host flush is a kernel-side concern).
 * Replies go to req.owner_id (send_sync requester queue; #441/#51).
 *
 * Bounded render: at most CONSOLE_CELLS_PER_PASS glyph cells per drain
 * pass. The remainder stays in the ring (tail not advanced past the
 * budget), so a boot flood backpressures instead of monopolising the PD.
 * The exact bound is published as pass_max_cells for the kernel test.
 */
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include "driver_pd.h"
#include "font8x16.h"

static uint64_t g_my_id;

/* Scanout + text geometry. */
/* Scanout is granted as consecutive CONSOLE_MMIO_CHUNK-sized MmioCaps
 * (one user MMIO slot per cap: the map primitive hands out a fixed
 * CONFIG_USER_MMIO_REGION_SIZE region, and a full scanout is larger). */
static unsigned long g_fb_handle[CONSOLE_MAX_SCANOUT_CHUNKS];
static unsigned long g_fb_va[CONSOLE_MAX_SCANOUT_CHUNKS];
static unsigned long g_fb_chunks;
static int g_fb_mapped;
static uint32_t g_width;
static uint32_t g_height;
static uint32_t g_pitch;
static uint32_t g_bpp;
static uint32_t g_cols;
static uint32_t g_rows;
static uint32_t g_cursor_x;
static uint32_t g_cursor_row;
static uint32_t g_top_row;

/* Text ring (SharedRingHeader v2 wire; see src/kernel/ipc/shm_ring.hpp). */
struct console_ring_header {
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

static unsigned long g_ring_handle;
static unsigned long g_ring_va;
static int g_ring_mapped;

static unsigned long g_status_handle;
static unsigned long g_status_va;
static int g_status_mapped;

static struct console_ring_header *ring_hdr(void) {
    return (struct console_ring_header *)g_ring_va;
}

static struct console_status *status(void) {
    return (struct console_status *)g_status_va;
}

static int ring_valid(void) {
    struct console_ring_header *h = ring_hdr();
    uint64_t magic = __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE);
    uint64_t version = __atomic_load_n(&h->version, __ATOMIC_ACQUIRE);
    uint64_t cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&h->element_size, __ATOMIC_ACQUIRE);
    if (magic != CONSOLE_RING_MAGIC || version != CONSOLE_RING_VERSION)
        return 0;
    if (cap == 0 || (cap & (cap - 1)) != 0)
        return 0; /* power-of-two capacity (wrap mask) */
    if (esz != CONSOLE_RING_ESZ)
        return 0;
    return 1;
}

/* Pop one element into dst; returns the payload length (0..7) on a
 * popped element, -1 when the ring is empty/invalid. */
static long ring_pop(unsigned char *dst) {
    struct console_ring_header *h = ring_hdr();
    uint64_t cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&h->element_size, __ATOMIC_ACQUIRE);
    if (cap == 0 || esz == 0)
        return -1;
    uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return -1;
    unsigned char *base = (unsigned char *)g_ring_va + 4096;
    unsigned long off = (unsigned long)(((tail / esz) * esz) & (cap - 1));
    unsigned char *src = base + off;
    for (unsigned long i = 0; i < esz; i++)
        dst[i] = src[i];
    __atomic_store_n(&h->tail, tail + esz, __ATOMIC_RELEASE);
    unsigned long n = dst[0];
    if (n > esz - 1)
        n = esz - 1; /* hostile length: clamp, never OOB */
    return (long)n;
}

/* --- scanout pixel paths (format mirrors kernel Framebuffer::put_pixel) --- */

/* Byte store across chunked scanout regions (a pixel never straddles a
 * chunk boundary in practice, but per-byte resolution makes that a
 * non-issue for any pitch/format). Out-of-range writes are dropped. */
static void fb_put(unsigned long off, unsigned char v) {
    unsigned long chunk = off / CONSOLE_MMIO_CHUNK;
    if (chunk >= g_fb_chunks)
        return;
    ((volatile unsigned char *)g_fb_va[chunk])[off % CONSOLE_MMIO_CHUNK] = v;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t color) {
    if (!g_fb_mapped)
        return;
    uint32_t bpp_bytes = g_bpp / 8;
    if (bpp_bytes == 0)
        return;
    if (x >= g_width || y >= g_height)
        return;
    if (x + w > g_width)
        w = g_width - x;
    if (y + h > g_height)
        h = g_height - y;
    for (uint32_t row = 0; row < h; row++) {
        unsigned long base = (unsigned long)(y + row) * g_pitch +
                             (unsigned long)x * bpp_bytes;
        for (uint32_t col = 0; col < w; col++) {
            unsigned long off = base + (unsigned long)col * bpp_bytes;
            fb_put(off + 0, (unsigned char)(color & 0xFF));
            fb_put(off + 1, (unsigned char)((color >> 8) & 0xFF));
            fb_put(off + 2, (unsigned char)((color >> 16) & 0xFF));
            if (bpp_bytes > 3)
                fb_put(off + 3, 0xFF); /* alpha for >24bpp (LEARNINGS) */
        }
    }
}

static void draw_glyph(uint32_t cx, uint32_t cy, unsigned char ch) {
    if (ch < 0x20 || ch > 0x7E)
        ch = '?';
    const uint8_t *glyph = font8x16[ch - 0x20];
    uint32_t bpp_bytes = g_bpp / 8;
    if (bpp_bytes == 0)
        return;
    uint32_t px0 = cx * FONT_WIDTH;
    uint32_t py0 = cy * FONT_HEIGHT;
    if (px0 + FONT_WIDTH > g_width || py0 + FONT_HEIGHT > g_height)
        return;
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        unsigned long base = (unsigned long)(py0 + row) * g_pitch +
                             (unsigned long)px0 * bpp_bytes;
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (bits & (0x80u >> col)) ? CONSOLE_FG
                                                     : CONSOLE_BG;
            unsigned long off = base + (unsigned long)col * bpp_bytes;
            fb_put(off + 0, (unsigned char)(color & 0xFF));
            fb_put(off + 1, (unsigned char)((color >> 8) & 0xFF));
            fb_put(off + 2, (unsigned char)((color >> 16) & 0xFF));
            if (bpp_bytes > 3)
                fb_put(off + 3, 0xFF);
        }
    }
}

static uint32_t phys_row(uint32_t logical) {
    return (g_top_row + logical) % g_rows;
}

static void clear_row(uint32_t logical) {
    if (g_rows == 0)
        return;
    fill_rect(0, phys_row(logical) * FONT_HEIGHT, g_width, FONT_HEIGHT,
              CONSOLE_BG);
}

static void scroll_one(void) {
    g_top_row = (g_top_row + 1) % g_rows;
    g_cursor_row = g_rows - 1;
    /* Only the recycled band is repainted (circular text grid): bounded
     * by width * FONT_HEIGHT pixels, never a whole-screen copy. */
    clear_row(g_cursor_row);
}

static void newline(void) {
    g_cursor_x = 0;
    if (g_cursor_row + 1 >= g_rows) {
        scroll_one();
    } else {
        g_cursor_row++;
        clear_row(g_cursor_row);
    }
}

/* Render one character; returns 1 if a glyph cell was drawn. */
static int put_char(unsigned char ch) {
    if (ch == '\n') {
        newline();
        return 0;
    }
    if (ch == '\r') {
        g_cursor_x = 0;
        return 0;
    }
    if (ch == '\b') {
        if (g_cursor_x > 0)
            g_cursor_x--;
        draw_glyph(g_cursor_x, phys_row(g_cursor_row), ' ');
        return 1;
    }
    if (g_cols == 0) {
        return 0;
    }
    draw_glyph(g_cursor_x, phys_row(g_cursor_row), ch);
    g_cursor_x++;
    if (g_cursor_x >= g_cols)
        newline();
    return 1;
}

/* Peek the pending element length without consuming it (SPSC: the
 * producer only appends, so a peeked element stays valid for the pop). */
static long ring_peek_len(void) {
    struct console_ring_header *h = ring_hdr();
    uint64_t cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&h->element_size, __ATOMIC_ACQUIRE);
    if (cap == 0 || esz == 0)
        return -1;
    uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_RELAXED);
    if (head == tail)
        return -1;
    unsigned char *base = (unsigned char *)g_ring_va + 4096;
    unsigned long off = (unsigned long)(((tail / esz) * esz) & (cap - 1));
    unsigned long n = base[off];
    if (n > esz - 1)
        n = esz - 1; /* hostile length: clamp, never OOB */
    return (long)n;
}

/* Bounded drain: at most CONSOLE_CELLS_PER_PASS glyph cells per pass.
 * The pass budget is enforced by PEEKING: an element that would cross
 * the budget stays queued for the next pass. Popping first and breaking
 * mid-element (the naive shape) would silently drop the element's tail
 * bytes — producer accounting and the pixel oracle must agree exactly. */
static void console_drain(void) {
    unsigned char elem[CONSOLE_RING_ESZ];
    uint64_t cells = 0;
    uint64_t started = 0;
    if (!g_ring_mapped || g_rows == 0)
        return;
    if (__atomic_load_n(&ring_hdr()->drain_hold, __ATOMIC_ACQUIRE) != 0)
        return; /* quiesced: exact producer accounting tests */
    while (cells < CONSOLE_CELLS_PER_PASS) {
        long n = ring_peek_len();
        if (n < 0)
            break; /* empty */
        if ((uint64_t)n > CONSOLE_CELLS_PER_PASS - cells)
            break; /* next pass takes it whole */
        if (ring_pop(elem) < 0)
            break; /* producer raced the indices: retry next pass */
        {
            long m = elem[0] > CONSOLE_RING_ESZ - 1
                         ? CONSOLE_RING_ESZ - 1
                         : elem[0];
            if (m != n)
                break; /* torn element: tail moved under us — fail closed */
            started = 1;
            for (long i = 0; i < m; i++)
                cells += (uint64_t)put_char(elem[1 + i]);
        }
    }
    if (started && g_status_mapped) {
        struct console_status *st = status();
        __atomic_store_n(&st->rendered_cells,
                         st->rendered_cells + cells, __ATOMIC_RELEASE);
        if (cells > st->pass_max_cells)
            __atomic_store_n(&st->pass_max_cells, cells, __ATOMIC_RELEASE);
        __atomic_store_n(&st->passes, st->passes + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&st->cursor_row, g_cursor_row, __ATOMIC_RELEASE);
    }
}

static int fb_map_kind(unsigned long handle, unsigned long index,
                       unsigned long phys, unsigned long size, int is_frame) {
    unsigned long limit =
        is_frame ? CONSOLE_FRAME_CHUNK : CONSOLE_MMIO_CHUNK;
    if (index >= CONSOLE_MAX_SCANOUT_CHUNKS || size == 0 || size > limit)
        return -1;
    if (g_fb_handle[index] == handle && g_fb_va[index] != 0)
        return 0; /* idempotent re-BIND (epoch re-derive) */
    if (g_fb_va[index] != 0)
        return -1; /* changed handle: fail closed, never double-map */
    long va = is_frame ? __syscall5(SYS_FRAME_MAP, (long)handle, 0, 0, 0)
                       : __syscall5(SYS_MMIO_MAP, (long)handle, 0, 0, 0);
    if (va == 0 || va == -1)
        return -1;
    g_fb_va[index] = (unsigned long)va;
    g_fb_handle[index] = handle;
    (void)phys; /* chunk offsets are derived from the chunk index */
    if (index + 1 > g_fb_chunks)
        g_fb_chunks = index + 1;
    g_fb_mapped = 1;
    return 0;
}

/* MMIO-backed scanout chunk (true device memory, e.g. x86 VGA VRAM). */
static int fb_map(unsigned long handle, unsigned long index,
                  unsigned long phys, unsigned long size) {
    return fb_map_kind(handle, index, phys, size, 0);
}

/* Frame-backed scanout chunk (RAM-backed backends): same chunk array,
 * mapped with SYS_FRAME_MAP. A MAP of the wrong kind for an already
 * mapped chunk fails closed — the bind wire is kind-explicit. */
static int fb_map_frame(unsigned long handle, unsigned long index,
                        unsigned long phys, unsigned long size) {
    return fb_map_kind(handle, index, phys, size, 1);
}

/* Geometry publish lands in console_mode AND at MAP_STATUS (bind order
 * is BIND/MODE/SHM/STATUS, so MODE cannot publish itself). */
static void publish_geometry(void);
static void console_mode(uint32_t pitch, uint32_t width, uint32_t height,
                         uint32_t bpp) {
    if (pitch == 0 || width == 0 || height == 0 || bpp < 8)
        return;
    g_pitch = pitch;
    g_width = width;
    g_height = height;
    g_bpp = bpp;
    g_cols = width / FONT_WIDTH;
    g_rows = height / FONT_HEIGHT;
    if (g_cols > CONSOLE_MAX_COLS)
        g_cols = CONSOLE_MAX_COLS;
    if (g_rows > CONSOLE_MAX_ROWS)
        g_rows = CONSOLE_MAX_ROWS;
    g_cursor_x = 0;
    g_cursor_row = 0;
    g_top_row = 0;
    fill_rect(0, 0, width, height, CONSOLE_BG);
    publish_geometry(); /* no-op until MAP_STATUS (bind order is
                         * BIND/MODE/SHM/STATUS) */
}



static void publish_geometry(void) {
    if (!g_status_mapped || g_cols == 0)
        return;
    struct console_status *st = status();
    __atomic_store_n(&st->mode_w, (uint64_t)g_width, __ATOMIC_RELEASE);
    __atomic_store_n(&st->mode_h, (uint64_t)g_height, __ATOMIC_RELEASE);
    __atomic_store_n(&st->pitch, (uint64_t)g_pitch, __ATOMIC_RELEASE);
    __atomic_store_n(&st->bpp, (uint64_t)g_bpp, __ATOMIC_RELEASE);
    __atomic_store_n(&st->cols, (uint64_t)g_cols, __ATOMIC_RELEASE);
    __atomic_store_n(&st->rows, (uint64_t)g_rows, __ATOMIC_RELEASE);
}

static void publish_map_state(void) {
    if (!g_status_mapped)
        return;
    struct console_status *st = status();
    __atomic_store_n(&st->scanout_mapped, (uint64_t)g_fb_mapped,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&st->ring_mapped, (uint64_t)g_ring_mapped,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&st->status_mapped, 1, __ATOMIC_RELEASE);
}

int main(void) {
    drv_assert_shape();
    g_my_id = (uint64_t)getpid();
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        console_drain();
        /* 1-tick poll once the ring is live (bounded drain latency);
         * block forever before MAP (zero idle overhead). */
        int rc = drv_recv(rbuf, sizeof(rbuf), g_ring_mapped ? 1 : 0);
        if (rc < 0)
            continue; /* fail-closed: no reply on transport error */
        struct DriverRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; /* torn/stale payload: kernel type must match body op */

        struct DriverRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        unsigned long dest = req.owner_id != 0 ? req.owner_id : 1;

        switch (req.op) {
        case DRV_OP_BIND:
            if (fb_map((unsigned long)req.arg0, (unsigned long)req.flags,
                       (unsigned long)req.arg1, (unsigned long)req.arg2) !=
                0) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            publish_map_state();
            rep.arg0 = (uint64_t)1;
            break;
        case DRV_OP_BIND_FRAME:
            if (fb_map_frame((unsigned long)req.arg0,
                             (unsigned long)req.flags,
                             (unsigned long)req.arg1,
                             (unsigned long)req.arg2) != 0) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            publish_map_state();
            rep.arg0 = (uint64_t)1;
            break;
        case DRV_OP_CONSOLE_MODE:
            if (!g_fb_mapped) {
                rep.status = (uint16_t)(uint16_t)-38;
                break;
            }
            console_mode((uint32_t)req.arg0, (uint32_t)req.arg1,
                         (uint32_t)req.arg2, (uint32_t)req.flags);
            publish_map_state();
            rep.arg0 = (uint64_t)1;
            break;
        case DRV_OP_MAP_SHM: {
            unsigned long pages = (unsigned long)req.arg1;
            if ((unsigned long)req.arg2 != CONSOLE_RING_ESZ || pages < 2) {
                rep.status = (uint16_t)(uint16_t)-22;
                break;
            }
            if (g_ring_mapped && (unsigned long)req.arg0 == g_ring_handle) {
                rep.arg0 = (uint64_t)1; /* idempotent re-MAP */
                break;
            }
            if (g_ring_mapped) {
                __syscall5(SYS_FRAME_UNMAP, (long)g_ring_va, 0, 0, 0);
                g_ring_mapped = 0;
                g_ring_handle = 0;
                g_ring_va = 0;
            }
            long va = __syscall5(SYS_FRAME_MAP, (long)req.arg0, 0, 0, 0);
            if (va == 0 || va == -1) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            g_ring_va = (unsigned long)va;
            if (!ring_valid()) {
                __syscall5(SYS_FRAME_UNMAP, (long)g_ring_va, 0, 0, 0);
                g_ring_va = 0;
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            g_ring_handle = (unsigned long)req.arg0;
            g_ring_mapped = 1;
            publish_map_state();
            rep.arg0 = (uint64_t)1;
            break;
        }
        case DRV_OP_MAP_STATUS: {
            unsigned long pages = (unsigned long)req.arg1;
            if (pages < 1) {
                rep.status = (uint16_t)(uint16_t)-22;
                break;
            }
            if (g_status_mapped &&
                (unsigned long)req.arg0 == g_status_handle) {
                rep.arg0 = (uint64_t)1; /* idempotent re-MAP */
                break;
            }
            if (g_status_mapped) {
                __syscall5(SYS_FRAME_UNMAP, (long)g_status_va, 0, 0, 0);
                g_status_mapped = 0;
                g_status_handle = 0;
                g_status_va = 0;
            }
            long va = __syscall5(SYS_FRAME_MAP, (long)req.arg0, 0, 0, 0);
            if (va == 0 || va == -1) {
                rep.status = (uint16_t)(uint16_t)-5;
                break;
            }
            g_status_va = (unsigned long)va;
            /* Header-free page: zero it so the kernel never reads a
             * stale counter from a previous composition. */
            memset((void *)g_status_va, 0, pages * 4096);
            g_status_handle = (unsigned long)req.arg0;
            g_status_mapped = 1;
            publish_map_state();
            publish_geometry(); /* MODE arrives before STATUS */
            rep.arg0 = (uint64_t)1;
            break;
        }
        case DRV_OP_PRESENT:
            /* Bounded ack. Host flush for virtio-gpu is a kernel-side
             * concern (Framebuffer::present()); multiboot2 and ramfb
             * scanouts are directly visible guest memory. */
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
