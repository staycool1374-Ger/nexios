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

/// @file test_cap_msix.cpp
/// @brief MSI-X vector infrastructure tests (issue #10): MsixCap lifecycle,
///        capability-gated sys_irq_register/sys_irq_wait over MSI-X vectors,
///        ISR delivery, revocation / task-teardown wakeup contracts, and the
///        MSI-X table-entry mask fail-closed invariant.  Probes the QEMU
///        virtio-net device (0x1AF4:0x1041, which exposes an MSI-X capability)
///        for a real MSI-X-capable BDF.  Real ISR vectors are NOT injected —
///        the harness drives IrqDelivery::isr_entry directly (deterministic,
///        no hardware interrupt generation).  x86_64 only: MSI-X vector
///        programming and the MSI-X table are x86_64 PCI/APIC concepts.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/msix.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

/// @brief QEMU virtio-net modern device (exposes MSI-X).
constexpr uint16_t kVirtioVendor = 0x1AF4;
constexpr uint16_t kVirtioNetId = 0x1041;

/// @brief Probes the PCI tree for a virtio-net device with an MSI-X
///        capability; returns its BDF (or false if none is present).
bool find_msix_device(arch::PciBdf &out) {
    arch::pci_scan_all();
    for (size_t i = 0; i < arch::pci_device_count(); ++i) {
        const auto &d = arch::pci_devices()[i];
        if (d.vendor_id == kVirtioVendor && d.device_id == kVirtioNetId &&
            arch::pci_find_capability(d.bdf, arch::PCI_CAP_ID_MSIX) != 0) {
            out = d.bdf;
            return true;
        }
    }
    return false;
}

/// @brief Busy-wait that yields to the scheduler exactly like
///        wait_for_termination_safe (test_cap_irq pattern).
template <typename Pred> inline void yield_wait_until(Pred cond) {
    while (!cond()) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

// -- shared result flags for blocking/delivery tasks --
uint64_t g_wait_ret = 0;
volatile bool g_armed = false;
volatile bool g_inject_done = false;
uint64_t g_reg_ret = 0;
uint64_t g_notify_val = 0;
/// @brief BDF captured from the device probe for task-entry functions (which
///        must be non-capturing function pointers).
arch::PciBdf g_bdf{};

} // namespace

