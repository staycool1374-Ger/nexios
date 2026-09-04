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

/// @file test_fpu_inv.cpp
/// @brief FPU/SIMD context invariants (issue #93) — INV-FPU1..3 tests.
///
/// Verifies the hardened #NM handler (docs/specs/fpu-context.md):
///   - INV-FPU1: no dynamic allocation in the vector-7 path.
///   - INV-FPU2: the owner-swap is non-interruptible (#NM is an interrupt
///     gate; the handler runs with IF cleared).
///   - Alignment: the per-TCB FXSAVE area is 64-byte aligned.
///   - Stale-restore regression: #NM with prev_fpu_owner == current (the
///     armed-switch window) must NOT fxrstor stale TCB state over live
///     registers.
/// This file is NOT in the mk/rules.mk filter-out list (test_fpu*.cpp are
/// excluded for GCC-16 / -mgeneral-regs-only reasons); it uses only
/// memory-operand x87 asm + arch::* helpers, never XMM register constraints.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// IEEE 754 double 3.14159 ≈ 0x400921F9F01B866E
static constexpr uint64_t FPU_PI_BITS = 0x400921F9F01B866EULL;
// IEEE 754 double 2.71828 ≈ 0x4005BF0A8B145769ULL
static constexpr uint64_t FPU_EULER_BITS = 0x4005BF0A8B145769ULL;

#if defined(CONFIG_ARCH_X86_64)
namespace {

/// @brief Forces a #NM by setting CR0.TS and executing an x87 instruction.
void force_nm() {
    uint64_t cr0 = arch::read_cr0();
    cr0 |= (1ULL << 3); // TS = 1
    arch::write_cr0(cr0);
    asm volatile("fnop" ::: "memory");
}

/// @brief CR0.TS state.
bool ts_set() {
    return (arch::read_cr0() >> 3) & 1;
}

} // namespace

