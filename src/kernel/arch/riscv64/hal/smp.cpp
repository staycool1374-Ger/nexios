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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file smp.cpp
/// @brief RISC-V64 SMP AP bring-up (issue #351, slice-3).  The BSP walks
///        the DTB hart nodes (slice-1 helpers), probes SBI HSM, then wakes
///        each secondary with HSM hart_start (the MMU-off ap_entry stub as
///        start_addr, logical id as opaque).  No spin-table or CLINT
///        fallback exists: absent HSM support fails closed.  IPIs use the
///        legacy SBI hart-mask ecall (C2); the single S-mode software
///        interrupt fans out via a per-hart mailbox (reschedule /
///        shootdown / loopback).  TLB shootdown applies per-batch-entry
///        SFENCE.VMA with a bounded full-fence fallback.  Woken APs run
///        ap_main() and idle with timer ticks live (Phase C scheduling
///        lands in slice-4/5).  Runs once at boot under interrupt guard;
///        any failure parks with zero APs or panics naming the hart ID —
///        never half-woken.  Single-hart topologies are a silent no-op.
///        RISC-V-only.

#include <kernel/arch/riscv64/hal/smp.hpp>

#include <kernel/arch/riscv64/hal/percpu.hpp>
#include <kernel/arch/riscv64/hal/plic.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/boot/bootinfo.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/core/global_state.hpp>
#include <assert.hpp>
#include <logger.hpp>

/// @brief Per-AP HSM handoff block (see ap_entry.S).  Lives in a PMM page
/// (PA native, no translation needed); the BSP passes its PA as the HSM
/// opaque argument.  All HH addresses are link-time constants, valid once
/// the AP installs the provided root (which covers the whole kernel).
/// Layout matches the stub's fixed offsets — keep in sync with ap_entry.S.
struct ApParam {
    uint64_t satp;       // +0: live kernel root (MODE|PPN, BSP-read)
    uint64_t stack_pa;   // +8: AP stack top, PA (MMU-off SP, unused post-MMU)
    uint64_t stack_hh;   // +16: AP stack top, HH VA (post-MMU SP)
    uint64_t percpu_hh;  // +24: per_cpu HH base (&per_cpu[0])
    uint64_t ap_main_hh; // +32: ap_main HH entry
    uint64_t logical;    // +40: logical id (echo-checked by the stub)
    uint64_t hart;       // +48: hartid (echo, diagnostic only)
};
static_assert(sizeof(ApParam) == 56, "ApParam layout is stub-contracted");

/// @brief AP entry PA slot (linker-computed absolute address, see
/// linker_riscv64.ld: QUAD(rv64_ap_entry) resolved at link time).
extern const uint64_t rv64_ap_entry_pa_slot[];

