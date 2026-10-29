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
/// @brief AArch64 SMP AP bring-up (issue #351, slice-2).  The BSP walks the
///        DTB cpu nodes (slice-1 helpers), publishes the AP entry cells,
///        then wakes each secondary with PSCI_CPU_ON (SMC/HVC conduit from
///        the DTB psci node) as primary or the spin-table cpu-release-addr
///        hold-loop as fallback.  Woken APs run ap_main() (per-CPU + GIC +
///        timer init) and idle on wfi with timer ticks live (Phase C
///        scheduling lands in slice-4/5).  Runs once at boot under
///        interrupt guard; any failure parks with zero APs or panics
///        naming the MPIDR — never half-woken.  Single-CPU topologies
///        (DTB cpu count <= 1) are a silent no-op.

#include <kernel/arch/aarch64/hal/smp.hpp>

#include <kernel/arch/aarch64/hal/percpu.hpp>
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <constants.hpp>
#include <assert.hpp>
#include <logger.hpp>

/// @brief AP entry publish cells (extern "C": the ap_entry.S stub reads
///        them by unmangled name with the MMU off, via VA - HHDM slide).
extern "C" {
/// @brief DTB reg (MPIDR Aff0) per logical CPU (BSP-filled at bring_up).
uint64_t s_ap_mpidr[CONFIG_MAX_CPUS] = {};
/// @brief Valid entries in s_ap_mpidr (logical CPU count incl. BSP).
uint64_t s_ap_count = 0;
/// @brief Shared boot cells: [kernel TTBR phys, MAIR, TCR, HHDM, per_cpu].
uint64_t s_ap_boot[5] = {};
/// @brief Per-logical stack-top PA (16-byte aligned; AAPCS64 has no bias).
uint64_t s_ap_stack[CONFIG_MAX_CPUS] = {};
}

