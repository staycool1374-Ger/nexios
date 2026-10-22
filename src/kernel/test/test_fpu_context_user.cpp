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

/// @file test_fpu_context_user.cpp
/// @brief Phase-2 FPU/SIMD user-task tests (issue #353): prove interleaved
/// user-task FP isolation + multi-task interference through the REAL
/// eager-switch path (which fires only for user tasks, so kernel-task
/// tests cannot cover it).
///
/// Vehicle: per-task emitted U-mode machine code (objdump-verified word
/// arrays, see /tmp/fpu_stub_{a64,rv}.S reference listings) + a private
/// data page per worker for pattern verification + exit codes. The stub
/// sets FP regs from integer bit patterns (no FP arithmetic anywhere),
/// blocks in RECEIVE, verifies its own regs after each harness IPC::send
/// wake, and exits with the mismatch mask. The harness observes DONE magic
/// + exit codes + fpu_used/fpu_state_gen + saved fpu_state bytes.
///
/// Journal design note: there is NO shared page. The kernel refuses to map
/// kernel-owned pages user-accessible (PMM::is_user_page ENSURE in
/// map_page_in_pml4 — fail-fast by design), and a user-owned shared page
/// would be freed by every worker's teardown (double-free). Each worker
/// reports alive/rounds into its OWN data page, which the harness reads
/// via the HHDM alias — same protocol, no sharing hazards.
///
/// U-mode legality: no CSR/system regs, no wfi/dc/ic/eret, no misa-style
/// traps anywhere in the stub (#357 lesson). Every wait is tick-bounded,
/// so a wedge becomes a loud FAIL, never a TIMEOUT.

#if defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Indexed 64-bit lane patterns (mirrors test_fpu_context.cpp): distinct,
// nonzero, non-all-ones. Integer bit patterns only — never FP arithmetic.
inline uint64_t fpu_lane_pattern(size_t idx) {
    return 0x0123456789ABCDEFULL ^ ((uint64_t)(idx + 1) * 0x9E3779B97F4A7C15ULL);
}

// Test-local layout. Below USER_SPACE_LIMIT, clear of the auto yield stub
// (0x40000000), the stack (0x70000000) and the buffer window (>=0x100000000).
constexpr uint64_t kUserCodeVa = 0x41000000ULL;   // R+X, per task
constexpr uint64_t kUserDataVa = 0x41001000ULL;   // RW + NX, per task
constexpr uint64_t kUserPrio = 20;               // above the prio-10 harness
constexpr size_t kUserStackBytes = 32 * 1024;
constexpr uint64_t kRounds = 3;
constexpr uint64_t kJoinBudgetTicks = 400;
constexpr uint64_t kDoneMagic = 0xD0D0F00DULL;
constexpr uint64_t kAliveMagic = 0xA0ULL;

// Private data page layout (byte offsets).
constexpr uint64_t kOffLo = 0x00;     // lo[0..2] expected patterns
constexpr uint64_t kOffHi = 0x18;     // hi[0..2] expected (aarch64 ~lo)
constexpr uint64_t kOffFail = 0x30;   // mismatch mask (0 = clean)
constexpr uint64_t kOffRounds = 0x38; // bit r set after round r verified
constexpr uint64_t kOffDone = 0x40;   // kDoneMagic written before SYS_EXIT
constexpr uint64_t kOffIndex = 0x48;  // task index (diagnostics)
constexpr uint64_t kOffAlive = 0x50;  // kAliveMagic once regs are set
constexpr uint64_t kOffRecv = 0x100;  // 16-byte RECEIVE scratch

struct FpuUserWorker {
    TaskControlBlock *tcb = nullptr;
    uint64_t code_phys = 0;
    uint64_t data_phys = 0;
};

#if defined(CONFIG_ARCH_AARCH64)

