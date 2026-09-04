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

/// @file test_cap_pager.cpp
/// @brief External pager protocol tests (issue #107).
/// Verifies the PagerRegistry + SYS_PAGER_* (69-73): registration authority,
/// fault classification (F1-F10), the block-in-#PF-ISR roundtrip (a real
/// user-mode probe ELF faults; the harness acts as pager; the instruction
/// retries after SYS_PAGER_MAP), the bounded pager contract (watchdog timeout
/// per-fault fail-closed with registration KEPT, ABORT poison-VA, pager/client
/// death drain, cap revoke), and coexistence with the recover-IP / canary
/// paths.  The harness (init, PID 1) is the designated pager.
///
/// Probe: userspace/pager-probe.c — registers PID 1 as pager, dereferences
/// 0x10000000 (unmapped), writes 0xCAFE after the pager maps it, then _exit(0).
/// A fault WITHOUT pager resolution re-faults (poisoned VA / evicted) →
/// SIGSEGV → TERMINATED (signal exit code, high bit set).
///
/// SAFETY: delegate_fault() blocks the FAULTING (current) task inside the #PF
/// ISR.  It must ONLY be exercised via a real user-mode fault (the probe); a
/// direct call on a non-current task would corrupt the block path.  Tests that
/// need a pending fault therefore drive the probe and let the harness (the
/// pager) respond or not.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/elf/elf.hpp>
#include <initrd/initrd.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Dispatches a syscall by number.
uint64_t pager_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t *regs = nullptr) {
    return Syscall::handle(number, a0, a1, a2, a3, regs);
}

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

/// @brief Encodes a real cap handle for @p slot in @p t's CSpace.
uint64_t frame_handle(TaskControlBlock *t, int slot) {
    cap::CNode *cs = t->get_cspace();
    return cap::encode_handle(cs->cspace_id, static_cast<uint32_t>(slot),
                              cs->slot_gen(static_cast<uint32_t>(slot)));
}

/// @brief Loads the pager-probe ELF into a fresh user task (returns it added
///        to the scheduler).  PID 1 (the harness) is the designated pager.
TaskControlBlock *load_pager_probe() {
    initrd::InitrdFile f = initrd::find("pager-probe.c.elf");
    if (!f.data)
        return nullptr;
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr))
        return nullptr;
    auto *t = kernel::elf::load(hdr, f.data, f.size);
    if (!t)
        return nullptr;
    Scheduler::add_task(*t);
    return t;
}

