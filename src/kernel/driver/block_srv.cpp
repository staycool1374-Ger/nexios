#include <kernel/driver/block_srv.hpp>

#if CONFIG_BLOCK_SRV

/*
 * NexIOS RTOS — Block server proxy (issue #47).
 * Whole TU is `#if CONFIG_BLOCK_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#47 tree. Kernel probing stays until flip.
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

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
    return reply.status;
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
    return reply.status;
}

} // namespace block_srv
} // namespace kernel

#endif // CONFIG_BLOCK_SRV