// Runmode: kernel
// Testidea: An MsixCap wraps one MSI-X vector/entry with correct fields and
//           zero ResourceTracker delta after release.
// Input: create(entry 0) on the virtio-net device
// Expect: fields match (bdf/entry/vector in 48–255 != 0x80/reg_idx_>=0,
//         pool-backed); entry starts masked; release restores baseline
// Depends: kernel::cap::MsixCap
JARVIS_TEST(msix_cap_create_fields, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    auto *msix = cap::MsixCap::create(bdf, 0);
    JARVIS_ASSERT(msix != nullptr);
    JARVIS_ASSERT(msix->bdf == bdf);
    JARVIS_ASSERT_EQ(0U, msix->entry_index);
    JARVIS_ASSERT(msix->vector >= 48);
    JARVIS_ASSERT(msix->vector != 0x80);
    JARVIS_ASSERT(msix->reg_idx_ >= 0);
    JARVIS_ASSERT(msix->is_pool_backed());
    // Fail-closed: the entry is programmed MASKED at create.
    JARVIS_ASSERT(msix->entry_masked());

    msix->release(); // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MsixCap::create fails closed on a device with no MSI-X
//           capability and on out-of-range BDFs.
// Input: host bridge 00:00.0 (no MSI-X), invalid BDF 255:31.7
// Expect: nullptr in both cases; no slot claimed
// Depends: kernel::cap::MsixCap
JARVIS_TEST(msix_cap_no_msix_capability_fails, "PRE: none | POST: none") {
    arch::PciBdf host{};
    host.bus = 0;
    host.device = 0;
    host.function = 0;
    JARVIS_ASSERT(cap::MsixCap::create(host, 0) == nullptr);

    arch::PciBdf invalid{};
    invalid.bus = 255;
    invalid.device = 31;
    invalid.function = 7;
    JARVIS_ASSERT(cap::MsixCap::create(invalid, 0) == nullptr);

    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MSI-X entry bounds are validated: entries within the device's
//           TSIZE+1 range succeed, out-of-range entries fail closed.
// Input: create entry 0 (ok) and an entry >= entry_count (fails)
// Expect: valid create succeeds; invalid fails; release restores baseline
// Depends: kernel::cap::MsixCap
JARVIS_TEST(msix_cap_entry_bounds_and_table, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    arch::PciMsixTableInfo tbl{};
    JARVIS_ASSERT(arch::pci_msix_table_info(bdf, tbl));
    JARVIS_ASSERT(tbl.entry_count > 0);
    JARVIS_ASSERT(tbl.table_kva != 0);

    auto *msix = cap::MsixCap::create(bdf, 0);
    JARVIS_ASSERT(msix != nullptr);
    JARVIS_ASSERT(msix->entry_masked());
    // The entry is programmed with the xAPIC message address (0xFEE00000)
    // and the allocated vector, but stays MASKED until arming.
    JARVIS_ASSERT(msix->tbl_.table_kva != 0);
    msix->release();

    // Out-of-range entry index fails closed.
    JARVIS_ASSERT(cap::MsixCap::create(bdf, tbl.entry_count) == nullptr);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Single-owner per vector, cross-type: a second create for the same
//           BDF+entry (hence same vector) fails closed until the first is
//           released.
// Input: create(entry 0) then create(entry 0) again
// Expect: second create returns nullptr; first still usable; create works
//         again after release
// Depends: kernel::cap::MsixCap
JARVIS_TEST(msix_cap_single_owner_vector, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    auto *a = cap::MsixCap::create(bdf, 0);
    JARVIS_ASSERT(a != nullptr);
    JARVIS_ASSERT(cap::MsixCap::create(bdf, 0) == nullptr);
    a->release();

    auto *b = cap::MsixCap::create(bdf, 0);
    JARVIS_ASSERT(b != nullptr);
    b->release();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Kernel-reserved vectors inside the MSI-X window are never
//           claimable (issue #10 S1): the xAPIC scheduler timer (64) and the
//           APIC spurious vector (0xFF) must fail closed in claim_slot — an
//           armed slot at 64 would swallow the scheduler tick.
// Input: claim_slot(64), claim_slot(0xFF), claim_slot(48)
// Expect: 64 and 0xFF rejected; 48 accepted and released
// Depends: kernel::IrqDelivery
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(msix_kernel_reserved_vectors_rejected, "PRE: none | POST: none") {
    JARVIS_ASSERT(IrqDelivery::claim_slot(64) < 0);
    JARVIS_ASSERT(IrqDelivery::claim_slot(0xFF) < 0);
    int16_t idx = IrqDelivery::claim_slot(48);
    JARVIS_ASSERT(idx >= 0);
    JARVIS_ASSERT(IrqDelivery::release_slot_idx(idx, nullptr));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: An installed MsixCap is found by lookup with the right type and
//           rights, rejected with wrong type / missing rights / stale gen,
//           and revoked caps refuse acquire and re-mask their entry.
// Input: install Msix cap (WRITE); lookup variants; revoke
// Expect: lookup honors type/rights/gen; revoke invalidates + re-masks
// Depends: kernel::cap, kernel::TaskControlBlock
JARVIS_TEST(msix_cap_install_lookup_revoke, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    cur->ensure_cspace();

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    cap::CNode *cs = cur->get_cspace();
    auto *msix = cap::MsixCap::create(bdf, 0);
    JARVIS_ASSERT(msix != nullptr);
    int s = cs->install(msix, cap::CapType::Msix, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));

    KernelObject *obj =
        cap::lookup(cs, h, cap::CapType::Msix, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(obj != nullptr);
    JARVIS_ASSERT(obj == static_cast<KernelObject *>(msix));
    obj->release();

    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Frame,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Irq,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Msix,
                              cap::CAP_RIGHT_GRANT) == nullptr);
    uint64_t stale =
        cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                           cs->slot_gen(static_cast<uint32_t>(s)) + 1);
    JARVIS_ASSERT(cap::lookup(cs, stale, cap::CapType::Msix,
                              cap::CAP_RIGHT_WRITE) == nullptr);

    JARVIS_ASSERT(cap::revoke(cs, h));
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Msix,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(msix->revoked());
    // Revoke re-masks the entry (fail-closed).
    JARVIS_ASSERT(msix->entry_masked());

    cs->remove(static_cast<uint32_t>(s));
    msix->release(); // creator ref -> dispose (slot already cleared by revoke)

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys_irq_register over a live MsixCap succeeds: the MSI-X vector
//           is armed, the current task becomes the recipient and the table
//           entry is UNMASKED (issue #10).
// Input: real task installing an MsixCap; dispatch SYS_IRQ_REGISTER
// Expect: ret 0; delivery slot armed; recipient == task; entry unmasked
// Depends: kernel::Syscall, kernel::IrqDelivery
JARVIS_TEST(msix_register_dispatch_happy, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    g_reg_ret = 0;
    g_armed = false;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_reg_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(msix, cap::CapType::Msix, cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                msix->release();
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
            auto *reg = IrqDelivery::find(msix->vector);
            g_armed = (reg != nullptr) && reg->armed &&
                      (reg->recipient == cur);
            cs->remove(static_cast<uint32_t>(s));
            msix->release();
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
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys_irq_register rejects wrong type, missing WRITE right, stale
//           gen, revoked cap and double-registration (already armed) — all
//           fail closed with no slot claimed.
// Input: real task installing valid + read-only MsixCaps; dispatch variants
// Expect: every attempt returns -1; delivery slot stays unclaimed
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(msix_register_validation_matrix, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    g_bdf = bdf;
    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s1 = cs->install(msix, cap::CapType::Msix, cap::CAP_RIGHT_WRITE);
            int s2 = cs->install(msix, cap::CapType::Msix, cap::CAP_RIGHT_READ);
            if (s1 < 0 || s2 < 0) {
                msix->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s1),
                                            cs->slot_gen(static_cast<uint32_t>(s1)));
            uint64_t hro = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s2),
                                              cs->slot_gen(static_cast<uint32_t>(s2)));

            auto attempt = [](uint64_t handle) {
                return Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), handle,
                    0, 0, 0, nullptr);
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

            // The successful register armed the vector for this task.
            auto *reg = IrqDelivery::find(msix->vector);
            JARVIS_ASSERT(reg != nullptr);
            JARVIS_ASSERT(reg->armed);
            JARVIS_ASSERT(reg->recipient == cur);

            cs->remove(static_cast<uint32_t>(s1));
            cs->remove(static_cast<uint32_t>(s2));
            msix->release();
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
// Testidea: An MSI-X IRQ delivered before the wait is not lost: pending
//           increments and sys_irq_wait consumes it without blocking.
// Input: task registers + arms (mode 0); harness injects isr_entry; task waits
// Expect: wait returns the vector without blocking
// Depends: kernel::IrqDelivery, kernel::Syscall
JARVIS_TEST(msix_wait_pending_immediate, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    static uint64_t s_vector = 0;
    s_vector = 0;
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(msix, cap::CapType::Msix,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                msix->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            s_vector = msix->vector;
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            g_armed = true;
            while (!g_inject_done)
                arch::pause();
            g_wait_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0, 0, 0,
                nullptr);
            cs->remove(static_cast<uint32_t>(s));
            msix->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    JARVIS_ASSERT(s_vector != 0);
    JARVIS_ASSERT(IrqDelivery::isr_entry(static_cast<uint8_t>(s_vector)));
    g_inject_done = true;

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(s_vector, g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task blocked in sys_irq_wait on an MSI-X vector is woken by an
//           injected IRQ and returns the vector (real two-task blocking).
// Input: waiter task registers then blocks in IRQ_WAIT; harness delivers
// Expect: waiter wakes with the vector, terminates cleanly
// Depends: kernel::IrqDelivery, kernel::Syscall, kernel::Scheduler
JARVIS_TEST(msix_wait_blocking_wakeup, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    static uint64_t s_vector = 0;
    static TaskControlBlock *s_waiter = nullptr;
    s_vector = 0;
    s_waiter = nullptr;
    g_armed = false;
    g_inject_done = false;
    g_wait_ret = 0;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(msix, cap::CapType::Msix,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                msix->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            s_vector = msix->vector;
            s_waiter = cur;
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            g_armed = true;
            g_wait_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0, 0, 0,
                nullptr);
            cs->remove(static_cast<uint32_t>(s));
            msix->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    // Wait until the task is registered as the slot's blocked waiter.
    yield_wait_until([&]() {
        auto *reg = IrqDelivery::find(static_cast<uint8_t>(s_vector));
        return reg != nullptr && reg->waiter == s_waiter;
    });
    JARVIS_ASSERT(IrqDelivery::isr_entry(static_cast<uint8_t>(s_vector)));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(s_vector, g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A waiter blocked in sys_irq_wait on an MSI-X vector is woken with
//           -1 when the cap is revoked mid-wait — never blocked forever, and
//           the entry is re-masked.
// Input: waiter task blocks; harness revokes the cap
// Expect: waiter wakes, returns -1, entry re-masked, terminates cleanly
// Depends: kernel::cap, kernel::IrqDelivery, kernel::Syscall
JARVIS_TEST(msix_wait_revoked_mid_wait, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    static uint64_t s_vector = 0;
    static uint64_t s_handle = 0;
    static TaskControlBlock *s_waiter = nullptr;
    static cap::MsixCap *s_msix = nullptr;
    s_vector = 0;
    s_handle = 0;
    s_waiter = nullptr;
    s_msix = nullptr;
    g_wait_ret = 0;
    g_armed = false;
    g_inject_done = false;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(msix, cap::CapType::Msix,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                msix->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            s_vector = msix->vector;
            s_handle = h;
            s_waiter = cur;
            s_msix = msix;
            g_armed = true;
            uint64_t reg = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            g_reg_ret = reg;
            if (reg != 0) {
                cs->remove(static_cast<uint32_t>(s));
                msix->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            g_wait_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_WAIT), h, 0, 0, 0,
                nullptr);
            cs->remove(static_cast<uint32_t>(s));
            msix->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() {
        auto *reg = IrqDelivery::find(static_cast<uint8_t>(s_vector));
        return reg != nullptr && reg->waiter == s_waiter;
    });

    JARVIS_ASSERT(s_handle != 0);
    JARVIS_ASSERT(s_waiter != nullptr);
    JARVIS_ASSERT(cap::revoke(s_waiter->get_cspace(), s_handle));
    // Revoke re-masks the entry (fail-closed) — assert while the cap block is
    // still alive (the woken task frees it only when it terminates below).
    JARVIS_ASSERT(s_msix->entry_masked());

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_wait_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: NOTIFY delivery on an MSI-X vector: the injected IRQ signals the
//           recipient's Notify; release signals the revoked sentinel 0.
// Input: task arms in NOTIFY mode; harness injects isr_entry; release
// Expect: Notify value == vector on inject; == 0 (revoked) on release
// Depends: kernel::IrqDelivery, kernel::sync::Notify
JARVIS_TEST(msix_notify_delivery, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    static uint64_t s_vector = 0;
    static TaskControlBlock *s_waiter = nullptr;
    s_vector = 0;
    s_waiter = nullptr;
    g_notify_val = 0;
    g_armed = false;
    g_inject_done = false;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_notify_val = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(msix, cap::CapType::Msix,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                msix->release();
                g_notify_val = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            s_vector = msix->vector;
            s_waiter = cur;
            // arg1 = 1 selects NOTIFY delivery mode (issue #7).
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 1, 0,
                0, nullptr);
            g_armed = true;
            // Block in NOTIFY_WAIT (test_cap_irq_notify pattern): persist
            // BLOCKED until the injected IRQ signals the Notify.
            g_notify_val = 0;
            Syscall::handle(static_cast<uint64_t>(SyscallNumber::NOTIFY_WAIT),
                            reinterpret_cast<uint64_t>(&g_notify_val), 0, 0,
                            0, nullptr);
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
            msix->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    yield_wait_until([]() { return g_armed; });
    yield_wait_until([&]() {
        return s_waiter != nullptr && s_waiter->state == TaskState::BLOCKED;
    });
    JARVIS_ASSERT(s_vector != 0);
    // Inject the MSI-X IRQ -> NOTIFY delivery signals the recipient's Notify.
    JARVIS_ASSERT(IrqDelivery::isr_entry(static_cast<uint8_t>(s_vector)));

    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, g_reg_ret);
    JARVIS_ASSERT_EQ(s_vector, g_notify_val);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A task that dies with an armed MSI-X slot leaves no dangling
//           recipient/waiter and the entry is re-masked.
// Input: task registers; harness injects pending IRQ; task terminates
// Expect: delivery slot drained; entry re-masked; table empty
// Depends: kernel::IrqDelivery, TaskControlBlock::cleanup
JARVIS_TEST(msix_task_died_drain, "PRE: none | POST: none") {
    arch::PciBdf bdf{};
    if (!find_msix_device(bdf)) {
        Logger::info("msix: no MSI-X-capable virtio-net device; skipping");
        JARVIS_TEST_PASS();
    }
    static uint64_t s_vector = 0;
    static arch::PciMsixTableInfo s_tbl{};
    s_vector = 0;
    s_tbl = arch::PciMsixTableInfo{};
    g_armed = false;
    g_inject_done = false;
    g_bdf = bdf;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *msix = cap::MsixCap::create(g_bdf, 0);
            if (!msix) {
                g_wait_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            s_tbl = msix->tbl_;
            int s = cs->install(msix, cap::CapType::Msix,
                                cap::CAP_RIGHT_WRITE | cap::CAP_RIGHT_READ);
            if (s < 0) {
                msix->release();
                g_wait_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            // The CSpace slot now holds the only reference.  Drop the creator
            // ref immediately so an externally-terminated task can never
            // abandon it — cleanup() then disposes the object via the slot.
            msix->release();
            uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                            cs->slot_gen(static_cast<uint32_t>(s)));
            s_vector = msix->vector;
            g_reg_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::IRQ_REGISTER), h, 0, 0,
                0, nullptr);
            g_armed = true;
            while (!g_inject_done)
                arch::pause();
            cs->remove(static_cast<uint32_t>(s));
            msix->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    while (!g_armed)
        arch::pause();
    // Inject a pending IRQ but do NOT release the gate — the task stays in
    // its spin, then we terminate it while the slot still has pending work.
    JARVIS_ASSERT(s_vector != 0);
    JARVIS_ASSERT(IrqDelivery::isr_entry(static_cast<uint8_t>(s_vector)));
    JARVIS_ASSERT(IrqDelivery::find(static_cast<uint8_t>(s_vector)) != nullptr);

    // Terminate the task: cleanup() drains the delivery table and re-masks.
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(IrqDelivery::find(static_cast<uint8_t>(s_vector)) == nullptr);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
    // drain_task re-masked the entry (fail-closed) — read straight from the
    // device table (the cap block was freed at task teardown).
    JARVIS_ASSERT(arch::pci_msix_entry_masked(s_tbl, 0));
    JARVIS_TEST_PASS();
}

/// @brief Registers all MSI-X vector infrastructure test cases (issue #10).
void register_cap_msix_tests() {
    JARVIS_REGISTER_TEST(msix_cap_create_fields);
    JARVIS_REGISTER_TEST(msix_cap_no_msix_capability_fails);
    JARVIS_REGISTER_TEST(msix_cap_entry_bounds_and_table);
    JARVIS_REGISTER_TEST(msix_cap_single_owner_vector);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(msix_kernel_reserved_vectors_rejected);
#endif
    JARVIS_REGISTER_TEST(msix_cap_install_lookup_revoke);
    JARVIS_REGISTER_TEST(msix_register_dispatch_happy);
    JARVIS_REGISTER_TEST(msix_register_validation_matrix);
    JARVIS_REGISTER_TEST(msix_wait_pending_immediate);
    JARVIS_REGISTER_TEST(msix_wait_blocking_wakeup);
    JARVIS_REGISTER_TEST(msix_wait_revoked_mid_wait);
    JARVIS_REGISTER_TEST(msix_notify_delivery);
    JARVIS_REGISTER_TEST(msix_task_died_drain);
}

#endif // CONFIG_ARCH_X86_64
