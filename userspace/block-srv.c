/* Block-storage server PD (issue #47, strangler step-b2).
 *
 * Real block I/O behind CONFIG_BLOCK_SRV: ATA PIO on x86_64 (I/O-port
 * caps) and virtio-blk over MMIO v2 on aarch64/riscv64 (MMIO + shared
 * frame caps). Completion via PD-owned IRQs (WAIT_ACK delivery with
 * mask-on-delivery + bounded wait); the bounded device poll stays as
 * the fail-closed fallback (timeout, revoke, or INJECT_IRQ_SKIP).
 *
 * Wire: 48 B BlockRpc, mirrors kernel/driver/block_srv.hpp (op, flags,
 * rsvd, epoch, owner id+gen, lba, arg, handle, status_op, status, aux).
 * Order: CAP_GET rounds first (PD maps each cap, echoes), BIND last
 * (PD probes with every cap present, answers backend + geometry).
 * Shared 4-page window layout mirrors kBlock*Offset (data 0x1000,
 * header 0x1200, status 0x1300, IRQ stats 0x1400; page 0 is the virtio
 * queue area).
 * Replies go to req.owner_id (send_sync_bounded waits on the
 * requester's own queue; PID 1 is a dead letter).
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <ipc.h>
#include <cap/nexios_cap.h>

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    BLK_OP_BIND = 0,
    BLK_OP_GEOMETRY = 1,
    BLK_OP_READ_BLOCK = 2,
    BLK_OP_WRITE_BLOCK = 3,
    BLK_OP_FLUSH = 4,
    BLK_OP_DMA_MAP = 5,
    BLK_OP_DMA_UNMAP = 6,
    BLK_OP_LOOPBACK = 7,
    BLK_OP_INJECT_MID_TRANSFER = 8,
    BLK_OP_CAP_GET = 9,
    BLK_OP_INJECT_IRQ_SKIP = 10,
} block_srv_op_t;

/* Cap kinds (mirror kernel SDF CapKind). */
#define BLK_CAP_IRQ 0
#define BLK_CAP_MSIX 1
#define BLK_CAP_MMIO 2
#define BLK_CAP_DMA 3
#define BLK_CAP_FRAME 4
#define BLK_CAP_SHM 5
#define BLK_CAP_DMA_REGION 6

/* Backends (mirror kernel BlkBackend). */
#define BLK_BACKEND_NONE 0
#define BLK_BACKEND_ATA_PIO 1
#define BLK_BACKEND_VIRTIO_MMIO 2

struct BlockRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t lba;
    uint64_t arg;
    uint32_t handle;
    uint16_t status_op;
    uint16_t status;
    uint64_t aux;
};

static void blk_assert_shape(void) {
    char check[sizeof(struct BlockRpc) == 48 ? 1 : -1];
    (void)check;
}

/* Shared window layout (mirror kernel kBlock*Offset). */
#define BLK_REGION_PAGES 4
#define BLK_DATA_OFF 0x1000UL
#define BLK_HDR_OFF 0x1200UL
#define BLK_STATUS_OFF 0x1300UL
#define BLK_QDESC_OFF 0x0000UL
#define BLK_QAVAIL_OFF 0x0400UL
#define BLK_QUSED_OFF 0x0800UL
#define BLK_SECTOR 512UL
#define BLK_QSIZE 64

/* Linux errno shapes carried in status (negated). */
#define BLK_EIO 5
#define BLK_EINVAL 22
#define BLK_ENOSYS 38
#define BLK_EACCES 13

#define BLK_MAX_CAPS 8
#define BLK_IRQ_STATS_OFF 0x1400UL
#define BLK_IRQ_STATS_MAGIC 0x424C4B4952515354UL
#define BLK_IRQ_STATS_VERSION 1UL
/* Bounded IRQ wait (issue #47 step-b2): 1000 ticks (1 s at the 1000 Hz
 * CONFIG_TICK_HZ). Normal completions arrive in microseconds; the bound
 * only fires on genuine device silence or TCG/AIO first-touch latency
 * spikes (convicted: first transfer after (re)probe can exceed 100 ticks
 * under TCG while the IRQ still arrives late and is absorbed as a
 * stale pending by the next wait). The contract stays bounded and
 * fail-safe: expiry falls back to the bounded device poll. */
#define BLK_IRQ_WAIT_TICKS 1000UL
#define BLK_IRQ_MODE_WAIT_ACK 2UL

struct blk_irq_stats {
    unsigned long magic;
    unsigned long version;
    unsigned long irq_waits;
    unsigned long irq_completions;
    unsigned long irq_timeouts;
    unsigned long poll_fallbacks;
};

struct blk_cap {
    int mapped;
    uint8_t kind;
    unsigned long handle;
    unsigned long phys;
    unsigned long size;
    unsigned long va;
};

static struct blk_cap g_caps[BLK_MAX_CAPS];
static unsigned g_mapped_count = 0;
static uint32_t g_epoch = 0;
static uint8_t g_backend = BLK_BACKEND_NONE;
static uint64_t g_sectors = 0;
static int g_probed = 0;
static int g_inject_once = 0;
static int g_inject_irq_skip_once = 0;

/* PD-owned completion IRQ (issue #47 step-b2): handle + device phys the
 * cap was conveyed with. Zero handle = pure poll path. */
static unsigned long g_irq_handle = 0;
static unsigned long g_irq_phys = 0;

/* x86_64 ATA state (valid when g_backend == ATA_PIO). */
static unsigned g_ata_base = 0;
static unsigned char g_ata_drive = 0;
/* virtio state (valid when g_backend == VIRTIO_MMIO). */
static unsigned long g_mmio_va = 0;
static unsigned long g_frame_va = 0;
static unsigned long g_frame_phys = 0;
static int g_virtio_ready = 0;
static int g_virtio_legacy = 0;
static unsigned long g_q_desc_va = 0;
static unsigned long g_q_avail_va = 0;
static unsigned long g_q_used_va = 0;
static unsigned g_virtio_qsize = 0;
static unsigned g_virtio_avail_idx = 0;
static unsigned g_virtio_last_used = 0;

#if defined(__x86_64__)
static inline void blk_outb(unsigned short port, unsigned char v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(port));
}

