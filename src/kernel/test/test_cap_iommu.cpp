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

/// @file test_cap_iommu.cpp
/// @brief IOMMU DMA protection tests (issue #4): IoMmuDmaCap lifecycle,
///        capability-gated domain programming (identity-IOVA second-level
///        tables in the VT-d layout), syscall dispatch + validation and
///        bounded-exhaustion fail-closed behavior.  Presence is force-
///        injected per test (no live IOMMU hardware in QEMU default boots)
///        and every test leaves the domain table EMPTY — a stale SL root
///        would dangle after the PMM snapshot rewind.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/iommu.hpp>
#include <kernel/iommu/iommu.hpp>
#include <kernel/iommu/vtd.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
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

using iommu::IoMmuManager;
using iommu::vtd::kSlEntryRead;
using iommu::vtd::kSlEntryWrite;

constexpr uint64_t kBad = static_cast<uint64_t>(-1);

/// @brief Runs @p entry as a real task, waits for termination and drains
///        zombies (test_cap_mmio pattern).
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

/// @brief Walks the 4-level SL path for @p iova through the HHDM mapping and
///        returns the LEAF ENTRY VALUE (0 when a table is missing).
///        PMM pages live below 128 MiB, so L4/L3 indices are always 0 here.
uint64_t sl_leaf_value(uint64_t root_phys, uint64_t iova) {
    auto *l4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + root_phys);
    uint64_t e3 = l4[(iova >> 39) & 0x1FF] & iommu::vtd::kSlEntryAddrMask;
    if (e3 == 0)
        return 0;
    auto *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + e3);
    uint64_t e2 = l3[(iova >> 30) & 0x1FF] & iommu::vtd::kSlEntryAddrMask;
    if (e2 == 0)
        return 0;
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + e2);
    uint64_t e1 = l2[(iova >> 21) & 0x1FF] & iommu::vtd::kSlEntryAddrMask;
    if (e1 == 0)
        return 0;
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + e1);
    return l1[(iova >> 12) & 0x1FF];
}

/// @brief Intermediate (L2) entry value for @p iova (the L1 table address).
uint64_t sl_l2_value(uint64_t root_phys, uint64_t iova) {
    auto *l4 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + root_phys);
    uint64_t e3 = l4[(iova >> 39) & 0x1FF] & iommu::vtd::kSlEntryAddrMask;
    if (e3 == 0)
        return 0;
    auto *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + e3);
    uint64_t e2 = l3[(iova >> 30) & 0x1FF] & iommu::vtd::kSlEntryAddrMask;
    if (e2 == 0)
        return 0;
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + e2);
    return l2[(iova >> 21) & 0x1FF];
}

// -- shared result flags for the syscall-dispatch tasks --
uint64_t g_map_ret = 0;
uint64_t g_unmap_ret = 0;
uint64_t g_mapping_count = 0;
uint64_t g_matrix_ok = 0;

/// @brief Happy-path dispatch: the task creates its own IoMmuDmaCap (bound
///        to its task id), wraps one PMM page in a FrameCap, installs both
///        caps and dispatches SYS_IOMMU_MAP.
void iommu_map_happy_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();

    auto *dma = cap::IoMmuDmaCap::create();
    if (!dma) {
        g_map_ret = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t page = PMM::alloc_page();
    auto *fc = (page != 0) ? cap::FrameCap::create(page, 1, false) : nullptr;
    if (!fc) {
        dma->release();
        g_map_ret = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }

    int s1 = cs->install(dma, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    int s2 = cs->install(fc, cap::CapType::Frame,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s1 < 0 || s2 < 0) {
        dma->release();
        fc->release();
        g_map_ret = 97;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h_dma = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s1),
        cs->slot_gen(static_cast<uint32_t>(s1)));
    uint64_t h_frame = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s2),
        cs->slot_gen(static_cast<uint32_t>(s2)));

    g_map_ret = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_dma, h_frame, 0, 0,
        nullptr);
    g_mapping_count = IoMmuManager::mapping_count(dma->domain_idx_);

    cs->remove(static_cast<uint32_t>(s1));
    cs->remove(static_cast<uint32_t>(s2));
    dma->release(); // dispose destroys the domain
    fc->release();  // dispose frees the page
    Scheduler::terminate(*cur, 0);
}

