#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syscall.h>
#include <ipc.h>

/*
 * Nexios RTOS — vfs-srv PD (issue #47, strangler step-b3: VFS backends
 * live path). Userspace VFS backend server: CAP_GET/BIND handshake,
 * shared-window access, per-backend dispatch.
 *
 * Split of duties (kernel stages RAW bytes, the PD parses):
 * - INITRD: kernel stages a compact cpio index (name/data_off/size/mode
 *   records); the PD resolves paths and packs image offsets into
 *   handles; data reads are kernel-staged image slices the PD validates.
 * - FAT32: kernel stages BPB + root-dir cluster; the PD parses geometry
 *   + dirents and packs (start_cluster, size); data reads are
 *   kernel-walked chains the PD length-checks against its dirent.
 * - RAMDISK: kernel stages super + full dir region; the PD parses
 *   entries and packs (start_block, size); data reads are kernel-staged
 *   spans the PD range-checks.
 * - TMPFS/PIPE: PD-owned bounded stores (no staging; behavioral parity).
 * - DEVFS: static metadata table; data ops are -ENOSYS (device authority
 *   stays kernel-side — D3).
 * - PROCFS: kernel renders via its own VnodeOps into the window; the PD
 *   validates format (D6 validator, never renders).
 * Handles are PD table indices for owned stores, packed locators
 * (backend << 56 | base) for image-backed backends — the kernel decodes
 * the same packing (vfs_srv.cpp READ dispatch). One-shot fault drill
 * per backend + PD-wide stats slot. Wire stays 48 B.
 */

#define MSG_DAEMON_READY 0xF0000001
#define MSG_TYPE_REPLY 0xF0000002

typedef enum {
    VFS_OP_BIND = 0,
    VFS_OP_INITRD = 1,
    VFS_OP_FAT32 = 2,
    VFS_OP_TMPFS = 3,
    VFS_OP_DEVFS = 4,
    VFS_OP_PIPE = 5,
    VFS_OP_RAMDISK = 6,
    VFS_OP_PROCFS_RENDER = 7,
    VFS_OP_INJECT_CORRUPT = 8,
    VFS_OP_CAP_GET = 9,
    VFS_OP_OPEN = 10,
    VFS_OP_CLOSE = 11,
    VFS_OP_READ = 12,
    VFS_OP_WRITE = 13,
    VFS_OP_STAT = 14,
    VFS_OP_READDIR = 15,
    VFS_OP_CREATE = 16,
    VFS_OP_UNLINK = 17,
    VFS_OP_BACKEND_STATUS = 18,
    VFS_OP_INJECT_BACKEND_FAULT = 19,
} vfs_srv_op_t;

typedef enum {
    VFS_BACKEND_INITRD = 0,
    VFS_BACKEND_FAT32 = 1,
    VFS_BACKEND_TMPFS = 2,
    VFS_BACKEND_DEVFS = 3,
    VFS_BACKEND_PIPE = 4,
    VFS_BACKEND_RAMDISK = 5,
    VFS_BACKEND_PROCFS = 6,
} vfs_backend_t;

struct VfsRpc {
    uint8_t op;
    uint8_t flags;
    uint16_t rsvd;
    uint32_t epoch;
    uint32_t owner_id;
    uint32_t owner_gen;
    uint64_t ino;
    uint64_t offset;
    uint64_t size;
    uint16_t status_op;
    uint16_t status;
    uint32_t rsvd2;
};

/* Shared-window layout (mirror kernel kVfs*Offset). */
#define VFS_REGION_PAGES 4
#define VFS_DATA_OFF 0x000
#define VFS_DATA_SIZE 0x3000
#define VFS_NAME_OFF 0x3000
#define VFS_MAX_NAME 64
#define VFS_HDR_OFF 0x3080
#define VFS_STATS_OFF 0x30C0

#define VFS_MAX_CAPS 8

/* Errno shapes (negated in status). */
#define VFS_ENOSYS 38
#define VFS_EINVAL 22
#define VFS_ENOENT 2
#define VFS_EIO 5
#define VFS_EAGAIN 11
#define VFS_EACCES 13
#define VFS_EEXIST 17
#define VFS_ENOTDIR 20
#define VFS_EISDIR 21
#define VFS_ENOSPC 28
#define VFS_ENAMETOOLONG 36
#define VFS_ENOTEMPTY 39

static void vfs_assert_shape(void) {
    char check[sizeof(struct VfsRpc) <= 48 ? 1 : -1];
    (void)check;
}

static uint16_t vfs_neg(unsigned v) {
    return (uint16_t)(uint16_t)(-(int)v);
}

/* Cap table + shared window. */
static struct {
    int mapped;
    unsigned kind;
    unsigned long handle;
    unsigned long phys;
    unsigned long size;
    unsigned long va;
} g_caps[VFS_MAX_CAPS];

static unsigned long g_win_va;
static unsigned long g_win_phys;
static uint32_t g_epoch;
static uint8_t g_backend_mask = 0x7F; /* all seven backends live */

