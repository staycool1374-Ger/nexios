#include <kernel/driver/driver_pds.hpp>

#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                             \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace driver_pds {
namespace {

uint64_t g_role_pids[6] = {};
uint32_t g_epoch = 1;

struct Tombstone {
    uint32_t owner_id;
    uint32_t owner_gen;
};

Tombstone g_tombstones[16] = {};
uint64_t g_tombstone_count = 0;

uint64_t role_index(sdf::PdRole role) {
    return static_cast<uint64_t>(role);
}

} // namespace

void set_pd_pid(sdf::PdRole role, uint64_t pid) {
    const uint64_t idx = role_index(role);
    if (idx < 6) {
        __atomic_store_n(&g_role_pids[idx], pid, __ATOMIC_RELAXED);
    }
}

uint64_t get_pd_pid(sdf::PdRole role) {
    const uint64_t idx = role_index(role);
    if (idx >= 6) {
        return 0;
    }
    return __atomic_load_n(&g_role_pids[idx], __ATOMIC_RELAXED);
}

bool pd_alive(sdf::PdRole role) {
    const uint64_t pid = get_pd_pid(role);
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

uint32_t pd_epoch() {
    return __atomic_load_n(&g_epoch, __ATOMIC_RELAXED);
}

void pd_bump_epoch() {
    __atomic_fetch_add(&g_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void pd_on_restart(sdf::PdRole role, uint64_t pid) {
    set_pd_pid(role, pid);
    pd_bump_epoch();
}

void pd_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
    uint64_t count =
        __atomic_load_n(&g_tombstone_count, __ATOMIC_RELAXED);
    if (count >= 16) {
        return; // bounded residual drop (documented, ≤ table size)
    }
    g_tombstones[count].owner_id = owner_id;
    g_tombstones[count].owner_gen = owner_gen;
    __atomic_store_n(&g_tombstone_count, count + 1, __ATOMIC_RELAXED);
}

bool pd_rpc(sdf::PdRole role, const DriverRpc &req, DriverRpc &reply) {
    const uint64_t pid = get_pd_pid(role);
    if (pid == 0 || !pd_alive(role)) {
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
    if (raw.sender_id != pid || reply.epoch != pd_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

} // namespace driver_pds
} // namespace kernel

#endif // any CONFIG_DRIVER_PD_*