static inline unsigned char blk_inb(unsigned short port) {
    unsigned char r;
    __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline unsigned short blk_inw(unsigned short port) {
    unsigned short r;
    __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void blk_outw(unsigned short port, unsigned short v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port));
}
#endif

static void blk_unmap_all(void) {
    for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
        if (!g_caps[i].mapped)
            continue;
        if (g_caps[i].kind == BLK_CAP_MMIO ||
            g_caps[i].kind == BLK_CAP_DMA_REGION) {
            if (g_caps[i].va != 0) {
                unsigned long unmap = (g_caps[i].kind == BLK_CAP_MMIO)
                                          ? SYS_MMIO_UNMAP
                                          : SYS_FRAME_UNMAP;
                __syscall5(unmap, (long)g_caps[i].va, 0, 0, 0);
                g_caps[i].va = 0;
            }
        }
        /* I/O-port grants persist per task (ledger); re-grant is
         * skipped by the granted-port record, so nothing to undo. */
        g_caps[i].mapped = 0;
    }
    g_mapped_count = 0;
    g_probed = 0;
    g_backend = BLK_BACKEND_NONE;
    g_sectors = 0;
    g_mmio_va = 0;
    g_frame_va = 0;
    g_frame_phys = 0;
    g_virtio_ready = 0;
    g_inject_once = 0;
    g_inject_irq_skip_once = 0;
    g_irq_handle = 0;
    g_irq_phys = 0;
}

static void blk_purge_on_epoch(uint32_t epoch) {
    if (g_epoch != epoch) {
        blk_unmap_all();
        g_epoch = epoch;
    }
}

static uint16_t blk_neg(uint16_t code) {
    return (uint16_t)(uint16_t)(0 - code);
}

/* IRQ completion statistics (mirror kernel BlockIrqStats; issue #47
 * step-b2). Published in the shared window at BLK_IRQ_STATS_OFF with
 * release-ordered stores; the kernel test reads them through its HHDM
 * alias. No-ops until the DMA window is mapped. */
static struct blk_irq_stats *blk_stats_ptr(void) {
    if (g_frame_va == 0)
        return 0;
    return (struct blk_irq_stats *)(g_frame_va + BLK_IRQ_STATS_OFF);
}

static void blk_stats_publish(void) {
    struct blk_irq_stats *s = blk_stats_ptr();
    if (!s)
        return;
    s->irq_waits = 0;
    s->irq_completions = 0;
    s->irq_timeouts = 0;
    s->poll_fallbacks = 0;
    __asm__ volatile("" ::: "memory");
    s->version = BLK_IRQ_STATS_VERSION;
    __asm__ volatile("" ::: "memory");
    s->magic = BLK_IRQ_STATS_MAGIC;
}

static void blk_stats_bump(int which) {
    struct blk_irq_stats *s = blk_stats_ptr();
    unsigned long *c;
    if (!s || s->magic != BLK_IRQ_STATS_MAGIC)
        return;
    switch (which) {
    case 0:
        c = &s->irq_waits;
        break;
    case 1:
        c = &s->irq_completions;
        break;
    case 2:
        c = &s->irq_timeouts;
        break;
    default:
        c = &s->poll_fallbacks;
        break;
    }
    __asm__ volatile("" ::: "memory");
    *c = *c + 1;
    __asm__ volatile("" ::: "memory");
}

#define BLK_STAT_WAITS 0
#define BLK_STAT_COMPLETIONS 1
#define BLK_STAT_TIMEOUTS 2
#define BLK_STAT_FALLBACKS 3

/* Bounded completion wait (issue #47 step-b2): when a PD-owned IRQ is
 * armed, wait up to BLK_IRQ_WAIT_TICKS for the completion IRQ, then
 * validate it through is_ok (device-state confirmation — never infer
 * success from the IRQ alone). A consumed-but-unconfirmed IRQ drained
 * a stale pending from an earlier phase (e.g. a WRITE's data-ready
 * IRQ consumed by the completion wait): wait once more for the real
 * completion. A first-wait timeout means silence (the device never
 * fired): poll immediately, no retry. Returns 0 when is_ok passed
 * (a completion is recorded), 1 when the caller must take the bounded
 * poll (a fallback is recorded here). One WAITS bump per transfer so
 * waits line up with completions in the stats window. */
static void blk_completion_ok(void);
static void blk_completion_poll_fallback(void);

static int blk_wait_once(int *consumed) {
    long r = nexios_cap_irq_wait_timeout(g_irq_handle, BLK_IRQ_WAIT_TICKS);
    if (r < 0) {
        blk_stats_bump(BLK_STAT_TIMEOUTS);
        *consumed = 0;
        return -1;
    }
    *consumed = 1;
    return 0;
}

static int blk_wait_validated(int (*is_ok)(void)) {
    if (g_irq_handle == 0 || g_inject_irq_skip_once) {
        if (g_inject_irq_skip_once)
            g_inject_irq_skip_once = 0;
        blk_stats_bump(BLK_STAT_FALLBACKS);
        return 1;
    }
    blk_stats_bump(BLK_STAT_WAITS);
    int consumed = 0;
    if (blk_wait_once(&consumed) == 0 && is_ok() == 0) {
        blk_completion_ok();
        return 0;
    }
    if (consumed) {
        /* Stale pending drained but device not done — the real
         * completion IRQ is in flight; wait for it once. */
        if (blk_wait_once(&consumed) == 0 && is_ok() == 0) {
            blk_completion_ok();
            return 0;
        }
    }
    blk_stats_bump(BLK_STAT_FALLBACKS);
    return 1;
}

static void blk_completion_ok(void) {
    blk_stats_bump(BLK_STAT_COMPLETIONS);
}

static void blk_completion_poll_fallback(void) {
    blk_stats_bump(BLK_STAT_FALLBACKS);
}

/* Map one conveyed cap. Returns 0 on success, negated errno else.
 * Same-handle re-conveyance is idempotent (a retried bind after a
 * mid-bind transport failure must not wedge the PD); a changed
 * handle unmaps the stale slot first so no VA mapping leaks. */