// Word builders (field-constructed, verified against the reference
// assembly below via static_assert).
constexpr uint32_t a64_movz(uint32_t rd, uint32_t imm16) {
    return 0xD2800000U | (imm16 << 5) | rd;
}
constexpr uint32_t a64_str_uoff(uint32_t rd, uint32_t rn, uint32_t off) {
    return 0xF9000000U | ((off >> 3) << 10) | (rn << 5) | rd; // 64-bit
}
constexpr uint32_t a64_ldr_uoff(uint32_t rd, uint32_t rn, uint32_t off) {
    return 0xF9400000U | ((off >> 3) << 10) | (rn << 5) | rd;
}
static_assert(a64_str_uoff(21, 19, 0x30) == 0xF9001A75U, "str x21");
static_assert(a64_ldr_uoff(1, 19, 0x18) == 0xF9400E61U, "ldr x1");
static_assert(a64_movz(5, 1) == 0xD2800025U, "movz x5,#1");
static_assert(a64_movz(5, (uint32_t)kAliveMagic) == 0xD2801405U, "mov alive");
static_assert(a64_movz(5, 0xA0) == 0xD2801405U, "movz x5,#alive");

// Fixed words (objdump-verified, see /tmp/fpu_stub_a64.S).
constexpr uint32_t A64_STR_XZR_38 = 0xF9001E7FU;   // str xzr,[x19,#56]
constexpr uint32_t A64_STR_XZR_40 = 0xF900227FU;   // str xzr,[x19,#64]
constexpr uint32_t A64_MOV_X21_XZR = 0xAA1F03E5U;   // mov x21,xzr (fail acc=0)
constexpr uint32_t A64_FMOV_D0_X1 = 0x9E670020U;    // fmov d0,x1
constexpr uint32_t A64_FMOV_D1_X1 = 0x9E670021U;    // fmov d1,x1
constexpr uint32_t A64_FMOV_D2_X1 = 0x9E670022U;    // fmov d2,x1
constexpr uint32_t A64_MOV_V0D1_X1 = 0x4E181C20U;   // mov v0.d[1],x1
constexpr uint32_t A64_MOV_V1D1_X1 = 0x4E181C21U;   // mov v1.d[1],x1
constexpr uint32_t A64_MOV_V2D1_X1 = 0x4E181C22U;   // mov v2.d[1],x1
constexpr uint32_t A64_ADD_X1_X19 = 0x91040261U;    // add x1,x19,#0x100
constexpr uint32_t A64_MOV_X0_0 = 0xD2800000U;      // mov x0,#0
constexpr uint32_t A64_MOV_X2_16 = 0xD2800202U;     // mov x2,#16
constexpr uint32_t A64_MOV_X3_0 = 0xD2800003U;      // mov x3,#0
constexpr uint32_t A64_MOV_X8_RCV = 0xD2800048U;    // mov x8,#2 RECEIVE
constexpr uint32_t A64_MOV_X8_EXIT = 0xD28000C8U;   // mov x8,#6 EXIT
constexpr uint32_t A64_SVC = 0xD4000001U;           // svc #0
constexpr uint32_t A64_FMOV_X2_D0 = 0x9E660002U;    // fmov x2,d0
constexpr uint32_t A64_FMOV_X2_D1 = 0x9E660022U;    // fmov x2,d1
constexpr uint32_t A64_FMOV_X2_D2 = 0x9E660042U;    // fmov x2,d2
constexpr uint32_t A64_UMOV_X2_V0 = 0x4E183C02U;    // umov x2,v0.d[1]
constexpr uint32_t A64_UMOV_X2_V1 = 0x4E183C22U;    // umov x2,v1.d[1]
constexpr uint32_t A64_UMOV_X2_V2 = 0x4E183C42U;    // umov x2,v2.d[1]
constexpr uint32_t A64_EOR = 0xCA020023U;           // eor x3,x1,x2
constexpr uint32_t A64_ORR_ACC = 0xAA0302B5U;       // orr x21,x21,x3
constexpr uint32_t A64_MOV_X0_X21 = 0xAA1503E0U;   // mov x0,x21 (exit code; NOT ...E1 which targets x1)
constexpr uint32_t A64_MOVZ_F00D = 0xD29E01A5U;     // movz x5,#0xF00D
constexpr uint32_t A64_MOVK_D0D0 = 0xF2BA1A05U;     // movk x5,#0xD0D0,lsl#16
constexpr uint32_t A64_B_SELF = 0x14000000U;        // b . (safety net)
static_assert(A64_MOV_X0_X21 == 0xAA1503E0U, "mov x0,x21 (Rd=x0, not x1)");

#else // CONFIG_ARCH_RISCV64

