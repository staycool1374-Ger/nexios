/*
 * NexIOS RTOS — SMP bring-up (Phase 5)
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

/// @file test_load_balancer.cpp
/// @brief RT load-balancer tests (issue #61; stubs from issue #85,
///        module 8).  Scheduler::balancer_tick() migrates queued
///        aperiodic kernel tasks from the busiest up-CPU to the idlest
///        past BALANCER_THRESHOLD, re-pinning toward balance.  Tests
///        branch on up_cpu_count(): with an AP up (standard -smp 2
///        harness) moves are observable; single-CPU runs prove the
///        no-strand property (no moves off CPU0).

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/irq_guard.hpp>

using namespace kernel;

namespace {
// Register + queue a BLOCKED kernel task and widen its mask.  Must run
// inside the test's single IrqGuard window (cookbook Rule 2): an
// untimely tick would dispatch past — or drop, via next_task()'s
// defense-in-depth dequeue — a BLOCKED head, breaking determinism.
TaskControlBlock *make_queued_task(TaskControlBlock *t) {
    t->state = TaskState::BLOCKED;
    Scheduler::register_task(*t);
    Scheduler::enqueue_ready(*t);
    Scheduler::set_affinity(*t, 0x3);
    return t;
}

// Teardown for a never-dispatched BLOCKED test task (affinity-test
// pattern: remove + cleanup + free; no terminate — it never ran).
void destroy_queued_task(TaskControlBlock *t) {
    Scheduler::remove_task(*t);
    t->cleanup();
    MemPool::free(t);
}

uint64_t total_migrations() {
    uint64_t total = 0;
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c)
        total += Scheduler::migration_count(c);
    return total;
}

} // namespace

// Runmode: kernel
// Testidea: An idle CPU pulls a task from a busy CPU's queue.
// Input: Three queued tasks on CPU0, CPU1 idle, balancer tick runs.
// Expect: Either no move (all three stay on 0, counters unchanged —
//         single up CPU, nothing may strand) or exactly one move
//         (counters +1, the moved task re-pinned 0x2 and queued on
//         1, the rest on 0).  Per-task placement only: absolute
//         depths include background tasks in `all` runs.
// Depends: Scheduler::balancer_tick (issue #61)
JARVIS_TEST(load_balancer_idle_pull, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    auto *b = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    auto *c = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(a != nullptr && b != nullptr && c != nullptr);
    {
        arch::IrqGuard irq_guard{};
        make_queued_task(a);
        make_queued_task(b);
        make_queued_task(c);
        uint64_t before = total_migrations();
        Scheduler::balancer_tick();
        uint64_t after = total_migrations();
        TaskControlBlock *all[3] = {a, b, c};
        uint64_t on_zero = 0;
        uint64_t on_one = 0;
        for (auto *t : all) {
            bool q0 = Scheduler::is_queued_on(*t, 0);
            bool q1 = Scheduler::is_queued_on(*t, 1);
            JARVIS_ASSERT(q0 != q1);
            if (q1) {
                ++on_one;
                JARVIS_ASSERT(Scheduler::get_affinity(*t) == 0x2);
            } else {
                ++on_zero;
            }
        }
        if (after == before) {
            JARVIS_ASSERT(on_zero == 3 && on_one == 0);
        } else {
            JARVIS_ASSERT(after == before + 1);
            JARVIS_ASSERT(on_zero == 2 && on_one == 1);
        }
    }
    destroy_queued_task(a);
    destroy_queued_task(b);
    destroy_queued_task(c);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An overloaded CPU pushes tasks to an idle CPU, bounded by
//           the per-tick migration cap.
// Input: CPU0 overloaded (5 queued), CPU1 idle, one balancer tick.
// Expect: Either no move (all five stay on 0) or exactly the cap
//         (counters +2, three on 0 and two re-pinned on 1).
//         Per-task placement only (see idle_pull above).
// Depends: Scheduler::balancer_tick (issue #61)
JARVIS_TEST(load_balancer_work_push, "PRE: none | POST: none") {
    TaskControlBlock *tasks[5];
    for (auto &t : tasks) {
        t = TaskControlBlock::create([]() {}, 10,
                                     TaskControlBlock::NO_PERIOD);
        JARVIS_ASSERT(t != nullptr);
    }
    {
        arch::IrqGuard irq_guard{};
        for (auto *t : tasks)
            make_queued_task(t);
        uint64_t before = total_migrations();
        Scheduler::balancer_tick();
        uint64_t after = total_migrations();
        uint64_t on_zero = 0;
        uint64_t on_one = 0;
        for (auto *t : tasks) {
            bool q0 = Scheduler::is_queued_on(*t, 0);
            bool q1 = Scheduler::is_queued_on(*t, 1);
            JARVIS_ASSERT(q0 != q1);
            if (q1)
                ++on_one;
            else
                ++on_zero;
        }
        if (after == before) {
            JARVIS_ASSERT(on_zero == 5 && on_one == 0);
        } else {
            JARVIS_ASSERT(after == before + 2);
            JARVIS_ASSERT(on_zero == 3 && on_one == 2);
        }
    }
    for (auto *t : tasks)
        destroy_queued_task(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Real-time (periodic) tasks never migrate between CPUs.
// Input: Periodic RT task + overload on CPU0, balancer ticks run.
// Expect: RT task stays queued on CPU0 with its mask intact across
//         N ticks, whatever the balancer does around it.
// Depends: Scheduler::balancer_tick, is_rt_task (issue #61)
JARVIS_TEST(load_balancer_no_rt_migration, "PRE: none | POST: none") {
    auto *rt = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(rt != nullptr);
    JARVIS_ASSERT(Scheduler::is_rt_task(*rt));
    TaskControlBlock *tasks[3];
    for (auto &t : tasks) {
        t = TaskControlBlock::create([]() {}, 10,
                                     TaskControlBlock::NO_PERIOD);
        JARVIS_ASSERT(t != nullptr);
        JARVIS_ASSERT(!Scheduler::is_rt_task(*t));
    }
    {
        arch::IrqGuard irq_guard{};
        make_queued_task(rt);
        for (auto *t : tasks)
            make_queued_task(t);
        for (int i = 0; i < 3; ++i) {
            Scheduler::balancer_tick();
            JARVIS_ASSERT(Scheduler::is_queued_on(*rt, 0));
            JARVIS_ASSERT(!Scheduler::is_queued_on(*rt, 1));
        }
        JARVIS_ASSERT(Scheduler::is_queued_on(*rt, 0));
    }
    destroy_queued_task(rt);
    for (auto *t : tasks)
        destroy_queued_task(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Migration threshold prevents thrashing: queues already
//           within threshold trigger no migration.
// Input: Two tasks both landing on CPU0 (diff vs empty CPU1 is 2,
//        not beyond threshold), balancer tick runs.
// Expect: Zero migrations, every task exactly where it was queued.
// Depends: Scheduler::balancer_tick (issue #61)
JARVIS_TEST(load_balancer_threshold_holds, "PRE: none | POST: none") {
    auto *a = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(a != nullptr);
    auto *b = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(b != nullptr);
    {
        arch::IrqGuard irq_guard{};
        make_queued_task(a);
        make_queued_task(b);
        uint64_t before = total_migrations();
        Scheduler::balancer_tick();
        JARVIS_ASSERT(total_migrations() == before);
        JARVIS_ASSERT(Scheduler::is_queued_on(*a, 0));
        JARVIS_ASSERT(Scheduler::is_queued_on(*b, 0));
    }
    destroy_queued_task(a);
    destroy_queued_task(b);
    JARVIS_TEST_PASS();
}

void register_load_balancer_tests() {
    Logger::info("Registering load balancer tests");
    JARVIS_REGISTER_TEST(load_balancer_idle_pull);
    JARVIS_REGISTER_TEST(load_balancer_work_push);
    JARVIS_REGISTER_TEST(load_balancer_no_rt_migration);
    JARVIS_REGISTER_TEST(load_balancer_threshold_holds);
}
#endif  // CONFIG_ARCH_X86_64
