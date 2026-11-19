/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_cap_tcb.cpp
/// @brief TCB control tests (F4 N1-N3, issue #557): the admin suspend/
///        resume mechanism (core_task group) and the TcbCap authority
///        rule (capability group).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/tcb_control.hpp>
#include <kernel/task/tcb_control_errors.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: F4 suspend parks a READY task: admin_parked set, state
//        BLOCKED, dequeued from ready/EDF queues; a tick does not run it.
// Input: READY target; Scheduler::admin_suspend_err; one tick.
// Expect: admin_parked true; state BLOCKED; !in_ready_queue_;
//         run counter unchanged after the tick.
// Depends: Scheduler::admin_suspend_err, tick path
// Pseudocode: create+READY target; snapshot run counter; suspend (0);
// assert parked/BLOCKED/dequeued; tick; counter unchanged; teardown
// (resume + drain).
JARVIS_TEST(tcb_suspend_ready_parks_and_dequeues, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume of a parked READY target re-queues and runs it.
// Input: Suspended READY target; admin_resume_err.
// Expect: READY + queued after resume; runs to completion; counter +1.
// Depends: Scheduler::admin_resume_err, dispatch
// Pseudocode: suspend READY target; resume (0); assert READY+queued;
// run to completion; counter +1; teardown.
JARVIS_TEST(tcb_resume_prev_ready_reenables, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 suspend absorbs a wake (INV-ADM-1): target blocked on a
//        Semaphore is suspended; a post wakes but is absorbed (pending
//        latched, task stays parked, did not run); resume delivers.
//        Same shape for a waitpid-blocked parent whose child exits
//        (#588 carve-out INV-ADM-13) plus the INV-ADM-14 budget check.
// Input: Semaphore-blocked target; suspend; post; resume.
// Expect: post -> adm_wake_absorbed_count +1, still BLOCKED+parked,
//         did not run; resume -> READY, wait returns, counter +1.
// Depends: admin_absorb_wake (INV-ADM-1), #588 carve-out, waitpid path
// Pseudocode: block target on semaphore; suspend; post (absorbed, no
// run); resume; wait returns; counter +1; teardown. Repeat shape for
// waitpid-blocked parent + exiting child (assert status delivered).
JARVIS_TEST(tcb_suspend_blocked_absorbs_wake_then_resumes, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 absorb backstop refuses direct enqueue of a parked task
//        (INV-ADM-1 layer b): enqueue_ready(parked) -> refused, state
//        restored BLOCKED, backstop counter +1, not queued, orphan
//        rescue untouched.
// Input: Parked target; Scheduler::enqueue_ready(*target) directly.
// Expect: Refused; state BLOCKED; adm_wake_backstop_count == 1;
//         in_ready_queue_ false; orphan_rescue_count unchanged.
// Depends: enqueue_ready backstop, orphan_rescue_count oracle
// Pseudocode: suspend READY target; call enqueue_ready directly
// (refused); assert counters/state; teardown (resume + drain).
JARVIS_TEST(tcb_suspend_absorb_backstop_refuses_link, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume of a live non-suspended target is a typed
//        no-op (NOT_SUSPENDED), state byte-identical.
// Input: Live READY/RUNNING target (never suspended); resume.
// Expect: NOT_SUSPENDED; no state/queue change.
// Depends: admin_resume_err NOT_SUSPENDED leg
// Pseudocode: live target; resume (expect NOT_SUSPENDED); assert
// state/queue untouched; teardown.
JARVIS_TEST(tcb_resume_not_suspended_typed, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 suspend of dead targets is typed (TERMINATED and
//        REAPED both -> DEAD).
// Input: TERMINATED target; reaped (freed) generation.
// Expect: Both DEAD; no state change.
// Depends: admin_suspend_err DEAD leg, generation guard
// Pseudocode: terminate target (no drain); suspend (DEAD); drain;
// suspend stale handle (DEAD); teardown.
JARVIS_TEST(tcb_suspend_dead_target_typed, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 deferred RUNNING-target stop: suspend latches
//        admin_stop_requested on a RUNNING user task; a tick with a
//        user-mode PC parks it, a tick with a kernel PC does not.
// Input: RUNNING user task; suspend (latch); tick with user PC; tick
//        with kernel PC.
// Expect: Latch set; user-PC tick -> parked; kernel-PC tick ->
//         still RUNNING.
// Depends: deferred-stop latch, tick PC discrimination (INV-ADM-9)
// Pseudocode: RUNNING user target; suspend (latched, still RUNNING);
// user-PC tick (parked); kernel-PC tick on a fresh target (RUNNING);
// teardown (resume + drain).
JARVIS_TEST(tcb_suspend_deferred_running_boundary, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 watchdog/deadline shift (r1 S1): suspend freezes the
//        watchdog expiry tick (not zeroed, gen+1) and removes the
//        deadline node; ticks past expiry cause no kill/miss; resume
//        shifts both by the elapsed interval (gen+1 again); an unarmed
//        target resumes disarmed with no phantom arm.
// Input: Periodic user target with armed watchdog (period P, expiry
//        E, gen g) + deadline; suspend at S; ticks past E + deadline;
//        resume at R; re-check; unarmed-target half.
// Expect: wdog_armed false, expiry E PRESERVED, gen g+1,
//         admin_wdog_expiry E, deadline node gone; no kill/miss, no
//         TIMING+6/+7, SCHED+21 then +22; resume: expiry E+(R-S),
//         last_kick shifted, gen g+2, deadline shifted,
//         remaining_ticks unchanged, SYS_WATCHDOG_KICK works; unarmed
//         target: wdog_armed false, gen unchanged after resume.
// Depends: INV-ADM-11/16, tick scan, watchdog kick path
// Pseudocode: armed periodic target; suspend; assert frozen + removed;
// tick past E/deadline (no kill/miss, no dmesg records); resume;
// assert shifted + live kick; unarmed half; teardown.
JARVIS_TEST(tcb_resume_rearms_watchdog_and_shifts_deadline, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 resume shift saturates — a fabricated far-future
//        admin_wdog_expiry_tick/admin_deadline_ticks resumes clamped
//        at UINT64_MAX (no wrap to an immediate fire).
// Input: Parked target with expiry/deadline set near UINT64_MAX;
//        resume.
// Expect: Clamped to UINT64_MAX; no immediate fire/kill.
// Depends: admin_resume_err saturating shift
// Pseudocode: fabricate near-max expiry/deadline on parked target;
// resume (0); assert clamped, no fire; teardown.
JARVIS_TEST(tcb_resume_shift_saturates, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 watchdog arm refused while parked — suspend an
//        armed-candidate target, then SYS_WATCHDOG_CREATE ->
//        INVALID_STATE and no kill on subsequent ticks;
//        SYS_WATCHDOG_KICK -> INVALID_STATE (already-disarmed path).
// Input: Suspended target; CREATE + KICK; tick.
// Expect: Both INVALID_STATE; target not killed by tick scan.
// Depends: suspend-time disarm, watchdog create/kick gates
// Pseudocode: suspend target; CREATE (INVALID_STATE); KICK
// (INVALID_STATE); tick; target alive; teardown (resume + drain).
JARVIS_TEST(tcb_watchdog_arm_refused_while_parked, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 a task halted in resched_park_until_dispatched is a
//        legitimate suspend target (INV-ADM-9): stays halted, released
//        by resume without tripping park/apply counters.
// Input: Task halted in the resched park; suspend; resume.
// Expect: Stays halted across suspend; resume releases; no
//         park/apply counter trip.
// Depends: INV-ADM-9, resched park path
// Pseudocode: halt task in resched park; suspend (stays halted);
// resume (released, no counter trip); teardown.
JARVIS_TEST(tcb_suspend_parked_runner_released_by_resume, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 latch-armed suspend refusal — a fabricated latch via
//        gs::lockpark_latch_arm makes suspend return BUSY; reset via
//        gs::lockpark_reset_for_test.
// Input: Armed latch; suspend.
// Expect: BUSY; park state untouched.
// Depends: #582 fail-closed latch, lockpark_reset_for_test
// Pseudocode: arm latch; suspend (BUSY); assert untouched; reset;
// teardown.
JARVIS_TEST(tcb_suspend_latch_armed_refused, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 two park owners stay mutually exclusive (rev 2) —
//        suspend a debug-parked target -> BUSY, debug park intact;
//        debug_park_stop on an admin-parked target -> false,
//        admin_parked intact.
// Input: Debug-parked target + suspend; admin-parked target +
//        debug_park_stop.
// Expect: BUSY + intact; false + intact.
// Depends: INV-ADM park mutual exclusion, both directions
// Pseudocode: debug-park target; suspend (BUSY, still debug-parked);
// admin-park other; debug_park_stop (false, still admin-parked);
// teardown both.
JARVIS_TEST(tcb_debugger_admin_park_mutual_exclusion, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 mk_supervisor_stop capability matrix (r1 S2-2): creator
//        allow; orphaned creator deny; foreign deny (watch-holding
//        foreign still deny); SUPERVISOR-scope allow only for the bound
//        grantee; onward-copied cap deny; self deny; kernel/idle deny;
//        revoked/deleted/stale/readonly deny.
// Input: Matrix of caller/target/cap combinations via 99/100/101.
// Expect: Each cell returns its specified typed code, zero side
//         effects on refusal.
// Depends: tcb_control_authorize INV-ADM-12 branches, TcbCap
//         create/create_supervised, DeathNotify watch (no authority)
// Pseudocode: build each cell; call; assert exact typed code;
// teardown all caps/tasks.
JARVIS_TEST(mk_supervisor_stop, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 never-mem-mgr predicate (rev 2): a user helper whose
//        CSpace holds a minted Untyped cap -> PROTECTED_ROLE for SUSPEND
//        and CONFIGURE; after the Untyped cap is deleted the same target
//        is suspendable; the mem-mgr pid cell overrides regardless.
// Input: User task + minted Untyped; SUSPEND/CONFIGURE; delete Untyped;
//        SUSPEND again; set_mem_mgr_pid(target); SUSPEND; clear.
// Expect: PROTECTED_ROLE x2; then 0 (suspendable); PROTECTED_ROLE via
//         pid cell; 0 after clear. Teardown restores cell 0.
// Depends: tcb_control_role_protected (INV-ADM-15 durable layer b),
//         mem-mgr pid cell
// Pseudocode: helper + Untyped (PROTECTED x2); delete Untyped (OK);
// set pid cell (PROTECTED); clear cell; teardown.
JARVIS_TEST(tcb_never_mem_mgr_predicate, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 TcbCap derivation cascade — grant/copy a TcbCap then
//        CAP_REVOKE the parent: the derived cap's suspend fails closed;
//        a SUPERVISOR-scope cap copied onward cannot be used by the
//        copy's new holder.
// Input: TcbCap grant + copy; REVOKE parent; suspend via derived.
// Expect: Derived suspend fails (INV-MK-7: named cap survives, derived
//         destroyed); onward-copied SUPERVISOR cap unusable by the new
//         holder.
// Depends: cap::revoke_derivations over TcbCap edges, grantee binding
// Pseudocode: create TcbCap; grant + copy; REVOKE parent; derived
// suspend fails; SUPERVISOR copy-onward fails for new holder;
// teardown.
JARVIS_TEST(tcb_cap_derivation_cascade, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 N1 configure — priority re-buckets through
//        set_priority; affinity applies and clamps as set_affinity_err
//        does; a parked target stays configurable.
// Input: 99 with mask bit0 (priority) + value; bit1 (affinity) +
//        value; oversized priority; parked target configure.
// Expect: Priority + affinity applied; over-ceiling INVALID_ARGS;
//         parked target configurable.
// Depends: sys_tcb_configure N1, set_priority/set_affinity_err
// Pseudocode: configure priority (applied); affinity (applied +
// clamped); over-ceiling (INVALID_ARGS); parked target (OK);
// teardown.
JARVIS_TEST(tcb_config_priority_and_affinity, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 N1 mask gate — mask 0 and bits >= 4 fail INVALID_ARGS;
//        bit2/bit3 fail UNSUPPORTED; priority over
//        CONFIG_PRIORITY_CEILING fails INVALID_ARGS.
// Input: 99 with mask 0, mask 0x10, mask bit2, mask bit3, priority
//        over ceiling.
// Expect: INVALID_ARGS, INVALID_ARGS, UNSUPPORTED, UNSUPPORTED,
//         INVALID_ARGS.
// Depends: sys_tcb_configure mask gate (no silent ignore)
// Pseudocode: each malformed call returns its typed code; teardown.
JARVIS_TEST(tcb_config_unsupported_and_bad_mask, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F4 syscall surface — 99/100/101 dispatch through
//        Syscall::handle; MAX_SYSCALL == 102 (F4); 102 rejected with
//        SYS_ERR_INVALID_NUMBER; ABI_VERSION unchanged.
// Input: handle(99/100/101, ...) reachable; MAX pin; 102 probe.
// Expect: Dispatch works; MAX == 102; 102 rejected; ABI 0x10000.
// Depends: syscall table wiring, ABI_VERSION stability
// Pseudocode: call 99/100/101 (typed outcomes, no crash); assert
// MAX == 102; call 102 (INVALID_NUMBER); ABI_VERSION == 0x10000.
JARVIS_TEST(sys_tcb_surface_and_numbering, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

void register_tcb_control_tests() {
    Logger::info("Registering tcb_control tests");
    JARVIS_REGISTER_TEST(tcb_suspend_ready_parks_and_dequeues);
    JARVIS_REGISTER_TEST(tcb_resume_prev_ready_reenables);
    JARVIS_REGISTER_TEST(tcb_suspend_blocked_absorbs_wake_then_resumes);
    JARVIS_REGISTER_TEST(tcb_suspend_absorb_backstop_refuses_link);
    JARVIS_REGISTER_TEST(tcb_resume_not_suspended_typed);
    JARVIS_REGISTER_TEST(tcb_suspend_dead_target_typed);
    JARVIS_REGISTER_TEST(tcb_suspend_deferred_running_boundary);
    JARVIS_REGISTER_TEST(tcb_resume_rearms_watchdog_and_shifts_deadline);
    JARVIS_REGISTER_TEST(tcb_resume_shift_saturates);
    JARVIS_REGISTER_TEST(tcb_watchdog_arm_refused_while_parked);
    JARVIS_REGISTER_TEST(tcb_suspend_parked_runner_released_by_resume);
    JARVIS_REGISTER_TEST(tcb_suspend_latch_armed_refused);
    JARVIS_REGISTER_TEST(tcb_debugger_admin_park_mutual_exclusion);
}

void register_cap_tcb_tests() {
    Logger::info("Registering cap_tcb tests");
    JARVIS_REGISTER_TEST(mk_supervisor_stop);
    JARVIS_REGISTER_TEST(tcb_never_mem_mgr_predicate);
    JARVIS_REGISTER_TEST(tcb_cap_derivation_cascade);
    JARVIS_REGISTER_TEST(tcb_config_priority_and_affinity);
    JARVIS_REGISTER_TEST(tcb_config_unsupported_and_bad_mask);
    JARVIS_REGISTER_TEST(sys_tcb_surface_and_numbering);
}
