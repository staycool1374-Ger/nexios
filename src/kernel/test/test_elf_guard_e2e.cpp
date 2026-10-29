/*
 * NexIOS RTOS — Live ELF-born guard-abort E2E (issue #435)
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

/// @file test_elf_guard_e2e.cpp
/// @brief #435 live ELF-born guard-abort E2E (aarch64 only): a synthetic
///        user task stores to its own kslot guard page and the harness
///        observes the live fault path — hook-false halt expectation
///        (variant A) and recoverable resume (variant B).  P3 direct-call
///        coverage stays green as regression; this lane makes it live.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/debug/crash_dump.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;

#if defined(CONFIG_ARCH_AARCH64)

namespace {

// Native ELF machine for synthetic headers (mirrors test_elf_loader.cpp).
constexpr uint16_t kNativeEm = 0xB7;

// Probe layout: code page @0x400000, RW data page @0x402000 holding the
// slot-VA placeholder qword at offset 0 (RW pages are RO-verifier exempt,
// so the post-load supervisor patch cannot trip auth re-verify).
constexpr uint64_t kCodeVa = 0x400000ULL;
constexpr uint64_t kDataVa = 0x402000ULL;
constexpr uint64_t kRecoveryVa = 0x400018ULL; // variant B resume label (w6)

// aa64 word builders (field-constructed, objdump-verified precedents in
// test_fpu_context_user.cpp).
constexpr uint32_t a64_movz(uint32_t rd, uint32_t imm16) {
    return 0xD2800000U | ((imm16 & 0xFFFFU) << 5) | (rd & 31U);
}
constexpr uint32_t a64_ldr_literal(uint32_t rd, uint32_t bytes_from_pc) {
    return 0x58000000U | (((bytes_from_pc / 4U) & 0x7FFFFU) << 5) |
           (rd & 31U);
}
constexpr uint32_t a64_str_x0_x1() { return 0xF9000020U; } // str x0,[x1]
constexpr uint32_t A64_MOV_X8_EXIT = 0xD28000C8U;          // mov x8,#6 EXIT
constexpr uint32_t A64_SVC = 0xD4000001U;                 // svc #0
constexpr uint32_t A64_B_SELF = 0x14000000U;              // b . (safety net)
constexpr uint32_t A64_NOP = 0xD503201FU;

static_assert(a64_movz(0, 0xBEEF) == 0xD297DDE0U, "movz x0,#0xBEEF");
static_assert(a64_ldr_literal(1, 0x2000) == 0x58010001U, "ldr x1,[pc,#0x2000]");
static_assert(a64_str_x0_x1() == 0xF9000020U, "str x0,[x1]");

// Probe words 0-8 (variant A executes 0-5 then re-faults; variant B
// resumes at w6 after handler recovery and exits 0).
// w0: ldr x1,[pc,#0x2000] -> slot VA (supervisor-patched placeholder)
// w1: movz x0,#0xBEEF (probe marker)
// w2: str x0,[x1] (guard fault: EC 0x24, FAR = slot VA)
// w3: movz x0,#0xDEAD (skipped-if-faulted marker)
// w4: mov x8,#6
// w5: svc #0 (exit 0xDEAD — must never be reached)
// w6: movz x0,#0 (RECOVERY label)
// w7: mov x8,#6
// w8: svc #0 (exit 0)
inline size_t emit_probe_code(uint32_t *code) {
    code[0] = a64_ldr_literal(1, 0x2000);
    code[1] = a64_movz(0, 0xBEEF);
    code[2] = a64_str_x0_x1();
    code[3] = a64_movz(0, 0xDEAD);
    code[4] = A64_MOV_X8_EXIT;
    code[5] = A64_SVC;
    code[6] = a64_movz(0, 0);
    code[7] = A64_MOV_X8_EXIT;
    code[8] = A64_SVC;
    code[9] = A64_B_SELF;
    return 10;
}

// Build the two-segment probe image (R+X code + RW data, ET_EXEC entry
// 0x400000 — mirrors build_rw_elf in test_elf_loader.cpp).
uint64_t build_guard_probe_elf(elf::ELF64Header *hdr, uint8_t *data) {
    __builtin_memset(hdr, 0, sizeof(elf::ELF64Header));
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_EXEC;
    hdr->machine = kNativeEm;
    hdr->version = 1;
    hdr->entry = kCodeVa;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 2;
    auto *ph0 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header));
    ph0->type = elf::PT_LOAD;
    ph0->flags = elf::PF_R | elf::PF_X;
    ph0->offset = sizeof(elf::ELF64Header) +
                  2 * sizeof(elf::ELF64ProgramHeader);
    ph0->vaddr = kCodeVa;
    ph0->paddr = kCodeVa;
    ph0->filesz = 0x1000;
    ph0->memsz = 0x1000;
    ph0->align = 0x1000;
    auto *ph1 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader));
    ph1->type = elf::PT_LOAD;
    ph1->flags = elf::PF_R | elf::PF_W;
    ph1->offset = ph0->offset + 0x1000;
    ph1->vaddr = kDataVa;
    ph1->paddr = kDataVa;
    ph1->filesz = 0x100;
    ph1->memsz = 0x1000;
    ph1->align = 0x1000;
    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
    uint32_t *code =
        reinterpret_cast<uint32_t *>(data + ph0->offset);
    size_t words = emit_probe_code(code);
    for (size_t i = words; i < 0x1000 / 4; ++i)
        code[i] = A64_NOP;
    uint64_t *rw = reinterpret_cast<uint64_t *>(data + ph1->offset);
    for (size_t i = 0; i < 0x100 / 8; ++i)
        rw[i] = static_cast<uint64_t>(i);
    rw[0] = 0x0BAD0BAD0BAD0BADULL; // slot-VA placeholder (patched post-load)
    return ph1->offset + 0x100;
}

// Patch one u64 of the LOADED image through the kernel HHDM alias of the
// user page (RO-verifier exempt: placeholder lives in the RW data page).
// Mirrors the loader_tamper_killed alias pattern.
void patch_loaded_u64(TaskControlBlock *t, uint64_t uva, uint64_t value) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(uva, t->get_page_table());
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *alias = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    __builtin_memcpy(alias, &value, sizeof(value));
}

// Bounded dispatch spin (runelf template: TERMINATED-required, no vacuous
// pass; yields via need_resched + hlt so the probe actually runs).
// Watchdog-friendly: periodic serial progress (a silent hlt-spin trips
// the host stall monitor), and breaks on TERMINATED even when no halt
// was observed (converts a hang into a loud FAIL with state diagnostics).
void spin_probe(TaskControlBlock *t, int bound = 20000) {
    for (int i = 0; i < bound; ++i) {
        if (!TaskControlBlock::is_valid(t) ||
            t->state == TaskState::TERMINATED ||
            t->state == TaskState::REAPED)
            break;
        if ((i % 2000) == 0)
            Logger::info("guard-e2e: waiting for probe, iter=%d", i);
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

} // namespace

extern uint64_t stack_overflow_hook_latched_task();
extern uint64_t stack_overflow_hook_latched_va();
extern void stack_overflow_hook_reset_latch();
extern void stack_overflow_hook_set_force_false(bool);

// Runmode: kernel
// Testidea: #435 variant A — a live ELF-born user task stores to its own
// kslot guard page; with the test hook forced false the fault takes the
// first-chance halt path (CONFIG_DEBUG seam returns, eret re-faults in a
// bounded loop) and the supervisor observes halt + frame + latch, then
// terminates without hang.
// Input: synthetic probe ELF (faulting store to patched slot VA);
// hook-false armed, crash-halt seam armed, recover disarmed.
// Expect: halt_count >= 1; last frame STACK_OVERFLOW + addr in
// [slot, slot+4096); latched task id == probe id + VA == slot;
// termination-without-hang (bounded join + reap).
// Depends: dtb-independent; aarch64_el1_unexpected_fault guard arm;
// test hook force-false switch; crash-halt seam.
#if CONFIG_DEBUG && CONFIG_STACK_OVERFLOW_HOOK
JARVIS_TEST(aarch64_elf_guard_abort_e2e_halt, "PRE: none | POST: none") {
    JARVIS_ASSERT(kernel::Scheduler::is_test_active());
    // Start-reset (assert-fail returns, so end-reset never runs on red).
    stack_overflow_hook_set_force_false(false);
    stack_overflow_hook_reset_latch();
    kernel::gs::user_access_recover_ip() = 0;
    kernel::debug::test_reset_crash_halt();
    kernel::debug::test_arm_crash_halt();

    elf::ELF64Header hdr{};
    uint8_t img[8192];
    uint64_t sz = build_guard_probe_elf(&hdr, img);
    auto *t = kernel::elf::load(&hdr, img, sz);
    JARVIS_ASSERT(t != nullptr);
    uint64_t slot = t->kstack_slot_va_;
    JARVIS_ASSERT_FMT(slot != 0,
                      "probe has no kslot (HHDM fallback) — no live guard");
    JARVIS_ASSERT(kernel::is_stack_guard_fault(*t, slot));
    patch_loaded_u64(t, kDataVa, slot);

    stack_overflow_hook_set_force_false(true);
    Scheduler::add_task(*t);
    for (int i = 0; i < 20000; ++i) {
        if (kernel::debug::test_crash_halt_count() >= 1)
            break;
        if (!TaskControlBlock::is_valid(t))
            break;
        if (t->state == TaskState::TERMINATED ||
            t->state == TaskState::REAPED)
            break;
        if ((i % 2000) == 0)
            Logger::info("guard-e2e-halt: iter=%d halt=%lu state=%u", i,
                         kernel::debug::test_crash_halt_count(),
                         static_cast<unsigned>(t->state));
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
    stack_overflow_hook_set_force_false(false);

    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    JARVIS_ASSERT_FMT(t->state != TaskState::TERMINATED,
                      "probe TERMINATED (exit 0x%lx) — store did not fault",
                      t->exit_code);
    JARVIS_ASSERT_FMT(kernel::debug::test_crash_halt_count() >= 1,
                      "no live guard halt observed (halt count %lu)",
                      kernel::debug::test_crash_halt_count());
    kernel::debug::FirstChanceFrame f = kernel::debug::test_last_frame();
    JARVIS_ASSERT_FMT(
        f.cause == kernel::debug::FirstChanceCause::STACK_OVERFLOW,
        "halt cause %u, want STACK_OVERFLOW",
        static_cast<unsigned>(f.cause));
    JARVIS_ASSERT_FMT(f.addr >= slot && f.addr < slot + 4096,
                      "halt addr 0x%lx outside guard [0x%lx, +0x1000)",
                      f.addr, slot);
    JARVIS_ASSERT_FMT(stack_overflow_hook_latched_task() == t->id,
                      "latched task %lu, want probe %lu",
                      stack_overflow_hook_latched_task(), t->id);
    JARVIS_ASSERT_FMT(stack_overflow_hook_latched_va() == slot,
                      "latched VA 0x%lx, want guard 0x%lx",
                      stack_overflow_hook_latched_va(), slot);

    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    kernel::debug::test_reset_crash_halt();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #435 variant B — same live faulting store, but the supervisor
// arms the global recover IP to the resume label past the store; the
// handler recovers (hook never consulted), the probe exits 0, nothing
// halts.  Single-global arm window spans scheduling — acceptable
// single-CPU with the loader quiesced (no guarded copies in flight);
// a per-task recover slot is P4-adjacent product work, not this lane.
// Input: probe ELF; recover_ip = resume VA; hook default (true).
// Expect: TERMINATED + exit_code 0 (recovery observed, skip-marker
// 0xDEAD never exits); halt_count == 0; recover consumed (== 0 after).
// Depends: aarch64_el1_unexpected_fault recover-first path;
// user_access_recover_ip accessor.
JARVIS_TEST(aarch64_elf_guard_abort_e2e_recoverable,
            "PRE: none | POST: none") {
    JARVIS_ASSERT(kernel::Scheduler::is_test_active());
    stack_overflow_hook_set_force_false(false);
    stack_overflow_hook_reset_latch();
    kernel::gs::user_access_recover_ip() = 0;
    kernel::debug::test_reset_crash_halt();
    kernel::debug::test_arm_crash_halt();

    elf::ELF64Header hdr{};
    uint8_t img[8192];
    uint64_t sz = build_guard_probe_elf(&hdr, img);
    auto *t = kernel::elf::load(&hdr, img, sz);
    JARVIS_ASSERT(t != nullptr);
    uint64_t slot = t->kstack_slot_va_;
    JARVIS_ASSERT_FMT(slot != 0,
                      "probe has no kslot (HHDM fallback) — no live guard");
    JARVIS_ASSERT(kernel::is_stack_guard_fault(*t, slot));
    patch_loaded_u64(t, kDataVa, slot);

    kernel::gs::user_access_recover_ip() = kRecoveryVa;
    Scheduler::add_task(*t);
    spin_probe(t);

    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    JARVIS_ASSERT_FMT(t->state == TaskState::TERMINATED,
                      "probe not TERMINATED (state %u) — recovery missed",
                      static_cast<unsigned>(t->state));
    JARVIS_ASSERT_FMT(t->exit_code == 0,
                      "exit_code 0x%lx, want 0 (recovery label reached)",
                      t->exit_code);
    JARVIS_ASSERT_FMT(kernel::debug::test_crash_halt_count() == 0,
                      "unexpected halt (count %lu) on recover path",
                      kernel::debug::test_crash_halt_count());
    JARVIS_ASSERT_FMT(kernel::gs::user_access_recover_ip() == 0,
                      "recover IP not consumed (0x%lx)",
                      kernel::gs::user_access_recover_ip());

    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    kernel::debug::test_reset_crash_halt();
    JARVIS_TEST_PASS();
}
#endif // CONFIG_DEBUG && CONFIG_STACK_OVERFLOW_HOOK

void register_elf_guard_e2e_tests() {
#if CONFIG_DEBUG && CONFIG_STACK_OVERFLOW_HOOK
    JARVIS_REGISTER_TEST(aarch64_elf_guard_abort_e2e_halt);
    JARVIS_REGISTER_TEST(aarch64_elf_guard_abort_e2e_recoverable);
#endif
}

#else // !CONFIG_ARCH_AARCH64

void register_elf_guard_e2e_tests() {}

#endif // CONFIG_ARCH_AARCH64
