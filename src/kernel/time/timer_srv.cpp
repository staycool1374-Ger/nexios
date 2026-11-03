#include <kernel/time/timer_srv.hpp>

#if CONFIG_TIMER_SRV

/*
 * NexIOS RTOS — Timer/Event externalization proxy (issue #241).
 * Whole TU is `#if CONFIG_TIMER_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#241 tree.
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel::time {
namespace timer_srv {
namespace {

uint64_t g_timer_srv_pid = 0;
uint32_t g_timer_srv_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

/// @brief Bounded wait budget for one server RPC (coarse ticks).
/// Fail-closed `-EAGAIN` past this; never parks forever on a dead server.
constexpr uint64_t kRpcGuardTicks = 5000;

} // namespace

void set_timer_srv_pid(uint64_t pid) {
    g_timer_srv_pid = pid;
}

uint64_t get_timer_srv_pid() {
    return __atomic_load_n(&g_timer_srv_pid, __ATOMIC_RELAXED);
}

bool timer_srv_alive() {
    const uint64_t pid = get_timer_srv_pid();
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

uint32_t timer_srv_epoch() {
    return __atomic_load_n(&g_timer_srv_epoch, __ATOMIC_RELAXED);
}

void timer_srv_bump_epoch() {
    __atomic_fetch_add(&g_timer_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void on_restart(uint64_t pid) {
    set_timer_srv_pid(pid);
    timer_srv_bump_epoch();
}

void note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
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

bool timer_srv_rpc(const TimerRpc &req, TimerRpc &reply) {
    const uint64_t pid = get_timer_srv_pid();
    if (pid == 0 || !timer_srv_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != timer_srv_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

int64_t sys_clock_gettime_srv(uint64_t clock_id, uint64_t out_timespec_ptr) {
    (void)clock_id;
    (void)out_timespec_ptr;
    return -38; // -ENOSYS: server path lands with the flip (not this lane)
}

int64_t sys_nanosleep_srv(uint64_t req_ptr, uint64_t rem_ptr) {
    (void)req_ptr;
    (void)rem_ptr;
    return -38;
}

int64_t sys_timer_create_srv(uint64_t clock_id, uint64_t sevp_ptr,
                             uint64_t timerid_ptr) {
    (void)clock_id;
    (void)sevp_ptr;
    (void)timerid_ptr;
    return -38;
}

int64_t sys_timerfd_create_srv(uint64_t clock_id, uint64_t flags) {
    (void)clock_id;
    (void)flags;
    return -38;
}

} // namespace timer_srv
} // namespace kernel::time

#endif // CONFIG_TIMER_SRV
