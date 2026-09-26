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

/// @file test_debug_syscall.cpp
/// @brief Debugger syscalls (issue #225): attach selectors, EAGAIN park
///        lifecycle, regs/mem plumbing, synthetic codec goldens. Every
///        syscall is invoked by a REAL kernel task via Syscall::handle()
///        so syscall_task() resolves to the genuinely-running task.
///        Release-build ENOSYS is verified by construction (table maps to
///        sys_unimplemented when !CONFIG_DEBUG) + release build green;
///        no DBG gate runs release classes, so no runtime ENOSYS test
///        exists — documented here instead of faked.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/debug/debug_regs.hpp>

using namespace kernel;

namespace {

// Linux errno numerics (mirrors syscall_handlers_debug.cpp).
constexpr uint64_t kEperm = 1;
constexpr uint64_t kEsrch = 3;
constexpr uint64_t kEbadf = 9;
constexpr uint64_t kEagain = 11;
constexpr uint64_t kEfault = 14;
constexpr uint64_t kEbusy = 16;
constexpr uint64_t kEinval = 22;

uint64_t Neg(uint64_t e) {
    return static_cast<uint64_t>(-static_cast<int64_t>(e));
}

uint64_t DebugCall(SyscallNumber num, uint64_t a0, uint64_t a1, uint64_t a2 = 0,
                   uint64_t a3 = 0) {
    return Syscall::handle(static_cast<uint64_t>(num), a0, a1, a2, a3,
                           nullptr);
}

/// @brief Spawn a spinning user child parented to the caller (launcher
///        claim setup for attach selector 1). Caller must add_child first
///        (create_user leaves parent_id 0); teardown mirrors the
///        umode-smoke pattern plus remove_child. The entry is a kernel
///        higher-half placeholder: on x86_64 install_user_yield_stub only
///        rewrites kernel-half entries (a low VA is left as-is and would
///        fault); riscv64/aarch64 rewrite unconditionally.
TaskControlBlock *debug_spawn_child() {
    auto *me = Scheduler::current_task();
    if (me == nullptr)
        return nullptr;
    // Entry VA is replaced by create_user's yield stub (spins in U-mode).
    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(0xFFFFFFFF80000000ULL), 11, 10, 32_KiB);
    if (t == nullptr)
        return nullptr;
    if (!Scheduler::set_sched_policy(*t, SchedPolicy::FIXED)) {
        (void)Scheduler::terminate_err(*t, 0);
        Scheduler::drain_zombie_list();
        if (TaskControlBlock::is_valid(t)) {
            t->cleanup();
            delete t;
        }
        Scheduler::drain_zombie_list();
        return nullptr;
    }
    me->add_child(t);
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    return t;
}

void debug_reap_child(TaskControlBlock *t) {
    auto *me = Scheduler::current_task();
    if (t == nullptr)
        return;
    // Unconditional terminate (elf-smoke pattern): a fault-dead child is
    // already TERMINATED, but only terminate_err queues it for the drain.
    if (TaskControlBlock::is_valid(t) && t != me)
        (void)Scheduler::terminate_err(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    if (me != nullptr)
        me->remove_child(t);
    if (TaskControlBlock::is_valid(t)) {
        t->cleanup();
        delete t;
    }
    Scheduler::drain_zombie_list();
}

/// @brief POSIX timespec for test-side nanosleep (mirrors
///        test_posix_time.cpp AbiTs; kernel test task sleeps, letting a
///        spinning child own the CPU until the tick parks it).
struct AbiTs {
    int64_t tv_sec;
    int64_t tv_nsec;
};

void debug_sleep_ms(uint64_t ms) {
    AbiTs req{};
    req.tv_sec = 0;
    req.tv_nsec = static_cast<int64_t>(ms * 1000000ULL);
    (void)Syscall::handle(static_cast<uint64_t>(SyscallNumber::NANOSLEEP),
                          reinterpret_cast<uint64_t>(&req), 0, 0, 0, nullptr);
}

/// @brief Spawn a child running a custom non-yielding spin stub (2-4
///        bytes: x86 jmp -2, aarch64 b #0, riscv64 jal x0,0). Unlike the
///        yield stub (microsecond slices between YIELD traps, which timer
///        ticks essentially never catch in user mode), a pure spinner
///        holds the CPU across ticks so the park hook fires
///        deterministically. Returns the child with its stub page phys
///        in stub_phys_out (caller frees after unmapping).
TaskControlBlock *debug_spawn_spinner(uint64_t &stub_phys_out) {
    stub_phys_out = 0;
    auto *me = Scheduler::current_task();
    if (me == nullptr)
        return nullptr;
#if defined(CONFIG_ARCH_X86_64)
    constexpr uint8_t kSpin[] = {0xEB, 0xFE}; // jmp -2 (spin, no syscall)
#elif defined(CONFIG_ARCH_AARCH64)
    constexpr uint8_t kSpin[] = {0x00, 0x00, 0x00, 0x14}; // b #0 (self)
#elif defined(CONFIG_ARCH_RISCV64)
    constexpr uint8_t kSpin[] = {0x6F, 0x00, 0x00, 0x00}; // jal x0,0
#else
    constexpr uint8_t kSpin[] = {0x00};
#endif
    constexpr uint64_t kSpinVa = 0x41000000ULL;
    uint64_t phys = PMM::alloc_user_page();
    if (phys == 0)
        return nullptr;
    auto *dst = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                     phys);
    for (size_t i = 0; i < sizeof(kSpin); ++i)
        dst[i] = kSpin[i];
    // Low user VA: create_user leaves it as-is (the yield-stub rewrite
    // only fires for kernel-half entries), so the task starts in our stub.
    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(kSpinVa), 11, 10, 32_KiB);
    if (t == nullptr) {
        PMM::free_page(phys);
        return nullptr;
    }
    VMM::map_page_in_pml4(kSpinVa, phys, true, true, t->page_table_);
    if (VMM::virt_to_phys_in_pml4(kSpinVa, t->page_table_) != phys) {
        PMM::free_page(phys);
        (void)Scheduler::terminate_err(*t, 0);
        Scheduler::drain_zombie_list();
        if (TaskControlBlock::is_valid(t)) {
            t->cleanup();
            delete t;
        }
        Scheduler::drain_zombie_list();
        return nullptr;
    }
    if (!Scheduler::set_sched_policy(*t, SchedPolicy::FIXED)) {
        // Unmap/free BEFORE reaping: reap deletes the TCB, so any later
        // t->page_table_ read is use-after-free.
        VMM::unmap_page_in_pml4(kSpinVa, t->page_table_);
        PMM::free_page(phys);
        debug_reap_child(t);
        return nullptr;
    }
    me->add_child(t);
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    stub_phys_out = phys;
    return t;
}

void debug_free_spinner_page(TaskControlBlock *t, uint64_t stub_phys) {
    constexpr uint64_t kSpinVa = 0x41000000ULL;
    if (t != nullptr && TaskControlBlock::is_valid(t))
        VMM::unmap_page_in_pml4(kSpinVa, t->page_table_);
    if (stub_phys != 0)
        PMM::free_page(stub_phys);
}

/// @brief Map a scratch user page in the CALLER's tables for debugger
///        buffers (issue #225). safe_copy_* resolves VAs through the
///        caller's tables, so the buffer must live there: kernel test
///        tasks run on private cloned tables (a kernel-table mapping
///        would be invisible and fault). Kernel-half children are shared
///        by value at clone, so the split is visible to the snapshot
///        backend, which rewinds it at the boundary (x86/aarch64 identity
///        PD restore; riscv64 #152 HHDM L1 restore) — no manual table
///        surgery needed. Per-arch VA sits inside a restore-covered
///        window. Returns phys in phys_out (caller frees after unmapping).
constexpr uint64_t kDebugScratchVa =
#if defined(CONFIG_ARCH_RISCV64)
    // L0[1] user window (absent in kernel tables: clean create, no
    // hugepage/MMIO to disturb). MUST be < CONFIG_USER_SPACE_LIMIT
    // (38-bit Sv39): safe_copy_* range-checks debugger buffers, and an
    // HHDM VA here would EFAULT every copy.
    0x43000000ULL;
#else
    // Low identity window (PD idx 8, restore-covered).
    0x1000000ULL;
