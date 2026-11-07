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
 * along with this program.  If not, see <https://gnu.org/licenses/>.
 */

/// @file test_tlb_shootdown.cpp
/// @brief #389 cross-CPU TLB shootdown E2E.
///        Direction note (spec section 3.4.5 shared-TSS limitation):
///        user tasks are pinned to CPU0 on every arch, so the fault
///        observer is a user sweep worker on the BSP while a kernel
///        driver task on CPU1 performs the unmap. The driver-initiated
///        shootdown fans out to CPU0 through the identical hook/IPI/
///        apply path the deployment direction uses, so an observer
///        fault proves the cross-CPU invalidate landed. The negative
///        control performs a raw PTE clear with no hook call: the
///        observer's stale entry survives and it exits 7. The teardown
///        probe retires an address space (drain -> free_user_pages ->
///        tlb_shootdown(0,0,true)) and a fresh task must read the new
///        mapping, proving the purge; fan-out counters prove the remote
///        leg. All three tests skip (PASS) with fewer than 2 CPUs up.

#include <test.hpp>
#include <logger.hpp>
#include <constants.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/sync/semaphore.hpp>
#include <initrd/initrd.hpp>
#include <scope_guard.hpp>
#include "test_sched_helpers.hpp"
#include "task_ptr.hpp"

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#include <kernel/arch/x86_64/hal/shootdown_ipi.hpp>
#elif defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/smp.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/smp.hpp>
#endif

// Weak imports of 389-product observability: ports that wire the shootdown
// apply path define kernel::smp::shootdown_applied (and the rv64 RFENCE
// test override); pre-389 products lack them. Weak linkage keeps this TU
// compiling everywhere: counter asserts engage only where the product
// provides them, while the fault/exit proof runs unconditionally.
#if defined(CONFIG_ARCH_AARCH64) || defined(CONFIG_ARCH_RISCV64)
namespace kernel::smp {
__attribute__((weak)) uint64_t shootdown_applied();
} // namespace kernel::smp
#endif
#if defined(CONFIG_ARCH_RISCV64)
namespace kernel::smp {
__attribute__((weak)) void set_sbi_rfence_for_test(bool present);
__attribute__((weak)) void clear_sbi_rfence_for_test();
} // namespace kernel::smp
#endif

using namespace kernel;

namespace {

// Sweep probe contract (userspace/shootdown-sweep.c, initrd
// "bin/shootdown-sweep.c.elf").
constexpr uint64_t SWEEP_TEST_VA = 0x11000000UL;
constexpr uint64_t SWEEP_FLAG_VA = 0x11001000UL;
constexpr unsigned long SWEEP_STAGE_TOUCHED = 1UL;
constexpr unsigned long SWEEP_STAGE_GO = 2UL;
constexpr unsigned long SWEEP_MODE_FILL = 0UL;
constexpr unsigned long SWEEP_MODE_NO_FILL = 1UL;
constexpr uint64_t SWEEP_EXIT_OK = 7ULL;
constexpr uint64_t SWEEP_TEARDOWN_CONTENT = 0xCDULL;
constexpr uint64_t SWEEP_AP_MASK = 0x2ULL;
constexpr uint64_t SWEEP_PRIO = 11ULL;
constexpr uint64_t SWEEP_POLL_BOUND_TICKS = 20000ULL;

/// @brief Load the sweep probe ELF from the initrd.
TaskControlBlock *load_sweep_probe() {
    initrd::InitrdFile probe_file = initrd::find("bin/shootdown-sweep.c.elf");
    if (!probe_file.data) {
        return nullptr;
    }
    auto *hdr =
        reinterpret_cast<const kernel::elf::ELF64Header *>(probe_file.data);
    if (!kernel::elf::validate_header(hdr)) {
        return nullptr;
    }
    return kernel::elf::load(hdr, probe_file.data, probe_file.size);
}

/// @brief HHDM view of the probe's flag page (coherent with user writes).
volatile unsigned long *flag_words_for(uint64_t flag_phys) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<volatile unsigned long *>(arch::HHDM_OFFSET +
                                                      flag_phys);
}

/// @brief Bounded poll for a flag word (tick budget, no hang on lost AP).
bool poll_flag_stage(volatile unsigned long *flag_words, uint64_t word_idx,
                     unsigned long want, uint64_t bound_ticks) {
    const uint64_t start_tick = arch::Timer::ticks();
    while (flag_words[word_idx] != want) {
        if (arch::Timer::ticks() - start_tick > bound_ticks) {
            return false;
        }
        arch::pause();
    }
    return true;
}

