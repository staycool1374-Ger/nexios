#include <kernel/vfs/procfs_srv.hpp>

#if CONFIG_PROCFS_SRV

/*
 * NexIOS RTOS — Procfs-server policy proxy (issue #54).
 * Whole TU is `#if CONFIG_PROCFS_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#54 tree. Renders stay in-kernel: the
 * vfsd PROCFS_RENDER=7 probe retains sole render ownership pre-flip
 * and this PD claims none. Step-a: BIND+LOOPBACK live, render ops
 * typed -ENOSYS pre-flip (no render flip until the §7 gate).
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace procfs_srv {
namespace {

uint64_t g_procfs_srv_pid = 0;
uint32_t g_procfs_srv_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

/// @brief Bounded wait for one server RPC (coarse ticks).
/// Fail-closed `-EAGAIN` past this; never parks a kernel task forever.
constexpr uint64_t kRpcGuardTicks = 5000;

} // namespace

void set_procfs_srv_pid(uint64_t pid) {
    g_procfs_srv_pid = pid;
}

uint64_t get_procfs_srv_pid() {
    return __atomic_load_n(&g_procfs_srv_pid, __ATOMIC_RELAXED);
}

bool procfs_srv_alive() {
    const uint64_t pid = get_procfs_srv_pid();
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

uint32_t procfs_srv_epoch() {
    return __atomic_load_n(&g_procfs_srv_epoch, __ATOMIC_RELAXED);
}

void procfs_srv_bump_epoch() {
    __atomic_fetch_add(&g_procfs_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void procfs_srv_on_restart(uint64_t pid) {
    set_procfs_srv_pid(pid);
    procfs_srv_bump_epoch();
}

void procfs_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
    // Lock-free ring write (teardown context: no locks, no IPC).
    uint64_t count =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_RELAXED);
    if (count >= 16) {
        return; // bounded residual drop (documented, <= table size)
    }
    g_tombstones[count].owner_id = owner_id;
    g_tombstones[count].owner_gen = owner_gen;
    __atomic_store_n(&g_tombstone_count, count + 1, __ATOMIC_RELAXED);
}

bool procfs_srv_rpc(const ProcfsSrvRpc &req, ProcfsSrvRpc &reply) {
    const uint64_t pid = get_procfs_srv_pid();
    if (pid == 0 || !procfs_srv_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != procfs_srv_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

namespace {

bool procfs_srv_bind_op(ProcfsSrvOp op, uint64_t arg0, uint64_t *echo_out,
                        int64_t *status_out) {
    ProcfsSrvRpc req{};
    req.op = static_cast<uint8_t>(op);
    req.arg0 = arg0;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = procfs_srv_epoch();
    ProcfsSrvRpc reply{};
    if (!procfs_srv_rpc(req, reply)) {
        return false;
    }
    if (echo_out != nullptr) {
        *echo_out = reply.arg0;
    }
    if (status_out != nullptr) {
        *status_out = reply.status;
    }
    return true;
}

} // namespace

int64_t procfs_srv_probe_bind() {
    int64_t status = -38;
    if (!procfs_srv_bind_op(ProcfsSrvOp::BIND, 0, nullptr, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

int64_t procfs_srv_probe_loopback(uint64_t payload, uint64_t *echo) {
    int64_t status = -38;
    uint64_t got = 0;
    if (!procfs_srv_bind_op(ProcfsSrvOp::LOOPBACK, payload, &got,
                             &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    if (status != 0) {
        return status;
    }
    if (echo != nullptr) {
        *echo = got;
    }
    return 0;
}

int64_t procfs_srv_probe_render(uint64_t ino, uint64_t offset,
                                uint64_t size) {
    (void)ino;
    (void)offset;
    (void)size;
    return -38; // -ENOSYS: renders land with the step-b flip, not step-a
}

} // namespace procfs_srv
} // namespace kernel

#endif // CONFIG_PROCFS_SRV