namespace kernel::smp {

namespace {

// Start-gate flag: BSP publishes (RELEASE) after bring_up() finishes
// parking verified APs; APs spin (ACQUIRE + pause) until it lands.
bool scheduler_ready_ = false;

// One park stack per potential AP (4 KiB each; AP idles on wfi + ticks).
alignas(16) uint8_t ap_stacks[CONFIG_MAX_CPUS > 1 ? CONFIG_MAX_CPUS - 1 : 1]
                         [4096] = {};

// Set by ap_main() after per-CPU + GIC init; polled by the BSP.
volatile uint8_t ap_ready[CONFIG_MAX_CPUS] = {};

// BSP-observed parked-AP count (published at the end of bring_up()).
uint8_t g_aps_up = 0;

// SGI loopback acks, one slot per logical CPU (BSP-owned, written by the
// receiving CPU's handler — C=0 on both sides, so all accesses hit RAM).
volatile uint64_t s_sgi_ack[CONFIG_MAX_CPUS] = {};

// Conduit selected from the DTB psci node ("smc" or "hvc"; empty = none).
bool s_use_smc = true;
bool s_have_psci = false;

/// @brief PA of the aa64_ap_entry stub (VA == PA: .boot.text links low).
/// Loaded through a literal pool (full 64-bit ABS64): the stub lives ~2^48
/// below high text, out of adrp range, so no direct C++ reference may take
/// its address (R_AARCH64_ADR_PREL_PG_HI21 truncation at link).
uint64_t ap_entry_pa() {
    uint64_t entry_addr = 0;
    asm volatile("ldr %0, =aa64_ap_entry" : "=r"(entry_addr));
    return entry_addr;
}

/// @brief PSCI conduit call (SMC variant): func(arg0, arg1, arg2) -> x0.
uint64_t psci_call_smc(uint64_t func, uint64_t arg0, uint64_t arg1,
                       uint64_t arg2) {
    register uint64_t reg_x0 asm("x0") = func;
    register uint64_t reg_x1 asm("x1") = arg0;
    register uint64_t reg_x2 asm("x2") = arg1;
    register uint64_t reg_x3 asm("x3") = arg2;
    asm volatile("smc #0"
                 : "+r"(reg_x0)
                 : "r"(reg_x1), "r"(reg_x2), "r"(reg_x3)
                 : "memory");
    return reg_x0;
}

/// @brief PSCI conduit call (HVC variant): func(arg0, arg1, arg2) -> x0.
uint64_t psci_call_hvc(uint64_t func, uint64_t arg0, uint64_t arg1,
                       uint64_t arg2) {
    register uint64_t reg_x0 asm("x0") = func;
    register uint64_t reg_x1 asm("x1") = arg0;
    register uint64_t reg_x2 asm("x2") = arg1;
    register uint64_t reg_x3 asm("x3") = arg2;
    asm volatile("hvc #0"
                 : "+r"(reg_x0)
                 : "r"(reg_x1), "r"(reg_x2), "r"(reg_x3)
                 : "memory");
    return reg_x0;
}

/// @brief PSCI call on the probed conduit (fail-closed: no conduit means
///        the caller falls back to spin-table, never issues a blind SMC).
/// @return PSCI return code, or ~0ULL when no conduit was probed.
uint64_t psci_call(uint64_t func, uint64_t arg0, uint64_t arg1,
                   uint64_t arg2) {
    if (!s_have_psci) {
        return ~0ULL;
    }
    if (s_use_smc) {
        return psci_call_smc(func, arg0, arg1, arg2);
    }
    return psci_call_hvc(func, arg0, arg1, arg2);
}

/// @brief Bounded method-string compare (DTB payloads may lack NUL).
bool method_is(const char *method, const char *want) {
    if (method == nullptr || want == nullptr) {
        return false;
    }
    uint32_t idx = 0;
    while (want[idx] != '\0' && idx < 8U) {
        if (method[idx] != want[idx]) {
            return false;
        }
        ++idx;
    }
    return want[idx] == '\0' && method[idx] == '\0';
}

/// @brief Clean a VA range to the point of coherency (the AP entry stub
///        reads the publish cells with the MMU off, so BSP-cached writes
///        must be pushed to RAM first).  Mirrors the boot.S cvac loop.
void clean_to_ram(const void *base, uint64_t size) {
    const uint64_t line_size = 64;
    uint64_t clean_addr = reinterpret_cast<uint64_t>(base) & ~(line_size - 1U);
    const uint64_t clean_end = reinterpret_cast<uint64_t>(base) + size;
    uint64_t clean_count = 0;
    while (clean_addr < clean_end && clean_count < 1024U) {
        asm volatile("dc cvac, %0" : : "r"(clean_addr) : "memory");
        clean_addr += line_size;
        ++clean_count;
    }
    asm volatile("dsb sy" : : : "memory");
}

} // namespace

uint8_t ap_count() {
    return g_aps_up;
}

uint64_t ap_aff0(uint8_t logical) {
    // GIC targets are Aff0 (v2 interface / v3 TargetList bit), not the
    // compressed logical index: refusal skips make them differ on gapped
    // topologies (same mapping the ap_entry stub resolves via MPIDR).
    if (logical == 0 || logical >= CONFIG_MAX_CPUS) {
        return 0xFFU; // unmapped: caller treats as un-targetable (fail-closed)
    }
    return s_ap_mpidr[logical] & 0xFFU;
}

void publish_scheduler_ready() {
    __atomic_store_n(&scheduler_ready_, true, __ATOMIC_RELEASE);
}

void bring_up() {
    arch::IrqGuard guard{}; // boot is not yet preemptible-safe: no ticks here
    const void *dtb = kernel::boot::dtb_from_boot_info();
    if (dtb == nullptr) {
        // No DTB: topology unprovable — warn and park (BSP-only behavior).
        // The BSP booted via the embedded fallback; without a live tree no
        // secondary may be released (spec §3 slice-0 ordering).
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
        return; // single-CPU: silent no-op, BSP-only behavior
    }

    // PSCI probe: version >= 1.0 on the DTB psci conduit, else no PSCI.
    char psci_method[8] = {};
    if (kernel::boot::dtb_psci_method(dtb, psci_method, 8)) {
        if (method_is(psci_method, "smc")) {
            s_use_smc = true;
            s_have_psci = true;
        } else if (method_is(psci_method, "hvc")) {
            s_use_smc = false;
            s_have_psci = true;
        }
    }
    if (s_have_psci) {
        const uint64_t psci_version = psci_call(PSCI_FN_VERSION, 0, 0, 0);
        if (psci_version < PSCI_VERSION_MIN) {
            // Conduit skew (spec §5 risk-a): probe failed — no blind SMC.
            kernel::Logger::warn("SMP: PSCI version skew — PSCI disabled");
            s_have_psci = false;
        }
    }

    const uint64_t bsp_aff0 = arch::mpidr_aff0();
    const uint64_t entry_pa = ap_entry_pa();

    // Publish the shared boot cells (TTBR live on the BSP, MAIR/TCR live,
    // per_cpu base HH VA, HHDM slide) + clean them to RAM for the MMU-off
    // stub.  TTBR1 is the kernel half the BSP runs on — the AP installs
    // the identical regime, so it lands in the same address space.
    uint64_t live_ttbr = 0;
    uint64_t live_mair = 0;
    uint64_t live_tcr = 0;
    asm volatile("mrs %0, ttbr1_el1" : "=r"(live_ttbr));
    asm volatile("mrs %0, mair_el1" : "=r"(live_mair));
    asm volatile("mrs %0, tcr_el1" : "=r"(live_tcr));
    s_ap_boot[0] = live_ttbr;
    s_ap_boot[1] = live_mair;
    s_ap_boot[2] = live_tcr;
    s_ap_boot[3] = arch::HHDM_OFFSET;
    s_ap_boot[4] = reinterpret_cast<uint64_t>(&arch::per_cpu[0]);

    // Loopback handler (registered once, before any wake): the receiving
    // CPU records its own slot, identified via its live TPIDR identity.
    // No EOI in-handler — handle_gic_irq closes the SGI at its tail (an
    // early eoi() would trip the ownership guard as unmatched).
    arch::IDT::register_handler_raw(
        arch::SMP_LOOPBACK_SGI, [](uint64_t, uint64_t, uint64_t) {
            const uint64_t here = arch::cpu_index() % CONFIG_MAX_CPUS;
            __atomic_store_n(&s_sgi_ack[here], 1, __ATOMIC_RELEASE);
        });
    // Enable the kernel SGI set (loopback + scheduler + shootdown) for
    // SMP: distributor (v2) or own redistributor (v3).  Single-CPU runs
    // returned above, so the shared BSP init path stays bit-exact.
    arch::gic_enable_sgi();

    uint8_t logical = 1;
    for (int cpu_idx = 0; cpu_idx < cpu_count; ++cpu_idx) {
        if (logical >= CONFIG_MAX_CPUS) {
            break;
        }
        const uint64_t cpu_aff = cpu_entries[cpu_idx].id & 0xFFU;
        if (cpu_aff == bsp_aff0) {
            continue; // never wake the BSP
        }
        // AAPCS64: SP must stay 16-byte aligned — no x86 retaddr bias.
        // (x86 subtracts 8 so a jmp-entered ap_main observes RSP%16==8;
        // AArch64 faults on any misaligned SP with SA enabled.)
        const uint64_t stack_top =
            reinterpret_cast<uint64_t>(&ap_stacks[logical - 1][0]) + 4096U;
        s_ap_mpidr[logical] = cpu_aff;
        s_ap_count = logical + 1U;
        s_ap_stack[logical] = stack_top - arch::HHDM_OFFSET;
        ap_ready[logical] = 0;
        __atomic_store_n(&s_sgi_ack[logical], 0, __ATOMIC_RELEASE);
        // Publish + clean before the wake (the stub reads with MMU off).
        clean_to_ram(&s_ap_mpidr[0], sizeof(s_ap_mpidr));
        clean_to_ram(&s_ap_count, sizeof(s_ap_count));
        clean_to_ram(&s_ap_boot[0], sizeof(s_ap_boot));
        clean_to_ram(&s_ap_stack[0], sizeof(s_ap_stack));

        bool wake_ok = false;
        bool cpu_absent = false;
        if (s_have_psci &&
            cpu_entries[cpu_idx].method !=
                kernel::boot::CpuEnableMethod::SPIN_TABLE) {
            // PSCI primary (QEMU virt path): entry + context id = logical.
            // A refusal (absent CPU: the embedded DTB carries max topology)
            // is an expected skip, never a fault — keep parking the rest.
            // NOTE: the idle task is created only AFTER a successful wake
            // (below) — pre-creating for absent CPUs leaks tasks into
            // all_tasks_ and corrupts the registry walk (fail-closed: the
            // AP adopts post-gate, so late creation is race-free).
            const uint64_t psci_ret =
                psci_call(PSCI_FN_CPU_ON, cpu_aff, entry_pa, logical);
            if (psci_ret == PSCI_RET_SUCCESS) {
                wake_ok = true;
            } else {
                cpu_absent = true;
            }
        } else if (cpu_entries[cpu_idx].method ==
                       kernel::boot::CpuEnableMethod::SPIN_TABLE &&
                   cpu_entries[cpu_idx].release_addr != 0U) {
            // Spin-table fallback: release-addr hold-loop + sev.
            auto *release_cell = reinterpret_cast<volatile uint64_t *>(
                arch::HHDM_OFFSET + cpu_entries[cpu_idx].release_addr);
            *release_cell = entry_pa;
            asm volatile("dsb sy" : : : "memory");
            asm volatile("sev");
            wake_ok = true;
        } else {
            // Unknown method (or PSCI-skewed with no release-addr):
            // this CPU cannot be booted safely — skip it, warn loudly.
            kernel::Logger::warn("SMP: cpu has no bootable method — skipped");
            continue;
        }
        if (cpu_absent) {
            continue; // PSCI refusal: CPU not online (max-topology probe)
        }
        if (!wake_ok) {
            kernel::Logger::warn("SMP: PSCI CPU_ON refused — parking");
            return;
        }

        // Rendezvous: the AP sets ap_ready[logical] at the end of its
        // init; 1 s of counter time, then a controlled panic naming the
        // MPIDR (mirrors the x86 LAPIC-ID panic contract).
        const uint64_t counter_freq = arch::Timer::tsc_freq_hz();
        uint64_t wait_start = 0;
        asm volatile("mrs %0, cntpct_el0" : "=r"(wait_start));
        bool ap_up = false;
        for (;;) {
            if (ap_ready[logical] != 0) {
                ap_up = true;
                break;
            }
            uint64_t wait_now = 0;
            asm volatile("mrs %0, cntpct_el0" : "=r"(wait_now));
            if (counter_freq != 0 &&
                wait_now - wait_start >= counter_freq) {
                break;
            }
            arch::pause();
        }
        if (!ap_up) {
            kernel::Logger::warn("SMP: AP failed to start (MPIDR Aff0 ");
            kernel::Logger::warn("— see DTB cpu walk for the ID list");
            panic("SMP: AP start timeout");
        }
        // Idle task created on the BSP only after the AP proved alive
        // (single-threaded; the AP adopts it post-gate — late creation is
        // race-free and never leaks tasks for refused/absent CPUs).
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
    // One-shot cross-CPU SGI verification (post-gate, IRQs live on the
    // BSP, APs IRQ-live in their idle loops).  The BSP never needs to
    // receive: each AP's handler acks its own slot, the BSP polls memory.
    // A deaf AP cannot take cross-CPU wakeups: panic (fail-closed).
    // Single-CPU runs (g_aps_up == 0) are a silent no-op.
    static bool verified_once = false;
    if (verified_once) {
        return;
    }
    verified_once = true;
    const uint64_t counter_freq = arch::Timer::tsc_freq_hz();
    for (uint8_t logical = 1; logical <= g_aps_up; ++logical) {
        __atomic_store_n(&s_sgi_ack[logical], 0, __ATOMIC_RELEASE);
        bool sgi_ok = false;
        // GIC targets are Aff0, not the compressed logical index.
        const uint32_t sgi_target =
            static_cast<uint32_t>(ap_aff0(logical) & 0xFFU);
        for (uint32_t attempt = 0; attempt < 3U && !sgi_ok; ++attempt) {
            if (!arch::gic_send_sgi(sgi_target, arch::SMP_LOOPBACK_SGI)) {
                break;
            }
            uint64_t probe_start = 0;
            asm volatile("mrs %0, cntpct_el0" : "=r"(probe_start));
            for (;;) {
                if (__atomic_load_n(&s_sgi_ack[logical], __ATOMIC_ACQUIRE) !=
                    0U) {
                    sgi_ok = true;
                    break;
                }
                uint64_t probe_now = 0;
                asm volatile("mrs %0, cntpct_el0" : "=r"(probe_now));
                if (counter_freq != 0 &&
                    probe_now - probe_start >= counter_freq) {
                    break;
                }
                arch::pause();
            }
        }
        if (!sgi_ok) {
            // A deaf AP cannot take cross-CPU wakeups (reschedule or
            // shootdown): fail closed and loud, naming the MPIDR.
            kernel::Logger::warn("SMP: SGI loopback failed (MPIDR Aff0 ");
            kernel::Logger::warn("— see DTB cpu walk for the ID list");
            panic("SMP: SGI loopback timeout");
        }
    }
    kernel::Logger::info("SMP: SGI loopback verified");
}

extern "C" void ap_main(uint64_t logical_id, uint64_t ap_mpidr) {
    // Per-CPU identity (sets this AP's TPIDR_EL1) + own GIC CPU interface.
    // Interrupts stay disabled until the idle phase below (mirrors the
    // x86 phase order: init + gate first, timer/IRQ last).
    arch::percpu_init_ap(logical_id, ap_mpidr);
    arch::gic_ap_init();
    // Arm this CPU's own physical timer (per-CPU CNT P; the ISR re-arms).
    // Delivery is still masked, so arming here is side-effect-free.
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
    // VBAR is per-CPU: point this AP at the shared vector table (IDT::init
    // only programs the calling CPU; IDT::load is a barrier-only no-op).
    arch::IDT::init();
    // Drop onto the idle task's own kernel stack with interrupts enabled
    // atomically, then halt (mirrors x86 enter_idle_context + rv64 dispatch
    // fix: SP = idle top, daif clear, wfi loop). The SP switch and the
    // unmask must be one atomic step: idling on ap_stacks leaves a
    // TCB-foreign SP that corrupts owner-resolution on the first AP-side
    // deferred switch (H2W orphan, same class as rv64 PMM stack).
    asm volatile("mov sp, %0; msr daif, xzr; 1: wfi; b 1b" ::"r"(
                     idle->kernel_stack_top)
                 : "memory");
    __builtin_unreachable();
}

} // namespace kernel::smp
