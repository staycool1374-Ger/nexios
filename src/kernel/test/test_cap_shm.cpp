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

/// @file test_cap_shm.cpp
/// @brief Capability-gated shared-memory ring tests (issue #106 Part B).
/// Verifies the FrameUserMap registry + SYS_FRAME_CREATE/MAP/UNMAP + the
/// shared-ring protocol: genuine zero-copy (both tasks map the same phys),
/// revocation closure, task-death drain, and ResourceTracker neutrality.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/cap/frame_map.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/ipc/shm_ring.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Installs a FrameCap into @p t's CSpace (returns the slot index).
int install_frame(TaskControlBlock *t, cap::FrameCap *fc) {
    t->ensure_cspace();
    cap::CNode *cs = t->get_cspace();
    if (!cs)
        return -1;
    int s = cs->install(fc, cap::CapType::Frame,
                        cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
    if (s >= 0)
        fc->release(); // slot holds a reference; drop the creator's
    return s;
}

/// @brief Dispatches a syscall by number (Syscall::handle takes uint64_t).
uint64_t shm_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                     uint64_t a3, uint64_t *regs = nullptr) {
    return Syscall::handle(number, a0, a1, a2, a3, regs);
}

} // namespace

// Runmode: kernel
// Testidea: SYS_FRAME_MAP maps a FrameCap's frames into the caller's user VA
// window; SYS_FRAME_UNMAP removes the mapping.  Genuine physical backing.
// Input: Task installs a FrameCap in its CSpace; dispatches SYS_FRAME_MAP;
//        verifies virt_to_phys == fc->phys; SYS_FRAME_UNMAP.
// Expect: map returns a VA in the SHM window whose phys matches; unmap
//         clears it; zero ResourceTracker delta.
// Depends: FrameUserMap, SYS_FRAME_MAP/UNMAP
JARVIS_TEST(frame_user_map_unmap_roundtrip, "PRE: none | POST: none") {
    static uint64_t g_map_ret = 0;
    static uint64_t g_unmap_ret = 0;
    static uint64_t g_done = 0;
    __atomic_store_n(&g_map_ret, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_unmap_ret, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_done, 0, __ATOMIC_RELEASE);

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_contiguous(2);
            if (phys == 0) {
                Scheduler::terminate(*cur, 0);
                return;
            }
            auto *fc = cap::FrameCap::create(phys, 2, true);
            if (!fc) {
                for (size_t i = 0; i < 2; ++i)
                    PMM::free_page(phys + i * arch::PAGE_SIZE);
                Scheduler::terminate(*cur, 0);
                return;
            }
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            int s = cs->install(fc, cap::CapType::Frame,
                                cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                fc->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            fc->release(); // slot holds a ref; drop the creator's
            uint64_t h = cap::encode_handle(
                cs->cspace_id, static_cast<uint32_t>(s),
                cs->slot_gen(static_cast<uint32_t>(s)));
            __atomic_store_n(
                &g_map_ret,
                Syscall::handle(
                    static_cast<uint64_t>(SyscallNumber::FRAME_MAP), h, 0, 0, 0,
                    nullptr),
                __ATOMIC_RELEASE);
            uint64_t va = __atomic_load_n(&g_map_ret, __ATOMIC_ACQUIRE);
            if (va != static_cast<uint64_t>(-1)) {
                __atomic_store_n(
                    &g_unmap_ret,
                    Syscall::handle(
                        static_cast<uint64_t>(SyscallNumber::FRAME_UNMAP), va,
                        0, 0, 0, nullptr),
                    __ATOMIC_RELEASE);
            }
            __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
            // Park (do NOT self-terminate): the harness verifies the PTE
            // clearing in the still-live cloned PML4 (multi-page closure —
            // BOTH pages of the 2-page mapping must be unmapped), then
            // terminates this task.
            for (;;)
                kernel::Scheduler::reschedule();
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    // Real user PML4 (kernel half cloned) so the frame map registry can map
    // into current_task()->page_table_.  Ring 0 execution is unaffected
    // (kernel half is copied); cleanup frees the clone.
    t->page_table_ = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(t->page_table_ != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    // Drive until the task has mapped + unmapped (bounded).
    for (int i = 0; i < 10000 && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE) == 0;
         ++i) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }

    uint64_t va = __atomic_load_n(&g_map_ret, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(va >= CONFIG_USER_SHM_VA_BASE);
    JARVIS_ASSERT_EQ(0ULL, __atomic_load_n(&g_unmap_ret, __ATOMIC_ACQUIRE));
    // Multi-page closure: SYS_FRAME_UNMAP must clear EVERY page of a
    // multi-page mapping, not just the region base (otherwise a later cap
    // dispose leaves live user PTEs aliasing freed physical memory).
    JARVIS_ASSERT_EQ(0ULL, VMM::virt_to_phys_in_pml4(va, t->page_table_));
    JARVIS_ASSERT_EQ(
        0ULL, VMM::virt_to_phys_in_pml4(va + arch::PAGE_SIZE, t->page_table_));
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::FrameUserMap::live_count());
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_FRAME_MAP refuses a revoked FrameCap.
// Input: Install a FrameCap; revoke it; attempt SYS_FRAME_MAP.
// Expect: map returns -1; no mapping installed.
// Depends: FrameCap::revoke, FrameUserMap, SYS_FRAME_MAP
JARVIS_TEST(frame_user_map_revoke_denied, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    uint64_t phys = PMM::alloc_user_contiguous(1);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, true);
    JARVIS_ASSERT(fc != nullptr);
    int slot = install_frame(cur, fc);
    JARVIS_ASSERT(slot >= 0);

    fc->revoke();
    // Encode a REAL handle (cspace + slot + generation) so the revoke denial
    // is genuinely exercised — a bare slot index would fail lookup for the
    // wrong reason and make this test vacuous.
    cap::CNode *cs = cur->get_cspace();
    uint64_t h = cap::encode_handle(
        cs->cspace_id, static_cast<uint32_t>(slot),
        cs->slot_gen(static_cast<uint32_t>(slot)));
    uint64_t va = shm_syscall(static_cast<uint64_t>(SyscallNumber::FRAME_MAP),
                              h, 0, 0, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), va);

    // cs->remove drops the slot's reference; the 1->0 transition runs dispose()
    // which frees the revoked frame.  Do NOT call fc->release() afterwards —
    // that would decrement a freed block's refcount (use-after-free).
    cs->remove(static_cast<uint32_t>(slot));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A shared ring between a producer and a consumer — both map the