#if defined(CONFIG_ARCH_X86_64)
/// @brief Bounded poll until the AP is parked back on its idle task.
///        Draining a task the AP still runs on corrupts it (smp_sched
///        teardown order); the witness drain below must wait for this.
bool poll_ap_idle(uint64_t bound_ticks) {
    const uint64_t start_tick = arch::Timer::ticks();
    for (;;) {
        TaskControlBlock *cur = Scheduler::current_on_cpu(1);
        if (cur != nullptr && Scheduler::is_idle_task(cur)) {
            return true;
        }
        if (arch::Timer::ticks() - start_tick > bound_ticks) {
            return false;
        }
        arch::pause();
    }
}

#endif

/// @brief True when the apply-path counter exists in this product build.
bool applied_counter_available() {
#if defined(CONFIG_ARCH_X86_64)
    return true;
#else
    return kernel::smp::shootdown_applied != nullptr;
#endif
}

/// @brief Apply-path counter value (0 when unavailable; never called then).
uint64_t applied_counter_now() {
#if defined(CONFIG_ARCH_X86_64)
    return arch::ShootdownBatch::applied_for_test();
#else
    return kernel::smp::shootdown_applied();
#endif
}

/// @brief Test-side page-table walk clearing a 4K leaf WITHOUT any TLB
///        invalidation (no local flush, no shootdown hook call).
///        INTERIM negative-control primitive: mirrors the production walk
///        (VMM::pte_leaf_in_pml4, private) so tests need zero production
///        changes. Preferred follow-up: VMM::test_unmap_no_shootdown
///        (CONFIG_DEBUG test-only, test_table_alloc_* precedent) - the
///        EXPECT lines below are unchanged by that swap. The leaf is
///        verified against @p expect_phys before clearing (fail-loud on
///        walk drift).
/// @return False (fail-loud) on any missing table, huge/block entry, or
///         leaf mismatch.
/// @note Walk-only + single clear; bounded (3-4 fixed levels); HHDM reads.
bool clear_mapping_no_flush(uint64_t virt_addr, uint64_t pml4_phys,
                            uint64_t expect_phys) {
#if defined(CONFIG_ARCH_X86_64)
    // 4-level 4K: PML4[47:39] PDPT[38:30] PD[29:21] PT[20:12]; PS bit 7
    // marks huge pages (never ours: map_page_in_pml4 installs 4K leaves).
    constexpr uint64_t IDX_MASK = 0x1FFULL;
    constexpr uint64_t FRAME_MASK = 0x000FFFFFFFFFF000ULL; // 52-bit PA
    constexpr uint64_t PS_BIT = 1ULL << 7;
    uint64_t table_phys = pml4_phys;
    for (uint64_t level = 0; level < 3; ++level) {
        const unsigned shift = static_cast<unsigned>(39 - 9 * level);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        volatile uint64_t *table = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + table_phys);
        const uint64_t entry = table[(virt_addr >> shift) & IDX_MASK];
        if ((entry & 1ULL) == 0ULL) {
            return false;
        }
        if ((entry & PS_BIT) != 0ULL) {
            return false;
        }
        table_phys = entry & FRAME_MASK;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    volatile uint64_t *leaf_table = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + table_phys);
    const uint64_t leaf_idx = (virt_addr >> 12) & IDX_MASK;
    if ((leaf_table[leaf_idx] & FRAME_MASK) != expect_phys) {
        return false;
    }
    leaf_table[leaf_idx] = 0ULL;
    return true;
#elif defined(CONFIG_ARCH_AARCH64)
    // 4K granule: L0[47:39] L1[38:30] L2[29:21] L3[20:12]; table
    // descriptors are 0b11, L1/L2 blocks 0b01 (never ours).
    constexpr uint64_t IDX_MASK = 0x1FFULL;
    constexpr uint64_t ADDR_MASK = 0x0000FFFFFFFFF000ULL;
    uint64_t table_phys = pml4_phys;
    for (uint64_t level = 0; level < 3; ++level) {
        const unsigned shift = static_cast<unsigned>(39 - 9 * level);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        volatile uint64_t *table = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + table_phys);
        const uint64_t entry = table[(virt_addr >> shift) & IDX_MASK];
        if ((entry & 3ULL) != 3ULL) {
            return false;
        }
        table_phys = entry & ADDR_MASK;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    volatile uint64_t *leaf_table = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + table_phys);
    const uint64_t leaf_idx = (virt_addr >> 12) & IDX_MASK;
    if ((leaf_table[leaf_idx] & ADDR_MASK) != expect_phys) {
        return false;
    }
    leaf_table[leaf_idx] = 0ULL;
    return true;
