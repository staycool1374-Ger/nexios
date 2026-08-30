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

/// @file test_cap_mmio.cpp
/// @brief MMIO caps + fine-grained I/O delegation tests (issue #3):
///        MmioCap lifecycle, capability-gated MMIO mapping, TSS I/O bitmap
///        layout, sys_ioport_grant dispatch + validation, per-task bitmap
///        switch/cleanup.  Ring-3 #GP on denied ports is NOT testable from
///        the ring-0 harness — the tests assert bitmap state, TSS descriptor
///        layout and owner bookkeeping instead.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/arch/hal/gdt.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace {

/// @brief Runs @p entry as a real task, waits for termination and drains
///        zombies (test_cap_syscall pattern).
TaskControlBlock *run_cap_task(void (*entry)(), uint64_t prio = 11,
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

/// @brief True if every byte of @p buf is 0xFF.
bool all_deny(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (buf[i] != 0xFF)
            return false;
    return true;
}

// -- shared result flags for the syscall-dispatch task --
uint64_t g_grant_ret = 0;
uint64_t g_grant_owner_ok = 0;
uint64_t g_grant_port_ok = 0;

/// @brief Installs an IO-type MmioCap [0x3F8, 8) and dispatches
///        SYS_IOPORT_GRANT for the covered range.
void ioport_grant_happy_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    auto *mmio = cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
    if (!mmio) {
        g_grant_ret = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    int s = cs->install(mmio, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
    if (s < 0) {
        mmio->release();
        g_grant_ret = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));
    g_grant_ret = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOPORT_GRANT), h, 0x3F8, 8, 0,
        nullptr);
    g_grant_owner_ok = (arch::iopb_loaded_owner() == cur) ? 1 : 0;
    g_grant_port_ok =
        (arch::iopb_port_allowed(*cur, 0x3F8) &&
         !arch::iopb_port_allowed(*cur, 0x3F7))
            ? 1
            : 0;
    cs->remove(static_cast<uint32_t>(s));
    mmio->release();
    Scheduler::terminate(*cur, 0);
}

/// @brief Dispatches SYS_IOPORT_GRANT for a failing input; returns the result.
/// @param cap_handle Handle (or 0 when @p install is false).
/// @param start First port to grant.
/// @param count Number of ports.
/// @return The syscall return value.
uint64_t ioport_grant_attempt(uint64_t cap_handle, uint64_t start,
                              uint64_t count) {
    return Syscall::handle(static_cast<uint64_t>(SyscallNumber::IOPORT_GRANT),
                           cap_handle, start, count, 0, nullptr);
}

} // namespace