static int blk_map_cap(unsigned idx, uint8_t kind, unsigned long handle,
                       unsigned long phys, unsigned long size,
                       unsigned io_port) {
    if (idx >= BLK_MAX_CAPS || handle == 0 || handle == (unsigned long)-1)
        return -BLK_EINVAL;
    if (g_caps[idx].mapped) {
        if (g_caps[idx].handle == handle && g_caps[idx].kind == kind &&
            g_caps[idx].phys == phys && g_caps[idx].size == size)
            return 0;
        if (g_caps[idx].kind == BLK_CAP_MMIO ||
            g_caps[idx].kind == BLK_CAP_DMA_REGION) {
            if (g_caps[idx].va != 0) {
                unsigned long unmap = (g_caps[idx].kind == BLK_CAP_MMIO)
                                          ? SYS_MMIO_UNMAP
                                          : SYS_FRAME_UNMAP;
                __syscall5(unmap, (long)g_caps[idx].va, 0, 0, 0);
                g_caps[idx].va = 0;
            }
        }
        /* I/O-port grants persist per task ledger; the grant-once
         * record below skips re-granting. */
        g_caps[idx].mapped = 0;
        g_mapped_count--;
    }
    if (kind == BLK_CAP_MMIO && io_port) {
#if defined(__x86_64__)
        /* I/O-port delegation (TSS bitmap grant, once per port base). */
        static unsigned long g_granted[BLK_MAX_CAPS];
        for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
            if (g_granted[i] == phys)
                goto granted;
        }
        if (__syscall5(SYS_IOPORT_GRANT, (long)handle, (long)phys,
                       (long)size, 0) != 0)
            return -BLK_EIO;
        for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
            if (g_granted[i] == 0) {
                g_granted[i] = phys;
                break;
            }
        }
    granted:
        g_caps[idx].mapped = 1;
        g_caps[idx].kind = kind;
        g_caps[idx].handle = handle;
        g_caps[idx].phys = phys;
        g_caps[idx].size = size;
        g_caps[idx].va = 0;
        g_mapped_count++;
        return 0;
#else
        (void)handle;
        (void)phys;
        (void)size;
        return -BLK_ENOSYS;
#endif
    }
    if (kind == BLK_CAP_MMIO) {
        long va = __syscall5(SYS_MMIO_MAP, (long)handle, 0, 0, 0);
        if (va <= 0)
            return -BLK_EIO;
        g_caps[idx].mapped = 1;
        g_caps[idx].kind = kind;
        g_caps[idx].handle = handle;
        g_caps[idx].phys = phys;
        g_caps[idx].size = size;
        g_caps[idx].va = (unsigned long)va;
        g_mapped_count++;
        return 0;
    }
    if (kind == BLK_CAP_DMA_REGION) {
        long va = __syscall5(SYS_FRAME_MAP, (long)handle, 0, 0, 0);
        if (va <= 0)
            return -BLK_EIO;
        g_caps[idx].mapped = 1;
        g_caps[idx].kind = kind;
        g_caps[idx].handle = handle;
        g_caps[idx].phys = phys;
        g_caps[idx].size = size;
        g_caps[idx].va = (unsigned long)va;
        g_mapped_count++;
        return 0;
    }
    if (kind == BLK_CAP_IRQ || kind == BLK_CAP_MSIX) {
        /* IRQ/MSIX caps convey the delivery vector (issue #47 step-b2):
         * no mapping — the handle is armed via irq_register and waited
         * on via irq_wait. Same-handle re-map is a no-op (idempotent
         * kernel arm). MSIX is conveyance-only in b2 (no AHCI backend
         * yet); the handle is stored so a later backend can arm it. */
        g_caps[idx].mapped = 1;
        g_caps[idx].kind = kind;
        g_caps[idx].handle = handle;
        g_caps[idx].phys = phys;
        g_caps[idx].size = size;
        g_caps[idx].va = 0;
        g_mapped_count++;
        return 0;
    }
    return -BLK_ENOSYS;
}

#if defined(__x86_64__)
#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ 0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7
#define ATA_POLL_N 100000
#define ATA_DEVCTL_OFF 0x206
#define ATA_DEVCTL_NIEN 0x02