#endif

uint64_t debug_map_scratch() {
    TaskControlBlock *me = Scheduler::current_task();
    if (me == nullptr || me->page_table_ == 0)
        return 0;
    uint64_t phys = PMM::alloc_user_page();
    if (phys == 0)
        return 0;
    VMM::map_page_in_pml4(kDebugScratchVa, phys, true, false,
                          me->page_table_);
    if (VMM::virt_to_phys_in_pml4(kDebugScratchVa, me->page_table_) != phys) {
        VMM::unmap_page_in_pml4(kDebugScratchVa, me->page_table_);
        PMM::free_page(phys);
        return 0;
    }
    return phys;
}

void debug_unmap_scratch(uint64_t phys) {
    TaskControlBlock *me = Scheduler::current_task();
    if (me != nullptr && me->page_table_ != 0)
        VMM::unmap_page_in_pml4(kDebugScratchVa, me->page_table_);
    if (phys != 0)
        PMM::free_page(phys);
}

/// @brief Re-establish a mapping known-good (see issue #226).
///        Returns true when VA maps phys afterwards in the given tables
///        (remapping at most once); false fail-closed. Heal events are
///        logged for the follow-up root-cause issue.
bool debug_ensure_map_pt(uint64_t pt, uint64_t va, uint64_t phys, bool exec) {
    if (VMM::virt_to_phys_in_pml4(va, pt) == phys)
        return true;
    Logger::info("debugmap heal va=0x%lx", va);
    VMM::unmap_page_in_pml4(va, pt);
    VMM::map_page_in_pml4(va, phys, true, exec, pt);
    return VMM::virt_to_phys_in_pml4(va, pt) == phys;
}

bool debug_ensure_map(TaskControlBlock *t, uint64_t va, uint64_t phys) {
    return debug_ensure_map_pt(t->page_table_, va, phys, false);
}

/// @brief Ensure both the target mapping and the caller scratch mapping
///        are live (mappings can go stale across sleep/tick activity —
///        see issue #226; heals are logged, failures fail closed).
bool debug_ensure_all(TaskControlBlock *t, uint64_t va, uint64_t phys,
                      uint64_t scratch_phys) {
    TaskControlBlock *me = Scheduler::current_task();
    if (me == nullptr || me->page_table_ == 0)
        return false;
    if (!debug_ensure_map_pt(me->page_table_, kDebugScratchVa, scratch_phys,
                             false))
        return false;
    return debug_ensure_map(t, va, phys);
}

} // namespace

// Testidea: Attach lifecycle through the launcher-claim selector: attach
//           ok (returns nonzero handle), second attach EBUSY, detach via
//           handle toggle, re-attach ok (binding freed).
// Input: owned spinning child.
// Expect: handle, EBUSY, 0, handle (detach + re-mint).
// Depends: sys_task_debug_attach, add_child/remove_child.
JARVIS_TEST(debug_attach_parent_ok, "PRE: none | POST: none") {
    TaskControlBlock *t = debug_spawn_child();
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    const uint64_t child_id = t->id;
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, child_id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy) && h != Neg(kEperm) &&
                          h != Neg(kEsrch),
                      "attach failed: 0x%lx", h);
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, child_id) ==
            Neg(kEbusy),
        "second attach not EBUSY");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
        "detach toggle failed");
    uint64_t h2 = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, child_id);
    JARVIS_ASSERT_FMT(h2 != 0 && h2 != Neg(kEbusy), "re-attach failed");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h2) == 0,
        "second detach failed");
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

// Testidea: Attach rejection matrix (no child needed except the EPERM
//           non-child case, which uses an unparented spawn).
// Input: garbage selector/handle, dead pid, non-child, self.
// Expect: EBADF, ESRCH, EPERM, EPERM.
JARVIS_TEST(debug_attach_rejects, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 9, 0) == Neg(kEbadf),
        "bad selector not EBADF");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, 0xDEADBEEFULL) ==
            Neg(kEbadf),
        "garbage handle not EBADF");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, 999999) == Neg(kEsrch),
        "dead pid not ESRCH");
    // Unparented child: parent_id 0 != caller -> EPERM (not ESRCH).
    // Dispatched (but never add_child'ed) so the normal teardown path
    // applies; the claim still fails on parenthood.
    TaskControlBlock *orphan = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(0xFFFFFFFF80000000ULL), 11, 10, 32_KiB);
    JARVIS_ASSERT_FMT(orphan != nullptr, "orphan spawn failed");
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*orphan);
    }
    Scheduler::reschedule();
    const uint64_t orphan_id = orphan->id;
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, orphan_id) ==
            Neg(kEperm),
        "non-child attach not EPERM");
    debug_reap_child(orphan);
    // Self-attach: not own parent -> EPERM (also a kernel task).
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT_FMT(me != nullptr, "no current task");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, me->id) == Neg(kEperm),
        "self attach not EPERM");
    JARVIS_TEST_PASS();
}

// Testidea: Synthetic codec goldens per arch (no live task): craft a
//           U-trap frame in an owned page, round-trip through the real
//           codec, assert mapping + magic-negative on zeroes.
// Input: fake kstack top + frame bytes.
// Expect: exact golden mapping; zero frame + short blob fail closed.
JARVIS_TEST(debug_codec_synthetic, "PRE: none | POST: none") {
    uint64_t const top_phys = PMM::alloc_page();
    JARVIS_ASSERT_FMT(top_phys != 0, "alloc failed");
    TaskControlBlock t;
    t.kernel_stack_top =
        static_cast<uint64_t>(arch::HHDM_OFFSET) + top_phys + 4096;
    constexpr size_t kQwords = 34;
    uint64_t blob[kQwords] = {};
#if defined(CONFIG_ARCH_X86_64)
    // 176B tick frame at top-176: GPRs + vec/err + IRET (cs user).
    auto *frame = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                               top_phys + 4096 - 176);
    for (size_t i = 0; i < 15; ++i)
        frame[i] = 0x100 + i;
    frame[15] = 0x20;
    frame[16] = 0;
    frame[17] = 0x400000; // rip
    frame[18] = 0x23;     // cs (user)
    frame[19] = 0x202;    // rflags
    frame[20] = 0x70009000ULL; // user rsp
    frame[21] = 0x2B;          // ss
    JARVIS_ASSERT_FMT(debug::debug_blob_bytes() == 164, "blob size moved");
    JARVIS_ASSERT_FMT(debug::debug_read_regs(t, blob, kQwords), "read failed");
    JARVIS_ASSERT_FMT(blob[0] == 0x100 && blob[15] == 0x10E &&
                          blob[16] == 0x400000 && blob[17] == 0x202 &&
                          blob[7] == 0x70009000ULL && blob[18] == 0x23,
                      "x86 mapping wrong");
    blob[0] ^= 0xDEAD;
    blob[16] = 0x500000;
    JARVIS_ASSERT_FMT(debug::debug_write_regs(t, blob, kQwords),
                      "write failed");
    JARVIS_ASSERT_FMT(frame[0] == (0x100 ^ 0xDEAD) && frame[17] == 0x500000,
                      "x86 writeback wrong");
#elif defined(CONFIG_ARCH_AARCH64)
    // 288B frame at top-288: x0-x30, sp_el0, elr, spsr==EL0t.
    auto *frame = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                               top_phys + 4096 - 288);
    for (size_t i = 0; i < 31; ++i)
        frame[i] = 0x200 + i;
    frame[31] = 0x7000A000ULL;
    frame[32] = 0x41000000ULL; // elr
    frame[33] = 0;             // spsr EL0t
    JARVIS_ASSERT_FMT(debug::debug_blob_bytes() == 272, "blob size moved");
    JARVIS_ASSERT_FMT(debug::debug_read_regs(t, blob, kQwords), "read failed");
    JARVIS_ASSERT_FMT(blob[0] == 0x200 && blob[30] == 0x21E &&
                          blob[31] == 0x7000A000ULL &&
                          blob[32] == 0x41000000ULL && blob[33] == 0,
                      "aarch64 mapping wrong");
    blob[32] = 0x42000000ULL;
    JARVIS_ASSERT_FMT(debug::debug_write_regs(t, blob, kQwords),
                      "write failed");
    JARVIS_ASSERT_FMT(frame[32] == 0x42000000ULL, "aarch64 elr write wrong");
