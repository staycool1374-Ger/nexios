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

/// @file test_tls.cpp
/// @brief TLS_SET(85) + TLS-on-context-switch tests (issue #74).  DRIVEN:
///        every syscall is invoked by a REAL dispatched kernel task so
///        syscall_task() resolves genuinely (test_syscall.cpp pattern).
///        Publish/apply mechanics use synthetic-user tasks (is_user_ set on
///        a kernel-context TCB): a C++ lambda cannot run as a true userspace
///        task (test_ipc_blocking.cpp documents the constraint), and the
///        validation/publish paths key off the flag + page-table fields the
///        test controls.  Full ring-3 acceptance belongs to #75 (crt0 calls
///        TLS_SET through the real gate).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/nexios_config.h>
#include <constants.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

// Create a REAL kernel task, dispatch it, and wait for genuine
// termination (test_syscall.cpp run_syscall_task pattern, local copy).
TaskControlBlock *run_tls_task(void (*entry)()) {
    auto *t = TaskControlBlock::create(entry, 11, 10);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

void release_tls_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
    Scheduler::drain_zombie_list();
}

// Live TLS register readback, per arch (x86_64 FS_BASE, aarch64
// TPIDR_EL0, riscv64 tp).  Test-context reads are unprivileged-safe:
// RDMSR/MRS/MV execute fine in ring 0; user tasks use the same.
uint64_t read_live_tls() {
#if defined(CONFIG_ARCH_X86_64)
    return arch::rdmsr(arch::MSR_FS_BASE);
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t v = 0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
#elif defined(CONFIG_ARCH_RISCV64)
    uint64_t v = 0;
    asm volatile("mv %0, tp" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

// Live TLS register clear (test hygiene: leave no stale base behind).
void write_live_tls(uint64_t v) {
#if defined(CONFIG_ARCH_X86_64)
    arch::wrmsr(arch::MSR_FS_BASE, v);
#elif defined(CONFIG_ARCH_AARCH64)
    asm volatile("msr tpidr_el0, %0" : : "r"(v) : "memory");
#elif defined(CONFIG_ARCH_RISCV64)
    asm volatile("mv tp, %0" : : "r"(v) : "memory");
#else
    (void)v;
#endif
}

uint64_t tls_call(uint64_t base) {
    return Syscall::handle(static_cast<uint64_t>(SyscallNumber::TLS_SET),
                           base, 0, 0, 0, nullptr);
}

constexpr uint64_t tls_invalid_code() {
    return static_cast<uint64_t>(0) -
           static_cast<uint64_t>(errors::SYS_ERR_TLS_INVALID_BASE);
}

#if defined(CONFIG_ARCH_X86_64)
constexpr uint64_t k_tls_base = 0x00007FFF00000000ULL;
constexpr uint64_t k_tls_noncanonical = 0x0000800000000000ULL;
#elif defined(CONFIG_ARCH_AARCH64)
constexpr uint64_t k_tls_base = 0x0000FFF000000000ULL;
constexpr uint64_t k_tls_noncanonical = 0xFFFF000000000000ULL;
#elif defined(CONFIG_ARCH_RISCV64)
constexpr uint64_t k_tls_base = 0x0000007F00000000ULL;
constexpr uint64_t k_tls_noncanonical = 0x0000008000000000ULL;
#else
constexpr uint64_t k_tls_base = 0x00007FFF00000000ULL;
constexpr uint64_t k_tls_noncanonical = 0x0000800000000000ULL;
#endif

void tls_yield() {
    Syscall::handle(static_cast<uint64_t>(SyscallNumber::YIELD), 0, 0,
                    0, 0, nullptr);
}

// One ping-pong participant body (file-static: task entries cannot
// capture). Warmup absorbs the stale-register first runs; then N
// asserted rounds. Any schedule interleaving is sound: each task only
// ever asserts its OWN base.
void tls_ping_body(uint64_t own, uint64_t &mismatch) {
    for (uint64_t i = 0; i < 3; ++i)
        tls_yield();
    for (uint64_t i = 0; i < 4; ++i) {
        if (read_live_tls() != own)
            ++mismatch;
        tls_yield();
    }
}

} // namespace

// Runmode: kernel
// Testidea: TLS_SET(0) clears a previously set base, field and live.
// Input: Dispatched task sets a valid base, then TLS_SET(0).
// Expect: Both return 0; tls_base_==0; live register==0.
// Depends: Syscall TLS_SET (issue #74)
JARVIS_TEST(tls_set_zero_clears, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_clr_ret = 0;
    static uint64_t g_field = 0;
    static uint64_t g_live = 0;

    auto *t = run_tls_task([]() {
        g_set_ret = tls_call(k_tls_base);
        g_clr_ret = tls_call(0);
        auto *c = Scheduler::current_task();
        g_field = (c != nullptr) ? c->tls_base_ : 1ULL;
        g_live = read_live_tls();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_set_ret);
    JARVIS_ASSERT_EQ(0ULL, g_clr_ret);
    JARVIS_ASSERT_EQ(0ULL, g_field);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Non-canonical addresses fail closed with the dedicated code.
// Input: x86_64 0x0000800000000000 (48-bit non-canonical); aarch64 upper
//        nonzero; riscv64 bit39 set (passes LIMIT, fails Sv39 canonical).
// Expect: -SYS_ERR_TLS_INVALID_BASE; stored base unchanged (stays 0).
// Depends: Syscall TLS_SET validation (issue #74)
JARVIS_TEST(tls_set_rejects_noncanonical, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_field = 0;

    auto *t = run_tls_task([]() {
        g_ret = tls_call(k_tls_noncanonical);
        auto *c = Scheduler::current_task();
        g_field = (c != nullptr) ? c->tls_base_ : 1ULL;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(tls_invalid_code(), g_ret);
    JARVIS_ASSERT_EQ(0ULL, g_field);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel-half addresses fail closed on every arch.
// Input: TLS_SET(0xFFFF800000000000) (canonical but kernel-half).
// Expect: -SYS_ERR_TLS_INVALID_BASE; stored base unchanged.
// Depends: Syscall TLS_SET user-half gate (issue #74, S1)
JARVIS_TEST(tls_set_rejects_kernel_half, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_field = 0;

    auto *t = run_tls_task([]() {
        g_ret = tls_call(0xFFFF800000000000ULL);
        auto *c = Scheduler::current_task();
        g_field = (c != nullptr) ? c->tls_base_ : 1ULL;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(tls_invalid_code(), g_ret);
    JARVIS_ASSERT_EQ(0ULL, g_field);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A valid user-half base stores AND live-applies (setter runs
//           on the calling task: §13 crt0 TLS_SET-then-use needs this).
// Input: TLS_SET(k_tls_base) from the running task.
// Expect: Returns 0; tls_base_ reads back; live register equals base.
// Depends: Scheduler::set_tls_base_err live-apply (issue #74)
JARVIS_TEST(tls_set_accepts_user_half, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_field = 0;
    static uint64_t g_live = 0;

    auto *t = run_tls_task([]() {
        g_ret = tls_call(k_tls_base);
        auto *c = Scheduler::current_task();
        g_field = (c != nullptr) ? c->tls_base_ : 0ULL;
        g_live = read_live_tls();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(k_tls_base, g_field);
    JARVIS_ASSERT_EQ(k_tls_base, g_live);
    write_live_tls(0);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: clone() inherits the parent's TLS base (userspace re-sets).
// Input: Dispatched parent sets base, clones, reads child field.
// Expect: Child tls_base_ == parent base.
// Depends: TaskControlBlock::clone inherit (issue #74)
JARVIS_TEST(tls_clone_inherits, "PRE: none | POST: none") {
    static uint64_t g_set_ret = 0;
    static uint64_t g_child_base = 0;

    auto *t = run_tls_task([]() {
        g_set_ret = tls_call(k_tls_base);
        // clone() builds the child iret frame from regs[] — pass a
        // synthetic user frame (test_process.cpp pattern), never null.
        uint64_t regs[22] = {};
        regs[17] = 0x1000;
        regs[18] = arch::SEG_USER_CODE;
        regs[19] = arch::RFLAGS_DEFAULT;
        regs[20] = 0x80000000;
        regs[21] = arch::SEG_USER_DATA;
        auto *c = TaskControlBlock::clone(regs);
        if (c != nullptr) {
            g_child_base = c->tls_base_;
            kernel::test::terminate_and_drain(*c);
        }
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_set_ret);
    JARVIS_ASSERT_EQ(k_tls_base, g_child_base);
    write_live_tls(0);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: TaskFields capture/restore round-trips tls_base_ (snapshot
//           parity: the field rides the existing POD machinery).
// Input: Capture all fields, mutate current base, restore, read back.
// Expect: Restored base equals the captured (pre-mutation) value.
// Depends: Scheduler::capture/restore_task_fields (issue #74)
JARVIS_TEST(tls_snapshot_parity, "PRE: none | POST: none") {
    static Scheduler::TaskFields s_fields[CONFIG_MAX_TASKS];
    static uint64_t g_before = 0;
    static uint64_t g_after = 0;

    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    g_before = cur->tls_base_;
    Scheduler::capture_task_fields(s_fields);
    cur->tls_base_ = k_tls_base;
    Scheduler::restore_task_fields(s_fields);
    g_after = cur->tls_base_;
    JARVIS_ASSERT_EQ(g_before, g_after);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: TLS_SET stays out of the FAST mask and dispatches FULL.
// Input: Assert bit 85 clear in SYSCALL_FAST_MASK; call via handle().
// Expect: Mask bit clear; handle(TLS_SET, valid) returns 0 (FULL path).
// Depends: Tiered dispatch INV (fastpath §3-4, issue #92)
JARVIS_TEST(tls_not_fast, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static bool g_mask_clear = false;

    g_mask_clear =
        ((Syscall::SYSCALL_FAST_MASK >> 85) & 1U) == 0U;
    auto *t = run_tls_task([]() {
        g_ret = tls_call(k_tls_base);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_mask_clear);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    write_live_tls(0);
    release_tls_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Site-1 publish observed through REAL dispatch (reschedule()
//           only sets need_resched; the tick arms via switch_to_task).
//           Part A: a synthetic-user task observes its live base after
//           genuine dispatch. Part B: a kernel task carrying a nonzero
//           field must publish 0 — proven by the live register surviving
//           a kernel dispatch unchanged (a leaked publish would load it).
// Input: Ku (synthetic-user, base B) records live FS on dispatch; then
//        harness sets live X and dispatches kernel Kk (field B).
// Expect: Ku observed B; live still X after Kk; x86_64 GS_BASE unchanged.
// Depends: switch_to_task publish via tick path (issue #74)
JARVIS_TEST(tls_publish_applies, "PRE: none | POST: none") {
    static uint64_t g_user_live = 0;
    static uint64_t g_live_after = 0;
    static uint64_t g_gs_same = 0;

    constexpr uint64_t k_base_b = 0x00007A7400000000ULL;
    constexpr uint64_t k_base_x = 0x00007A7500000000ULL;
#if defined(CONFIG_ARCH_X86_64)
    uint64_t gs_before = arch::rdmsr(arch::MSR_GS_BASE);
#endif
    auto *ku = TaskControlBlock::create(
        []() {
            g_user_live = read_live_tls();
        },
        12, 10);
    JARVIS_ASSERT(ku != nullptr);
    ku->is_user_ = true;
    ku->tls_base_ = k_base_b;
    Scheduler::add_task(*ku);
    kernel::test::wait_for_termination_safe(ku);
    JARVIS_ASSERT_EQ(k_base_b, g_user_live);
    ku->is_user_ = false;
    release_tls_task(ku);

    // Harness sets live X, then dispatches a kernel task rigged with a
    // nonzero field: a correct kernel publish (0) loads nothing.
    JARVIS_ASSERT_EQ(0ULL, tls_call(k_base_x));
    auto *kk = TaskControlBlock::create([]() {}, 12, 10);
    JARVIS_ASSERT(kk != nullptr);
    kk->tls_base_ = k_base_b;
    Scheduler::add_task(*kk);
    kernel::test::wait_for_termination_safe(kk);
    g_live_after = read_live_tls();
    JARVIS_ASSERT_EQ(k_base_x, g_live_after);
#if defined(CONFIG_ARCH_X86_64)
    uint64_t gs_after = arch::rdmsr(arch::MSR_GS_BASE);
    g_gs_same = (gs_before == gs_after) ? 1ULL : 0ULL;
#else
    g_gs_same = 1ULL;
#endif
    JARVIS_ASSERT_EQ(1ULL, g_gs_same);
    write_live_tls(0);
    release_tls_task(kk);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: ACCEPTANCE — preemption ping-pong between two TLS-distinct
//           synthetic-user tasks: each always observes its own live base.
// Input: Tasks A/B (distinct bases) loop {assert live==own; YIELD} x4.
// Expect: Zero cross-observations across all preemptions.
// Depends: Publish + epilogue apply on every dispatch (issue #74)
JARVIS_TEST(tls_preemption_ping_pong, "PRE: none | POST: none") {
    static uint64_t g_mismatch = 0;
    static constexpr uint64_t k_base_a = 0x00007AA000000000ULL;
    static constexpr uint64_t k_base_b = 0x00007BB000000000ULL;

    auto *a = TaskControlBlock::create(
        []() {
            tls_ping_body(k_base_a, g_mismatch);
        },
        12, 10);
    auto *b = TaskControlBlock::create(
        []() {
            tls_ping_body(k_base_b, g_mismatch);
        },
        11, 10);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(b != nullptr);
    a->is_user_ = true;
    a->tls_base_ = k_base_a;
    b->is_user_ = true;
    b->tls_base_ = k_base_b;
    Scheduler::add_task(*a);
    Scheduler::add_task(*b);
    kernel::test::wait_for_termination_safe(a);
    kernel::test::wait_for_termination_safe(b);
    JARVIS_ASSERT_EQ(0ULL, g_mismatch);
    a->is_user_ = false;
    b->is_user_ = false;
    write_live_tls(0);
    release_tls_task(a);
    release_tls_task(b);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Every cancel path leaves no stale TLS slot behind.
// Input: Publish a nonzero slot, drive cancel_pending_switch_cpu(0).
// Expect: Slot reads back 0.
// Depends: Cancel-path clears beside CR3 clears (issue #74, S2)
JARVIS_TEST(tls_cancel_clears_slot, "PRE: none | POST: none") {
    static uint64_t g_slot = 0;

    {
        arch::IrqGuard ig{};
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                         0x00007A7400000000ULL, __ATOMIC_RELEASE);
        Scheduler::cancel_pending_switch_cpu(0);
        g_slot = __atomic_load_n(&Scheduler::SwSlots::load_tls_from(),
                                 __ATOMIC_ACQUIRE);
    }
    JARVIS_ASSERT_EQ(0ULL, g_slot);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The second publish site (switch_away_from_terminating tick
//           path) publishes TLS end-to-end: a terminating driver arms to
//           a synthetic-user task that observes its live base.
//           NOTE (hang lesson): the driver must NOT return after arming —
//           the trampoline terminate() re-arms off a dying current task,
//           which would clobber this arm and strand the armed target
//           (marked RUNNING, never dispatched). The sanctioned sys_exit
//           never returns either. So D blocks itself post-arm (BLOCKED +
//           dequeued per §11.2, hlt) and is terminated from outside.
// Input: Driver D (prio 12) arms-away to V (prio 11, base B), then
//        self-blocks; V reads the live register on dispatch.
// Expect: V observed exactly B; slot consumed (0) afterwards.
// Depends: switch_away_from_terminating publish (issue #74)
JARVIS_TEST(tls_second_publish_site, "PRE: none | POST: none") {
    static constexpr uint64_t k_base_v = 0x00007CC000000000ULL;
    static uint64_t g_observed = 0;
    static uint64_t g_slot = 0;

    auto *v = TaskControlBlock::create(
        []() {
            // Tolerate pre-arm dispatch (V may run before D arms): retry
            // boundedly until the applied base appears. Bound is small:
            // same-prio FIFO guarantees D runs on V's first yield, so
            // the arm lands within a couple of rounds (a large bound
            // burns minutes under TCG with zero added coverage).
            for (uint64_t i = 0; i < 20; ++i) {
                uint64_t live = read_live_tls();
                if (live == k_base_v) {
                    g_observed = live;
                    return;
                }
                Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::YIELD), 0, 0,
                    0, 0, nullptr);
            }
        },
        11, 10);
    JARVIS_ASSERT(v != nullptr);
    v->is_user_ = true;
    v->tls_base_ = k_base_v;
    Scheduler::add_task(*v);

    auto *d = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            JARVIS_ASSERT(self != nullptr);
            Scheduler::switch_away_from_terminating(*self);
            // Never return (see note above): self-block, terminated
            // from outside at teardown.
            {
                arch::IrqGuard ig{};
                self->state = TaskState::BLOCKED;
                Scheduler::dequeue_ready(*self);
            }
            for (;;)
                arch::hlt();
        },
        12, 10);
    JARVIS_ASSERT(d != nullptr);
    Scheduler::add_task(*d);
    kernel::test::wait_for_termination_safe(v);
    g_slot = __atomic_load_n(&Scheduler::SwSlots::load_tls_from(),
                             __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_EQ(k_base_v, g_observed);
    JARVIS_ASSERT_EQ(0ULL, g_slot);
    v->is_user_ = false;
    write_live_tls(0);
    release_tls_task(d);
    release_tls_task(v);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Arch-portable stale-slot pin: publish nonzero via the C++
//           accessor, clear_switch_globals() must zero it (pins the
//           publish+arm atomicity invariant on non-x86 builds too; live
//           epilogue coverage awaits #28-family real-arch execution).
// Input: Store nonzero to load_tls_from, call clear_switch_globals().
// Expect: Slot reads back 0.
// Depends: clear_switch_globals TLS clear (issue #74)
JARVIS_TEST(tls_no_stale_slot_after_cancel, "PRE: none | POST: none") {
    static uint64_t g_slot = 0;

    {
        arch::IrqGuard ig{};
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                         0x00007A7400000000ULL, __ATOMIC_RELEASE);
        Scheduler::clear_switch_globals();
        g_slot = __atomic_load_n(&Scheduler::SwSlots::load_tls_from(),
                                 __ATOMIC_ACQUIRE);
    }
    JARVIS_ASSERT_EQ(0ULL, g_slot);
    JARVIS_TEST_PASS();
}

void register_tls_tests() {
    Logger::info("Registering TLS tests");
    JARVIS_REGISTER_TEST(tls_set_zero_clears);
    JARVIS_REGISTER_TEST(tls_set_rejects_noncanonical);
    JARVIS_REGISTER_TEST(tls_set_rejects_kernel_half);
    JARVIS_REGISTER_TEST(tls_set_accepts_user_half);
    JARVIS_REGISTER_TEST(tls_clone_inherits);
    JARVIS_REGISTER_TEST(tls_snapshot_parity);
    JARVIS_REGISTER_TEST(tls_not_fast);
    JARVIS_REGISTER_TEST(tls_publish_applies);
    JARVIS_REGISTER_TEST(tls_preemption_ping_pong);
    JARVIS_REGISTER_TEST(tls_cancel_clears_slot);
    JARVIS_REGISTER_TEST(tls_second_publish_site);
    JARVIS_REGISTER_TEST(tls_no_stale_slot_after_cancel);
}