static int ata_wait_bsy(unsigned base) {
    for (unsigned i = 0; i < ATA_POLL_N; ++i) {
        if (!(blk_inb((unsigned short)(base + 7)) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

static int ata_wait_drq(unsigned base) {
    for (unsigned i = 0; i < ATA_POLL_N; ++i) {
        unsigned char s = blk_inb((unsigned short)(base + 7));
        if (s & ATA_SR_BSY)
            continue;
        if (s & ATA_SR_ERR)
            return -1;
        if (s & ATA_SR_DRQ)
            return 0;
    }
    return -1;
}

/* Device-control (nIEN) access (issue #47 step-b2): the control block
 * rides its own IO-port cap (primary 0x3F6, secondary 0x376 =
 * command base + 0x206). Touch it only when the grant is present —
 * an ungranted outb faults the PD. */
static int ata_ctrl_granted(unsigned base) {
    unsigned long ctrl = (unsigned long)base + ATA_DEVCTL_OFF;
    for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
        if (g_caps[i].mapped && g_caps[i].kind == BLK_CAP_MMIO &&
            g_caps[i].phys == ctrl)
            return 1;
    }
    return 0;
}

static void ata_set_nien(unsigned base, int nien) {
    if (!ata_ctrl_granted(base))
        return;
    blk_outb((unsigned short)(base + ATA_DEVCTL_OFF),
             (unsigned char)(nien ? ATA_DEVCTL_NIEN : 0x00));
}

/* Device-state confirmation for an IRQ wake (issue #47 step-b2):
 * the transfer is complete when the device is out of BSY with no
 * error latched. Reads the selected channel's status (g_ata_base). */
static int ata_completion_ok(void) {
    unsigned char s = blk_inb((unsigned short)(g_ata_base + 7));
    return ((s & (ATA_SR_BSY | ATA_SR_ERR)) == 0) ? 0 : -1;
}

static int ata_identify(unsigned base, unsigned char drive,
                        uint64_t *sectors_out) {
    blk_outb((unsigned short)(base + 6), (unsigned char)drive);
    blk_outb((unsigned short)(base + 2), 0);
    blk_outb((unsigned short)(base + 3), 0);
    blk_outb((unsigned short)(base + 4), 0);
    blk_outb((unsigned short)(base + 5), 0);
    blk_outb((unsigned short)(base + 7), ATA_CMD_IDENTIFY);
    unsigned char status = blk_inb((unsigned short)(base + 7));
    if (status == 0 || status == 0xFF)
        return -1;
    if (ata_wait_bsy(base))
        return -1;
    status = blk_inb((unsigned short)(base + 7));
    if (status & ATA_SR_ERR)
        return -1;
    if (ata_wait_drq(base))
        return -1;
    uint16_t buf[256];
    for (int i = 0; i < 256; ++i)
        buf[i] = blk_inw((unsigned short)base);
    if (buf[0] & (1u << 15))
        return -1;
    uint64_t sectors =
        ((uint64_t)buf[61] << 16) | (uint64_t)buf[60];
    if (sectors == 0)
        return -1;
    *sectors_out = sectors;
    return 0;
}

static int ata_read_sector(unsigned base, unsigned char drive, uint64_t lba,
                           unsigned char *dst) {
    if (ata_wait_bsy(base))
        return -1;
    blk_outb((unsigned short)(base + 6),
             (unsigned char)(drive | ((lba >> 24) & 0x0F)));
    blk_outb((unsigned short)(base + 2), 1);
    blk_outb((unsigned short)(base + 3), (unsigned char)(lba & 0xFF));
    blk_outb((unsigned short)(base + 4),
             (unsigned char)((lba >> 8) & 0xFF));
    blk_outb((unsigned short)(base + 5),
             (unsigned char)((lba >> 16) & 0xFF));
    blk_outb((unsigned short)(base + 7), ATA_CMD_READ);
    /* Completion (issue #47 step-b2): the READ IRQ fires with the data
     * block (DRQ). blk_wait_validated confirms the device is out of
     * BSY/ERR (stale-pending retry inside); the data phase below stays
     * a bounded DRQ poll either way. Never report success from the IRQ
     * alone — status + data decide. */
    blk_wait_validated(ata_completion_ok);
    if (ata_wait_drq(base))
        return -1;
    for (int i = 0; i < 256; ++i) {
        unsigned short w = blk_inw((unsigned short)base);
        dst[i * 2] = (unsigned char)(w & 0xFF);
        dst[i * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
    }
    return 0;
}

static int ata_write_sector(unsigned base, unsigned char drive, uint64_t lba,
                            const unsigned char *src) {
    if (ata_wait_bsy(base))
        return -1;
    blk_outb((unsigned short)(base + 6),
             (unsigned char)(drive | ((lba >> 24) & 0x0F)));
    blk_outb((unsigned short)(base + 2), 1);
    blk_outb((unsigned short)(base + 3), (unsigned char)(lba & 0xFF));
    blk_outb((unsigned short)(base + 4),
             (unsigned char)((lba >> 8) & 0xFF));
    blk_outb((unsigned short)(base + 5),
             (unsigned char)((lba >> 16) & 0xFF));
    blk_outb((unsigned short)(base + 7), ATA_CMD_WRITE);
    if (ata_wait_drq(base))
        return -1;
    for (int i = 0; i < 256; ++i) {
        unsigned short w =
            (unsigned short)src[i * 2] |
            (unsigned short)((unsigned short)src[i * 2 + 1] << 8);
        blk_outw((unsigned short)base, w);
    }
    blk_outb((unsigned short)(base + 7), ATA_CMD_FLUSH);
    /* Completion (issue #47 step-b2): the WRITE IRQ fires at command
     * completion (BSY clear). blk_wait_validated confirms BSY clear
     * and no ERR (stale-pending retry inside); otherwise the bounded
     * BSY poll below decides. Never report success from the IRQ alone. */
    if (blk_wait_validated(ata_completion_ok) == 0)
        return 0;
    if (ata_wait_bsy(base))
        return -1;
    return 0;
}
#endif

/* ---- virtio-blk over MMIO v2 (aarch64/riscv64) ---- */
#define VIRTIO_MAGIC_OFF 0x000
#define VIRTIO_VER_OFF 0x004
#define VIRTIO_DEVID_OFF 0x008
#define VIRTIO_FEAT_OFF 0x010
#define VIRTIO_FEATSEL_OFF 0x014
#define VIRTIO_DRVFEAT_OFF 0x020
#define VIRTIO_DRVFEATSEL_OFF 0x024
#define VIRTIO_QSEL_OFF 0x030
#define VIRTIO_QNUMMAX_OFF 0x034
#define VIRTIO_QNUM_OFF 0x038
#define VIRTIO_GUEST_PAGE_SIZE_OFF 0x028
#define VIRTIO_QALIGN_OFF 0x03C
#define VIRTIO_QPFN_OFF 0x040
#define VIRTIO_QREADY_OFF 0x044
#define VIRTIO_QNOTIFY_OFF 0x050
#define VIRTIO_ISR_OFF 0x060
#define VIRTIO_ISR_ACK_OFF 0x064
#define VIRTIO_ISR_USED_BIT 0x01
#define VIRTIO_ISR_CONFIG_BIT 0x02
#define VIRTIO_STATUS_OFF 0x070
#define VIRTIO_QDESCLO_OFF 0x080
#define VIRTIO_QDESCHI_OFF 0x084
#define VIRTIO_QDRVLO_OFF 0x090
#define VIRTIO_QDRVHI_OFF 0x094
#define VIRTIO_QDEVLO_OFF 0x0A0
#define VIRTIO_QDEVHI_OFF 0x0A4
#define VIRTIO_CONFIG_OFF 0x100
#define VIRTIO_MAGIC_VAL 0x74726976
#define VIRTIO_BLK_ID 2
#define VIRTIO_ST_ACK 1
#define VIRTIO_ST_DRIVER 2
#define VIRTIO_ST_FEAT_OK 8
#define VIRTIO_ST_DRV_OK 4
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_DESC_F_NEXT 1
#define VIRTIO_DESC_F_WRITE 2
#define VIRTIO_POLL_N 1000000

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static uint32_t vmmio_read32(unsigned long base, unsigned long off) {
    volatile uint32_t *p =
        (volatile uint32_t *)(base + off);
    return *p;
}

static void vmmio_write32(unsigned long base, unsigned long off,
                          uint32_t v) {
    volatile uint32_t *p =
        (volatile uint32_t *)(base + off);
    *p = v;
}

/* Returns 0 on success with *sectors_out and g_virtio_legacy set
 * (0 = modern v2, 1 = legacy v1, like the kernel finder). */
static int virtio_probe(unsigned long mmio_va, uint64_t *sectors_out) {
    if (vmmio_read32(mmio_va, VIRTIO_MAGIC_OFF) != VIRTIO_MAGIC_VAL)
        return 2;
    uint32_t ver = vmmio_read32(mmio_va, VIRTIO_VER_OFF);
    if (ver != 2 && ver != 1)
        return 3;
    uint32_t devid = vmmio_read32(mmio_va, VIRTIO_DEVID_OFF);
    if (devid != VIRTIO_BLK_ID)
        return 0x100 + (int)(devid & 0xFF); /* diag: actual id */
    g_virtio_legacy = (ver == 1) ? 1 : 0;
    vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, 0);
    for (unsigned i = 0; i < 100; ++i) {
        if (vmmio_read32(mmio_va, VIRTIO_STATUS_OFF) == 0)
            break;
    }
    uint32_t s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
    vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, s | VIRTIO_ST_ACK);
    s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
    vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, s | VIRTIO_ST_DRIVER);
    if (!g_virtio_legacy) {
        /* Modern: negotiate VERSION_1 (bit 32, high word). */
        vmmio_write32(mmio_va, VIRTIO_FEATSEL_OFF, 0);
        (void)vmmio_read32(mmio_va, VIRTIO_FEAT_OFF);
        vmmio_write32(mmio_va, VIRTIO_DRVFEATSEL_OFF, 0);
        vmmio_write32(mmio_va, VIRTIO_DRVFEAT_OFF, 0);
        vmmio_write32(mmio_va, VIRTIO_FEATSEL_OFF, 1);
        uint32_t dev_hi = vmmio_read32(mmio_va, VIRTIO_FEAT_OFF);
        vmmio_write32(mmio_va, VIRTIO_DRVFEATSEL_OFF, 1);
        vmmio_write32(mmio_va, VIRTIO_DRVFEAT_OFF,
                      (dev_hi & 1u) ? 1u : 0u);
        if (!(dev_hi & 1u))
            return -1;
    } else {
        /* Legacy v1: no VERSION_1 bit; accept the device as-is. */
        vmmio_write32(mmio_va, VIRTIO_FEATSEL_OFF, 0);
        (void)vmmio_read32(mmio_va, VIRTIO_FEAT_OFF);
        vmmio_write32(mmio_va, VIRTIO_DRVFEATSEL_OFF, 0);
        vmmio_write32(mmio_va, VIRTIO_DRVFEAT_OFF, 0);
        vmmio_write32(mmio_va, VIRTIO_FEATSEL_OFF, 1);
        (void)vmmio_read32(mmio_va, VIRTIO_FEAT_OFF);
        vmmio_write32(mmio_va, VIRTIO_DRVFEATSEL_OFF, 1);
        vmmio_write32(mmio_va, VIRTIO_DRVFEAT_OFF, 0);
    }
    s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
    vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, s | VIRTIO_ST_FEAT_OK);
    s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
    if (!(s & VIRTIO_ST_FEAT_OK))
        return 5;
    uint32_t lo = vmmio_read32(mmio_va, VIRTIO_CONFIG_OFF);
    uint32_t hi = vmmio_read32(mmio_va, VIRTIO_CONFIG_OFF + 4);
    uint64_t sectors = ((uint64_t)hi << 32) | lo;
    if (sectors == 0)
        return 6;
    *sectors_out = sectors;
    return 0;
}

