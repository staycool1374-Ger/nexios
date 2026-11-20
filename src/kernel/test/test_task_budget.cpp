/*
 * NexIOS RTOS — Task Object Budget (M4/F10)
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

/// @file test_task_budget.cpp
/// @brief F10 object budget test (issue #563): attach_object budget
///        enforcement and detach_all_objects walk bound.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/cap/tcb_cap.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/nexios_config.h>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

namespace {

// Non-EDF driver harness (mirrors the TcbCap tests): a driver with
// an EDF deadline earlier than its target would starve the target,
// so the driver runs deadline-free like the harness itself.
TaskControlBlock *run_budget_task(void (*entry)(), uint64_t prio = 11,
                                  uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    t->deadline_ticks = 0;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

// Minimal pool-backed attach probe. TcbCap-sized objects live in the
// 32-block 128B pool (exactly enough to flake a 34-live-object
// test), so the budget test uses these 32B-pool probes instead —
// the budget mechanism is size-agnostic.
struct BudgetProbe : public KernelObject {
    void dispose() noexcept override {
        kernel::test::ResourceTracker::instance().track_cap_object_remove();
        MemPool::free(this);
    }
    bool is_shared() const noexcept override {
        return true;
    }
};

} // namespace

// Runmode: kernel
// Testidea: F10 object budget walk bound — set_object_budget clamps
//        (0 and sub-row values read back the 32 row default);
//        budget 33 allows exactly 33 attaches and refuses the 34th;
//        detach_all_objects() clears all 33 (a full re-attach
//        succeeds); tracker delta 0.
// Input: Fresh (never-added) holder task; 34 TcbCaps on the driver.
// Expect: effective 33; 33 attaches true + 34th false; re-attach
//         33 true; delta 0; budget 0/4 -> effective 32.
// Depends: set_object_budget/effective_object_budget,
//        attach_object/detach_all_objects
// Pseudocode: holder = create (no add); budget 33 (effective 33);
// attach x34 (33 ok, 34th refused); detach_all; re-attach x33
// (all ok); release all; delta 0; budget 0 and 4 read 32;
// add + terminate + drain holder.
JARVIS_TEST(object_budget_walk_bound, "PRE: none | POST: none") {
    static uint64_t g_eff = 0;
    static uint64_t g_attach = 0;
    static uint64_t g_refused = 0;
    static uint64_t g_reattach = 0;
    static uint64_t g_default0 = 0;
    static uint64_t g_default4 = 0;
    static uint64_t g_minted = 0;
    static uint64_t g_holder = 0;
    g_eff = g_attach = g_refused = g_reattach = 0;
    g_default0 = g_default4 = g_minted = g_holder = 0;

    auto *t = run_budget_task([]() {
        (void)Scheduler::current_task();
        // Holder: created, never added (the documented attach
        // window is pre-add). Caps target the live driver.
        auto *holder = TaskControlBlock::create(
            []() {
                for (;;)
                    arch::hlt();
            },
            20, 10);
        if (!holder)
            return;
        g_holder = 1;
        holder->set_object_budget(33);
        g_eff = holder->effective_object_budget();
        // Mint 34 probes up front (MUST succeed — minting is not
        // attaching; the budget gates the holder's list only).
        // BudgetProbe (not TcbCap): TcbCap-sized objects live in the
        // 32-block 128B pool, exactly enough to flake a 34-live test.
        BudgetProbe *caps[34] = {};
        uint64_t minted = 0;
        for (uint64_t i = 0; i < 34; ++i) {
            auto *p = static_cast<BudgetProbe *>(
                MemPool::alloc(sizeof(BudgetProbe)));
            if (p == nullptr)
                break;
            new (p) BudgetProbe;
            p->mark_pool_backed();
            kernel::test::ResourceTracker::instance().track_cap_object_add();
            caps[i] = p;
            ++minted;
        }
        if (minted != 34) {
            for (uint64_t i = 0; i < 34; ++i) {
                if (caps[i] != nullptr)
                    caps[i]->release();
            }
            g_minted = minted;
            return;
        }
        g_minted = 34;
        uint64_t ok = 0;
        for (uint64_t i = 0; i < 33; ++i) {
            if (holder->attach_object(caps[i]))
                ++ok;
        }
        g_attach = ok;
        g_refused = holder->attach_object(caps[33]) ? 0ULL : 1ULL;
        holder->detach_all_objects();
        uint64_t reok = 0;
        for (uint64_t i = 0; i < 33; ++i) {
            if (holder->attach_object(caps[i]))
                ++reok;
        }
        g_reattach = reok;
        holder->detach_all_objects();
        for (uint64_t i = 0; i < 34; ++i)
            caps[i]->release();
        holder->set_object_budget(0);
        g_default0 = holder->effective_object_budget();
        holder->set_object_budget(4);
        g_default4 = holder->effective_object_budget();
        // Teardown the holder through the normal path (add, then
        // terminate + drain; its object list is empty).
        Scheduler::add_task(*holder);
        Scheduler::terminate(*holder, 0);
        Scheduler::drain_zombie_list();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_holder);
    JARVIS_ASSERT_EQ(34ULL, g_minted);
    JARVIS_ASSERT_EQ(33ULL, g_eff);
    JARVIS_ASSERT_EQ(33ULL, g_attach);
    JARVIS_ASSERT_EQ(1ULL, g_refused);
    JARVIS_ASSERT_EQ(33ULL, g_reattach);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(CONFIG_MAX_PER_TASK_OBJECTS),
                     g_default0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(CONFIG_MAX_PER_TASK_OBJECTS),
                     g_default4);
    JARVIS_TEST_PASS();
}

void register_task_budget_tests() {
    Logger::info("Registering task_budget tests");
    JARVIS_REGISTER_TEST(object_budget_walk_bound);
}