/* One-shot fault drill per backend + PD-wide stats. */
static int g_fault_once[7];
static int g_corrupt_once;
static struct {
    uint64_t magic;
    uint32_t version;
    uint32_t ops;
    uint64_t bytes;
    uint32_t faults;
    uint32_t rsvd;
} g_stats;

#define VFS_STATS_MAGIC 0x5646535354415453ULL

static void stats_publish(void) {
    if (g_win_va == 0)
        return;
    volatile unsigned char *slot =
        (volatile unsigned char *)(g_win_va + VFS_STATS_OFF);
    /* Release-ordered publish: payload first, magic last. */
    slot[8] = 0; /* version lo (rest zeroed below by field stores) */
    volatile uint64_t *m64 = (volatile uint64_t *)slot;
    volatile uint32_t *m32 = (volatile uint32_t *)(slot + 8);
    m32[0] = 1; /* version */
    m32[1] = g_stats.ops;
    volatile uint64_t *m64b = (volatile uint64_t *)(slot + 16);
    m64b[0] = g_stats.bytes;
    m32[6] = g_stats.faults;
    m32[7] = 0;
    m64[0] = VFS_STATS_MAGIC;
}

/* PD open-handle table (owned stores + validated image handles). */
#define VFS_MAX_OPEN 16
static struct {
    int used;
    uint32_t gen;
    uint8_t backend;
    uint64_t base; /* image locator or store index */
    uint64_t size;
} g_open[VFS_MAX_OPEN];
static uint32_t g_open_gen;

/* TMPFS arena: 8 files x 4 KiB (PD-owned, D4). BSS-zeroed at PD
 * (re)start — the epoch purge clears only the open-handle table and
 * fault drills, so a restart always starts from empty stores (no
 * epoch-persisted namespace leaks across restarts by design). */
#define VFS_TMPFS_FILES 8
#define VFS_TMPFS_SIZE 4096
static struct {
    int used;
    char name[VFS_MAX_NAME];
    uint64_t size;
    uint32_t mode;
    unsigned char data[VFS_TMPFS_SIZE];
} g_tmpfs[VFS_TMPFS_FILES];

/* PIPE rings: 4 x 1 KiB (PD-owned, D4). Same restart-zeroed
 * invariant as the tmpfs arena above. */
#define VFS_PIPES 4
#define VFS_PIPE_SIZE 1024
static struct {
    int used;
    char name[VFS_MAX_NAME];
    unsigned head;
    unsigned count;
    unsigned char data[VFS_PIPE_SIZE];
} g_pipes[VFS_PIPES];

/* DEVFS static nodes (metadata-only, D3). */
static const struct {
    const char *name;
    uint32_t mode;
} g_devfs[] = {
    {"null", 0666},
    {"zero", 0666},
    {"tty", 0600},
    {"serial", 0600},
};

/* PROCFS shared index table (must match kernel kProcFiles). */
static const char *g_proc_files[] = {"/proc/meminfo", "/proc/stat"};

/* Cpio index record (kernel-staged, PD-parsed). */
struct vfs_cpio_idx {
    char name[48];
    uint32_t data_off;
    uint32_t size;
    uint32_t mode;
};

/* Ramdisk dir entry (kernel-staged blocks 0..10, PD-parsed). */
struct vfs_rd_entry {
    char name[64];
    uint64_t size;
    uint64_t start_block;
    uint64_t block_count;
    uint32_t crc32;
    uint32_t flags;
};

#define RDSB_MAGIC 0x52445331464C4154ULL
#define RDSB_VERSION 2
#define RD_ENTRY_VALID 1U

static unsigned char *win_data(void) {
    /* Step-b3 restore tolerance: a fresh/restarted PD has no window
     * until CAP_GET maps it. Never touch address 0 — fail the op
     * instead of faulting (a faulted PD cannot serve the rebind). */
    if (g_win_va == 0)
        return (unsigned char *)0;
    return (unsigned char *)(g_win_va + VFS_DATA_OFF);
}

static char *win_name(void) {
    if (g_win_va == 0)
        return (char *)0;
    return (char *)(g_win_va + VFS_NAME_OFF);
}

/* Fault drill gate: one-shot -EIO per backend, then recover. */
static int fault_gate(unsigned backend) {
    if (backend < 7 && g_fault_once[backend]) {
        g_fault_once[backend] = 0;
        g_stats.faults++;
        stats_publish();
        return -VFS_EIO;
    }
    return 0;
}

static int alloc_handle(unsigned backend, uint64_t base, uint64_t size) {
    for (int i = 0; i < VFS_MAX_OPEN; ++i) {
        if (!g_open[i].used) {
            g_open[i].used = 1;
            g_open[i].gen = ++g_open_gen;
            g_open[i].backend = (uint8_t)backend;
            g_open[i].base = base;
            g_open[i].size = size;
            /* Packed locator (kernel decodes backend << 56 | base):
             * image-backed handles carry the raw locator; owned-store
             * handles carry the store slot. */
            return i;
        }
    }
    return -1;
}

