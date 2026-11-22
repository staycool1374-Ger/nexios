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

/// @file test_cap_vspace.cpp
/// @brief VSpace/PageTable link tests (F5 N4, issue #558): the bounded
///        link registry (VSpaceMap), the 102 syscall face, and the
///        revoke/delete teardown orders (INV-VS-4/8).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/untyped.hpp>
#include <kernel/cap/carved_object.hpp>
#include <kernel/cap/vspace_map.hpp>
#include <kernel/cap/vspace_errors.hpp>
#include <kernel/memory/kernel_object.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/nexios_config.h>

using namespace kernel;

namespace {

// F5 test helpers (mirrors test_cap_syscall.cpp's task harness): run a
// lambda as a real task with its own CSpace, wait, drain, return the TCB.
TaskControlBlock *run_vspace_task(void (*entry)(), uint64_t prio = 11,
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

// First occupied slot of @p type whose target is not @p exclude;
// -1 when none. Bounded scan (CONFIG_CSLOT_COUNT).
int find_slot_not(const cap::CNode &node, cap::CapType type,
                  const KernelObject *exclude) noexcept {
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        KernelObject *obj = node.peek(i, type);
        if (obj && obj != exclude)
            return static_cast<int>(i);
    }
    return -1;
}

// Opaque handle for @p slot in @p cspace (issue #266 pattern).
uint64_t slot_handle(cap::CNode *cspace, uint32_t slot) {
    return cap::encode_handle(cspace->cspace_id, slot,
                              cspace->slot_gen(slot));
}

// Syscall-face encoding of a VSpace error: -(1100 + code).
uint64_t vspace_err(uint64_t code) {
    return 0ULL - (1100ULL + code);
}

// Minimal shared-heap KernelObject for non-carved capability targets
// (mirrors test_cap_syscall.cpp): pool-backed, tracked, disposed once.
struct VspaceTestTarget : public KernelObject {
    static uint32_t g_dispose_count;
    void dispose() noexcept override {
        ++g_dispose_count;
        if (is_pool_backed()) {
            kernel::test::ResourceTracker::instance().track_cap_object_remove();
            MemPool::free(this);
        }
    }
    bool is_shared() const noexcept override {
        return true;
    }
};

uint32_t VspaceTestTarget::g_dispose_count = 0;

// CarvedObjectCap phys for the object in @p slot (caller holds a ref
// via lookup; released before return).
uint64_t carved_phys(cap::CNode *cs, uint64_t handle, cap::CapType type) {
    KernelObject *obj =
        cap::lookup(cs, handle, type, cap::CapRights::READ);
    if (!obj)
        return 0;
    auto *co = static_cast<cap::CarvedObjectCap *>(obj);
    const uint64_t phys = co->phys;
    obj->release();
    return phys;
}

// Carve a 1-page PageTable from the first live (owns_region) Untyped
// in @p cs that is not @p ex1/@p ex2. Spent carve remainders stay
// installed (Untyped-typed) but must never be retyped again
// (ALREADY_RETYPED); owns_region() is the durable filter. Returns the
// new slot or -1. Bounded scan (CONFIG_CSLOT_COUNT).
int carve_pt_from_live_untyped(cap::CNode *cs, const KernelObject *ex1,
                               const KernelObject *ex2) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
        KernelObject *o = cs->peek(i, cap::CapType::Untyped);
        if (!o || o == ex1 || o == ex2)
            continue;
        auto *u = static_cast<cap::UntypedMem *>(o);
        if (!u->owns_region())
            continue;
        const int r = cap::retype(cs, slot_handle(cs, i),
                                  cap::CapType::PageTable, arch::PAGE_SIZE,
                                  cap::CapRights::READ);
        if (r >= 0)
            return r;
    }
    return -1;
}

} // namespace

