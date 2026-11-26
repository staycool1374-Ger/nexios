/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file proc_svc.cpp
/// @brief Proc-mgr service layer bodies (F11 Stage A, #564): the process
///        syscall mechanisms, moved statement-identical from
///        syscall_handlers_process.cpp (fork/waitpid/exec/kill/signal/
///        sigreturn) and syscall_handlers_misc.cpp (getpid). The
///        Syscall::sys_* wrappers keep their exact signatures and delegate
///        here — with CONFIG_PROC_MGR=0 the kernel path runs unchanged
///        (INV-PROC-7 behavior identity). No trap frame, TCB pointer or
///        kernel stack address ever crosses a PD boundary (INV-PROC-1):
///        these bodies run in the invoking client's context with the
///        caller's regs; Stage B adds a verdict-only proxy call, never a
///        mechanism move.

#include <kernel/proc/proc_svc.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/test/test_isolate.hpp>
#if defined(CONFIG_DEBUG)
#include <kernel/test/wait_publish_seam.hpp>
#endif
#include <signal.hpp>
#include <constants.hpp>
#include <string.hpp>
#include <kernel/nexios_config.h>
#if CONFIG_PROC_MGR
#include <kernel/proc/proc_mgr_proxy.hpp>
#include <kernel/proc/proc_tcb_caps.hpp>
#endif
#if CONFIG_PROC_MGR_CARVE_FORK
#include <kernel/proc/proc_fork_carve.hpp>
#endif

