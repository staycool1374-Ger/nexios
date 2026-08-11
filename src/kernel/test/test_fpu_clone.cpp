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

/// @file test_fpu_clone.cpp
/// @brief FPU state preservation across fork/clone tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): a REAL dispatched kernel task (prio
/// ≥ 11, with a cloned PML4 so clone() exercises the user path) uses the FPU
/// and clones in its own running context.  The FPU owner / fpu_used / child
/// FXSAVE state are captured by the genuinely-running task, not the harness.

#if defined(CONFIG_ARCH_X86_64)
#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/vmm.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

static constexpr uint64_t FPU_PI_BITS = 0x400921F9F01B866EULL;

// Runmode: kernel
// Testidea: Cloning a task that owns the FPU copies the FXSAVE state to the
// child.  A REAL dispatched kernel task (with cloned PML4) uses x87
// (finit + fldl pi), becomes fpu_owner, then clones.
// Input: Dispatched task uses FPU and calls TaskControlBlock::clone in its
//        own running context.
// Expect: child->fpu_used == true, child FXSAVE tag word shows ST0 non-empty.
// Depends: TaskControlBlock::clone, kernel::Scheduler, arch::fxsave
JARVIS_TEST(fpu_clone_copies_state, "PRE: none | POST: none") {
    static uint64_t g_owner_ok = 0;
    static uint64_t g_parent_fpu_used = 0;
    static uint64_t g_child_fpu_used = 0;
    static uint64_t g_child_tag = 0;
    static uint64_t g_parent_tag = 0;
    static uint64_t g_ran = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t pi = FPU_PI_BITS;
            asm volatile("finit\nfldl %0" : : "m"(pi) : "memory");

            auto *fpu_owner_val = __atomic_load_n(&fpu_owner, __ATOMIC_ACQUIRE);
            g_owner_ok = (fpu_owner_val == self) ? 1 : 0;
            g_parent_fpu_used = self->fpu_used ? 1 : 0;

            uint64_t regs[22] = {};
            regs[17] = 0x1000;
            regs[18] = arch::SEG_USER_CODE;
            regs[19] = arch::RFLAGS_DEFAULT;
            regs[20] = 0x80000000;
            regs[21] = arch::SEG_USER_DATA;

            auto *child = TaskControlBlock::clone(regs);
            if (child == nullptr)
                return;
            g_child_fpu_used = child->fpu_used ? 1 : 0;
            g_child_tag = child->fpu_state[4] & 1;
            g_parent_tag = self->fpu_state[4] & 1;
            g_ran = 1;

            // Clean up child (TCB allocated from MemPool via clone + alloc_id).
            child->cleanup();
            MemPool::free(child);
        },
        11, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->is_user_ = true; // simulate a user parent for clone()
    parent->user_stack_ = 0x80000000;
    Scheduler::add_task(*parent);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(parent);

    // FXSAVE abridged tag word at offset 4: bit 0 = ST0 (1 = non-empty).

    kernel::test::terminate_and_drain(*parent);
    JARVIS_ASSERT_EQ(1ULL, g_ran);
    JARVIS_ASSERT_EQ(1ULL, g_owner_ok);
    JARVIS_ASSERT_EQ(1ULL, g_parent_fpu_used);
    JARVIS_ASSERT_EQ(1ULL, g_child_fpu_used);
    JARVIS_ASSERT_EQ(1ULL, g_child_tag);
    JARVIS_ASSERT_EQ(1ULL, g_parent_tag);

    // Re-init FPU to leave clean state for subsequent tests.
    asm volatile("finit" ::: "memory");

    JARVIS_TEST_PASS();
}

void register_fpu_clone_tests() {
    Logger::info("Registering FPU clone tests");
    JARVIS_REGISTER_TEST(fpu_clone_copies_state);
}
#endif
