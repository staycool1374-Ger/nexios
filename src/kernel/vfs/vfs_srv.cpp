#include <kernel/vfs/vfs_srv.hpp>

#if CONFIG_VFS_SRV

/*
 * NexIOS RTOS — VFS server proxy (issue #47).
 * Whole TU is `#if CONFIG_VFS_SRV`-guarded: the OFF build (`0`, shipped
 * default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#47 tree. Authorize stays kernel-side: this
 * proxy is diagnostic/parity only until the flip.
 *
 * Step b3 split of duties: the kernel stages RAW backend bytes (cpio
 * image slices, FAT sectors, ramdisk blocks) into the shared window;
 * the PD parses and owns namespace, handles and policy. POSIX routing
 * stays kernel-side (no sys_* flip in b3).
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/boot_sdf.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/vfs/fat32.hpp>
#include <kernel/vfs/ramdisk_fs.hpp>
#include <kernel/ramdisk/ramdiskd.hpp>

extern "C" uint8_t _binary_initrd_cpio_start[];
extern "C" uint8_t _binary_initrd_cpio_end[];

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace vfs_srv {
namespace {

uint64_t g_vfs_srv_pid = 0;
uint32_t g_vfs_srv_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

/// @brief Bounded wait budget for one server RPC (coarse ticks).
/// Fail-closed `-EAGAIN` past this; never parks a kernel task forever.
constexpr uint64_t kRpcGuardTicks = 5000;

} // namespace

void set_vfs_srv_pid(uint64_t pid) {
    g_vfs_srv_pid = pid;
}

uint64_t get_vfs_srv_pid() {
    return __atomic_load_n(&g_vfs_srv_pid, __ATOMIC_RELAXED);
}

bool vfs_srv_alive() {
    const uint64_t pid = get_vfs_srv_pid();
    if (pid == 0) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    return srv->state != TaskState::TERMINATED &&
           srv->state != TaskState::REAPED;
}

uint32_t vfs_srv_epoch() {
    return __atomic_load_n(&g_vfs_srv_epoch, __ATOMIC_RELAXED);
}

void vfs_srv_bump_epoch() {
    __atomic_fetch_add(&g_vfs_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void vfs_srv_on_restart(uint64_t pid) {
    set_vfs_srv_pid(pid);
    vfs_srv_bump_epoch();
}

void vfs_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
    // Lock-free ring write (teardown context: no locks, no IPC).
    uint64_t count =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_RELAXED);
    if (count >= 16) {
        return; // bounded residual drop (documented, ≤ table size)
    }
    g_tombstones[count].owner_id = owner_id;
    g_tombstones[count].owner_gen = owner_gen;
    __atomic_store_n(&g_tombstone_count, count + 1, __ATOMIC_RELAXED);
}

bool vfs_srv_rpc(const VfsRpc &req, VfsRpc &reply) {
    const uint64_t pid = get_vfs_srv_pid();
    if (pid == 0 || !vfs_srv_alive()) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    Message msg{};
    msg.type = static_cast<uint64_t>(req.op);
    msg.priority = cur->priority;
    __builtin_memcpy(msg.data, &req, sizeof(req));
    msg.data_size = sizeof(req);
    Message raw{};
    if (!IPC::send_sync_bounded(pid, msg, raw, kRpcGuardTicks)) {
        return false;
    }
    if (raw.data_size != sizeof(reply)) {
        return false;
    }
    __builtin_memcpy(&reply, raw.data, sizeof(reply));
    // Reply identity: server, epoch, owner (id+gen). A mismatch is
    // dropped by the caller (bounded re-wait), never consumed.
    if (raw.sender_id != pid || reply.epoch != vfs_srv_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

int64_t vfs_srv_fault_corrupt() {
    VfsRpc req{};
    req.op = static_cast<uint8_t>(VfsSrvOp::INJECT_CORRUPT);
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return -38; // -ENOSYS: no task context, fail closed
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = vfs_srv_epoch();
    VfsRpc reply{};
    if (!vfs_srv_rpc(req, reply)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return reply.status;
}

namespace {

// Bound cache (issue #47 step-b3, block_srv_bind precedent): cached at
// bind, valid only when g_bound_epoch == vfs_srv_epoch(). Every field is
// atomic-only (SMP tasks may bind concurrently; values are idempotent
// but plain/atomic mixing is UB — step-b1 audit S2).
uint32_t g_bound_epoch = 0;
uint8_t g_backend_mask = 0;
uint64_t g_data_phys = 0;
uint64_t g_data_pages = 0;
// Single in-flight RPC claim: fail-fast (-EAGAIN), never spun on, so
// there is no lock-order edge and no spin-across-reschedule (§11.1).
uint8_t g_claim = 0;

void clear_bound() {
    __atomic_store_n(&g_bound_epoch, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_backend_mask, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_phys, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_pages, 0ULL, __ATOMIC_RELAXED);
}

void fill_identity(VfsRpc &req) {
    auto *cur = Scheduler::current_task();
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = vfs_srv_epoch();
}

// Encode one row cap for conveyance, cross-checked against the SDF
// record (slot, gen, phys/size). Fail-closed: any mismatch returns 0.
uint64_t encode_row_handle(TaskControlBlock &srv, uint64_t idx,
                           const sdf::CapSpec &spec) {
    int slot = -1;
    uint32_t gen = 0;
    if (!sdf::sdf_role_cap(sdf::PdRole::VFS_SRV, idx, slot, gen)) {
        return 0;
    }
    auto *space = srv.get_cspace();
    if (space == nullptr) {
        return 0;
    }
    const uint64_t handle =
        cap::encode_user_handle(space->cspace_id, slot, gen);
    if (handle == static_cast<uint64_t>(-1)) {
        return 0;
    }
    if (spec.kind == sdf::CapKind::DMA_REGION) {
        uint64_t phys = 0;
        uint64_t pages = 0;
        if (!sdf::sdf_role_frame(sdf::PdRole::VFS_SRV, idx, phys,
                                 pages) ||
            phys == 0 || pages == 0) {
            return 0;
        }
    }
    return handle;
}

// Window accessors. The claim must be held; copies are plain memcpy
// pairs fenced with acquire/release (UP-only; no spinlock and no
// IrqGuard is taken — nothing here may sleep, and the claim is never
// held across a blocking RPC, so no guard would add safety).
uint8_t *window_base() {
    const uint64_t phys =
        __atomic_load_n(&g_data_phys, __ATOMIC_RELAXED);
    if (phys == 0) {
        return nullptr;
    }
    return reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
}

// Stage a NUL-terminated path into the name slot (kernel → PD).
// Rejects over-long names before touching the window.
bool stage_name(const char *path) {
    if (path == nullptr) {
        return false;
    }
    uint64_t len = 0;
    while (len < kVfsMaxName && path[len] != '\0') {
        ++len;
    }
    if (len >= kVfsMaxName) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr) {
        return false;
    }
    __builtin_memcpy(win + kVfsNameOffset, path, len + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Copy caller bytes into the data region (kernel → PD, WRITE path).
bool stage_bytes_in(const uint8_t *in, uint64_t size) {
    if (size > kVfsDataSize) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr || (size != 0 && in == nullptr)) {
        return false;
    }
    if (size != 0) {
        __builtin_memcpy(win + kVfsDataOffset, in, size);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Copy staged bytes out of the data region (PD → kernel, READ path).
bool fetch_bytes_out(uint8_t *out, uint64_t size) {
    if (size > kVfsDataSize) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr || (size != 0 && out == nullptr)) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (size != 0) {
        __builtin_memcpy(out, win + kVfsDataOffset, size);
    }
    return true;
}

// Fetch the PD-written dirent name from the data region (READDIR path).
bool fetch_name_out(char *name_out) {
    auto *win = window_base();
    if (win == nullptr || name_out == nullptr) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t len = 0;
    while (len < kVfsMaxName - 1 && win[kVfsDataOffset + len] != '\0') {
        ++len;
    }
    if (win[kVfsDataOffset + len] != '\0') {
        return false;
    }
    __builtin_memcpy(name_out, win + kVfsDataOffset, len + 1);
    return true;
}

// Raw-byte staging (step b3 split of duties): copy backend image bytes
// into the data region. Initrd slices come from the linked image;
// FAT32 slices are raw device sectors past the partition base.
bool stage_initrd(uint64_t img_off, uint64_t size) {
    const uint64_t img_size = static_cast<uint64_t>(
        _binary_initrd_cpio_end - _binary_initrd_cpio_start);
    if (size > kVfsDataSize || img_off > img_size ||
        size > img_size - img_off) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr) {
        return false;
    }
    __builtin_memcpy(win + kVfsDataOffset,
                     _binary_initrd_cpio_start + img_off, size);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Single RPC with the claim held. Thin wrapper: identity envelope is
// filled by the caller, reply validated by vfs_srv_rpc.
bool rpc_once(VfsRpc &req, VfsRpc &reply) {
    fill_identity(req);
    return vfs_srv_rpc(req, reply);
}

} // namespace

bool vfs_srv_bind() {
    const uint64_t pid = get_vfs_srv_pid();
    if (pid == 0 || !vfs_srv_alive()) {
        return false;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr) {
        return false;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    sdf::CapSpec specs[sdf::kMaxCapsPerRole] = {};
    const uint64_t count = sdf::sdf_row(sdf::PdRole::VFS_SRV, specs);
    if (count > sdf::kMaxCapsPerRole) {
        return false;
    }
    // CAP_GET rounds convey one cap each (index order): the PD maps and
    // echoes. BIND goes last so the PD probes with every cap present.
    // Restore tolerance: a snapshot rewind can stale the composed slots
    // (encode fails) while the PD survives. Retry once via a fresh
    // re-compose (re-mints the window + slots), then re-convey.
    for (uint64_t attempt = 0; ; ++attempt) {
        bool ok = true;
        for (uint64_t i = 0; i < count && ok; ++i) {
            const uint64_t handle = encode_row_handle(*srv, i, specs[i]);
            if (handle == 0) {
                ok = false;
                break;
            }
            uint64_t cap_phys = specs[i].addr;
            uint64_t cap_size = specs[i].size;
            if (specs[i].kind == sdf::CapKind::DMA_REGION) {
                // sdf_role_frame returns (phys, pages); the size gate
                // is in bytes (block_srv_bind precedent).
                if (!sdf::sdf_role_frame(sdf::PdRole::VFS_SRV, i,
                                         cap_phys, cap_size) ||
                    cap_phys == 0) {
                    return false;
                }
                cap_size *= arch::PAGE_SIZE;
                if (cap_size < kVfsRegionPages * arch::PAGE_SIZE) {
                    return false;
                }
            }
            VfsRpc get{};
            get.op = static_cast<uint8_t>(VfsSrvOp::CAP_GET);
            get.owner_id = static_cast<uint32_t>(cur->id);
            get.owner_gen = cur->generation;
            get.epoch = vfs_srv_epoch();
            get.ino = cap_phys;
            get.offset = cap_size;
            // 48 B budget: kind rides flags, the encoded row handle
            // rides size (the PD needs the handle for SYS_FRAME_MAP;
            // phys/size alone do not resolve a CSpace slot).
            get.size = handle;
            get.flags = static_cast<uint8_t>(specs[i].kind);
            get.status_op = static_cast<uint16_t>(i);
            VfsRpc got{};
            if (!vfs_srv_rpc(get, got)) {
                clear_bound();
                return false;
            }
            if (got.op != get.op || got.epoch != get.epoch ||
                got.owner_id != get.owner_id ||
                got.owner_gen != get.owner_gen ||
                got.ino != get.ino || got.offset != get.offset ||
                got.size != get.size || got.flags != get.flags ||
                got.status_op != get.status_op || got.status != 0) {
                clear_bound();
                return false;
            }
        }
        if (ok) {
            break;
        }
        // Stale compose: re-mint once, then re-convey. A second stale
        // round is a real defect, not restore skew — fail closed.
        if (attempt > 0 || !sdf::sdf_rebind(*srv, sdf::PdRole::VFS_SRV)) {
            clear_bound();
            return false;
        }
    }
    VfsRpc req{};
    req.op = static_cast<uint8_t>(VfsSrvOp::BIND);
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = vfs_srv_epoch();
    req.offset = count;
    VfsRpc reply{};
    if (!vfs_srv_rpc(req, reply)) {
        return false;
    }
    if (reply.op != req.op || reply.epoch != req.epoch ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    if (reply.status != 0 || reply.offset != count) {
        return false;
    }
    // Resolve the shared data window (exactly one DMA_REGION per row).
    uint64_t data_phys = 0;
    uint64_t data_pages = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (specs[i].kind == sdf::CapKind::DMA_REGION) {
            if (data_phys != 0) {
                return false; // two windows: row malformed
            }
            if (!sdf::sdf_role_frame(sdf::PdRole::VFS_SRV, i, data_phys,
                                     data_pages)) {
                return false;
            }
        }
    }
    if (data_phys == 0) {
        return false;
    }
    // Live-topology mask (step b3 degraded path): the PD reports what
    // it implements; the kernel clears backends with no live backing
    // (FAT32 needs a mounted partition, RAMDISK a live daemon).
    uint8_t live = static_cast<uint8_t>(reply.size & 0x7FU);
    if (kernel::gs::get_fat32_partition() == nullptr) {
        live &= static_cast<uint8_t>(
            ~(1U << static_cast<uint8_t>(VfsBackend::FAT32)));
    }
    if (kernel::ramdiskd::get_ramdiskd_pid() == 0) {
        live &= static_cast<uint8_t>(
            ~(1U << static_cast<uint8_t>(VfsBackend::RAMDISK)));
    }
    __atomic_store_n(&g_data_phys, data_phys, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_pages, data_pages, __ATOMIC_RELAXED);
    __atomic_store_n(&g_backend_mask, live, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bound_epoch, vfs_srv_epoch(), __ATOMIC_RELAXED);
    return true;
}

uint8_t vfs_srv_backend_mask() {
    if (__atomic_load_n(&g_bound_epoch, __ATOMIC_RELAXED) !=
        vfs_srv_epoch()) {
        return 0;
    }
    return __atomic_load_n(&g_backend_mask, __ATOMIC_RELAXED);
}

bool vfs_srv_backend_available(VfsBackend backend) {
    const uint8_t bit = static_cast<uint8_t>(1U << static_cast<uint8_t>(backend));
    return (vfs_srv_backend_mask() & bit) != 0;
}

// Claim guard: try-claim, fail-fast -EAGAIN, release on every exit.
namespace {

bool claim_or_busy(int64_t &busy) {
    if (__atomic_test_and_set(&g_claim, __ATOMIC_ACQUIRE) != 0) {
        busy = -11; // -EAGAIN: one in-flight RPC, never spun on
        return false;
    }
    busy = 0;
    return true;
}

bool bound_or_rebind() {
    if (__atomic_load_n(&g_bound_epoch, __ATOMIC_RELAXED) ==
        vfs_srv_epoch()) {
        return true;
    }
    clear_bound();
    return vfs_srv_bind();
}

} // namespace

// Directory-region staging (step b3): the kernel ships the metadata
// bytes the PD parses. All spans are window-bounded and fail-closed.

// Compact cpio index record (PD parses this, never the raw image).
struct CpioIndexEntry {
    char name[48];
    uint32_t data_off;
    uint32_t size;
    uint32_t mode;
};

// Cached cpio index (step b3 performance): walking the ~2.3 MiB image
// per op costs seconds under TCG and trips the 5000-tick RPC bound.
// Build once per epoch into .bss (not a pooled resource — no tracker
// entry; snapshot rewind simply triggers a lazy rebuild).
namespace {
constexpr uint64_t kCpioIndexCap = 128;
CpioIndexEntry g_cpio_index[kCpioIndexCap] = {};
uint64_t g_cpio_index_count = 0;
uint32_t g_cpio_index_epoch = 0;
bool g_cpio_index_truncated = false;
} // namespace

// Build the compact index of the linked initrd image into the .bss
// cache (one walk per epoch; see g_cpio_index_* above). Returns the
// entry count, or -1 when the image is malformed or the table
// overflows (fail-closed, PD sees no index).
int64_t build_cpio_cache() {
    const uint8_t *img = _binary_initrd_cpio_start;
    const uint64_t img_size = static_cast<uint64_t>(
        _binary_initrd_cpio_end - _binary_initrd_cpio_start);
    constexpr uint64_t kMaxEntries = 128;
    constexpr uint64_t kHdrLen = 110; // "070701" + 13x8 hex
    auto *table = g_cpio_index;
    const uint64_t cap = kMaxEntries;
    uint64_t off = 0;
    uint64_t count = 0;
    while (true) {
        if (off + kHdrLen > img_size) {
            return -1;
        }
        // Freestanding-safe magic compare (no memcmp libcall on rv64).
        if (img[off + 0] != '0' || img[off + 1] != '7' ||
            img[off + 2] != '0' || img[off + 3] != '7' ||
            img[off + 4] != '0' || img[off + 5] != '1') {
            return -1;
        }
        // Parse the 13 hex fields (namesize @ field 11, filesize @ 6,
        // mode @ 1). Bounded: fixed offsets, no scanning.
        auto hex8 = [&](uint64_t at, uint32_t &out) {
            uint32_t v = 0;
            for (uint64_t i = 0; i < 8; ++i) {
                const uint8_t c = img[off + at + i];
                uint32_t d = 0;
                if (c >= '0' && c <= '9') {
                    d = static_cast<uint32_t>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    d = static_cast<uint32_t>(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    d = static_cast<uint32_t>(c - 'A' + 10);
                } else {
                    return false;
                }
                v = (v << 4) | d;
            }
            out = v;
            return true;
        };
        uint32_t mode = 0;
        uint32_t filesize = 0;
        uint32_t namesize = 0;
        if (!hex8(6 + 8, mode) || !hex8(6 + 6 * 8, filesize) ||
            !hex8(6 + 11 * 8, namesize)) {
            return -1;
        }
        const uint64_t name_off = off + kHdrLen;
        if (namesize == 0 || namesize > 64 ||
            name_off + namesize > img_size) {
            return -1;
        }
        const uint64_t data_off =
            (name_off + namesize + 3) & ~static_cast<uint64_t>(3);
        if (data_off > img_size || filesize > img_size - data_off) {
            return -1;
        }
        char name[64] = {};
        __builtin_memcpy(name, img + name_off,
                         namesize < 64 ? namesize : 64);
        // Freestanding-safe trailer compare (no memcmp libcall).
        static const char kTrailer[10] = {'T', 'R', 'A', 'I', 'L', 'E',
                                          'R', '!', '!', '!'};
        bool is_trailer = true;
        for (uint64_t i = 0; i < 10; ++i) {
            if (name[i] != kTrailer[i]) {
                is_trailer = false;
                break;
            }
        }
        if (is_trailer) {
            break;
        }
        if (count >= cap) {
            return -1;
        }
        __builtin_memset(table[count].name, 0,
                         sizeof(table[count].name));
        uint64_t nl = 0;
        while (nl + 1 < sizeof(table[count].name) && name[nl] != '\0') {
            table[count].name[nl] = name[nl];
            ++nl;
        }
        table[count].data_off = static_cast<uint32_t>(data_off);
        table[count].size = filesize;
        table[count].mode = mode;
        ++count;
        off = (data_off + filesize + 3) & ~static_cast<uint64_t>(3);
        if (off >= img_size) {
            return -1;
        }
    }
    g_cpio_index_count = count;
    g_cpio_index_truncated = false;
    g_cpio_index_epoch = vfs_srv_epoch();
    return static_cast<int64_t>(count);
}

// Stage the cached cpio index into the data region (fast memcpy; the
// walk happens once per epoch in build_cpio_cache).
bool stage_cpio_index() {
    if (g_cpio_index_epoch != vfs_srv_epoch() || g_cpio_index_truncated) {
        g_cpio_index_count = 0;
        g_cpio_index_truncated = false;
        if (build_cpio_cache() < 0) {
            g_cpio_index_truncated = true;
            return false;
        }
    }
    auto *win = window_base();
    if (win == nullptr || g_cpio_index_count == 0) {
        return false;
    }
    const uint64_t bytes =
        g_cpio_index_count * sizeof(CpioIndexEntry);
    if (bytes > kVfsDataSize) {
        return false;
    }
    __builtin_memcpy(win + kVfsDataOffset, g_cpio_index, bytes);
    // Zero the tail so the PD's empty-name scan terminates even if a
    // previous op left a longer table staged.
    __builtin_memset(win + kVfsDataOffset + bytes, 0,
                     kVfsDataSize - bytes);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Stage BPB + first root-dir cluster for FAT32 (PD parses both).
bool stage_fat32_dir() {
    constexpr uint64_t kSector = 512;
    auto *part = kernel::gs::get_fat32_partition();
    if (part == nullptr) {
        return false;
    }
    if (!part->bpb().valid) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr) {
        return false;
    }
    const uint64_t base = part->partition_lba();
    if (!part->device().read_sector(base, win + kVfsDataOffset)) {
        return false;
    }
    const uint64_t cluster_lba =
        part->cluster_to_lba(part->bpb().root_cluster);
    const uint64_t cluster_bytes =
        static_cast<uint64_t>(part->bpb().sectors_per_cluster) * kSector;
    if (kSector + cluster_bytes > kVfsDataSize) {
        return false;
    }
    const uint64_t nsec = cluster_bytes / kSector;
    for (uint64_t i = 0; i < nsec; ++i) {
        if (!part->device().read_sector(
                cluster_lba + i,
                win + kVfsDataOffset + kSector + i * kSector)) {
            return false;
        }
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Stage one FAT32 cluster chain as contiguous bytes (mechanical walk;
// the PD validates the staged length against its parsed dirent).
bool stage_fat32_chain(uint32_t start_cluster, uint64_t file_size,
                        uint64_t skip, uint64_t want) {
    constexpr uint64_t kSector = 512;
    auto *part = kernel::gs::get_fat32_partition();
    if (part == nullptr || !part->bpb().valid || want > kVfsDataSize ||
        skip > file_size || want > file_size - skip) {
        return false;
    }
    auto *win = window_base();
    if (win == nullptr) {
        return false;
    }
    const uint64_t cluster_bytes =
        static_cast<uint64_t>(part->bpb().sectors_per_cluster) * kSector;
    uint32_t cluster = start_cluster;
    uint64_t file_off = 0;
    uint64_t staged = 0;
    uint8_t *dst = win + kVfsDataOffset;
    uint64_t trips = 0;
    while (staged < want) {
        if (++trips > 64 || kernel::fat32::Fat32Partition::is_eof(cluster)) {
            return false;
        }
        const uint64_t cluster_lba = part->cluster_to_lba(cluster);
        const uint64_t nsec = cluster_bytes / kSector;
        for (uint64_t i = 0; i < nsec && staged < want; ++i) {
            uint8_t sec[kSector];
            if (!part->device().read_sector(cluster_lba + i, sec)) {
                return false;
            }
            for (uint64_t b = 0; b < kSector && staged < want; ++b) {
                if (file_off >= skip) {
                    dst[staged++] = sec[b];
                }
                ++file_off;
                if (file_off >= file_size) {
                    break;
                }
            }
        }
        if (file_off >= file_size) {
            break;
        }
        uint32_t next = 0;
        if (!part->read_fat_entry(cluster, next)) {
            return false;
        }
        cluster = next;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return staged == want;
}

// Stage a ramdisk byte span contiguously at the data region head,
// chunked so no single block read overruns the window (alignment
// overhang can push span+pad past 24 blocks for a 12 KiB read).
bool stage_ramdisk_span(uint64_t start_block, uint64_t file_size,
                        uint64_t offset, uint64_t size) {
    auto *win = window_base();
    if (win == nullptr || offset > file_size ||
        size > file_size - offset || size > kVfsDataSize) {
        return false;
    }
    uint64_t done = 0;
    uint64_t trips = 0;
    uint8_t sec[512];
    while (done < size) {
        if (++trips > 32) {
            return false; // bounded: 12 KiB needs ≤ 25 blocks
        }
        const uint64_t cur = offset + done;
        const uint64_t blk = start_block + cur / 512;
        const uint64_t blk_off = cur % 512;
        if (!kernel::vfs::ramdisk_fs_read_blocks(blk, 1, sec)) {
            return false;
        }
        uint64_t take = 512 - blk_off;
        if (take > size - done) {
            take = size - done;
        }
        __builtin_memcpy(win + kVfsDataOffset + done, sec + blk_off,
                         take);
        done += take;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return done == size;
}
bool stage_ramdisk_dir() {
    auto *win = window_base();
    if (win == nullptr) {
        return false;
    }
    // Superblock (block 0) + full dir region (blocks 1..10); the PD
    // parses entries. 11 x 512 B = 5632 B, fits the data region.
    for (uint64_t b = 0; b <= 10; ++b) {
        if (!kernel::vfs::ramdisk_fs_read_blocks(
                b, 1, win + kVfsDataOffset + b * 512)) {
            return false;
        }
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

bool stage_procfs_path(const char *path) {
    auto *win = window_base();
    if (win == nullptr || path == nullptr || path[0] == '\0') {
        return false;
    }
    kernel::vfs::Vnode *vn = kernel::vfs::resolve(path);
    if (vn == nullptr || vn->ops == nullptr ||
        vn->ops->read == nullptr) {
        if (vn != nullptr) {
            kernel::vfs::release(vn);
        }
        return false;
    }
    const int64_t got =
        vn->ops->read(*vn, win + kVfsDataOffset, kVfsDataSize, 0);
    kernel::vfs::release(vn);
    if (got < 0 || static_cast<uint64_t>(got) > kVfsDataSize) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return true;
}

// Stage a /proc file render via the in-kernel procfs (D6 validator:
// the kernel renders through its own VnodeOps; the PD validates format
// + digest, never renders). Two entry points sharing one helper:
// - OPEN/STAT/READDIR: path comes from the name slot (staged under the
//   same claim, so no cross-op race).
// - READ: the locator base carries the shared-table index
//   (VFS_PROC_* — must match userspace/vfs-srv.c), no name slot use.
// fd-table untouched: resolve + ops->read directly, release after.
bool stage_procfs(uint64_t idx) {
    static const char *const kProcFiles[] = {"/proc/meminfo",
                                             "/proc/stat"};
    if (idx >= 2) {
        return false;
    }
    return stage_procfs_path(kProcFiles[idx]);
}

// Stage the directory region for an OPEN/STAT/READDIR path op. Returns
// false when the backend needs no staging (PD-owned stores) — the
// caller treats that as "proceed without staging".
bool stage_dir_region(VfsBackend backend) {
    switch (backend) {
    case VfsBackend::INITRD:
        return stage_cpio_index();
    case VfsBackend::FAT32:
        return stage_fat32_dir();
    case VfsBackend::RAMDISK:
        return stage_ramdisk_dir();
    case VfsBackend::TMPFS:
    case VfsBackend::DEVFS:
    case VfsBackend::PIPE:
        return true; // PD-owned/static: nothing to stage
    case VfsBackend::PROCFS: {
        // Render by name-slot path (same claim, no race); the PD
        // matches the path against its VFS_PROC_* table for the
        // READ-time index.
        auto *win = window_base();
        if (win == nullptr) {
            return false;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        char path[kVfsMaxName];
        uint64_t len = 0;
        while (len < kVfsMaxName - 1 &&
               win[kVfsNameOffset + len] != '\0') {
            path[len] = static_cast<char>(win[kVfsNameOffset + len]);
            ++len;
        }
        path[len] = '\0';
        return len != 0 && stage_procfs_path(path);
    }
    case VfsBackend::COUNT:
        break;
    }
    return false;
}

int64_t vfs_srv_open(VfsBackend backend, const char *path, uint64_t flags,
                     uint64_t &handle) {
    handle = 0;
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_name(path) ||
            !stage_dir_region(backend)) {
            rc = -38;
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::OPEN);
        req.offset = static_cast<uint64_t>(backend);
        req.size = flags;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            rc = -38;
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
        if (rc == 0) {
            handle = reply.ino;
        }
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_close(uint64_t handle) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind()) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::CLOSE);
        req.ino = handle;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

// Locator convention (step b3): for image-backed backends the PD passes
// the raw locator back in ino/offset — initrd image byte offset in ino,
// FAT32 start cluster in ino (file size in the PD handle, re-passed via
// size hi/lo? no: size carries the read length; the PD validates the
// range against its open-handle table and fails closed on overrun).
// The kernel bounds every stage against the image/device size.
int64_t vfs_srv_read(uint64_t handle, uint64_t offset, uint8_t *out,
                     uint64_t size) {
    if (size == 0) {
        return 0; // POSIX: zero-length reads succeed with no bytes
    }
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || out == nullptr ||
            size > kVfsDataSize) {
            break;
        }
        const uint64_t locator = handle;
        // The locator kind rides the top byte (PD-assigned at OPEN).
        // Direct-handle layout (PD packs, kernel decodes): low 56 bits
        // = (locator24 << 32 | size32); the size caps reads (EOF clamp).
        // PD-owned stores (tmpfs/pipe) use table slots instead.
        const auto backend =
            static_cast<VfsBackend>((locator >> 56) & 0x7FU);
        const uint64_t base = locator & 0x00FFFFFFFFFFFFFFULL;
        // Clamp the request to the file size carried in the handle
        // (image-backed backends); PD-owned stores serve whole windows.
        uint64_t want = size;
        if (backend == VfsBackend::INITRD ||
            backend == VfsBackend::FAT32 ||
            backend == VfsBackend::RAMDISK) {
            const uint64_t file_size = base & 0xFFFFFFFFULL;
            if (offset >= file_size) {
                rc = 0; // EOF: zero-length read, no staging, no RPC
                break;
            }
            if (want > file_size - offset) {
                want = file_size - offset;
            }
        }
        if (want == 0 || want > kVfsDataSize) {
            rc = (want == 0) ? 0 : -38;
            break;
        }
        bool staged = false;
        if (backend == VfsBackend::INITRD) {
            const uint64_t data_off = (base >> 32) & 0xFFFFFFULL;
            staged = stage_initrd(data_off + offset, want);
        } else if (backend == VfsBackend::FAT32) {
            // Base packs (start_cluster << 32 | file_size); the skip
            // is the read offset. Chain walk is mechanical transport;
            // the PD validates the staged length at serve.
            const uint32_t start_cluster =
                static_cast<uint32_t>((base >> 32) & 0xFFFFFFULL);
            const uint64_t file_size = base & 0xFFFFFFFFULL;
            staged = stage_fat32_chain(start_cluster, file_size, offset,
                                       want);
        } else if (backend == VfsBackend::RAMDISK) {
            // Base packs (start_block << 32 | file_size).
            const uint64_t start_block = (base >> 32) & 0xFFFFFFULL;
            const uint64_t file_size = base & 0xFFFFFFFFULL;
            staged = stage_ramdisk_span(start_block, file_size, offset,
                                        want);
        } else if (backend == VfsBackend::PROCFS) {
            staged = stage_procfs(base);
        } else {
            // PD-owned stores serve from their own arenas (WRITE-path
            // mirror): stage nothing, let the PD fill the window.
            staged = true;
        }
        if (!staged) {
            rc = -5; // -EIO: staging failed, fail closed
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::READ);
        req.ino = locator;
        req.offset = offset;
        req.size = want;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
        if (rc < 0) {
            break;
        }
        const uint64_t got = reply.size;
        if (got > want) {
            rc = -5;
            break;
        }
        if (!fetch_bytes_out(out, got)) {
            rc = -5;
            break;
        }
        rc = static_cast<int64_t>(got);
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_write(uint64_t handle, uint64_t offset, const uint8_t *in,
                      uint64_t size) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_bytes_in(in, size)) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::WRITE);
        req.ino = handle;
        req.offset = offset;
        req.size = size;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
        if (rc == 0) {
            // POSIX write semantics: return the validated byte count.
            if (reply.size > size) {
                rc = -5; // PD echoes more than staged: fail closed
            } else {
                rc = static_cast<int64_t>(reply.size);
            }
        }
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_stat(VfsBackend backend, const char *path,
                     uint64_t &st_size, uint64_t &st_mode) {
    st_size = 0;
    st_mode = 0;
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_name(path) ||
            !stage_dir_region(backend)) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::STAT);
        req.offset = static_cast<uint64_t>(backend);
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
        if (rc == 0) {
            st_size = reply.offset;
            st_mode = reply.size;
        }
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_readdir(VfsBackend backend, const char *path,
                        uint64_t &next_ino, char *name_out) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_name(path) ||
            !stage_dir_region(backend)) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::READDIR);
        req.offset = static_cast<uint64_t>(backend);
        req.ino = next_ino;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
        if (rc == 0) {
            // 0 = entry (name in data region, next ino echoed),
            // 1 = end of dir.
            if (reply.size == 1) {
                rc = 1;
                break;
            }
            if (name_out == nullptr || !fetch_name_out(name_out)) {
                rc = -5;
                break;
            }
            next_ino = reply.ino;
        }
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_create(VfsBackend backend, const char *path,
                       uint64_t mode) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_name(path)) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::CREATE);
        req.offset = static_cast<uint64_t>(backend);
        req.size = mode;
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

int64_t vfs_srv_unlink(VfsBackend backend, const char *path) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind() || !stage_name(path)) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::UNLINK);
        req.offset = static_cast<uint64_t>(backend);
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

bool vfs_srv_backend_status(VfsBackend backend, VfsBackendStatus &out) {
    __builtin_memset(&out, 0, sizeof(out));
    if (__atomic_load_n(&g_bound_epoch, __ATOMIC_RELAXED) !=
        vfs_srv_epoch()) {
        return false;
    }
    // Publish the request first (PD fills the stats slot), then read
    // the slot with magic/version validation (block_srv_irq_stats
    // shape). Claim serializes against concurrent op windows.
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return false;
    }
    bool ok = false;
    do {
        if (!bound_or_rebind()) {
            break;
        }
        // Re-load the window phys after bind: a re-compose mints a
        // fresh window, and a stale HHDM read would only pass on
        // magic-gated luck (SIL3 audit #47 step-b3 S3).
        const uint64_t phys =
            __atomic_load_n(&g_data_phys, __ATOMIC_RELAXED);
        if (phys == 0) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::BACKEND_STATUS);
        req.offset = static_cast<uint64_t>(backend);
        VfsRpc reply{};
        if (!rpc_once(req, reply) || reply.status != 0) {
            break;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const auto *slot = reinterpret_cast<const VfsBackendStatus *>(
            arch::HHDM_OFFSET + phys + kVfsStatsOffset);
        VfsBackendStatus local{};
        __builtin_memcpy(&local, slot, sizeof(local));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (local.magic != kVfsBackendStatusMagic ||
            local.version != kVfsBackendStatusVersion) {
            break;
        }
        out = local;
        ok = true;
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return ok;
}

int64_t vfs_srv_fault_backend(VfsBackend backend) {
    int64_t busy = 0;
    if (!claim_or_busy(busy)) {
        return busy;
    }
    int64_t rc = -38;
    do {
        if (!bound_or_rebind()) {
            break;
        }
        VfsRpc req{};
        req.op = static_cast<uint8_t>(VfsSrvOp::INJECT_BACKEND_FAULT);
        req.offset = static_cast<uint64_t>(backend);
        VfsRpc reply{};
        if (!rpc_once(req, reply)) {
            break;
        }
        rc = static_cast<int64_t>(static_cast<int16_t>(reply.status));
    } while (false);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    return rc;
}

} // namespace vfs_srv
} // namespace kernel

#endif // CONFIG_VFS_SRV
