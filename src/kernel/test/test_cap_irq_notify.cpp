/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file test_cap_irq_notify.cpp
/// @brief User-space IRQ delivery — NOTIFY mode tests (issue #7): the
///        hardware IRQ dispatcher transforms incoming interrupts into
///        capability-backed IPC notifications on the recipient task's Notify
///        (eliminating Ring 0 driver execution).  Real ISR vectors are NOT
///        injected — the harness drives IrqDelivery::isr_entry directly
///        (deterministic, no hardware dependency).  Vector 40 (RTC, IRQ8) is
///        used throughout: its periodic interrupt is disabled at boot, so no
///        spurious delivery perturbs the tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/hal/interrupt_controller.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace {

/// @brief Test vector: RTC IRQ line (vector 40), periodic interrupts disabled.
constexpr uint8_t kTestVector = 40;

// -- shared result flags for blocking/delivery tasks --
uint64_t g_wait_ret = 0;
volatile bool g_armed = false;
volatile bool g_inject_done = false;
uint64_t g_reg_ret = 0;
uint64_t g_notify_val = 0;

/// @brief Busy-wait that yields to the scheduler exactly like
///        wait_for_termination_safe: re-arms scheduler_need_resched and
///        hlts, so the timer ISR can dispatch the cooperating peer task.
template <typename Pred> inline void yield_wait_until(Pred cond) {
    while (!cond()) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

/// @brief True once the delivery slot for @p vector is armed in NOTIFY mode.
bool slot_armed_notify(uint8_t vector) {
    auto *reg_slot = IrqDelivery::find(vector);
    return reg_slot != nullptr && reg_slot->armed &&
           reg_slot->delivery_mode == IrqDeliveryMode::NOTIFY;
}

/// @brief True once the task @p t is BLOCKED (registered as its Notify's
///        waiter inside sys_notify_wait).
bool task_blocked(TaskControlBlock *t) {
    return t->state == TaskState::BLOCKED;
}

/// @brief Task fixture: installs an IrqCap (WRITE|READ), arms it in NOTIFY
///        mode via sys_irq_register(arg1=1), then blocks on the Notify wait
///        until an injected IRQ (or revoke) wakes it.  Stores the wait result
///        in g_wait_ret / g_notify_val.  Busy-waits on g_inject_done when
///        @p wait_for_inject is true.
void irq_notify_waiter_entry(bool wait_for_inject) {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    auto *irq = cap::IrqCap::create(kTestVector);
    if (!irq) {
        g_wait_ret = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    int s = cs->install(irq, cap::CapType::Irq,
                        cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
    if (s < 0) {
        irq->release();
        g_wait_ret = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }
    irq->release();
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));
    // arg1 = 1 selects NOTIFY delivery mode (issue #7).
    g_reg_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER),
                                h, 1, 0, 0, nullptr);
    g_armed = true;
    if (wait_for_inject) {
        while (!g_inject_done)
            arch::pause();
    }
    // NOTIFY-driver blocking pattern: Notify::wait() registers us as the
    // Notify's waiter, sets BLOCKED and arms a deferred reschedule — it does
    // NOT spin (only IrqThread-style loops spin until a timer tick applies
    // the switch).  Mirror sys_irq_wait's spin on state==BLOCKED so the task
    // persists BLOCKED until the injected IRQ (or revoke) wakes it.
    g_notify_val = 0;
    g_wait_ret =
        Syscall::handle(static_cast<uint64_t>(SyscallNumber::NOTIFY_WAIT),
                        reinterpret_cast<uint64_t>(&g_notify_val), 0, 0, 0,
                        nullptr);
    if (arch::interrupts_enabled()) {
        while (cur->state == TaskState::BLOCKED) {
            arch::pause();
        }
    } else {
        // Interrupts disabled: roll back to a consistent RUNNING state.
        cur->notify.init();
        cur->state = TaskState::RUNNING;
        Scheduler::enqueue_ready(*cur);
    }
    // Re-read the delivered value after the wake: the ISR may have delivered
    // while we were descheduled mid-wait, in which case Notify::wait()
    // returned a stale 0.  try_wait consumes non-zero pending values; a
    // revoked sentinel (0) leaves g_notify_val 0 — both are asserted by the
    // tests.
    uint64_t reval = 0;
    if (cur->notify.try_wait(&reval)) {
        g_notify_val = reval;
    }
    cs->remove(static_cast<uint32_t>(s));
    Scheduler::terminate(*cur, 0);
}

} // namespace

