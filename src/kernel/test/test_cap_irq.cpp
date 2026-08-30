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

/// @file test_cap_irq.cpp
/// @brief IRQ caps + user-space IRQ delivery tests (issue #2): IrqCap
///        lifecycle, capability-gated sys_irq_register/sys_irq_wait dispatch
///        + validation, ISR delivery to a registered task, revocation / task
///        teardown wakeup contracts, PIC mask-state restoration.  Real ISR
///        vectors are NOT injected — the harness drives IrqDelivery::isr_entry
///        directly (deterministic, no hardware dependency).  Vector 40 (RTC,
///        IRQ8) is used throughout: its periodic interrupt is disabled at boot
///        (RTC::init), so no spurious delivery perturbs the tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/memory/kernel_object.hpp>
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

/// @brief Runs @p entry as a real task, waits for termination and drains
///        zombies (test_cap_mmio pattern).
TaskControlBlock *run_irq_task(void (*entry)(), uint64_t prio = 11,
                               uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    return t;
}

// -- shared result flags for blocking/delivery tasks --
uint64_t g_wait_ret = 0;
volatile bool g_armed = false;
volatile bool g_inject_done = false;
uint64_t g_reg_ret = 0;

/// @brief Busy-wait that yields to the scheduler exactly like
///        wait_for_termination_safe: re-arms scheduler_need_resched and
///        hlts, so the timer ISR can dispatch the cooperating peer task.
///        Deterministic under the deferred-switch model (an arch::pause()
///        spin does not reliably yield to a higher-priority peer).
template <typename Pred> inline void yield_wait_until(Pred cond) {
    while (!cond()) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

/// @brief True once the delivery slot has @p t registered as its waiter.
bool waiter_is(TaskControlBlock *t) {
    auto *reg_slot = IrqDelivery::find(kTestVector);
    return reg_slot != nullptr && reg_slot->waiter == t;
}

/// @brief Task fixture: installs an IrqCap (WRITE|READ), arms it, then either
///        returns immediately (pending-immediate) or blocks in sys_irq_wait.
///        Stores the wait result in g_wait_ret.  Busy-waits on g_inject_done
///        when @p wait_for_inject is true (pending-immediate ordering).
void irq_waiter_entry(bool wait_for_inject) {
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
    // The CSpace slot now holds the only reference.  Drop the creator ref
    // immediately so an externally-terminated task (died-drain fixture) can
    // never abandon it — cleanup() then disposes the object via the slot.
    irq->release();
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));
    g_reg_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER),
                                h, 0, 0, 0, nullptr);
    g_armed = true;
    if (wait_for_inject) {
        while (!g_inject_done)
            arch::pause();
    }
    g_wait_ret =
        Syscall::handle(static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0,
                        0, 0, nullptr);
    cs->remove(static_cast<uint32_t>(s));
    Scheduler::terminate(*cur, 0);
}

} // namespace

