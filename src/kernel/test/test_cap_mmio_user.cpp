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

/// @file test_cap_mmio_user.cpp
/// @brief User-space MMIO page-frame mapping tests (issue #8): the
///        SYS_MMIO_MAP/SYS_MMIO_UNMAP syscall surface, the MmioUserMap
///        registry (fixed user VA window), user-PML4 mapping resolution,
///        revoke-invalidate, cleanup drain and registry exhaustion.  Real
///        device BARs are NOT accessed — the tests wrap a PMM page in an
///        MmioCap and drive the registry/syscalls deterministically.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

using namespace kernel;

namespace {

/// @brief Physical frame backing the "device BAR".  A USER PML4 page — the
///        user-map path ENSUREs a user page is mapped user-accessible (a
///        kernel page would trip the is_user_page guard); the registry maps it
///        without ever touching the contents.
uint64_t g_dev_phys = 0;

/// @brief Creates a user task (real user PML4 with the yield stub) that the
///        harness drives directly (never dispatched).  Freed via SimpleTaskPtr.
SimpleTaskPtr make_user_fixture() {
    return SimpleTaskPtr(
        TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
}

} // namespace

// Runmode: kernel
// Testidea: SYS_MMIO_MAP over a MEMORY-type MmioCap returns a user VA in the
//           fixed window; the mapping resolves in the caller's user PML4.
// Input: user fixture task installs a MEMORY MmioCap, dispatches MMIO_MAP
// Expect: ret != -1; ret in [BASE, BASE + MAPS*REGION); PTE resolves to phys
// Depends: kernel::Syscall, kernel::cap::MmioUserMap
JARVIS_TEST(mmio_user_map_dispatch_returns_user_va, "PRE: none | POST: none") {
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);

    SimpleTaskPtr t = make_user_fixture();
    JARVIS_ASSERT(t.get() != nullptr);
    JARVIS_ASSERT(t->page_table_ != 0);

    auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);

    uint64_t va = cap::MmioUserMap::map(*t, *mmio);
    JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(va >= CONFIG_USER_MMIO_VA_BASE);
    JARVIS_ASSERT(va < CONFIG_USER_MMIO_VA_BASE +
                          static_cast<uint64_t>(CONFIG_CAP_MAX_MMIO_MAPS) *
                              CONFIG_USER_MMIO_REGION_SIZE);
    JARVIS_ASSERT(cap::MmioUserMap::is_owner(*t, va));
    JARVIS_ASSERT_EQ(static_cast<size_t>(1), cap::MmioUserMap::live_count());

    // The mapping resolves to the device phys in the task's user PML4.
    JARVIS_ASSERT_EQ(g_dev_phys, VMM::virt_to_phys_in_pml4(va, t->page_table_));

    JARVIS_ASSERT(cap::MmioUserMap::unmap(*t, va));
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys_in_pml4(va, t->page_table_));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());

    mmio->release();
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The syscall handler dispatches SYS_MMIO_MAP/UNMAP over a live
//           installed MmioCap and returns the user VA.
// Input: real task installs a MEMORY cap + dispatches MMIO_MAP, stores va
// Expect: ret is a valid window VA; sys_mmio_unmap(va) returns 0
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(mmio_user_syscall_dispatch, "PRE: none | POST: none") {
    static uint64_t g_map_ret = 0;
    static uint64_t g_unmap_ret = 0;
    g_map_ret = 0;
    g_unmap_ret = 0;
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                              arch::PciBarType::MEMORY_32);
            if (!mmio) {
                g_map_ret = 99;
                Scheduler::terminate(*cur, 0);
                return;
            }
            int s = cs->install(mmio, cap::CapType::Mmio,
                                cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                mmio->release();
                g_map_ret = 98;
                Scheduler::terminate(*cur, 0);
                return;
            }
            uint64_t h = cap::encode_handle(
                cs->cspace_id, static_cast<uint32_t>(s),
                cs->slot_gen(static_cast<uint32_t>(s)));
            g_map_ret = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::MMIO_MAP), h, 0, 0, 0,
                nullptr);
            if (g_map_ret != static_cast<uint64_t>(-1)) {
                g_unmap_ret = Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::MMIO_UNMAP),
                    g_map_ret, 0, 0, 0, nullptr);
            }
            cs->remove(static_cast<uint32_t>(s));
            mmio->release();
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    // Give the task a real user PML4 (kernel half cloned) so the MMIO map
    // registry can map into current_task()->page_table_.  Ring 0 execution is
    // unaffected (kernel half is copied); cleanup frees the clone.
    t->page_table_ = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(t->page_table_ != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(g_map_ret != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(g_unmap_ret == 0);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MMIO map refuses revoked caps and IO-type caps; the registry
//           stays empty (fail closed).
// Input: revoked MEMORY cap + IO cap mapped into a user fixture
// Expect: both map() return -1; live_count 0
// Depends: kernel::cap::MmioCap, kernel::cap::MmioUserMap
JARVIS_TEST(mmio_user_map_rejects_io_and_revoked, "PRE: none | POST: none") {
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);
    SimpleTaskPtr t = make_user_fixture();
    JARVIS_ASSERT(t.get() != nullptr);

    auto *io = cap::MmioCap::create(0x3F8, 8, arch::PciBarType::IO);
    JARVIS_ASSERT(io != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1),
                     cap::MmioUserMap::map(*t, *io));

    auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    mmio->revoke();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1),
                     cap::MmioUserMap::map(*t, *mmio));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());

    mmio->release();
    io->release();
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The map registry is bounded; exhausting it fails closed and
//           unmap restores capacity.
// Input: fill every slot; one more map
// Expect: the overflow map returns -1; after unmap the slot is reusable
// Depends: kernel::cap::MmioUserMap, CONFIG_CAP_MAX_MMIO_MAPS
JARVIS_TEST(mmio_user_map_pool_exhaustion, "PRE: none | POST: none") {
    enum { MAX = CONFIG_CAP_MAX_MMIO_MAPS };
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);
    SimpleTaskPtr t = make_user_fixture();
    JARVIS_ASSERT(t.get() != nullptr);
    cap::MmioCap *caps[MAX];
    uint64_t vas[MAX];
    for (int i = 0; i < MAX; ++i) {
        // All caps wrap the SAME user phys (the registry keys slots by VA,
        // not by phys; re-mapping one frame at different VAs is valid).
        caps[i] = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                       arch::PciBarType::MEMORY_32);
        JARVIS_ASSERT(caps[i] != nullptr);
        vas[i] = cap::MmioUserMap::map(*t, *caps[i]);
        JARVIS_ASSERT(vas[i] != static_cast<uint64_t>(-1));
        JARVIS_ASSERT(vas[i] != 0);
    }
    // Registry full — the next map fails closed.
    auto *extra = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                       arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(extra != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), cap::MmioUserMap::map(*t, *extra));
    JARVIS_ASSERT_EQ(static_cast<size_t>(MAX), cap::MmioUserMap::live_count());

    // Unmap one slot and verify it is reusable.
    JARVIS_ASSERT(cap::MmioUserMap::unmap(*t, vas[0]));
    JARVIS_ASSERT_EQ(static_cast<size_t>(MAX - 1), cap::MmioUserMap::live_count());
    uint64_t re = cap::MmioUserMap::map(*t, *extra);
    JARVIS_ASSERT(re != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(re, vas[0]); // slot 0 VA reused after release
    JARVIS_ASSERT(cap::MmioUserMap::unmap(*t, re));
    // Unmap every remaining mapping (slots 1..MAX-1 were still live).
    for (int i = 1; i < MAX; ++i)
        JARVIS_ASSERT(cap::MmioUserMap::unmap(*t, vas[i]));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());

    extra->release();
    for (int i = 0; i < MAX; ++i)
        caps[i]->release();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_MMIO_UNMAP with a VA the task does not own returns -1 (a
//           task cannot unmap another task's mapping).
// Input: map on task A; unmap the same VA on task B
// Expect: B's unmap returns -1; A still owns the VA
// Depends: kernel::cap::MmioUserMap
JARVIS_TEST(mmio_user_unmap_wrong_owner, "PRE: none | POST: none") {
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);
    SimpleTaskPtr a = make_user_fixture();
    SimpleTaskPtr b = make_user_fixture();
    JARVIS_ASSERT(a.get() != nullptr);
    JARVIS_ASSERT(b.get() != nullptr);

    auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    uint64_t va = cap::MmioUserMap::map(*a, *mmio);
    JARVIS_ASSERT(va != static_cast<uint64_t>(-1));

    JARVIS_ASSERT(!cap::MmioUserMap::unmap(*b, va));
    JARVIS_ASSERT(cap::MmioUserMap::is_owner(*a, va));
    JARVIS_ASSERT_EQ(static_cast<size_t>(1), cap::MmioUserMap::live_count());

    JARVIS_ASSERT(cap::MmioUserMap::unmap(*a, va));
    mmio->release();
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Revoking the backing cap immediately unmaps the mapping (issue #8
//           revocation closure) and the VA is no longer resolvable.
// Input: map, revoke the cap, then check the PTE
// Expect: live_count 0; PTE unmapped; is_owner false
// Depends: kernel::cap::MmioUserMap, kernel::cap::MmioCap
JARVIS_TEST(mmio_user_revoke_invalidates_mapping, "PRE: none | POST: none") {
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);
    SimpleTaskPtr t = make_user_fixture();
    JARVIS_ASSERT(t.get() != nullptr);

    auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    uint64_t va = cap::MmioUserMap::map(*t, *mmio);
    JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
    JARVIS_ASSERT_EQ(g_dev_phys, VMM::virt_to_phys_in_pml4(va, t->page_table_));

    mmio->revoke(); // must unmap the PTE + free the slot
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    JARVIS_ASSERT(!cap::MmioUserMap::is_owner(*t, va));
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys_in_pml4(va, t->page_table_));

    mmio->release();
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Task teardown drains the task's MMIO mappings (no dangling device
//           PTE in a recycled PML4).
// Input: map into a user fixture, destroy the task
// Expect: live_count 0 after teardown
// Depends: kernel::cap::MmioUserMap, TaskControlBlock::cleanup
JARVIS_TEST(mmio_user_cleanup_drains_maps, "PRE: none | POST: none") {
    uint64_t va = 0;
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);
    {
        SimpleTaskPtr t = make_user_fixture();
        JARVIS_ASSERT(t.get() != nullptr);
        auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                          arch::PciBarType::MEMORY_32);
        JARVIS_ASSERT(mmio != nullptr);
        va = cap::MmioUserMap::map(*t, *mmio);
        JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
        JARVIS_ASSERT_EQ(static_cast<size_t>(1), cap::MmioUserMap::live_count());
        // SimpleTaskPtr dtor: remove_task + cleanup -> MmioUserMap::drain_task.
    }
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    JARVIS_ASSERT(!cap::MmioUserMap::is_owner(*Scheduler::current_task(), va));
    PMM::free_page(g_dev_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A full map → unmap / map → revoke cycle returns the registry and
//           the PML4 to baseline with zero ResourceTracker delta.
// Input: map, verify PTE, unmap; repeat with revoke
// Expect: live_count 0; cap_objects/PMM deltas zero
// Depends: kernel::cap::MmioUserMap, kernel::test::ResourceTracker
JARVIS_TEST(mmio_user_map_unmap_zero_delta, "PRE: none | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    g_dev_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_dev_phys != 0);

    {
        SimpleTaskPtr t = make_user_fixture();
        JARVIS_ASSERT(t.get() != nullptr);
        auto *mmio = cap::MmioCap::create(g_dev_phys, arch::PAGE_SIZE,
                                          arch::PciBarType::MEMORY_32);
        JARVIS_ASSERT(mmio != nullptr);
        uint64_t va = cap::MmioUserMap::map(*t, *mmio);
        JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
        JARVIS_ASSERT(cap::MmioUserMap::unmap(*t, va));
        mmio->release();
    } // fixture destroyed here — its PML4/stack/stub pages are released

    PMM::free_page(g_dev_phys); // release the device frame before the delta
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Fork (deep_copy_user_pages) does NOT inherit a user MMIO mapping:
//           the child's MMIO-window VA stays unmapped (fail-closed — a device
//           page must never be deep-copied into a child, and device phys is
//           outside the kernel direct map so copying it would #PF).
// Input: map a device-like phys (PCI MMIO hole, >= total RAM) into a parent
//        PML4; deep-copy to a child PML4
// Expect: child's window VA unmapped; parent's mapping intact
// Depends: kernel::cap::MmioUserMap, kernel::memory::VMM
JARVIS_TEST(mmio_user_fork_child_not_inherited, "PRE: none | POST: none") {
    // Device-like phys outside the kernel direct map (PCI MMIO hole).  Not a
    // PMM page — never allocated/freed, only referenced as a PTE frame.
    constexpr uint64_t kDevPhys = 0x80000000ULL;
    JARVIS_ASSERT(kDevPhys >= PMM::total_memory());

    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);

    auto *mmio = cap::MmioCap::create(kDevPhys, arch::PAGE_SIZE,
                                      arch::PciBarType::MEMORY_32);
    JARVIS_ASSERT(mmio != nullptr);
    constexpr uint64_t kTestVa = CONFIG_USER_MMIO_VA_BASE;
    JARVIS_ASSERT(VMM::map_mmio_from_cap(mmio, kTestVa, true, parent_pml4));
    JARVIS_ASSERT_EQ(kDevPhys,
                     VMM::virt_to_phys_in_pml4(kTestVa, parent_pml4));

    // Fork: the child must NOT inherit the device mapping (fail-closed).
    JARVIS_ASSERT(VMM::deep_copy_user_pages(parent_pml4, child_pml4));
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys_in_pml4(kTestVa, child_pml4));

    // The parent's mapping is untouched.
    JARVIS_ASSERT_EQ(kDevPhys,
                     VMM::virt_to_phys_in_pml4(kTestVa, parent_pml4));

    VMM::unmap_mmio_from_cap(mmio, kTestVa, parent_pml4);
    mmio->release();
    VMM::free_user_pages(parent_pml4);
    VMM::free_user_pages(child_pml4);
    PMM::free_page(parent_pml4);
    PMM::free_page(child_pml4);
    JARVIS_TEST_PASS();
}

/// @brief Registers all user-space MMIO mapping test cases (issue #8).
void register_cap_mmio_user_tests() {
    JARVIS_REGISTER_TEST(mmio_user_map_dispatch_returns_user_va);
    JARVIS_REGISTER_TEST(mmio_user_syscall_dispatch);
    JARVIS_REGISTER_TEST(mmio_user_map_rejects_io_and_revoked);
    JARVIS_REGISTER_TEST(mmio_user_map_pool_exhaustion);
    JARVIS_REGISTER_TEST(mmio_user_unmap_wrong_owner);
    JARVIS_REGISTER_TEST(mmio_user_revoke_invalidates_mapping);
    JARVIS_REGISTER_TEST(mmio_user_cleanup_drains_maps);
    JARVIS_REGISTER_TEST(mmio_user_map_unmap_zero_delta);
    JARVIS_REGISTER_TEST(mmio_user_fork_child_not_inherited);
}