// Runmode: kernel
// Testidea: INV-FPU1 — the #NM handler performs no dynamic allocation.  A
// forced #NM storm must leave PMM/MemPool/ResourceTracker deltas at zero.
// Input: Snapshot ResourceTracker + PMM; N forced #NM (set CR0.TS + x87 op);
//        re-capture.
// Expect: All deltas == 0; fpu_owner == current; CR0.TS clear after.
// Depends: ResourceTracker, PMM, #NM handler
JARVIS_TEST(fpu_nm_no_alloc, "PRE: none | POST: none") {
    kernel::test::ResourceCounters before;
    kernel::test::ResourceTracker::instance().capture(before);
    uint64_t pages_before = PMM::pool_used_pages();

    auto *current = Scheduler::current_task();
    JARVIS_ASSERT(current != nullptr);

    // Prime the FPU so the #NM path exercises save+restore (owner == current).
    asm volatile("finit" ::: "memory");

    for (uint64_t i = 0; i < 200; ++i)
        force_nm();

    uint64_t pages_after = PMM::pool_used_pages();
    auto *owner = __atomic_load_n(&fpu_owner, __ATOMIC_ACQUIRE);

    JARVIS_ASSERT_EQ(pages_before, pages_after);
    JARVIS_ASSERT(owner == current);
    JARVIS_ASSERT(!ts_set());
    kernel::test::ResourceTracker::instance().check(before, __func__);
    asm volatile("finit" ::: "memory");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: INV-FPU2 — the #NM owner-swap is non-interruptible.  #NM enters
// via an interrupt gate (IF cleared); the kernel is -mgeneral-regs-only so no
// #NM can nest inside an ISR.  fpu_nm_depth_max must stay <= baseline + 1
// across a #NM storm (only the #NM entry itself bumps the depth, never a
// nested timer).
// Input: Reset fpu_nm_depth_max; baseline isr_nesting_depth (harness = 0);
//        N forced #NM.
// Expect: fpu_nm_depth_max <= baseline + 1.
// Depends: fpu_nm_depth_max (global_state), #NM handler
JARVIS_TEST(fpu_nm_nesting_impossible, "PRE: none | POST: none") {
    uint64_t baseline = isr_nesting_depth;
    __atomic_store_n(&fpu_nm_depth_max, 0, __ATOMIC_RELEASE);

    asm volatile("finit" ::: "memory");
    for (uint64_t i = 0; i < 200; ++i)
        force_nm();

    uint64_t depth_max =
        __atomic_load_n(&fpu_nm_depth_max, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(depth_max <= baseline + 1);
    asm volatile("finit" ::: "memory");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The per-TCB FXSAVE area is 64-byte aligned and 512 bytes.
// Input: Compile-time static_asserts (offsetof % 64 == 0, sizeof == 512);
//        runtime check on a created TCB.
// Expect: Alignment and size hold; fpu_state_gen starts at 0.
// Depends: TaskControlBlock layout
JARVIS_TEST(fpu_save_area_alignment, "PRE: none | POST: none") {
    static_assert(__builtin_offsetof(TaskControlBlock, fpu_state) % 64 == 0,
                  "fpu_state must be 64-byte aligned (issue #93)");
    static_assert(sizeof(TaskControlBlock::fpu_state) == 512,
                  "fpu_state must be exactly 512 bytes (FXSAVE)");
    static_assert(alignof(TaskControlBlock::fpu_state) == 64,
                  "fpu_state alignment must be 64 (issue #93)");

    auto *t = TaskControlBlock::create([]() {}, 1, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(reinterpret_cast<uintptr_t>(&t->fpu_state) % 64 == 0);
    JARVIS_ASSERT_EQ(0U, static_cast<unsigned>(t->fpu_state_gen));

    kernel::test::terminate_and_drain2(t, t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Regression for the stale-restore S1 (issue #93): #NM with
// prev_fpu_owner == current (the armed-switch window where TS was published
// but the deferred switch has not applied) must NOT fxrstor stale TCB state
// over live registers.  The owner's live ST0 must survive the round-trip.
// Input: Arm TS, then finit (fires #NM with prev==nullptr → establishes
//        owner==current); fldl pi; set CR0.TS again (simulate armed arm);
//        fldl euler (fires #NM with prev==current); read back.
// Expect: After the retried load, ST0 holds euler then pi — live state
//         preserved, no stale clobber.
// Depends: #NM handler stale-restore guard
JARVIS_TEST(fpu_nm_own_arm_no_clobber, "PRE: none | POST: none") {
    uint64_t r1 = 0;
    uint64_t r2 = 0;

    // snapshot_restore resets fpu_owner to nullptr, so the FIRST FPU op of
    // this test must fire #NM to establish owner==current.  Arm TS first:
    // finit fires #NM with prev==nullptr → init path, owner = harness.
    uint64_t cr0 = arch::read_cr0();
    cr0 |= (1ULL << 3);
    arch::write_cr0(cr0);
    asm volatile("finit\n"
                 "fldl %0" // pi -> ST0 (owner now == harness)
                 :
                 : "m"(FPU_PI_BITS)
                 : "memory");

    // Simulate the published-but-unapplied switch arm: TS set, owner still
    // current.  The next x87 op fires #NM with prev_fpu_owner == current.
    force_nm();

    // This retried load must NOT have clobbered the live x87 stack with stale
    // TCB state: fldl pushes euler onto ST0, fstpl pops it into r1, and the
    // second fstpl pops the original pi into r2.
    asm volatile("fldl %0\n"
                 "fstpl %1\n"
                 "fstpl %2"
                 :
                 : "m"(FPU_EULER_BITS), "m"(r1), "m"(r2)
                 : "memory");

    JARVIS_ASSERT_EQ(FPU_EULER_BITS, r1);
    JARVIS_ASSERT_EQ(FPU_PI_BITS, r2);
    asm volatile("finit" ::: "memory");
    JARVIS_TEST_PASS();
}
#endif  // CONFIG_ARCH_X86_64

void register_fpu_inv_tests() {
    Logger::info("Registering fpu_invariants tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(fpu_nm_no_alloc);
    JARVIS_REGISTER_TEST(fpu_nm_nesting_impossible);
    JARVIS_REGISTER_TEST(fpu_save_area_alignment);
    JARVIS_REGISTER_TEST(fpu_nm_own_arm_no_clobber);
#endif  // CONFIG_ARCH_X86_64
}