/// @brief Validation matrix: every failing SYS_IOMMU_MAP variant must return
///        -1 with zero mappings programmed.
void iommu_map_matrix_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    uint64_t ok = 1;

    // Absent IOMMU: graceful degradation.
    IoMmuManager::force_present(false);
    if (Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), 0, 0, 0, 0,
            nullptr) != kBad)
        ok = 0;
    IoMmuManager::force_present(true);

    auto *dma = cap::IoMmuDmaCap::create();
    auto *ro_dma = cap::IoMmuDmaCap::create();
    uint64_t page = PMM::alloc_page();
    auto *fc = (page != 0) ? cap::FrameCap::create(page, 1, false) : nullptr;
    if (!dma || !ro_dma || !fc) {
        if (dma)
            dma->release();
        if (ro_dma)
            ro_dma->release();
        if (fc)
            fc->release();
        g_matrix_ok = 0;
        Scheduler::terminate(*cur, 0);
        return;
    }
    int s1 = cs->install(dma, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    int s2 = cs->install(ro_dma, cap::CapType::IoMmuDma, cap::CAP_RIGHT_READ);
    int s3 = cs->install(fc, cap::CapType::Frame,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s1 < 0 || s2 < 0 || s3 < 0) {
        dma->release();
        ro_dma->release();
        fc->release();
        g_matrix_ok = 0;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h_dma = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s1),
        cs->slot_gen(static_cast<uint32_t>(s1)));
    uint64_t h_ro = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s2),
        cs->slot_gen(static_cast<uint32_t>(s2)));
    uint64_t h_frame = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s3),
        cs->slot_gen(static_cast<uint32_t>(s3)));

    // Bad handle (unoccupied slot).
    uint64_t r1 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), kBad, h_frame, 0, 0,
        nullptr);
    // Wrong type: the FRAME handle is not an IoMmuDmaCap.
    uint64_t r2 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_frame, h_frame, 0,
        0, nullptr);
    // Missing WRITE right on the DMA cap (READ-only install).
    uint64_t r3 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_ro, h_frame, 0, 0,
        nullptr);
    // Stale generation on the DMA handle.
    uint64_t h_stale = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s1),
        cs->slot_gen(static_cast<uint32_t>(s1)) + 1);
    uint64_t r4 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_stale, h_frame, 0,
        0, nullptr);
    // Foreign owner: a domain programmed by another task is refused.
    dma->owner_task_id_ = static_cast<uint32_t>(cur->id) + 1;
    uint64_t r5 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_dma, h_frame, 0, 0,
        nullptr);
    dma->owner_task_id_ = static_cast<uint32_t>(cur->id);
    if (r1 != kBad || r2 != kBad || r3 != kBad || r4 != kBad || r5 != kBad)
        ok = 0;
    // No table mutation anywhere in the matrix.
    if (IoMmuManager::mapping_count(dma->domain_idx_) != 0)
        ok = 0;

    cs->remove(static_cast<uint32_t>(s1));
    cs->remove(static_cast<uint32_t>(s2));
    cs->remove(static_cast<uint32_t>(s3));
    dma->release();
    ro_dma->release();
    fc->release();
    g_matrix_ok = ok;
    Scheduler::terminate(*cur, 0);
}