namespace kernel::proc {

#if CONFIG_PROC_MGR
namespace {

// Issue #564 F11 Stage B: one verdict RPC per syscall (never per
// waitpid loop iteration), in caller context with no lock held
// (INV-PROC-1/2/3). The mechanism below each verdict is unchanged; a
// non-OK verdict fails the op closed with the generic boundary code.
errors::ProcError ask_proc_verdict(ProcMgrOp op, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2) noexcept {
    return proc_mgr_proxy::proc_mgr_verdict(op, arg0, arg1, arg2);
}

bool proc_verdict_denied(errors::ProcError err) noexcept {
    return err != errors::PROC_ERR_OK;
}

} // namespace
#endif

uint64_t ProcSvc::fork(uint64_t, uint64_t, uint64_t, uint64_t,
                       uint64_t *regs) {
    if (!regs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
#if CONFIG_PROC_MGR
    // Q-B2 deny-only (INV-PROC-11): the proxy may refuse the fork; the
    // quota/ownership mechanism below still runs on OK.
    if (proc_verdict_denied(
            ask_proc_verdict(ProcMgrOp::FORK, 0, 0, 0))) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
#endif
    // Issue #564, F11 Stage C: carve-composed fork (retype + VSpace
    // map + foreign-frame map + carved-root activation, no kernel deep
    // copy) under CONFIG_PROC_MGR_CARVE_FORK; the knob-OFF leg is the
    // statement-identical kernel-composed clone (INV-CARVE-3: the sole
    // user-task publication path — unpublished until add_task_err
    // either way; the hold + add_task_err + release-then-destroy tail
    // below is shared verbatim, INV-CARVE-10).
    auto *child =
#if CONFIG_PROC_MGR_CARVE_FORK
        carve_fork(regs);
#else
        TaskControlBlock::clone(regs);
#endif
    if (!child)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
#if CONFIG_PROC_MGR
    // INV-PROC-12 (pre-publication hold): the hold is installed before
    // Scheduler::add_task_err publishes the child. Hold failure destroys
    // the child pre-publication (no publish_task_death, no death record,
    // no waiter wake — mirrors the admission-denial destroy below).
    auto *holder = proc_mgr_proxy::proc_mgr_holder();
    if (holder == nullptr) {
        TaskControlBlock::destroy(child);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    errors::ProcError hold_err = errors::PROC_ERR_OK;
    if (proc_tcb_caps::hold(*holder, child->id, child->generation,
                            &hold_err) < 0) {
        TaskControlBlock::destroy(child);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
#endif
    // Issue #20: fork goes through the enforced admission gate.  On denial
    // the child was never registered, so destroy() teardown is safe
    // (remove paths no-op on absent entries); the parent is unaffected and
    // the caller sees -1 (fail-closed, no zombie birth, no parent wake).
    errors::SchedulerError adm = Scheduler::add_task_err(*child);
    if (adm != errors::SCHED_ERR_OK) {
#if CONFIG_PROC_MGR
        (void)proc_tcb_caps::release(*holder, child->id, child->generation);
#endif
        TaskControlBlock::destroy(child);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    return child->id;
}

namespace {

/// @brief Scan-predicate helpers (issue #590 step 1): identical semantics
///        to the inlined scan they replace.  find_wait_child_by_id is the
///        only way the publish re-verify re-resolves the child — a carried
///        pointer never crosses the publication window.
TaskControlBlock *find_wait_child(TaskControlBlock &parent,
                                  uint64_t target_pid) noexcept {
    uint64_t count = Scheduler::task_count();
    for (uint64_t i = 0; i < count; ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->parent_id == parent.id) {
            // Issue #53: a thread with a registered joiner is owned by
            // that joiner (which reaps it on collect) — waitpid skips
            // it instead of racing the join.  Non-threads and
            // join-less threads are unaffected.
            if (t->is_thread() &&
                __atomic_load_n(&t->join_waiter_, __ATOMIC_ACQUIRE) !=
                    nullptr) {
                continue;
            }
            // (uint64_t)-1 is the any-child wildcard (waitpid pid -1),
            // not a failure code — deliberately raw, see H6 note #257.
            if (target_pid == static_cast<uint64_t>(-1) ||
                t->id == target_pid) {
                return t;
            }
        }
    }
    return nullptr;
}

TaskControlBlock *find_wait_child_by_id(TaskControlBlock &parent,
                                        uint64_t child_id) noexcept {
    uint64_t count = Scheduler::task_count();
    for (uint64_t i = 0; i < count; ++i) {
        auto *t = Scheduler::task_at(i);
        if (t && t->parent_id == parent.id && t->id == child_id) {
            if (t->is_thread() &&
                __atomic_load_n(&t->join_waiter_, __ATOMIC_ACQUIRE) !=
                    nullptr) {
                continue;
            }
            return t;
        }
    }
    return nullptr;
}

/// @brief Bounded revive-retry cap (issue #590 §6): the undo leg gives up
///        after this many TERMINATED re-observations and falls through to
///        the normal block with the publication intact (never undo — a
///        real wake must not fight the re-verify).
constexpr uint64_t kWaitReverifyRetryMax = 4;

} // namespace

uint64_t ProcSvc::waitpid(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t, uint64_t *) {
    uint64_t target_pid = arg0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto status = checked(reinterpret_cast<uint64_t *>(arg1));
    auto *status_ptr = (!syscall_is_user_task() || status.valid())
                           ? status.unsafe_ptr()
                           : nullptr;
    auto *cur = syscall_task();
    if (!cur)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
#if CONFIG_PROC_MGR
    // Issue #564 F11 Stage B: exactly one verdict per waitpid call (the
    // ownership predicate), resolved before the blocking loop — no RPC
    // inside the loop. The find/collect/block/reap machinery below stays
    // statement-identical (INV-PROC-1). Kernel-task callers bypass the
    // verdict (proc-mgr owns only user processes); otherwise
    // scheduler_reap_respects_parent_wait fails under ON.
    if (syscall_is_user_task() &&
        proc_verdict_denied(
            ask_proc_verdict(ProcMgrOp::WAITPID, target_pid, 0, 0))) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
#endif
    // Issue #221: a user trap must not return to userspace until the wait
    // resolves — returning -1 immediately fails single-shot waiters (the
    // fork-marker parent prints WAITPID-FAILED).  Kernel callers hlt-loop
    // themselves (see scheduler_waitpid_wakes_parent); user traps wait
    // in-trap (hlt, preemptible) and re-scan on wake, mirroring the
    // sys_receive blocked-wait pattern.
    uint64_t reverify_retries = 0;
    for (;;) {
        TaskControlBlock *child = find_wait_child(*cur, target_pid);
        if (!child) {
            // Issue #590 second residence: the corpse may have left the
            // live tables inside the publication window (released to the
            // zombie list) — collect it there before failing closed.
            uint64_t zid = 0;
            uint64_t zcode = 0;
            if (Scheduler::collect_zombie_child(*cur, target_pid, zid,
                                                zcode)) {
                if (status_ptr) {
                    // MP-4 (SMAP): user write via safe_copy_to_user
                    // (stac-wrapped); kernel-task callers write directly.
                    if (syscall_is_user_task()) {
                        if (!safe_copy_to_user(status.unsafe_ptr(), &zcode,
                                               1))
                            return static_cast<uint64_t>(
                                errors::SYS_ERR_GENERIC_FAIL);
                    } else {
                        *status_ptr = zcode;
                    }
                }
                return zid;
            }
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        // Issue #593 (INV-593-5): a corpse still current on any CPU is
        // unfreeable — its stack is live until the trap epilogue applies
        // the standing arm.  Falling through is safe: observing the child
        // in all_tasks_ proves release_zombie has not run yet, so the
        // funnel's later wake still matches this parent's publication (no
        // lost wake), and a WNOHANG caller correctly reports nothing ready.
        if (child->state == TaskState::TERMINATED &&
            !Scheduler::is_current_on_any_cpu(child)) {
            if (child->is_thread() &&
                !TaskControlBlock::thread_join_try_claim(*child)) {
                // Lost the collect race to a concurrent joiner/detach/
                // reaper collect: re-scan instead of double-reaping.
                // (waitpid skips joined threads above, so the winner here
                // can only be a detach-consume or the reaper.)
                continue;
            }
            if (status_ptr) {
                // MP-4 (SMAP): user write via safe_copy_to_user
                // (stac-wrapped); kernel-task callers write directly.
                uint64_t code = child->exit_code;
                if (syscall_is_user_task()) {
                    if (!safe_copy_to_user(status.unsafe_ptr(), &code, 1))
                        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
                } else {
                    *status_ptr = code;
                }
            }
            uint64_t cid = child->id;
            cur->remove_child(child);
            child->cleanup();
            Scheduler::remove_task(*child);
            delete child;
            return cid;
        }
        if (arg2 & 1)
            return static_cast<uint64_t>(errors::SYS_ERR_OK); // WNOHANG with nothing reaped: zero pids ready
                      // (POSIX waitpid count, not status — keep numeric).
        // Issue #588: two-cert contract BEFORE any wait-state publication
        // (INV-WAIT-2 — a rejected pointer must leave no stale
        // waiting_child_pid behind): range (checked() above) → mapped →
        // writable over the whole 8-byte window, fail closed with the typed
        // fault code.  The wake-path armed store (deliver_wait_status) is
        // the TOCTOU backstop for remaps while BLOCKED.
        if (syscall_is_user_task() && status.unsafe_ptr()) {
            const uint64_t status_va =
                reinterpret_cast<uint64_t>(status.unsafe_ptr());
            if (VMM::virt_to_phys_in_pml4(status_va, cur->get_page_table()) ==
                0)
                return static_cast<uint64_t>(errors::SYS_ERR_FAULT);
            if (!VMM::user_write_window_writable(status_va, sizeof(uint64_t),
                                                 cur->get_page_table()))
                return static_cast<uint64_t>(errors::SYS_ERR_FAULT);
        }
        // Issue #590 INV-WAIT-4: publish order is status → dequeue →
        // BLOCKED → pid, pid LAST.  The wake gate is state==BLOCKED +
        // pid, and the fence below pairs them — a terminator that
        // observes the pid also observes BLOCKED.
        //
        // Issue #607: the whole publication is ONE IRQ-atomic unit.
        // It used to inherit whatever IF the caller held — 0 for an
        // int $0x80 user trap (interrupt gate 0xEE), but 1 for a
        // kernel-task caller (:390 leg) and for a CONFIG_PROC_MGR=1
        // build (ask_proc_verdict's blocking IPC returns with sti,
        // ipc.cpp:1475/1507).  A tick landing mid-sequence there
        // dequeues the parent before the pid is published, the
        // terminator's wake_waiting_parent reads waited != child.id,
        // returns NO_WAITER, and the parent blocks with no waker.
        // Closing it also removes the undeclared dependence on
        // trap-entry IF that irq-preemption-contract.md:13 forbids.
        // Boundedness: no lock (dequeue_ready is lock-free); fixed-
        // bound loops only (128 ready levels, 8 CPU arms); no
        // alloc/log/IPC and no reschedule/park in this scope.  The
        // #590 seam guard inside fire_wait_publish nests (depth 2)
        // and cannot emit a stray sti (irq_was_=false there).
        const uint64_t child_id = child->id;
        {
            arch::IrqGuard publish_guard{};
            cur->waiting_child_status = status_ptr;
            Scheduler::dequeue_ready(*cur);
            cur->state = TaskState::BLOCKED;
            Scheduler::note_block_transition("proc_waitpid_wait", cur);
            cur->waiting_child_pid = target_pid;
            // Issue #590 INV-WAIT-3: pair the terminator-side fence.
            // Both fences are required — release/acquire is not
            // sufficient here (store buffering would let both sides
            // miss on weak models).  Placed before the kernel-caller
            // return so the kernel-caller path is fenced as well.
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
#if defined(CONFIG_DEBUG)
            kernel::test::fire_wait_publish(*cur);
#endif
        }
        // Issue #590 INV-WAIT-6 (consumption precedence): after the
        // fence, a pid that no longer matches what was just published
        // proves a wake consumed the publication (the wake's clear is
        // the only other writer of this field on a live parent).  A
        // delivered wake always wins: do NOT undo, never return −1,
        // never block — resolution happens at the top of the loop via
        // the normal collect.  Undoing here would turn a delivered wake
        // into −1 and strand a parented zombie.  Compared against
        // target_pid (not 0) so a pathological target 0 cannot alias
        // the sentinel; waiting_child_status is NOT the indicator
        // (deliver_wait_status clears it on both the delivered and the
        // faulted path).
        if (__atomic_load_n(&cur->waiting_child_pid, __ATOMIC_ACQUIRE) !=
            target_pid)
            continue;
        // Re-resolve by id (never a carried pointer across the window):
        // child_id was captured in the scan epoch above.
        TaskControlBlock *re = find_wait_child_by_id(*cur, child_id);
        if (re != nullptr && re->state == TaskState::TERMINATED) {
            if (reverify_retries < kWaitReverifyRetryMax) {
                ++reverify_retries;
                // Undo order is status first, pid last: a concurrent
                // wake holding the old slot pointer must never see a
                // half-cleared publication.
                cur->waiting_child_status = nullptr;
                cur->waiting_child_pid = 0;
                Scheduler::set_task_ready(*cur);
                continue;
            }
            // Retry-cap exhaustion (only reachable when the rescan keeps
            // observing TERMINATED while ownership is lost): log once
            // and fall through to the normal block — do NOT undo, the
            // publication must stay intact for a real wake.
            log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                                log::kDmesgBase_SCHED + 23,
                                log::LogSeverity::WARN,
                                "Sched: waitpid re-verify cap exhausted",
                                cur->id);
        } else if (re == nullptr) {
            // The corpse left the live tables inside the window: resolve
            // via the second residence (collect_zombie_child).  On a
            // miss the pre-INV-WAIT-4 hole made the hlt loop exit on
            // pid == 0 and the rescan miss the orphaned child —
            // spurious −1 with a lost status (never an indefinite
            // block).  That leg now warns once and fails closed.
            uint64_t zid = 0;
            uint64_t zcode = 0;
            if (Scheduler::collect_zombie_child(*cur, target_pid, zid,
                                                zcode)) {
                if (status_ptr) {
                    if (syscall_is_user_task()) {
                        if (!safe_copy_to_user(status.unsafe_ptr(), &zcode,
                                               1))
                            return static_cast<uint64_t>(
                                errors::SYS_ERR_GENERIC_FAIL);
                    } else {
                        *status_ptr = zcode;
                    }
                }
                return zid;
            }
            log::dmesg_push_sev(log::ErrorSubsystem::SCHED,
                                log::kDmesgBase_SCHED + 23,
                                log::LogSeverity::WARN,
                                "Sched: waitpid unrecoverable wait",
                                cur->id);
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        if (!syscall_is_user_task())
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        // User trap: wait in-trap until woken (waiting flag cleared and
        // state changed by the wake path), then re-scan — the child is
        // TERMINATED and collected above (the reaper defers parented
        // zombies, issue #221, so it cannot vanish in between).  Spurious
        // resumes re-enter the wait; ticks preempt via sti/hlt/cli.
        Scheduler::reschedule();
        while (cur->state == TaskState::BLOCKED &&
               __atomic_load_n(&cur->waiting_child_pid, __ATOMIC_ACQUIRE) !=
                   0) {
            arch::sti();
            arch::hlt();
            arch::cli();
        }
    }
}

// VULN-H4/W1: hard bounds on the exec argv/envp scan.  Without these, a
// malicious task can place a non-NUL-terminated string abutting an unmapped
// page to trigger an unvalidated kernel-side page fault (DoS), or pass an
// unbounded entry count that stalls the syscall (WCET violation).
static constexpr size_t MAX_EXEC_ARGS = 64;
static constexpr size_t MAX_EXEC_ARG_LEN = SYSCALL_MAX_PATH; // 256

/// @brief Validate a user-space argv/envp array and bound every string.
/// @param ptr User pointer to the null-terminated pointer array.
/// @param is_user_task true if the caller is a Ring-3 task.
/// @param[in,out] out_total_len Accumulates the combined (argv+envp) string
///        byte total incl. terminators, for VULN-U2's stack reservation check.
/// @return true if the whole array (and every string window) is valid.
static bool validate_argv_envp(const char *const *ptr, bool is_user_task,
                               uint64_t *out_total_len = nullptr) {
    if (!ptr)
        return true;
    if (is_user_task) {
        auto arr = checked(ptr, static_cast<size_t>(1));
        if (!arr.valid())
            return false;
        const char *const *p = ptr;
        size_t arg_count = 0;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_exec);
        NEXIOS_FAULT_RECOVERY_KEEP(recover_exec);
        arch::stac();
        while (*p) {
            if (++arg_count > MAX_EXEC_ARGS) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
            // Validate the ENTIRE maximum-length window up front, so every
            // byte touched by the scan below is already certified mapped.
            auto s = checked(*p, static_cast<uint64_t>(MAX_EXEC_ARG_LEN));
            if (!s.valid()) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
            size_t len = 0;
            for (; len < MAX_EXEC_ARG_LEN; ++len) {
                if (s.unsafe_ptr()[len] == '\0')
                    break;
                if (len == MAX_EXEC_ARG_LEN - 1) {
                    arch::clac();
                    g_user_access_recover_ip = 0;
                    return false; // unterminated within the window
                }
            }
            if (out_total_len)
                *out_total_len += len + 1;
            ++p;
            auto next = checked(p, static_cast<size_t>(1));
            if (!next.valid()) {
                arch::clac();
                g_user_access_recover_ip = 0;
                return false;
            }
        }
        arch::clac();
        g_user_access_recover_ip = 0;
        return true;
    }
    return true;

recover_exec:
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;
}

uint64_t ProcSvc::exec(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                       uint64_t, uint64_t *regs) {
    kernel::test::mark_vfs_touched();
    if (!syscall_is_user_task())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #52: a thread shares its address space — exec would swap the
    // shared root out from under its siblings.  Fail closed (threads exit
    // and the group leader re-execs, or spawn a fresh process).
    auto *exec_task = syscall_task();
    if (exec_task != nullptr && exec_task->is_thread())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    vfs::Vnode *vn = nullptr;
    char path_buf[SYSCALL_MAX_PATH];
#if CONFIG_PROC_MGR
    // Issue #564 F11 Stage B: verdict after the early presence gates
    // above and before the first filesystem touch. Validation, read and
    // elf::exec_into_current stay untouched; the id is preserved and the
    // hold is untouched (INV-PROC-1 deny-only).
    if (proc_verdict_denied(
            ask_proc_verdict(ProcMgrOp::EXEC, arg0, arg1, arg2))) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
#endif
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (!strncpy_from_user(path_buf, reinterpret_cast<const char *>(arg0),
                           SYSCALL_MAX_PATH))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    vn = vfs::resolve(path_buf);
    if (!vn)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *argv = reinterpret_cast<const char *const *>(arg1);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *envp = reinterpret_cast<const char *const *>(arg2);
    // VULN-H4/W1 + VULN-U2: validate both arrays with hard bounds and
    // accumulate the combined string length for the stack-reservation check.
    uint64_t str_total = 0;
    if (!validate_argv_envp(argv, true, &str_total)) {
        vfs::release(vn); // issue #268: vn already owned here
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    if (!validate_argv_envp(envp, true, &str_total)) {
        vfs::release(vn); // issue #268
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    if (vn->size == 0 || vn->size > 512_KiB) {
        vfs::release(vn); // issue #268
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    size_t file_pages = (static_cast<size_t>(vn->size) + 4095) / 4096;
    uint64_t file_phys = PMM::alloc_contiguous(file_pages);
    if (!file_phys) {
        vfs::release(vn); // issue #268
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *file_buf =
        reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + file_phys);
    uint64_t want_size = vn->size; // capture before release below
    int64_t r = vn->ops->read(*vn, file_buf, want_size, 0);
    vfs::release(vn); // issue #268: image is buffered, vnode unneeded below
    if (r <= 0 || static_cast<uint64_t>(r) != want_size) {
        for (size_t i = 0; i < file_pages; ++i)
            PMM::free_page(file_phys + i * 4096);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    auto *hdr = reinterpret_cast<const elf::ELF64Header *>(file_buf);
    // VULN-H2: pass the actual bytes read so validate_segment bounds
    // phdr->offset+filesz against the real file, not a constant.
    if (!elf::exec_into_current(hdr, file_buf, argv, envp, regs,
                                static_cast<uint64_t>(r))) {
        for (size_t i = 0; i < file_pages; ++i)
            PMM::free_page(file_phys + i * 4096);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    for (size_t i = 0; i < file_pages; ++i)
        PMM::free_page(file_phys + i * 4096);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t ProcSvc::kill(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                       uint64_t *) {
    uint64_t target_pid = arg0;
    uint64_t sig = arg1;
    if (sig >= MAX_SIGNAL_HANDLERS)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // SIG_NONE (signal 0) is the null signal — POSIX existence check, no
    // delivery
    if (sig == static_cast<uint64_t>(kernel::Signal::SIG_NONE))
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    // Issue #374: fail fast on never-issued IDs without probing.
    if (!Scheduler::validate_id(target_pid))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    auto *t = Scheduler::find_task(target_pid);
    if (!t)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // Issue #564 F11 Stage D D3 (INV-PROC-15): self-fatal kill bypasses
    // any proc-mgr verdict/RPC and terminates via the trap funnel; all
    // other kill paths (self non-fatal, other-target) consult the verdict.
    // Verdict never blocks suicide; fatal classification stays kernel-owned.
    const bool self_fatal =
        (t == syscall_task() &&
         (signal_is_fatal(sig) || !t->has_signal_handler(sig)));
    if (!self_fatal) {
#if CONFIG_PROC_MGR
        // Issue #564 F11 Stage B (Q-B2 deny-only, INV-PROC-11): the proxy
        // supplies deny vs allow only — it never reclassifies fatal vs
        // handler. Delivery stays pending_signals + the #593 trap-funnel leg
        // in the invoking client's context below, and the gate is called as
        // the client (the unchanged parent_id branch decides).
        if (proc_verdict_denied(
                ask_proc_verdict(ProcMgrOp::KILL, target_pid, sig, 0))) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
#endif
    }
    if (t == syscall_task()) {
        if (self_fatal) {
            // Issue #593: the self-fatal leg runs the trap funnel
            // (terminate_err_from_trap) instead of marking TERMINATED
            // inline — the funnel dequeues, marks, zombie-lists, then wakes
            // the waitpid parent, and the arm below is the trap-return gate
            // on all three arches (x86 isr_stubs.asm depth-1 epilogue for
            // int 0x80; aarch64 syscall_entry.S applies on any non-zero
            // arm; riscv64 applies via the force_apply bit that
            // switch_away_from_terminating sets).  terminate_err is
            // deliberately not used (INV-593-1): its scope-B switch_to_task
            // resolves the live ISR RSP as the save owner (see sys_exit's
            // warning in syscall_handlers_misc.cpp).  Refusal is impossible:
            // t == current (mirrors the arch EL0-fault legs).
            const errors::SchedulerError trap_term_err =
                Scheduler::terminate_err_from_trap(
                    *t,
                    static_cast<uint64_t>(-static_cast<int64_t>(sig)),
                    signal_is_fault_class(sig)
                        ? kernel::ipc::DeathCause::Fault
                        : kernel::ipc::DeathCause::Kill);
            (void)trap_term_err;
            Scheduler::switch_away_from_terminating(*t);
        } else {
            t->pending_signals |= (1ULL << sig);
        }
    } else {
        t->pending_signals |= (1ULL << sig);
        // Issue #278 (owner review): a wheel-blocked nanosleep parks
        // with no other wake source — deliver the wake here or the
        // signal sleeps until wheel expiry. Sleepers only (other wait
        // channels re-check on their own wakeups); set_task_ready is
        // deferral-safe for wake-while-current.
        if (t->state == TaskState::BLOCKED && t->sleep_armed)
            Scheduler::set_task_ready(*t);
    }
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t ProcSvc::signal(uint64_t arg0, uint64_t arg1, uint64_t, uint64_t,
                         uint64_t *) {
    auto *t = syscall_task();
    if (!t)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t sig = arg0;
    if (sig >= MAX_SIGNAL_HANDLERS)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    if (signal_is_fatal(sig))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // SIG_NONE cannot be caught or ignored
    if (sig == static_cast<uint64_t>(kernel::Signal::SIG_NONE))
        return static_cast<uint64_t>(errors::SYS_ERR_OK);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto handler = reinterpret_cast<sighandler_t>(arg1);
#if CONFIG_PROC_MGR
    // Issue #564 F11 Stage B: handler-table decision per-TCB (deny-only);
    // installation below is unchanged mechanism.
    if (proc_verdict_denied(
            ask_proc_verdict(ProcMgrOp::SIGNAL, sig, arg1, 0))) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
#endif
    t->set_signal_handler(sig, handler);
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t ProcSvc::sigreturn(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t *regs) {    auto *t = syscall_task();
    // Issue #564 F11 Stage B: policy/mechanism split — the trap-frame
    // restore below is mechanism only, so the proxy is not on this path;
    // the handler table it restores is proc-mgr-owned via signal()
    // (deny-only, INV-PROC-11).
    if (!t || !regs)
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    uint64_t user_rsp = regs[20];
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<const SignalFrame *>(
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        reinterpret_cast<uint64_t *>(user_rsp));
    auto chk = checked(frame, 1);
    if (!chk.valid())
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    // MP-4 (SMAP): copy the frame to a kernel local (stac-wrapped) instead of
    // dereferencing user memory directly.
    kernel::SignalFrame kf{};
    if (!safe_copy_from_user(&kf, frame, 1))
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    regs[17] = kf.saved_rip;
    regs[20] = kf.saved_rsp;
    regs[19] = kf.saved_rflags;
    regs[18] = kf.saved_cs;
    regs[21] = kf.saved_ss;
    regs[0] = 0;
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

uint64_t ProcSvc::getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *) {
    // Issue #564 F11 Stage B: O(1) read — no RPC on this path (INV-PROC-2
    // holds trivially). Writer discipline: the fork path (alloc_id in
    // clone) is the only producer of a new identity and exec preserves
    // it, so no PD-side replica exists to diverge; GETPID stays a FAST
    // member with no lock, no IPC, no allocation.
    auto *t = syscall_task();
    return t ? t->id : 0;
}

errors::ProcError
ProcSvc::handle_decision(const ProcMgrRpc &decision) noexcept {
    // Stage B verdict validator: reserved fields must be zero (R-INPUT)
    // and the op must name a served op. STOP/RESUME carry a disposition
    // that stays refused (STOP_REFUSED): Stage B verdicts are deny-only
    // (INV-PROC-11) and kill is enforced in invoking-client context via
    // the unchanged Q9 parent_id branch (INV-PROC-6) — the PD never owns
    // a stop disposition, so no verdict can grant one.
    if (decision.rsvd != 0 || decision.rsvd3 != 0) {
        return errors::PROC_ERR_BAD_REQUEST;
    }
    switch (static_cast<ProcMgrOp>(decision.op)) {
    case ProcMgrOp::BIND:
    case ProcMgrOp::LOOPBACK:
    case ProcMgrOp::FORK:
    case ProcMgrOp::EXEC:
    case ProcMgrOp::WAITPID:
    case ProcMgrOp::KILL:
    case ProcMgrOp::GETPID:
    case ProcMgrOp::SIGNAL:
    case ProcMgrOp::SIGRETURN:
        return errors::PROC_ERR_OK;
    case ProcMgrOp::STOP:
    case ProcMgrOp::RESUME:
        return errors::PROC_ERR_STOP_REFUSED;
    }
    return errors::PROC_ERR_BAD_REQUEST;
}

} // namespace kernel::proc
