#include <kernel/cap/cap_broker.hpp>

#if CONFIG_CAP_BROKER

/*
 * NexIOS RTOS — Capability-broker policy proxy (issue #240).
 * Whole TU is `#if CONFIG_CAP_BROKER`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#240 tree. Broker-allows/kernel-enforces:
 * the broker owns per-task policy words, the kernel enforces the cached
 * word on the delegation path. Step-a: BIND+LOOPBACK live, policy ops
 * typed -ENOSYS pre-flip (no default-deny flip until the §7 gate).
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace cap_broker {
namespace {

uint64_t g_cap_broker_pid = 0;
uint32_t g_cap_broker_epoch = 1;

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

void set_cap_broker_pid(uint64_t pid) {
    g_cap_broker_pid = pid;
}

uint64_t get_cap_broker_pid() {
    return __atomic_load_n(&g_cap_broker_pid, __ATOMIC_RELAXED);
}

bool cap_broker_alive() {
    const uint64_t pid = get_cap_broker_pid();
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

uint32_t cap_broker_epoch() {
    return __atomic_load_n(&g_cap_broker_epoch, __ATOMIC_RELAXED);
}

void cap_broker_bump_epoch() {
    __atomic_fetch_add(&g_cap_broker_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void cap_broker_on_restart(uint64_t pid) {
    set_cap_broker_pid(pid);
    cap_broker_bump_epoch();
}

void cap_broker_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
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

bool cap_broker_rpc(const CapBrokerRpc &req, CapBrokerRpc &reply) {
    const uint64_t pid = get_cap_broker_pid();
    if (pid == 0 || !cap_broker_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != cap_broker_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

namespace {

bool cap_broker_bind_op(CapBrokerOp op, uint64_t arg0, uint64_t *echo_out,
                        int64_t *status_out) {
    CapBrokerRpc req{};
    req.op = static_cast<uint8_t>(op);
    req.arg0 = arg0;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = cap_broker_epoch();
    CapBrokerRpc reply{};
    if (!cap_broker_rpc(req, reply)) {
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

int64_t cap_broker_probe_bind() {
    int64_t status = -38;
    if (!cap_broker_bind_op(CapBrokerOp::BIND, 0, nullptr, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

int64_t cap_broker_probe_loopback(uint64_t payload, uint64_t *echo) {
    int64_t status = -38;
    uint64_t got = 0;
    if (!cap_broker_bind_op(CapBrokerOp::LOOPBACK, payload, &got,
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

int64_t cap_broker_probe_set_policy(uint64_t target_id, uint32_t mask) {
    (void)target_id;
    (void)mask;
    return -38; // -ENOSYS: policy ops land with the step-b flip, not step-a
}

int64_t cap_broker_probe_get_policy(uint64_t target_id, uint32_t *mask) {
    (void)target_id;
    (void)mask;
    return -38; // -ENOSYS: policy ops land with the step-b flip, not step-a
}

} // namespace cap_broker
} // namespace kernel

#endif // CONFIG_CAP_BROKER