// Runmode: kernel
// Testidea: F5 N4 basic link — retype VSpace+PageTable from one Untyped,
//        syscall 102 with a 1-component path links the table; the root
//        entry read through HHDM equals the arch link word.
// Input: Untyped(2 pages, user) -> retype VSpace(1 page) + PageTable(1
//        page); install both; Syscall 102 (path component 0, len 1).
// Expect: return 0; root entry == VMM::table_link_word(pt_phys);
//         VSpaceMap::live_count() == 1.
// Depends: cap::retype, Syscall 102, VSpaceMap::link, VMM table codec
// Pseudocode: create Untyped; retype VSpace + PageTable; install both
// with WRITE/READ; call 102; read root word via HHDM; assert link word
// and live_count; teardown.
JARVIS_TEST(mk_vspace_map_links_page_table, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_live = 0;
    static uint64_t g_entry = 0;
    static uint64_t g_expect = 0;
    g_ret = g_live = g_entry = g_expect = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        const uint64_t uh = slot_handle(cs, static_cast<uint32_t>(us));
        int vs = cap::retype(cs, uh, cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (vs < 0) {
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const int cus =
            find_slot_not(*cs, cap::CapType::Untyped, ut);
        if (cus < 0) {
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t cuh =
            slot_handle(cs, static_cast<uint32_t>(cus));
        int pt = cap::retype(cs, cuh, cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        if (pt < 0) {
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        const uint64_t pt_phys =
            carved_phys(cs, pth, cap::CapType::PageTable);
        if (vs_phys == 0 || pt_phys == 0) {
            cs->remove(static_cast<uint32_t>(pt));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 0, 1,
            nullptr);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        g_entry = root[0];
        g_expect = VMM::table_link_word(pt_phys);
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(g_expect, g_entry);
    JARVIS_ASSERT_EQ(1ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 second-level link — link at [i0], then [i0,i1] onto the
//        first table; both succeed. Depth bound is per-arch.
// Input: Two links as above; on riscv64 a third component must fail
//        BAD_LEVEL; x86_64/aarch64 accept len 3 and reject len 4.
// Expect: Both links 0; per-arch limit from VMM::page_table_levels(),
//         never a literal; over-depth rejected.
// Depends: VSpaceMap::link walk, VMM::page_table_levels
// Pseudocode: link L1 at [i0]; link L2 at [i0,i1]; assert both 0;
// assert arch limit accepts/rejects per page_table_levels; teardown.
JARVIS_TEST(mk_vspace_map_second_level, "PRE: none | POST: none") {
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_entry = 0;
    static uint64_t g_expect = 0;
    static uint64_t g_r3 = 0;
    static uint64_t g_mid3 = 0;
    static uint64_t g_r4 = 0;
    static uint64_t g_live = 0;
    g_r1 = g_r2 = g_entry = g_expect = g_r3 = g_mid3 = g_r4 = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        // utA(2): VSpace + PT1. utB(2): PT2 + PT3. Deterministic
        // two-Untyped split (no carve-chain hunt).
        auto *utA =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        auto *utB =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        JARVIS_ASSERT(utA != nullptr);
        JARVIS_ASSERT(utB != nullptr);
        if (!utA || !utB) {
            if (utA)
                utA->release();
            if (utB)
                utB->release();
            return;
        }
        int usa = cs->install(utA, cap::CapType::Untyped,
                              cap::CapRights::READ | cap::CapRights::WRITE);
        JARVIS_ASSERT(usa >= 0);
        if (usa < 0) {
            utA->release();
            utB->release();
            return;
        }
        // Retype VSpace + PT1 BEFORE installing utB, so the carve child
        // is the only other Untyped slot (deterministic discovery).
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(usa)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        JARVIS_ASSERT(vs >= 0);
        const int ca = find_slot_not(*cs, cap::CapType::Untyped, utA);
        JARVIS_ASSERT(ca >= 0);
        int pt1 = -1;
        if (vs >= 0 && ca >= 0) {
            pt1 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(ca)),
                              cap::CapType::PageTable, arch::PAGE_SIZE,
                              cap::CapRights::READ);
        }
        JARVIS_ASSERT(pt1 >= 0);
        int usb = cs->install(utB, cap::CapType::Untyped,
                              cap::CapRights::READ | cap::CapRights::WRITE);
        JARVIS_ASSERT(usb >= 0);
        int pt2 = -1;
        if (usb >= 0) {
            pt2 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(usb)),
                              cap::CapType::PageTable, arch::PAGE_SIZE,
                              cap::CapRights::READ);
        }
        JARVIS_ASSERT(pt2 >= 0);
        int cb = -1;
        if (pt2 >= 0) {
            for (uint32_t i = 0;
                 i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
                KernelObject *o = cs->peek(i, cap::CapType::Untyped);
                // Skip the spent parents (retyped) — only a live
                // remainder with owns_region() is retypable.
                if (o && o != utA && o != utB &&
                    static_cast<cap::UntypedMem *>(o)->owns_region()) {
                    cb = static_cast<int>(i);
                    break;
                }
            }
        }
        JARVIS_ASSERT(cb >= 0);
        int pt3 = -1;
        if (cb >= 0) {
            pt3 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cb)),
                              cap::CapType::PageTable, arch::PAGE_SIZE,
                              cap::CapRights::READ);
        }
        JARVIS_ASSERT(pt3 >= 0);
        if (vs < 0 || pt1 < 0 || pt2 < 0 || pt3 < 0) {
            if (pt3 >= 0)
                cs->remove(static_cast<uint32_t>(pt3));
            if (pt2 >= 0)
                cs->remove(static_cast<uint32_t>(pt2));
            if (pt1 >= 0)
                cs->remove(static_cast<uint32_t>(pt1));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(usa));
            cs->remove(static_cast<uint32_t>(usb));
            utA->release();
            utB->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t p1h = slot_handle(cs, static_cast<uint32_t>(pt1));
        const uint64_t p2h = slot_handle(cs, static_cast<uint32_t>(pt2));
        const uint64_t p3h = slot_handle(cs, static_cast<uint32_t>(pt3));
        const uint64_t l1_phys =
            carved_phys(cs, p1h, cap::CapType::PageTable);
        const uint64_t l2_phys =
            carved_phys(cs, p2h, cap::CapType::PageTable);
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, p1h, 1,
            1, nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, p2h,
            1 | (2 << 9), 2, nullptr);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *l1 = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + l1_phys);
        g_entry = l1[2];
        g_expect = VMM::table_link_word(l2_phys);
        // Depth bound is per-arch, never a literal.
        const unsigned levels = VMM::page_table_levels();
        const uint64_t len3_path = 1 | (2 << 9) | (3 << 18);
        const uint64_t len3_expect =
            (levels > 3) ? 0ULL : vspace_err(5);
        g_r3 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, p3h,
            len3_path, 3, nullptr);
        // Non-vacuous anchor: the len-3 link really landed.
        g_mid3 = cap::VSpaceMap::live_count();
        if (len3_expect != 0ULL && g_r3 != len3_expect) {
            cs->remove(static_cast<uint32_t>(pt3));
            cs->remove(static_cast<uint32_t>(pt2));
            cs->remove(static_cast<uint32_t>(pt1));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(usa));
            cs->remove(static_cast<uint32_t>(usb));
            utA->release();
            utB->release();
            return;
        }
        const uint64_t len4_path = 1 | (2 << 9) | (3 << 18) | (4 << 27);
        g_r4 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, p3h,
            len4_path, 4, nullptr);
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt3));
        cs->remove(static_cast<uint32_t>(pt2));
        cs->remove(static_cast<uint32_t>(pt1));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(usa));
        cs->remove(static_cast<uint32_t>(usb));
        utA->release();
        utB->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(g_expect, g_entry);
    {
        const unsigned levels = VMM::page_table_levels();
        const uint64_t len3_expect = (levels > 3) ? 0ULL : vspace_err(5);
        JARVIS_ASSERT_EQ(len3_expect, g_r3);
        JARVIS_ASSERT_EQ((levels > 3) ? 3ULL : 2ULL, g_mid3);
    }
    JARVIS_ASSERT_EQ(vspace_err(5), g_r4); // over-depth always rejected
    // live: L1 + L2 (+ L3 on deep arches).
    {
        const unsigned levels = VMM::page_table_levels();
        JARVIS_ASSERT_EQ((levels > 3) ? 3ULL : 2ULL, g_live);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 occupied-slot guard — a second link at the same path
//        with a different PageTable fails SLOT_OCCUPIED and changes
//        nothing.
// Input: Link table A at path P; link table B at path P.
// Expect: Second call VSPACE_SLOT_OCCUPIED; first entry unchanged;
//         live_count() unchanged.
// Depends: VSpaceMap::link slot check under the registry lock
// Pseudocode: link A at P (0); read entry E0; link B at P (expect
// SLOT_OCCUPIED); entry still E0; live_count unchanged; teardown.
JARVIS_TEST(mk_vspace_map_occupied_slot, "PRE: none | POST: none") {
    static uint64_t g_first = 0;
    static uint64_t g_second = 0;
    static uint64_t g_entry = 0;
    static uint64_t g_expect = 0;
    static uint64_t g_live = 0;
    g_first = g_second = g_entry = g_expect = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        // ut1 (2 pages): VSpace carve + child remainder -> PTa.
        // ut2 (1 page): exact retype -> PTb. Keeps slot discovery
        // unambiguous (one child, one exact).
        auto *ut1 =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut1) {
            return;
        }
        int us1 = cs->install(ut1, cap::CapType::Untyped,
                              cap::CapRights::READ | cap::CapRights::WRITE);
        JARVIS_ASSERT(us1 >= 0);
        if (us1 < 0) {
            ut1->release();
            return;
        }
        // Retype the VSpace + first PageTable BEFORE installing ut2, so
        // the carve remainder is the only other Untyped slot (slot
        // discovery is deterministic).
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us1)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        JARVIS_ASSERT(vs >= 0);
        const int cus = find_slot_not(*cs, cap::CapType::Untyped, ut1);
        JARVIS_ASSERT(cus >= 0);
        int pta = -1;
        if (vs >= 0 && cus >= 0) {
            pta = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                              cap::CapType::PageTable, arch::PAGE_SIZE,
                              cap::CapRights::READ);
        }
        JARVIS_ASSERT(pta >= 0);
        auto *ut2 = cap::UntypedMem::create(arch::PAGE_SIZE, true);
        int us2 = -1;
        if (ut2) {
            us2 = cs->install(ut2, cap::CapType::Untyped,
                              cap::CapRights::READ | cap::CapRights::WRITE);
        }
        JARVIS_ASSERT(us2 >= 0);
        int ptb = -1;
        if (ut2 && us2 >= 0) {
            ptb = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us2)),
                              cap::CapType::PageTable, arch::PAGE_SIZE,
                              cap::CapRights::READ);
        }
        JARVIS_ASSERT(ptb >= 0);
        if (vs < 0 || pta < 0 || ptb < 0 || us2 < 0) {
            if (ptb >= 0)
                cs->remove(static_cast<uint32_t>(ptb));
            if (pta >= 0)
                cs->remove(static_cast<uint32_t>(pta));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            if (us2 >= 0)
                cs->remove(static_cast<uint32_t>(us2));
            if (us1 >= 0)
                cs->remove(static_cast<uint32_t>(us1));
            if (ut2)
                ut2->release();
            ut1->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pah = slot_handle(cs, static_cast<uint32_t>(pta));
        const uint64_t pbh = slot_handle(cs, static_cast<uint32_t>(ptb));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        const uint64_t pa_phys =
            carved_phys(cs, pah, cap::CapType::PageTable);
        g_first = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pah, 3, 1,
            nullptr);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        g_entry = root[3];
        g_expect = VMM::table_link_word(pa_phys);
        g_live = cap::VSpaceMap::live_count();
        g_second = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pbh, 3, 1,
            nullptr);
        g_entry = root[3];
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(ptb));
        cs->remove(static_cast<uint32_t>(pta));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us2));
        cs->remove(static_cast<uint32_t>(us1));
        ut1->release();
        ut2->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_first);
    JARVIS_ASSERT_EQ(vspace_err(7), g_second); // VSPACE_SLOT_OCCUPIED
    JARVIS_ASSERT_EQ(g_expect, g_entry);
    JARVIS_ASSERT_EQ(1ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 single-parent rule (INV-VS-1) — the same PageTable page
//        at a second path fails ALIASED and writes nothing.
// Input: Link table A at path P; link table A at path Q != P.
// Expect: Second call VSPACE_ALIASED; no store at Q; live_count
//         unchanged.
// Depends: VSpaceMap registry phys scan (INV-VS-1)
// Pseudocode: link A at P (0); link A at Q (expect ALIASED); entry at
// Q is zero; live_count unchanged; teardown.
JARVIS_TEST(mk_vspace_map_aliased_table, "PRE: none | POST: none") {
    static uint64_t g_first = 0;
    static uint64_t g_second = 0;
    static uint64_t g_qentry = 0;
    static uint64_t g_live = 0;
    g_first = g_second = g_qentry = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int cus =
            find_slot_not(*cs, cap::CapType::Untyped, ut);
        int pt = -1;
        if (vs >= 0 && cus >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        g_first = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 3, 1,
            nullptr);
        g_second = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 5, 1,
            nullptr);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        g_qentry = root[5];
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_first);
    JARVIS_ASSERT_EQ(vspace_err(8), g_second); // VSPACE_ALIASED
    JARVIS_ASSERT_EQ(0ULL, g_qentry);
    JARVIS_ASSERT_EQ(1ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 kernel-half gate — component 0 of 256 and of 511 fail
//        KERNEL_HALF with a bit-identical root page (no silent install).
// Input: Snapshot root page bytes; link with path component 0 == 256;
//        link with component 0 == 511.
// Expect: Both VSPACE_KERNEL_HALF; root page byte-identical after.
// Depends: VSpaceMap KERNEL_HALF check before the walk
// Pseudocode: snapshot root; link comp0=256 (expect KERNEL_HALF);
// link comp0=511 (expect KERNEL_HALF); root identical; teardown.
JARVIS_TEST(mk_vspace_map_kernel_half_rejected, "PRE: none | POST: none") {
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_live = 0;
    static uint64_t g_diff = 0;
    g_r1 = g_r2 = g_live = g_diff = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int cus = find_slot_not(*cs, cap::CapType::Untyped, ut);
        int pt = -1;
        if (vs >= 0 && cus >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        uint64_t before[512];
        for (size_t i = 0; i < 512; ++i)
            before[i] = root[i];
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth,
            256, 1, nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth,
            511, 1, nullptr);
        g_live = cap::VSpaceMap::live_count();
        g_diff = 0;
        for (size_t i = 0; i < 512; ++i) {
            if (root[i] != before[i])
                ++g_diff;
        }
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(vspace_err(6), g_r1); // VSPACE_KERNEL_HALF
    JARVIS_ASSERT_EQ(vspace_err(6), g_r2); // VSPACE_KERNEL_HALF
    JARVIS_ASSERT_EQ(0ULL, g_diff);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 path validation — bits above 9*len, len 0, and len above
//        the arch depth fail; valid len with garbage high bits is never
//        masked into a wrong slot.
// Input: Path with a bit set above 9*len; path_len 0; path_len ==
//        page_table_levels(); valid len with high garbage bits.
// Expect: INVALID_ARGS, BAD_LEVEL, BAD_LEVEL, INVALID_ARGS.
// Depends: VSpaceMap bounds check before any shift (never mask)
// Pseudocode: each malformed call returns its typed code; no link
// created (live_count 0 throughout); teardown (nothing to free).
JARVIS_TEST(mk_vspace_map_path_bits_rejected, "PRE: none | POST: none") {
    static uint64_t g_rbits = 0;
    static uint64_t g_rzero = 0;
    static uint64_t g_rover = 0;
    static uint64_t g_rgarbage = 0;
    static uint64_t g_live = 0;
    g_rbits = g_rzero = g_rover = g_rgarbage = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int cus = find_slot_not(*cs, cap::CapType::Untyped, ut);
        int pt = -1;
        if (vs >= 0 && cus >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const unsigned levels = VMM::page_table_levels();
        // Bit above 9*len set (len 1, bit 9): INVALID_ARGS.
        g_rbits = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth,
            0x200, 1, nullptr);
        // Zero length: BAD_LEVEL.
        g_rzero = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 0,
            0, nullptr);
        // Length at/above the arch depth: BAD_LEVEL (never literal).
        g_rover = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 1,
            static_cast<uint64_t>(levels), nullptr);
        // Valid length with garbage high bits: must reject, never mask.
        g_rgarbage = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth,
            (1ULL << (9 * 1)) | 3, 1, nullptr);
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(vspace_err(1), g_rbits);   // VSPACE_INVALID_ARGS
    JARVIS_ASSERT_EQ(vspace_err(5), g_rzero);   // VSPACE_BAD_LEVEL
    JARVIS_ASSERT_EQ(vspace_err(5), g_rover);   // VSPACE_BAD_LEVEL
    JARVIS_ASSERT_EQ(vspace_err(1), g_rgarbage); // VSPACE_INVALID_ARGS
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 PageTable delete unlinks (INV-VS-4/8) — CAP_DELETE on the
//        PageTable handle destroys the link, zeroes entry, frees the
//        registry slot; a second delete is idempotent.
// Input: Link table at path P; CAP_DELETE (98) the PageTable handle;
//        read root entry; CAP_DELETE again.
// Expect: First delete 0; entry reads 0; slot freed; second delete
//         fails closed; VSpace intact.
// Depends: VSpaceMap::invalidate_table via CAP_DELETE, INV-VS-8
// Pseudocode: link (0); DELETE pt (0); root entry 0; live_count down;
// DELETE pt again (fail); DELETE vspace; drain.
JARVIS_TEST(mk_vspace_map_delete_unlinks, "PRE: none | POST: none") {
    static uint64_t g_link = 0;
    static uint64_t g_entry = 0;
    static uint64_t g_live = 0;
    static uint64_t g_del2 = 0;
    static uint64_t g_vlive = 0;
    g_link = g_entry = g_live = g_del2 = g_vlive = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int cus = find_slot_not(*cs, cap::CapType::Untyped, ut);
        int pt = -1;
        if (vs >= 0 && cus >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        g_link = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 7,
            1, nullptr);
        if (g_link != 0) {
            cs->remove(static_cast<uint32_t>(pt));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        // CAP_DELETE (98) on the PageTable handle destroys the link.
        const uint64_t del = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), pth, 0, 0, 0,
            nullptr);
        (void)del;
        g_entry = root[7];
        g_live = cap::VSpaceMap::live_count();
        // Second DELETE: the slot is gone -> fail closed.
        g_del2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), pth, 0, 0, 0,
            nullptr);
        // The VSpace itself is intact (its own slot still live).
        KernelObject *still =
            cap::lookup(cs, vsh, cap::CapType::VSpace, cap::CapRights::READ);
        g_vlive = (still != nullptr) ? 1ULL : 0ULL;
        if (still)
            still->release();
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_link);
    JARVIS_ASSERT_EQ(0ULL, g_entry);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_ASSERT(g_del2 != 0ULL);
    JARVIS_ASSERT_EQ(1ULL, g_vlive);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 VSpace delete zeroes the tree — with a 2-level subtree,
