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

/// @file syscall_handlers_thread.cpp
/// @brief THREAD_CREATE(93) handler (issue #52): spawn a thread sharing
/// an address space.  TCB-per-thread scheduler integration — the child is
/// a first-class schedulable task sharing the owner's root PML4 with a
/// private window stack (see TaskControlBlock::spawn_thread).
///
/// Authority model (audit S2-1): self-spawn (arg0 == 0) is capless —
/// same authority as FORK.  Cross-task spawn (arg0 = pid) requires the
/// caller to hold a live Task capability carrying the SHARE right (never
/// implied by WRITE); the handle is the authority token, the pid selects
/// the validated target.  Target-bound ThreadCap (WdogCap-style) is a
/// documented follow-up; v1 has no production SHARE minters, so the
/// cross-task path is fail-closed by construction.  Stays out of
/// k_syscall_fast[] (page-table-touching, issue #92 discipline).

#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/syscall/syscall_helpers.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/kernel.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/thread_cap.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/timer.hpp>

// Saved user-stack pointer accessor (mirrors syscall_handlers_misc.cpp:
// x86_64 kernel_stack_top is the trap frame base; aarch64 sp_el0;
// riscv64 sp).  Used only for the waitpid-style RAX override on wake.
#if defined(CONFIG_ARCH_X86_64)
#define THREAD_JOIN_STACK_PTR(t) ((t)->context.rsp)
#elif defined(CONFIG_ARCH_AARCH64)
#define THREAD_JOIN_STACK_PTR(t) ((t)->context.sp_el0)
#elif defined(CONFIG_ARCH_RISCV64)
#define THREAD_JOIN_STACK_PTR(t) ((t)->context.sp)
#endif