/// @brief Unmap dispatch: map via syscall, unmap via syscall, then the
///        failing variants (already-unmapped, bad handle).
void iommu_unmap_dispatch_entry() {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();

    auto *dma = cap::IoMmuDmaCap::create();
    uint64_t page = PMM::alloc_page();
    auto *fc = (page != 0) ? cap::FrameCap::create(page, 1, false) : nullptr;
    if (!dma || !fc) {
        if (dma)
            dma->release();
        if (fc)
            fc->release();
        g_unmap_ret = 99;
        Scheduler::terminate(*cur, 0);
        return;
    }
    int s1 = cs->install(dma, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    int s2 = cs->install(fc, cap::CapType::Frame,
                         cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s1 < 0 || s2 < 0) {
        dma->release();
        fc->release();
        g_unmap_ret = 98;
        Scheduler::terminate(*cur, 0);
        return;
    }
    uint64_t h_dma = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s1),
        cs->slot_gen(static_cast<uint32_t>(s1)));
    uint64_t h_frame = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(s2),
        cs->slot_gen(static_cast<uint32_t>(s2)));

    uint64_t r_map = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), h_dma, h_frame, 0, 0,
        nullptr);
    uint64_t r_unmap = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_UNMAP), h_dma, h_frame, 0,
        0, nullptr);
    uint64_t count_after = IoMmuManager::mapping_count(dma->domain_idx_);
    // Unmapping again (no such mapping) must fail closed.
    uint64_t r_again = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_UNMAP), h_dma, h_frame, 0,
        0, nullptr);
    // Bad DMA handle must fail closed.
    uint64_t r_bad = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_UNMAP), kBad, h_frame, 0,
        0, nullptr);

    g_map_ret = r_map;
    g_unmap_ret =
        (r_unmap == 0 && count_after == 0 && r_again == kBad && r_bad == kBad)
            ? 0
            : 1;

    cs->remove(static_cast<uint32_t>(s1));
    cs->remove(static_cast<uint32_t>(s2));
    dma->release();
    fc->release();
    Scheduler::terminate(*cur, 0);
}

} // namespace

// Runmode: kernel
// Testidea: With no IOMMU present (default boot state) probe() is false,
//           IoMmuDmaCap::create() fails gracefully and SYS_IOMMU_MAP returns
//           -1 — kernel DMA is untouched.
// Input: default presence state; create(); syscall dispatch
// Expect: probe()==false, create()==nullptr, syscall -1
// Depends: kernel::iommu::IoMmuManager, kernel::cap::IoMmuDmaCap
JARVIS_TEST(iommu_no_hw_probe_false_create_fails, "PRE: none | POST: none") {
    IoMmuManager::force_present(false);
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();

    JARVIS_ASSERT(!IoMmuManager::probe());
    JARVIS_ASSERT(cap::IoMmuDmaCap::create() == nullptr);

    uint64_t r1 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_MAP), 0, 0, 0, 0, nullptr);
    uint64_t r2 = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::IOMMU_UNMAP), 0, 0, 0, 0,
        nullptr);
    JARVIS_ASSERT_EQ(kBad, r1);
    JARVIS_ASSERT_EQ(kBad, r2);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: With presence force-injected, create/install/lookup works and
