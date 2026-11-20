#include <kernel/proc/proc_mgr_proxy.hpp>

#if CONFIG_PROC_MGR

/*
 * NexIOS RTOS — Proc-mgr policy proxy (F11, #564).
 * Whole TU is `#if CONFIG_PROC_MGR`-guarded: the OFF build (`0`,
 * shipped default) yields an empty TU (no symbols). Stage A wires no
 * caller: BIND/LOOPBACK plus the seven verdict ops land with the Stage B
 * PD; the proxy below is the bounded transport they will share
 * (procfs_srv.cpp shape: PID cell, epoch, liveness, bounded Call with
 * reply-identity validation per the #441 dead-letter rule).
 */

#include <kernel/ipc/ipc.hpp>
#include <kernel/ipc/ipc_errors.hpp>
#include <kernel/proc/proc_svc.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel::proc::proc_mgr_proxy {
namespace {

/// @brief Probe helper (procfs_srv shape): one owner-tagged round-trip
///        for BIND/LOOPBACK; echo + status captured on success.
bool probe_op(ProcMgrOp op, uint64_t arg0, uint64_t *echo_out,
              int64_t *status_out) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return false;
    }
    ProcMgrRpc req{};
    req.op = static_cast<uint8_t>(op);
    req.epoch = proc_mgr_epoch();
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.arg0 = arg0;
    ProcMgrRpc reply{};
    if (proc_mgr_call(req, reply) != errors::PROC_ERR_OK) {
        return false;
    }
    if (ProcSvc::handle_decision(reply) != errors::PROC_ERR_OK) {
        return false;
    }
    if (echo_out != nullptr) {
        *echo_out = reply.arg0;
    }
    if (status_out != nullptr) {
        *status_out = static_cast<int64_t>(reply.status);
    }
    return true;
}

} // namespace

TaskControlBlock *proc_mgr_holder() noexcept {
    const uint64_t pid = get_proc_mgr_pid();
    if (pid == 0) {
        return nullptr;
    }
    auto *srv = Scheduler::find_task(pid);
    if (srv == nullptr || srv->magic != TaskControlBlock::TCB_MAGIC) {
        return nullptr;
    }
    if (srv->state != TaskState::TERMINATED &&
        srv->state != TaskState::REAPED) {
        return srv;
    }
    return nullptr;
}

errors::ProcError proc_mgr_verdict(ProcMgrOp op, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2) noexcept {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return errors::PROC_ERR_NO_SERVER;
    }
    ProcMgrRpc req{};
    req.op = static_cast<uint8_t>(op);
    req.flags = 0;
    req.rsvd = 0;
    req.epoch = proc_mgr_epoch();
    req.owner_id = static_cast<uint32_t>(cur->id);
    req.owner_gen = cur->generation;
    req.arg0 = arg0;
    req.arg1 = arg1;
    req.arg2 = arg2;
    req.status_op = 0;
    req.status = 0;
    req.rsvd3 = 0;
    ProcMgrRpc reply{};
    const errors::ProcError call_err = proc_mgr_call(req, reply);
    if (call_err == errors::PROC_ERR_RPC_BOUND) {
        // Outside any lock (INV-PROC-3: this function holds none).
        log::dmesg_push_sev(log::ErrorSubsystem::TASK,
                            log::kDmesgBase_TASK + 21, log::LogSeverity::WARN,
                            "Proc-mgr RPC bound expired", cur->id);
        return errors::PROC_ERR_RPC_BOUND;
    }
    if (call_err != errors::PROC_ERR_OK) {
        return call_err;
    }
    const errors::ProcError shape_err = ProcSvc::handle_decision(reply);
    if (shape_err != errors::PROC_ERR_OK) {
        return shape_err;
    }
    if (reply.status != 0) {
        // Deny-only rule (INV-PROC-11): any non-zero PD verdict fails
        // the op closed. Stage B policy never produces this for the
        // seven ops (allow-with-typed-refusals); the mapping keeps the
        // refusal typed instead of consuming it.
        return errors::PROC_ERR_STOP_REFUSED;
    }
    return errors::PROC_ERR_OK;
}