// Runmode: kernel
// Testidea: An IrqCap armed with an unknown delivery mode fails closed: the
//           register returns -1 and the slot is never armed.
// Input: sys_irq_register(arg1=2) over a valid WRITE IrqCap
// Expect: ret -1; delivery slot unarmed; table returns to baseline
// Depends: kernel::Syscall, kernel::IrqDelivery
JARVIS_TEST(irq_notify_mode_validation, "PRE: none | POST: none") {
    g_reg_ret = 0;
    g_armed = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *irq = cap::IrqCap::create(kTestVector);
            if (!irq) {
                g_reg_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(irq, cap::CapType::Irq,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                irq->release();
                g_reg_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h =
                cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                   cs->slot_gen(static_cast<uint32_t>(s)));
            // Unknown mode 2 — must be rejected, never armed.
            uint64_t bad = static_cast<uint64_t>(-1);
            uint64_t r1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 2, 0,
                0, nullptr);
            uint64_t r2 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0xFF,
                0, 0, nullptr);
            uint64_t r3 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0x101,
                0, 0, nullptr);
            JARVIS_ASSERT_EQ(bad, r1);
            JARVIS_ASSERT_EQ(bad, r2);
            JARVIS_ASSERT_EQ(bad, r3);
            auto *reg = IrqDelivery::find(kTestVector);
            JARVIS_ASSERT(reg == nullptr || !reg->armed);
            cs->remove(static_cast<uint32_t>(s));
            irq->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A NOTIFY-mode IRQ delivered before the wait is not lost: the ISR
//           stores the vector value on the recipient's Notify and try_wait
//           consumes it without blocking.
// Input: task arms mode=1; harness injects isr_entry; task try_waits
// Expect: try_wait true; value == vector; no block, clean teardown
// Depends: kernel::IrqDelivery, kernel::sync::Notify
JARVIS_TEST(irq_notify_pending_immediate, "PRE: none | POST: none") {
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *irq = cap::IrqCap::create(kTestVector);
            if (!irq) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(irq, cap::CapType::Irq,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                irq->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            irq->release();
            uint64_t h =
                cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                   cs->slot_gen(static_cast<uint32_t>(s)));
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 1, 0,
                0, nullptr);
            g_armed = true;
            while (!g_inject_done)
                arch::pause();
            // The ISR already stored the vector on our Notify — consume it
            // without blocking (Notify::wait() always blocks; a NOTIFY-mode
            // driver polls try_wait or blocks before the first IRQ).
            uint64_t val = 0;
            g_wait_ret = cur->notify.try_wait(&val) ? 1 : 0;
            g_notify_val = val;
            cs->remove(static_cast<uint32_t>(s));
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));
    g_inject_done = true;

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(1ULL, g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(kTestVector), g_notify_val);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task blocked in sys_notify_wait is woken by an injected
//           NOTIFY-mode IRQ and receives the vector value (real two-task
//           blocking wakeup via the IPC notification bridge).
// Input: waiter task arms mode=1 then blocks in NOTIFY_WAIT; harness delivers
// Expect: waiter wakes with the vector value, terminates cleanly
// Depends: kernel::IrqDelivery, kernel::Syscall, kernel::Scheduler
JARVIS_TEST(irq_notify_blocking_wakeup, "PRE: none | POST: none") {
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;
    g_notify_val = 0;

    auto *t = TaskControlBlock::create(
        []() { irq_notify_waiter_entry(false); }, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    // Handshake: the task armed the slot AND is BLOCKED inside
    // Notify::wait() (its Notify waiter is registered).
    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() { return task_blocked(t); });
    JARVIS_ASSERT(slot_armed_notify(kTestVector));
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(0ULL, g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(kTestVector), g_notify_val);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys_irq_wait refuses a slot armed in NOTIFY mode — the blocking
//           wait path bypasses pending/waiter bookkeeping, so WAIT would
//           never wake (fail closed, both lock scopes).
// Input: task arms mode=1; then calls SYS_IRQ_WAIT on the same handle
// Expect: wait returns -1; slot stays armed; clean teardown
// Depends: kernel::Syscall, kernel::IrqDelivery
JARVIS_TEST(irq_notify_wait_syscall_refused, "PRE: none | POST: none") {
    g_armed = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *irq = cap::IrqCap::create(kTestVector);
            if (!irq) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(irq, cap::CapType::Irq,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                irq->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            irq->release();
            uint64_t h =
                cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                   cs->slot_gen(static_cast<uint32_t>(s)));
            uint64_t bad = static_cast<uint64_t>(-1);
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 1, 0,
                0, nullptr);
            // The slot is armed in NOTIFY mode — sys_irq_wait must refuse it.
            uint64_t rw = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0, 0, 0,
                nullptr);
            JARVIS_ASSERT_EQ(bad, rw);
            auto *reg = IrqDelivery::find(kTestVector);
            JARVIS_ASSERT(reg != nullptr && reg->armed);
            JARVIS_ASSERT(reg->delivery_mode == IrqDeliveryMode::NOTIFY);
            g_armed = true;
            cs->remove(static_cast<uint32_t>(s));
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A driver blocked in sys_notify_wait is woken with the revoked
//           sentinel (0) when the cap is revoked mid-wait — never blocked
//           forever (wakers own the wakeup, CODING_STYLE §12.3).
// Input: waiter task blocks in NOTIFY_WAIT; harness revokes the cap
// Expect: waiter wakes with value 0, terminates cleanly
// Depends: kernel::cap, kernel::IrqDelivery, kernel::Syscall
JARVIS_TEST(irq_notify_revoke_wakes_waiter, "PRE: none | POST: none") {
    static uint64_t g_handle = 0;
    static TaskControlBlock *g_waiter = nullptr;
    g_handle = 0;
    g_waiter = nullptr;
    g_wait_ret = 0;
    g_notify_val = 0;
    g_armed = false;
    g_inject_done = false;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *irq = cap::IrqCap::create(kTestVector);
            if (!irq) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(irq, cap::CapType::Irq,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                irq->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h =
                cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                   cs->slot_gen(static_cast<uint32_t>(s)));
            g_handle = h;
            g_waiter = cur;
            g_armed = true;
            uint64_t reg = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 1, 0,
                0, nullptr);
            g_reg_ret = reg;
            if (reg != 0) {
                cs->remove(static_cast<uint32_t>(s));
                irq->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            g_wait_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::NOTIFY_WAIT),
                reinterpret_cast<uint64_t>(&g_notify_val), 0, 0, 0, nullptr);
            // Mirror the blocking fixture: persist BLOCKED until the revoke
            // signals the Notify with the revoked sentinel (0).
            if (arch::interrupts_enabled()) {
                while (cur->state == TaskState::BLOCKED) {
                    arch::pause();
                }
            } else {
                cur->notify.init();
                cur->state = TaskState::RUNNING;
                Scheduler::enqueue_ready(*cur);
            }
            uint64_t reval = 0;
            if (cur->notify.try_wait(&reval)) {
                g_notify_val = reval;
            }
            cs->remove(static_cast<uint32_t>(s));
            irq->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() { return task_blocked(t); });

    // Revoke the cap mid-wait: release_slot_idx signals the Notify with the
    // revoked sentinel (0) — the driver wakes, never BLOCKED forever.
    JARVIS_ASSERT(g_handle != 0);
    JARVIS_ASSERT(g_waiter != nullptr);
    JARVIS_ASSERT(cap::revoke(g_waiter->get_cspace(), g_handle));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(0ULL, g_wait_ret);
    JARVIS_ASSERT_EQ(0ULL, g_notify_val);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A NOTIFY-mode task that dies with a delivered notification leaves