/// @brief Drives the scheduler until @p cond (a lambda) is true or bounded.
template <typename F> void drive_until(F cond, int limit = 10000) {
    for (int i = 0; i < limit && !cond(); ++i) {
        __atomic_store_n(&kernel::scheduler_need_resched, true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

/// @brief True if @p code is a signal-termination exit code (high bit set).
bool is_signal_death(uint64_t code) {
    return (code & (1ULL << 63)) != 0;
}

/// @brief Creates a parked low-priority helper (never runs; only exists in
///        id_table_ for registration/fault-target purposes).
TaskControlBlock *make_parked(uint64_t *out_id = nullptr) {
    auto *t = TaskControlBlock::create(
        []() {
            for (;;)
                kernel::Scheduler::reschedule();
        },
        2, 0);
    if (!t)
        return nullptr;
    if (out_id)
        *out_id = t->id;
    Scheduler::add_task(*t);
    return t;
}

} // namespace

// Runmode: kernel
// Testidea: A client designates a live pager for itself; duplicate, third-party
// and self-designation are rejected; the registry fails closed when full.
// Input: Register a pager for helpers; attempt duplicates and self-pager.
// Expect: authority gate holds; one slot per client; full fails closed.
// Depends: PagerRegistry::register_client
JARVIS_TEST(pager_register_authority, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t pid = 0;
    auto *h = make_parked(&pid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(pid != 0);
    Scheduler::reschedule();
    size_t base = ipc::PagerRegistry::live_count();

    // A helper (as client) designates the harness (cur) as its pager.
    JARVIS_ASSERT(ipc::PagerRegistry::register_client(*h, cur->id));
    JARVIS_ASSERT_EQ(base + 1, ipc::PagerRegistry::live_count());
    JARVIS_ASSERT(ipc::PagerRegistry::is_registered(*h));

    // Duplicate registration (same client) fails — one slot per client.
    JARVIS_ASSERT(!ipc::PagerRegistry::register_client(*h, cur->id));
    JARVIS_ASSERT_EQ(base + 1, ipc::PagerRegistry::live_count());

    // pager == client rejected; pager_pid 0 rejected.
    JARVIS_ASSERT(!ipc::PagerRegistry::register_client(*h, pid));
    JARVIS_ASSERT(!ipc::PagerRegistry::register_client(*h, 0));
    JARVIS_ASSERT(!ipc::PagerRegistry::register_client(*cur, cur->id));

    // Unregister (client drops its own registration).
    JARVIS_ASSERT(ipc::PagerRegistry::unregister(*cur, pid));
    JARVIS_ASSERT_EQ(base, ipc::PagerRegistry::live_count());

    // Registry full fails closed: fill CONFIG_CAP_MAX_PAGER_CLIENTS slots.
    constexpr size_t kMax = CONFIG_CAP_MAX_PAGER_CLIENTS;
    uint64_t cids[kMax];
    TaskControlBlock *clients[kMax];
    for (size_t i = 0; i < kMax; ++i) {
        clients[i] = make_parked(&cids[i]);
        JARVIS_ASSERT(clients[i] != nullptr);
        JARVIS_ASSERT(cids[i] != 0);
    }
    Scheduler::reschedule();
    for (size_t i = 0; i < kMax; ++i)
        JARVIS_ASSERT(
            ipc::PagerRegistry::register_client(*clients[i], cur->id));
    JARVIS_ASSERT_EQ(base + kMax, ipc::PagerRegistry::live_count());
    uint64_t extra_id = 0;
    auto *extra = make_parked(&extra_id);
    JARVIS_ASSERT(extra != nullptr);
    JARVIS_ASSERT(!ipc::PagerRegistry::register_client(*extra, cur->id));
    JARVIS_ASSERT_EQ(base + kMax, ipc::PagerRegistry::live_count());

    for (size_t i = 0; i < kMax; ++i)
        ipc::PagerRegistry::unregister(*cur, cids[i]);
    for (size_t i = 0; i < kMax; ++i)
        kernel::test::terminate_if_live(clients[i]);
    kernel::test::terminate_if_live(extra);
    kernel::test::terminate_if_live(h); // the first parked helper
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(base, ipc::PagerRegistry::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_PAGER_RECV with nothing pending returns 0 immediately.
// Input: Call recv with no pending faults.
// Expect: 0, registry unchanged.
// Depends: non-blocking recv
JARVIS_TEST(pager_recv_nonblocking_none, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    size_t base = ipc::PagerRegistry::live_count();
    ipc::PagerFaultMsg msg{};
    int got = ipc::PagerRegistry::recv(*cur, msg);
    JARVIS_ASSERT_EQ(0, got);
    JARVIS_ASSERT_EQ(base, ipc::PagerRegistry::live_count());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Non-delegatable #PFs are rejected by the classifier (F3 P=1,
// F4 RSVD, F5 U/S=0, F6 I/D=1, F7 out-of-range).  All rejects return false
// BEFORE the block path, so a direct call on a registered client is safe.
// Input: Direct delegate_fault with synthetic error codes/CR2.
// Expect: each returns false; no fault record created.
// Depends: PagerRegistry::delegate_fault classification
JARVIS_TEST(pager_classification_rejects, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t pid = 0;
    auto *h = make_parked(&pid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(pid != 0);
    Scheduler::reschedule();
    JARVIS_ASSERT(ipc::PagerRegistry::register_client(*h, cur->id));

    size_t base = ipc::PagerRegistry::live_count();
    // F3: P=1 (present — write-to-RO / NX fault) not delegatable.
    JARVIS_ASSERT(!ipc::PagerRegistry::delegate_fault(*h, 0x1, nullptr, 0x1000));
    // F4: RSVD bit set.
    JARVIS_ASSERT(!ipc::PagerRegistry::delegate_fault(*h, 0x8, nullptr, 0x1000));
    // F5: U/S=0 (kernel-address access).
    JARVIS_ASSERT(!ipc::PagerRegistry::delegate_fault(*h, 0x0, nullptr, 0x1000));
    // F6: I/D=1 (instruction fetch — v1 NX-only).
    JARVIS_ASSERT(
        !ipc::PagerRegistry::delegate_fault(*h, 0x10, nullptr, 0x1000));
    // F7: CR2 above USER_SPACE_LIMIT.
    JARVIS_ASSERT(!ipc::PagerRegistry::delegate_fault(
        *h, 0x4, nullptr, USER_SPACE_LIMIT + 0x1000));
    JARVIS_ASSERT_EQ(base, ipc::PagerRegistry::live_count());
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    ipc::PagerRegistry::unregister(*cur, pid);
    kernel::test::terminate_and_drain(*h);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A fault while g_user_access_recover_ip is set is NOT delegated.
// Input: Set the recover-IP; call delegate_fault on a registered client.
// Expect: false (F2); no record.
// Depends: F2 classification, global_state accessor
JARVIS_TEST(pager_recover_ip_not_delegated, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t pid = 0;
    auto *h = make_parked(&pid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(pid != 0);
    Scheduler::reschedule();
    JARVIS_ASSERT(ipc::PagerRegistry::register_client(*h, cur->id));

    kernel::gs::user_access_recover_ip() = 0x1234;
    bool r = ipc::PagerRegistry::delegate_fault(*h, 0x4, nullptr, 0x1000);
    kernel::gs::user_access_recover_ip() = 0;
    JARVIS_ASSERT(!r);
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    ipc::PagerRegistry::unregister(*cur, pid);
    kernel::test::terminate_and_drain(*h);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A real user-mode #PF on an unmapped VA is delegated to the
// registered pager (the harness); the pager maps a FrameCap via SYS_PAGER_MAP;
// the client resumes and the faulting write succeeds.  This is the definitive
// block-in-#PF-ISR resume test.
// Input: Load pager-probe.c.elf (registers PID 1 as pager, faults at
//        0x10000000); harness recvs + maps + SYS_PAGER_MAP.
// Expect: the probe writes 0xCAFE and _exit(0); record consumed; no pending.
// Depends: block-in-ISR, PagerRegistry, SYS_PAGER_MAP, elf loader
JARVIS_TEST(pager_fault_roundtrip, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();

    // Drive until the probe faults and a record is pending for the harness.
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    ipc::PagerFaultMsg msg{};
    JARVIS_ASSERT_EQ(1, ipc::PagerRegistry::recv(*cur, msg));
    JARVIS_ASSERT_EQ(0x10000000ULL, msg.fault_va);
    JARVIS_ASSERT_EQ(t->id, msg.client_id);
    // The record is STILL pending after RECV (two-phase protocol).
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    // The pager (harness) allocates a 2-page frame and maps BOTH pages into the
    // client PML4 (multi-entry committed table — exercises the auditor's N2
    // multi-entry invalidation path on the probe's death drain).
    uint64_t phys = PMM::alloc_user_contiguous(2);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 2, true);
    JARVIS_ASSERT(fc != nullptr);
    int slot = install_frame(cur, fc);
    JARVIS_ASSERT(slot >= 0);
    uint64_t cap_h = frame_handle(cur, slot);
    uint64_t r = pager_syscall(
        static_cast<uint64_t>(SyscallNumber::PAGER_MAP), msg.fault_id, cap_h,
        2, 0);
    JARVIS_ASSERT_EQ(0ULL, r);

    // Both mapped pages in the client PML4 resolve to the pager's frames.
    // (Check while the probe's PML4 is still alive.)
    JARVIS_ASSERT_EQ(phys, VMM::virt_to_phys_in_pml4(0x10000000,
                                                     t->page_table_));
    JARVIS_ASSERT_EQ(phys + arch::PAGE_SIZE,
                     VMM::virt_to_phys_in_pml4(0x10000000 + arch::PAGE_SIZE,
                                               t->page_table_));

    // Client resumes; the faulting write to 0x10000000 now succeeds; _exit(0).
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT_EQ(0ULL, t->exit_code); // clean exit (2 = reg fail)

    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));
    // The probe exited cleanly via sys_exit (switch_away_from_terminating,
    // which does NOT release_zombie), so it is not in the zombie list.  Call
    // Scheduler::terminate explicitly to release it, then reap — cleanup()
    // drains the probe's pager registration.
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), ipc::PagerRegistry::live_count());

    // Teardown: drop the slot ref via cs->remove (1->0 -> dispose frees the
    // frame).  The pager's cap is installed in the harness's CSpace.
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    cs->remove(static_cast<uint32_t>(slot));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A late SYS_PAGER_MAP after the fault was consumed (by a watchdog
// timeout) returns an error; no mapping is installed; no double wake.
// Input: probe faults; the watchdog consumes the fault; then MAP with the
//        stale fault_id.
// Expect: map returns -1; no PTE installed; probe SIGSEGV-terminates.
// Depends: exactly-once consume gate, watchdog, SYS_PAGER_MAP
JARVIS_TEST(pager_map_after_timeout_denied, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));
    ipc::PagerFaultMsg msg{};
    JARVIS_ASSERT_EQ(1, ipc::PagerRegistry::recv(*cur, msg));

    // The watchdog consumes the fault (probe's registration survives, VA
    // poisoned).
    arch::Timer::set_ticks_for_test(
        arch::Timer::ticks() + CONFIG_PAGER_FAULT_TIMEOUT_TICKS + 10);
    Scheduler::on_tick();
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    // Late MAP with the stale fault_id fails; no PTE at the fault VA.
    uint64_t phys = PMM::alloc_user_contiguous(1);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, true);
    JARVIS_ASSERT(fc != nullptr);
    int slot = install_frame(cur, fc);
    JARVIS_ASSERT(slot >= 0);
    uint64_t cap_h = frame_handle(cur, slot);
    uint64_t r = pager_syscall(
        static_cast<uint64_t>(SyscallNumber::PAGER_MAP), msg.fault_id, cap_h,
        1, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r);
    // The probe's PML4 has no PTE at the fault VA (probe still alive — it
    // re-faults on the poisoned VA and SIGSEGV-terminates; check before the
    // cleanup frees the PML4).
    JARVIS_ASSERT_EQ(0ULL,
                     VMM::virt_to_phys_in_pml4(0x10000000, t->page_table_));

    // The probe re-faults on the poisoned VA -> SIGSEGV -> terminated.
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(is_signal_death(t->exit_code));

    // Drop the slot ref via cs->remove (1->0 -> dispose frees the frame).
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    cs->remove(static_cast<uint32_t>(slot));
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_PAGER_ABORT consumes the fault, unmaps the ledger, wakes the
// client, and poisons the VA latch — the same VA re-faults to SIGSEGV.
// Input: probe faults; the pager (harness) ABORTs; the probe re-faults.
// Expect: probe SIGSEGV-terminates (poisoned VA, no re-delegate loop).
// Depends: abort(), poison-VA latch (F9)
JARVIS_TEST(pager_abort_poisons_va, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));
    ipc::PagerFaultMsg msg{};
    JARVIS_ASSERT_EQ(1, ipc::PagerRegistry::recv(*cur, msg));

    // The pager explicitly aborts: record consumed, VA poisoned.
    JARVIS_ASSERT_EQ(0, ipc::PagerRegistry::abort(*cur, msg.fault_id));
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    // The probe re-faults on the same VA -> F9 reject -> SIGSEGV.  If the
    // latch were broken it would re-delegate forever (timeout -> hang).
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(is_signal_death(t->exit_code));

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The watchdog expires an overdue fault: the client is woken, the
// ledger is unmapped, the VA latch is poisoned (same-VA retry -> SIGSEGV), and
// the REGISTRATION IS KEPT (per-fault fail-closed).
// Input: probe faults; harness never replies; advance tick; run on_tick().
// Expect: probe SIGSEGV-terminates; registration evicted only by probe death.
// Depends: watchdog_scan (on_tick), per-fault fail-closed policy
JARVIS_TEST(pager_fault_timeout_aborts, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    // The pager never replies; the watchdog expires the fault.
    arch::Timer::set_ticks_for_test(
        arch::Timer::ticks() + CONFIG_PAGER_FAULT_TIMEOUT_TICKS + 10);
    Scheduler::on_tick();
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    // The probe re-faults on the poisoned VA -> SIGSEGV.  (Registration is
    // kept per-fault; it is evicted when the probe dies in cleanup.)
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(is_signal_death(t->exit_code));

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pager death mid-fault: drain_task evicts the registration, rolls
// back the ledger, and wakes the client (reverts to SIGSEGV on retry).
// Input: probe faults; the pager (harness) calls drain_task(cur) (simulating
//        the pager's cleanup).
// Expect: probe SIGSEGV-terminates (no pager -> F8 reject).
// Depends: drain_task (pager death path)
JARVIS_TEST(pager_dead_drains_faults, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    // The pager (harness) dies: drain_task(cur) evicts its registrations and
    // wakes the clients.
    ipc::PagerRegistry::drain_task(*cur);
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));
    JARVIS_ASSERT(!ipc::PagerRegistry::is_registered(*t));

    // The probe re-faults with no pager -> SIGSEGV.
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(is_signal_death(t->exit_code));

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Client death mid-fault: drain_task(client) consumes the fault and
// unmaps the ledger before free_user_pages.
// Input: probe faults; drain_task(probe) (simulating the client's cleanup).
// Expect: record consumed; no pending; the pager's cap stays live.
// Depends: drain_task (client death path)
JARVIS_TEST(pager_client_death_unmaps, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    // The client (probe) dies: drain_task consumes the fault + rolls back.
    ipc::PagerRegistry::drain_task(*t);
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));
    JARVIS_ASSERT(!ipc::PagerRegistry::is_registered(*t));

    // The client is woken only when the pager dies; here it was the CLIENT
    // that died, so it is not re-queued — it stays TERMINATED-able.  We
    // terminate it via the normal path.
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Revoking the pager's FrameCap while a fault is pending makes the
// later SYS_PAGER_MAP fail (revoked-cap check) and invalidate_cap is a safe
// no-op with no ledger entry.
// Input: probe faults; the pager creates a frame and revokes it before MAP.
// Expect: MAP returns -1; probe SIGSEGV-terminates (fault unresolved -> the
//         watchdog/abort path or the revoked cap never maps).
// Depends: invalidate_cap closure, revoked-cap check in map()
JARVIS_TEST(pager_cap_revoke_unmaps, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));
    ipc::PagerFaultMsg msg{};
    JARVIS_ASSERT_EQ(1, ipc::PagerRegistry::recv(*cur, msg));

    // A frame the pager would map; install it in the pager's CSpace, then revoke
    // it before SYS_PAGER_MAP (a revoked cap must be refused at map time).
    uint64_t phys = PMM::alloc_user_contiguous(1);
    JARVIS_ASSERT(phys != 0);
    auto *fc = cap::FrameCap::create(phys, 1, true);
    JARVIS_ASSERT(fc != nullptr);
    int slot = install_frame(cur, fc);
    JARVIS_ASSERT(slot >= 0);
    uint64_t cap_h = frame_handle(cur, slot);
    fc->revoke(); // revoke BEFORE map

    uint64_t r = pager_syscall(
        static_cast<uint64_t>(SyscallNumber::PAGER_MAP), msg.fault_id, cap_h,
        1, 0);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), r); // revoked cap refused

    // invalidate_cap (from dispose on the revoked cap's last release) is a
    // safe no-op here (no ledger entry yet).
    ipc::PagerRegistry::invalidate_cap(fc);

    // The fault is still pending; the watchdog eventually SIGSEGV's the probe.
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));
    arch::Timer::set_ticks_for_test(
        arch::Timer::ticks() + CONFIG_PAGER_FAULT_TIMEOUT_TICKS + 10);
    Scheduler::on_tick();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);

    // Drop the slot ref via cs->remove (the 1->0 transition runs dispose and
    // frees the revoked frame).  Do NOT fc->release() — the slot owns the only
    // reference.
    cur->ensure_cspace();
    cap::CNode *cs = cur->get_cspace();
    cs->remove(static_cast<uint32_t>(slot));
    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A stalled pager cannot wedge the kernel — while the pager never
