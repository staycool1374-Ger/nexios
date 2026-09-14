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
#include <kernel/sync/semaphore.hpp>
#include <kernel/memory/mempool.hpp>

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

void register_smp_sched_tests() {
    Logger::info("Registering smp sched tests");
    JARVIS_REGISTER_TEST(smp_sched_ap_runs_pinned);
    JARVIS_REGISTER_TEST(smp_sched_ipi_wake);
    JARVIS_REGISTER_TEST(smp_sched_bsp_unaffected);
    JARVIS_REGISTER_TEST(smp_sched_cross_move);
}
#endif // CONFIG_ARCH_X86_64