// Runmode: kernel
// Testidea: An IrqCap wraps an IRQ vector with correct fields and zero
//           ResourceTracker delta after release.
// Input: create(vector 40)
// Expect: fields match; cap_objects returns to baseline after release
// Depends: kernel::cap::IrqCap
JARVIS_TEST(irq_cap_create_fields, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    auto *irq = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(irq != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(kTestVector), irq->vector);
    JARVIS_ASSERT(irq->reg_idx_ >= 0);
    JARVIS_ASSERT(irq->is_pool_backed());

    irq->release(); // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: IrqCap vector validation fails closed: vectors outside the
//           hardware IRQ window and the reserved timer vector (32) are
//           rejected.
// Input: create(31), create(48), create(32), create(33)
// Expect: only the in-window non-timer vector succeeds
// Depends: kernel::cap::IrqCap
JARVIS_TEST(irq_cap_vector_bounds, "PRE: none | POST: none") {
    JARVIS_ASSERT(cap::IrqCap::create(31) == nullptr);
    JARVIS_ASSERT(cap::IrqCap::create(48) == nullptr);
    JARVIS_ASSERT(cap::IrqCap::create(32) == nullptr); // timer reserved
    auto *irq = cap::IrqCap::create(33);
    JARVIS_ASSERT(irq != nullptr);
    irq->release();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Only one live IrqCap may claim a given vector — a second create
//           for the same vector fails closed (single-owner).
// Input: create(40) then create(40) again
// Expect: second create returns nullptr; first still usable; create works
//         again after release
// Depends: kernel::cap::IrqCap
JARVIS_TEST(irq_cap_single_owner_vector, "PRE: none | POST: none") {
    auto *a = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(cap::IrqCap::create(kTestVector) == nullptr);
    a->release();

    // Slot freed — the vector is claimable again.
    auto *b = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(b != nullptr);
    b->release();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The delivery table is bounded: claiming every in-window vector
//           fills the table and the next create fails closed; releasing
//           restores.
// Input: create one cap per in-window vector (33..47), then one more
// Expect: all 15 succeed; the 16th fails; after release the table is empty
// Depends: kernel::cap::IrqCap, kernel::IrqDelivery
JARVIS_TEST(irq_cap_live_bound_exhaustion, "PRE: none | POST: none") {
    enum { FIRST = 33, LAST = 47 };
    cap::IrqCap *caps[LAST - FIRST + 1];
    size_t n = 0;
    for (uint8_t v = FIRST; v <= LAST; ++v) {
        auto *c = cap::IrqCap::create(v);
        JARVIS_ASSERT(c != nullptr);
        caps[n++] = c;
    }
    // Table + vector space exhausted.
    JARVIS_ASSERT(cap::IrqCap::create(FIRST) == nullptr);
    JARVIS_ASSERT_EQ(n, IrqDelivery::occupied_count());

    for (size_t i = 0; i < n; ++i)
        caps[i]->release();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An installed IrqCap is found by lookup with the right type and
//           rights, rejected with wrong type / missing rights / stale gen,
//           and revoked caps refuse acquire.
// Input: install Irq cap (WRITE); lookup variants; revoke
// Expect: lookup honors type/rights/gen; revoke invalidates
// Depends: kernel::cap, kernel::TaskControlBlock
JARVIS_TEST(irq_cap_install_lookup_revoke, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    cur->ensure_cspace();

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    cap::CNode *cs = cur->get_cspace();
    auto *irq = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(irq != nullptr);
    int s = cs->install(irq, cap::CapType::Irq, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));

    KernelObject *obj =
        cap::lookup(cs, h, cap::CapType::Irq, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(obj != nullptr);
    JARVIS_ASSERT(obj == static_cast<KernelObject *>(irq));
    obj->release();

    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Frame,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Irq,
                              cap::CAP_RIGHT_GRANT) == nullptr);
    uint64_t stale =
        cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                           cs->slot_gen(static_cast<uint32_t>(s)) + 1);
    JARVIS_ASSERT(cap::lookup(cs, stale, cap::CapType::Irq,
                              cap::CAP_RIGHT_WRITE) == nullptr);

    JARVIS_ASSERT(cap::revoke(cs, h));
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Irq,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(irq->revoked());

    cs->remove(static_cast<uint32_t>(s));
    irq->release(); // creator ref -> dispose (slot already cleared by revoke)

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys_irq_register over a live IrqCap succeeds: the vector is armed,
//           the current task becomes the recipient and the PIC line is
//           unmasked (PIC mask restore check is x86_64-only).
// Input: real task installing an IrqCap; dispatch SYS_IRQ_REGISTER
// Expect: ret 0; delivery slot armed; recipient == task
// Depends: kernel::Syscall, kernel::IrqDelivery
JARVIS_TEST(irq_register_dispatch_happy, "PRE: none | POST: none") {
    g_reg_ret = 0;
    g_armed = false;
    g_wait_ret = 0;

    arch::IrqState pic = arch::ArchInterruptController::snapshot();
#if !defined(CONFIG_ARCH_X86_64)
    (void)pic;
#endif

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
            int s = cs->install(irq, cap::CapType::Irq, cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                irq->release();
                g_reg_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h =
                cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                   cs->slot_gen(static_cast<uint32_t>(s)));
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            auto *reg = IrqDelivery::find(kTestVector);
            g_armed = (reg != nullptr) && reg->armed &&
                      (reg->recipient == cur);
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

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT(g_armed);

    // Slot released at task teardown: table empty, line masks restored
    // (x86_64: 8259A PIC masks; other architectures have no PIC state).
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(pic.pic1_mask),
                     static_cast<uint16_t>(arch::ArchInterruptController::snapshot().pic1_mask));
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(pic.pic2_mask),
                     static_cast<uint16_t>(arch::ArchInterruptController::snapshot().pic2_mask));
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys_irq_register rejects wrong type, missing WRITE right, stale
//           gen, revoked cap and double-registration (already armed) — all
//           fail closed with no slot claimed.
// Input: real task installing valid + read-only caps; dispatch variants
// Expect: every attempt returns -1; delivery slot stays unclaimed
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(irq_register_validation_matrix, "PRE: none | POST: none") {
    auto *t = run_irq_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();

        auto *irq = cap::IrqCap::create(kTestVector);
        auto *ro = cap::IrqCap::create(static_cast<uint8_t>(kTestVector + 1));
        if (!irq || !ro) {
            if (irq)
                irq->release();
            if (ro)
                ro->release();
            Scheduler::terminate(*cur, 0);
            return;
        }
        int s1 = cs->install(irq, cap::CapType::Irq, cap::CAP_RIGHT_WRITE);
        int s2 = cs->install(ro, cap::CapType::Irq, cap::CAP_RIGHT_READ);
        if (s1 < 0 || s2 < 0) {
            irq->release();
            ro->release();
            Scheduler::terminate(*cur, 0);
            return;
        }
        uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s1),
                                        cs->slot_gen(static_cast<uint32_t>(s1)));
        uint64_t hro = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s2),
                                          cs->slot_gen(static_cast<uint32_t>(s2)));

        auto attempt = [](uint64_t handle) {
            return Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), handle, 0,
                0, 0, nullptr);
        };

        // Missing WRITE right.
        uint64_t r1 = attempt(hro);
        // Stale generation.
        uint64_t stale =
            cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s1),
                               cs->slot_gen(static_cast<uint32_t>(s1)) + 1);
        uint64_t r2 = attempt(stale);
        // First successful register.
        uint64_t r3 = attempt(h);
        // Double-registration (already armed).
        uint64_t r4 = attempt(h);

        uint64_t bad = static_cast<uint64_t>(-1);
        JARVIS_ASSERT_EQ(bad, r1);
        JARVIS_ASSERT_EQ(bad, r2);
        JARVIS_ASSERT_EQ(0ULL, r3);
        JARVIS_ASSERT_EQ(bad, r4);
        // Wrong-type: use a FrameCap slot for a genuinely wrong type.
        auto *fr = cap::FrameCap::create(0x1000000ULL, 1, false);
        if (fr) {
            int sf = cs->install(fr, cap::CapType::Frame, cap::CAP_RIGHT_WRITE);
            if (sf >= 0) {
                uint64_t hf = cap::encode_handle(
                    cs->cspace_id, static_cast<uint32_t>(sf),
                    cs->slot_gen(static_cast<uint32_t>(sf)));
                JARVIS_ASSERT_EQ(bad, attempt(hf));
                cs->remove(static_cast<uint32_t>(sf));
            }
            fr->release();
        }

        // The successful register armed the vector for this task; the
        // failed attempts (wrong type / no WRITE / stale gen / double-arm)
        // did not perturb it.
        auto *reg = IrqDelivery::find(kTestVector);
        JARVIS_ASSERT(reg != nullptr);
        JARVIS_ASSERT(reg->armed);
        JARVIS_ASSERT(reg->recipient == cur);

        cs->remove(static_cast<uint32_t>(s1));
        cs->remove(static_cast<uint32_t>(s2));
        irq->release();
        ro->release();
        Scheduler::terminate(*cur, 0);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An IRQ delivered before the wait is not lost: pending increments