/* Legacy v1 chunk geometry (mirrors the kernel
 * virtio_legacy_used_off): desc at chunk+0, avail right after,
 * used at the next 4 KiB boundary. */
#define BLK_LEGACY_CHUNK_OFF 0x2000UL
#define BLK_LEGACY_ALIGN 0x1000UL

static unsigned long blk_legacy_used_off(unsigned qsize) {
    unsigned long end =
        (unsigned long)qsize * 16 + 6 + (unsigned long)qsize * 2;
    return (end + BLK_LEGACY_ALIGN - 1) & ~(BLK_LEGACY_ALIGN - 1);
}

/* Clear any latched virtio ISR bits before the first IRQ wait
 * (issue #47 step-b2): a stale used/config bit would deliver an
 * immediate (spurious) IRQ on the first unmask. Same registers on
 * legacy v1 and modern v2. */
static void virtio_ack_stale_isr(unsigned long mmio_va) {
    (void)vmmio_read32(mmio_va, VIRTIO_ISR_OFF);
    vmmio_write32(mmio_va, VIRTIO_ISR_ACK_OFF,
                  VIRTIO_ISR_USED_BIT | VIRTIO_ISR_CONFIG_BIT);
}

static int virtio_setup_queue(unsigned long mmio_va, unsigned long phys,
                              unsigned qsize) {
    vmmio_write32(mmio_va, VIRTIO_QSEL_OFF, 0);
    if (!g_virtio_legacy) {
        uint32_t max = vmmio_read32(mmio_va, VIRTIO_QNUMMAX_OFF);
        if (max == 0)
            return -1;
        if (qsize > max)
            qsize = max;
        if (qsize > BLK_QSIZE)
            qsize = BLK_QSIZE;
        /* Fresh rings: PMM frames are not zeroed; the device reads
         * desc/avail/used unsynchronized, so publish clean state. */
        for (unsigned long i = 0; i < 0x1000UL; ++i)
            ((unsigned char *)g_frame_va)[i] = 0;
        g_virtio_last_used = 0;
        g_virtio_avail_idx = 0;
        g_virtio_qsize = qsize;
        g_q_desc_va = g_frame_va + BLK_QDESC_OFF;
        g_q_avail_va = g_frame_va + BLK_QAVAIL_OFF;
        g_q_used_va = g_frame_va + BLK_QUSED_OFF;
        vmmio_write32(mmio_va, VIRTIO_QNUM_OFF, qsize);
        vmmio_write32(mmio_va, VIRTIO_QDESCLO_OFF,
                      (uint32_t)(phys + BLK_QDESC_OFF));
        vmmio_write32(mmio_va, VIRTIO_QDESCHI_OFF,
                      (uint32_t)((phys + BLK_QDESC_OFF) >> 32));
        vmmio_write32(mmio_va, VIRTIO_QDRVLO_OFF,
                      (uint32_t)(phys + BLK_QAVAIL_OFF));
        vmmio_write32(mmio_va, VIRTIO_QDRVHI_OFF,
                      (uint32_t)((phys + BLK_QAVAIL_OFF) >> 32));
        vmmio_write32(mmio_va, VIRTIO_QDEVLO_OFF,
                      (uint32_t)(phys + BLK_QUSED_OFF));
        vmmio_write32(mmio_va, VIRTIO_QDEVHI_OFF,
                      (uint32_t)((phys + BLK_QUSED_OFF) >> 32));
        vmmio_write32(mmio_va, VIRTIO_QREADY_OFF, 1);
        uint32_t s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
        vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, s | VIRTIO_ST_DRV_OK);
        virtio_ack_stale_isr(mmio_va);
        return 0;
    }
    /* Legacy v1: one contiguous chunk in pages 2-3 (pages 0-1 stay
     * the data/header/status window). PFN path, no Ready bit. */
    vmmio_write32(mmio_va, VIRTIO_GUEST_PAGE_SIZE_OFF, 4096);
    uint32_t max = vmmio_read32(mmio_va, VIRTIO_QNUMMAX_OFF);
    if (max == 0)
        return -1;
    if (qsize > max)
        qsize = max;
    if (qsize > BLK_QSIZE)
        qsize = BLK_QSIZE;
    unsigned long used_off = blk_legacy_used_off(qsize);
    unsigned long used_size = 8 + (unsigned long)qsize * 8;
    if (used_off + used_size > 2 * BLK_LEGACY_ALIGN)
        return -1;
    unsigned long chunk_va = g_frame_va + BLK_LEGACY_CHUNK_OFF;
    unsigned long chunk_phys = phys + BLK_LEGACY_CHUNK_OFF;
    if ((chunk_phys % BLK_LEGACY_ALIGN) != 0)
        return -1;
    /* Fresh rings (see modern path): publish clean state. */
    for (unsigned long i = 0; i < 2 * BLK_LEGACY_ALIGN; ++i)
        ((unsigned char *)chunk_va)[i] = 0;
    g_virtio_last_used = 0;
    g_virtio_avail_idx = 0;
    g_virtio_qsize = qsize;
    g_q_desc_va = chunk_va;
    g_q_avail_va = chunk_va + (unsigned long)qsize * 16;
    g_q_used_va = chunk_va + used_off;
    vmmio_write32(mmio_va, VIRTIO_QNUM_OFF, qsize);
    vmmio_write32(mmio_va, VIRTIO_QALIGN_OFF,
                  (uint32_t)BLK_LEGACY_ALIGN);
        vmmio_write32(mmio_va, VIRTIO_QPFN_OFF,
                      (uint32_t)(chunk_phys / BLK_LEGACY_ALIGN));
        uint32_t s = vmmio_read32(mmio_va, VIRTIO_STATUS_OFF);
        vmmio_write32(mmio_va, VIRTIO_STATUS_OFF, s | VIRTIO_ST_DRV_OK);
        virtio_ack_stale_isr(mmio_va);
        return 0;
    }