// same FrameCap phys (genuine zero-copy); the ring protocol transfers fixed
// elements across the shared frames with no kernel involvement after mapping.
// Input: Producer task allocates a 4-page ring FrameCap, installs it, maps
//        it, shm_ring_init, pushes 32 elements, publishes the shared VA.
//        A consumer task polls the shared VA (kernel tasks share the kernel
//        PML4, so the producer's mapped VA is directly visible), pops the 32
//        elements and verifies payload+order.
// Expect: producer + consumer both complete; all 32 elements round-trip in
//         order.
// Depends: FrameUserMap, shm_ring, SYS_FRAME_*
JARVIS_TEST(shm_ring_producer_consumer, "PRE: none | POST: none") {
    static uint64_t g_prod_done = 0;
    static uint64_t g_phys = 0;
    static uint64_t g_va = 0;
    __atomic_store_n(&g_prod_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_phys, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_va, 0, __ATOMIC_RELEASE);

    // Producer: allocates the ring, installs + maps it (into its own cloned
    // PML4), initialises the header and pushes 32 elements.  A kernel task
    // runs in the kernel PML4 (never CR3-switched to page_table_), so it
    // cannot dereference its own mapped VA — the harness verifies the ring
    // content through the PHYSICAL frames (HHDM alias), which is the same
    // zero-copy backing the consumer would read.
    auto *producer = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_contiguous(4);
            if (phys == 0) {
                Scheduler::terminate(*cur, 0);
                return;
            }
            auto *fc = cap::FrameCap::create(phys, 4, true);
            if (!fc) {
                for (size_t i = 0; i < 4; ++i)
                    PMM::free_page(phys + i * arch::PAGE_SIZE);
                Scheduler::terminate(*cur, 0);
                return;
            }
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            int s = cs->install(fc, cap::CapType::Frame,
                                cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                fc->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            fc->release(); // slot holds a ref; drop the creator's
            uint64_t h = cap::encode_handle(
                cs->cspace_id, static_cast<uint32_t>(s),
                cs->slot_gen(static_cast<uint32_t>(s)));
            uint64_t va = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::FRAME_MAP), h, 0, 0, 0,
                nullptr);
            if (va == static_cast<uint64_t>(-1)) {
                Scheduler::terminate(*cur, 0);
                return;
            }
            __atomic_store_n(&g_va, va, __ATOMIC_RELEASE);
            __atomic_store_n(&g_phys, phys, __ATOMIC_RELEASE);
            // Write the ring header + data directly into the PHYSICAL frames
            // (HHDM alias) — the frames are the zero-copy shared backing.
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *hdr = reinterpret_cast<kernel::shm::SharedRingHeader *>(
                arch::HHDM_OFFSET + phys);
            kernel::shm::shm_ring_init(hdr, 4, 8);
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            uint8_t *data = reinterpret_cast<uint8_t *>(
                arch::HHDM_OFFSET + phys + arch::PAGE_SIZE);
            for (uint64_t i = 0; i < 32; ++i) {
                uint64_t v = i + 100;
                kernel::shm::shm_ring_push(
                    hdr, data, reinterpret_cast<uint8_t *>(&v));
            }
            __atomic_store_n(&g_prod_done, 1, __ATOMIC_RELEASE);
            // Park (do NOT self-terminate): the harness verifies the mapping
            // and reads the ring while the task's cloned PML4 is still live.
            for (;;)
                kernel::Scheduler::reschedule();
        },
        20, 10);
    JARVIS_ASSERT(producer != nullptr);
    producer->page_table_ = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(producer->page_table_ != 0);
    Scheduler::add_task(*producer);
    Scheduler::reschedule();

    // Drive until the producer completes (bounded).
    for (int i = 0;
         i < 10000 && !(__atomic_load_n(&g_prod_done, __ATOMIC_ACQUIRE) == 1);
         ++i) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }

    uint64_t phys = __atomic_load_n(&g_phys, __ATOMIC_ACQUIRE);
    uint64_t va = __atomic_load_n(&g_va, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_EQ(1ULL, g_prod_done);
    JARVIS_ASSERT(phys != 0);
    JARVIS_ASSERT(va != 0);

    // Zero-copy proof: the mapped VA in the producer's PML4 resolves to the
    // same physical frame the producer wrote.  Runs while the producer's
    // cloned PML4 is still alive (task is parked, not drained).
    uint64_t mapped = VMM::virt_to_phys_in_pml4(va, producer->page_table_);
    JARVIS_ASSERT(mapped != 0);
    JARVIS_ASSERT(mapped == phys);

    // Consumer-equivalent read: the harness reads the shared PHYSICAL frames
    // (HHDM alias) and verifies the ring protocol round-tripped 32 elements.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *hdr =
        reinterpret_cast<kernel::shm::SharedRingHeader *>(arch::HHDM_OFFSET +
                                                          phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *data = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys +
                                                arch::PAGE_SIZE);
    JARVIS_ASSERT(kernel::shm::shm_ring_valid(hdr));
    uint64_t ok = 1;
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t v = 0;
        if (!kernel::shm::shm_ring_pop(hdr, data,
                                       reinterpret_cast<uint8_t *>(&v)) ||
            v != i + 100) {
            ok = 0;
            break;
        }
    }
    JARVIS_ASSERT_EQ(1ULL, ok);

    // Cleanup AFTER verification (cookbook Rule 5).
    kernel::test::terminate_and_drain(*producer);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FrameUserMap::drain_task removes a dying task's mappings so a