// Word builders (field-constructed, verified via static_assert).
constexpr uint32_t rv_addi(uint32_t rd, uint32_t imm12) {
    return (imm12 << 20) | (rd << 7) | 0x13U;
}
constexpr uint32_t rv_sd(uint32_t rs1, uint32_t rs2, uint32_t off) {
    return (((off >> 5) & 0x7FU) << 25) | (rs2 << 20) | (rs1 << 15) |
           (3U << 12) | (((off) & 0x1FU) << 7) | 0x23U;
}
constexpr uint32_t rv_ld(uint32_t rd, uint32_t rs1, uint32_t off) {
    return ((off) << 20) | (rs1 << 15) | (3U << 12) | (rd << 7) | 0x03U;
}
static_assert(rv_addi(5, 160) == 0x0A000293U, "li t0,160");
static_assert(rv_addi(10, 0) == 0x00000513U, "li a0,0");
static_assert(rv_addi(17, 6) == 0x00600893U, "li a7,6");
static_assert(rv_sd(19, 5, 0) == 0x0059B023U, "sd t0,0(s3)");
static_assert(rv_sd(18, 0, 0x40) == 0x04093023U, "sd zero,64(s2)");
static_assert(rv_sd(18, 20, 0x30) == 0x03493823U, "sd s4,48(s2)");
static_assert(rv_ld(5, 18, 0) == 0x00093283U, "ld t0,0(s2)");
static_assert(rv_ld(7, 18, 16) == 0x01093383U, "ld t2,16(s2)");

// Fixed words (-march=rv64imafd 32-bit, see /tmp/fpu_stub_rv.S).
constexpr uint32_t RV_FMV_D_X_F0 = 0xF2028053U;  // fmv.d.x f0,t0
constexpr uint32_t RV_FMV_D_X_F1 = 0xF20280D3U;  // fmv.d.x f1,t0
constexpr uint32_t RV_FMV_D_X_F2 = 0xF2028153U;  // fmv.d.x f2,t0
constexpr uint32_t RV_FMV_X_D_T1_F0 = 0xE2000353U; // fmv.x.d t1,f0
constexpr uint32_t RV_FMV_X_D_T1_F1 = 0xE2008353U; // fmv.x.d t1,f1
constexpr uint32_t RV_FMV_X_D_T1_F2 = 0xE2010353U; // fmv.x.d t1,f2
constexpr uint32_t RV_XOR = 0x00734333U;          // xor t1,t1,t2
constexpr uint32_t RV_OR_ACC = 0x006A6A33U;       // or s4,s4,t1
constexpr uint32_t RV_ADDI_A1 = 0x10090593U;      // addi a1,s2,256
constexpr uint32_t RV_ECALL = 0x00000073U;        // ecall
constexpr uint32_t RV_MV_A0_S4 = 0x000A0513U;     // mv a0,s4
constexpr uint32_t RV_LI_S4_0 = 0x00000A13U;      // li s4,0 (fail acc=0)
constexpr uint32_t RV_J_SELF = 0x0000006FU;      // jal x0,0 (safety net)
constexpr uint32_t RV_LUI_D = 0x0000D2B7U;        // lui t0,0xd
constexpr uint32_t RV_ADDIW_D0D0 = 0x0D02829BU;    // addiw t0,t0,208
constexpr uint32_t RV_SLLI_16 = 0x01029293U;      // slli t0,t0,16
constexpr uint32_t RV_LUI_F = 0x0000F337U;        // lui t1,0xf
constexpr uint32_t RV_ADDIW_F00D = 0x00D3031BU;   // addiw t1,t1,13
constexpr uint32_t RV_OR_T0 = 0x0062E2B3U;        // or t0,t0,t1

#endif