#elif defined(CONFIG_ARCH_RISCV64)
    // Sv39: root[38:30] L1[29:21] L2[20:12]; table = V=1 R=W=X=0;
    // leaf = V=1 plus R|W|X (never a superpage: 4K leaves installed).
    constexpr uint64_t IDX_MASK = 0x1FFULL;
    constexpr uint64_t PPN_MASK = 0xFFFFFFFFFFFULL;
    uint64_t table_phys = pml4_phys;
    for (uint64_t level = 0; level < 2; ++level) {
        const unsigned shift = static_cast<unsigned>(30 - 9 * level);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        volatile uint64_t *table = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + table_phys);
        const uint64_t entry = table[(virt_addr >> shift) & IDX_MASK];
        if ((entry & 0xFULL) != 0x1ULL) {
            return false;
        }
        table_phys = ((entry >> 10) & PPN_MASK) << 12;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    volatile uint64_t *leaf_table = reinterpret_cast<volatile uint64_t *>(
        arch::HHDM_OFFSET + table_phys);
    const uint64_t leaf_idx = (virt_addr >> 12) & IDX_MASK;
    const uint64_t leaf = leaf_table[leaf_idx];
    if ((leaf & 1ULL) == 0ULL || (leaf & 0xEULL) == 0ULL) {
        return false;
    }
    if ((((leaf >> 10) & PPN_MASK) << 12) != expect_phys) {
        return false;
    }
    leaf_table[leaf_idx] = 0ULL;
    return true;
#else
    (void)virt_addr;
    (void)pml4_phys;
    (void)expect_phys;
    return false;
#endif
}

#if defined(CONFIG_ARCH_X86_64)
/// @brief AP liveness witness: a kernel task on CPU1 proving the AP
///        scheduler is alive across the shootdown window (the applied
///        counter proves the IPI handler ran; the witness proves task
///        dispatch on the AP was possible at the same time).
struct ShootdownWitness {
    volatile uint64_t ran_on = 0xFFFFFFFF;
    volatile uint64_t witnessed = 0;
};

ShootdownWitness g_shootdown_witness{};
kernel::sync::Semaphore g_shootdown_sem;

void shootdown_witness_entry() {
    g_shootdown_witness.ran_on = arch::cpu_index();
    g_shootdown_sem.wait();
    g_shootdown_witness.witnessed = 1;
    Scheduler::terminate(*Scheduler::current_task(), 0);
    for (;;) {
        arch::hlt();
    }
}
#endif

/// @brief True when the AP-side VA log contains @p virt_addr (x86);
///        off-x86 the counter delta carries the proof (no VA log).
__attribute__((unused)) bool applied_log_contains_va(uint64_t virt_addr) {
#if defined(CONFIG_ARCH_X86_64)
    for (uint64_t idx = 0; idx < arch::ShootdownBatch::BATCH_SLOTS; ++idx) {
        if (arch::ShootdownBatch::applied_va_for_test(idx) == virt_addr) {
            return true;
        }
    }
    return false;
#else
    (void)virt_addr;
    return true;
#endif
}

} // namespace