//           revoke destroys the domain fail-closed (SL root zeroed, PMM
//           delta zero, wrong type/rights refused by lookup).
// Input: force_present(true); create + install + lookup variants + revoke
// Expect: lifecycle ok; domain gone after revoke; zero resource delta
// Depends: kernel::cap::IoMmuDmaCap, kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_cap_create_install_lookup_revoke, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    IoMmuManager::force_present(false);
    JARVIS_ASSERT(cap::IoMmuDmaCap::create() == nullptr);
    IoMmuManager::force_present(true);

    auto *dma = cap::IoMmuDmaCap::create();
    JARVIS_ASSERT(dma != nullptr);
    JARVIS_ASSERT(dma->domain_idx_ >= 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(cur->id),
                     static_cast<uint64_t>(dma->owner_task_id_));
    JARVIS_ASSERT(IoMmuManager::domain_valid(dma->domain_idx_));
    uint64_t root = IoMmuManager::sl_root(dma->domain_idx_);
    JARVIS_ASSERT(root != 0);

    cap::CNode *cs = cur->get_cspace();
    int s = cs->install(dma, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(s >= 0);
    uint64_t h = cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(s),
                                    cs->slot_gen(static_cast<uint32_t>(s)));

    KernelObject *obj =
        cap::lookup(cs, h, cap::CapType::IoMmuDma, cap::CAP_RIGHT_WRITE);
    JARVIS_ASSERT(obj == static_cast<KernelObject *>(dma));
    obj->release();
    // Wrong type / missing rights refuse.
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::Frame,
                              cap::CAP_RIGHT_WRITE) == nullptr);
    JARVIS_ASSERT(cap::lookup(cs, h, cap::CapType::IoMmuDma,
                              cap::CAP_RIGHT_READ) == nullptr);

    // Revoke destroys the domain BEFORE publishing the revoke.
    JARVIS_ASSERT(cap::revoke(cs, h));
    JARVIS_ASSERT(dma->revoked());
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     IoMmuManager::sl_root(dma->domain_idx_));
    JARVIS_ASSERT(!IoMmuManager::domain_valid(dma->domain_idx_));

    cs->remove(static_cast<uint32_t>(s));
    dma->release(); // dispose: domain already gone — idempotent
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mapping a 2-page FrameCap programs both SL leaf entries with
//           R|W and the frame's physical address (identity IOVA).
// Input: 2 contiguous PMM pages wrapped in a FrameCap; manager map R|W
// Expect: both leaf entries == page_phys | R | W via the HHDM table walk
// Depends: kernel::iommu::IoMmuManager, vtd entry layouts
JARVIS_TEST(iommu_domain_map_programs_sl1_entries, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);

    uint64_t phys = PMM::alloc_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 2, false);
    JARVIS_ASSERT(fc != nullptr);

    uint32_t flags = kSlEntryRead | kSlEntryWrite;
    JARVIS_ASSERT(IoMmuManager::map_frame(idx, *fc, flags));
    JARVIS_ASSERT_EQ(static_cast<size_t>(1), IoMmuManager::mapping_count(idx));

    uint64_t root = IoMmuManager::sl_root(idx);
    JARVIS_ASSERT(root != 0);
    for (size_t p = 0; p < 2; ++p) {
        uint64_t iova = phys + p * arch::PAGE_SIZE;
        uint64_t leaf = sl_leaf_value(root, iova);
        JARVIS_ASSERT_EQ(iova & iommu::vtd::kSlEntryAddrMask,
                         leaf & iommu::vtd::kSlEntryAddrMask);
        JARVIS_ASSERT_EQ(static_cast<uint64_t>(flags),
                         leaf & (kSlEntryRead | kSlEntryWrite));
    }

    JARVIS_ASSERT(IoMmuManager::unmap_frame(idx, *fc));
    IoMmuManager::domain_destroy(idx);
    fc->release(); // dispose frees both pages
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SL entry rights flow from the mapping flags: READ-only mapping
//           sets R and clears W; WRITE-only sets W and clears R.
// Input: two 1-page FrameCaps mapped with R-only and W-only flags
// Expect: leaf entry flag bits match exactly
// Depends: kernel::iommu::IoMmuManager, vtd::kSlEntry*
JARVIS_TEST(iommu_map_rights_flow_from_frame_cap, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);

    uint64_t phys_a = PMM::alloc_page();
    uint64_t phys_b = PMM::alloc_page();
    JARVIS_ASSERT(phys_a != 0 && phys_b != 0);
    auto *fc_a = cap::FrameCap::create(phys_a, 1, false);
    auto *fc_b = cap::FrameCap::create(phys_b, 1, false);
    JARVIS_ASSERT(fc_a != nullptr && fc_b != nullptr);

    JARVIS_ASSERT(IoMmuManager::map_frame(idx, *fc_a, kSlEntryRead));
    JARVIS_ASSERT(IoMmuManager::map_frame(idx, *fc_b, kSlEntryWrite));

    uint64_t root = IoMmuManager::sl_root(idx);
    uint64_t leaf_a = sl_leaf_value(root, phys_a);
    uint64_t leaf_b = sl_leaf_value(root, phys_b);
    JARVIS_ASSERT_EQ(kSlEntryRead, leaf_a & (kSlEntryRead | kSlEntryWrite));
    JARVIS_ASSERT_EQ(kSlEntryWrite, leaf_b & (kSlEntryRead | kSlEntryWrite));

    IoMmuManager::domain_destroy(idx);
    fc_a->release();
    fc_b->release();
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mapping a revoked FrameCap fails closed — no table mutation
//           (and unaligned/empty frames are rejected as well).
// Input: FrameCap revoked before map_frame; unaligned frame
// Expect: map_frame returns false; mapping_count unchanged
// Depends: kernel::iommu::IoMmuManager, kernel::cap::FrameCap
JARVIS_TEST(iommu_map_rejects_revoked_frame, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);

    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, false);
    JARVIS_ASSERT(fc != nullptr);
    fc->revoke();

    JARVIS_ASSERT(!IoMmuManager::map_frame(
        idx, *fc, kSlEntryRead | kSlEntryWrite));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::mapping_count(idx));
    // An unaligned frame is rejected as well (fail-closed hygiene).
    auto *bad = cap::FrameCap::create(phys + 1, 1, false);
    JARVIS_ASSERT(bad != nullptr);
    JARVIS_ASSERT(!IoMmuManager::map_frame(
        idx, *bad, kSlEntryRead | kSlEntryWrite));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::mapping_count(idx));

    IoMmuManager::domain_destroy(idx);
    fc->release();
    bad->release();
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unmapping clears the leaf entry and reclaims the now-empty L1
//           table page (PMM allocation bit drops); the record is freed so
//           the same frame can be mapped again.
// Input: 1-page mapping (creates a full L4->L1 chain); unmap
// Expect: leaf zero; L1 table page is_allocated false; re-map ok
// Depends: kernel::iommu::IoMmuManager, PMM::is_allocated
JARVIS_TEST(iommu_unmap_clears_entries_frees_pages, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);

    uint64_t phys = PMM::alloc_page();
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, false);
    JARVIS_ASSERT(fc != nullptr);
    JARVIS_ASSERT(IoMmuManager::map_frame(
        idx, *fc, kSlEntryRead | kSlEntryWrite));

    uint64_t root = IoMmuManager::sl_root(idx);
    uint64_t l1_phys = sl_l2_value(root, phys) & iommu::vtd::kSlEntryAddrMask;
    JARVIS_ASSERT(l1_phys != 0);
    JARVIS_ASSERT(PMM::is_allocated(l1_phys));

    JARVIS_ASSERT(IoMmuManager::unmap_frame(idx, *fc));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), sl_leaf_value(root, phys));
    JARVIS_ASSERT(!PMM::is_allocated(l1_phys));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::mapping_count(idx));

    // The record was freed: the same frame maps again.
    JARVIS_ASSERT(IoMmuManager::map_frame(
        idx, *fc, kSlEntryRead | kSlEntryWrite));
    JARVIS_ASSERT_EQ(static_cast<size_t>(1), IoMmuManager::mapping_count(idx));

    IoMmuManager::domain_destroy(idx);
    fc->release();
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: domain_destroy frees EVERY page the domain owns (root + all
//           table pages, chain-shape agnostic — the exact accounting is the
//           ResourceTracker delta at the end); other domains are untouched.
// Input: two domains, one with 2 mappings; destroy the loaded one
// Expect: table pages reclaimed (free_memory grows), domain invalid, other
//         domain still valid, zero total delta at teardown
// Depends: kernel::iommu::IoMmuManager
JARVIS_TEST(iommu_domain_destroy_frees_all_pages, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    int16_t other = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0 && other >= 0 && idx != other);

    uint64_t phys_a = PMM::alloc_page();
    uint64_t phys_b = PMM::alloc_page();
    JARVIS_ASSERT(phys_a != 0 && phys_b != 0);
    auto *fc_a = cap::FrameCap::create(phys_a, 1, false);
    auto *fc_b = cap::FrameCap::create(phys_b, 1, false);
    JARVIS_ASSERT(fc_a != nullptr && fc_b != nullptr);
    JARVIS_ASSERT(IoMmuManager::map_frame(idx, *fc_a, kSlEntryRead));
    JARVIS_ASSERT(IoMmuManager::map_frame(idx, *fc_b, kSlEntryWrite));

    // The frames stay held by the FrameCaps; destroy must reclaim at least
    // the domain root + every table page the mappings created.
    uint64_t after_maps = PMM::free_memory();
    IoMmuManager::domain_destroy(idx);
    JARVIS_ASSERT(PMM::free_memory() > after_maps);
    JARVIS_ASSERT(!IoMmuManager::domain_valid(idx));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::mapping_count(idx));
    JARVIS_ASSERT(IoMmuManager::domain_valid(other)); // untouched

    IoMmuManager::domain_destroy(other);
    fc_a->release();
    fc_b->release();
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: attach_device programs the root entry (Present) and the context
//           entry (Present + translate + ASR == domain SL root);
//           clear_attachment zeroes both.
// Input: PciBdf{bus=1,dev=2,fn=0}; attach then clear
// Expect: introspection confirms both entries; zero after clear
// Depends: kernel::iommu::IoMmuManager, arch::PciBdf
JARVIS_TEST(iommu_attach_device_programs_context_entry,
            "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    int16_t idx = IoMmuManager::domain_create(
        static_cast<uint32_t>(cur->id));
    JARVIS_ASSERT(idx >= 0);
    arch::PciBdf bdf{1, 2, 0};
    uint64_t root = IoMmuManager::sl_root(idx);
    JARVIS_ASSERT(root != 0);

    JARVIS_ASSERT(IoMmuManager::attach_device(idx, bdf));
    JARVIS_ASSERT(IoMmuManager::root_entry_present(1));
    JARVIS_ASSERT_EQ(root, IoMmuManager::context_asr(idx, bdf));

    IoMmuManager::clear_attachment(idx, bdf);
    JARVIS_ASSERT(!IoMmuManager::root_entry_present(1));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0),
                     IoMmuManager::context_asr(idx, bdf));

    // Attaching to an invalid domain fails closed.
    JARVIS_ASSERT(!IoMmuManager::attach_device(
        static_cast<int16_t>(CONFIG_IOMMU_MAX_DOMAINS), bdf));
    JARVIS_ASSERT(!IoMmuManager::attach_device(-1, bdf));

    IoMmuManager::domain_destroy(idx);
    IoMmuManager::force_present(false);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_IOMMU_MAP dispatch happy path: a real task holding an