namespace kernel::smp {

/// @brief AP entry PA (linker-computed absolute address, see the
/// rv64_ap_entry_pa_slot QUAD in linker_riscv64.ld: VMA==LMA==PA for the
/// low .boot region, so the absolute value IS the physical address
/// firmware needs). Fail-closed (0) when the slot is out of range.
inline uint64_t rv64_ap_entry_pa() {
    const uint64_t addr = rv64_ap_entry_pa_slot[0];
    if (addr == 0U || addr >= 0x100000000ULL) {
        return 0;
    }
    return addr;
}

namespace {

// Start-gate flag: BSP publishes (RELEASE) after bring_up() finishes
// parking verified APs; APs spin (ACQUIRE + pause) until it lands.
bool scheduler_ready_ = false;

// Per-logical hartid (DTB reg value, the HSM target). BSP-owned; APs never
// read it (theirs arrives via the HSM a0/arch register + param echo).
uint64_t s_ap_hart[CONFIG_MAX_CPUS] = {};

// Per-logical AP param-block PA (PMM page; 0 = unwoken). BSP-owned.
uint64_t s_ap_param_pa[CONFIG_MAX_CPUS] = {};

// Set by ap_main() after per-hart + PLIC + timer init; polled by the BSP.
volatile uint8_t ap_ready[CONFIG_MAX_CPUS] = {};

// BSP-observed parked-AP count (published at the end of bring_up()).
uint8_t g_aps_up = 0;

// Per-hart IPI mailbox (BSP-written, AP-drained; C=0 not required here —
// RISC-V TCG is coherent and all accesses are explicit atomics anyway).
volatile uint64_t ipi_mailbox_[CONFIG_MAX_CPUS] = {};

// SGI-style loopback acks, one slot per logical hart (BSP polls).
volatile uint64_t s_ipi_ack[CONFIG_MAX_CPUS] = {};

// TLB-shootdown batch state (mirrors the x86 ShootdownBatch contract in
// miniature: per-target slots, BSP publishes count, AP drains + applies).
struct ShootSlot {
    static constexpr uint64_t SLOTS = 16;
    uint64_t addrs[SLOTS];
    uint64_t count;
};
ShootSlot shoot_slots_[CONFIG_MAX_CPUS] = {};
uint64_t shoot_applied_ = 0; // Test readback: entries applied by AP ISRs.

/// @brief SBI v0.2+ call result (sbiret: error in a0, value in a1).
struct SbiRet {
    uint64_t error;
    uint64_t value;
};

/// @brief SBI v0.2+ call: EID in a7, FID in a6, args in a0-a2.
SbiRet sbi_call(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1,
                uint64_t arg2) {
    register uint64_t reg_a0 asm("a0") = arg0;
    register uint64_t reg_a1 asm("a1") = arg1;
    register uint64_t reg_a2 asm("a2") = arg2;
    register uint64_t reg_a6 asm("a6") = fid;
    register uint64_t reg_a7 asm("a7") = eid;
    asm volatile("ecall"
                 : "+r"(reg_a0), "+r"(reg_a1), "+r"(reg_a2)
                 : "r"(reg_a6), "r"(reg_a7)
                 : "memory");
    SbiRet ret = {reg_a0, reg_a1};
    return ret;
}

/// @brief Legacy (v0.1) SBI call: function ID in a7, args in a0-a1.
/// Only used for send_ipi (C2: present in QEMU virt OpenSBI by contract).
/// a1 is inout: OpenSBI normalizes every SBI return to sbiret (error in
/// a0, value in a1), so a caller-kept live value in a1 would be
/// corrupted otherwise (observed as a wild post-ecall load fault).
uint64_t sbi_legacy(uint64_t func, uint64_t arg0) {
    register uint64_t reg_a0 asm("a0") = arg0;
    register uint64_t reg_a1 asm("a1") = 0;
    register uint64_t reg_a7 asm("a7") = func;
    asm volatile("ecall" : "+r"(reg_a0), "+r"(reg_a1) : "r"(reg_a7) : "memory");
    (void)reg_a1; // Legacy value unused; the inout keeps it coherent.
    return reg_a0; // 0 on success (SBI_SUCCESS), nonzero = refused/failed.
}

/// @brief Probe an SBI extension (base EID 0x10, FID 3).
/// @return true when the probe ran AND reports the extension present.
bool sbi_probe_extension(uint64_t ext_id) {
    SbiRet probe = sbi_call(SBI_EID_BASE, SBI_BASE_PROBE_EXTENSION, ext_id,
                            0, 0);
    if (probe.error != 0) {
        return false; // No base-probe ABI (pre-v0.2 SBI): unprovable.
    }
    return probe.value != 0U;
}

/// @brief Whether the v0.2 IPI extension probed present (cached).
/// Unknown until first probe (-1), then 1/0. Probing runs once at boot.
int64_t s_ipi_ext_present = -1;

/// @brief Send an IPI to a hart mask (probed v0.2 path preferred).
/// Tries the v0.2 IPI extension (explicit mask base, probed once); falls
/// back to the legacy hart-mask ecall (C2: present in QEMU virt OpenSBI).
/// @return true when the SBI call was accepted, false otherwise.
bool sbi_send_ipi_mask(uint64_t hart_mask) {
    if (s_ipi_ext_present < 0) {
        s_ipi_ext_present = sbi_probe_extension(SBI_EID_IPI) ? 1 : 0;
    }
    if (s_ipi_ext_present > 0) {
        SbiRet ret = sbi_call(SBI_EID_IPI, SBI_IPI_SEND_IPI, hart_mask, 0, 0);
        if (ret.error == 0U) {
            return true;
        }
        // Probed present but the send failed: do NOT silently fall back
        // to an unprobed path mid-flight; report the failure upstream.
        return false;
    }
    return sbi_legacy(SBI_LEGACY_SEND_IPI, hart_mask) == 0U;
}

} // namespace

uint8_t ap_count() {
    return g_aps_up;
}

void publish_scheduler_ready() {
    __atomic_store_n(&scheduler_ready_, true, __ATOMIC_RELEASE);
}

bool send_resched_ipi(uint64_t logical) {
    if (logical == 0 || logical >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint64_t hart = s_ap_hart[logical];
    if (hart >= 64U) {
        return false; // Legacy hart-mask is XLEN bits; no silent narrow.
    }
    __atomic_fetch_or(&ipi_mailbox_[logical], SMP_IPI_RESCHED,
                      __ATOMIC_RELEASE);
    return sbi_send_ipi_mask(1ULL << hart);
}

bool send_shootdown_ipi(uint64_t logical) {
    if (logical == 0 || logical >= CONFIG_MAX_CPUS) {
        return false;
    }
    const uint64_t hart = s_ap_hart[logical];
    if (hart >= 64U) {
        return false; // Legacy hart-mask is XLEN bits; no silent narrow.
    }
    __atomic_fetch_or(&ipi_mailbox_[logical], SMP_IPI_SHOOTDOWN,
                      __ATOMIC_RELEASE);
    return sbi_send_ipi_mask(1ULL << hart);
}

bool queue_shootdown(uint64_t logical, uint64_t addr) {
    if (logical == 0 || logical >= CONFIG_MAX_CPUS || addr == 0U) {
        return false;
    }
    ShootSlot &slot = shoot_slots_[logical];
    if (slot.count >= ShootSlot::SLOTS) {
        return false; // Full: caller flushes first, then retries.
    }
    slot.addrs[slot.count] = addr;
    ++slot.count;
    return true;
}

uint64_t flush_shootdowns(uint64_t logical) {
    if (logical == 0 || logical >= CONFIG_MAX_CPUS) {
        return 0;
    }
    ShootSlot &slot = shoot_slots_[logical];
    if (slot.count == 0U) {
        return 0; // Nothing queued: no IPI.
    }
    __atomic_fetch_or(&ipi_mailbox_[logical], SMP_IPI_SHOOTDOWN,
                      __ATOMIC_RELEASE);
    const uint64_t hart = s_ap_hart[logical];
    if (hart >= 64U) {
        return 0; // Legacy hart-mask is XLEN bits; no silent narrow.
    }
    return sbi_send_ipi_mask(1ULL << hart) ? 1U : 0U;
}

void apply_shootdowns() {
    const uint64_t here = arch::cpu_index() % CONFIG_MAX_CPUS;
    ShootSlot &slot = shoot_slots_[here];
    uint64_t count = __atomic_load_n(&slot.count, __ATOMIC_ACQUIRE);
    if (count > ShootSlot::SLOTS) {
        // Corrupt count (never written by the bounded BSP path): full
        // fence once, drop the batch — fail-closed and bounded.
        arch::sfence_vma();
        __atomic_store_n(&slot.count, 0, __ATOMIC_RELEASE);
        return;
    }
    for (uint64_t idx = 0; idx < count; ++idx) {
        // Per-entry range form (rs1 = VA): bounded, no blanket invalidation.
        arch::sfence_vma_va(slot.addrs[idx]);
        ++shoot_applied_;
    }
    __atomic_store_n(&slot.count, 0, __ATOMIC_RELEASE);
}

uint64_t shootdown_applied() {
    return __atomic_load_n(&shoot_applied_, __ATOMIC_ACQUIRE);
}

void handle_sswi_ipi() {
    const uint64_t here = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t bits =
        __atomic_exchange_n(&ipi_mailbox_[here], 0, __ATOMIC_ACQUIRE);
    if ((bits & SMP_IPI_LOOPBACK) != 0U) {
        __atomic_store_n(&s_ipi_ack[here], 1, __ATOMIC_RELEASE);
    }
    if ((bits & SMP_IPI_SHOOTDOWN) != 0U) {
        apply_shootdowns();
    }
    if ((bits & SMP_IPI_RESCHED) != 0U) {
        kernel::Scheduler::sched_ipi_handler();
    }
}

void bring_up() {
    arch::IrqGuard guard{}; // boot is not yet preemptible-safe: no ticks here
    const void *dtb = kernel::boot::dtb_from_boot_info();
    if (dtb == nullptr) {
        // No DTB: topology unprovable — warn and park (BSP-only behavior).
        // OpenSBI always passes a live DTB; a missing one means firmware
        // skew, never a guessable topology.
        kernel::Logger::warn("SMP: no DTB — parking with 0 APs");
        return;
    }
    kernel::boot::DtbCpuEntry cpu_entries[CONFIG_MAX_CPUS];
    for (uint32_t clear_idx = 0; clear_idx < CONFIG_MAX_CPUS; ++clear_idx) {
        cpu_entries[clear_idx].id = 0;
        cpu_entries[clear_idx].method = kernel::boot::CpuEnableMethod::UNKNOWN;
        cpu_entries[clear_idx].release_addr = 0;
    }
    const int cpu_count =
        kernel::boot::dtb_walk_cpus(dtb, cpu_entries, CONFIG_MAX_CPUS);
    if (cpu_count <= 0) {
        // Untrusted topology (fail-closed walk): never boot a guess.
        kernel::Logger::warn("SMP: DTB cpu walk untrusted — parking");
        return;
    }
    if (cpu_count <= 1) {
        return; // single-hart: silent no-op, BSP-only behavior
    }

    // HSM probe (spec §5 risk-a + C2): no HSM => no bring-up primitive.
    // There is no spin-table or CLINT fallback by design (silent fallbacks
    // strand secondaries in firmware holds with no kernel supervision).
    if (!sbi_probe_extension(SBI_EID_HSM)) {
        kernel::Logger::warn("SMP: SBI HSM absent — parking with 0 APs");
        return;
    }

    const uint64_t bsp_hart =
        static_cast<uint64_t>(kernel::gs::boot_info().hart_id);
    // HSM start_addr must be the MMU-off PA: materialize it with absolute
    // %hi/%lo relocations (PC-relative forms cannot span the high-VMA to
    // low-VMA distance and truncate at link). Fail closed on nonsense.
    const uint64_t entry_pa = rv64_ap_entry_pa();
    if (entry_pa == 0U) {
        // The HSM start_addr must be the MMU-off PA; a zero/out-of-range
        // slot means the linker contract broke — fail closed, never boot
        // a secondary into unmapped space.
        panic("SMP: AP entry address unusable");
    }
    // Live kernel root (VMM tables): the AP installs the identical regime
    // the BSP runs on, so it lands in the same address space (boot tables
    // stay untouched — they belong to the early-boot window only).
    uint64_t live_satp = 0;
    asm volatile("csrr %0, satp" : "=r"(live_satp));

    uint8_t logical = 1;
    for (int cpu_idx = 0; cpu_idx < cpu_count; ++cpu_idx) {
        if (logical >= CONFIG_MAX_CPUS) {
            break;
        }
        const uint64_t cpu_hart = cpu_entries[cpu_idx].id;
        if (cpu_hart == bsp_hart) {
            continue; // never re-enter the BSP
        }
        // Per-AP handoff block + 4 KiB stack, both PMM-owned (PA native —
        // no VA translation needed; the HHDM alias covers post-MMU use).
        // Permanent boot allocation, like the x86 trampoline reservation.
        const uint64_t param_pa = kernel::PMM::alloc_page();
        const uint64_t stack_pa = kernel::PMM::alloc_page();
        if (param_pa == 0U || stack_pa == 0U) {
            kernel::Logger::warn("SMP: AP handoff OOM — parking");
            return;
        }
        auto *param = reinterpret_cast<volatile ApParam *>(
            arch::HHDM_OFFSET + param_pa);
        param->satp = live_satp;
        param->stack_pa = stack_pa + arch::PAGE_SIZE;
        param->stack_hh = arch::HHDM_OFFSET + stack_pa + arch::PAGE_SIZE;
        param->percpu_hh = reinterpret_cast<uint64_t>(&arch::per_cpu[0]);
        param->ap_main_hh = reinterpret_cast<uint64_t>(&ap_main);
        param->logical = logical;
        param->hart = cpu_hart;
        asm volatile("fence iorw, iorw" : : : "memory");
        s_ap_hart[logical] = cpu_hart;
        s_ap_param_pa[logical] = param_pa;
        ap_ready[logical] = 0;
        __atomic_store_n(&s_ipi_ack[logical], 0, __ATOMIC_RELEASE);

        // HSM start (hartid, entry PA, opaque = param-block PA).  A refusal
        // (firmware cannot start this hart) parks with zero APs — the
        // live DTB lists only real harts, so any refusal is a fault,
        // never an expected absence.
        SbiRet start_ret = sbi_call(SBI_EID_HSM, SBI_HSM_HART_START,
                                    cpu_hart, entry_pa, param_pa);
        if (start_ret.error != 0U) {
            kernel::Logger::warn("SMP: HSM hart_start refused — parking");
            return;
        }

        // Rendezvous: the AP sets ap_ready[logical] at the end of its
        // init; 1 s of time-counter, then a controlled panic naming the
        // hart (mirrors the x86 LAPIC-ID panic contract).
        const uint64_t counter_freq = arch::Timer::raw_counter_freq_hz();
        const uint64_t wait_start = arch::Timer::raw_counter();
        bool ap_up = false;
        for (;;) {
            if (ap_ready[logical] != 0) {
                ap_up = true;
                break;
            }
            if (counter_freq != 0 &&
                arch::Timer::raw_counter() - wait_start >= counter_freq) {
                break;
            }
            arch::pause();
        }
        if (!ap_up) {
            kernel::Logger::warn("SMP: AP failed to start (hart ID ");
            kernel::Logger::warn("— see DTB cpu walk for the ID list");
            panic("SMP: AP start timeout");
        }
        // Idle task created only after the AP proved alive (same
        // no-leak ordering as the aa64 lane: pre-creating for refused
        // harts would strand tasks in all_tasks_).
        if (!kernel::Scheduler::create_ap_idle(logical)) {
            kernel::Logger::warn("SMP: AP idle task OOM — parking");
            return;
        }
        kernel::Logger::info("SMP: AP parked");
        ++logical;
    }
    g_aps_up = static_cast<uint8_t>(logical - 1U);
}

void verify_booted_aps() {
    // One-shot cross-hart IPI verification (post-gate, IRQs live on the
    // APs; the BSP polls shared ack flags — no BSP IRQ needed, so this
    // runs pre-sti in the idle-loop entry, immune to first-tick
    // preemption). A deaf AP cannot take cross-hart wakeups: panic
    // (fail-closed). Single-hart runs (g_aps_up == 0) are a silent no-op.
    static bool verified_once = false;
    if (verified_once) {
        return;
    }
    verified_once = true;
    const uint64_t counter_freq = arch::Timer::raw_counter_freq_hz();
    for (uint8_t logical = 1; logical <= g_aps_up; ++logical) {
        __atomic_store_n(&s_ipi_ack[logical], 0, __ATOMIC_RELEASE);
        bool ipi_ok = false;
        for (uint32_t attempt = 0; attempt < 3U && !ipi_ok; ++attempt) {
            __atomic_fetch_or(&ipi_mailbox_[logical], SMP_IPI_LOOPBACK,
                              __ATOMIC_RELEASE);
            const uint64_t hart = s_ap_hart[logical];
            if (hart >= 64U) {
                break;
            }
            if (!sbi_send_ipi_mask(1ULL << hart)) {
                continue;
            }
            const uint64_t probe_start = arch::Timer::raw_counter();
            for (;;) {
                if (__atomic_load_n(&s_ipi_ack[logical], __ATOMIC_ACQUIRE) !=
                    0U) {
                    ipi_ok = true;
                    break;
                }
                if (counter_freq != 0 &&
                    arch::Timer::raw_counter() - probe_start >=
                        counter_freq) {
                    break;
                }
                arch::pause();
            }
        }
        if (!ipi_ok) {
            kernel::Logger::warn("SMP: IPI loopback failed (hart ID ");
            kernel::Logger::warn("— see DTB cpu walk for the ID list");
            panic("SMP: IPI loopback timeout");
        }
    }
    kernel::Logger::info("SMP: IPI loopback verified");
}

extern "C" void ap_main(uint64_t logical_id, uint64_t ap_hart) {
    // Per-hart identity (sets this AP's tp) + own PLIC S-context.
    // Interrupts stay disabled until the idle phase below.
    arch::percpu_init_ap(logical_id, ap_hart);
    arch::plic_threshold_s_for_hart(ap_hart, 0);
    // Issue #351 dispatch fix (C5): arm this hart's S-timer so ap_tick()
    // runs and drains need_resched from IPI + periodic dispatch. S3 kept
    // the AP timer-quiet to avoid racing the BSP scheduler pre-slice-4;
    // slice-4+ gates are now live (set_task_ready mailbox+IPI, IPI loopback
    // verified), so AP must tick. Mirrors aa64 smp.cpp:434.
    const uint32_t timer_hz = kernel::BootParams::instance().timer_hz;
    arch::Timer::set_frequency(timer_hz);
    ap_ready[logical_id] = 1;

    // Start-gate: spin parked until the BSP publishes scheduler_ready
    // after bring_up() finishes (AP tick must never read all_tasks_
    // mid-rebuild — mirrors the x86 scheduler_ready_ gate, spec §3.4.3).
    while (__atomic_load_n(&scheduler_ready_, __ATOMIC_ACQUIRE) == false) {
        arch::pause();
    }

    TaskControlBlock *idle = kernel::Scheduler::own_idle();
    if (idle == nullptr || idle->magic != TaskControlBlock::TCB_MAGIC) {
        panic("SMP: AP idle missing");
    }
    kernel::Scheduler::set_current_task(idle);
    // stvec is per-hart: point this AP at the shared vector table
    // (IDT::init only programs the calling hart; IDT::load is a no-op).
    arch::IDT::init();
    // Per-hart vectors: re-assert stvec for this hart before enabling
    // STIE/SSIE and before post-gate re-arm — stvec is per-hart (not
    // shared), AP hart1 must have trap_vector direct mode, mirroring
    // aa64 VBAR per-CPU (SMP slice-2). BSP covered hart0; this covers
    // hart1. No extra pmpcfg needed on QEMU virt (S-mode default).
    arch::IDT::init(); // per-hart re-assert (IDT::init is stvec-only, idempotent)
    // Re-arm S-timer after gate (fresh deadline, handler VBAR live, SIE
    // still clear so no trap until SP+SIE atomic). Pre-gate Timer::set_
    // was stale by gate spin (deadline long past).
    {
        const uint32_t hz = kernel::BootParams::instance().timer_hz;
        arch::Timer::set_frequency(hz);
    }
    // Enable the S-soft IPI + S-timer lines (still masked globally via
    // sstatus.SIE clear, so no trap yet). STIE needed for ap_tick().
    {
        uint64_t sie_bits = 0;
        asm volatile("csrr %0, sie" : "=r"(sie_bits));
        sie_bits |= (1ULL << 1); // SSIE: S-mode software interrupts (IPIs)
        sie_bits |= (1ULL << 5); // STIE: S-mode timer (ap_tick dispatch)
        asm volatile("csrw sie, %0" : : "r"(sie_bits) : "memory");
    }
    // Drop onto the idle task's own kernel stack with interrupts on, then
    // halt (mirrors x86 enter_idle_context + the BSP reboot path: SP =
    // idle top, SIE set, wfi loop).  The SP switch and the unmask must be
    // one atomic step: idling on the PMM bring-up stack leaves a TCB-
    // foreign SP that corrupts owner-resolution on the first AP-side
    // deferred switch (issue #351 S3 triage: H2W orphan-displaced ->
    // validate_switch fail -> ENSURE).  Never returns.
    asm volatile("mv sp, %0; csrs sstatus, 2; 1: wfi; j 1b" ::"r"(
                     idle->kernel_stack_top)
                 : "memory");
    __builtin_unreachable();
}

} // namespace kernel::smp