namespace kernel {

namespace {
// JOIN arg1 selector (issue #53): 0 = wait for termination and collect
// the exit status, 1 = detach the thread so it auto-reaps on death.
constexpr uint64_t kThreadJoinWait = 0;
constexpr uint64_t kThreadJoinDetach = 1;

// Bound for the join-cycle walk (S1(a)): a cycle back to the caller fails
// closed instead of deadlocking.  64 comfortably exceeds the group-table
// capacity (16) while staying bounded.
constexpr uint64_t kThreadJoinWalkBound = 64;
} // namespace

// Reap a claimed TERMINATED thread: unlink from the parent (if any),
// tear down (own stack, group slot, root on last member), unregister and
// free.  Mirrors waitpid's inline reap exactly (remove_child + cleanup +
// remove_task + pool-aware delete).
void thread_join_reap(TaskControlBlock &target) noexcept {
    if (target.parent_id != 0) {
        auto *parent = Scheduler::find_task(target.parent_id);
        if (parent != nullptr) {
            parent->remove_child(&target);
        }
        target.parent_id = 0;
    }
    target.cleanup();
    Scheduler::remove_task(target);
    // Pool-backed member operator delete (routes to MemPool::free):
    // plain-identifier form, mirroring waitpid's `delete child;`.
    TaskControlBlock *reaped = &target;
    delete reaped;
}

uint64_t Syscall::sys_thread_create(uint64_t arg0, uint64_t arg1,
                                     uint64_t arg2, uint64_t arg3,
                                     uint64_t *regs) {
    auto *caller = syscall_task();
    if (caller == nullptr || !caller->is_user_) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    TaskControlBlock *owner = nullptr;
    if (arg0 == 0) {
        // Self-spawn: reserved handle must be zero (fail-closed).
        if (arg3 != 0) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        owner = caller;
    } else {
        // Cross-task spawn: a live (Task, SHARE) authority token is
        // mandatory; no token type is ever implicitly sufficient.
        if (arg3 == 0) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        caller->ensure_cspace();
        cap::CNode *cspace = caller->get_cspace();
        if (cspace == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        KernelObject *token = cap::lookup(cspace, arg3, cap::CapType::Task,
                                          cap::CapRights::SHARE);
        if (token == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        token->release();
        // Issue #374: fail fast on never-issued IDs without probing.
        if (!Scheduler::validate_id(arg0)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        auto *target = Scheduler::find_task(arg0);
        if (!TaskControlBlock::is_valid(target)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        owner = target;
    }
    // Issue #53 (option Y): 5th-register birth context (issue #317
    // mechanism). An opaque word planted in the child's first C argument
    // register (rdi/x0/a0) so a plain C trampoline receives it; 0 keeps
    // the legacy zeroed frame. MAPPED-certified up front so a garbage
    // pointer fails here instead of faulting the child on first dispatch.
    // The return contract (tid) is unchanged — #52 tests unaffected.
    uint64_t entry_arg = syscall_arg4(regs);
    if (entry_arg != 0 &&
        VMM::virt_to_phys_in_pml4(entry_arg, owner->get_page_table()) == 0) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    TaskControlBlock *thread =
        TaskControlBlock::spawn_thread(*owner, arg1, arg2, entry_arg);
    if (thread == nullptr) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // Issue #20 precedent (sys_fork): admission gate before the thread is
    // observable; on denial destroy (never registered, no zombie).
    if (Scheduler::add_task_err(*thread) != errors::SCHED_ERR_OK) {
        TaskControlBlock::destroy(thread);
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    return thread->id;
}

uint64_t Syscall::sys_thread_exit(uint64_t arg0, uint64_t, uint64_t, uint64_t,
                                  uint64_t *) {
    auto *t = syscall_task();
    // Threads only (issue #53 S1(b)): processes exit via sys_exit, kernel
    // tasks never exit here.  Self-only by construction (the caller
    // terminates itself); never returns.
    if (t == nullptr || !t->is_user_ || !t->is_thread()) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    t->state = TaskState::TERMINATED;
    t->exit_code = arg0;
    report_user_task_end(*t, true, arg0, 0);
    // Join rendezvous: hand the status to a registered joiner before the
    // task leaves the CPU (the joiner may be anywhere).  No-op when no
    // joiner is registered.
    TaskControlBlock::thread_join_note_termination(*t);
    if (Scheduler::current_task() == t) {
        Scheduler::switch_away_from_terminating(*t);
    } else {
        // Defensive mirror of sys_exit (a self-exit is always current):
        // refuse cross-CPU teardown instead of corrupting live context.
        const errors::SchedulerError exit_err =
            Scheduler::terminate_err(*t, arg0);
        if (exit_err != errors::SCHED_ERR_OK) {
            return static_cast<uint64_t>(errors::from_sched_error(
                static_cast<uint64_t>(exit_err)));
        }
    }

    if (t->first_child) {
        TaskControlBlock *init_task = Scheduler::task_at(0);
        if (init_task) {
            auto *child = t->first_child;
            while (child) {
                auto *next = child->next_sibling;
                t->remove_child(child);
                init_task->add_child(child);
                child = next;
            }
        }
    }

    if (t->parent_id) {
        uint64_t count = Scheduler::task_count();
        for (uint64_t i = 0; i < count; ++i) {
            auto *p = Scheduler::task_at(i);
            // Waiter match mirrors sys_exit (own pid or the -1 wildcard).
            if (p && p->id == t->parent_id &&
                (p->waiting_child_pid == t->id ||
                 p->waiting_child_pid == static_cast<uint64_t>(-1))) {
                if (p->waiting_child_status) {
                    uint64_t old_cr3 = 0;
                    bool switched = false;
                    if (p->get_page_table() &&
                        arch::read_cr3() != p->get_page_table()) {
                        old_cr3 = arch::read_cr3();
                        arch::write_cr3(p->get_page_table());
                        switched = true;
                    }
                    arch::stac();
                    *p->waiting_child_status = t->exit_code;
                    arch::clac();
                    if (switched)
                        arch::write_cr3(old_cr3);
                    p->waiting_child_status = nullptr;
                }
                p->waiting_child_pid = 0;
                // Issue #53: a thread with a registered joiner stays
                // parented (the joiner reaps it); a detached thread orphans
                // like a regular task so the reaper auto-reaps it.
                bool keep_parented = false;
                if (__atomic_load_n(&t->join_waiter_, __ATOMIC_ACQUIRE) !=
                    nullptr) {
                    keep_parented = true;
                } else if (t->is_thread() && !t->join_detached_) {
                    keep_parented = true;
                }
                if (!keep_parented) {
                    p->remove_child(t);
                    t->parent_id = 0;
                }
                if (p->state != TaskState::TERMINATED) {
                    Scheduler::set_task_ready(*p);
                    // NOLINTNEXTLINE(performance-no-int-to-ptr)
                    uint64_t *frame = reinterpret_cast<uint64_t *>(
                        THREAD_JOIN_STACK_PTR(p));
                    if (frame) {
#if defined(CONFIG_ARCH_RISCV64)
                        frame[9] = t->id;
#else
                        frame[0] = t->id;
#endif
                    }
                }
                break;
            }
        }
    }
    return static_cast<uint64_t>(errors::SYS_ERR_OK);
}

/// @brief Resolves a ThreadCap handle to its live target TCB (issue #53,
/// option Y). Freshness per call: re-lookup the handle every pass so a
/// revoked cap or a reaped target fails closed instead of using a stale
/// pointer. Returns nullptr when the handle is not a ThreadCap, the cap
/// was revoked, or the target is gone/stale. TERMINATED targets resolve
/// fine (EXIT-before-JOIN collects them); only truly gone targets fail.
TaskControlBlock *thread_join_resolve(TaskControlBlock &cur,
                                      uint64_t handle) noexcept {
    cur.ensure_cspace();
    cap::CNode *cspace = cur.get_cspace();
    if (cspace == nullptr) {
        return nullptr;
    }
    KernelObject *obj = cap::lookup(cspace, handle, cap::CapType::Task,
                                    cap::CapRights::READ);
    if (obj == nullptr) {
        return nullptr;
    }
    auto *cap = static_cast<cap::ThreadCap *>(obj);
    const bool tagged = (cap->tag == cap::ThreadCap::kThreadCapTag);
    const uint64_t tid = cap->target_id;
    const uint32_t tgen = cap->target_gen;
    obj->release();
    // ThreadCap is the only Task-typed object v1 can mint, but the tag
    // keeps that invariant checked instead of assumed.
    if (!tagged) {
        return nullptr;
    }
    if (!Scheduler::validate_id(tid)) {
        return nullptr;
    }
    auto *target = Scheduler::find_task(tid);
    if (target == nullptr || !TaskControlBlock::is_valid(target) ||
        !target->is_thread()) {
        return nullptr;
    }
    // Generation match: ABA guard on TCB slot reuse (ids are never
    // reused in-boot today; the check holds regardless).
    if (target->generation != tgen) {
        return nullptr;
    }
    return target;
}

uint64_t Syscall::sys_thread_wrap(uint64_t arg0, uint64_t arg1, uint64_t,
                                  uint64_t, uint64_t *) {
    auto *cur = syscall_task();
    // User tasks only (mirrors sys_thread_create's gate).
    if (cur == nullptr || !cur->is_user_) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // Target must be a live thread (self-wrap allowed; self-join still
    // fails closed in THREAD_JOIN).
    if (!Scheduler::validate_id(arg0)) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    auto *target = Scheduler::find_task(arg0);
    if (target == nullptr || !TaskControlBlock::is_valid(target) ||
        !target->is_thread()) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    const uint32_t target_gen = target->generation;
    // Authority (issue #53 S1(a)): wrapping your own thread (child or
    // self) is capless — same authority as FORK. Same-address-space
    // siblings may wrap each other (a group member already shares all
    // of the target's memory, so no authority crosses a domain here).
    // Wrapping a foreign thread requires a live (Task, SHARE) token,
    // mirroring sys_thread_create's arg3; v1 has no SHARE minters, so
    // foreign wraps fail closed by construction.
    bool authorized = (target->id == cur->id);
    if (!authorized && target->parent_id == cur->id) {
        authorized = true;
    }
    if (!authorized && target->thread_group_ != 0 &&
        target->thread_group_ == cur->thread_group_) {
        authorized = true;
    }
    if (!authorized && arg1 != 0) {
        cur->ensure_cspace();
        cap::CNode *cspace = cur->get_cspace();
        if (cspace == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        KernelObject *token =
            cap::lookup(cspace, arg1, cap::CapType::Task,
                        cap::CapRights::SHARE);
        if (token == nullptr) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        token->release();
        // Re-validate liveness after the lookup (no TOCTOU on the id:
        // a reap in between unregisters the id, so a match means live).
        if (!Scheduler::validate_id(arg0)) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        target = Scheduler::find_task(arg0);
        if (target == nullptr || !TaskControlBlock::is_valid(target) ||
            !target->is_thread() || target->generation != target_gen) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        authorized = true;
    }
    if (!authorized) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // Mint the wrapper and install it into the caller's CSpace with READ
    // (join authority; READ implies nothing about SPAWN — cross-task
    // CREATE still demands SHARE).
    cap::ThreadCap *cap = cap::ThreadCap::create(target->id, target_gen);
    if (cap == nullptr) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    cur->ensure_cspace();
    cap::CNode *cspace = cur->get_cspace();
    if (cspace == nullptr) {
        cap->release();  // never installed — drop the creator reference
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    uint32_t gen = 0;
    const int idx =
        cspace->install(cap, cap::CapType::Task, cap::CapRights::READ, &gen);
    if (idx < 0) {
        cap->release();  // never installed — drop the creator reference
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    const uint64_t handle = cap::encode_user_handle(cspace->cspace_id, idx, gen);
    cap->release();
    return handle;
}

uint64_t Syscall::sys_thread_join(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                  uint64_t arg3, uint64_t *) {
    auto *cur = syscall_task();
    // User tasks only (mirrors sys_thread_create's gate); kernel tasks
    // never join.
    if (cur == nullptr || !cur->is_user_) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    const bool detaching = (arg1 == kThreadJoinDetach);
    if (arg1 != kThreadJoinWait && arg1 != kThreadJoinDetach) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // arg3 is reserved (option Y): the cap handle in arg0 is the whole
    // authority (validated at WRAP time as child-or-self or SHARE token),
    // so no second token is taken.  Non-zero fails closed.
    if (arg3 != 0) {
        return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
    }
    // Validate the status out-pointer up front for JOIN (waitpid mirror:
    // pre-certify MAPPED so the in-trap write cannot fault).  Detach
    // never touches it.
    uint64_t *status_ptr = nullptr;
    if (!detaching && arg2 != 0) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto status = checked(reinterpret_cast<uint64_t *>(arg2),
                              static_cast<size_t>(1));
        if (!status.valid() ||
            VMM::virt_to_phys_in_pml4(
                reinterpret_cast<uint64_t>(status.unsafe_ptr()),
                cur->get_page_table()) == 0) {
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        status_ptr = status.unsafe_ptr();
    }

    for (;;) {
        // Resolve the target from the cap handle every pass (thread_join_
        // resolve re-looks-up the handle: a revoked cap or a reaped target
        // fails closed instead of using a stale pointer).  Holding the
        // handle is the whole authority (option Y, S1(a)).
        auto *target = thread_join_resolve(*cur, arg0);
        if (target == nullptr) {
            // Gone (cap revoked, target reaped) or never a ThreadCap: if
            // our earlier handoff fired, the status was delivered — report
            // it.
            if (__atomic_load_n(&cur->join_ready_, __ATOMIC_ACQUIRE)) {
                uint64_t status = __atomic_load_n(&cur->join_status_,
                                                  __ATOMIC_ACQUIRE);
                if (status_ptr != nullptr) {
                    if (!safe_copy_to_user(status_ptr, &status, 1)) {
                        return static_cast<uint64_t>(
                            errors::SYS_ERR_GENERIC_FAIL);
                    }
                }
                __atomic_store_n(&cur->join_ready_, false, __ATOMIC_RELEASE);
                __atomic_store_n(&cur->join_target_id_, uint64_t{0},
                                 __ATOMIC_RELEASE);
                return static_cast<uint64_t>(errors::SYS_ERR_OK);
            }
            cur->thread_join_unregister();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        const uint64_t target_id = target->id;
        // Self-join always fails closed (a thread may detach itself, but
        // never join itself).
        if (!detaching && target_id == cur->id) {
            cur->thread_join_unregister();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        // Detached threads cannot be joined; joining a thread twice fails
        // closed (the slot names the single joiner).
        auto *slot = __atomic_load_n(&target->join_waiter_, __ATOMIC_ACQUIRE);
        if (!detaching &&
            __atomic_load_n(&target->join_detached_, __ATOMIC_ACQUIRE)) {
            cur->thread_join_unregister();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        if (!detaching && slot != nullptr && slot != cur) {
            cur->thread_join_unregister();
            return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
        }
        if (detaching) {
            // Detach: claim the TERMINATED thread and reap it now, or mark
            // a running thread detached (auto-reap on death).  A registered
            // joiner wins over detach (fail closed).  Never blocks, never
            // touches the status pointer.
            if (target->state == TaskState::TERMINATED) {
                if (!TaskControlBlock::thread_join_try_claim(*target)) {
                    cur->thread_join_unregister();
                    return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
                }
                thread_join_reap(*target);
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_OK);
            }
            if (slot != nullptr) {
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            bool expected = false;
            if (!__atomic_compare_exchange_n(&target->join_detached_,
                                            &expected, true, false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            cur->thread_join_unregister();
            return static_cast<uint64_t>(errors::SYS_ERR_OK);
        }
        // Join cycle check (S1(a)): walk the wait-for chain; reaching the
        // caller (or the walk bound) fails closed instead of deadlocking.
        {
            bool cycle = false;
            uint64_t walk_id = target_id;
            for (uint64_t depth = 0; depth <= kThreadJoinWalkBound; ++depth) {
                if (depth == kThreadJoinWalkBound) {
                    cycle = true;
                    break;
                }
                auto *w = Scheduler::find_task(walk_id);
                if (w == nullptr || !TaskControlBlock::is_valid(w)) {
                    break;
                }
                uint64_t next = __atomic_load_n(&w->join_target_id_,
                                               __ATOMIC_ACQUIRE);
                if (next == 0) {
                    break;
                }
                if (next == cur->id) {
                    cycle = true;
                    break;
                }
                walk_id = next;
            }
            if (cycle) {
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
        }
        if (target->state == TaskState::TERMINATED) {
            // EXIT-before-JOIN: collect immediately (no blocking).  Claim
            // first (serializes concurrent collectors), write the status
            // (retryable: the claim is released on copy failure), then
            // reap and report.
            if (!TaskControlBlock::thread_join_try_claim(*target)) {
                // Lost the claim race: fall through to the handoff latch
                // below.
            } else if (status_ptr != nullptr &&
                       !safe_copy_to_user(status_ptr, &target->exit_code,
                                          1)) {
                __atomic_store_n(&target->join_reaped_, false,
                                 __ATOMIC_RELEASE);
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            } else {
                uint64_t code = target->exit_code;
                if (status_ptr != nullptr) {
                    if (!safe_copy_to_user(status_ptr, &code, 1)) {
                        __atomic_store_n(&target->join_reaped_, false,
                                         __ATOMIC_RELEASE);
                        cur->thread_join_unregister();
                        return static_cast<uint64_t>(
                            errors::SYS_ERR_GENERIC_FAIL);
                    }
                }
                thread_join_reap(*target);
                __atomic_store_n(&cur->join_target_id_, uint64_t{0},
                                 __ATOMIC_RELEASE);
                return static_cast<uint64_t>(errors::SYS_ERR_OK);
            }
        }
        if (__atomic_load_n(&cur->join_ready_, __ATOMIC_ACQUIRE)) {
            // Handoff fired (target TERMINATED and possibly already
            // reaped): report the latched status without touching the
            // target.
            uint64_t status = __atomic_load_n(&cur->join_status_,
                                              __ATOMIC_ACQUIRE);
            if (status_ptr != nullptr) {
                if (!safe_copy_to_user(status_ptr, &status, 1)) {
                    return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
                }
            }
            __atomic_store_n(&cur->join_ready_, false, __ATOMIC_RELEASE);
            __atomic_store_n(&cur->join_target_id_, uint64_t{0},
                             __ATOMIC_RELEASE);
            return static_cast<uint64_t>(errors::SYS_ERR_OK);
        }
        // Register as the joiner (CAS: exactly one joiner wins) unless
        // already registered from a previous pass.  The slot write is
        // preceded by a find_task identity re-check: a reap in between
        // would have unregistered the id, so a match means live.
        if (slot == nullptr) {
            if (Scheduler::find_task(target_id) != target) {
                cur->thread_join_unregister();
                continue;
            }
            auto *expected = slot;
            if (!__atomic_compare_exchange_n(&target->join_waiter_, &expected,
                                            cur, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                // Lost the race (double join): fail closed.
                cur->thread_join_unregister();
                return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);
            }
            __atomic_store_n(&cur->join_target_id_, target_id,
                             __ATOMIC_RELEASE);
        }
        // Block in-trap until woken (mirror waitpid's hlt loop: ticks and
        // the handoff wake rescan; loss-wakeup-free by re-check).
        Scheduler::dequeue_ready(*cur);
        cur->state = TaskState::BLOCKED;
        Scheduler::reschedule();
        while (cur->state == TaskState::BLOCKED &&
               __atomic_load_n(&cur->join_target_id_, __ATOMIC_ACQUIRE) !=
                   0) {
            arch::sti();
            arch::hlt();
            arch::cli();
        }
    }
}

} // namespace kernel
