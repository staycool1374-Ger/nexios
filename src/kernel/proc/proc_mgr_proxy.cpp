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
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

namespace kernel::proc::proc_mgr_proxy {
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