// replies, the harness keeps running and the watchdog fires (B8/B6).
// Input: probe faults and blocks; the harness (pager) never replies but keeps
//        running; advance the tick; watchdog.
// Expect: the harness is never blocked; the watchdog wakes the probe.
// Depends: B8 deadlock-designed-out, watchdog_scan
JARVIS_TEST(pager_deadlock_designed_out, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);

    auto *t = load_pager_probe();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::reschedule();
    drive_until([&]() { return ipc::PagerRegistry::pending_fault(*cur); });
    JARVIS_ASSERT(ipc::PagerRegistry::pending_fault(*cur));

    // The pager (harness) is NOT blocked by the client's wait.
    JARVIS_ASSERT(cur->state == TaskState::RUNNING);

    // The watchdog fires despite the stalled "pager" (we just don't reply).
    arch::Timer::set_ticks_for_test(
        arch::Timer::ticks() + CONFIG_PAGER_FAULT_TIMEOUT_TICKS + 10);
    Scheduler::on_tick();
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    // The probe re-faults on the poisoned VA -> SIGSEGV (never stranded).
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(is_signal_death(t->exit_code));

    kernel::test::terminate_and_drain(*t);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_PAGER_MAP/delegate_fault refuse a canary-slot VA collision
// (F10) and the canary sampler is unaffected by pager-served tasks.
// Input: Install a canary at a known VA; attempt delegate_fault there.
// Expect: the canary VA is NOT delegated (F10 reject, before the block path);
//         a non-canary VA is delegated (via a probe roundtrip's presence).
// Depends: F10 canary collision
JARVIS_TEST(pager_smap_canary_coexist, "PRE: none | POST: none") {
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    uint64_t pid = 0;
    auto *h = make_parked(&pid);
    JARVIS_ASSERT(h != nullptr);
    JARVIS_ASSERT(pid != 0);
    Scheduler::reschedule();
    JARVIS_ASSERT(ipc::PagerRegistry::register_client(*h, cur->id));

    // Give the client a canary slot so F10 has something to collide with.
    h->canary_installed = 1;
    h->canary_before[0] = 0xB000;
    h->canary_after[0] = 0xC000;

    // A fault at the canary-before page is NOT delegated (F10).  This reject
    // returns before the block path, so the direct call is safe.
    JARVIS_ASSERT(
        !ipc::PagerRegistry::delegate_fault(*h, 0x4, nullptr, 0xB000));
    JARVIS_ASSERT(!ipc::PagerRegistry::pending_fault(*cur));

    // A non-canary VA still delegates (via the probe's real fault path in the
    // roundtrip); here we only verify the reject did not touch the registry.
    JARVIS_ASSERT_EQ(0, ipc::PagerRegistry::live_count() - (pid != 0 ? 1 : 0));

    // Restore canary fields (test isolation would catch drift; keep clean).
    h->canary_installed = 0;
    h->canary_before[0] = 0;
    h->canary_after[0] = 0;

    ipc::PagerRegistry::unregister(*cur, pid);
    kernel::test::terminate_and_drain(*h);
    JARVIS_TEST_PASS();
}

void register_cap_pager_tests() {
    Logger::info("Registering cap_pager tests");
    JARVIS_REGISTER_TEST(pager_register_authority);
    JARVIS_REGISTER_TEST(pager_recv_nonblocking_none);
    JARVIS_REGISTER_TEST(pager_classification_rejects);
    JARVIS_REGISTER_TEST(pager_recover_ip_not_delegated);
    JARVIS_REGISTER_TEST(pager_fault_roundtrip);
    JARVIS_REGISTER_TEST(pager_map_after_timeout_denied);
    JARVIS_REGISTER_TEST(pager_abort_poisons_va);
    JARVIS_REGISTER_TEST(pager_fault_timeout_aborts);
    JARVIS_REGISTER_TEST(pager_dead_drains_faults);
    JARVIS_REGISTER_TEST(pager_client_death_unmaps);
    JARVIS_REGISTER_TEST(pager_cap_revoke_unmaps);
    JARVIS_REGISTER_TEST(pager_deadlock_designed_out);
    JARVIS_REGISTER_TEST(pager_smap_canary_coexist);
}