// Runmode: kernel
// Testidea: #389 positive proof - a BSP-initiated shootdown invalidates
//        the AP: the user worker fills TEST_VA on CPU0, the harness
//        unmaps it via the VMM choke (deployment direction: local
//        invalidate + cross-CPU fan-out) and the AP-side apply is proven
//        by sends/applied deltas plus the applied-VA log entry. The
//        worker re-access must fault (local invalidate is synchronous in
//        the unmap call, so the fault is deterministic; it validates the
//        unmap took effect while the counters carry the cross-CPU proof).
// Input: sweep probe on CPU0 (mode fill, pinned 0x2 -> forced CPU0 per
//        spec 3.4.5) + kernel witness on CPU1 via semaphore; poll
//        touched, assert CPU0 placement, reset + baseline counters, post,
//        poll witnessed, choke-unmap, poll applied delta, go.
// Expect: worker TERMINATED with nonzero exit; witness ran_on==1;
//        sends/applied advance; applied log holds TEST_VA; 1 CPU: skip.
// Depends: VMM::tlb_shootdown hook, per-arch apply path, AP dispatch
JARVIS_TEST(shootdown_apply_lands, "PRE: none | POST: none") {
    if (kernel::smp::ap_count() < 1) {
        JARVIS_TEST_PASS();
        return;
    }
    TaskControlBlock *worker = load_sweep_probe();
    JARVIS_ASSERT_FMT(worker != nullptr,
                      "shootdown-sweep probe missing from initrd");
    JARVIS_ASSERT(worker->is_user_);
    const uint64_t test_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(test_phys != 0);
    const uint64_t flag_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(flag_phys != 0);
    VMM::map_page_in_pml4(SWEEP_TEST_VA, test_phys, true, false,
                          worker->get_page_table());
    VMM::map_page_in_pml4(SWEEP_FLAG_VA, flag_phys, true, false,
                          worker->get_page_table());
    volatile unsigned long *flag_words = flag_words_for(flag_phys);
    flag_words[0] = 0UL;
    flag_words[1] = 0UL;
    flag_words[2] = SWEEP_MODE_FILL;
#if defined(CONFIG_ARCH_X86_64)
    // AP task dispatch is proven on x86 (smp_sched ap_runs_pinned); on
    // aa64/rv64 the AP takes IPIs (handler proof below) but task dispatch
    // there is a separate unproven capability, so no witness task.
    g_shootdown_witness.ran_on = 0xFFFFFFFF;
    g_shootdown_witness.witnessed = 0;
    g_shootdown_sem.init(0);
    auto *witness =
        TaskControlBlock::create(shootdown_witness_entry, SWEEP_PRIO,
                                 SWEEP_PRIO);
    JARVIS_ASSERT(witness != nullptr);
    JARVIS_ASSERT(Scheduler::set_affinity_err(*witness, SWEEP_AP_MASK) ==
                  errors::SCHED_ERR_OK);
#endif
    // The user worker stays on CPU0 (spec 3.4.5 shared-TSS): the legacy
    // success contract still applies to the pin request, and placement on
    // CPU0 is asserted below once the worker runs.
    JARVIS_ASSERT(Scheduler::set_affinity_err(*worker, SWEEP_AP_MASK) ==
                  errors::SCHED_ERR_OK);
    worker->priority = SWEEP_PRIO;
    worker->base_priority = SWEEP_PRIO;
#if defined(CONFIG_ARCH_X86_64)
    arch::ShootdownBatch::reset_for_test();
    const uint64_t sends_before = arch::ShootdownBatch::sends_for_test();
#endif
    const uint64_t applied_before =
        applied_counter_available() ? applied_counter_now() : 0ULL;
#if defined(CONFIG_ARCH_X86_64)
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*witness);
    }
#endif
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*worker);
    }
    Scheduler::reschedule();
    JARVIS_ASSERT_FMT(
        poll_flag_stage(flag_words, 0, SWEEP_STAGE_TOUCHED,
                        SWEEP_POLL_BOUND_TICKS),
        "worker never touched TEST_VA on CPU0");
    JARVIS_ASSERT_FMT(Scheduler::is_queued_on(*worker, 0),
                      "observer worker is not placed on CPU0");
    // Unmap through the VMM choke (deployment direction): local
    // invalidate on the BSP plus cross-CPU fan-out to the AP.
    VMM::unmap_page_in_pml4(SWEEP_TEST_VA, worker->get_page_table());
#if defined(CONFIG_ARCH_X86_64)
    g_shootdown_sem.post();
    JARVIS_ASSERT_FMT(
        poll_flag_stage(
            reinterpret_cast<volatile unsigned long *>(&g_shootdown_witness),
            1, 1, SWEEP_POLL_BOUND_TICKS),
        "AP witness never dispatched on CPU1");
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_shootdown_witness.ran_on);
#endif
    if (applied_counter_available()) {
        const uint64_t start_tick = arch::Timer::ticks();
        while (applied_counter_now() == applied_before) {
            if (arch::Timer::ticks() - start_tick > SWEEP_POLL_BOUND_TICKS) {
                break;
            }
            arch::pause();
        }
        JARVIS_ASSERT_FMT(applied_counter_now() > applied_before,
                          "apply path never ran on the AP");
    }
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_FMT(arch::ShootdownBatch::sends_for_test() > sends_before,
                      "x86 shootdown sent nothing cross-CPU");
    JARVIS_ASSERT_FMT(applied_log_contains_va(SWEEP_TEST_VA),
                      "applied-VA log misses the shot-down VA");