// Emit one unrolled round (identical shape every round; only the round
// marker differs). Returns words written.
inline size_t emit_round(uint32_t *code, uint64_t round) {
#if defined(CONFIG_ARCH_AARCH64)
    *code++ = A64_MOV_X0_0;
    *code++ = A64_ADD_X1_X19;
    *code++ = A64_MOV_X2_16;
    *code++ = A64_MOV_X3_0;
    *code++ = A64_MOV_X8_RCV;
    *code++ = A64_SVC;
    const uint32_t ldr[6] = {a64_ldr_uoff(1, 19, 0),    a64_ldr_uoff(1, 19, 0x18),
                             a64_ldr_uoff(1, 19, 8),    a64_ldr_uoff(1, 19, 0x20),
                             a64_ldr_uoff(1, 19, 0x10), a64_ldr_uoff(1, 19, 0x28)};
    const uint32_t get[6] = {A64_FMOV_X2_D0, A64_UMOV_X2_V0, A64_FMOV_X2_D1,
                             A64_UMOV_X2_V1, A64_FMOV_X2_D2, A64_UMOV_X2_V2};
    for (int l = 0; l < 6; ++l) {
        *code++ = ldr[l];
        *code++ = get[l];
        *code++ = A64_EOR;
        *code++ = A64_ORR_ACC;
    }
    *code++ = a64_movz(5, (uint32_t)(round + 1));
    *code++ = a64_str_uoff(5, 19, (uint32_t)kOffRounds);
    return 6 + 24 + 2;
#else
    *code++ = rv_addi(10, 0); // li a0,0
    *code++ = RV_ADDI_A1;
    *code++ = rv_addi(12, 16); // li a2,16
    *code++ = rv_addi(13, 0);  // li a3,0
    *code++ = rv_addi(17, 2);  // li a7,2 RECEIVE
    *code++ = RV_ECALL;
    const uint32_t get[3] = {RV_FMV_X_D_T1_F0, RV_FMV_X_D_T1_F1,
                             RV_FMV_X_D_T1_F2};
    const uint32_t off[3] = {0, 8, 16};
    for (int l = 0; l < 3; ++l) {
        *code++ = get[l];
        *code++ = rv_ld(7, 18, off[l]); // ld t2,off(s2)
        *code++ = RV_XOR;
        *code++ = RV_OR_ACC;
    }
    *code++ = rv_addi(5, (uint32_t)(round + 1)); // li t0,r+1
    *code++ = rv_sd(18, 5, (uint32_t)kOffRounds);
    return 6 + 12 + 2;
#endif
}

// Emit the full worker stub. Identical for every worker (patterns live in
// the data page, not the code). Returns words written.
inline size_t emit_worker_stub(uint32_t *code) {
    size_t n = 0;
#if defined(CONFIG_ARCH_AARCH64)
    code[n++] = A64_STR_XZR_38; // rounds_done = 0
    code[n++] = A64_STR_XZR_40; // done_magic = 0
    code[n++] = A64_MOV_X21_XZR; // fail accumulator = 0 (never assume init)
    const uint32_t lo[3] = {0, 8, 0x10};
    const uint32_t hi[3] = {0x18, 0x20, 0x28};
    const uint32_t setd[3] = {A64_FMOV_D0_X1, A64_FMOV_D1_X1, A64_FMOV_D2_X1};
    const uint32_t sethi[3] = {A64_MOV_V0D1_X1, A64_MOV_V1D1_X1,
                               A64_MOV_V2D1_X1};
    for (int l = 0; l < 3; ++l) {
        code[n++] = a64_ldr_uoff(1, 19, lo[l]);
        code[n++] = setd[l];
        code[n++] = a64_ldr_uoff(1, 19, hi[l]);
        code[n++] = sethi[l];
    }
    code[n++] = a64_movz(5, (uint32_t)kAliveMagic);
    code[n++] = a64_str_uoff(5, 19, (uint32_t)kOffAlive);
    for (uint64_t r = 0; r < kRounds; ++r)
        n += emit_round(code + n, r);
    code[n++] = a64_str_uoff(21, 19, (uint32_t)kOffFail);
    code[n++] = A64_MOVZ_F00D;
    code[n++] = A64_MOVK_D0D0;
    code[n++] = a64_str_uoff(5, 19, (uint32_t)kOffDone);
    code[n++] = A64_MOV_X0_X21;
    code[n++] = A64_MOV_X8_EXIT;
    code[n++] = A64_SVC;
    code[n++] = A64_B_SELF;
#else
    code[n++] = rv_sd(18, 0, (uint32_t)kOffRounds); // sd zero,56(s2)
    code[n++] = rv_sd(18, 0, (uint32_t)kOffDone);   // sd zero,64(s2)
    code[n++] = RV_LI_S4_0;                         // fail accumulator = 0
    const uint32_t off[3] = {0, 8, 16};
    const uint32_t setd[3] = {RV_FMV_D_X_F0, RV_FMV_D_X_F1, RV_FMV_D_X_F2};
    for (int l = 0; l < 3; ++l) {
        code[n++] = rv_ld(5, 18, off[l]); // ld t0,off(s2)
        code[n++] = setd[l];
    }
    code[n++] = rv_addi(5, (uint32_t)kAliveMagic);
    code[n++] = rv_sd(18, 5, (uint32_t)kOffAlive);
    for (uint64_t r = 0; r < kRounds; ++r)
        n += emit_round(code + n, r);
    code[n++] = rv_sd(18, 20, (uint32_t)kOffFail); // sd s4,48(s2)
    code[n++] = RV_LUI_D;
    code[n++] = RV_ADDIW_D0D0;
    code[n++] = RV_SLLI_16;
    code[n++] = RV_LUI_F;
    code[n++] = RV_ADDIW_F00D;
    code[n++] = RV_OR_T0;
    code[n++] = rv_sd(18, 5, (uint32_t)kOffDone); // sd t0,64(s2)
    code[n++] = RV_MV_A0_S4;
    code[n++] = rv_addi(17, 6); // li a7,6 EXIT
    code[n++] = RV_ECALL;
    code[n++] = RV_J_SELF;
#endif
    return n;
}