//           no dangling recipient in the delivery table: cleanup drains the
//           slot and re-masks the vector (the destroyed-Notify ordering trap —
//           drain_task must NOT notify the dying task's destroyed Notify).
// Input: task arms mode=1; harness injects; task terminates
// Expect: delivery slot drained; ResourceTracker deltas zero
// Depends: kernel::IrqDelivery, TaskControlBlock::cleanup
JARVIS_TEST(irq_notify_task_died_drain, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() { irq_notify_waiter_entry(true); }, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    while (!g_armed)
        arch::pause();
    // Deliver a notification but do NOT release the gate — the task stays in
    // its spin, then we terminate it while the slot still has a recipient.
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));
    JARVIS_ASSERT(IrqDelivery::find(kTestVector) != nullptr);

    // Terminate the task: cleanup() drains the delivery table.
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(IrqDelivery::find(kTestVector) == nullptr);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(before.tasks, after.tasks);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The PIC mask state and the delivery table return to their
//           pre-test baseline after a full NOTIFY-mode register → deliver →
//           teardown cycle.  x86_64 only — the 8259A line-mask save/restore
//           does not exist on other architectures.
// Input: full cycle on vector 40 with PIC mask snapshot/restore
// Expect: mask == snapshot; occupied_count == baseline; RT == baseline
// Depends: kernel::arch::ArchInterruptController, kernel::IrqDelivery
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(irq_notify_ack_pic_state, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    arch::IrqState pic = arch::ArchInterruptController::snapshot();
    size_t base_occ = IrqDelivery::occupied_count();

    auto *irq = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(irq != nullptr);
    JARVIS_ASSERT(IrqDelivery::arm(static_cast<int16_t>(irq->reg_idx_), *irq,
                                   irq->vector, *Scheduler::current_task(),
                                   IrqDeliveryMode::NOTIFY));

    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));

    irq->release(); // dispose → disarm + re-mask

    JARVIS_ASSERT_EQ(base_occ, IrqDelivery::occupied_count());
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(pic.pic1_mask),
                     static_cast<uint16_t>(arch::ArchInterruptController::snapshot().pic1_mask));
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(pic.pic2_mask),
                     static_cast<uint16_t>(arch::ArchInterruptController::snapshot().pic2_mask));
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}
#endif // CONFIG_ARCH_X86_64

/// @brief Registers all NOTIFY-mode IRQ delivery test cases (issue #7).
void register_cap_irq_notify_tests() {
    JARVIS_REGISTER_TEST(irq_notify_mode_validation);
    JARVIS_REGISTER_TEST(irq_notify_pending_immediate);
    JARVIS_REGISTER_TEST(irq_notify_blocking_wakeup);
    JARVIS_REGISTER_TEST(irq_notify_wait_syscall_refused);
    JARVIS_REGISTER_TEST(irq_notify_revoke_wakes_waiter);
    JARVIS_REGISTER_TEST(irq_notify_task_died_drain);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(irq_notify_ack_pic_state);
#endif
}