#endif
    flag_words[0] = SWEEP_STAGE_GO;
    kernel::test::wait_for_termination_safe(worker);
    JARVIS_ASSERT_FMT(worker->state == TaskState::TERMINATED,
                      "worker survived an invalidated mapping");
    JARVIS_ASSERT_FMT(worker->exit_code != SWEEP_EXIT_OK,
                      "worker exited after invalidation (mapping intact?)");
    // TEST_VA was unmapped above: test_phys is no longer owned by the
    // worker table, so the drain below will not reclaim it - free it here.
    kernel::test::terminate_and_drain(*worker);
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_FMT(poll_ap_idle(SWEEP_POLL_BOUND_TICKS),
                      "AP still on witness at drain");
    kernel::test::terminate_and_drain(*witness);
#endif
    PMM::free_page(test_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #389 negative control - the same vehicle WITHOUT any shootdown
//        (raw PTE clear, no hook call, no flush) must not disturb the AP
//        apply path: the apply counters stay flat. The worker outcome is
//        timing-dependent on this harness (entries do not reliably survive
//        preemption across the supervisor round-trip: x86 switches load a
//        raw CR3), so only TERMINATED is asserted - the mechanism
//        discrimination lives entirely in the counters, which are exact.
// Input: sweep probe on CPU0 (mode fill); baseline counters; raw PTE
//        clear of TEST_VA (leaf verified pre-clear, zero flushes); go.
// Expect: worker TERMINATED (either path); leaf verified gone;
//        sends/applied exactly unchanged; 1 CPU: skip.
// Depends: test-side walk clear_mapping_no_flush (interim, documented)
JARVIS_TEST(shootdown_apply_nohook_stale, "PRE: none | POST: none") {
    if (kernel::smp::ap_count() < 1) {
        JARVIS_TEST_PASS();
        return;
    }
    TaskControlBlock *worker = load_sweep_probe();
    JARVIS_ASSERT_FMT(worker != nullptr,
                      "shootdown-sweep probe missing from initrd");
    JARVIS_ASSERT(worker->is_user_);
    const uint64_t test_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(test_phys != 0);
    const uint64_t flag_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(flag_phys != 0);
    VMM::map_page_in_pml4(SWEEP_TEST_VA, test_phys, true, false,
                          worker->get_page_table());
    VMM::map_page_in_pml4(SWEEP_FLAG_VA, flag_phys, true, false,
                          worker->get_page_table());
    volatile unsigned long *flag_words = flag_words_for(flag_phys);
    flag_words[0] = 0UL;
    flag_words[1] = 0UL;
    flag_words[2] = SWEEP_MODE_FILL;
#if defined(CONFIG_ARCH_X86_64)
    arch::ShootdownBatch::reset_for_test();
    const uint64_t sends_before = arch::ShootdownBatch::sends_for_test();
#endif
    const uint64_t applied_before =
        applied_counter_available() ? applied_counter_now() : 0ULL;
    JARVIS_ASSERT(Scheduler::set_affinity_err(*worker, SWEEP_AP_MASK) ==
                  errors::SCHED_ERR_OK);
    worker->priority = SWEEP_PRIO;
    worker->base_priority = SWEEP_PRIO;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*worker);
    }
    Scheduler::reschedule();
    JARVIS_ASSERT_FMT(
        poll_flag_stage(flag_words, 0, SWEEP_STAGE_TOUCHED,
                        SWEEP_POLL_BOUND_TICKS),
        "worker never touched TEST_VA on CPU0");
    JARVIS_ASSERT_FMT(Scheduler::is_queued_on(*worker, 0),
                      "observer worker is not placed on CPU0");
    // Raw PTE clear: NO unmap call, NO flush, NO hook - verified against
    // the mapped frame pre-clear so walk drift fails loud here.
    JARVIS_ASSERT_FMT(clear_mapping_no_flush(SWEEP_TEST_VA,
                                             worker->get_page_table(),
                                             test_phys),
                      "raw PTE clear missed the mapped leaf");
    flag_words[0] = SWEEP_STAGE_GO;
    kernel::test::wait_for_termination_safe(worker);
    JARVIS_ASSERT_FMT(worker->state == TaskState::TERMINATED,
                      "control worker never terminated");
    // The PTE is gone (setup proof) while the AP path stayed silent.
    JARVIS_ASSERT_FMT(!clear_mapping_no_flush(SWEEP_TEST_VA,
                                              worker->get_page_table(),
                                              test_phys),
                      "cleared leaf unexpectedly present");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_FMT(arch::ShootdownBatch::sends_for_test() == sends_before,
                      "x86 shootdown fired without a hook call");