// Runmode: kernel
// Testidea: The x86_64 TSS I/O permission bitmap layout is default-deny:
//           iopb offset == sizeof(TSS), descriptor limit ==
//           sizeof(TSSBlock)-1, terminator == 0xFF and the loaded bitmap is
//           all-1s.  x86_64 only — the TSS/IOPB mechanism does not exist on
//           other architectures.
// Input: none (boot-time GDT::init state)
// Expect: layout invariants hold
// Depends: kernel::arch::GDT
#if defined(CONFIG_ARCH_X86_64)
JARVIS_TEST(tss_iopb_layout_valid, "PRE: none | POST: none") {
    uint16_t off = arch::GDT::tss_iopb_offset();
    uint16_t limit = arch::GDT::tss_descriptor_limit();
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(sizeof(arch::TSS)), off);
    JARVIS_ASSERT_EQ(static_cast<uint16_t>(sizeof(arch::TSSBlock) - 1), limit);
    JARVIS_ASSERT_EQ(0xFFU, static_cast<unsigned>(arch::GDT::iopb_terminator()));
    JARVIS_ASSERT(all_deny(arch::GDT::iopb_bitmap(), 8192));
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: An MEMORY_32 MmioCap wraps a BAR range with correct fields and
//           zero ResourceTracker delta after release.
// Input: create(0x1000000, 0x1000, MEMORY_32)
// Expect: fields match; cap_objects returns to baseline after release
// Depends: kernel::cap::MmioCap
JARVIS_TEST(mmio_cap_create_memory_bar_fields, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    auto *mmio = cap::MmioCap::create(0x1000000, 0x1000,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    JARVIS_ASSERT_EQ(0x1000000ULL, mmio->phys);
    JARVIS_ASSERT_EQ(0x1000ULL, mmio->size);
    JARVIS_ASSERT(mmio->bar_type == arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio->is_pool_backed());

    mmio->release(); // creator ref -> dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MmioCap range validation fails closed: IO range must fit the
//           64 KiB port space; MEMORY ranges must be page-aligned; size 0
//           rejected.
// Input: valid IO cap; size-0; IO overflow; unaligned MEMORY base
// Expect: only the valid one succeeds
// Depends: kernel::cap::MmioCap
JARVIS_TEST(mmio_cap_create_io_bar_bounds, "PRE: none | POST: none") {
    auto *io = cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
    JARVIS_ASSERT(io != nullptr);
    io->release();

    JARVIS_ASSERT(cap::MmioCap::create(0x1000, 0, arch::PciBarType::IO) ==
                  nullptr);
    // phys+size overflow the port space.
    JARVIS_ASSERT(cap::MmioCap::create(0xFFF8, 0x100, arch::PciBarType::IO) ==
                  nullptr);
    // Memory BAR base must be page-aligned.
    JARVIS_ASSERT(cap::MmioCap::create(0x1234, 0x1000,
                                       arch::PciBarType::MEMORY_32) == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: An installed MmioCap is found by lookup with the right type and
//           rights, rejected with wrong type / missing rights / stale gen,
//           and revoked caps refuse acquire.
// Input: install Mmio cap (WRITE); lookup variants; revoke
// Expect: lookup honors type/rights/gen; revoke invalidates
// Depends: kernel::cap, kernel::TaskControlBlock
JARVIS_TEST(mmio_cap_install_lookup_revoke, "PRE: none | POST: none") {
    // The harness root CNode is a persistent per-task object; create it before
    // the baseline capture so it is not counted as a test leak.
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    cur->ensure_cspace();

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    cap::CNode *cs = cur->get_cspace();

    auto *mmio = cap::MmioCap::create(0x1000000, 0x1000,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    int s = cs->install(mmio, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));

    // Correct type + WRITE -> pinned target.
    KernelObject *obj =
        cap::lookup(cs, h, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(obj != nullptr);
    JARVIS_ASSERT(obj == static_cast<KernelObject *>(mmio));
    obj->release();

    // Wrong type.
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Frame,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    // Missing rights.
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Mmio,
                              cap::CAP_RIGHT_GRANT) == nullptr);
    // Stale generation.
    uint64_t stale =
        cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                           cs->slot_gen(static_cast<uint32_t>(s)) + 1);
    JARVIS_ASSERT(cap::lookup(cs, stale, cap::CapType::Mmio,
                              cap::CAP_RIGHT_WRITE) == nullptr);

    // Revoke invalidates.
    JARVIS_ASSERT(cap::revoke(cs, h));
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Mmio,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(mmio->revoked());

    cs->remove(static_cast<uint32_t>(s));
    mmio->release(); // dispose

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A memory-BAR MmioCap maps into a scratch PML4 page-by-page and
//           unmaps cleanly; revoked and IO-type caps refuse mapping.
// Input: one PMM page wrapped in an MmioCap; clone a PML4; map/unmap
// Expect: virt_to_phys resolves; unmap clears; revoked/IO refusals
// Depends: kernel::cap::MmioCap, kernel::memory::VMM
JARVIS_TEST(mmio_map_memory_bar_roundtrip, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    auto *mmio = cap::MmioCap::create(phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);

    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    constexpr uint64_t VA = 0x400000ULL;

    JARVIS_ASSERT(VMM::map_mmio_from_cap(mmio, VA, false, pml4));
    JARVIS_ASSERT_EQ(phys, VMM::virt_to_phys_in_pml4(VA, pml4));
    VMM::unmap_mmio_from_cap(mmio, VA, pml4);
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys_in_pml4(VA, pml4));

    // IO-type cap refuses MMIO mapping.
    auto *io = cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
    JARVIS_ASSERT(io != nullptr);
    JARVIS_ASSERT(!VMM::map_mmio_from_cap(io, VA, false, pml4));

    // Revoked cap refuses mapping.
    mmio->revoke();
    JARVIS_ASSERT(!VMM::map_mmio_from_cap(mmio, VA, false, pml4));

    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    io->release();
    mmio->release();
    PMM::free_page(phys);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_IOPORT_GRANT over a covered IO-MmioCap range succeeds, makes
//           the task the loaded TSS bitmap owner and clears exactly the
//           granted port bits.
// Input: real task, IO cap [0x3F8,8), dispatch IOPORT_GRANT(0x3F8,8)
// Expect: ret 0; owner == task; port 0x3F8 allowed, 0x3F7 denied
// Depends: kernel::Syscall, kernel::arch::iopb
JARVIS_TEST(ioport_grant_dispatch_happy, "PRE: none | POST: none") {
    g_grant_ret = 0;
    g_grant_owner_ok = 0;
    g_grant_port_ok = 0;

    // User-mode fixture task so the loaded-owner path (iopb_switch_to) applies.
    auto *t = TaskControlBlock::create(ioport_grant_happy_entry, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    t->is_user_ = true;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(0ULL, g_grant_ret);
    JARVIS_ASSERT_EQ(1ULL, g_grant_owner_ok);
    JARVIS_ASSERT_EQ(1ULL, g_grant_port_ok);

    // Task cleanup() released the slot: owner null, TSS default-deny again.
    JARVIS_ASSERT(arch::iopb_loaded_owner() == nullptr);
    JARVIS_ASSERT(all_deny(arch::GDT::iopb_bitmap(), 8192));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_IOPORT_GRANT rejects wrong type, missing WRITE right, ranges
//           outside the cap, count==0, port-space overflow and stale-gen
//           handles — all fail closed with no slot claimed.
// Input: real task installing an IO cap + a WRITE-only-capped variant
// Expect: every attempt returns -1; iopb_slot_ stays NONE
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(ioport_grant_validation_matrix, "PRE: none | POST: none") {
    auto *t = run_cap_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();

        auto *mmio = cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
        auto *ro =
            cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
        if (!mmio || !ro) {
            if (mmio)
                mmio->release();
            if (ro)
                ro->release();
            Scheduler::terminate(*cur, 0);
            return;
        }
        int s1 = cs->install(mmio, cap::CapType::Mmio, cap::CAP_RIGHT_WRITE);
        int s2 = cs->install(ro, cap::CapType::Mmio, cap::CAP_RIGHT_READ);
        if (s1 < 0 || s2 < 0) {
            mmio->release();
            ro->release();
            Scheduler::terminate(*cur, 0);
            return;
        }
        uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s1),
                                        cs->slot_gen(static_cast<uint32_t>(s1)));
        uint64_t hro = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s2),
                                          cs->slot_gen(static_cast<uint32_t>(s2)));

        // Missing WRITE right.
        uint64_t r1 = ioport_grant_attempt(hro, 0x3F8, 8);
        // Range entirely below the cap.
        uint64_t r2 = ioport_grant_attempt(h, 0x300, 4);
        // Range extends past the cap.
        uint64_t r3 = ioport_grant_attempt(h, 0x3F8, 16);
        // count == 0.
        uint64_t r4 = ioport_grant_attempt(h, 0x3F8, 0);
        // Port-space overflow.
        uint64_t r5 = ioport_grant_attempt(h, 0x3F8, 65536);
        // Stale generation.
        uint64_t stale =
            cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s1),
                               cs->slot_gen(static_cast<uint32_t>(s1)) + 1);
        uint64_t r6 = ioport_grant_attempt(stale, 0x3F8, 8);

        uint64_t bad = static_cast<uint64_t>(-1);
        JARVIS_ASSERT_EQ(bad, r1);
        JARVIS_ASSERT_EQ(bad, r2);
        JARVIS_ASSERT_EQ(bad, r3);
        JARVIS_ASSERT_EQ(bad, r4);
        JARVIS_ASSERT_EQ(bad, r5);
        JARVIS_ASSERT_EQ(bad, r6);
        JARVIS_ASSERT(cur->iopb_slot_ == TaskControlBlock::IOPB_SLOT_NONE);
        JARVIS_ASSERT(!arch::iopb_port_allowed(*cur, 0x3F8));

        cs->remove(static_cast<uint32_t>(s1));
        cs->remove(static_cast<uint32_t>(s2));
        mmio->release();
        ro->release();
        Scheduler::terminate(*cur, 0);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: iopb_switch_to applies the owning task's bitmap on switch-in,