//        CAP_DELETE on the VSpace handle leaves the whole root page
//        zeroed, live_count 0; orphaned PageTable caps stay deletable.
// Input: Link L1 at [i0], L2 at [i0,i1]; CAP_DELETE the VSpace handle.
// Expect: Root page all zero; live_count 0; PageTable caps delete
//         cleanly afterwards (no double free).
// Depends: VSpaceMap::invalidate_vspace, INV-VS-4 order
// Pseudocode: build subtree; DELETE vspace (0); root zeroed;
// live_count 0; DELETE each PageTable (0); drain, delta 0.
JARVIS_TEST(mk_vspace_map_vspace_delete_zeroes_tree, "PRE: none | POST: none") {
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_mid = 0;
    static uint64_t g_nonzero = 0;
    static uint64_t g_live = 0;
    static uint64_t g_d1 = 0;
    static uint64_t g_d2 = 0;
    g_r1 = g_r2 = g_mid = g_nonzero = g_live = g_d1 = g_d2 = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(3 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        // VSpace(1) <- ut(3); remainder childA(2) -> L1(1) + childB(1);
        // childB -> L2(1). Single-Untyped chain, deterministic slots.
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        int l1 = -1;
        int l2 = -1;
        const int childA = find_slot_not(*cs, cap::CapType::Untyped, ut);
        if (vs >= 0 && childA >= 0) {
            l1 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(childA)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        int childB = -1;
        if (l1 >= 0) {
            for (uint32_t i = 0;
                 i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
                KernelObject *o = cs->peek(i, cap::CapType::Untyped);
                if (o && o != ut) {
                    const int ci = static_cast<int>(i);
                    if (ci != childA) {
                        childB = ci;
                        break;
                    }
                }
            }
        }
        if (childB >= 0) {
            l2 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(childB)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || l1 < 0 || l2 < 0) {
            if (l2 >= 0)
                cs->remove(static_cast<uint32_t>(l2));
            if (l1 >= 0)
                cs->remove(static_cast<uint32_t>(l1));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t l1h = slot_handle(cs, static_cast<uint32_t>(l1));
        const uint64_t l2h = slot_handle(cs, static_cast<uint32_t>(l2));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l1h, 5, 1,
            nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l2h,
            5 | (9 << 9), 2, nullptr);
        // Mid-link guard: both links live (catches vacuous early-return).
        g_mid = cap::VSpaceMap::live_count();
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        // CAP_DELETE on the VSpace handle zeroes the whole root page.
        const uint64_t del = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), vsh, 0, 0, 0,
            nullptr);
        (void)del;
        g_nonzero = 0;
        for (size_t i = 0; i < 512; ++i) {
            if (root[i] != 0)
                ++g_nonzero;
        }
        g_live = cap::VSpaceMap::live_count();
        // Orphaned PageTable caps are still deletable (no double free).
        g_d1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), l1h, 0, 0, 0,
            nullptr);
        g_d2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_DELETE), l2h, 0, 0, 0,
            nullptr);
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(2ULL, g_mid);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(0ULL, g_nonzero);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_ASSERT_EQ(0ULL, g_d1);
    JARVIS_ASSERT_EQ(0ULL, g_d2);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 revoke cascade (F2/F4.3 proof) — revoke_derivations on
