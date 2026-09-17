/*
 * NexIOS RTOS — SMP Phase C1 AP scheduling
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

/// @file test_smp_sched.cpp
/// @brief AP task execution tests (issue #25, Phase C1).
///        An AP-pinned kernel task must dispatch on the AP (proven by the
///        CPU index it observes), self-terminate through the genuine exit
///        path, and leave the AP parked on its idle — while BSP tasks run
///        unaffected.  A semaphore rendezvous exercises the cross-CPU
///        mailbox+IPI wake path end to end.  All tests pass trivially with
///        0 APs up (default single-CPU variant); the AP paths run under
///        the smp2 variant.  Teardown order is rigid: poll done, poll AP
///        back on idle, THEN drain (freeing a task the AP still runs on
///        trips the corruption detector).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/memory/mempool.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Shared rendezvous (file-static: no allocator involvement).
struct ApRendezvous {
    volatile uint64_t done = 0;
    volatile uint64_t ran_on = 0xFFFFFFFF;
    volatile uint64_t task_id = 0;
};

ApRendezvous g_ap_run{};
ApRendezvous g_ap_ipi{};
ApRendezvous g_bsp_run{};
kernel::sync::Semaphore g_ap_sem;

/// @brief TSC-bounded poll (fail fast with a clear message instead of
///        hanging the suite when the AP never rendezvous).
bool poll_until(volatile uint64_t &flag, uint64_t value, uint64_t ms) {
    uint64_t freq = arch::Timer::tsc_freq_hz();
    uint64_t start = arch::rdtsc();
    uint64_t budget = (freq / 1000000ULL) * (ms * 1000ULL);
    while (flag != value) {
        if (freq != 0 && arch::rdtsc() - start > budget)
            return false;
        asm volatile("pause");
    }
    return true;
}

/// @brief True when the AP is parked back on an idle task.
bool ap_parked_on_idle(uint64_t ms) {
    uint64_t freq = arch::Timer::tsc_freq_hz();
    uint64_t start = arch::rdtsc();
    uint64_t budget = (freq / 1000000ULL) * (ms * 1000ULL);
    for (;;) {
        TaskControlBlock *cur = Scheduler::current_on_cpu(1);
        if (cur && Scheduler::is_idle_task(cur))
            return true;
        if (freq != 0 && arch::rdtsc() - start > budget)
            return false;
        asm volatile("pause");
    }
}

void ap_worker_entry() {
    g_ap_run.ran_on = arch::cpu_index();
    g_ap_run.done = 1;
    Scheduler::terminate(*Scheduler::current_task(), 0);
    for (;;) {
        arch::hlt();
    }
}

void ap_ipi_worker_entry() {
    g_ap_ipi.ran_on = arch::cpu_index();
    g_ap_sem.wait();
    g_ap_ipi.done = 1;
    Scheduler::terminate(*Scheduler::current_task(), 0);
    for (;;) {
        arch::hlt();
    }
}

void bsp_worker_entry() {
    g_bsp_run.ran_on = arch::cpu_index();
    g_bsp_run.done = 1;
    Scheduler::terminate(*Scheduler::current_task(), 0);
    for (;;) {
        arch::hlt();
    }
}

void placement_probe_entry() {
    for (;;) {
        arch::hlt();
    }
}

// Issue #23: semaphore-gated entry for partitioned-admission tests —
// admitted tasks block on first dispatch (never self-terminate mid-setup,
// never burn the CPU), keeping LUB scans and queue placement stable.
void smp_gate_entry() {
    auto *self = Scheduler::current_task();
    auto *gate = reinterpret_cast<sync::Semaphore *>(self->user_data);
    gate->wait();
}

} // namespace

// Runmode: kernel
// Testidea: A task pinned to CPU1 dispatches on the AP (not the BSP):
//           it records the CPU it observes, flags completion, and exits
//           through the genuine terminate path.  The AP must be parked
//           back on idle before the zombie is drained (else the corruption
//           detector trips on a freed current).
// Input: create + set_affinity(0x2) + add_task; poll done (2 s), poll
//        AP-on-idle (2 s), drain, verify removal.
// Expect: 0 APs: trivial pass. 1 AP: done==1, ran_on==1, AP on idle,
//         task gone after drain.
// Depends: affinity targeting, AP tick dispatch, switch_away on AP,
//         per-CPU current observation
JARVIS_TEST(smp_sched_ap_runs_pinned, "PRE: iocd | POST: none") {
    if (smp::ap_count() == 0) {
        // JARVIS_TEST_PASS() records only (no return) — return explicitly
        // or the body runs clamped to CPU0 below and fails ran_on == 1.
        JARVIS_TEST_PASS();
        return;
    }
    g_ap_run.done = 0;
    g_ap_run.ran_on = 0xFFFFFFFF;
    auto *t = TaskControlBlock::create(ap_worker_entry, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    g_ap_run.task_id = t->id;
    Scheduler::set_affinity(*t, 0x2);
    Scheduler::add_task(*t);
    JARVIS_ASSERT(poll_until(g_ap_run.done, 1, 2000));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_ap_run.ran_on);
    JARVIS_ASSERT(ap_parked_on_idle(2000));
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::find_task(g_ap_run.task_id) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A BSP sem_post() to a waiter pinned on the AP travels the
//           cross-CPU mailbox+IPI path: the AP drains in its IPI handler,
//           dispatches on its next tick, and the waiter runs to completion.
// Input: create + set_affinity(0x2) + add_task (blocks in wait on AP);
//        poll BLOCKED, sem_post, poll done, poll AP-on-idle, drain.
// Expect: 0 APs: trivial pass. 1 AP: waiter ran_on==1, done==1, AP on
//         idle afterwards, task gone after drain.
// Depends: mailbox_publish/drain, SCHED IPI, semaphore wait/post
JARVIS_TEST(smp_sched_ipi_wake, "PRE: iocd | POST: none") {
    if (smp::ap_count() == 0) {
        // (see ap_runs_pinned: JARVIS_TEST_PASS() records only — return.)
        JARVIS_TEST_PASS();
        return;
    }
    g_ap_sem.init(0);
    g_ap_ipi.done = 0;
    g_ap_ipi.ran_on = 0xFFFFFFFF;
    auto *t = TaskControlBlock::create(ap_ipi_worker_entry, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    g_ap_ipi.task_id = t->id;
    Scheduler::set_affinity(*t, 0x2);
    Scheduler::add_task(*t);
    // Wait until the worker blocks in sem_wait (bounded; then it can only
    // be woken through the mailbox+IPI path under test).
    {
        uint64_t freq = arch::Timer::tsc_freq_hz();
        uint64_t start = arch::rdtsc();
        uint64_t budget = (freq / 1000000ULL) * 2000000ULL;
        while (t->state != TaskState::BLOCKED) {
            if (t->state == TaskState::TERMINATED)
                break;
            if (freq != 0 && arch::rdtsc() - start > budget)
                break;
            asm volatile("pause");
        }
        JARVIS_ASSERT(t->state == TaskState::BLOCKED);
    }
    g_ap_sem.post();
    JARVIS_ASSERT(poll_until(g_ap_ipi.done, 1, 2000));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_ap_ipi.ran_on);
    JARVIS_ASSERT(ap_parked_on_idle(2000));
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::find_task(g_ap_ipi.task_id) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: BSP-side scheduling is unaffected by AP bring-up: a CPU0
//           task dispatches on the BSP and completes while the AP idles
//           (or works) beside it.  Runs on every variant (no AP needed).
// Input: create (default affinity) + add_task; poll done; drain.
// Expect: done==1, ran_on==0, task gone after drain.
// Depends: default affinity, BSP dispatch path intact under C1
JARVIS_TEST(smp_sched_bsp_unaffected, "PRE: iocd | POST: none") {
    g_bsp_run.done = 0;
    g_bsp_run.ran_on = 0xFFFFFFFF;
    auto *t = TaskControlBlock::create(bsp_worker_entry, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    uint64_t id = t->id;
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    Scheduler::add_task(*t);
    JARVIS_ASSERT(poll_until(g_bsp_run.done, 1, 2000));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_bsp_run.ran_on);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT(Scheduler::find_task(id) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Changing a queued BLOCKED task's mask moves it between CPU
//           queues deterministically (nothing can dispatch a BLOCKED
//           task, so placement is stable across ticks on both CPUs).
// Input: register + BLOCKED + set_affinity(0x2) + enqueue (lands [1]);
//        set_affinity(0x3) (lowest bit 0 → moves to [0]).
// Expect: 0 APs: clamp path (stays [0]). 1 AP: [1] then [0], never both.
// Depends: Scheduler::set_affinity re-queue path, is_queued_on
JARVIS_TEST(smp_sched_cross_move, "PRE: iocd | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    Scheduler::set_affinity(*t, 0x2);
    Scheduler::enqueue_ready(*t);
    if (smp::ap_count() == 0) {
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
        JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    } else {
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x2), t->cpu_affinity);
        JARVIS_ASSERT(Scheduler::is_queued_on(*t, 1));
        JARVIS_ASSERT(!Scheduler::is_queued_on(*t, 0));
        Scheduler::set_affinity(*t, 0x3);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x3), t->cpu_affinity);
        JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
        JARVIS_ASSERT(!Scheduler::is_queued_on(*t, 1));
    }
    Scheduler::remove_task(*t);
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Queue placement routes deterministically per mask: tasks
//           pinned 0x1 land on [0] exclusively, tasks pinned 0x2 land
//           on [1] exclusively (clamped to [0] with 0 APs).  Setup,
//           assert and teardown run PER TASK, never batched: next_task()
//           dequeue-drops queued-but-BLOCKED occupants on every tick, so
//           a batched window spanning several PMM allocations lets a
//           tick scavenge earlier probes before they are asserted (the
//           per-task window matches smp_sched_cross_move's).  Masks are
//           asserted too (cpu_affinity is tick-stable, unlike queue
//           membership).
// Input: 4x (create + BLOCKED + register + pin [0x1,0x1,0x2,0x2] +
//        enqueue + assert + remove + cleanup + free), one task live.
// Expect: Per task: affinity == pinned-or-clamped mask;
//         is_queued_on(expected) && !is_queued_on(other).
// Depends: Scheduler::set_affinity clamp + re-queue path, is_queued_on
JARVIS_TEST(smp_sched_queue_placement_fanout, "PRE: iocd | POST: none") {
    constexpr uint64_t kMasks[4] = {0x1, 0x1, 0x2, 0x2};
    bool has_ap = smp::ap_count() > 0;
    for (uint64_t i = 0; i < 4; ++i) {
        TaskControlBlock *probe = TaskControlBlock::create(
            placement_probe_entry, 10, TaskControlBlock::NO_PERIOD);
        JARVIS_ASSERT(probe != nullptr);
        probe->state = TaskState::BLOCKED;
        Scheduler::register_task(*probe);
        Scheduler::set_affinity(*probe, kMasks[i]);
        uint64_t want = 0;
        uint64_t want_mask = 0x1;
        if (has_ap && kMasks[i] == 0x2) {
            want = 1;
            want_mask = 0x2;
        }
        JARVIS_ASSERT_EQ(want_mask, probe->cpu_affinity);
        Scheduler::enqueue_ready(*probe);
        JARVIS_ASSERT(Scheduler::is_queued_on(*probe, want));
        JARVIS_ASSERT(!Scheduler::is_queued_on(*probe, 1 - want));
        Scheduler::remove_task(*probe);
        probe->cleanup();
        MemPool::free(probe);
    }
    JARVIS_TEST_PASS();
}

// Issue #23: partitioned-admission test helpers — gated entries keep
// admitted tasks alive across setup windows (never self-terminate on a
// tick the way empty lambdas do); placement asserts run under one
// IrqGuard (cookbook Rule 2).
static TaskControlBlock *smp_make_task(uint64_t wcet, uint64_t period,
                                       sync::Semaphore *gate) {
    auto *t = TaskControlBlock::create(smp_gate_entry, 11, period);
    if (t == nullptr)
        return nullptr;
    t->wcet_ticks = wcet;
    t->user_data = gate;
    return t;
}

static void smp_destroy_denied(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    JARVIS_ASSERT(Scheduler::find_task(t->id) == nullptr);
    TaskControlBlock::destroy(t);
}

// Runmode: kernel
// Testidea: per-CPU bound denies an overloaded destination partition.
// Input: A(60%) pinned to CPU1 (CPU0 when no AP), then B(60%) same target.
// Expect: A admitted; B → SCHED_ERR_ADMISSION_DENIED with tables and
// ResourceTracker unchanged (destroy_denied asserts absence).
// Depends: admission_check_cpu_locked via add_task_err (issue #23)
JARVIS_TEST(smp_sched_percpu_add_denies_overloaded_cpu,
            "PRE: none | POST: none") {
    bool has_ap = smp::ap_count() > 0;
    uint64_t cpu1 = has_ap ? 0x2 : 0x1;
    sync::Semaphore gate;
    gate.init(0, 1);
    // NOTE: B is positioned BEFORE A is added — the void set_affinity
    // wrapper runs the same destination probe, so positioning onto an
    // already-full CPU is (correctly) refused with the old mask kept.
    auto *b = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(b != nullptr);
    Scheduler::set_affinity(*b, cpu1);
    auto *a = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(a != nullptr);
    Scheduler::set_affinity(*a, cpu1);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*a) == errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::add_task_err(*b) ==
                      errors::SCHED_ERR_ADMISSION_DENIED);
    }
    smp_destroy_denied(b);
    gate.post();
    kernel::test::terminate_and_drain(*a);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: affinity move into a saturated partition is denied and the
// source placement is fully retained (fail-closed migration gate).
// Input: M(implicit 100%) on CPU0 + F(60%) on CPU1; move M→CPU1.
// Expect (AP): DENIED, mask still 0x1, queued on 0 not 1. No AP:
// clamp no-op OK (masks collapse to CPU0 by design).
// Depends: Scheduler::set_affinity_err (issue #23)
JARVIS_TEST(smp_sched_set_affinity_denied_keeps_source,
            "PRE: none | POST: none") {
    bool has_ap = smp::ap_count() > 0;
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *m = smp_make_task(0, 100, &gate);
    JARVIS_ASSERT(m != nullptr);
    auto *f = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(f != nullptr);
    if (has_ap)
        Scheduler::set_affinity(*f, 0x2);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*m) == errors::SCHED_ERR_OK);
        errors::SchedulerError fr = Scheduler::add_task_err(*f);
        if (has_ap) {
            JARVIS_ASSERT(fr == errors::SCHED_ERR_OK);
        } else {
            // No AP: F collapses onto CPU0 (M implicit 100% + 60 > 82),
            // so the fill itself is denied here — the move leg below is
            // smp2-only (single-CPU clamp moves are covered in
            // sched_affinity_err_cpu0_ok).
            JARVIS_ASSERT(fr == errors::SCHED_ERR_ADMISSION_DENIED);
            smp_destroy_denied(f);
            f = nullptr;
        }
        if (has_ap) {
            errors::SchedulerError r =
                Scheduler::set_affinity_err(*m, 0x2);
            JARVIS_ASSERT(r == errors::SCHED_ERR_ADMISSION_DENIED);
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), m->cpu_affinity);
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                             Scheduler::queue_target(*m));
            JARVIS_ASSERT(Scheduler::is_queued_on(*m, 0));
            JARVIS_ASSERT(!Scheduler::is_queued_on(*m, 1));
        } else {
            JARVIS_ASSERT(Scheduler::set_affinity_err(*m, 0x1) ==
                          errors::SCHED_ERR_OK);
        }
    }
    gate.post();
    kernel::test::terminate_and_drain(*m);
    if (f != nullptr)
        kernel::test::terminate_and_drain(*f);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: exempt moves are always allowed, even into a saturated CPU.
// Input: saturate CPU1 (60%), add aperiodic task on CPU0, move it to CPU1.
// Expect: move returns OK, mask + queue follow the target on both
// variants (single-CPU collapses to a no-op OK).
// Depends: set_affinity_err exemption path (issue #23)
JARVIS_TEST(smp_sched_exempt_move_always_allowed,
            "PRE: none | POST: none") {
    bool has_ap = smp::ap_count() > 0;
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *fill = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(fill != nullptr);
    if (has_ap)
        Scheduler::set_affinity(*fill, 0x2);
    auto *ap = smp_make_task(0, 0, &gate);
    JARVIS_ASSERT(ap != nullptr);
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*fill) ==
                      errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::add_task_err(*ap) == errors::SCHED_ERR_OK);
        JARVIS_ASSERT(Scheduler::set_affinity_err(
                          *ap, has_ap ? 0x2 : 0x1) == errors::SCHED_ERR_OK);
        if (has_ap) {
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x2), ap->cpu_affinity);
            JARVIS_ASSERT(Scheduler::is_queued_on(*ap, 1));
        }
    }
    gate.post();
    gate.post();
    kernel::test::terminate_and_drain(*fill);
    kernel::test::terminate_and_drain(*ap);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: balancer_tick moves only non-RT tasks (RT never selected —
// the structural migration-safety property) and honors the destination
// probe for whatever it moves.
// Input: 1 periodic RT task + 4 aperiodic mask-0x3 tasks on CPU0.
// Expect (AP): migration_count[0] >= 1, RT still targets CPU0, every
// moved task targets CPU1. No AP: early return, counts unchanged.
// Depends: balancer_tick candidate filter + probe (issues #61/#23)
JARVIS_TEST(smp_sched_balancer_moves_nonrt_keeps_rt,
            "PRE: none | POST: none") {
    bool has_ap = smp::ap_count() > 0;
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *rt = smp_make_task(0, 10, &gate);
    JARVIS_ASSERT(rt != nullptr);
    TaskControlBlock *workers[4] = {};
    for (uint64_t i = 0; i < 4; ++i) {
        workers[i] = smp_make_task(0, 0, &gate);
        JARVIS_ASSERT(workers[i] != nullptr);
        if (has_ap)
            Scheduler::set_affinity(*workers[i], 0x3);
    }
    {
        arch::IrqGuard irq;
        Scheduler::add_task(*rt);
        for (uint64_t i = 0; i < 4; ++i)
            Scheduler::add_task(*workers[i]);
        Scheduler::reset_migration_counts();
        Scheduler::balancer_tick();
        if (!has_ap) {
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                             Scheduler::migration_count(0));
        } else {
            JARVIS_ASSERT(Scheduler::migration_count(0) >= 1);
            JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                             Scheduler::queue_target(*rt));
            uint64_t moved = 0;
            for (uint64_t i = 0; i < 4; ++i) {
                if (Scheduler::queue_target(*workers[i]) == 1)
                    ++moved;
            }
            JARVIS_ASSERT(moved >= 1);
        }
    }
    gate.post();
    kernel::test::terminate_and_drain(*rt);
    for (uint64_t i = 0; i < 4; ++i)
        kernel::test::terminate_and_drain(*workers[i]);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: partitioned admission diverges from the legacy global sum:
// system-wide 130% still admits when no single partition is overfull.
// Input: A(60%) CPU0 + B(60%) CPU1, then C(wcet 10) CPU0 (70<=82).
// Expect: all three admitted (single-CPU: B targets CPU0 and is denied
// instead — 120>82 — while A and C still admit; the partition key, not
// the global sum, decides in both cases).
// Depends: admission_check_cpu_locked partition key (issue #23)
JARVIS_TEST(smp_sched_percpu_partition_divergence,
            "PRE: none | POST: none") {
    bool has_ap = smp::ap_count() > 0;
    sync::Semaphore gate;
    gate.init(0, 1);
    auto *a = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(a != nullptr);
    auto *b = smp_make_task(60, 100, &gate);
    JARVIS_ASSERT(b != nullptr);
    auto *c = smp_make_task(10, 100, &gate);
    JARVIS_ASSERT(c != nullptr);
    if (has_ap) {
        Scheduler::set_affinity(*b, 0x2);
    }
    {
        arch::IrqGuard irq;
        JARVIS_ASSERT(Scheduler::add_task_err(*a) == errors::SCHED_ERR_OK);
        errors::SchedulerError rb = Scheduler::add_task_err(*b);
        if (has_ap) {
            JARVIS_ASSERT(rb == errors::SCHED_ERR_OK);
        } else {
            JARVIS_ASSERT(rb == errors::SCHED_ERR_ADMISSION_DENIED);
            smp_destroy_denied(b);
            b = nullptr;
        }
        JARVIS_ASSERT(Scheduler::add_task_err(*c) == errors::SCHED_ERR_OK);
    }
    gate.post();
    kernel::test::terminate_and_drain(*a);
    if (b != nullptr)
        kernel::test::terminate_and_drain(*b);
    kernel::test::terminate_and_drain(*c);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_smp_sched_tests() {
    Logger::info("Registering smp sched tests");
    JARVIS_REGISTER_TEST(smp_sched_ap_runs_pinned);
    JARVIS_REGISTER_TEST(smp_sched_ipi_wake);
    JARVIS_REGISTER_TEST(smp_sched_bsp_unaffected);
    JARVIS_REGISTER_TEST(smp_sched_cross_move);
    JARVIS_REGISTER_TEST(smp_sched_queue_placement_fanout);
    JARVIS_REGISTER_TEST(smp_sched_percpu_add_denies_overloaded_cpu);
    JARVIS_REGISTER_TEST(smp_sched_set_affinity_denied_keeps_source);
    JARVIS_REGISTER_TEST(smp_sched_exempt_move_always_allowed);
    JARVIS_REGISTER_TEST(smp_sched_balancer_moves_nonrt_keeps_rt);
    JARVIS_REGISTER_TEST(smp_sched_percpu_partition_divergence);
}
#endif // CONFIG_ARCH_X86_64