//           re-masks to default-deny on a non-granted user task, restores on
//           switch-back and is a no-op for the already-loaded owner.
// Input: two fixture tasks (is_user_=true, never dispatched)
// Expect: owner/bitmap transitions as described
// Depends: kernel::arch::iopb
JARVIS_TEST(ioport_switch_applies_and_restores, "PRE: none | POST: none") {
    TaskPtr a = create_test_task();
    TaskPtr b = create_test_task();
    JARVIS_ASSERT(a.get() != nullptr);
    JARVIS_ASSERT(b.get() != nullptr);
    a->is_user_ = true;
    b->is_user_ = true;

    JARVIS_ASSERT(arch::iopb_claim(*a));
    JARVIS_ASSERT(arch::iopb_grant_range(*a, 0x60, 4));

    arch::iopb_switch_to(*a);
    JARVIS_ASSERT(arch::iopb_loaded_owner() == a.get());
    JARVIS_ASSERT(arch::iopb_port_allowed(*a, 0x60));
    JARVIS_ASSERT(!arch::iopb_port_allowed(*a, 0x5F));

    // Non-granted user task: default-deny, owner cleared.
    arch::iopb_switch_to(*b);
    JARVIS_ASSERT(arch::iopb_loaded_owner() == nullptr);
    JARVIS_ASSERT(all_deny(arch::GDT::iopb_bitmap(), 8192));
    JARVIS_ASSERT(!arch::iopb_port_allowed(*b, 0x60));

    // Switch back restores A's grants.
    arch::iopb_switch_to(*a);
    JARVIS_ASSERT(arch::iopb_loaded_owner() == a.get());
    JARVIS_ASSERT(arch::iopb_port_allowed(*a, 0x60));

    // Repeated switch to the same owner is a no-op (owner unchanged).
    arch::iopb_switch_to(*a);
    JARVIS_ASSERT(arch::iopb_loaded_owner() == a.get());

    arch::iopb_release(*a); // A held the loaded slot
    arch::iopb_release(*b); // no-op
    JARVIS_ASSERT(arch::iopb_loaded_owner() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Task cleanup releases its IOPB slot and re-masks the TSS even
//           when the task is the loaded owner (no dangling permissive state).
// Input: fixture task claims + becomes loaded owner, then is destroyed
// Expect: owner null + all-1s after TaskPtr teardown
// Depends: kernel::arch::iopb, TaskControlBlock::cleanup
JARVIS_TEST(ioport_task_cleanup_releases_slot_and_remasks, "PRE: none | POST: none") {
    {
        TaskPtr c = create_test_task();
        JARVIS_ASSERT(c.get() != nullptr);
        c->is_user_ = true;
        JARVIS_ASSERT(arch::iopb_claim(*c));
        arch::iopb_switch_to(*c);
        JARVIS_ASSERT(arch::iopb_loaded_owner() == c.get());
        // TaskPtr dtor: remove_task + cleanup -> iopb_release.
    }
    JARVIS_ASSERT(arch::iopb_loaded_owner() == nullptr);
    JARVIS_ASSERT(all_deny(arch::GDT::iopb_bitmap(), 8192));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The IOPB pool is bounded by CONFIG_IOPB_MAX_TASKS; exhausting it
//           fails closed (claim returns false) and releasing restores.
// Input: claim every pool slot, then one more
// Expect: the overflow claim fails; after teardown the pool is free
// Depends: kernel::arch::iopb, CONFIG_IOPB_MAX_TASKS
JARVIS_TEST(ioport_pool_exhaustion_fails_closed, "PRE: none | POST: none") {
    enum { MAX = CONFIG_IOPB_MAX_TASKS };
    TaskPtr tasks[MAX];
    for (int i = 0; i < MAX; ++i) {
        tasks[i] = create_test_task();
        JARVIS_ASSERT(tasks[i].get() != nullptr);
        tasks[i]->is_user_ = true;
        JARVIS_ASSERT(arch::iopb_claim(*tasks[i]));
    }
    TaskPtr extra = create_test_task();
    JARVIS_ASSERT(extra.get() != nullptr);
    JARVIS_ASSERT(!arch::iopb_claim(*extra)); // pool exhausted

    // Teardown releases every slot (TaskPtr dtor -> cleanup -> iopb_release).
    extra.reset();
    for (int i = 0; i < MAX; ++i)
        tasks[i].reset();
    JARVIS_ASSERT(arch::iopb_loaded_owner() == nullptr);
    JARVIS_TEST_PASS();
}

/// @brief Registers all MMIO-cap / I/O-delegation test cases (issue #3).
void register_cap_mmio_tests() {
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(tss_iopb_layout_valid);
#endif
    JARVIS_REGISTER_TEST(mmio_cap_create_memory_bar_fields);
    JARVIS_REGISTER_TEST(mmio_cap_create_io_bar_bounds);
    JARVIS_REGISTER_TEST(mmio_cap_install_lookup_revoke);
    JARVIS_REGISTER_TEST(mmio_map_memory_bar_roundtrip);
    JARVIS_REGISTER_TEST(ioport_grant_dispatch_happy);
    JARVIS_REGISTER_TEST(ioport_grant_validation_matrix);
    JARVIS_REGISTER_TEST(ioport_switch_applies_and_restores);
    JARVIS_REGISTER_TEST(ioport_task_cleanup_releases_slot_and_remasks);
    JARVIS_REGISTER_TEST(ioport_pool_exhaustion_fails_closed);
}