//           IoMmuDmaCap (WRITE) + FrameCap (READ|WRITE) maps the frame and
//           the mapping record is visible.
// Input: real task, both caps installed, dispatch IOMMU_MAP
// Expect: ret 0; mapping_count == 1; no leaks after teardown
// Depends: kernel::Syscall, kernel::cap, kernel::iommu
JARVIS_TEST(sys_iommu_map_dispatch_happy, "PRE: none | POST: none") {
    g_map_ret = 0;
    g_mapping_count = 0;
    IoMmuManager::force_present(true);

    auto *t = run_cap_task(iommu_map_happy_entry);
    JARVIS_ASSERT(t != nullptr);
    IoMmuManager::force_present(false);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_map_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_mapping_count);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_IOMMU_MAP validation matrix: absent IOMMU, bad handle,
//           wrong cap type, missing WRITE right, stale generation and
//           foreign-task owner — all fail closed with -1 and no mutation.
// Input: real task dispatching failing variants
// Expect: every attempt -1; mapping_count unchanged
// Depends: kernel::Syscall, kernel::cap
JARVIS_TEST(sys_iommu_map_validation_matrix, "PRE: none | POST: none") {
    g_matrix_ok = 0;
    IoMmuManager::force_present(true);

    auto *t = run_cap_task(iommu_map_matrix_entry);
    JARVIS_ASSERT(t != nullptr);
    IoMmuManager::force_present(false);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_matrix_ok);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_IOMMU_UNMAP dispatch: happy path clears the mapping;