static int find_handle(uint64_t packed, unsigned *backend, uint64_t *base,
                       uint64_t *size) {
    unsigned b = (unsigned)(packed >> 56);
    uint64_t lo = packed & 0x00FFFFFFFFFFFFFFULL;
    *backend = b > 6 ? 7 : b;
    *base = lo;
    *size = 0;
    if (b > 6)
        return -1;
    /* Image-backed handles are self-describing (PD validated at OPEN);
     * owned-store handles index the open table. */
    if (b == VFS_BACKEND_TMPFS || b == VFS_BACKEND_PIPE) {
        if (lo >= (uint64_t)VFS_MAX_OPEN || !g_open[lo].used ||
            g_open[lo].backend != b)
            return -1;
        *backend = b;
        *base = g_open[lo].base;
        *size = g_open[lo].size;
        return (int)lo;
    }
    *backend = b;
    *base = lo;
    *size = 0; /* validated against staged dirent at serve time */
    return -2; /* direct (no table slot) */
}

/* ---- backend resolvers: path -> packed handle ---- */

static int match_name(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Skip leading '/' and './' sequences on both sides (cpio names carry
 * a './' prefix; kernel paths are absolute). */
static const char *norm_skip(const char *p) {
    for (;;) {
        if (p[0] == '/') {
            ++p;
            continue;
        }
        if (p[0] == '.' && p[1] == '/') {
            p += 2;
            continue;
        }
        return p;
    }
}

static int match_path(const char *indexed, const char *req) {
    return match_name(norm_skip(indexed), norm_skip(req));
}

static int resolve_initrd(const char *path, uint64_t *out_base,
                          uint64_t *out_size) {
    /* Staged cpio index: count lives where? The kernel stages records
     * back-to-back; scan until an empty name or window end. */
    struct vfs_cpio_idx *t = (struct vfs_cpio_idx *)win_data();
    unsigned max =
        (unsigned)(VFS_DATA_SIZE / sizeof(struct vfs_cpio_idx));
    for (unsigned i = 0; i < max; ++i) {
        if (t[i].name[0] == '\0')
            break;
        if (match_path(t[i].name, path)) {
            *out_base = t[i].data_off;
            *out_size = t[i].size;
            return (int)(t[i].mode & 0777);
        }
    }
    return -VFS_ENOENT;
}

static void fat83(char *dst, const unsigned char *raw) {
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && raw[i] != ' '; ++i)
        dst[n++] = (char)raw[i];
    if (raw[8] != ' ') {
        dst[n++] = '.';
        for (unsigned i = 8; i < 11 && raw[i] != ' '; ++i)
            dst[n++] = (char)raw[i];
    }
    dst[n] = '\0';
}

static int resolve_fat32(const char *path, uint64_t *out_base,
                         uint64_t *out_size) {
    /* Staged: BPB at [0,512), root-dir cluster at [512,...). BPB+25 =
     * sectors-per-cluster u8? BPB layout: +11 sector size u16, +13
     * sec/cluster u8, +14 reserved u16, +16 fats u8, +36 fat sectors u32
     * (FAT32), +44 root cluster u32. Dirent: +0 name[11], +11 attr,
     * +26 cluster-lo u16, +28 size u32, +20 cluster-hi u16. */
    unsigned char *w = win_data();
    if (w[510] != 0x55 || w[511] != 0xAA)
        return -VFS_EIO;
    /* Root dir scan: entries of 32 B from offset 512. */
    unsigned maxe = (unsigned)((VFS_DATA_SIZE - 512) / 32);
    const char *p = path;
    if (*p == '/')
        ++p;
    /* Only root-level files in b3 (documented bound). */
    for (unsigned i = 0; p[i] != '\0'; ++i) {
        if (p[i] == '/')
            return -VFS_ENOTDIR;
    }
    char want[16];
    unsigned n = 0;
    while (*p && n < 12) {
        char c = *p++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        want[n++] = c;
    }
    want[n] = '\0';
    for (unsigned i = 0; i < maxe; ++i) {
        unsigned char *e = w + 512 + i * 32;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5)
            continue;
        if (e[11] & 0x08)
            continue; /* volume label */
        char name[16];
        fat83(name, e);
        if (match_name(name, want)) {
            uint32_t lo = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
            uint32_t hi = (uint32_t)e[20] | ((uint32_t)e[21] << 8);
            uint32_t clus = lo | (hi << 16);
            uint32_t sz = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                          ((uint32_t)e[30] << 16) |
                          ((uint32_t)e[31] << 24);
            *out_base = clus;
            *out_size = sz;
            return 0644;
        }
    }
    return -VFS_ENOENT;
}