/* Device-state confirmation for a virtio IRQ wake (issue #47 step-b2):
 * the used ring must have advanced past the submitted descriptor with
 * our id and a zero status byte. g_virtio_wait_last latches the
 * pre-submit used index (set before QNOTIFY). */
static unsigned g_virtio_wait_last = 0;

static int virtio_completion_ok(void) {
    struct virtq_used *used = (struct virtq_used *)g_q_used_va;
    volatile unsigned char *status =
        (volatile unsigned char *)(g_frame_va + BLK_STATUS_OFF);
    if (g_q_used_va == 0 || g_frame_va == 0 || g_virtio_qsize == 0)
        return -1;
    if (used->idx == (uint16_t)g_virtio_wait_last)
        return -1;
    if (used->ring[g_virtio_wait_last % g_virtio_qsize].id != 0)
        return -1;
    if (*status != 0)
        return -1;
    return 0;
}

static int virtio_transfer(int is_write, uint64_t lba) {
    unsigned long mmio = g_mmio_va;
    unsigned long fva = g_frame_va;
    unsigned long phys = g_frame_phys;
    unsigned qsize = g_virtio_qsize;
    if (mmio == 0 || fva == 0 || qsize == 0)
        return -1;
    if (!g_virtio_ready) {
        if (virtio_setup_queue(mmio, phys,
                               qsize > BLK_QSIZE ? BLK_QSIZE : qsize))
            return -1;
        g_virtio_ready = 1;
    }
    if (g_q_desc_va == 0 || g_q_avail_va == 0 || g_q_used_va == 0)
        return -1;
    /* Stale-latch drain (issue #47 step-b2): a held ISR bit from an
     * earlier transfer (e.g. one that completed via the poll fallback
     * without ACKing) would be consumed as a stale IRQ by the wait
     * below and fail validation, stranding the real completion with
     * no fresh edge. ACK before submitting so every IRQ observed past
     * this point belongs to this transfer. Mirrors the setup-time ACK;
     * single-threaded PD, no concurrent completer. */
    vmmio_write32(mmio, VIRTIO_ISR_ACK_OFF,
                  VIRTIO_ISR_USED_BIT | VIRTIO_ISR_CONFIG_BIT);
    struct virtq_desc *desc = (struct virtq_desc *)g_q_desc_va;
    struct virtq_avail *avail = (struct virtq_avail *)g_q_avail_va;
    struct virtq_used *used = (struct virtq_used *)g_q_used_va;
    struct virtio_blk_req *hdr =
        (struct virtio_blk_req *)(fva + BLK_HDR_OFF);
    unsigned char *data = (unsigned char *)(fva + BLK_DATA_OFF);
    volatile unsigned char *status =
        (volatile unsigned char *)(fva + BLK_STATUS_OFF);
    (void)data;
    unsigned long hdr_phys = phys + BLK_HDR_OFF;
    unsigned long data_phys = phys + BLK_DATA_OFF;
    unsigned long status_phys = phys + BLK_STATUS_OFF;
    hdr->type = is_write ? (uint32_t)VIRTIO_BLK_T_OUT
                         : (uint32_t)VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = lba;
    *status = 0xFF;
    desc[0].addr = hdr_phys;
    desc[0].len = 16;
    desc[0].flags = VIRTIO_DESC_F_NEXT;
    desc[0].next = 1;
    desc[1].addr = data_phys;
    desc[1].len = BLK_SECTOR;
    desc[1].flags = is_write ? VIRTIO_DESC_F_NEXT
                             : (VIRTIO_DESC_F_NEXT | VIRTIO_DESC_F_WRITE);
    desc[1].next = 2;
    desc[2].addr = status_phys;
    desc[2].len = 1;
    desc[2].flags = VIRTIO_DESC_F_WRITE;
    desc[2].next = 0;
    __asm__ volatile("" ::: "memory");
    unsigned last = g_virtio_last_used;
    avail->ring[avail->idx % qsize] = 0;
    __asm__ volatile("" ::: "memory");
    avail->idx = (uint16_t)(avail->idx + 1);
    g_virtio_avail_idx = avail->idx;
    __asm__ volatile("" ::: "memory");
    g_virtio_wait_last = last;
    vmmio_write32(mmio, VIRTIO_QNOTIFY_OFF, 0);
    /* Fast path (issue #47 step-b2): the ISR latch may already show a
     * posted completion — the device can finish during the notify
     * itself, while the GIC/PLIC entry is still masked from the
     * previous delivery (a masked-window edge is never re-signalled).
     * The used-ring advance disambiguates a fresh completion from a
     * stale latch (last_used tracks every observed completion, IRQ
     * or polled path alike): only a real advance counts, otherwise
     * fall through to the bounded wait. Bumping WAITS keeps the
     * waits/completions stats aligned (a zero-block wait). */
    if ((vmmio_read32(mmio, VIRTIO_ISR_OFF) & VIRTIO_ISR_USED_BIT) != 0 &&
        virtio_completion_ok() == 0) {
        vmmio_write32(mmio, VIRTIO_ISR_ACK_OFF,
                      VIRTIO_ISR_USED_BIT);
        g_virtio_last_used = used->idx;
        blk_stats_bump(BLK_STAT_WAITS);
        blk_completion_ok();
        return 0;
    }
    /* Completion (issue #47 step-b2): blk_wait_validated confirms the
     * used-ring advance + id + zero status (stale-pending retry
     * inside). On success clear the ISR latch so the level line
     * deasserts. Otherwise take the bounded poll below. */
    if (blk_wait_validated(virtio_completion_ok) == 0) {
        vmmio_write32(mmio, VIRTIO_ISR_ACK_OFF,
                      VIRTIO_ISR_USED_BIT);
        g_virtio_last_used = used->idx;
        return 0;
    }
    unsigned n = 0;
    while (used->idx == (uint16_t)last) {
        if (++n >= VIRTIO_POLL_N)
            return -1;
    }
    g_virtio_last_used = used->idx;
    if (used->ring[last % qsize].id != 0)
        return -1;
    if (*status != 0)
        return -1;
    return 0;
}

