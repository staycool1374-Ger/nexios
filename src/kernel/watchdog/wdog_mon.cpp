#include <kernel/watchdog/wdog_mon.hpp>

#if CONFIG_WDOG_MON

/*
 * NexIOS RTOS — Watchdog-monitor policy proxy (issue #390).
 * Whole TU is `#if CONFIG_WDOG_MON`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#390 tree. Monitor-composes-policy: the
 * kernel keeps the tick-scan (#41) + death pulses (#105) + watchdogd
 * base (#277); this proxy adds class policy + SHM + escalation over
 * that mechanism. Step-a: BIND+LOOPBACK live, register/heartbeat ops
 * typed -ENOSYS pre-flip (no supervision flip until the §7 gate).
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace wdog_mon {
namespace {

uint64_t g_wdog_mon_pid = 0;
uint32_t g_wdog_mon_epoch = 1;

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

void set_wdog_mon_pid(uint64_t pid) {
    g_wdog_mon_pid = pid;
}

uint64_t get_wdog_mon_pid() {
    return __atomic_load_n(&g_wdog_mon_pid, __ATOMIC_RELAXED);
}

bool wdog_mon_alive() {
    const uint64_t pid = get_wdog_mon_pid();
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

uint32_t wdog_mon_epoch() {
    return __atomic_load_n(&g_wdog_mon_epoch, __ATOMIC_RELAXED);
}

void wdog_mon_bump_epoch() {
    __atomic_fetch_add(&g_wdog_mon_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void wdog_mon_on_restart(uint64_t pid) {
    set_wdog_mon_pid(pid);
    wdog_mon_bump_epoch();
}

void wdog_mon_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
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

bool wdog_mon_rpc(const WdogMonRpc &req, WdogMonRpc &reply) {
    const uint64_t pid = get_wdog_mon_pid();
    if (pid == 0 || !wdog_mon_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != wdog_mon_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

namespace {

bool wdog_mon_bind_op(WdogMonOp op, uint64_t arg0, uint64_t *echo_out,
                      int64_t *status_out) {
    WdogMonRpc req{};
    req.op = static_cast<uint8_t>(op);
    req.arg0 = arg0;
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = wdog_mon_epoch();
    WdogMonRpc reply{};
    if (!wdog_mon_rpc(req, reply)) {
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

int64_t wdog_mon_probe_bind() {
    int64_t status = -38;
    if (!wdog_mon_bind_op(WdogMonOp::BIND, 0, nullptr, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

int64_t wdog_mon_probe_loopback(uint64_t payload, uint64_t *echo) {
    int64_t status = -38;
    uint64_t got = 0;
    if (!wdog_mon_bind_op(WdogMonOp::LOOPBACK, payload, &got, &status)) {
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

int64_t wdog_mon_probe_register(uint64_t target_id, uint32_t klass) {
    (void)target_id;
    (void)klass;
    return -38; // -ENOSYS: supervision lands with the step-b flip, not step-a
}

int64_t wdog_mon_probe_heartbeat(uint64_t target_id) {
    (void)target_id;
    return -38; // -ENOSYS: supervision lands with the step-b flip, not step-a
}

} // namespace wdog_mon
} // namespace kernel

#endif // CONFIG_WDOG_MON