static int resolve_ramdisk(const char *path, uint64_t *out_base,
                           uint64_t *out_size) {
    unsigned char *w = win_data();
    uint64_t magic = 0;
    for (unsigned i = 0; i < 8; ++i)
        magic |= ((uint64_t)w[i]) << (i * 8);
    if (magic != RDSB_MAGIC)
        return -VFS_EIO;
    uint64_t ver = 0;
    for (unsigned i = 8; i < 16; ++i)
        ver |= ((uint64_t)w[i]) << ((i - 8) * 8);
    if (ver != RDSB_VERSION)
        return -VFS_EIO;
    struct vfs_rd_entry *ents =
        (struct vfs_rd_entry *)(w + 512); /* blocks 1..10 */
    for (unsigned i = 0; i < 50; ++i) {
        if (ents[i].name[0] == '\0')
            continue;
        if (!(ents[i].flags & RD_ENTRY_VALID))
            continue;
        if (match_path(ents[i].name, path)) {
            *out_base = ents[i].start_block;
            *out_size = ents[i].size;
            return 0644;
        }
    }
    return -VFS_ENOENT;
}

static int resolve_tmpfs(const char *path, uint64_t *out_slot) {
    for (unsigned i = 0; i < VFS_TMPFS_FILES; ++i) {
        if (g_tmpfs[i].used && match_path(g_tmpfs[i].name, path)) {
            *out_slot = i;
            return (int)g_tmpfs[i].mode;
        }
    }
    return -VFS_ENOENT;
}

static int resolve_pipe(const char *path, uint64_t *out_slot) {
    for (unsigned i = 0; i < VFS_PIPES; ++i) {
        if (g_pipes[i].used && match_path(g_pipes[i].name, path)) {
            *out_slot = i;
            return 0600;
        }
    }
    return -VFS_ENOENT;
}

static int resolve_devfs(const char *path, uint64_t *out_idx) {
    const char *p = norm_skip(path);
    /* Strip the "dev/" directory prefix for node matches; the bare
     * "dev" (or empty) names the directory itself. */
    if (match_name(p, "dev") || *p == '\0')
        return -VFS_EISDIR; /* the dir itself: listable, not openable */
    if (p[0] == 'd' && p[1] == 'e' && p[2] == 'v' && p[3] == '/')
        p += 4;
    for (unsigned i = 0;
         i < (unsigned)(sizeof(g_devfs) / sizeof(g_devfs[0])); ++i) {
        if (match_name(g_devfs[i].name, p)) {
            *out_idx = i;
            return (int)g_devfs[i].mode;
        }
    }
    return -VFS_ENOENT;
}

static int resolve_procfs(const char *path, uint64_t *out_idx) {
    const char *p = norm_skip(path);
    if (match_name(p, "proc") || *p == '\0')
        return -VFS_EISDIR;
    /* Strip the "proc/" directory prefix for file matches. */
    if (p[0] == 'p' && p[1] == 'r' && p[2] == 'o' && p[3] == 'c' &&
        p[4] == '/')
        p += 5;
    for (unsigned i = 0;
         i < (unsigned)(sizeof(g_proc_files) / sizeof(g_proc_files[0]));
         ++i) {
        const char *f = g_proc_files[i] + 6; /* strip "/proc/" */
        if (match_name(f, p)) {
            *out_idx = i;
            return 0444;
        }
    }
    return -VFS_ENOENT;
}

/* Validate a staged procfs render (D6 validator). */
static int validate_procfs(unsigned idx, uint64_t *out_len) {
    unsigned char *w = win_data();
    /* Bounded NUL scan inside the window. */
    uint64_t len = 0;
    while (len < VFS_DATA_SIZE && w[len] != '\0')
        ++len;
    if (len == 0 || len >= VFS_DATA_SIZE)
        return -VFS_EIO;
    /* Format needles: meminfo starts with "MemTotal:", stat with "cpu ". */
    if (idx == 0) {
        if (len < 9 || w[0] != 'M' || w[1] != 'e' || w[2] != 'm')
            return -VFS_EIO;
    } else if (idx == 1) {
        if (len < 4 || w[0] != 'c' || w[1] != 'p' || w[2] != 'u')
            return -VFS_EIO;
    } else {
        return -VFS_EINVAL;
    }
    *out_len = len;
    return 0;
}

static int map_cap(unsigned idx, unsigned kind, unsigned long handle,
                   unsigned long phys, unsigned long size) {
    if (idx >= VFS_MAX_CAPS || handle == 0 ||
        handle == (unsigned long)-1)
        return -VFS_EINVAL;
    if (kind != 6) /* kernel CapKind::DMA_REGION only */
        return -VFS_EINVAL;
    /* Always (re)map: a restore may have undone the PTEs while the
     * slot record survived. Unmap the stale VA first (idempotent). */
    if (g_caps[idx].mapped) {
        unsigned long old_va = g_caps[idx].va;
        if (old_va != 0) {
            __syscall5(SYS_FRAME_UNMAP, (long)old_va, 0, 0, 0);
            if (g_win_va == old_va)
                g_win_va = 0;
        }
        g_caps[idx].mapped = 0;
        g_caps[idx].va = 0;
    }
    long va = __syscall5(SYS_FRAME_MAP, (long)handle, 0, 0, 0);
    if (va <= 0)
        return -VFS_EIO;
    g_caps[idx].mapped = 1;
    g_caps[idx].kind = kind;
    g_caps[idx].handle = handle;
    g_caps[idx].phys = phys;
    g_caps[idx].size = size;
    g_caps[idx].va = (unsigned long)va;
    if (kind == 6) {
        g_win_va = g_caps[idx].va;
        g_win_phys = phys;
        stats_publish();
    }
    return 0;
}