/* Select and arm the PD-owned IRQ for a device phys (issue #47
 * step-b2): match the IRQ/MSIX cap whose conveyed phys names this
 * device, register WAIT_ACK delivery, and latch the handle. MSIX is
 * conveyance-only in b2 (no AHCI backend yet) — stored, never armed.
 * No match or a failed register leaves the handle clear (pure poll). */
static void blk_arm_irq_for(unsigned long phys) {
    g_irq_handle = 0;
    g_irq_phys = 0;
    if (phys == 0)
        return;
    for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
        if (!g_caps[i].mapped)
            continue;
        if (g_caps[i].kind != BLK_CAP_IRQ &&
            g_caps[i].kind != BLK_CAP_MSIX)
            continue;
        if (g_caps[i].phys != phys)
            continue;
        if (g_caps[i].kind == BLK_CAP_MSIX)
            return;
        if (nexios_cap_irq_register(g_caps[i].handle,
                                    BLK_IRQ_MODE_WAIT_ACK) != 0)
            return;
        g_irq_handle = g_caps[i].handle;
        g_irq_phys = phys;
        return;
    }
}

/* Probe with all caps mapped (BIND time). Sets g_backend/g_sectors.
 * g_probe_stage records the first failing step for the degraded BIND
 * reply (spec: BIND aux carries sectors, or the stage when NONE). */
static int g_probe_stage = 0;
static void blk_probe(void) {
    g_probed = 1;
    g_backend = BLK_BACKEND_NONE;
    g_sectors = 0;
    g_probe_stage = 0;
#if defined(__x86_64__)
    for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
        if (!g_caps[i].mapped || g_caps[i].kind != BLK_CAP_MMIO)
            continue;
        unsigned base = (unsigned)g_caps[i].phys;
        /* Poll-only probe: mask device IRQs until the channel is
         * selected and its IRQ is armed (no stray IRQs pre-arm). */
        ata_set_nien(base, 1);
        static const unsigned char drives[2] = { 0xE0, 0xF0 };
        for (unsigned d = 0; d < 2; ++d) {
            uint64_t sectors = 0;
            if (ata_identify(base, drives[d], &sectors) == 0) {
                g_ata_base = base;
                g_ata_drive = drives[d];
                g_backend = BLK_BACKEND_ATA_PIO;
                g_sectors = sectors;
                blk_arm_irq_for(base);
                if (g_irq_handle != 0)
                    ata_set_nien(base, 0);
                return;
            }
        }
    }
#else
    for (unsigned i = 0; i < BLK_MAX_CAPS; ++i) {
        if (!g_caps[i].mapped || g_caps[i].kind != BLK_CAP_MMIO ||
            g_caps[i].va == 0)
            continue;
            /* Intra-page offset (issue #47 step-b1): the SDF cap
             * covers whole pages from the aligned base; the true
             * window starts at phys (conveyed exact), so registers
             * live at va + (phys & 0xFFF). */
            unsigned long reg_va =
                g_caps[i].va + (g_caps[i].phys & 0xFFFUL);
            uint64_t sectors = 0;
            int pr = virtio_probe(reg_va, &sectors);
            if (pr == 0) {
                g_mmio_va = reg_va;
                g_backend = BLK_BACKEND_VIRTIO_MMIO;
                g_sectors = sectors;
                g_virtio_qsize = BLK_QSIZE;
                for (unsigned j = 0; j < BLK_MAX_CAPS; ++j) {
                    if (g_caps[j].mapped &&
                        g_caps[j].kind == BLK_CAP_DMA_REGION &&
                        g_caps[j].va != 0) {
                        g_frame_va = g_caps[j].va;
                        g_frame_phys = g_caps[j].phys;
                    }
                }
                blk_arm_irq_for(g_caps[i].phys);
                if (g_irq_handle != 0) {
                    /* Stale latch from before the arm would deliver
                     * an immediate spurious IRQ: clear it now (the
                     * queue setup ACKs again before the first wait). */
                    virtio_ack_stale_isr(reg_va);
                }
                return;
            }
            g_probe_stage = pr;
        }
    if (g_probe_stage == 0)
        g_probe_stage = 1; /* no MMIO cap to probe */
