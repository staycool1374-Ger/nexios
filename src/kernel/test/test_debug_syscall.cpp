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

/// @brief Register all debugger-syscall tests.
void register_debug_syscall_tests() {
    Logger::info("Registering debug syscall tests");
    JARVIS_REGISTER_TEST(debug_attach_parent_ok);
    JARVIS_REGISTER_TEST(debug_attach_rejects);
    JARVIS_REGISTER_TEST(debug_codec_synthetic);
    JARVIS_REGISTER_TEST(debug_park_read_write_detach);
    JARVIS_REGISTER_TEST(debug_mem_partial);
}