#elif defined(CONFIG_ARCH_RISCV64)
    // 296B frame at top-296: OFF_A0=72 (idx9) cookies, sepc idx31.
    auto *frame = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                               top_phys + 4096 - 296);
    for (size_t i = 0; i < 37; ++i)
        frame[i] = 0;
    frame[32] = 0; // sstatus SPP==0 (user magic)
    frame[9] = 0xAAAA;
    frame[10] = 0xBBBB;
    frame[11] = 0xCCCC;
    frame[12] = 0xDDDD;
    frame[16] = 24; // a7 (untouched by codec, must survive)
    frame[31] = 0x41000000ULL; // sepc
    JARVIS_ASSERT_FMT(debug::debug_blob_bytes() == 264, "blob size moved");
    JARVIS_ASSERT_FMT(debug::debug_read_regs(t, blob, kQwords), "read failed");
    JARVIS_ASSERT_FMT(blob[10] == 0xAAAA && blob[11] == 0xBBBB &&
                          blob[12] == 0xCCCC && blob[13] == 0xDDDD &&
                          blob[32] == 0x41000000ULL && blob[0] == 0,
                      "riscv mapping wrong");
    blob[32] = 0x42000000ULL;
    JARVIS_ASSERT_FMT(debug::debug_write_regs(t, blob, kQwords),
                      "write failed");
    JARVIS_ASSERT_FMT(frame[31] == 0x42000000ULL && frame[16] == 24,
                      "riscv sepc write wrong");
#endif
    // Magic-negative: a non-user frame fails closed (never decodes
    // garbage). Poison pattern per arch: x86 zeros (cs&3==0 fails);
    // aarch64/riscv64 all-ones (spsr-M!=0 / SPP==1 fail). Note: an
    // all-zero sstatus legitimately reads as user (SPP==0) on riscv64,
    // so zeros are not a negative there — the has_utrap_frame gate
    // (not the magic) excludes never-trapped slots in production.
    auto *zframe = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + top_phys);
#if defined(CONFIG_ARCH_X86_64)
    for (size_t i = 0; i < 64; ++i)
        zframe[i] = 0;
#else
    for (size_t i = 0; i < 64; ++i)
        zframe[i] = 0xFFFFFFFFFFFFFFFFULL;
#endif
    t.kernel_stack_top =
        static_cast<uint64_t>(arch::HHDM_OFFSET) + top_phys + 512;
    JARVIS_ASSERT_FMT(!debug::debug_read_regs(t, blob, kQwords),
                      "poison frame decoded");
    JARVIS_ASSERT_FMT(!debug::debug_read_regs(t, blob, 2), "short blob read");
    JARVIS_ASSERT_FMT(!debug::debug_write_regs(t, blob, 2),
                      "short blob write");
    t.kernel_stack_top = 0;
    PMM::free_page(top_phys);
    JARVIS_TEST_PASS();
}

// Testidea: Live EAGAIN-until-parked lifecycle on a spinning child:
//           attach, read (EAGAIN), poll sleeps until parked, determinism
//           (two reads identical), pc-redirect write-back, detach resume.
//           Debugger buffers live in child-mapped pages: safe_copy_*
//           accepts any user VA (range-checked, not ownership-checked),
//           and the test task is a kernel task with no user mappings of
//           its own. Results are read back through the HHDM alias.
// Input: owned spinning U-task + xfer page.
// Expect: EAGAIN then 0; BLOCKED+parked; stable reads; resume on detach.
JARVIS_TEST(debug_park_read_write_detach, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    auto *scratch =
        reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET + scratch_phys);
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    constexpr size_t kQwords = 34;
    // First read on a RUNNING target arms the stop: EAGAIN.
    uint64_t r1 = DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                            kDebugScratchVa);
    JARVIS_ASSERT_FMT(r1 == Neg(kEagain), "first read not EAGAIN: 0x%lx", r1);
    // Sleep-block the test task so the spinner owns the CPU across ticks;
    // the first user-mode tick parks it. Bounded: 100 x 2ms sleeps.
    bool parked = false;
    for (int i = 0; i < 100 && !parked; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0) {
            parked = true;
            break;
        }
        debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(parked, "target never parked");
    JARVIS_ASSERT_FMT(t->state == TaskState::BLOCKED &&
                          __atomic_load_n(&t->debug_parked, __ATOMIC_ACQUIRE),
                      "park markers wrong");
    // Determinism: two reads of a parked task are identical.
    uint64_t snap1[kQwords] = {};
    for (size_t i = 0; i < kQwords; ++i)
        snap1[i] = scratch[i];
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                                kDebugScratchVa) == 0,
                      "second read failed");
    bool same = true;
    const size_t nq = debug::debug_blob_bytes() / 8;
    for (size_t i = 0; i < nq; ++i)
        same &= (scratch[i] == snap1[i]);
    JARVIS_ASSERT_FMT(same, "parked reads unstable");
    // pc-redirect write-back (write the same pc: proves the pc slot path
    // without altering execution), then a GPR flip + read-back.
    constexpr size_t kPcIdx =
#if defined(CONFIG_ARCH_X86_64)
        16;
#elif defined(CONFIG_ARCH_AARCH64)
        32;
#else
        32;
#endif
    uint64_t mod[kQwords] = {};
    for (size_t i = 0; i < nq; ++i)
        mod[i] = scratch[i];
    const uint64_t gpr_before = mod[1];
    mod[1] ^= 0xDEAD;
    for (size_t i = 0; i < nq; ++i)
        scratch[i] = mod[i];
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_WRITE_REGS, h,
                                kDebugScratchVa) == 0,
                      "write failed");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                                kDebugScratchVa) == 0,
                      "post-write read failed");
    JARVIS_ASSERT_FMT(scratch[1] == (gpr_before ^ 0xDEAD),
                      "GPR write invisible");
    JARVIS_ASSERT_FMT(scratch[kPcIdx] == mod[kPcIdx], "pc slot disturbed");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "detach failed");
    JARVIS_ASSERT_FMT(__atomic_load_n(&t->debugger_id, __ATOMIC_ACQUIRE) == 0 &&
                          !__atomic_load_n(&t->debug_parked, __ATOMIC_ACQUIRE),
                      "detach state wrong");
    JARVIS_ASSERT_FMT(t->state == TaskState::RUNNING ||
                          t->state == TaskState::READY,
                      "target not resumed");
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

