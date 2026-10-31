#include <kernel/term/term_srv.hpp>

#if CONFIG_TERM_SRV

/*
 * NexIOS RTOS — Terminal/emulation policy proxy (issue #51).
 * Whole TU is `#if CONFIG_TERM_SRV`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols), so the link is
 * byte-identical to the pre-#51 tree. Emulation policy only — the
 * kernel keeps panic raw_write + serial mechanism.
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel {
namespace term_srv {
namespace {

uint64_t g_term_srv_pid = 0;
uint32_t g_term_srv_epoch = 1;

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

void set_term_srv_pid(uint64_t pid) {
    g_term_srv_pid = pid;
}

uint64_t get_term_srv_pid() {
    return __atomic_load_n(&g_term_srv_pid, __ATOMIC_RELAXED);
}

bool term_srv_alive() {
    const uint64_t pid = get_term_srv_pid();
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

uint32_t term_srv_epoch() {
    return __atomic_load_n(&g_term_srv_epoch, __ATOMIC_RELAXED);
}

void term_srv_bump_epoch() {
    __atomic_fetch_add(&g_term_srv_epoch, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&g_tombstone_count, 0, __ATOMIC_RELAXED);
}

void term_srv_on_restart(uint64_t pid) {
    set_term_srv_pid(pid);
    term_srv_bump_epoch();
}

void term_srv_note_dead_owner(uint32_t owner_id, uint32_t owner_gen) {
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

bool term_srv_rpc(const TermRpc &req, TermRpc &reply) {
    const uint64_t pid = get_term_srv_pid();
    if (pid == 0 || !term_srv_alive()) {
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
    if (raw.sender_id != pid || reply.epoch != term_srv_epoch() ||
        reply.owner_id != req.owner_id ||
        reply.owner_gen != req.owner_gen) {
        return false;
    }
    return true;
}

namespace {

bool term_srv_bind_op(TermSrvOp op, int64_t *out_status) {
    TermRpc req{};
    req.op = static_cast<uint8_t>(op);
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.epoch = term_srv_epoch();
    TermRpc reply{};
    if (!term_srv_rpc(req, reply)) {
        return false;
    }
    if (out_status != nullptr) {
        *out_status = reply.status;
    }
    return true;
}

} // namespace

int64_t term_srv_probe_bind() {
    int64_t status = -38;
    if (!term_srv_bind_op(TermSrvOp::BIND, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

int64_t term_srv_fault_poison() {
    int64_t status = -38;
    if (!term_srv_bind_op(TermSrvOp::INJECT_POISON, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

} // namespace term_srv
} // namespace kernel

#endif // CONFIG_TERM_SRV
