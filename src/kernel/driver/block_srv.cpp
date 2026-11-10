#include <kernel/driver/block_srv.hpp>

#if CONFIG_BLOCK_SRV

/*
 * NexIOS RTOS — Block server proxy (issue #47).
 * Whole TU is `#if CONFIG_BLOCK_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#47 tree. Kernel probing stays until flip.
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/boot_sdf.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/memory/address.hpp>

namespace kernel {
namespace block_srv {
namespace {

uint64_t g_block_srv_pid = 0;
uint32_t g_block_srv_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

/// @brief Bounded wait budget for one server RPC (coarse ticks).
/// Fail-closed `-EAGAIN` past this; never parks a kernel task forever.
constexpr uint64_t kRpcGuardTicks = 5000;

// Bound device state (issue #47 step-b1): cached at bind, valid only
// when g_bound_epoch == block_srv_epoch(). Degraded binds (no backend)
// still latch the epoch so reads fail fast instead of rebinding.
uint32_t g_bound_epoch = 0;
BlkBackend g_backend = BlkBackend::NONE;
uint64_t g_sector_count = 0;
uint64_t g_data_phys = 0;
uint64_t g_data_pages = 0;
// Single in-flight RPC claim: fail-fast (-EAGAIN), never spun on, so
// there is no lock-order edge and no spin-across-reschedule (§11.1).
uint8_t g_claim = 0;
uint64_t g_seq = 0;

} // namespace

void set_block_srv_pid(uint64_t pid) {
    g_block_srv_pid = pid;
}

uint64_t get_block_srv_pid() {
    return __atomic_load_n(&g_block_srv_pid, __ATOMIC_RELAXED);
}

bool block_srv_alive() {
    const uint64_t pid = get_block_srv_pid();
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

uint32_t block_srv_epoch() {
    return __atomic_load_n(&g_block_srv_epoch, __ATOMIC_RELAXED);
}

void block_srv_bump_epoch() {
    __atomic_fetch_add(&g_block_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void block_srv_on_restart(uint64_t pid) {
    set_block_srv_pid(pid);
    block_srv_bump_epoch();
}

void block_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
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

bool block_srv_rpc(const BlockRpc &req, BlockRpc &reply) {
    const uint64_t pid = get_block_srv_pid();
    if (pid == 0 || !block_srv_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != block_srv_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

int64_t block_srv_fault_loopback() {
    BlockRpc req{};
    req.op = static_cast<uint8_t>(BlockSrvOp::LOOPBACK);
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return -38; // -ENOSYS: no task context, fail closed
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = block_srv_epoch();
    req.arg = 0xA5A5A5A5ULL; // loopback pattern seed
    BlockRpc reply{};
    if (!block_srv_rpc(req, reply)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return static_cast<int64_t>(static_cast<int16_t>(reply.status));
}

int64_t block_srv_fault_nodomain() {
    BlockRpc req{};
    req.op = static_cast<uint8_t>(BlockSrvOp::DMA_MAP);
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return -38; // -ENOSYS: no task context, fail closed
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = block_srv_epoch();
    req.handle = 0; // zero handle: must fail closed (-EACCES shape)
    BlockRpc reply{};
    if (!block_srv_rpc(req, reply)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return static_cast<int64_t>(static_cast<int16_t>(reply.status));
}

int64_t block_srv_fault_irq_skip() {
    BlockRpc req{};
    req.op = static_cast<uint8_t>(BlockSrvOp::INJECT_IRQ_SKIP);
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return -38; // -ENOSYS: no task context, fail closed
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = block_srv_epoch();
    BlockRpc reply{};
    if (!block_srv_rpc(req, reply)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return static_cast<int64_t>(static_cast<int16_t>(reply.status));
}

bool block_srv_irq_stats(BlockIrqStats &out) {
    // SIL3 audit #47 step-b2: §11.6 — window phys is stored atomically.
    const uint64_t window_phys =
        __atomic_load_n(&g_data_phys, __ATOMIC_RELAXED);
    if (window_phys == 0) {
        return false;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const auto *stats = reinterpret_cast<const BlockIrqStats *>(
        arch::HHDM_OFFSET + window_phys + kBlockIrqStatsOffset);
    BlockIrqStats local{};
    __builtin_memcpy(&local, stats, sizeof(local));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (local.magic != kBlockIrqStatsMagic ||
        local.version != kBlockIrqStatsVersion) {
        return false;
    }
    out = local;
    return true;
}

namespace {

// Fill the identity envelope shared by every RPC.
void fill_identity(BlockRpc &req) {
    auto *cur = Scheduler::current_task();
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = block_srv_epoch();
}

// Encode one row cap for conveyance, cross-checked against the SDF
// record (slot, gen, phys/size). Fail-closed: any mismatch returns 0.
uint64_t encode_row_handle(TaskControlBlock &srv, uint64_t idx,
                           const sdf::CapSpec &spec) {
    int slot = -1;
    uint32_t gen = 0;
    if (!sdf::sdf_role_cap(sdf::PdRole::BLOCK_SRV, idx, slot, gen)) {
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
        if (!sdf::sdf_role_frame(sdf::PdRole::BLOCK_SRV, idx, phys,
                                 pages) ||
            phys == 0 || pages == 0) {
            return 0;
        }
    }
    return handle;
}

void clear_bound() {
    // SIL3 audit #47 step-b1: §11.6 all-or-nothing atomics. The bound
    // cache is read atomically by the accessors, so every store here
    // is atomic (SMP tasks may bind concurrently; values are
    // idempotent but plain/atomic mixing is UB).
    __atomic_store_n(&g_bound_epoch, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(reinterpret_cast<uint8_t *>(&g_backend),
                     static_cast<uint8_t>(BlkBackend::NONE),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_sector_count, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_phys, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_pages, 0ULL, __ATOMIC_RELAXED);
}

} // namespace

bool block_srv_bind() {
    const uint64_t pid = get_block_srv_pid();
    if (pid == 0 || !block_srv_alive()) {
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
    const uint64_t count = sdf::sdf_row(sdf::PdRole::BLOCK_SRV, specs);
    if (count > sdf::kMaxCapsPerRole) {
        return false;
    }
    // CAP_GET rounds convey one cap each (index order): the PD maps and
    // echoes. BIND goes last so the PD probes with every cap present.
    // DMA_REGION rows carry addr=0 by design (fresh frames per compose);
    // the real window phys/pages come from the SDF frame record.
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t handle = encode_row_handle(*srv, i, specs[i]);
        if (handle == 0) {
            return false;
        }
        uint64_t cap_phys = specs[i].addr;
        uint64_t cap_size = specs[i].size;
        if (specs[i].kind == sdf::CapKind::DMA_REGION) {
            if (!sdf::sdf_role_frame(sdf::PdRole::BLOCK_SRV, i, cap_phys,
                                     cap_size) ||
                cap_phys == 0 || cap_size < kBlockRegionPages) {
                return false;
            }
            cap_size *= arch::PAGE_SIZE;
        }
        BlockRpc get{};
        get.op = static_cast<uint8_t>(BlockSrvOp::CAP_GET);
        fill_identity(get);
        get.flags = static_cast<uint8_t>(count);
        get.handle = static_cast<uint32_t>(i);
        get.arg = handle;
        get.lba = cap_phys;
        get.aux = cap_size;
        get.rsvd = static_cast<uint16_t>(specs[i].io_port);
        get.status_op = static_cast<uint16_t>(specs[i].kind);
        BlockRpc got{};
        if (!block_srv_rpc(get, got)) {
            clear_bound();
            return false;
        }
        if (got.op != get.op || got.epoch != get.epoch ||
            got.owner_id != get.owner_id ||
            got.owner_gen != get.owner_gen || got.handle != get.handle ||
            got.arg != get.arg ||
            got.status_op != static_cast<uint16_t>(specs[i].kind) ||
            got.status != 0) {
            clear_bound();
            return false;
        }
    }
    // BIND carries no cap (flags = row cap count); the PD probes the
    // mapped caps and answers backend + geometry.
    BlockRpc req{};
    req.op = static_cast<uint8_t>(BlockSrvOp::BIND);
    fill_identity(req);
    req.flags = static_cast<uint8_t>(count);
    BlockRpc reply{};
    if (!block_srv_rpc(req, reply)) {
        return false;
    }
    if (reply.op != req.op || reply.epoch != req.epoch ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    if (reply.status != 0) {
        return false;
    }
    const auto backend = static_cast<BlkBackend>(reply.status_op);
    if (backend != BlkBackend::NONE && backend != BlkBackend::ATA_PIO &&
        backend != BlkBackend::VIRTIO_MMIO) {
        return false;
    }
    if (reply.flags != count) {
        return false;
    }
    const uint64_t sectors = reply.aux;
    const uint64_t sector_size = reply.arg;
    if (backend != BlkBackend::NONE &&
        (sectors == 0 || sector_size != kBlockSectorSize)) {
        return false;
    }
    // Resolve the shared data window (exactly one DMA_REGION per row).
    uint64_t data_phys = 0;
    uint64_t data_pages = 0;
    if (count > 0) {
        bool found = false;
        for (uint64_t i = 0; i < count; ++i) {
            if (specs[i].kind == sdf::CapKind::DMA_REGION) {
                if (!sdf::sdf_role_frame(sdf::PdRole::BLOCK_SRV, i,
                                         data_phys, data_pages) ||
                    data_phys == 0 || data_pages < kBlockRegionPages) {
                    clear_bound();
                    return false;
                }
                found = true;
            }
        }
        if (!found) {
            clear_bound();
            return false;
        }
    }
    // SIL3 audit #47 step-b1: §11.6 atomic stores pair with the atomic
    // loads in the accessors, ensure_bound() and transfer_sector().
    __atomic_store_n(reinterpret_cast<uint8_t *>(&g_backend),
                     static_cast<uint8_t>(backend), __ATOMIC_RELAXED);
    __atomic_store_n(&g_sector_count,
                     (backend == BlkBackend::NONE) ? 0ULL : sectors,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_phys, data_phys, __ATOMIC_RELAXED);
    __atomic_store_n(&g_data_pages, data_pages, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bound_epoch, block_srv_epoch(), __ATOMIC_RELAXED);
    return true;
}

uint64_t block_srv_sector_count() {
    return __atomic_load_n(&g_sector_count, __ATOMIC_RELAXED);
}

uint64_t block_srv_sector_size() {
    if (__atomic_load_n(&g_sector_count, __ATOMIC_RELAXED) == 0) {
        return 0;
    }
    return kBlockSectorSize;
}

BlkBackend block_srv_backend() {
    return static_cast<BlkBackend>(
        __atomic_load_n(reinterpret_cast<uint8_t *>(&g_backend),
                        __ATOMIC_RELAXED));
}

namespace {

bool ensure_bound() {
    // SIL3 audit #47 step-b1: §11.6 atomic loads pair with the atomic
    // stores in clear_bound()/block_srv_bind().
    const uint32_t bound_epoch =
        __atomic_load_n(&g_bound_epoch, __ATOMIC_RELAXED);
    const uint8_t bound_backend = __atomic_load_n(
        reinterpret_cast<uint8_t *>(&g_backend), __ATOMIC_RELAXED);
    const uint64_t bound_sectors =
        __atomic_load_n(&g_sector_count, __ATOMIC_RELAXED);
    if (bound_epoch == block_srv_epoch()) {
        return bound_backend != static_cast<uint8_t>(BlkBackend::NONE) &&
               bound_sectors != 0;
    }
    clear_bound();
    if (!block_srv_bind()) {
        return false;
    }
    const uint8_t rebound_backend = __atomic_load_n(
        reinterpret_cast<uint8_t *>(&g_backend), __ATOMIC_RELAXED);
    const uint64_t rebound_sectors =
        __atomic_load_n(&g_sector_count, __ATOMIC_RELAXED);
    return rebound_backend != static_cast<uint8_t>(BlkBackend::NONE) &&
           rebound_sectors != 0;
}

// One transfer through the shared window. Write path pre-copies into
// the frame under a release fence; read path copies out under an
// acquire fence after a validated reply.
int64_t transfer_sector(bool is_write, uint64_t lba,
                        const uint8_t *write_src, uint8_t *read_dst) {
    if (!ensure_bound()) {
        return -38; // -ENOSYS: PD unavailable or degraded
    }
    // SIL3 audit #47 step-b1: §11.6 — the OOB bound is stored
    // atomically, so load it atomically.
    const uint64_t oob_sectors =
        __atomic_load_n(&g_sector_count, __ATOMIC_RELAXED);
    if (lba >= oob_sectors) {
        return -22; // -EINVAL: out of range
    }
    if ((is_write && write_src == nullptr) ||
        (!is_write && read_dst == nullptr)) {
        return -22; // -EINVAL: null window
    }
    if (__atomic_test_and_set(&g_claim, __ATOMIC_ACQUIRE) != 0) {
        return -11; // -EAGAIN: one in-flight RPC only, fail fast
    }
    // SIL3 audit #47 step-b1: §11.6 — window phys is stored atomically.
    const uint64_t window_phys =
        __atomic_load_n(&g_data_phys, __ATOMIC_RELAXED);
    auto *win = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + window_phys +
                                            kBlockDataOffset);
    if (!is_write) {
        __builtin_memset(win, 0, kBlockSectorSize);
    } else {
        __builtin_memcpy(win, write_src, kBlockSectorSize);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    const uint64_t seq =
        __atomic_fetch_add(&g_seq, 1ULL, __ATOMIC_RELAXED) + 1ULL;
    BlockRpc req{};
    req.op = static_cast<uint8_t>(is_write ? BlockSrvOp::WRITE_BLOCK
                                           : BlockSrvOp::READ_BLOCK);
    fill_identity(req);
    req.lba = lba;
    req.arg = seq;
    BlockRpc reply{};
    const bool rpc_ok = block_srv_rpc(req, reply);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    __atomic_clear(&g_claim, __ATOMIC_RELEASE);
    if (!rpc_ok) {
        return -5; // -EIO: transport/timeout, fail closed
    }
    if (reply.epoch != req.epoch) {
        // Restart raced the RPC: drop the stale reply, force a rebind
        // on the next call, report once (-EAGAIN, never retried here).
        clear_bound();
        return -11;
    }
    if (reply.op != req.op || reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen || reply.arg != seq ||
        reply.status_op != req.op) {
        return -5; // -EIO: mismatched/stale reply, never consumed
    }
    const int16_t code = static_cast<int16_t>(reply.status);
    if (code != 0) {
        return static_cast<int64_t>(code);
    }
    if (!is_write) {
        __builtin_memcpy(read_dst, win, kBlockSectorSize);
    }
    return 0;
}

} // namespace

int64_t block_srv_read_sector(uint64_t lba, uint8_t *buffer) {
    return transfer_sector(false, lba, nullptr, buffer);
}

int64_t block_srv_write_sector(uint64_t lba, const uint8_t *buffer) {
    return transfer_sector(true, lba, buffer, nullptr);
}

} // namespace block_srv
} // namespace kernel

#endif // CONFIG_BLOCK_SRV