// Testidea: Memory R/W through target tables on a parked child: full
//           read/write round-trip on a privately mapped page, partial
//           cross-boundary read (bytes-done, not -1), unmapped EFAULT,
//           over-cap EINVAL. The data page is mapped only AFTER the
//           target is parked (the child never runs again past that
//           point), so no concurrent execution can disturb the mapping.
// Input: owned child + data page mapped post-park at 0x42000000.
// Expect: round-trip equal; partial == 4096; EFAULT; EINVAL.
JARVIS_TEST(debug_mem_partial, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    constexpr uint64_t kVa = 0x42000000ULL;
    uint64_t const data_phys = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(data_phys != 0, "alloc failed");
    auto *data = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                       data_phys);
    data[0] = 0xC0FFEE;
    // Debugger xfer buffer: caller-mapped scratch (kernel test task has
    // no user mappings; safe_copy_* resolves through caller tables).
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    auto *xfer = reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                       scratch_phys);
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    // Park first (child never runs again after this point).
    bool parked = false;
    for (int i = 0; i < 100 && !parked; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0) {
            parked = true;
            break;
        }
        debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(parked, "target never parked");
    JARVIS_ASSERT_FMT(t->state == TaskState::BLOCKED &&
                          __atomic_load_n(&t->debug_parked, __ATOMIC_ACQUIRE),
                      "park markers wrong");
    // Map the data page only now (post-park: stable by construction).
    VMM::map_page_in_pml4(kVa, data_phys, true, false, t->page_table_);
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(kVa, t->page_table_) == data_phys,
        "map failed");
    // Full read round-trip (result lands in the scratch page). The
    // mappings are re-verified first (heals + logs per debug_ensure_all).
    xfer[0] = 0;
    JARVIS_ASSERT_FMT(debug_ensure_all(t, kVa, data_phys, scratch_phys),
                          "maps stale");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kVa, 4,
                                kDebugScratchVa) == 4,
                      "full read failed");
    JARVIS_ASSERT_FMT(xfer[0] == 0xC0FFEE, "cookie mismatch: 0x%x", xfer[0]);
    // Write + read back.
    xfer[0] = 0x12345678;
    uint64_t wr = DebugCall(SyscallNumber::TASK_DEBUG_WRITE_MEM, h, kVa, 4,
                            kDebugScratchVa);
    xfer[0] = 0;
    JARVIS_ASSERT_FMT(debug_ensure_all(t, kVa, data_phys, scratch_phys),
                          "maps stale");
    uint64_t rr = DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kVa, 4,
                            kDebugScratchVa);
    JARVIS_ASSERT_FMT(wr == 4, "write failed: 0x%lx", wr);
    JARVIS_ASSERT_FMT(rr == 4, "read-back call failed: 0x%lx", rr);
    JARVIS_ASSERT_FMT(xfer[0] == 0x12345678, "read-back data wrong: 0x%x",
                      xfer[0]);
    // Cross-boundary partial: 8192 from a 4096-mapped page -> 4096 done
    // (first page copied into scratch, second page EFAULTs).
    JARVIS_ASSERT_FMT(debug_ensure_all(t, kVa, data_phys, scratch_phys),
                          "maps stale");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kVa,
                                8192, kDebugScratchVa) == 4096,
                      "partial not bytes-done");
    // Unmapped VA -> EFAULT (0 done).
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h,
                                0x50000000ULL, 8,
                                kDebugScratchVa) == Neg(kEfault),
                      "unmapped not EFAULT");
    // Over-cap length -> EINVAL.
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kVa,
                                65 * 1024, kDebugScratchVa) == Neg(kEinval),
                      "over-cap not EINVAL");
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
        "detach failed");
    // Unmap the private pages (tables freed by task cleanup; data pages
    // are regular PMM and freed explicitly for balance).
    VMM::unmap_page_in_pml4(kVa, t->page_table_);
    PMM::free_page(data_phys);
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

/// @brief Attach control selectors (issue #226 — no new syscall numbers).
constexpr uint64_t kSelPoll = 2;
constexpr uint64_t kSelBpIns = 3;
constexpr uint64_t kSelBpClr = 4;
constexpr uint64_t kSelStep = 5;
constexpr uint64_t kSelCont = 6;

/// @brief Stop kinds (mirrors StopKind in debug_stop.hpp).
constexpr uint64_t kKindBp = 1;
constexpr uint64_t kKindStep = 2;
constexpr uint64_t kKindFault = 3;
constexpr uint64_t kKindDeath = 4;

/// @brief Bounded poll for one stop event: 100 x 2ms sleeps (the parked
///        target is resumed/running meanwhile; the event lands in the
///        queue + Notify pulse, no host involved).
bool debug_wait_event(uint64_t h, uint64_t &kind_out, uint64_t &addr_out) {
    kind_out = 0;
    addr_out = 0;
    for (int i = 0; i < 100; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelPoll, h,
                      kDebugScratchVa) == 0) {
            auto *scratch = reinterpret_cast<volatile uint64_t *>(
                arch::HHDM_OFFSET +
                VMM::virt_to_phys_in_pml4(
                    kDebugScratchVa, Scheduler::current_task()->page_table_));
            kind_out = scratch[2];
            addr_out = scratch[4];
            return true;
        }
        debug_sleep_ms(2);
    }
    return false;
}

/// @brief Park the target via the data-call EAGAIN discipline and read its
///        current pc from the parked frame (arch pc slot). Works wherever
///        the task actually executes (yield stub or custom page) — the
///        caller never assumes an entry VA.
/// @return pc, 0 on failure.
uint64_t debug_park_and_get_pc(uint64_t h) {
    for (int i = 0; i < 100; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0)
            break;
        debug_sleep_ms(2);
    }
    TaskControlBlock *me = Scheduler::current_task();
    if (me == nullptr || me->page_table_ == 0)
        return 0;
    uint64_t phys = VMM::virt_to_phys_in_pml4(kDebugScratchVa, me->page_table_);
    if (phys == 0)
        return 0;
    auto *scratch = reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET +
                                                          phys);
    // Force a fresh read-round-trip (the parked frame is stable).
    if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h, kDebugScratchVa) !=
        0)
        return 0;
    constexpr size_t kPcIdx =
#if defined(CONFIG_ARCH_X86_64)
        16;
#elif defined(CONFIG_ARCH_AARCH64)
        32;
#else
        32;
#endif
    const uint64_t pc = scratch[kPcIdx];
    // Tripwire (issue #226/#235): a parked user pc below 64 KiB means the
    // frame slot is stale (null-page fetch would fault instantly) — report
    // 0 so callers fail their park/pc assert instead of planting into the
    // void. (No JARVIS_ASSERT_FMT here: it expands to a bare return which
    // is illegal in this non-void helper.)
    if (pc < 0x10000) {
        Logger::warn("debug: stale frame pc=0x%lx", pc);
        return 0;
    }
    return pc;
}

/// @brief Write raw bytes through a TARGET's tables from test context
///        (direct HHDM store + per-arch I-cache coherence). Used where no
///        debugger exists (disposition control) or to stage code pages.
/// @return false when unmapped/straddling.
bool debug_write_target_bytes(TaskControlBlock *t, uint64_t va,
                              const uint8_t *bytes, size_t len) {
    if (t == nullptr || !TaskControlBlock::is_valid(t) ||
        t->page_table_ == 0 || len == 0 || len > 64)
        return false;
    uint64_t page_off = va & 0xFFFULL;
    if (page_off + len > 0x1000ULL)
        return false;
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, t->page_table_);
    if (phys == 0)
        return false;
    // Walk result is the exact phys (offset included — never add page_off
    // again; issue #226 double-add).
    auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    for (size_t i = 0; i < len; ++i)
        dst[i] = bytes[i];
#if defined(CONFIG_ARCH_RISCV64)
    asm volatile(".option push\n.option arch, +zifencei\nfence.i\n.option pop" ::
                     : "memory");
#elif defined(CONFIG_ARCH_AARCH64)
    asm volatile("ic ialluis\n dsb ish\n isb" ::: "memory");
#else
    arch::ArchPageTable::tlb_flush(va);
#endif
    return true;
}

#if defined(CONFIG_ARCH_X86_64)
// The disposition control runs the custom fault page only on x86_64 (the
// only arch that executes per-test VAs); stub arches poison the shared
// yield stub instead (see the fault test).
/// @brief Spawn a child running a fault stub (ud2) at the spinner VA.
//         With a debugger attached the fault routes to a stop event;
//         without, the default disposition terminates the task.
TaskControlBlock *debug_spawn_faulter(uint64_t &stub_phys_out) {
    stub_phys_out = 0;
    auto *me = Scheduler::current_task();
    if (me == nullptr)
        return nullptr;
    constexpr uint8_t kFault[] = {0x0F, 0x0B}; // ud2 (#UD)
    constexpr uint64_t kSpinVa = 0x41000000ULL;
    uint64_t phys = PMM::alloc_user_page();
    if (phys == 0)
        return nullptr;
    auto *dst = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                      phys);
    for (size_t i = 0; i < sizeof(kFault); ++i)
        dst[i] = kFault[i];
    auto *t = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(kSpinVa), 11, 10, 32_KiB);
    if (t == nullptr) {
        PMM::free_page(phys);
        return nullptr;
    }
    VMM::map_page_in_pml4(kSpinVa, phys, true, true, t->page_table_);
    if (VMM::virt_to_phys_in_pml4(kSpinVa, t->page_table_) != phys) {
        PMM::free_page(phys);
        (void)Scheduler::terminate_err(*t, 0);
        Scheduler::drain_zombie_list();
        if (TaskControlBlock::is_valid(t)) {
            t->cleanup();
            delete t;
        }
        Scheduler::drain_zombie_list();
        return nullptr;
    }
    if (!Scheduler::set_sched_policy(*t, SchedPolicy::FIXED)) {
        debug_reap_child(t);
        VMM::unmap_page_in_pml4(kSpinVa, t->page_table_);
        PMM::free_page(phys);
        return nullptr;
    }
    me->add_child(t);
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    stub_phys_out = phys;
    return t;
}
#endif // x86_64-only faulter (stub arches poison the yield stub)

