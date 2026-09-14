/*
 * NexIOS RTOS — SMP Phase C1 affinity
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

/// @file test_sched_affinity.cpp
/// @brief CPU affinity tests (issue #25, Phase C1).
///        Affinity is a bitmask (bit N = may run on CPU N) with lowest-
///        set-bit targeting and no balancing in C1.  All tests run on
///        the BSP on every variant (single-CPU safe: cross-CPU targets
///        direct-enqueue under the lock when no AP is up, so placement
///        is observable without a second CPU).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/irq_guard.hpp>

using namespace kernel;

namespace {
// Destroy a test task exactly once: terminate only if still live
// (a dispatched test task self-terminates via its trampoline), then
// drain.  Unconditional double-terminate corrupts zombie accounting.
void destroy_test_task(TaskControlBlock *t) {
    uint64_t id = t->id;
    if (Scheduler::find_task(id) != nullptr &&
        t->state != TaskState::TERMINATED && t->state != TaskState::REAPED)
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
}

} // namespace


// Runmode: kernel
// Testidea: Fresh tasks default to CPU0 affinity (all pre-C1 behavior
//           unchanged) and land in CPU0's queue on add.
// Input: create() (no set_affinity), add_task().
// Expect: cpu_affinity == 0x1; queued on 0, not on 1.
// Depends: TCB ctor default, Scheduler::add_task targeting
// Note (issue #26): enqueue + placement asserts run under one IrqGuard
// (cookbook Rule 2) — a timer tick between add_task and is_queued_on
// would dispatch the fresh task and the placement assert would fail.
JARVIS_TEST(sched_affinity_default_mask, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    arch::IrqGuard irq_guard{};
    Scheduler::add_task(*t);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    JARVIS_ASSERT(!Scheduler::is_queued_on(*t, 1));
    uint64_t id = t->id;
    destroy_test_task(t);
    JARVIS_ASSERT(Scheduler::find_task(id) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Lowest-set-bit targeting is deterministic, clamped to up
//           CPUs: on the single-CPU default variant any remote bit
//           clamps to CPU0 (cross-CPU placement is covered in smp_sched
//           under smp2).
// Input: add_task(), set_affinity(0x2), set_affinity(0x5).
// Expect: Both clamp to 0x1, queued on 0 both times.
// Depends: Scheduler::set_affinity clamp, is_queued_on
// Note (issue #26): whole enqueue/affinity/assert sequence under one
// IrqGuard (cookbook Rule 2) — a tick after enqueue_ready would dispatch
// the task and the placement asserts would fail.
JARVIS_TEST(sched_affinity_lowest_bit_targets, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    arch::IrqGuard irq_guard{};
    Scheduler::register_task(*t);
    Scheduler::enqueue_ready(*t);
    // Single-CPU config (this class runs on the default variant): only
    // CPU0 is up, so any remote bit clamps to CPU0 (cross-CPU placement
    // is covered in smp_sched under smp2).
    Scheduler::set_affinity(*t, 0x2);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    JARVIS_ASSERT(!Scheduler::is_queued_on(*t, 1));
    Scheduler::set_affinity(*t, 0x5);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    Scheduler::remove_task(*t);
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An empty mask cannot strand a task (fail-closed clamp to
//           CPU0 with the field actually updated).
// Input: set_affinity(t, 0).
// Expect: cpu_affinity == 0x1 afterwards.
// Depends: Scheduler::set_affinity clamp
JARVIS_TEST(sched_affinity_empty_clamp, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::set_affinity(*t, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: User tasks are forced to CPU0 (shared-TSS limitation):
//           even an explicit AP mask does not stick.
// Input: create_user(), set_affinity(..., 0x2).
// Expect: cpu_affinity == 0x1 afterwards.
// Depends: Scheduler::set_affinity user clamp
JARVIS_TEST(sched_affinity_user_clamp, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::set_affinity(*t, 0x2);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1), t->cpu_affinity);
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mask operations preserve placement on single-CPU (clamped
//           masks keep the task queued exactly once on CPU0 — no loss,
//           no duplication). Cross-CPU moves are covered in smp_sched.
// Input: set_affinity(0x1), set_affinity(0x3) (clamps to 0x1).
// Expect: Still queued on 0 only, mask 0x1.
// Depends: Scheduler::set_affinity clamp path
// Note (issue #26): same cookbook-Rule-2 guard as above — placement
// asserts are only stable with ticks excluded.
JARVIS_TEST(sched_affinity_requeue_moves, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(t != nullptr);
    t->state = TaskState::BLOCKED;
    arch::IrqGuard irq_guard{};
    Scheduler::register_task(*t);
    Scheduler::enqueue_ready(*t);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    // Same-CPU mask change re-queues without duplication or loss
    // (cross-CPU moves are covered in smp_sched under smp2).
    Scheduler::set_affinity(*t, 0x1);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    Scheduler::set_affinity(*t, 0x3);
    JARVIS_ASSERT(Scheduler::is_queued_on(*t, 0));
    Scheduler::remove_task(*t);
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: is_idle_task recognizes the BSP idle (and only real idles):
//           fresh tasks and nullptr are not idle.
// Input: get_idle_task(), fresh TCB, nullptr.
// Expect: idle true; others false.
// Depends: Scheduler::is_idle_task, get_idle_task
JARVIS_TEST(sched_affinity_is_idle, "PRE: none | POST: none") {
    auto *idle = Scheduler::get_idle_task();
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT(Scheduler::is_idle_task(idle));
    auto *t = TaskControlBlock::create([]() {}, 10, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(!Scheduler::is_idle_task(t));
    JARVIS_ASSERT(!Scheduler::is_idle_task(nullptr));
    t->cleanup();
    MemPool::free(t);
    JARVIS_TEST_PASS();
}

void register_sched_affinity_tests() {
    Logger::info("Registering sched affinity tests");
    JARVIS_REGISTER_TEST(sched_affinity_default_mask);
    JARVIS_REGISTER_TEST(sched_affinity_lowest_bit_targets);
    JARVIS_REGISTER_TEST(sched_affinity_empty_clamp);
    JARVIS_REGISTER_TEST(sched_affinity_user_clamp);
    JARVIS_REGISTER_TEST(sched_affinity_requeue_moves);
    JARVIS_REGISTER_TEST(sched_affinity_is_idle);
}