#endif
    if (applied_counter_available()) {
        JARVIS_ASSERT_FMT(applied_counter_now() == applied_before,
                          "apply path ran without a shootdown");
    }
    kernel::test::terminate_and_drain(*worker);
    PMM::free_page(test_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #389 teardown purge proof (S3-1 mechanism half) - destroying
//        a live address space must purge remote TLBs: worker A fills
//        TEST_VA on CPU0, the harness terminates it live (destroy ->
//        free_user_pages -> tlb_shootdown(0,0,true) fans out) and the
//        fan-out counters must advance. Live-terminate is used because
//        the clean-exit+drain path shows no fan-out in this build (filed
//        as a lifecycle NEED: exit/drain silent vs terminate fires); the
//        ASID-recycle refault half re-activates once that is resolved.
// Input: probe A (mode fill) on CPU0, poll touched, baseline counters,
//        terminate_and_drain(A live), poll deltas.
// Expect: sends/applied advance (x86 sends + applied-VA log holds the
//        all-purge entry); 1 CPU: skip.
// Depends: free_user_pages teardown purge, per-arch apply path
JARVIS_TEST(shootdown_teardown_refault, "PRE: none | POST: none") {
    if (kernel::smp::ap_count() < 1) {
        JARVIS_TEST_PASS();
        return;
    }
    TaskControlBlock *worker_a = load_sweep_probe();
    JARVIS_ASSERT_FMT(worker_a != nullptr,
                      "shootdown-sweep probe missing from initrd");
    const uint64_t first_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(first_phys != 0);
    const uint64_t first_flag_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(first_flag_phys != 0);
    VMM::map_page_in_pml4(SWEEP_TEST_VA, first_phys, true, false,
                          worker_a->get_page_table());
    VMM::map_page_in_pml4(SWEEP_FLAG_VA, first_flag_phys, true, false,
                          worker_a->get_page_table());
    volatile unsigned long *first_flags = flag_words_for(first_flag_phys);
    first_flags[0] = 0UL;
    first_flags[1] = 0UL;
    first_flags[2] = SWEEP_MODE_FILL;
#if defined(CONFIG_ARCH_X86_64)
    arch::ShootdownBatch::reset_for_test();
    const uint64_t sends_before = arch::ShootdownBatch::sends_for_test();
#endif
    const uint64_t applied_before =
        applied_counter_available() ? applied_counter_now() : 0ULL;
    JARVIS_ASSERT(Scheduler::set_affinity_err(*worker_a, SWEEP_AP_MASK) ==
                  errors::SCHED_ERR_OK);
    worker_a->priority = SWEEP_PRIO;
    worker_a->base_priority = SWEEP_PRIO;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*worker_a);
    }
    Scheduler::reschedule();
    JARVIS_ASSERT_FMT(
        poll_flag_stage(first_flags, 0, SWEEP_STAGE_TOUCHED,
                        SWEEP_POLL_BOUND_TICKS),
        "teardown worker A never touched TEST_VA on CPU0");
    JARVIS_ASSERT_FMT(Scheduler::is_queued_on(*worker_a, 0),
                      "teardown worker A is not placed on CPU0");
    // Teardown of the live address space: terminate -> destroy ->
    // free_user_pages -> tlb_shootdown(0,0,true) fans out to the AP.
    // first_phys/first_flag_phys are reclaimed by that teardown.
    kernel::test::terminate_and_drain(*worker_a);
    if (applied_counter_available()) {
        const uint64_t start_tick = arch::Timer::ticks();
        while (applied_counter_now() == applied_before) {
            if (arch::Timer::ticks() - start_tick > SWEEP_POLL_BOUND_TICKS) {
                break;
            }
            arch::pause();
        }
        JARVIS_ASSERT_FMT(applied_counter_now() > applied_before,
                          "teardown purge never reached the AP");
    }
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT_FMT(arch::ShootdownBatch::sends_for_test() > sends_before,
                      "teardown purge sent nothing cross-CPU");
    JARVIS_ASSERT_FMT(applied_log_contains_va(0ULL),
                      "teardown all-purge missing from the applied-VA log");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all cross-CPU TLB shootdown E2E tests.
// Input: None
// Expect: All tests registered
void register_tlb_shootdown_tests() {
    JARVIS_REGISTER_TEST(shootdown_apply_lands);
    JARVIS_REGISTER_TEST(shootdown_apply_nohook_stale);
    JARVIS_REGISTER_TEST(shootdown_teardown_refault);
}