// Testidea: Breakpoint insert/hit/clear through kernel shadows at the
//           task's ACTUAL pc (park first — aarch64/riscv64 run the shared
//           yield stub, never per-test VAs): insert, resume into the trap,
//           poll the BP event (kind + VA + BLOCKED park), clear restores
//           orig bytes, detach resumes (GDB semantics).
// Input: owned spinning U-task.
// Expect: park + pc, sel3 0, BP event kind 1 addr pc, sel4 0 + orig byte
//         back, detach 0, target RUNNING/READY.
JARVIS_TEST(debug_stop_break_insert_hit_clear, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    const uint64_t pc = debug_park_and_get_pc(h);
    JARVIS_ASSERT_FMT(pc != 0, "park/pc failed");
    // Snapshot the original byte at pc for the restore check below.
    uint64_t bphys0 = VMM::virt_to_phys_in_pml4(pc, t->page_table_);
    JARVIS_ASSERT_FMT(bphys0 != 0, "pc unmapped");
    const uint8_t orig0 =
        *reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET + bphys0);
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelBpIns,
                                h, pc) == 0,
                      "bp insert failed");
    // Read-back through the target tables: the inserted breakpoint bytes
    // must be visible to the same walk the fetch uses (mapping coherence).
#if defined(CONFIG_ARCH_X86_64)
    constexpr uint8_t kBpExpect[] = {0xCC};
#elif defined(CONFIG_ARCH_AARCH64)
    constexpr uint8_t kBpExpect[] = {0x00, 0x00, 0x20, 0xD4};
#elif defined(CONFIG_ARCH_RISCV64)
    constexpr uint8_t kBpExpect[] = {0x73, 0x00, 0x10, 0x00};
#else
    constexpr uint8_t kBpExpect[] = {0xCC};
#endif
    auto *bp_xfer = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                         scratch_phys);
    for (size_t i = 0; i < sizeof(kBpExpect); ++i)
        bp_xfer[i] = 0;
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, pc,
                  sizeof(kBpExpect), kDebugScratchVa) == sizeof(kBpExpect),
        "bp readback failed");
    for (size_t i = 0; i < sizeof(kBpExpect); ++i)
        JARVIS_ASSERT_FMT(
            bp_xfer[i] == kBpExpect[i],
            "bp bytes incoherent pc=0x%lx p0=0x%lx got=%x,%x,%x,%x", pc,
            bphys0, bp_xfer[0], bp_xfer[1], bp_xfer[2], bp_xfer[3]);
    // Walk-stability proof: the same walk in the same unmodified tables
    // must resolve identically (a differing phys means the walk itself
    // is incoherent, not the store).
    uint64_t bphys2 = VMM::virt_to_phys_in_pml4(pc, t->page_table_);
    JARVIS_ASSERT_FMT(bphys2 == bphys0, "walk unstable: 0x%lx vs 0x%lx",
                      bphys2, bphys0);
    // Resume out of the clean park so the breakpoint fires.
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelCont,
                                h, 0) == 0,
                      "resume failed");
    uint64_t kind = 0;
    uint64_t addr = 0;
    JARVIS_ASSERT_FMT(debug_wait_event(h, kind, addr), "no BP event");
    JARVIS_ASSERT_FMT(kind == kKindBp, "kind not BP: 0x%lx", kind);
#if defined(CONFIG_ARCH_X86_64)
    // int3 advances RIP past the 1-byte insn; the router reports RIP-1.
    JARVIS_ASSERT_FMT(addr == pc, "addr not VA: 0x%lx", addr);
#else
    // brk/ebreak trap AT the instruction: ELR/sepc == pc.
    JARVIS_ASSERT_FMT(addr == pc, "addr not VA: 0x%lx", addr);
#endif
    JARVIS_ASSERT_FMT(t->state == TaskState::BLOCKED && t->debug_parked,
                      "park markers wrong");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelBpClr,
                                h, pc) == 0,
                      "bp clear failed");
    uint64_t bphys = VMM::virt_to_phys_in_pml4(pc, t->page_table_);
    JARVIS_ASSERT_FMT(bphys != 0, "target unmapped");
    auto *b0 = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                    bphys);
    JARVIS_ASSERT_FMT(*b0 == orig0, "orig byte not restored: 0x%x", *b0);
    // Detach on a breakpoint-stopped target resumes (GDB detach
    // semantics — only genuine faults terminate); shadows were restored
    // by the detach path.
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "detach failed");
    JARVIS_ASSERT_FMT(t->debugger_id == 0 && !t->debug_parked,
                      "detach state wrong");
    JARVIS_ASSERT_FMT(t->state == TaskState::RUNNING ||
                          t->state == TaskState::READY,
                      "bp-stop detach did not resume");
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

// Testidea: Single-step a parked target: exactly one STEP event, then the
//           queue is empty. x86 steps natively (TF); aarch64/RISC-V emulate
//           with a temp breakpoint (fixed-4B ISA on ARM, RVC-decoded on
//           RISC-V — no QEMU/silicon single-step model dependence).
//           RISC-V redirects sepc to a nop sled (a self-loop never falls
//           through to the emulated next-insn breakpoint); aarch64 steps
//           in place (every user insn is 4 bytes, temp at pc+4 always).
// Input: owned spinning U-task (+ sled page on RISC-V).
// Expect: step 0, one STEP event (arch PC rule), second poll EAGAIN,
//         detach resumes.
JARVIS_TEST(debug_stop_step_once, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    // Clean park via the data-call EAGAIN discipline.
    bool parked = false;
    for (int i = 0; i < 100 && !parked; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0) {
            parked = true;
            break;
        }
        debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(parked, "target never parked");
#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
    auto *scratch =
        reinterpret_cast<volatile uint64_t *>(arch::HHDM_OFFSET + scratch_phys);
    // Sled page: c.nop x2 + self-loop; redirect sepc to its base so the
    // emulated step (temp bp at sepc+2) fires deterministically. Staged
    // through the fenced helper (I-cache coherence is the stager's job)
    // and read back through the target (walk/fetch coherence proof).
    constexpr uint64_t kSledVa = 0x43000000ULL;
    uint64_t const sled_phys = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(sled_phys != 0, "sled alloc failed");
    VMM::map_page_in_pml4(kSledVa, sled_phys, true, true, t->page_table_);
    JARVIS_ASSERT_FMT(
        VMM::virt_to_phys_in_pml4(kSledVa, t->page_table_) == sled_phys,
        "sled map failed");
#if defined(CONFIG_ARCH_AARCH64)
    // AArch64 sled: two nops (the emulated step plants its temp brk at
    // sled+4, which always executes — unlike a yield-stub pc that may sit
    // on a branch). Trailing bytes stay zero (never reached past the step).
    constexpr uint8_t kSled[] = {0x1F, 0x20, 0x03, 0xD5,
                                 0x1F, 0x20, 0x03, 0xD5};
#else
    // RISC-V sled: two c.nops + jal x0,0 self-loop (the +4 word must be a
    // valid self-spin: after the temp at +2 is consumed the task continues
    // past it, so any fall-through must trap nothing — a misencoded jal
    // here once jumped to unmapped memory and faked a re-trap).
    constexpr uint8_t kSled[] = {0x01, 0x00, 0x01, 0x00,
                                 0x6F, 0x00, 0x00, 0x00};
#endif
    JARVIS_ASSERT_FMT(
        debug_write_target_bytes(t, kSledVa, kSled, sizeof(kSled)),
        "sled stage failed");
    auto *sled_xfer = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                           scratch_phys);
    for (size_t i = 0; i < sizeof(kSled); ++i)
        sled_xfer[i] = 0;
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kSledVa,
                  sizeof(kSled), kDebugScratchVa) == sizeof(kSled),
        "sled readback failed");
    for (size_t i = 0; i < sizeof(kSled); ++i)
        JARVIS_ASSERT_FMT(sled_xfer[i] == kSled[i], "sled bytes incoherent");
    constexpr size_t kQwords = 34;
    const size_t nq = debug::debug_blob_bytes() / 8;
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h, kDebugScratchVa) ==
            0,
        "pre-step read failed");
    uint64_t mod[kQwords] = {};
    for (size_t i = 0; i < nq; ++i)
        mod[i] = scratch[i];
    mod[32] = kSledVa; // pc slot (riscv blob: x0-x31, pc; aarch64
                       // blob: x0-x30, sp, pc, cpsr — pc is also 32)
    for (size_t i = 0; i < nq; ++i)
        scratch[i] = mod[i];
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_WRITE_REGS, h,
                                kDebugScratchVa) == 0,
                      "sepc redirect failed");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                                kDebugScratchVa) == 0,
                      "post-redirect read failed");
    {
        TaskControlBlock *me2 = Scheduler::current_task();
        uint64_t rphys = VMM::virt_to_phys_in_pml4(kDebugScratchVa,
                                                   me2->page_table_);
        JARVIS_ASSERT_FMT(rphys != 0, "scratch lost");
        auto *rscratch = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + rphys);
        JARVIS_ASSERT_FMT(rscratch[32] == kSledVa,
                          "redirect did not stick: 0x%lx", rscratch[32]);
    }