#endif
}

static int blk_do_read(uint64_t lba) {
    if (g_backend == BLK_BACKEND_NONE || g_sectors == 0)
        return -BLK_EIO;
    if (lba >= g_sectors)
        return -BLK_EINVAL;
#if defined(__x86_64__)
    if (g_backend == BLK_BACKEND_ATA_PIO) {
        unsigned char *dst =
            (unsigned char *)(g_frame_va + BLK_DATA_OFF);
        if (g_frame_va == 0)
            return -BLK_EIO;
        if (ata_read_sector(g_ata_base, g_ata_drive, lba, dst))
            return -BLK_EIO;
        return 0;
    }
#else
    if (g_backend == BLK_BACKEND_VIRTIO_MMIO) {
        if (virtio_transfer(0, lba))
            return -BLK_EIO;
        return 0;
    }
#endif
    return -BLK_ENOSYS;
}

static int blk_do_write(uint64_t lba) {
    if (g_backend == BLK_BACKEND_NONE || g_sectors == 0)
        return -BLK_EIO;
    if (lba >= g_sectors)
        return -BLK_EINVAL;
#if defined(__x86_64__)
    if (g_backend == BLK_BACKEND_ATA_PIO) {
        unsigned char *src =
            (unsigned char *)(g_frame_va + BLK_DATA_OFF);
        if (g_frame_va == 0)
            return -BLK_EIO;
        if (ata_write_sector(g_ata_base, g_ata_drive, lba, src))
            return -BLK_EIO;
        return 0;
    }
#else
    if (g_backend == BLK_BACKEND_VIRTIO_MMIO) {
        if (virtio_transfer(1, lba))
            return -BLK_EIO;
        return 0;
    }
#endif
    return -BLK_ENOSYS;
}

static unsigned char g_loopback[512];

int main(void) {
    blk_assert_shape();
    for (unsigned i = 0; i < sizeof(g_loopback); ++i)
        g_loopback[i] = (unsigned char)(i & 0xFF);
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue;
        struct BlockRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy =
            sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue;

        blk_purge_on_epoch(req.epoch);

        struct BlockRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;

        switch (req.op) {
        case BLK_OP_CAP_GET: {
            unsigned idx = req.handle;
            int r = blk_map_cap(idx, (uint8_t)req.status_op, req.arg,
                                req.lba, req.aux, req.rsvd);
            /* Publish the shared window: the frame can arrive on any
             * CAP_GET round; transfers and virtio queue setup consume
             * g_frame_va/_phys. Fresh frames drop queue readiness.
             * (Re)publishing the IRQ stats header rides the window. */
            if (r == 0 && idx < BLK_MAX_CAPS &&
                g_caps[idx].kind == BLK_CAP_DMA_REGION) {
                g_frame_va = g_caps[idx].va;
                g_frame_phys = g_caps[idx].phys;
                g_virtio_ready = 0;
                blk_stats_publish();
            }
            rep.handle = req.handle;
            rep.arg = req.arg;
            rep.status_op = req.status_op;
            rep.status = r == 0 ? 0 : blk_neg((uint16_t)-r);
            break;
        }
        case BLK_OP_BIND:
        case BLK_OP_GEOMETRY: {
            if (req.op == BLK_OP_BIND && !g_probed)
                blk_probe();
            rep.status_op = (uint16_t)g_backend;
            rep.flags = req.flags;
            rep.lba = 0;
            rep.arg = BLK_SECTOR;
            /* Degraded: aux carries the probe stage (spec §wire),
             * sectors when bound. */
            rep.aux = (g_backend == BLK_BACKEND_NONE)
                          ? (uint64_t)g_probe_stage
                          : g_sectors;
            rep.status =
                (g_backend == BLK_BACKEND_NONE && g_sectors != 0)
                    ? blk_neg(BLK_EIO)
                    : 0;
            break;
        }
        case BLK_OP_READ_BLOCK: {
            int r;
            if (g_inject_once) {
                g_inject_once = 0;
                r = -BLK_EIO;
            } else {
                r = blk_do_read(req.lba);
            }
            rep.lba = req.lba;
            rep.arg = req.arg;
            rep.status = r == 0 ? 0 : blk_neg((uint16_t)-r);
            break;
        }
        case BLK_OP_WRITE_BLOCK: {
            int r;
            if (g_inject_once) {
                g_inject_once = 0;
                r = -BLK_EIO;
            } else {
                r = blk_do_write(req.lba);
            }
            rep.lba = req.lba;
            rep.arg = req.arg;
            rep.status = r == 0 ? 0 : blk_neg((uint16_t)-r);
            break;
        }
        case BLK_OP_FLUSH:
            /* No write-back cache: writes are direct. */
            rep.status = 0;
            break;
        case BLK_OP_LOOPBACK:
            rep.status = (g_loopback[0] == 0x00 &&
                          g_loopback[511] == (unsigned char)0xFF)
                             ? 0
                             : blk_neg(BLK_EIO);
            break;
        case BLK_OP_DMA_MAP:
            rep.status = (req.handle == 0) ? blk_neg(BLK_EACCES)
                                           : blk_neg(BLK_ENOSYS);
            break;
        case BLK_OP_INJECT_MID_TRANSFER:
            g_inject_once = 1;
            rep.status = 0;
            break;
        case BLK_OP_INJECT_IRQ_SKIP:
            g_inject_irq_skip_once = 1;
            rep.status = 0;
            break;
        default:
            rep.status = blk_neg(BLK_ENOSYS);
            break;
        }

        ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