//           unmapping a non-mapped frame or a bad handle fails with -1.
// Input: real task, mapped frame; dispatch IOMMU_UNMAP variants
// Expect: happy ret 0 + mapping_count 0; invalid -1
// Depends: kernel::Syscall, kernel::cap, kernel::iommu
JARVIS_TEST(sys_iommu_unmap_dispatch, "PRE: none | POST: none") {
    g_map_ret = 0;
    g_unmap_ret = 0;
    IoMmuManager::force_present(true);

    auto *t = run_cap_task(iommu_unmap_dispatch_entry);
    JARVIS_ASSERT(t != nullptr);
    IoMmuManager::force_present(false);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_map_ret);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), g_unmap_ret);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Bounded resources fail closed: the domain table exhausts
//           (domain_create -1), the per-domain mapping table exhausts
//           (map false); full teardown leaves zero deltas.
// Input: create domains to CONFIG_IOMMU_MAX_DOMAINS; map to
//        CONFIG_IOMMU_MAX_MAPPINGS distinct frames
// Expect: overflow attempts fail; teardown clean
// Depends: kernel::iommu, CONFIG_IOMMU_*
JARVIS_TEST(iommu_bounded_exhaustion_fails_closed, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    cur->ensure_cspace();
    IoMmuManager::force_present(true);

    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);

    // Domain bound: the table is CONFIG_IOMMU_MAX_DOMAINS deep.
    int16_t domains[CONFIG_IOMMU_MAX_DOMAINS];
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_DOMAINS);
         ++i) {
        domains[i] = IoMmuManager::domain_create(
            static_cast<uint32_t>(cur->id));
        JARVIS_ASSERT(domains[i] >= 0);
    }
    JARVIS_ASSERT_EQ(static_cast<size_t>(CONFIG_IOMMU_MAX_DOMAINS),
                     IoMmuManager::occupied_domains());
    JARVIS_ASSERT(IoMmuManager::domain_create(
                      static_cast<uint32_t>(cur->id)) < 0);

    // Mapping bound: distinct frames until the record table is full.
    constexpr size_t kMaxMaps =
        static_cast<size_t>(CONFIG_IOMMU_MAX_MAPPINGS);
    uint64_t phys[kMaxMaps + 1];
    cap::FrameCap *fcs[kMaxMaps + 1] = {};
    size_t mapped = 0;
    for (; mapped < kMaxMaps; ++mapped) {
        phys[mapped] = PMM::alloc_page();
        JARVIS_ASSERT(phys[mapped] != 0);
        fcs[mapped] = cap::FrameCap::create(phys[mapped], 1, false);
        JARVIS_ASSERT(fcs[mapped] != nullptr);
        JARVIS_ASSERT(
            IoMmuManager::map_frame(domains[0], *fcs[mapped], kSlEntryRead));
    }
    JARVIS_ASSERT_EQ(kMaxMaps, IoMmuManager::mapping_count(domains[0]));
    // One more mapping must fail closed (record table exhausted).
    phys[kMaxMaps] = PMM::alloc_page();
    JARVIS_ASSERT(phys[kMaxMaps] != 0);
    fcs[kMaxMaps] = cap::FrameCap::create(phys[kMaxMaps], 1, false);
    JARVIS_ASSERT(fcs[kMaxMaps] != nullptr);
    JARVIS_ASSERT(
        !IoMmuManager::map_frame(domains[0], *fcs[kMaxMaps], kSlEntryRead));

    // Teardown: every domain destroyed, every frame released.
    for (size_t i = 0; i < static_cast<size_t>(CONFIG_IOMMU_MAX_DOMAINS); ++i)
        IoMmuManager::domain_destroy(domains[i]);
    for (size_t i = 0; i <= kMaxMaps; ++i) {
        if (fcs[i])
            fcs[i]->release();
    }
    IoMmuManager::force_present(false);

    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.cap_objects, after.cap_objects);
    JARVIS_ASSERT_EQ(before.cap_slots, after.cap_slots);
    JARVIS_ASSERT_EQ(before.pmm_pages_used, after.pmm_pages_used);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), IoMmuManager::occupied_domains());
    JARVIS_TEST_PASS();
}

/// @brief Registers all IOMMU DMA protection test cases (issue #4).
void register_cap_iommu_tests() {
    JARVIS_REGISTER_TEST(iommu_no_hw_probe_false_create_fails);
    JARVIS_REGISTER_TEST(iommu_cap_create_install_lookup_revoke);
    JARVIS_REGISTER_TEST(iommu_domain_map_programs_sl1_entries);
    JARVIS_REGISTER_TEST(iommu_map_rights_flow_from_frame_cap);
    JARVIS_REGISTER_TEST(iommu_map_rejects_revoked_frame);
    JARVIS_REGISTER_TEST(iommu_unmap_clears_entries_frees_pages);
    JARVIS_REGISTER_TEST(iommu_domain_destroy_frees_all_pages);
    JARVIS_REGISTER_TEST(iommu_attach_device_programs_context_entry);
    JARVIS_REGISTER_TEST(sys_iommu_map_dispatch_happy);
    JARVIS_REGISTER_TEST(sys_iommu_map_validation_matrix);
    JARVIS_REGISTER_TEST(sys_iommu_unmap_dispatch);
    JARVIS_REGISTER_TEST(iommu_bounded_exhaustion_fails_closed);
}