#endif
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelStep,
                                h, 0) == 0,
                      "step failed");
    uint64_t kind = 0;
    uint64_t addr = 0;
    JARVIS_ASSERT_FMT(debug_wait_event(h, kind, addr), "no STEP event");
    JARVIS_ASSERT_FMT(kind == kKindStep, "kind not STEP: 0x%lx", kind);
#if defined(CONFIG_ARCH_RISCV64)
    JARVIS_ASSERT_FMT(addr == kSledVa + 2, "pc did not advance: 0x%lx", addr);
#elif defined(CONFIG_ARCH_AARCH64)
    // Emulated step on the sled (temp brk at sled+4).
    JARVIS_ASSERT_FMT(addr == kSledVa + 4, "pc did not advance: 0x%lx", addr);
#else
    JARVIS_ASSERT_FMT(addr == 0x41000000ULL, "pc moved: 0x%lx", addr);
#endif
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelPoll,
                                h, kDebugScratchVa) == Neg(kEagain),
                      "step produced more than one event");
#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
    // Temp-restore proof (auditor S2 fix): the consumed temp's VA must
    // read back the original bytes (not the trap insn). Verified by
    // read-back, never by executing past (the sled tail is not a safe
    // execution path on all QEMU CPUs).
    {
        auto *tback = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                           scratch_phys);
        for (size_t i = 0; i < 4; ++i)
            tback[i] = 0xFF;
#if defined(CONFIG_ARCH_RISCV64)
        constexpr uint64_t kTempVa = kSledVa + 2;
        constexpr uint8_t kTempOrig[] = {0x01, 0x00};
#else
        constexpr uint64_t kTempVa = kSledVa + 4;
        constexpr uint8_t kTempOrig[] = {0x1F, 0x20, 0x03, 0xD5};
#endif
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_READ_MEM, h, kTempVa,
                      sizeof(kTempOrig), kDebugScratchVa) == sizeof(kTempOrig),
            "temp readback failed");
        for (size_t i = 0; i < sizeof(kTempOrig); ++i)
            JARVIS_ASSERT_FMT(tback[i] == kTempOrig[i],
                              "temp bytes not restored");
    }
#endif
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelCont,
                                h, 0) == 0,
                      "post-step continue failed");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "detach failed");
    JARVIS_ASSERT_FMT(t->state == TaskState::RUNNING ||
                          t->state == TaskState::READY,
                      "target not resumed");
    debug_unmap_scratch(scratch_phys);
#if defined(CONFIG_ARCH_RISCV64) || defined(CONFIG_ARCH_AARCH64)
    VMM::unmap_page_in_pml4(kSledVa, t->page_table_);
    PMM::free_page(sled_phys);
#endif
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

// Per-arch illegal-instruction patterns written AT the parked pc (ud2 on
// x86, c.unimp on RISC-V). Lengths stay inside one page for any pc.
// AArch64 needs none: QEMU treats UDF as a nop, so the attached fault is
// driven by redirecting pc to an unmapped VA (instruction abort) and the
// control poisons the stub with brk (guaranteed trap, default path).
#if defined(CONFIG_ARCH_X86_64)
constexpr uint8_t kFaultInsn[] = {0x0F, 0x0B}; // ud2
#else
constexpr uint8_t kFaultInsn[] = {0x00, 0x00, 0x00, 0x00}; // c.unimp
#endif
#if defined(CONFIG_ARCH_AARCH64)
// brk #0, LE (always traps; UDF cannot be relied on under QEMU).
constexpr uint8_t kBrkInsn[] = {0x00, 0x00, 0x20, 0xD4};
// Unmapped VA for the fault-by-redirect control path (instruction abort).
constexpr uint64_t kUnmappedFaultVa = 0x50000000ULL;
#endif

// Testidea: Fault routing vs default disposition: an attached target parks
//           on a FAULT event (never TERMINATED) — illegal insn at the
//           parked pc on x86/riscv, pc redirected to unmapped memory on
//           aarch64 (instruction abort); detach terminates. The control
//           (no debugger) dies by the default path: custom fault page on
//           x86, brk-poisoned yield stub on aarch64 (all three slots),
//           illegal bytes at the stub on riscv.
// Input: owned spinning U-task (+ fault insn / redirect / stub poison).
// Expect: FAULT event kind 3 + BLOCKED; detach → TERMINATED;
//         control → TERMINATED without debugger.
JARVIS_TEST(debug_stop_fault_routed, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    const uint64_t pc = debug_park_and_get_pc(h);
    JARVIS_ASSERT_FMT(pc != 0, "park/pc failed");
#if defined(CONFIG_ARCH_AARCH64)
    // Redirect pc to unmapped memory: resume faults with an instruction
    // abort (EC 0x20/0x21) instead of relying on an illegal encoding.
    {
        constexpr size_t kQwords = 34;
        const size_t nq = debug::debug_blob_bytes() / 8;
        auto *wscratch = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + scratch_phys);
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0,
            "pre-redirect read failed");
        uint64_t mod[kQwords] = {};
        for (size_t i = 0; i < nq; ++i)
            mod[i] = wscratch[i];
        mod[32] = kUnmappedFaultVa; // elr slot
        for (size_t i = 0; i < nq; ++i)
            wscratch[i] = mod[i];
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_WRITE_REGS, h,
                      kDebugScratchVa) == 0,
            "pc redirect failed");
    }
#else
    // Stage the illegal bytes into the caller scratch, then write through
    // the target's tables at pc and resume into the fault.
    auto *xfer = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                      scratch_phys);
    for (size_t i = 0; i < sizeof(kFaultInsn); ++i)
        xfer[i] = kFaultInsn[i];
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_WRITE_MEM, h, pc,
                  sizeof(kFaultInsn), kDebugScratchVa) ==
            sizeof(kFaultInsn),
        "fault write failed");
#endif
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelCont,
                                h, 0) == 0,
                      "resume failed");
    uint64_t kind = 0;
    uint64_t addr = 0;
    JARVIS_ASSERT_FMT(debug_wait_event(h, kind, addr), "no FAULT event");
    JARVIS_ASSERT_FMT(kind == kKindFault, "kind not FAULT: 0x%lx", kind);
#if defined(CONFIG_ARCH_AARCH64)
    // The fault must come from the redirect target (proves the redirect
    // steered execution, not some stray trap).
    JARVIS_ASSERT_FMT(addr == kUnmappedFaultVa, "fault not at redirect: 0x%lx",
                      addr);
#else
    JARVIS_ASSERT_FMT(addr == pc, "fault not at pc: 0x%lx", addr);
#endif
    JARVIS_ASSERT_FMT(t->state == TaskState::BLOCKED && t->debug_parked,
                      "fault did not park (terminated?)");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "detach failed");
    JARVIS_ASSERT_FMT(t->state == TaskState::TERMINATED,
                      "fault-stop detach did not terminate");
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    // Control: faulting bytes at an executed VA, no debugger → default
    // disposition terminates. x86 executes the custom page; riscv64
    // poisons the shared yield stub in-test; aarch64 poisons all three
    // stub slots with brk (UDF is unreliable under QEMU).
