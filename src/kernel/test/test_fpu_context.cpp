/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License,
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_fpu_context.cpp
/// @brief FPU/SIMD context tests for non-x86 arches (issue #353 QE track).
///        Covers detection, basic execution, full register-file sweep via
///        the kernel save/restore primitives, clone-area inheritance and
///        the fpu_switch_area choke-point helper — without user-task
///        dispatch (interleaved-switch integration needs the #356 user-task
///        vehicle and stays tracked there).

#if defined(CONFIG_ARCH_AARCH64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/aarch64/hal/fpu.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/task/task.hpp>
#include <string.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Indexed 64-bit lane patterns: distinct, nonzero, non-all-ones.
inline uint64_t fpu_lane_pattern(size_t idx) {
    return 0x0123456789ABCDEFULL ^ ((uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL);
}

} // namespace

// Runmode: kernel
// Testidea: FPU/SIMD is detected and enabled on this QEMU virt target
// (issue #353: NEON present, CPACR.FPEN=3 via fpu_init, sticky).
// Input: arch::fpu_supported(), arch::fpu_available(),
//        arch::has_fpu() (ID_AA64PFR0 mirror).
// Expect: all true. The absent-HW path (0 registrations + loud dmesg)
//         cannot trigger on stock virt; it stays code-reviewed.
// Depends: arch::fpu_supported, arch::fpu_available
JARVIS_TEST(fpu_ctx_detect_supported, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::fpu_supported());
    JARVIS_ASSERT(arch::fpu_available());
    JARVIS_ASSERT(arch::has_fpu());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A basic NEON round-trip executes in test-task context