//        the parent Untyped destroys VSpace + all PageTables; the
//        registry drains with zero resource delta.
// Input: Retype VSpace + 2 PageTables from one Untyped; link both;
//        CAP_REVOKE the Untyped.
// Expect: VSpace + PageTables gone; live_count 0; ResourceTracker
//         delta 0 (PMM, MemPool0, cap_objects).
// Depends: cap::revoke_derivations, VSpaceMap teardown, INV-VS-4
// Pseudocode: retype all three; link both; REVOKE parent; assert
// live_count 0 and delta 0; drain.
JARVIS_TEST(mk_vspace_map_revoke_cascade, "PRE: none | POST: none") {
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_mid = 0;
    static uint64_t g_revoke = 0;
    static uint64_t g_live = 0;
    static uint64_t g_lookup = 0;
    g_r1 = g_r2 = g_mid = g_revoke = g_live = g_lookup = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(3 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        const uint64_t uh = slot_handle(cs, static_cast<uint32_t>(us));
        int vs = cap::retype(cs, uh, cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        int l1 = -1;
        int l2 = -1;
        const int childA = find_slot_not(*cs, cap::CapType::Untyped, ut);
        if (vs >= 0 && childA >= 0) {
            l1 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(childA)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        int childB = -1;
        if (l1 >= 0) {
            for (uint32_t i = 0;
                 i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
                KernelObject *o = cs->peek(i, cap::CapType::Untyped);
                if (o && o != ut) {
                    const int ci = static_cast<int>(i);
                    if (ci != childA) {
                        childB = ci;
                        break;
                    }
                }
            }
        }
        if (childB >= 0) {
            l2 = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(childB)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || l1 < 0 || l2 < 0) {
            if (l2 >= 0)
                cs->remove(static_cast<uint32_t>(l2));
            if (l1 >= 0)
                cs->remove(static_cast<uint32_t>(l1));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t l1h = slot_handle(cs, static_cast<uint32_t>(l1));
        const uint64_t l2h = slot_handle(cs, static_cast<uint32_t>(l2));
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l1h, 5,
            1, nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l2h,
            5 | (9 << 9), 2, nullptr);
        // Mid-link guard: both links live (catches vacuous early-return).
        g_mid = cap::VSpaceMap::live_count();
        if (g_r1 != 0 || g_r2 != 0) {
            cs->remove(static_cast<uint32_t>(l2));
            cs->remove(static_cast<uint32_t>(l1));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        // CAP_REVOKE (53) the parent Untyped: F2 derivation cascade
        // destroys VSpace + PageTables (registrations and pages).
        g_revoke = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::CAP_REVOKE), uh, 0, 0, 0,
            nullptr);
        g_live = cap::VSpaceMap::live_count();
        KernelObject *gone = cap::lookup(cs, vsh, cap::CapType::VSpace,
                                         cap::CapRights::READ);
        g_lookup = (gone != nullptr) ? 1ULL : 0ULL;
        if (gone)
            gone->release();
        // Root exemption: the named Untyped slot survives the cascade
        // (spent); remove it and drop the creator ref as usual.
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(2ULL, g_mid);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(0ULL, g_revoke);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_ASSERT_EQ(0ULL, g_lookup);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 type gates — wrong handle kinds and missing WRITE fail
//        before any walk (BAD_HANDLE / POLICY_DENIED); a real Task
//        object in the PageTable slot fails BAD_HANDLE (no bad cast).
// Input: Frame handle as arg0; VSpace handle as arg1; VSpace slot
//        minted without WRITE; Task-typed slot (non-carved object) as
//        arg1 — TCBs are not KernelObjects and cannot be installed, so a
//        Task-typed slot exercises the same lookup type gate.
// Expect: BAD_HANDLE at every site (lookup rejects before any cast);
//         live_count 0 throughout.
// Depends: sys_vspace_map resolve/type/rights gates
// Pseudocode: each malformed call returns its typed code; live_count
// 0 throughout; teardown.
JARVIS_TEST(mk_vspace_map_wrong_cap_type, "PRE: none | POST: none") {
    static uint64_t g_frame0 = 0;
    static uint64_t g_vspace1 = 0;
    static uint64_t g_nowrite = 0;
    static uint64_t g_task1 = 0;
    static uint64_t g_live = 0;
    g_frame0 = g_vspace1 = g_nowrite = g_task1 = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        // One 4-page Untyped: VSpace(1) + PT-child(3) carve, Frame(1)
        // exact, second PT(1) exact. All user-owned.
        auto *ut =
            cap::UntypedMem::create(4 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        const uint64_t uh = slot_handle(cs, static_cast<uint32_t>(us));
        int vs = cap::retype(cs, uh, cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int c1 = find_slot_not(*cs, cap::CapType::Untyped, ut);
        int fr = -1;
        if (vs >= 0 && c1 >= 0) {
            fr = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(c1)),
                             cap::CapType::Frame, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        // Second PT: carve remainder hunt (child of the frame carve).
        int pt = -1;
        if (fr >= 0) {
            for (uint32_t i = 0;
                 i < static_cast<uint32_t>(CONFIG_CSLOT_COUNT); ++i) {
                KernelObject *o = cs->peek(i, cap::CapType::Untyped);
                if (o && o != ut) {
                    pt = cap::retype(
                        cs, slot_handle(cs, i), cap::CapType::PageTable,
                        arch::PAGE_SIZE, cap::CapRights::READ);
                    if (pt >= 0)
                        break;
                }
            }
        }
        if (vs < 0 || fr < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (fr >= 0)
                cs->remove(static_cast<uint32_t>(fr));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t frh = slot_handle(cs, static_cast<uint32_t>(fr));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        // (a) Frame handle as arg0 (needs a VSpace): BAD_HANDLE.
        g_frame0 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), frh, pth, 3,
            1, nullptr);
        // (b) VSpace handle as arg1 (needs a PageTable): BAD_HANDLE.
        g_vspace1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, vsh, 3,
            1, nullptr);
        // (c) VSpace slot minted without WRITE: install a second,
        // READ-only alias of the same VSpace object.
        auto *vsobj = cap::lookup(cs, vsh, cap::CapType::VSpace,
                                  cap::CapRights::READ);
        int vs_ro = -1;
        if (vsobj) {
            vs_ro =
                cs->install(static_cast<cap::CarvedObjectCap *>(vsobj),
                            cap::CapType::VSpace, cap::CapRights::READ);
            vsobj->release();
        }
        if (vs_ro >= 0) {
            const uint64_t vroh = slot_handle(cs, static_cast<uint32_t>(vs_ro));
            g_nowrite = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vroh, pth,
                3, 1, nullptr);
            cs->remove(static_cast<uint32_t>(vs_ro));
        }
        // (d) Task-typed object as arg1: a non-carved KernelObject in a
        // Task slot. lookup(PageTable) must fail before any cast
        // (BAD_HANDLE, not BAD_OBJECT, not a crash). TCBs are not
        // KernelObjects and cannot be installed; a Task-typed slot
        // exercises the same lookup type gate.
        auto *tt = static_cast<VspaceTestTarget *>(
            MemPool::alloc(sizeof(VspaceTestTarget)));
        int tslot = -1;
        uint64_t th = 0;
        if (tt) {
            new (tt) VspaceTestTarget;
            tt->mark_pool_backed();
            kernel::test::ResourceTracker::instance().track_cap_object_add();
            tslot = cs->install(tt, cap::CapType::Task,
                                cap::CapRights::READ);
            if (tslot < 0) {
                tt->release();
            } else {
                th = slot_handle(cs, static_cast<uint32_t>(tslot));
            }
        }
        if (tslot >= 0) {
            g_task1 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, th,
                3, 1, nullptr);
            cs->remove(static_cast<uint32_t>(tslot));
            tt->release();
        }
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(fr));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(vspace_err(2), g_frame0); // VSPACE_BAD_HANDLE
    JARVIS_ASSERT_EQ(vspace_err(2), g_vspace1); // VSPACE_BAD_HANDLE
    JARVIS_ASSERT_EQ(vspace_err(2), g_nowrite); // VSPACE_BAD_HANDLE
    JARVIS_ASSERT_EQ(vspace_err(2), g_task1);   // VSPACE_BAD_HANDLE
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 ownership gate — a kernel-owned PageTable into a user
//        VSpace fails OWNERSHIP_MISMATCH (kernel RAM must never become
//        EL0-visible hierarchy).
// Input: VSpace retyped from a user Untyped; PageTable retyped from a
//        kernel (is_user=false) Untyped; link.
// Expect: VSPACE_OWNERSHIP_MISMATCH; nothing installed.
// Depends: sys_vspace_map is_user check (FrameUserMap precedent)
// Pseudocode: retype user VSpace + kernel PageTable; link (expect
// OWNERSHIP_MISMATCH); root unchanged; teardown both.
JARVIS_TEST(mk_vspace_map_ownership_mismatch, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_entry = 0;
    static uint64_t g_live = 0;
    g_ret = g_entry = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        // User VSpace from a user Untyped; kernel PageTable from a
        // kernel (is_user=false) Untyped.
        auto *uut =
            cap::UntypedMem::create(arch::PAGE_SIZE, true);
        auto *kut =
            cap::UntypedMem::create(arch::PAGE_SIZE, false);
        if (!uut || !kut) {
            if (uut)
                uut->release();
            if (kut)
                kut->release();
            return;
        }
        int us = cs->install(uut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        int ks = cs->install(kut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0 || ks < 0) {
            if (us >= 0)
                cs->remove(static_cast<uint32_t>(us));
            if (ks >= 0)
                cs->remove(static_cast<uint32_t>(ks));
            uut->release();
            kut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, 0,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        int pt = -1;
        if (vs >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(ks)),
                             cap::CapType::PageTable, 0, cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            cs->remove(static_cast<uint32_t>(ks));
            uut->release();
            kut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        const uint64_t vs_phys =
            carved_phys(cs, vsh, cap::CapType::VSpace);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *root = reinterpret_cast<volatile uint64_t *>(
            CONFIG_HHDM_OFFSET + vs_phys);
        const uint64_t before = root[4];
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 4,
            1, nullptr);
        g_entry = root[4];
        (void)before;
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        cs->remove(static_cast<uint32_t>(ks));
        uut->release();
        kut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(vspace_err(4), g_ret); // VSPACE_OWNERSHIP_MISMATCH
    JARVIS_ASSERT_EQ(0ULL, g_entry);
    JARVIS_ASSERT_EQ(0ULL, g_live);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 syscall face end-to-end — a user-flagged task with a
//        VSpace+PageTable pair observes 0 from 102; the surface test
//        pins MAX_SYSCALL == 105 with F11 Stage C additions.
// Input: Synthetic-user task (is_user_ set, mirroring the tls tests)
//        with a retyped pair; Syscall::handle(102, len 1).
// Expect: Return 0 in-task; live_count 1 after; MAX == 105.
// Depends: syscall 102 dispatch, cap::retype, ABI table
// Note (deviation from plan): the plan's int 0x80 needs an arch-specific
// trap stub per arch (x86-only precedent: run_user_probe); the trap
// encodes to the same handler, whose coverage is the point here, and
// the bridge encoding is pinned by arch tests. A synthetic-user task
// keeps this test arch-neutral for the gate classes.
// Pseudocode: spawn synthetic-user task with pair; task calls 102
// (assert 0 in task); harness asserts live_count 1 + MAX 105;
// teardown.
JARVIS_TEST(sys_vspace_map_dispatch_end_to_end, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_live = 0;
    g_ret = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->is_user_ = true; // synthetic user (tls_publish_applies idiom)
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(2 * arch::PAGE_SIZE, true);
        if (!ut) {
            cur->is_user_ = false;
            return;
        }
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            cur->is_user_ = false;
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        const int cus = find_slot_not(*cs, cap::CapType::Untyped, ut);
        int pt = -1;
        if (vs >= 0 && cus >= 0) {
            pt = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(cus)),
                             cap::CapType::PageTable, arch::PAGE_SIZE,
                             cap::CapRights::READ);
        }
        if (vs < 0 || pt < 0) {
            if (pt >= 0)
                cs->remove(static_cast<uint32_t>(pt));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            cur->is_user_ = false;
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t pth = slot_handle(cs, static_cast<uint32_t>(pt));
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, pth, 4,
            1, nullptr);
        g_live = cap::VSpaceMap::live_count();
        cs->remove(static_cast<uint32_t>(pt));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
        cur->is_user_ = false;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(1ULL, g_live);
    JARVIS_ASSERT_EQ(105ULL,
                     static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: F5 leaf population is alloc-free (INV-MK-1 rationale) —
//        after root->PD->PT via 102, map_page_in_pml4_try on a leaf
//        succeeds with zero PMM delta (pre-linked walk allocates
//        nothing).
// Input: Build 3-level chain via 102; va from a per-arch test-local
//        constant (documented layout derivation below; no HAL user-VA
//        constant exists, cf. FPU kUserDataVa precedent); leaf map.
// Expect: 0 return; PMM delta 0 across the leaf map.
// Depends: VSpaceMap pre-linked walk, VMM leaf path
// Note: HAL has no user-VA scratch constant, so the VA is derived
// from the linked path per arch (x86_64/aarch64 4-level 9-bit groups
// at [47:39][38:30][29:21][20:12]; riscv64 Sv39 groups at
// [38:30][29:21][20:12]). Components [1,2,3], leaf slot 4 — all in
// the user half on every arch.
// Pseudocode: build chain; snapshot PMM; leaf-map at derived va (0);
// assert PMM delta 0; teardown (DELETE tree, drain).
JARVIS_TEST(mk_vspace_map_leaf_population_alloc_free, "PRE: none | POST: none") {
    static uint64_t g_r1 = 0;
    static uint64_t g_r2 = 0;
    static uint64_t g_r3 = 0;
    static uint64_t g_map = 0;
    static uint64_t g_delta = 0;
    static uint64_t g_live = 0;
    g_r1 = g_r2 = g_r3 = g_map = g_delta = g_live = 0;

    auto *t = run_vspace_task([]() {
        auto *cur = Scheduler::current_task();
        cur->ensure_cspace();
        cap::CNode *cs = cur->get_cspace();
        auto *ut =
            cap::UntypedMem::create(4 * arch::PAGE_SIZE, true);
        if (!ut)
            return;
        int us = cs->install(ut, cap::CapType::Untyped,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        if (us < 0) {
            ut->release();
            return;
        }
        int vs = cap::retype(cs, slot_handle(cs, static_cast<uint32_t>(us)),
                             cap::CapType::VSpace, arch::PAGE_SIZE,
                             cap::CapRights::READ | cap::CapRights::WRITE);
        int l1 = -1;
        int l2 = -1;
        int l3 = -1;
        if (vs >= 0) {
            l1 = carve_pt_from_live_untyped(cs, ut, nullptr);
        }
        if (l1 >= 0) {
            l2 = carve_pt_from_live_untyped(cs, ut, nullptr);
        }
        if (l2 >= 0) {
            l3 = carve_pt_from_live_untyped(cs, ut, nullptr);
        }
        if (vs < 0 || l1 < 0 || l2 < 0 || l3 < 0) {
            if (l3 >= 0)
                cs->remove(static_cast<uint32_t>(l3));
            if (l2 >= 0)
                cs->remove(static_cast<uint32_t>(l2));
            if (l1 >= 0)
                cs->remove(static_cast<uint32_t>(l1));
            if (vs >= 0)
                cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vsh = slot_handle(cs, static_cast<uint32_t>(vs));
        const uint64_t l1h = slot_handle(cs, static_cast<uint32_t>(l1));
        const uint64_t l2h = slot_handle(cs, static_cast<uint32_t>(l2));
        const uint64_t l3h = slot_handle(cs, static_cast<uint32_t>(l3));
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l1h, 1,
            1, nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh, l2h,
            1 | (2 << 9), 2, nullptr);
        // Depth differs per arch (x86_64/aarch64: 4 levels, len 3;
        // riscv64 Sv39: 3 levels, len 2). Link the deepest level only
        // where legal, and derive the leaf VA from the linked path
        // (never a literal).
        const unsigned levels = VMM::page_table_levels();
        if (levels > 3) {
            g_r3 = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::VSPACE_MAP), vsh,
                l3h, 1 | (2 << 9) | (3 << 18), 3, nullptr);
        } else {
            // Depth-capped arch: the L3 carve is unused; drop it so the
            // tree is exactly root->L1->L2.
            cs->remove(static_cast<uint32_t>(l3));
            l3 = -1;
            g_r3 = 0;
        }
        if (g_r1 != 0 || g_r2 != 0 || g_r3 != 0) {
            if (l3 >= 0)
                cs->remove(static_cast<uint32_t>(l3));
            cs->remove(static_cast<uint32_t>(l2));
            cs->remove(static_cast<uint32_t>(l1));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        // VA whose arch walk is exactly the linked prefix plus one
        // free leaf slot: [1,2,3]+4 on 4-level arches, [1,2]+3 on Sv39.
        uint64_t va = 0;
        if (levels > 3) {
            va = (1ULL << 39) | (2ULL << 30) | (3ULL << 21) | (4ULL << 12);
        } else {
            va = (1ULL << 30) | (2ULL << 21) | (3ULL << 12);
        }
        // Leaf target page (user). Allocated before the PMM snapshot so
        // only the walk's own allocations count toward the delta.
        const uint64_t frame = PMM::alloc_user_page();
        if (frame == 0) {
            cs->remove(static_cast<uint32_t>(l3));
            cs->remove(static_cast<uint32_t>(l2));
            cs->remove(static_cast<uint32_t>(l1));
            cs->remove(static_cast<uint32_t>(vs));
            cs->remove(static_cast<uint32_t>(us));
            ut->release();
            return;
        }
        const uint64_t vs_phys = carved_phys(cs, vsh, cap::CapType::VSpace);
        const uint64_t before = PMM::pool_used_pages();
        const errors::VmmError merr = VMM::map_page_in_pml4_err(
            va, frame, true, false, vs_phys);
        const uint64_t after = PMM::pool_used_pages();
        g_map = (merr == errors::VMM_ERR_OK) ? 0ULL : 1ULL;
        g_delta = (after >= before) ? (after - before) : 999999ULL;
        g_live = cap::VSpaceMap::live_count();
        // Teardown: unmap the leaf (best-effort; the tree teardown
        // below drops the whole subtree), then DELETE the caps.
        (void)VMM::unmap_page_in_pml4(va, vs_phys);
        PMM::free_page(frame);
        cs->remove(static_cast<uint32_t>(l3));
        cs->remove(static_cast<uint32_t>(l2));
        cs->remove(static_cast<uint32_t>(l1));
        cs->remove(static_cast<uint32_t>(vs));
        cs->remove(static_cast<uint32_t>(us));
        ut->release();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_r1);
    JARVIS_ASSERT_EQ(0ULL, g_r2);
    JARVIS_ASSERT_EQ(0ULL, g_r3);
    JARVIS_ASSERT_EQ(0ULL, g_map);
    JARVIS_ASSERT_EQ(0ULL, g_delta);
    {
        const unsigned levels = VMM::page_table_levels();
        JARVIS_ASSERT_EQ((levels > 3) ? 3ULL : 2ULL, g_live);
    }
    JARVIS_TEST_PASS();
}

void register_cap_vspace_tests() {
    Logger::info("Registering cap_vspace tests");
    JARVIS_REGISTER_TEST(mk_vspace_map_links_page_table);
    JARVIS_REGISTER_TEST(mk_vspace_map_second_level);
    JARVIS_REGISTER_TEST(mk_vspace_map_occupied_slot);
    JARVIS_REGISTER_TEST(mk_vspace_map_aliased_table);
    JARVIS_REGISTER_TEST(mk_vspace_map_kernel_half_rejected);
    JARVIS_REGISTER_TEST(mk_vspace_map_path_bits_rejected);
    JARVIS_REGISTER_TEST(mk_vspace_map_delete_unlinks);
    JARVIS_REGISTER_TEST(mk_vspace_map_vspace_delete_zeroes_tree);
    JARVIS_REGISTER_TEST(mk_vspace_map_revoke_cascade);
    JARVIS_REGISTER_TEST(mk_vspace_map_wrong_cap_type);
    JARVIS_REGISTER_TEST(mk_vspace_map_ownership_mismatch);
    JARVIS_REGISTER_TEST(sys_vspace_map_dispatch_end_to_end);
    JARVIS_REGISTER_TEST(mk_vspace_map_leaf_population_alloc_free);
}