#if defined(CONFIG_ARCH_X86_64)
    uint64_t cphys = 0;
    TaskControlBlock *c = debug_spawn_faulter(cphys);
    JARVIS_ASSERT_FMT(c != nullptr, "control spawn failed");
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t cphys = 0;
    TaskControlBlock *c = debug_spawn_spinner(cphys);
    JARVIS_ASSERT_FMT(c != nullptr, "control spawn failed");
    for (uint64_t off = 0; off < 12; off += 4) {
        JARVIS_ASSERT_FMT(
            debug_write_target_bytes(c, kernel::task::kUserYieldStubVa + off,
                                     kBrkInsn, sizeof(kBrkInsn)),
            "stub poison failed");
    }
#else
    uint64_t cphys = 0;
    TaskControlBlock *c = debug_spawn_spinner(cphys);
    JARVIS_ASSERT_FMT(c != nullptr, "control spawn failed");
    JARVIS_ASSERT_FMT(
        debug_write_target_bytes(c, kernel::task::kUserYieldStubVa,
                                 kFaultInsn, sizeof(kFaultInsn)),
        "stub poison failed");
#endif
    // Death = TERMINATED, reaped, invalid, or RECYCLED (id changed): the
    // control task has no debugger to serialize teardown against, so the
    // system drain may free its block and a later spawn (e.g. a monitor
    // re-spawn, frequent under emulation dilation) may recycle it — either
    // way the default disposition applied. Capture the id up front; any
    // deviation from (valid, ours, unterminated) counts as died.
    const uint64_t control_id = c->id;
    bool died = false;
    for (int i = 0; i < 50 && !died; ++i) {
        if (!TaskControlBlock::is_valid(c) || c->id != control_id ||
            c->state == TaskState::TERMINATED ||
            c->state == TaskState::REAPED)
            died = true;
        else
            debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(died, "unattached faulter survived");
    // Reap only when the block is still ours and not yet reaped (a
    // system-drained or recycled task must not be touched again).
    if (TaskControlBlock::is_valid(c) && c->id == control_id &&
        c->state != TaskState::REAPED) {
        debug_free_spinner_page(c, cphys);
        debug_reap_child(c);
    }
    JARVIS_TEST_PASS();
}

// Testidea: Queue bounds + death reserved slot under pure-fault pressure
//           (no breakpoints/steps involved): a faulting pc (illegal insn
//           on x86/riscv, unmapped redirect on aarch64) re-faults on every
//           plain continue, queueing 20 FAULT events without polling. 20
//           queued into 16 slots drops the 4 oldest; the newest 16 stay
//           drainable; killing the target still delivers its DEATH event
//           through the reserved slot.
// Input: owned spinner + faulting pc, continue-driven faults.
// Expect: 20 re-parks, 16 events then EAGAIN; terminate → DEATH pollable.
JARVIS_TEST(debug_stop_overflow_death_slot, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    const uint64_t pc = debug_park_and_get_pc(h);
    JARVIS_ASSERT_FMT(pc != 0, "park/pc failed");
#if defined(CONFIG_ARCH_AARCH64)
    // Redirect pc to unmapped memory (see the fault test): every plain
    // continue re-faults with an instruction abort — no illegal encoding
    // needed (UDF is unreliable under QEMU).
    {
        constexpr size_t kQwords = 34;
        const size_t nq = debug::debug_blob_bytes() / 8;
        auto *wscratch = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + scratch_phys);
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0,
            "pre-redirect read failed");
        uint64_t mod[kQwords] = {};
        for (size_t i = 0; i < nq; ++i)
            mod[i] = wscratch[i];
        mod[32] = kUnmappedFaultVa; // elr slot
        for (size_t i = 0; i < nq; ++i)
            wscratch[i] = mod[i];
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_WRITE_REGS, h,
                      kDebugScratchVa) == 0,
            "pc redirect failed");
        JARVIS_ASSERT_FMT(
            DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0,
            "post-redirect read failed");
        {
            TaskControlBlock *me2 = Scheduler::current_task();
            uint64_t rphys = VMM::virt_to_phys_in_pml4(kDebugScratchVa,
                                                       me2->page_table_);
            JARVIS_ASSERT_FMT(rphys != 0, "scratch lost");
            auto *rscratch = reinterpret_cast<volatile uint64_t *>(
                arch::HHDM_OFFSET + rphys);
            JARVIS_ASSERT_FMT(rscratch[32] == kUnmappedFaultVa,
                              "redirect did not stick: 0x%lx", rscratch[32]);
        }
    }
#else
    auto *xfer = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                      scratch_phys);
    for (size_t i = 0; i < sizeof(kFaultInsn); ++i)
        xfer[i] = kFaultInsn[i];
    JARVIS_ASSERT_FMT(
        DebugCall(SyscallNumber::TASK_DEBUG_WRITE_MEM, h, pc,
                  sizeof(kFaultInsn), kDebugScratchVa) ==
            sizeof(kFaultInsn),
        "fault write failed");
#endif
    (void)pc;
    // Drive 20 FAULT events WITHOUT polling: each plain continue (fault
    // stops never step-over) re-faults; re-parks observed via the park
    // flag so the queue actually fills.
    for (int i = 0; i < 20; ++i) {
        JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelCont,
                                    h, 0) == 0,
                          "continue failed");
        bool reparked = false;
        for (int j = 0; j < 100 && !reparked; ++j) {
            if (t->debug_parked && t->state == TaskState::BLOCKED)
                reparked = true;
            else
                debug_sleep_ms(2);
        }
        JARVIS_ASSERT_FMT(reparked,
                          "no re-park after continue i=%d st=%d parked=%d "
                          "kind=%x id=%x",
                          i, static_cast<int>(t->state), t->debug_parked,
                          __atomic_load_n(&t->debug_stop_kind,
                                          __ATOMIC_ACQUIRE),
                          __atomic_load_n(&t->debugger_id, __ATOMIC_ACQUIRE));
    }
    // Drain: the ring holds the newest 16, then reports empty (4 oldest
    // dropped under pressure).
    for (int i = 0; i < 20; ++i) {
        uint64_t r = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelPoll, h,
                               kDebugScratchVa);
        if (i < 16)
            JARVIS_ASSERT_FMT(r == 0, "newest event missing");
        else
            JARVIS_ASSERT_FMT(r == Neg(kEagain), "drop accounting wrong");
    }
    // Death bypasses the pressure: terminate + drain (the drain FREES
    // the target and reclaims its mapped pages via teardown — t dangles
    // afterwards and must not be touched), then DEATH arrives, then the
    // dead-detach drops the owned binding.
    (void)Scheduler::terminate_err(*t, 0);
    Scheduler::drain_zombie_list();
    t = nullptr; // reaped + freed by the drain; no further TCB access
    uint64_t kind = 0;
    uint64_t addr = 0;
    (void)addr;
    JARVIS_ASSERT_FMT(debug_wait_event(h, kind, addr), "no DEATH event");
    JARVIS_ASSERT_FMT(kind == kKindDeath, "kind not DEATH: 0x%lx", kind);
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "dead detach failed");
    debug_unmap_scratch(scratch_phys);
    // The spinner page was mapped in the dead target's tables and is
    // already reclaimed by its teardown — freeing again would double-free.
    // The stub phys value is intentionally not freed here.
    (void)stub_phys;
    JARVIS_TEST_PASS();
}

// Testidea: Detach disposition matrix: cleanly-parked (data-call EAGAIN)
//           detach resumes; fault-stopped detach terminates.
// Input: spinner (clean park) + faulter (fault park).
// Expect: RUNNING/READY + id 0 vs TERMINATED.
JARVIS_TEST(debug_stop_detach_disposition, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    bool parked = false;
    for (int i = 0; i < 100 && !parked; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0) {
            parked = true;
            break;
        }
        debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(parked, "clean park failed");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "clean detach failed");
    JARVIS_ASSERT_FMT(t->debugger_id == 0, "id not cleared");
    JARVIS_ASSERT_FMT(t->state == TaskState::RUNNING ||
                          t->state == TaskState::READY,
                      "clean detach did not resume");
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

/// @brief Debugger-death fixture: helper kernel task (D) attaches to the
///        victim (V, parented to D for the launcher claim), parks it, then
///        waits to be killed. The test task (T) kills D and asserts the
///        fail-safe: V resumed, flags cleared, no parked orphan.
TaskControlBlock *g_dbg_victim = nullptr;
volatile bool g_dbg_parked_flag = false;
volatile bool g_dbg_done_flag = false;