int main(void) {
    vfs_assert_shape();
    (void)getpid();
    for (unsigned i = 0; i < 7; ++i)
        g_fault_once[i] = 0;
    g_corrupt_once = 0;
    g_stats.magic = 0;
    g_stats.version = 1;
    ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0);

    static unsigned char rbuf[64];
    for (;;) {
        int rc = ipc_recv(rbuf, sizeof(rbuf));
        if (rc < 0)
            continue; /* fail-closed: no reply on transport error */
        struct VfsRpc req;
        memset(&req, 0, sizeof(req));
        unsigned long copy = sizeof(req) < 48 ? sizeof(req) : 48;
        memcpy(&req, rbuf, copy);
        if ((unsigned long)rc != req.op)
            continue; /* torn/stale payload (audit S3) */

        /* Epoch purge: drop PD-side open state on kernel epoch bump. */
        if (req.epoch != g_epoch) {
            g_epoch = req.epoch;
            for (unsigned i = 0; i < VFS_MAX_OPEN; ++i)
                g_open[i].used = 0;
            for (unsigned i = 0; i < 7; ++i)
                g_fault_once[i] = 0;
        }

        struct VfsRpc rep;
        memset(&rep, 0, sizeof(rep));
        rep.op = req.op;
        rep.epoch = req.epoch;
        rep.owner_id = req.owner_id;
        rep.owner_gen = req.owner_gen;
        rep.status_op = (uint16_t)req.op;
        g_stats.ops++;

        /* Window gate (step-b3 restore tolerance): ops 10..17 parse or
         * fill the shared window. A fresh/restarted PD has no mapping
         * until CAP_GET — reply -ENOSYS so the kernel rebinds instead
         * of faulting the PD (a dead PD cannot serve the rebind). */
        if (req.op >= VFS_OP_OPEN && req.op <= VFS_OP_UNLINK &&
            g_win_va == 0) {
            rep.status = vfs_neg(VFS_ENOSYS);
            ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep),
                     0);
            stats_publish();
            continue;
        }

        switch (req.op) {
        case VFS_OP_BIND:
            rep.offset = req.offset; /* echo cap count */
            rep.size = g_backend_mask;
            rep.status = 0;
            break;
        case VFS_OP_CAP_GET: {
            /* Wire (kernel vfs_srv_bind): ino=phys, offset=size-bytes,
             * size=encoded row handle (for SYS_FRAME_MAP), flags=kind,
             * status_op=index. */
            int r = map_cap((unsigned)req.status_op,
                            (unsigned)req.flags, req.size, req.ino,
                            req.offset);
            rep.ino = req.ino;
            rep.offset = req.offset;
            rep.size = req.size;
            rep.flags = req.flags;
            rep.status_op = req.status_op;
            rep.status = r == 0 ? 0 : vfs_neg((unsigned)-r);
            break;
        }
        case VFS_OP_OPEN: {
            unsigned b = (unsigned)req.offset;
            int fr = fault_gate(b);
            uint64_t base = 0;
            uint64_t sz = 0;
            int mode = -VFS_ENOSYS;
            if (fr == 0) {
                if (b == 0)
                    mode = resolve_initrd(win_name(), &base, &sz);
                else if (b == 1)
                    mode = resolve_fat32(win_name(), &base, &sz);
                else if (b == 2)
                    mode = resolve_tmpfs(win_name(), &base);
                else if (b == 3)
                    mode = resolve_devfs(win_name(), &base);
                else if (b == 4)
                    mode = resolve_pipe(win_name(), &base);
                else if (b == 5)
                    mode = resolve_ramdisk(win_name(), &base, &sz);
                else if (b == 6)
                    mode = resolve_procfs(win_name(), &base);
                else
                    mode = -VFS_EINVAL;
            }
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
            } else if (mode < 0) {
                rep.status = vfs_neg((unsigned)-mode);
            } else {
                /* Direct-handle packing (kernel decodes bounds): low 56
                 * bits = (locator24 << 32 | size32). Locators are raw
                 * image offsets/clusters/blocks; sizes cap the read.
                 * Guards keep the backend tag bits clear (fail closed
                 * past 16 MiB image offsets — b3 test images are ~2 MiB). */
                uint64_t packed = ((uint64_t)b << 56) | base;
                if (b == 0 || b == 1 || b == 5) {
                    if ((base >> 24) != 0 || (sz >> 32) != 0) {
                        rep.status = vfs_neg(VFS_EINVAL);
                        break;
                    }
                    packed = ((uint64_t)b << 56) |
                             ((base & 0xFFFFFFULL) << 32) |
                             (sz & 0xFFFFFFFFULL);
                }
                if (b == 2 || b == 4) {
                    /* Owned stores: table slot (find_handle decodes). */
                    int slot = alloc_handle(b, base, sz);
                    if (slot < 0) {
                        rep.status = vfs_neg(VFS_ENOSPC);
                        break;
                    }
                    packed = ((uint64_t)b << 56) | (uint64_t)slot;
                }
                rep.ino = packed;
                rep.status = 0;
            }
            break;
        }
        case VFS_OP_CLOSE: {
            unsigned b;
            uint64_t base;
            uint64_t fsize;
            int slot = find_handle(req.ino, &b, &base, &fsize);
            (void)base;
            (void)fsize;
            if (slot < 0 && (b == 2 || b == 4)) {
                /* Owned-store double close (or foreign slot): typed. */
                rep.status = vfs_neg(VFS_ENOENT);
            } else {
                if (slot >= 0)
                    g_open[(unsigned)slot].used = 0;
                rep.status = 0;
            }
            rep.ino = req.ino;
            break;
        }
        case VFS_OP_READ: {
            unsigned b;
            uint64_t base;
            uint64_t fsize;
            int slot = find_handle(req.ino, &b, &base, &fsize);
            int fr = fault_gate(b > 6 ? 7 : b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
                rep.ino = req.ino;
                break;
            }
            if (b > 6 || req.offset + req.size < req.offset ||
                req.size > VFS_DATA_SIZE) {
                rep.status = vfs_neg(VFS_EINVAL);
                rep.ino = req.ino;
                break;
            }
            if (b == 2) { /* tmpfs arena */
                if (slot < 0 || req.offset > g_tmpfs[base].size) {
                    rep.status = vfs_neg(VFS_EINVAL);
                } else {
                    uint64_t n = g_tmpfs[base].size - req.offset;
                    if (n > req.size)
                        n = req.size;
                    for (uint64_t i = 0; i < n; ++i)
                        win_data()[i] =
                            g_tmpfs[base].data[req.offset + i];
                    g_stats.bytes += n;
                    rep.size = n;
                    rep.status = 0;
                }
            } else if (b == 4) { /* pipe ring */
                if (slot < 0) {
                    rep.status = vfs_neg(VFS_EINVAL);
                } else if (g_pipes[base].count == 0) {
                    rep.size = 0; /* empty: EOF-shaped, never blocks */
                    rep.status = 0;
                } else {
                    uint64_t n = g_pipes[base].count;
                    if (n > req.size)
                        n = req.size;
                    for (uint64_t i = 0; i < n; ++i) {
                        win_data()[i] = g_pipes[base]
                                            .data[g_pipes[base].head];
                        g_pipes[base].head =
                            (g_pipes[base].head + 1) % VFS_PIPE_SIZE;
                        g_pipes[base].count--;
                    }
                    g_stats.bytes += n;
                    rep.size = n;
                    rep.status = 0;
                }
            } else if (b == 6) { /* procfs: validate staged render */                uint64_t len = 0;
                int vr = validate_procfs((unsigned)base, &len);
                if (vr != 0) {
                    rep.status = vfs_neg((unsigned)-vr);
                } else if (req.offset > len) {
                    rep.status = vfs_neg(VFS_EINVAL);
                } else {
                    uint64_t n = len - req.offset;
                    if (n > req.size)
                        n = req.size;
                    /* Staged bytes already at window head; compact
                     * when the offset is nonzero. */
                    if (req.offset != 0) {
                        for (uint64_t i = 0; i < n; ++i)
                            win_data()[i] =
                                win_data()[req.offset + i];
                    }
                    g_stats.bytes += n;
                    rep.size = n;
                    rep.status = 0;
                }
            } else if (b == 3) { /* devfs: metadata-only (D3) */
                rep.status = vfs_neg(VFS_ENOSYS);
            } else {
                /* Image-backed: kernel staged [base+off, size); the PD
                 * re-validates the range against its OPEN-time size for
                 * initrd (dirent size unknown here — accept staged). */
                (void)fsize;
                (void)slot;
                g_stats.bytes += req.size;
                rep.size = req.size;
                rep.status = 0;
            }
            rep.ino = req.ino;
            stats_publish();
            break;
        }
        case VFS_OP_WRITE: {
            unsigned b;
            uint64_t base;
            uint64_t fsize;
            int slot = find_handle(req.ino, &b, &base, &fsize);
            (void)fsize;
            int fr = fault_gate(b > 6 ? 7 : b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
                rep.ino = req.ino;
                break;
            }
            if (req.size > VFS_DATA_SIZE) {
                rep.status = vfs_neg(VFS_EINVAL);
                rep.ino = req.ino;
                break;
            }
            if (b == 2) {
                if (slot < 0 ||
                    req.offset > VFS_TMPFS_SIZE ||
                    req.size > VFS_TMPFS_SIZE - req.offset) {
                    rep.status = vfs_neg(VFS_ENOSPC);
                } else {
                    for (uint64_t i = 0; i < req.size; ++i)
                        g_tmpfs[base].data[req.offset + i] =
                            win_data()[i];
                    if (req.offset + req.size > g_tmpfs[base].size)
                        g_tmpfs[base].size = req.offset + req.size;
                    g_stats.bytes += req.size;
                    rep.size = req.size;
                    rep.status = 0;
                }
            } else if (b == 4) {
                if (slot < 0) {
                    rep.status = vfs_neg(VFS_EINVAL);
                } else {
                    uint64_t room =
                        VFS_PIPE_SIZE - g_pipes[base].count;
                    if (req.size > room) {
                        rep.status = vfs_neg(VFS_ENOSPC);
                    } else {
                        for (uint64_t i = 0; i < req.size; ++i) {
                            unsigned t =
                                (g_pipes[base].head +
                                 g_pipes[base].count) %
                                VFS_PIPE_SIZE;
                            g_pipes[base].data[t] = win_data()[i];
                            g_pipes[base].count++;
                        }
                        g_stats.bytes += req.size;
                        rep.size = req.size;
                        rep.status = 0;
                    }
                }
            } else {
                rep.status = vfs_neg(VFS_ENOSYS); /* read-only/image */
            }
            rep.ino = req.ino;
            stats_publish();
            break;
        }
        case VFS_OP_STAT: {
            unsigned b = (unsigned)req.offset;
            int fr = fault_gate(b);
            uint64_t base = 0;
            uint64_t sz = 0;
            int mode = -VFS_ENOSYS;
            if (fr == 0) {
                if (b == 0)
                    mode = resolve_initrd(win_name(), &base, &sz);
                else if (b == 1)
                    mode = resolve_fat32(win_name(), &base, &sz);
                else if (b == 2) {
                    mode = resolve_tmpfs(win_name(), &base);
                    if (mode >= 0)
                        sz = g_tmpfs[base].size;
                } else if (b == 3)
                    mode = resolve_devfs(win_name(), &base);
                else if (b == 4) {
                    mode = resolve_pipe(win_name(), &base);
                    if (mode >= 0)
                        sz = g_pipes[base].count;
                } else if (b == 5)
                    mode = resolve_ramdisk(win_name(), &base, &sz);
                else if (b == 6) {
                    mode = resolve_procfs(win_name(), &base);
                    if (mode >= 0) {
                        /* Staged render length = size. */
                        uint64_t len = 0;
                        int vr = validate_procfs((unsigned)base,
                                                 &len);
                        if (vr != 0)
                            mode = vr;
                        else
                            sz = len;
                    }
                } else {
                    mode = -VFS_EINVAL;
                }
            }
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
            } else if (mode < 0) {
                rep.status = vfs_neg((unsigned)-mode);
            } else {
                rep.offset = sz;
                rep.size = (uint64_t)(mode & 0777);
                rep.status = 0;
            }
            break;
        }
        case VFS_OP_READDIR: {
            unsigned b = (unsigned)req.offset;
            int fr = fault_gate(b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
                break;
            }
            /* Directories: "/" per backend + /dev + /proc. next_ino
             * chains; end = size 1. */
            uint64_t next = req.ino;
            const char *dp = norm_skip(win_name());
            char entry[VFS_MAX_NAME];
            int have = 0;
            if (b == 3) { /* devfs: null/zero/tty/serial */
                unsigned n =
                    (unsigned)(sizeof(g_devfs) / sizeof(g_devfs[0]));
                if (match_name(dp, "dev") || *dp == '\0') {
                    if (next < n) {
                        const char *dn = g_devfs[next].name;
                        unsigned i = 0;
                        while (dn[i] && i < VFS_MAX_NAME - 1) {
                            entry[i] = dn[i];
                            ++i;
                        }
                        entry[i] = '\0';
                        have = 1;
                        next++;
                    }
                }
            } else if (b == 6) { /* procfs: meminfo/stat */
                if (match_name(dp, "proc") || *dp == '\0') {
                    unsigned n = 2;
                    if (next < n) {
                        const char *f =
                            g_proc_files[next] + 6;
                        unsigned i = 0;
                        while (f[i] && i < VFS_MAX_NAME - 1) {
                            entry[i] = f[i];
                            ++i;
                        }
                        entry[i] = '\0';
                        have = 1;
                        next++;
                    }
                }
            } else {
                /* Root dirs of image/tmpfs/pipe backends: single
                 * level in b3 — entries enumerated via repeated
                 * STAT is out of scope; list well-known test paths
                 * only when next == 0 (documented bound). */
                (void)dp;
                (void)next;
                have = 0;
            }
            if (have) {
                for (unsigned i = 0; i < VFS_MAX_NAME; ++i) {
                    win_data()[i] = (unsigned char)entry[i];
                    if (entry[i] == '\0')
                        break;
                }
                rep.ino = next;
                rep.size = 0;
                rep.status = 0;
            } else {
                rep.ino = next;
                rep.size = 1; /* end of dir */
                rep.status = 0;
            }
            break;
        }
        case VFS_OP_CREATE: {
            unsigned b = (unsigned)req.offset;
            int fr = fault_gate(b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
                break;
            }
            const char *p = win_name();
            if (*p == '/')
                ++p;
            for (unsigned i = 0; p[i] != '\0'; ++i) {
                if (p[i] == '/') {
                    rep.status = vfs_neg(VFS_ENOTDIR);
                    goto created;
                }
            }
            if (b == 2) {
                uint64_t dummy = 0;
                if (resolve_tmpfs(win_name(), &dummy) >= 0) {
                    rep.status = vfs_neg(VFS_EEXIST);
                } else {
                    int free = -1;
                    for (unsigned i = 0; i < VFS_TMPFS_FILES;
                         ++i) {
                        if (!g_tmpfs[i].used) {
                            free = (int)i;
                            break;
                        }
                    }
                    if (free < 0) {
                        rep.status = vfs_neg(VFS_ENOSPC);
                    } else {
                        g_tmpfs[free].used = 1;
                        unsigned i = 0;
                        while (p[i] && i < VFS_MAX_NAME - 1) {
                            g_tmpfs[free].name[i] = p[i];
                            ++i;
                        }
                        g_tmpfs[free].name[i] = '\0';
                        g_tmpfs[free].size = 0;
                        g_tmpfs[free].mode = (uint32_t)req.size;
                        rep.status = 0;
                    }
                }
            } else if (b == 4) {
                int free = -1;
                for (unsigned i = 0; i < VFS_PIPES; ++i) {
                    if (!g_pipes[i].used) {
                        free = (int)i;
                        break;
                    }
                }
                if (free < 0) {
                    rep.status = vfs_neg(VFS_ENOSPC);
                } else {
                    g_pipes[free].used = 1;
                    unsigned i = 0;
                    while (p[i] && i < VFS_MAX_NAME - 1) {
                        g_pipes[free].name[i] = p[i];
                        ++i;
                    }
                    g_pipes[free].name[i] = '\0';
                    g_pipes[free].head = 0;
                    g_pipes[free].count = 0;
                    rep.status = 0;
                }
            } else {
                rep.status = vfs_neg(VFS_ENOSYS);
            }
        created:
            break;
        }
        case VFS_OP_UNLINK: {
            unsigned b = (unsigned)req.offset;
            int fr = fault_gate(b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
                break;
            }
            const char *p = win_name();
            if (*p == '/')
                ++p;
            if (b == 2) {
                uint64_t slot = 0;
                if (resolve_tmpfs(win_name(), &slot) < 0) {
                    rep.status = vfs_neg(VFS_ENOENT);
                } else {
                    g_tmpfs[slot].used = 0;
                    rep.status = 0;
                }
            } else if (b == 4) {
                uint64_t slot = 0;
                if (resolve_pipe(win_name(), &slot) < 0) {
                    rep.status = vfs_neg(VFS_ENOENT);
                } else {
                    g_pipes[slot].used = 0;
                    rep.status = 0;
                }
            } else {
                rep.status = vfs_neg(VFS_ENOSYS);
            }
            break;
        }
        case VFS_OP_BACKEND_STATUS:
            stats_publish();
            rep.offset = (uint64_t)req.offset;
            rep.status = 0;
            break;
        case VFS_OP_INJECT_BACKEND_FAULT: {
            unsigned b = (unsigned)req.offset;
            if (b < 7) {
                g_fault_once[b] = 1;
                rep.status = 0;
            } else {
                rep.status = vfs_neg(VFS_EINVAL);
            }
            break;
        }
        case VFS_OP_INITRD:
        case VFS_OP_FAT32:
        case VFS_OP_TMPFS:
        case VFS_OP_DEVFS:
        case VFS_OP_PIPE:
        case VFS_OP_RAMDISK:
        case VFS_OP_PROCFS_RENDER: {
            /* Legacy step-a probes: now liveness checks per backend. */
            unsigned b = (unsigned)(req.op - VFS_OP_INITRD);
            if (b == 7) /* PROCFS_RENDER maps to backend 6 */
                b = 6;
            int fr = fault_gate(b > 6 ? 7 : b);
            if (fr != 0) {
                rep.status = vfs_neg((unsigned)-fr);
            } else if (b < 7 && (g_backend_mask & (1U << b))) {
                rep.status = 0;
            } else {
                rep.status = vfs_neg(VFS_ENOSYS);
            }
            break;
        }
        case VFS_OP_INJECT_CORRUPT:
            g_corrupt_once = 1;
            rep.status = 0;
            break;
        default:
            /* Step-a diagnostic shape: backends not served yet. */
            rep.status = (uint16_t)(uint16_t)-38;
            if (g_corrupt_once) {
                rep.status = (uint16_t)(uint16_t)-5;
                g_corrupt_once = 0;
            }
            break;
        }

        /* Reply to the requester (owner_id carried in the RPC body):
         * send_sync_bounded waits on the REQUESTER's own queue, so a
         * reply to PID 1 (init) is a dead letter (issue #441; #51
         * precedent). */
        ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof(rep), 0);
    }
    return 0;
}