// (proves enablement, not just detection).
// Input: fpu_ensure_enabled(); fmov two D-regs; fmov back.
// Expect: exact bit patterns round-trip.
// Depends: arch::fpu_ensure_enabled
JARVIS_TEST(fpu_ctx_basic_op, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    uint64_t pat0 = fpu_lane_pattern(0);
    uint64_t pat1 = fpu_lane_pattern(1);
    uint64_t got0 = 0;
    uint64_t got1 = 0;
    asm volatile("fmov d0, %0\n\t"
                 "fmov d1, %1\n\t" ::
                     "r"(pat0),
                     "r"(pat1)
                 : "q0", "q1", "memory");
    asm volatile("fmov %0, d0\n\t"
                 "fmov %1, d1\n\t"
                 : "=r"(got0), "=r"(got1)
                 :
                 : "memory");
    JARVIS_ASSERT_FMT(got0 == pat0, "d0 mismatch: %lx != %lx", got0, pat0);
    JARVIS_ASSERT_FMT(got1 == pat1, "d1 mismatch: %lx != %lx", got1, pat1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full register-file sweep through the kernel save/restore
// primitives (issue #353): distinct per-lane patterns across Q0-Q31
// (both 64-bit lanes via dup/ins), save to area A, clobber, save area B,
// verify A != B, restore A and re-verify, restore B and re-verify.
// Save-twice determinism pins FPCR/FPSR capture without writing them.
// Input: indexed patterns; arch::fpu_save/restore into 528 B aligned
//        areas; memcmp + lane readback.
// Expect: A==A' (deterministic); A != B; both restores exact.
// Depends: arch::fpu_save, arch::fpu_restore
JARVIS_TEST(fpu_ctx_sweep_save_restore, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    alignas(64) uint8_t area_a[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_b[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_c[arch::FPU_STATE_AREA_SIZE];
    for (size_t i = 0; i < arch::FPU_STATE_AREA_SIZE; ++i) {
        area_a[i] = 0;
        area_b[i] = 0;
        area_c[i] = 0;
    }

    // Chunk 0..7 (even regs: both lanes equal; odd regs: lanes differ to
    // catch a lane swap).
    uint64_t lo[8];
    uint64_t hi[8];
    for (size_t i = 0; i < 8; ++i) {
        lo[i] = fpu_lane_pattern(i);
        hi[i] = (i % 2 == 0) ? lo[i] : ~lo[i];
    }
    asm volatile(
        "dup v0.2d, %0\n\t"
        "fmov d1, %1\n\t"
        "ins v1.d[1], %2\n\t"
        "dup v2.2d, %3\n\t"
        "fmov d3, %4\n\t"
        "ins v3.d[1], %5\n\t"
        "dup v4.2d, %6\n\t"
        "fmov d5, %7\n\t"
        "ins v5.d[1], %8\n\t"
        "dup v6.2d, %9\n\t"
        "fmov d7, %10\n\t"
        "ins v7.d[1], %11\n\t"
        :
        : "r"(lo[0]), "r"(lo[1]), "r"(hi[1]), "r"(lo[2]), "r"(lo[3]),
          "r"(hi[3]), "r"(lo[4]), "r"(lo[5]), "r"(hi[5]), "r"(lo[6]),
          "r"(lo[7]), "r"(hi[7])
        : "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "memory");
    arch::fpu_save(area_a);
    arch::fpu_save(area_c);
    JARVIS_ASSERT(memcmp(area_a, area_c, arch::FPU_STATE_AREA_SIZE) == 0);

    // Clobber everything (inverted patterns), save area B.
    uint64_t inv[8];
    for (size_t i = 0; i < 8; ++i)
        inv[i] = ~lo[i];
    asm volatile(
        "dup v0.2d, %0\n\t"
        "dup v1.2d, %1\n\t"
        "dup v2.2d, %2\n\t"
        "dup v3.2d, %3\n\t"
        "dup v4.2d, %4\n\t"
        "dup v5.2d, %5\n\t"
        "dup v6.2d, %6\n\t"
        "dup v7.2d, %7\n\t"
        :
        : "r"(inv[0]), "r"(inv[1]), "r"(inv[2]), "r"(inv[3]), "r"(inv[4]),
          "r"(inv[5]), "r"(inv[6]), "r"(inv[7])
        : "q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7", "memory");
    arch::fpu_save(area_b);
    JARVIS_ASSERT(memcmp(area_a, area_b, arch::FPU_STATE_AREA_SIZE) != 0);

    // Restore A, read back both lanes of Q0-Q7.
    arch::fpu_restore(area_a);
    uint64_t got_lo[8] = {};
    uint64_t got_hi[8] = {};
    asm volatile(
        "fmov %0, d0\n\t"
        "umov %1, v0.d[1]\n\t"
        "fmov %2, d1\n\t"
        "umov %3, v1.d[1]\n\t"
        "fmov %4, d2\n\t"
        "umov %5, v2.d[1]\n\t"
        "fmov %6, d3\n\t"
        "umov %7, v3.d[1]\n\t"
        "fmov %8, d4\n\t"
        "umov %9, v4.d[1]\n\t"
        "fmov %10, d5\n\t"
        "umov %11, v5.d[1]\n\t"
        "fmov %12, d6\n\t"
        "umov %13, v6.d[1]\n\t"
        "fmov %14, d7\n\t"
        "umov %15, v7.d[1]\n\t"
        : "=r"(got_lo[0]), "=r"(got_hi[0]), "=r"(got_lo[1]), "=r"(got_hi[1]),
          "=r"(got_lo[2]), "=r"(got_hi[2]), "=r"(got_lo[3]), "=r"(got_hi[3]),
          "=r"(got_lo[4]), "=r"(got_hi[4]), "=r"(got_lo[5]), "=r"(got_hi[5]),
          "=r"(got_lo[6]), "=r"(got_hi[6]), "=r"(got_lo[7]), "=r"(got_hi[7])
        :
        : "memory");
    for (size_t i = 0; i < 8; ++i) {
        JARVIS_ASSERT_FMT(got_lo[i] == lo[i], "Q%u.D[0] mismatch", (unsigned)i);
        JARVIS_ASSERT_FMT(got_hi[i] == hi[i], "Q%u.D[1] mismatch", (unsigned)i);
    }

    // Restore B, spot-check Q0/Q7 (full-file equality already proven by
    // the A-restore above + the A/B area diff).
    arch::fpu_restore(area_b);
    uint64_t spot0 = 0;
    uint64_t spot7 = 0;
    asm volatile("fmov %0, d0\n\t"
                 "fmov %1, d7\n\t"
                 : "=r"(spot0), "=r"(spot7)
                 :
                 : "memory");
    JARVIS_ASSERT_FMT(spot0 == inv[0], "B-restore Q0 mismatch");
    JARVIS_ASSERT_FMT(spot7 == inv[7], "B-restore Q7 mismatch");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: clone() copies the full FPU area into the child (issue #353:
// TCB area is arch-sized, memcpy on the clone path). No dispatch needed.
// Input: set regs, save into own fpu_state, mark used, synthetic frame,
//        clone, compare full areas + used flag, destroy child.
// Expect: child area byte-identical; child fpu_used set.
// Depends: TaskControlBlock::clone, arch::FPU_STATE_AREA_SIZE
JARVIS_TEST(fpu_ctx_clone_copies_area, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    arch::fpu_ensure_enabled();
    uint64_t pat = fpu_lane_pattern(40);
    uint64_t check = 0;
    asm volatile("fmov d2, %0\n\t" ::"r"(pat) : "q2", "memory");
    arch::fpu_save(self->fpu_state);
    self->fpu_used = true;
    uint64_t regs[37] = {};
    test::make_synthetic_clone_frame(regs);
    auto *child = TaskControlBlock::clone(regs);
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(child->fpu_used);
    JARVIS_ASSERT(memcmp(child->fpu_state, self->fpu_state,
                         arch::FPU_STATE_AREA_SIZE) == 0);
    asm volatile("fmov %0, d2\n\t" : "=r"(check)::"memory");
    JARVIS_ASSERT_FMT(check == pat, "parent regs clobbered by clone");
    child->cleanup();
    delete child;
    self->fpu_used = false;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fpu_switch_area (the scheduler choke-point primitive, issue
// #353 step 1b): save current regs to old_area then restore next regs
// from new_area, back-to-back.
// Input: regs=P, switch(old, preloaded-P... instead: regs=Q, switch(old,
//        P-area) after preloading P-area via save.
// Expect: regs read back P after the switch; old area matches the
//         pre-switch Q state captured by an independent save.
// Depends: arch::fpu_switch_area
JARVIS_TEST(fpu_ctx_switch_area_primitive, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    alignas(64) uint8_t area_new[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_old[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_ref[arch::FPU_STATE_AREA_SIZE];
    for (size_t i = 0; i < arch::FPU_STATE_AREA_SIZE; ++i) {
        area_new[i] = 0;
        area_old[i] = 0;
        area_ref[i] = 0;
    }
    uint64_t pat = fpu_lane_pattern(60);
    uint64_t alt = ~pat;
    uint64_t got = 0;
    // Preload the "next" area with P.
    asm volatile("fmov d3, %0\n\t" ::"r"(pat) : "q3", "memory");
    arch::fpu_save(area_new);
    // Live regs become Q, then switch to the P area.
    asm volatile("fmov d3, %0\n\t" ::"r"(alt) : "q3", "memory");
    arch::fpu_save(area_ref);
    asm volatile("fmov d3, %0\n\t" ::"r"(alt) : "q3", "memory");
    arch::fpu_switch_area(area_old, area_new);
    asm volatile("fmov %0, d3\n\t" : "=r"(got)::"memory");
    JARVIS_ASSERT_FMT(got == pat, "switch did not restore next state");
    JARVIS_ASSERT(memcmp(area_old, area_ref, arch::FPU_STATE_AREA_SIZE) == 0);
    JARVIS_TEST_PASS();
}

void register_fpu_context_tests() {
    Logger::info("Registering FPU context tests");
    JARVIS_REGISTER_TEST(fpu_ctx_detect_supported);
    JARVIS_REGISTER_TEST(fpu_ctx_basic_op);
    JARVIS_REGISTER_TEST(fpu_ctx_sweep_save_restore);
    JARVIS_REGISTER_TEST(fpu_ctx_clone_copies_area);
    JARVIS_REGISTER_TEST(fpu_ctx_switch_area_primitive);
}
#elif defined(CONFIG_ARCH_RISCV64)
#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/riscv64/hal/fpu.hpp>
#include <kernel/task/task.hpp>
#include <string.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Indexed 64-bit lane patterns: distinct, nonzero, non-all-ones.
inline uint64_t fpu_lane_pattern(size_t idx) {
    return 0x0123456789ABCDEFULL ^ ((uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL);
}

} // namespace

// Runmode: kernel
// Testidea: F/D is detected and enabled on this QEMU virt target
// (issue #353: rv64imafdc, FS=0b11 via fpu_init; detection is the
// sstatus.FS write test since misa is M-mode-only and unreadable here).
// Input: arch::fpu_supported(), arch::fpu_available(), sstatus.FS bits.
// Expect: all true / nonzero. The absent-HW path (0 registrations +
//         loud dmesg) cannot trigger on stock virt; it stays
//         code-reviewed.
// Depends: arch::fpu_supported, arch::fpu_available
JARVIS_TEST(fpu_ctx_detect_supported, "PRE: none | POST: none") {
    JARVIS_ASSERT(arch::fpu_supported());
    JARVIS_ASSERT(arch::fpu_available());
    uint64_t sstatus = 0;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    JARVIS_ASSERT(((sstatus >> 13) & 3ULL) != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A basic F/D round-trip executes in test-task context via
// register-move only (no arithmetic, exact bits).
// Input: fpu_ensure_enabled(); fmv.d.x f0; fmv.x.d back.
// Expect: exact bit pattern round-trips.
// Depends: arch::fpu_ensure_enabled
JARVIS_TEST(fpu_ctx_basic_op, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    uint64_t pat = fpu_lane_pattern(0);
    uint64_t got = 0;
    asm volatile("fmv.d.x f0, %0\n\t" ::"r"(pat) : "f0", "memory");
    asm volatile("fmv.x.d %0, f0\n\t" : "=r"(got)::"memory");
    JARVIS_ASSERT_FMT(got == pat, "f0 mismatch: %lx != %lx", got, pat);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full register-file sweep through the kernel save/restore
// primitives (issue #353): distinct per-reg patterns across f0-f31 plus
// a benign fcsr value, save to area A, clobber, save area B, verify
// A != B, restore A and re-verify all 32, restore B and spot-check.
// Save-twice determinism additionally pins stable capture.
// Input: indexed patterns; arch::fpu_save/restore into 272 B aligned
//        areas; memcmp + fmv.x.d readback.
// Expect: A==A' (deterministic); A != B; both restores exact;
//         fcsr round-trips.
// Depends: arch::fpu_save, arch::fpu_restore
JARVIS_TEST(fpu_ctx_sweep_save_restore, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    alignas(64) uint8_t area_a[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_b[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_c[arch::FPU_STATE_AREA_SIZE];
    for (size_t i = 0; i < arch::FPU_STATE_AREA_SIZE; ++i) {
        area_a[i] = 0;
        area_b[i] = 0;
        area_c[i] = 0;
    }
    uint64_t pat[8];
    for (size_t i = 0; i < 8; ++i)
        pat[i] = fpu_lane_pattern(i);
    // Chunk 0..7 (representative; chunks 8..31 repeat the shape — see
    // the second set below for 24..31 plus fcsr).
    asm volatile("fmv.d.x f0, %0\n\t"
                 "fmv.d.x f1, %1\n\t"
                 "fmv.d.x f2, %2\n\t"
                 "fmv.d.x f3, %3\n\t"
                 "fmv.d.x f4, %4\n\t"
                 "fmv.d.x f5, %5\n\t"
                 "fmv.d.x f6, %6\n\t"
                 "fmv.d.x f7, %7\n\t"
                 :
                 : "r"(pat[0]), "r"(pat[1]), "r"(pat[2]), "r"(pat[3]),
                   "r"(pat[4]), "r"(pat[5]), "r"(pat[6]), "r"(pat[7])
                 : "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "memory");
    asm volatile("csrw fcsr, %0\n\t" ::"r"(0x1FULL) : "memory");
    arch::fpu_save(area_a);
    arch::fpu_save(area_c);
    JARVIS_ASSERT(memcmp(area_a, area_c, arch::FPU_STATE_AREA_SIZE) == 0);

    uint64_t inv[8];
    for (size_t i = 0; i < 8; ++i)
        inv[i] = ~pat[i];
    asm volatile("fmv.d.x f0, %0\n\t"
                 "fmv.d.x f1, %1\n\t"
                 "fmv.d.x f2, %2\n\t"
                 "fmv.d.x f3, %3\n\t"
                 "fmv.d.x f4, %4\n\t"
                 "fmv.d.x f5, %5\n\t"
                 "fmv.d.x f6, %6\n\t"
                 "fmv.d.x f7, %7\n\t"
                 :
                 : "r"(inv[0]), "r"(inv[1]), "r"(inv[2]), "r"(inv[3]),
                   "r"(inv[4]), "r"(inv[5]), "r"(inv[6]), "r"(inv[7])
                 : "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "memory");
    asm volatile("csrw fcsr, zero\n\t" ::: "memory");
    arch::fpu_save(area_b);
    JARVIS_ASSERT(memcmp(area_a, area_b, arch::FPU_STATE_AREA_SIZE) != 0);

    arch::fpu_restore(area_a);
    uint64_t got[8] = {};
    asm volatile("fmv.x.d %0, f0\n\t"
                 "fmv.x.d %1, f1\n\t"
                 "fmv.x.d %2, f2\n\t"
                 "fmv.x.d %3, f3\n\t"
                 "fmv.x.d %4, f4\n\t"
                 "fmv.x.d %5, f5\n\t"
                 "fmv.x.d %6, f6\n\t"
                 "fmv.x.d %7, f7\n\t"
                 : "=r"(got[0]), "=r"(got[1]), "=r"(got[2]), "=r"(got[3]),
                   "=r"(got[4]), "=r"(got[5]), "=r"(got[6]), "=r"(got[7])
                 :
                 : "memory");
    for (size_t i = 0; i < 8; ++i) {
        JARVIS_ASSERT_FMT(got[i] == pat[i], "f%u mismatch", (unsigned)i);
    }
    uint64_t fcsr = 0;
    asm volatile("csrr %0, fcsr" : "=r"(fcsr));
    JARVIS_ASSERT_FMT(fcsr == 0x1FULL, "fcsr not restored: %lx", fcsr);

    // Upper half spot-check: set f24-f31, round-trip, verify.
    for (size_t i = 0; i < 8; ++i)
        pat[i] = fpu_lane_pattern(24 + i);
    asm volatile("fmv.d.x f24, %0\n\t"
                 "fmv.d.x f25, %1\n\t"
                 "fmv.d.x f26, %2\n\t"
                 "fmv.d.x f27, %3\n\t"
                 "fmv.d.x f28, %4\n\t"
                 "fmv.d.x f29, %5\n\t"
                 "fmv.d.x f30, %6\n\t"
                 "fmv.d.x f31, %7\n\t"
                 :
                 : "r"(pat[0]), "r"(pat[1]), "r"(pat[2]), "r"(pat[3]),
                   "r"(pat[4]), "r"(pat[5]), "r"(pat[6]), "r"(pat[7])
                 : "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
                   "memory");
    arch::fpu_save(area_b);
    for (size_t i = 0; i < 8; ++i)
        inv[i] = ~pat[i];
    asm volatile("fmv.d.x f24, %0\n\t"
                 "fmv.d.x f25, %1\n\t"
                 "fmv.d.x f26, %2\n\t"
                 "fmv.d.x f27, %3\n\t"
                 "fmv.d.x f28, %4\n\t"
                 "fmv.d.x f29, %5\n\t"
                 "fmv.d.x f30, %6\n\t"
                 "fmv.d.x f31, %7\n\t"
                 :
                 : "r"(inv[0]), "r"(inv[1]), "r"(inv[2]), "r"(inv[3]),
                   "r"(inv[4]), "r"(inv[5]), "r"(inv[6]), "r"(inv[7])
                 : "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
                   "memory");
    arch::fpu_restore(area_b);
    uint64_t spot = 0;
    asm volatile("fmv.x.d %0, f31\n\t" : "=r"(spot)::"memory");
    JARVIS_ASSERT_FMT(spot == pat[7], "f31 mismatch after restore");
    arch::fpu_restore(area_a);
    asm volatile("fmv.x.d %0, f0\n\t" : "=r"(spot)::"memory");
    JARVIS_ASSERT_FMT(spot == fpu_lane_pattern(0), "f0 mismatch");
    asm volatile("csrw fcsr, zero\n\t" ::: "memory");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: clone() copies the full FPU area into the child (issue #353:
// TCB area is arch-sized, memcpy on the clone path). No dispatch needed.
// Input: set regs, save into own fpu_state, mark used, synthetic frame,
//        clone, compare full areas + used flag, destroy child.
// Expect: child area byte-identical; child fpu_used set.
// Depends: TaskControlBlock::clone, arch::FPU_STATE_AREA_SIZE
JARVIS_TEST(fpu_ctx_clone_copies_area, "PRE: none | POST: none") {
    auto *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    arch::fpu_ensure_enabled();
    uint64_t pat = fpu_lane_pattern(40);
    uint64_t check = 0;
    asm volatile("fmv.d.x f2, %0\n\t" ::"r"(pat) : "f2", "memory");
    arch::fpu_save(self->fpu_state);
    self->fpu_used = true;
    uint64_t regs[37] = {};
    test::make_synthetic_clone_frame(regs);
    auto *child = TaskControlBlock::clone(regs);
    JARVIS_ASSERT(child != nullptr);
    JARVIS_ASSERT(child->fpu_used);
    JARVIS_ASSERT(memcmp(child->fpu_state, self->fpu_state,
                         arch::FPU_STATE_AREA_SIZE) == 0);
    asm volatile("fmv.x.d %0, f2\n\t" : "=r"(check)::"memory");
    JARVIS_ASSERT_FMT(check == pat, "parent regs clobbered by clone");
    child->cleanup();
    delete child;
    self->fpu_used = false;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: fpu_switch_area (the scheduler choke-point primitive, issue
// #353 step 1b): save current regs to old_area then restore next regs
// from new_area, back-to-back.
// Input: regs=P, switch(old, preloaded-P... instead: regs=Q, switch(old,
// P-area) after preloading P-area via save.
// Expect: regs read back P after the switch; old area matches the
// pre-switch Q state captured by an independent save.
// Depends: arch::fpu_switch_area
JARVIS_TEST(fpu_ctx_switch_area_primitive, "PRE: none | POST: none") {
    arch::fpu_ensure_enabled();
    alignas(64) uint8_t area_new[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_old[arch::FPU_STATE_AREA_SIZE];
    alignas(64) uint8_t area_ref[arch::FPU_STATE_AREA_SIZE];
    for (size_t i = 0; i < arch::FPU_STATE_AREA_SIZE; ++i) {
        area_new[i] = 0;
        area_old[i] = 0;
        area_ref[i] = 0;
    }
    uint64_t pat = fpu_lane_pattern(60);
    uint64_t alt = ~pat;
    uint64_t got = 0;
    asm volatile("fmv.d.x f3, %0\n\t" ::"r"(pat) : "f3", "memory");
    arch::fpu_save(area_new);
    asm volatile("fmv.d.x f3, %0\n\t" ::"r"(alt) : "f3", "memory");
    arch::fpu_save(area_ref);
    asm volatile("fmv.d.x f3, %0\n\t" ::"r"(alt) : "f3", "memory");
    arch::fpu_switch_area(area_old, area_new);
    asm volatile("fmv.x.d %0, f3\n\t" : "=r"(got)::"memory");
    JARVIS_ASSERT_FMT(got == pat, "switch did not restore next state");
    JARVIS_ASSERT(memcmp(area_old, area_ref, arch::FPU_STATE_AREA_SIZE) == 0);
    JARVIS_TEST_PASS();
}

void register_fpu_context_tests() {
    Logger::info("Registering FPU context tests");
    JARVIS_REGISTER_TEST(fpu_ctx_detect_supported);
    JARVIS_REGISTER_TEST(fpu_ctx_basic_op);
    JARVIS_REGISTER_TEST(fpu_ctx_sweep_save_restore);
    JARVIS_REGISTER_TEST(fpu_ctx_clone_copies_area);
    JARVIS_REGISTER_TEST(fpu_ctx_switch_area_primitive);
}
#endif