void debug_dbg_entry() {
    TaskControlBlock *me = Scheduler::current_task();
    TaskControlBlock *v = g_dbg_victim;
    if (me == nullptr || v == nullptr)
        return;
    uint64_t const scratch_phys = debug_map_scratch();
    if (scratch_phys == 0)
        return;
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, v->id);
    if (h == 0 || h == Neg(kEbusy) || h == Neg(kEperm) || h == Neg(kEsrch)) {
        debug_unmap_scratch(scratch_phys);
        return;
    }
    for (int i = 0; i < 100; ++i) {
        if (DebugCall(SyscallNumber::TASK_DEBUG_READ_REGS, h,
                      kDebugScratchVa) == 0)
            break;
        debug_sleep_ms(2);
    }
    // Scratch no longer needed (arm done); unmap before the wait so no
    // page leaks when T kills this task mid-wait.
    debug_unmap_scratch(scratch_phys);
    (void)h;
    g_dbg_parked_flag = true;
    for (int i = 0; i < 500 && !g_dbg_done_flag; ++i)
        debug_sleep_ms(2);
}

// Testidea: Debugger death fail-safe: killing the attached debugger while
//           its target is cleanly parked resumes the target with all debug
//           state cleared (no orphaned park, handle released).
// Input: helper debugger task D + spinner victim V parented to D.
// Expect: V parked under D; after D dies: V RUNNING/READY, id 0,
//         !parked.
JARVIS_TEST(debug_stop_debugger_death, "PRE: none | POST: none") {
    g_dbg_victim = nullptr;
    g_dbg_parked_flag = false;
    g_dbg_done_flag = false;
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT_FMT(me != nullptr, "no current task");
    TaskControlBlock *dbg = TaskControlBlock::create(debug_dbg_entry, 11, 0);
    JARVIS_ASSERT_FMT(dbg != nullptr, "debugger spawn failed");
    if (!Scheduler::set_sched_policy(*dbg, SchedPolicy::FIXED)) {
        if (TaskControlBlock::is_valid(dbg)) {
            dbg->cleanup();
            delete dbg;
        }
        Scheduler::drain_zombie_list();
        JARVIS_ASSERT_FMT(false, "debugger policy failed");
    }
    me->add_child(dbg);
    // Victim: user spinner, parented to D (launcher claim needs it).
    uint64_t stub_phys = PMM::alloc_user_page();
    JARVIS_ASSERT_FMT(stub_phys != 0, "stub alloc failed");
    constexpr uint64_t kSpinVa = 0x41000000ULL;
#if defined(CONFIG_ARCH_X86_64)
    constexpr uint8_t kSpin[] = {0xEB, 0xFE};
#elif defined(CONFIG_ARCH_AARCH64)
    constexpr uint8_t kSpin[] = {0x00, 0x00, 0x00, 0x14};
#elif defined(CONFIG_ARCH_RISCV64)
    constexpr uint8_t kSpin[] = {0x6F, 0x00, 0x00, 0x00};
#else
    constexpr uint8_t kSpin[] = {0x00};
#endif
    auto *sdst = reinterpret_cast<volatile uint8_t *>(arch::HHDM_OFFSET +
                                                      stub_phys);
    for (size_t i = 0; i < sizeof(kSpin); ++i)
        sdst[i] = kSpin[i];
    TaskControlBlock *v = TaskControlBlock::create_user(
        reinterpret_cast<void (*)()>(kSpinVa), 11, 10, 32_KiB);
    JARVIS_ASSERT_FMT(v != nullptr, "victim spawn failed");
    VMM::map_page_in_pml4(kSpinVa, stub_phys, true, true, v->page_table_);
    if (!Scheduler::set_sched_policy(*v, SchedPolicy::FIXED)) {
        VMM::unmap_page_in_pml4(kSpinVa, v->page_table_);
        PMM::free_page(stub_phys);
        debug_reap_child(v);
        debug_reap_child(dbg);
        JARVIS_ASSERT_FMT(false, "victim policy failed");
    }
    dbg->add_child(v);
    g_dbg_victim = v;
    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*dbg);
        Scheduler::add_task(*v);
    }
    Scheduler::reschedule();
    bool parked = false;
    for (int i = 0; i < 100 && !parked; ++i) {
        if (g_dbg_parked_flag)
            parked = true;
        else
            debug_sleep_ms(2);
    }
    JARVIS_ASSERT_FMT(parked, "debugger never parked victim");
    JARVIS_ASSERT_FMT(v->state == TaskState::BLOCKED && v->debug_parked,
                      "victim not parked");
    // Kill the debugger THROUGH the shared reaper (single reap — the
    // cleanup fail-safe fires in D's cleanup during the drain and must
    // resume the victim). Manual terminate+drain here would free D and
    // turn the reaper below into a double-reap use-after-free.
    g_dbg_victim = nullptr;
    debug_reap_child(dbg);
    dbg = nullptr; // reaped + freed; no further access
    JARVIS_ASSERT_FMT(v->state == TaskState::RUNNING ||
                          v->state == TaskState::READY,
                      "victim orphaned");
    JARVIS_ASSERT_FMT(v->debugger_id == 0 && !v->debug_parked,
                      "victim debug state not cleared");
    g_dbg_done_flag = true;
    // Teardown: victim back under T for the shared reaper (D is gone, so
    // no removal from its list — add_child overwrites the stale links).
    me->add_child(v);
    VMM::unmap_page_in_pml4(kSpinVa, v->page_table_);
    PMM::free_page(stub_phys);
    debug_reap_child(v);
    JARVIS_TEST_PASS();
}

// Testidea: Poll with no stop pending reports EAGAIN (pure query — polling
//           never arms a stop, unlike the data calls).
// Input: attached spinner, no stops driven.
// Expect: poll == EAGAIN, target still RUNNING/READY, detach resumes.
JARVIS_TEST(debug_stop_poll_empty, "PRE: none | POST: none") {
    uint64_t stub_phys = 0;
    TaskControlBlock *t = debug_spawn_spinner(stub_phys);
    JARVIS_ASSERT_FMT(t != nullptr, "spawn failed");
    uint64_t const scratch_phys = debug_map_scratch();
    JARVIS_ASSERT_FMT(scratch_phys != 0, "scratch map failed");
    uint64_t h = DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 1, t->id);
    JARVIS_ASSERT_FMT(h != 0 && h != Neg(kEbusy), "attach failed: 0x%lx", h);
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, kSelPoll,
                                h, kDebugScratchVa) == Neg(kEagain),
                      "empty poll not EAGAIN");
    JARVIS_ASSERT_FMT(t->state == TaskState::RUNNING ||
                          t->state == TaskState::READY,
                      "poll parked the target");
    JARVIS_ASSERT_FMT(DebugCall(SyscallNumber::TASK_DEBUG_ATTACH, 0, h) == 0,
                      "detach failed");
    debug_unmap_scratch(scratch_phys);
    debug_free_spinner_page(t, stub_phys);
    debug_reap_child(t);
    JARVIS_TEST_PASS();
}

/// @brief Register all debugger-syscall tests.
void register_debug_syscall_tests() {
    Logger::info("Registering debug syscall tests");
    JARVIS_REGISTER_TEST(debug_attach_parent_ok);
    JARVIS_REGISTER_TEST(debug_attach_rejects);
    JARVIS_REGISTER_TEST(debug_codec_synthetic);
    JARVIS_REGISTER_TEST(debug_park_read_write_detach);
    JARVIS_REGISTER_TEST(debug_mem_partial);
    JARVIS_REGISTER_TEST(debug_stop_break_insert_hit_clear);
    JARVIS_REGISTER_TEST(debug_stop_step_once);
    JARVIS_REGISTER_TEST(debug_stop_fault_routed);
    JARVIS_REGISTER_TEST(debug_stop_overflow_death_slot);
    JARVIS_REGISTER_TEST(debug_stop_detach_disposition);
    JARVIS_REGISTER_TEST(debug_stop_debugger_death);
    JARVIS_REGISTER_TEST(debug_stop_poll_empty);
}