/// @brief Spawn one FP worker: real create_user task, private code+data
/// pages. Seeds data/base registers + entry point in BOTH the kstack trap
/// frame and the TaskContext (identical values — whichever source the arch
/// dispatch reads, the outcome is the same).
bool fpu_user_spawn(FpuUserWorker &w, uint32_t idx) {
    w.tcb = TaskControlBlock::create_user(test::forever_entry, kUserPrio, 10,
                                          kUserStackBytes);
    if (w.tcb == nullptr)
        return false;
    w.code_phys = PMM::alloc_user_page();
    w.data_phys = PMM::alloc_user_page();
    if (w.code_phys == 0 || w.data_phys == 0) {
        if (w.code_phys != 0)
            PMM::free_page(w.code_phys);
        if (w.data_phys != 0)
            PMM::free_page(w.data_phys);
        w.tcb->cleanup();
        delete w.tcb;
        w.tcb = nullptr;
        return false;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *code = reinterpret_cast<uint32_t *>(arch::HHDM_OFFSET + w.code_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *data = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + w.data_phys);
    const size_t nwords = emit_worker_stub(code);
    (void)nwords;
    for (size_t i = 0; i < 512; ++i)
        data[i] = 0;
    data[kOffLo / 8 + 0] = fpu_lane_pattern(10 + idx);
    data[kOffLo / 8 + 1] = fpu_lane_pattern(20 + idx);
    data[kOffLo / 8 + 2] = fpu_lane_pattern(30 + idx);
#if defined(CONFIG_ARCH_AARCH64)
    data[kOffHi / 8 + 0] = ~fpu_lane_pattern(10 + idx);
    data[kOffHi / 8 + 1] = ~fpu_lane_pattern(20 + idx);
    data[kOffHi / 8 + 2] = ~fpu_lane_pattern(30 + idx);
#endif
    data[kOffIndex / 8] = idx;

    VMM::map_page_in_pml4(kUserCodeVa, w.code_phys, true, true,
                          w.tcb->get_page_table());
    VMM::map_page_in_pml4(kUserDataVa, w.data_phys, true, false,
                          w.tcb->get_page_table());
    // Fail-fast: mapping bugs must never be misreported as trap faults.
    if (VMM::virt_to_phys_in_pml4(kUserCodeVa, w.tcb->get_page_table()) !=
        w.code_phys) {
        return false;
    }
    if (VMM::virt_to_phys_in_pml4(kUserDataVa, w.tcb->get_page_table()) !=
        w.data_phys) {
        return false;
    }

#if defined(CONFIG_ARCH_AARCH64)
    // kstack trap frame (vectors.S save_area: x19@152, x20@160 unused,
    // ELR@256) + TaskContext mirror — identical values, mechanism-proof.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<uint64_t *>(w.tcb->context.sp_el0);
    frame[19] = kUserDataVa;
    frame[32] = kUserCodeVa;
    w.tcb->context.x[19] = kUserDataVa;
    w.tcb->context.elr_el1 = kUserCodeVa;
#else
    // kstack trap frame (syscall_entry.S: s2@136, s3@144, SEPC@248).
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *frame = reinterpret_cast<uint64_t *>(w.tcb->context.sp);
    frame[17] = kUserDataVa;
    frame[31] = kUserCodeVa;
    w.tcb->context.s2 = kUserDataVa;
    w.tcb->context.sepc = kUserCodeVa;
    w.tcb->context.sstatus = (1ULL << 8) | (3ULL << 13); // SPIE + FS=11
#endif
    if (!Scheduler::set_sched_policy(*w.tcb, SchedPolicy::FIXED))
        return false;
    {
        arch::IrqGuard g;
        Scheduler::add_task(*w.tcb);
    }
    return true;
}

/// @brief Bounded wait: like wait_for_termination_safe but returns false
/// after budget_ticks instead of wedging the suite.
template <typename Pred> bool wait_until(Pred pred, uint64_t budget_ticks) {
    const uint64_t deadline = arch::Timer::ticks() + budget_ticks;
    while (!pred()) {
        if (arch::Timer::ticks() >= deadline)
            return false;
        __atomic_store_n(&::kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
    return true;
}

/// @brief Tear down workers. reached_exit distinguishes sys_exit
/// self-termination (unqueued) from fault death (already queued):
/// terminate exactly once either way, then single drain.
void fpu_user_teardown(FpuUserWorker *workers, size_t n,
                       const bool *reached_exit, uint64_t free_before,
                       bool &ok) {
    ok = true;
    for (size_t i = 0; i < n; ++i) {
        if (!TaskControlBlock::is_valid(workers[i].tcb)) {
            ok = false;
            continue;
        }
        if (workers[i].tcb->state != TaskState::TERMINATED ||
            reached_exit[i]) {
            // terminate on an already-queued TERMINATED task re-appends
            // (no idempotency guard) — the reached_exit discriminator
            // selects exactly one queueing per task.
            (void)Scheduler::terminate_err(*workers[i].tcb,
                                           workers[i].tcb->exit_code);
        }
    }
    Scheduler::drain_zombie_list();
    if (PMM::free_pages_ref() != free_before)
        ok = false;
}

// Runmode: kernel
// Testidea: A single user task keeps FP state across timer preemptions:
// set Q0/Q1/Q2 (both lanes), block, get woken 3x, verify all lanes each
// time, exit clean. The eager switch must save U-mode regs on the way out
// and restore them on the way back; fpu_used + fpu_state_gen prove the
// save arm ran (not just that registers were never touched).
// Input: One create_user worker (prio 20, FIXED), private pattern page;
// harness wakes via IPC::send, tick-bounded waits.
// Expect: reached EXIT, exit 0, fail 0, rounds 3, fpu_used,
// fpu_state_gen >= rounds, saved area == patterns.
// Depends: create_user, map user leaves, eager switch, SYS_RECEIVE/send,
// SYS_EXIT, drain.
JARVIS_TEST(fpu_ctx_user_single_task_persists, "PRE: none | POST: none") {
    const uint64_t free_before = PMM::free_pages_ref();

    FpuUserWorker w{};
    bool spawned = fpu_user_spawn(w, 0);
    JARVIS_ASSERT(spawned);
    Scheduler::reschedule();

    // Phase A: worker entered U-mode, set regs, blocked in RECEIVE.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *data = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + w.data_phys);
    bool alive_ok = wait_until([&] { return data[kOffAlive / 8] == kAliveMagic; },
                               kJoinBudgetTicks);
    bool blocked_ok = wait_until(
        [&] { return w.tcb->state == TaskState::BLOCKED; }, kJoinBudgetTicks);

    // Phase B: 3 wake+verify rounds, harness-ordered.
    bool rounds_ok = true;
    for (uint64_t r = 0; r < kRounds && alive_ok && blocked_ok; ++r) {
        Message msg{};
        msg.sender_id = 0;
        msg.type = 0xF9;
        msg.priority = 0;
        msg.data_size = 0;
        if (!IPC::send(w.tcb->id, msg)) {
            rounds_ok = false;
            break;
        }
        if (!wait_until([&] { return data[kOffRounds / 8] == r + 1; },
                        kJoinBudgetTicks)) {
            rounds_ok = false;
            break;
        }
    }

    // Phase C: worker exits; observe BEFORE teardown (drain frees TCB).
    bool exited_ok = wait_until(
        [&] { return w.tcb->state == TaskState::TERMINATED; },
        kJoinBudgetTicks);
    const uint64_t exit_code = w.tcb->exit_code;
    const bool reached_exit = (data[kOffDone / 8] == kDoneMagic);
    const uint64_t fail_bits = data[kOffFail / 8];
    const uint64_t rounds_done = data[kOffRounds / 8];
    const bool fpu_used = w.tcb->fpu_used;
    const uint32_t gen = w.tcb->fpu_state_gen;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *area = reinterpret_cast<const uint64_t *>(w.tcb->fpu_state);
    uint64_t got_lo[3];
#if defined(CONFIG_ARCH_AARCH64)
    uint64_t got_hi[3];
    got_lo[0] = area[0];
    got_hi[0] = area[1];
    got_lo[1] = area[2];
    got_hi[1] = area[3];
    got_lo[2] = area[4];
    got_hi[2] = area[5];
#else
    got_lo[0] = area[0];
    got_lo[1] = area[1];
    got_lo[2] = area[2];
#endif

    bool teardown_ok = true;
    fpu_user_teardown(&w, 1, &reached_exit, free_before, teardown_ok);

    JARVIS_ASSERT(alive_ok);
    JARVIS_ASSERT(blocked_ok);
    JARVIS_ASSERT(rounds_ok);
    JARVIS_ASSERT(exited_ok);
    JARVIS_ASSERT(reached_exit);
    const uint64_t want_lo[3] = {fpu_lane_pattern(10), fpu_lane_pattern(20),
                                 fpu_lane_pattern(30)};
    JARVIS_ASSERT_EQ(0ULL, exit_code);
    JARVIS_ASSERT_EQ(0ULL, fail_bits);
    JARVIS_ASSERT_EQ(3ULL, rounds_done); // last round number written
    JARVIS_ASSERT(fpu_used);
    JARVIS_ASSERT(gen >= (uint32_t)kRounds);
    for (int l = 0; l < 3; ++l) {
        JARVIS_ASSERT_FMT(got_lo[l] == want_lo[l], "lane %d lo mismatch", l);
#if defined(CONFIG_ARCH_AARCH64)
        JARVIS_ASSERT_FMT(got_hi[l] == ~want_lo[l], "lane %d hi mismatch", l);
#endif
    }
    JARVIS_ASSERT(teardown_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Three interleaved user tasks keep disjoint FP state: each sets
// distinct patterns, the harness wakes them in rotated order 3x, and every
// verify-after-wake must see only its own patterns (no cross-task
// contamination via the eager switch).
// Input: Three create_user workers (prio 20, FIXED), distinct seeds,
// private pattern pages; rotated wake order.
// Expect: all alive + BLOCKED before round 0; all exit 0, fail 0,
// rounds[i] == 3, fpu_used, gen >= 4; each saved area == own patterns.
// Depends: same as single-task + per-task FP isolation.
JARVIS_TEST(fpu_ctx_user_interleave_isolation, "PRE: none | POST: none") {
    constexpr size_t kN = 3;
    const uint64_t free_before = PMM::free_pages_ref();

    FpuUserWorker w[kN]{};
    bool spawned_ok = true;
    for (uint32_t i = 0; i < kN; ++i) {
        if (!fpu_user_spawn(w[i], i))
            spawned_ok = false;
    }
    JARVIS_ASSERT(spawned_ok);
    Scheduler::reschedule();

    // Phase A: every worker ran FP and blocked before any verification.
    bool alive_ok = true;
    for (size_t i = 0; i < kN; ++i) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *di = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                w[i].data_phys);
        if (!wait_until([&] { return di[kOffAlive / 8] == kAliveMagic; },
                        kJoinBudgetTicks))
            alive_ok = false;
    }
    bool blocked_ok = wait_until(
        [&] {
            return w[0].tcb->state == TaskState::BLOCKED &&
                   w[1].tcb->state == TaskState::BLOCKED &&
                   w[2].tcb->state == TaskState::BLOCKED;
        },
        kJoinBudgetTicks);

    // Phase B: rotated order (A→B→C, B→C→A, C→A→B), waiting for each
    // worker's round marker before moving on.
    bool rounds_ok = true;
    uint64_t sh_rounds[kN] = {0, 0, 0};
    for (uint64_t r = 0; r < kRounds && alive_ok && blocked_ok; ++r) {
        for (size_t k = 0; k < kN; ++k) {
            const size_t i = (r + k) % kN;
            Message msg{};
            msg.sender_id = 0;
            msg.type = 0xF9;
            msg.priority = 0;
            msg.data_size = 0;
            if (!IPC::send(w[i].tcb->id, msg)) {
                rounds_ok = false;
                break;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *di = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                    w[i].data_phys);
            if (!wait_until([&] { return di[kOffRounds / 8] == r + 1; },
                            kJoinBudgetTicks)) {
                rounds_ok = false;
                break;
            }
            sh_rounds[i] = di[kOffRounds / 8];
        }
    }

    // Phase C: observe before teardown (drain frees TCBs).
    bool exited_ok = wait_until(
        [&] {
            return w[0].tcb->state == TaskState::TERMINATED &&
                   w[1].tcb->state == TaskState::TERMINATED &&
                   w[2].tcb->state == TaskState::TERMINATED;
        },
        kJoinBudgetTicks);
    bool reached_exit[kN];
    uint64_t exit_code[kN];
    uint64_t fail_bits[kN];
    bool fpu_used[kN];
    uint32_t gen[kN];
    uint64_t got_lo[kN][3];
#if defined(CONFIG_ARCH_AARCH64)
    uint64_t got_hi[kN][3];
#endif
    for (size_t i = 0; i < kN; ++i) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *data = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                                  w[i].data_phys);
        reached_exit[i] = (data[kOffDone / 8] == kDoneMagic);
        exit_code[i] = w[i].tcb->exit_code;
        fail_bits[i] = data[kOffFail / 8];
        fpu_used[i] = w[i].tcb->fpu_used;
        gen[i] = w[i].tcb->fpu_state_gen;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *area =
            reinterpret_cast<const uint64_t *>(w[i].tcb->fpu_state);
#if defined(CONFIG_ARCH_AARCH64)
        got_lo[i][0] = area[0];
        got_hi[i][0] = area[1];
        got_lo[i][1] = area[2];
        got_hi[i][1] = area[3];
        got_lo[i][2] = area[4];
        got_hi[i][2] = area[5];
#else
        got_lo[i][0] = area[0];
        got_lo[i][1] = area[1];
        got_lo[i][2] = area[2];
#endif
    }

    bool teardown_ok = true;
    fpu_user_teardown(w, kN, reached_exit, free_before, teardown_ok);

    JARVIS_ASSERT(alive_ok);
    JARVIS_ASSERT(blocked_ok);
    JARVIS_ASSERT(rounds_ok);
    JARVIS_ASSERT(exited_ok);
    for (size_t i = 0; i < kN; ++i) {
        JARVIS_ASSERT(reached_exit[i]);
        JARVIS_ASSERT_EQ(0ULL, exit_code[i]);
        JARVIS_ASSERT_EQ(0ULL, fail_bits[i]);
        JARVIS_ASSERT_EQ(3ULL, sh_rounds[i]);
        JARVIS_ASSERT(fpu_used[i]);
        JARVIS_ASSERT(gen[i] >= 4U);
        const uint64_t want[3] = {fpu_lane_pattern(10 + i),
                                  fpu_lane_pattern(20 + i),
                                  fpu_lane_pattern(30 + i)};
        for (int l = 0; l < 3; ++l) {
            JARVIS_ASSERT_FMT(got_lo[i][l] == want[l], "w%u lane %d mismatch",
                              (unsigned)i, l);
#if defined(CONFIG_ARCH_AARCH64)
            JARVIS_ASSERT_FMT(got_hi[i][l] == ~want[l],
                              "w%u lane %d hi mismatch", (unsigned)i, l);
#endif
        }
    }
    JARVIS_ASSERT(teardown_ok);
    JARVIS_TEST_PASS();
}

} // namespace

void register_fpu_context_user_tests() {
    Logger::info("Registering FPU user-task tests");
    JARVIS_REGISTER_TEST(fpu_ctx_user_single_task_persists);
    JARVIS_REGISTER_TEST(fpu_ctx_user_interleave_isolation);
}

#endif // CONFIG_ARCH_AARCH64 || CONFIG_ARCH_RISCV64