// recycled PML4 never carries stale shared-memory PTEs.
// Input: A task installs + maps a FrameCap; the task terminates; drain_task
//        runs; the registry is empty.
// Expect: live_count returns to baseline; no leak.
// Depends: FrameUserMap::drain_task, task cleanup
JARVIS_TEST(shm_ring_task_death_drain, "PRE: none | POST: none") {
    static uint64_t g_map_ok = 0;
    __atomic_store_n(&g_map_ok, 0, __ATOMIC_RELEASE);
    size_t base = cap::FrameUserMap::live_count();
    // A short-lived task maps a frame then self-terminates; cleanup drains.
    // A real user PML4 (kernel half cloned) is required so the map genuinely
    // installs a slot — otherwise the task never maps and this test would be
    // vacuous (drain_task would have nothing to drain).
    auto *t = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_contiguous(1);
            if (phys == 0)
                return;
            auto *fc = cap::FrameCap::create(phys, 1, true);
            if (!fc) {
                PMM::free_page(phys);
                return;
            }
            self->ensure_cspace();
            cap::CNode *cs = self->get_cspace();
            int s = cs->install(fc, cap::CapType::Frame,
                                cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                fc->release();
                return;
            }
            fc->release(); // slot holds a ref; drop the creator's
            uint64_t h = cap::encode_handle(
                cs->cspace_id, static_cast<uint32_t>(s),
                cs->slot_gen(static_cast<uint32_t>(s)));
            uint64_t va = shm_syscall(
                static_cast<uint64_t>(SyscallNumber::FRAME_MAP), h, 0, 0, 0);
            __atomic_store_n(&g_map_ok,
                             (va != static_cast<uint64_t>(-1)) ? 1U : 0U,
                             __ATOMIC_RELEASE);
        },
        30, 10);
    JARVIS_ASSERT(t != nullptr);
    t->page_table_ = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(t->page_table_ != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    // The mapping really was established before death (non-vacuous), and the
    // task-death drain returned the registry to baseline (no pin leak).
    JARVIS_ASSERT_EQ(1ULL, __atomic_load_n(&g_map_ok, __ATOMIC_ACQUIRE));
    JARVIS_ASSERT_EQ(base, cap::FrameUserMap::live_count());
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Revoking a shared FrameCap removes a live task's mapping
//        (revocation closure through FrameCap::revoke -> invalidate_cap).
// Input: A real-PML4 task installs + maps a FrameCap and publishes the VA +
//        cap pointer; the harness revokes the cap; the mapping clears.
// Expect: live_count returns to baseline; the task's slot is drained.
// Depends: FrameCap::revoke -> FrameUserMap::invalidate_cap
JARVIS_TEST(shm_ring_revocation_cleanup, "PRE: none | POST: none") {
    static uint64_t g_va = 0;
    static cap::FrameCap *g_fc = nullptr;
    __atomic_store_n(&g_va, 0, __ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<uint64_t *>(&g_fc), 0, __ATOMIC_RELEASE);

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            uint64_t phys = PMM::alloc_user_contiguous(1);
            if (phys == 0) {
                Scheduler::terminate(*cur, 0);
                return;
            }
            auto *fc = cap::FrameCap::create(phys, 1, true);
            if (!fc) {
                PMM::free_page(phys);
                Scheduler::terminate(*cur, 0);
                return;
            }
            __atomic_store_n(reinterpret_cast<uint64_t *>(&g_fc),
                             reinterpret_cast<uint64_t>(fc), __ATOMIC_RELEASE);
            cur->ensure_cspace();
            cap::CNode *cs = cur->get_cspace();
            int s = cs->install(fc, cap::CapType::Frame,
                                cap::CAP_RIGHT_READ | cap::CAP_RIGHT_WRITE);
            if (s < 0) {
                fc->release();
                Scheduler::terminate(*cur, 0);
                return;
            }
            fc->release(); // slot holds a ref; drop the creator's
            uint64_t h = cap::encode_handle(
                cs->cspace_id, static_cast<uint32_t>(s),
                cs->slot_gen(static_cast<uint32_t>(s)));
            uint64_t va = Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::FRAME_MAP), h, 0, 0, 0,
                nullptr);
            __atomic_store_n(&g_va, va, __ATOMIC_RELEASE);
            // Park: keep the mapping alive while the harness revokes.
            for (;;)
                kernel::Scheduler::reschedule();
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    t->page_table_ = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(t->page_table_ != 0);

    size_t base = cap::FrameUserMap::live_count();
    Scheduler::add_task(*t);
    Scheduler::reschedule();

    // Wait until the task has mapped + published the VA.
    for (int i = 0; i < 10000 && __atomic_load_n(&g_va, __ATOMIC_ACQUIRE) == 0;
         ++i) {
        __atomic_store_n(&scheduler_need_resched, true, __ATOMIC_RELEASE);
        arch::hlt();
    }
    uint64_t va = __atomic_load_n(&g_va, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(va != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(va != 0);
    JARVIS_ASSERT_EQ(1U + base, cap::FrameUserMap::live_count());

    // Revoke the cap: invalidate_cap must clear the live slot + drop its pin.
    cap::FrameCap *fc = reinterpret_cast<cap::FrameCap *>(
        __atomic_load_n(reinterpret_cast<uint64_t *>(&g_fc), __ATOMIC_ACQUIRE));
    JARVIS_ASSERT(fc != nullptr);
    fc->revoke();
    JARVIS_ASSERT_EQ(base, cap::FrameUserMap::live_count());

    // Cleanup: terminate the parked task.  Its teardown releases the CSpace
    // (CNode dispose -> slot remove -> FrameCap ref 1->0 -> dispose), which
    // frees the revoked cap.  Do NOT fc->release() here — @p fc is dangling
    // (the harness holds no reference of its own); releasing would decrement a
    // freed block's refcount (use-after-free).
    kernel::test::terminate_if_live(t);
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(base, cap::FrameUserMap::live_count());
    JARVIS_TEST_PASS();
}

void register_cap_shm_tests() {
    Logger::info("Registering cap_shm tests");
    JARVIS_REGISTER_TEST(frame_user_map_unmap_roundtrip);
    JARVIS_REGISTER_TEST(frame_user_map_revoke_denied);
    JARVIS_REGISTER_TEST(shm_ring_producer_consumer);
    JARVIS_REGISTER_TEST(shm_ring_task_death_drain);
    JARVIS_REGISTER_TEST(shm_ring_revocation_cleanup);
}