int64_t proc_mgr_probe_bind() {
    int64_t status = -38;
    if (!probe_op(ProcMgrOp::BIND, 0, nullptr, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    return status;
}

int64_t proc_mgr_probe_loopback(uint64_t payload, uint64_t *echo_out) {
    int64_t status = -38;
    uint64_t got = 0;
    if (!probe_op(ProcMgrOp::LOOPBACK, payload, &got, &status)) {
        return -38; // -ENOSYS: server unavailable pre-flip
    }
    if (status != 0) {
        return status;
    }
    if (echo_out != nullptr) {
        *echo_out = got;
    }
    return 0;
}
namespace {

uint64_t g_proc_mgr_pid = 0;
uint32_t g_proc_mgr_epoch = 1;

} // namespace

void set_proc_mgr_pid(uint64_t pid) {
    __atomic_store_n(&g_proc_mgr_pid, pid, __ATOMIC_RELEASE);
}

uint64_t get_proc_mgr_pid() {
    return __atomic_load_n(&g_proc_mgr_pid, __ATOMIC_ACQUIRE);
}

bool proc_mgr_alive() {
    const uint64_t pid = get_proc_mgr_pid();
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

uint32_t proc_mgr_epoch() {
    return __atomic_load_n(&g_proc_mgr_epoch, __ATOMIC_ACQUIRE);
}

void proc_mgr_bump_epoch() {
    __atomic_fetch_add(&g_proc_mgr_epoch, 1U, __ATOMIC_RELAXED);
}

void proc_mgr_on_restart(uint64_t pid) {
    set_proc_mgr_pid(pid);
    proc_mgr_bump_epoch();
}

errors::ProcError proc_mgr_call(const ProcMgrRpc &req,
                               ProcMgrRpc &reply_out) noexcept {
    const uint64_t pid = get_proc_mgr_pid();
    if (pid == 0 || !proc_mgr_alive()) {
        return errors::PROC_ERR_NO_SERVER;
    }
    auto *cur = Scheduler::current_task();
    if (cur == nullptr) {
        return errors::PROC_ERR_NO_SERVER;
    }
    Message msg{};
    msg.type = static_cast<uint64_t>(req.op);
    msg.priority = cur->priority;
    __builtin_memcpy(msg.data, &req, sizeof(req));
    msg.data_size = sizeof(req);
    Message raw{};
    // INV-PROC-2: bounded Call (CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS); expiry
    // fails closed, never a held block. No lock held here (INV-PROC-3 —
    // the Stage B caller resolves and captures before the Call).
    const errors::IpcError call_err = IPC::send_sync_bounded_err(
        pid, msg, raw, CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS);
    if (call_err == errors::IPC_ERR_TIMEOUT) {
        return errors::PROC_ERR_RPC_BOUND;
    }
    if (call_err != errors::IPC_ERR_OK) {
        return errors::PROC_ERR_NO_SERVER;
    }
    if (raw.data_size != sizeof(reply_out)) {
        return errors::PROC_ERR_BAD_REQUEST;
    }
    __builtin_memcpy(&reply_out, raw.data, sizeof(reply_out));
    // Reply identity: server, epoch, owner (id+gen), op echo. A mismatch
    // is refused here (BAD_REQUEST), never consumed.
    if (raw.sender_id != pid || reply_out.epoch != proc_mgr_epoch() ||
        reply_out.owner_id != req.owner_id ||
        reply_out.owner_gen != req.owner_gen ||
        reply_out.status_op != req.op) {
        return errors::PROC_ERR_BAD_REQUEST;
    }
    return errors::PROC_ERR_OK;
}

} // namespace kernel::proc::proc_mgr_proxy

#endif // CONFIG_PROC_MGR