//           and sys_irq_wait consumes it without blocking.
// Input: task registers + arms; harness injects isr_entry; task waits
// Expect: wait returns the vector without blocking
// Depends: kernel::IrqDelivery, kernel::Syscall
JARVIS_TEST(irq_wait_pending_immediate, "PRE: none | POST: none") {
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() { irq_waiter_entry(true); }, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    // Wait until the task armed the vector, inject the IRQ, then release the
    // gate so the task calls sys_irq_wait with the pending already recorded.
    yield_wait_until([]() { return g_armed; });
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));
    g_inject_done = true;

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(kTestVector), g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task blocked in sys_irq_wait is woken by an injected IRQ and
//           returns the vector (real two-task blocking wakeup).
// Input: waiter task registers then blocks in IRQ_WAIT; harness delivers
// Expect: waiter wakes with the vector, terminates cleanly
// Depends: kernel::IrqDelivery, kernel::Syscall, kernel::Scheduler
JARVIS_TEST(irq_wait_blocking_wakeup, "PRE: none | POST: none") {
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() { irq_waiter_entry(false); }, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    // Wait until the task armed AND is registered as the slot's blocked waiter
    // (deterministic blocking-wakeup handshake, not just g_armed).
    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() { return waiter_is(t); });
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(kTestVector), g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A waiter blocked in sys_irq_wait is woken with -1 when the cap is
//           revoked mid-wait — never blocked forever.
// Input: waiter task blocks; harness revokes the cap
// Expect: waiter wakes, returns -1, terminates cleanly
// Depends: kernel::cap, kernel::IrqDelivery, kernel::Syscall
JARVIS_TEST(irq_wait_revoked_mid_wait, "PRE: none | POST: none") {
    static uint64_t g_handle = 0;
    static TaskControlBlock *g_waiter = nullptr;
    g_handle = 0;
    g_wait_ret = 0;
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
            uint64_t h = cap::encode_handle(cs->cspace_id,
                                            static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            g_handle = h;
            g_waiter = cur;
            g_armed = true;
            uint64_t reg = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            g_reg_ret = reg;
            if (reg != 0) {
                cs->remove(static_cast<uint32_t>(s));
                irq->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            g_wait_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0, 0, 0,
                nullptr);
            cs->remove(static_cast<uint32_t>(s));
            irq->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() { return waiter_is(t); });

    // Revoke the cap mid-wait: wakes the waiter with -1.
    JARVIS_ASSERT(g_handle != 0);
    JARVIS_ASSERT(g_waiter != nullptr);
    JARVIS_ASSERT(cap::revoke(g_waiter->get_cspace(), g_handle));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task that dies with a pending IRQ leaves no dangling
//           recipient/waiter in the delivery table: cleanup drains the slot
//           and re-masks the vector.
// Input: task registers; harness injects pending IRQ; task terminates
// Expect: delivery slot drained; recipient cleared; table returns to baseline
// Depends: kernel::IrqDelivery, TaskControlBlock::cleanup
JARVIS_TEST(irq_wait_task_died_drain, "PRE: none | POST: none") {
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;

    auto *t = TaskControlBlock::create(
        []() { irq_waiter_entry(true); }, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    while (!g_armed)
        arch::pause();
    // Inject a pending IRQ but do NOT release the gate — the task stays in
    // its spin, then we terminate it while the slot still has pending work.
    JARVIS_ASSERT(IrqDelivery::isr_entry(kTestVector));
    JARVIS_ASSERT(IrqDelivery::find(kTestVector) != nullptr);

    // Terminate the task: cleanup() drains the delivery table.
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(IrqDelivery::find(kTestVector) == nullptr);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The PIC mask state and the delivery table return to their
//           pre-test baseline after a full register → deliver → teardown
//           cycle.  x86_64 only — the 8259A line-mask save/restore does not
//           exist on other architectures.
// Input: full cycle on vector 40 with PIC mask snapshot/restore
// Expect: mask == snapshot; occupied_count == baseline; RT == baseline
// Depends: kernel::arch::ArchInterruptController, kernel::IrqDelivery
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(irq_delivery_ack_pic_state, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    arch::IrqState pic = arch::ArchInterruptController::snapshot();
    size_t base_occ = IrqDelivery::occupied_count();

    auto *irq = cap::IrqCap::create(kTestVector);
    JARVIS_ASSERT(irq != nullptr);
    JARVIS_ASSERT(IrqDelivery::arm(static_cast<int16_t>(irq->reg_idx_), *irq,
                                   *Scheduler::current_task()));

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

/// @brief Registers all IRQ-cap / user-space delivery test cases (issue #2).
void register_cap_irq_tests() {
    JARVIS_REGISTER_TEST(irq_cap_create_fields);
    JARVIS_REGISTER_TEST(irq_cap_vector_bounds);
    JARVIS_REGISTER_TEST(irq_cap_single_owner_vector);
    JARVIS_REGISTER_TEST(irq_cap_live_bound_exhaustion);
    JARVIS_REGISTER_TEST(irq_cap_install_lookup_revoke);
    JARVIS_REGISTER_TEST(irq_register_dispatch_happy);
    JARVIS_REGISTER_TEST(irq_register_validation_matrix);
    JARVIS_REGISTER_TEST(irq_wait_pending_immediate);
    JARVIS_REGISTER_TEST(irq_wait_blocking_wakeup);
    JARVIS_REGISTER_TEST(irq_wait_revoked_mid_wait);
    JARVIS_REGISTER_TEST(irq_wait_task_died_drain);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(irq_delivery_ack_pic_state);
#endif
}
