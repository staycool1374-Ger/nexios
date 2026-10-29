/*
 * NexIOS RTOS — Cross-CPU TLB Shootdown (SMP)
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

/// @file smp_shootdown.cpp
/// @brief RISC-V64 strong cross-CPU TLB-shootdown hook (issue #389).
///        Overrides the weak `VMM::smp_tlb_shootdown_hook` no-op.
///        PRIMARY: `sbi_remote_sfence_vma` RFENCE broadcast (SBI EID
///        0x52464E43, FID 1 `remote_sfence_vma(hart_mask,
///        hart_mask_base, start, size)`; FID 2 ASID variant explicitly
///        NOT used — FID 1's VA form covers all ASIDs, safe superset).
///        `all=true` uses the spec full-range encoding (start and size
///        both zero = whole-TLB fence).  SYNCHRONOUS-BLOCK: the ecall
///        returns only after remote harts fence, so exposure is bounded
///        by discipline, not timeout — hart mask restricted to
///        verified-up harts, SBI error falls back immediately (no retry
///        loop).  FALLBACK (RFENCE absent or send refused): the existing
///        `shoot_slots_` mailbox + S-soft `apply_shootdowns` path
///        (per-VA entries; all=true via `request_full_fence`, whose
///        empty-slot-with-bit the apply side fences whole).
/// @note Concurrency/caller contract: Lock: none.  Context: any-task.
///       SMP: single RFENCE ecall or straight-line mailbox sends; the
///       sync-block ecall runs with the verified-up mask only, so a
///       down hart can cost the bounded wait, never a hang.

#include <kernel/memory/vmm.hpp>
#include <kernel/arch/riscv64/hal/smp.hpp>
#include <kernel/arch/riscv64/hal/percpu.hpp>
#include <kernel/arch/page_table.hpp>

namespace {

// SBI RFENCE extension (SBI RFENCE §8.2 / v3.0 §8.1.2): EID 0x52464E43.
constexpr uint64_t SBI_EID_RFENCE = 0x52464E43UL;
// FID 1: remote_sfence_vma(hart_mask, hart_mask_base, start, size).
constexpr uint64_t SBI_RFENCE_SFENCE_VMA = 1UL;
// SBI base extension (probing), mirroring smp.cpp.
constexpr uint64_t SBI_EID_BASE = 0x10UL;
constexpr uint64_t SBI_BASE_PROBE_EXTENSION = 3UL;

// RFENCE presence: -1 unknown (probe once), 0 absent, 1 present.
int64_t s_rfence_present = -1;
// Test override (issue #389 QE fallback-forcing): -1 none, else forced.
int64_t s_rfence_override = -1;

/// @brief SBI v0.2+ call result (sbiret: error in a0, value in a1).
struct SbiRet {
    uint64_t error;
    uint64_t value;
};

/// @brief 3-arg SBI call (probe path only).
SbiRet sbi_call3(uint64_t eid, uint64_t fid, uint64_t arg0) {
    register uint64_t reg_a0 asm("a0") = arg0;
    register uint64_t reg_a1 asm("a1") = 0;
    register uint64_t reg_a6 asm("a6") = fid;
    register uint64_t reg_a7 asm("a7") = eid;
    asm volatile("ecall"
                 : "+r"(reg_a0), "+r"(reg_a1)
                 : "r"(reg_a6), "r"(reg_a7)
                 : "memory");
    SbiRet ret = {reg_a0, reg_a1};
    return ret;
}

/// @brief 4-arg SBI call (RFENCE path: a0-a3 carry the request).
SbiRet sbi_call4(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1,
                 uint64_t arg2, uint64_t arg3) {
    register uint64_t reg_a0 asm("a0") = arg0;
    register uint64_t reg_a1 asm("a1") = arg1;
    register uint64_t reg_a2 asm("a2") = arg2;
    register uint64_t reg_a3 asm("a3") = arg3;
    register uint64_t reg_a6 asm("a6") = fid;
    register uint64_t reg_a7 asm("a7") = eid;
    asm volatile("ecall"
                 : "+r"(reg_a0), "+r"(reg_a1), "+r"(reg_a2), "+r"(reg_a3)
                 : "r"(reg_a6), "r"(reg_a7)
                 : "memory");
    SbiRet ret = {reg_a0, reg_a1};
    return ret;
}

/// @brief Probe the RFENCE extension once (base EID 0x10, FID 3).
bool rfence_usable() {
    if (s_rfence_override >= 0) {
        return s_rfence_override > 0;
    }
    if (s_rfence_present < 0) {
        SbiRet probe = sbi_call3(SBI_EID_BASE, SBI_BASE_PROBE_EXTENSION,
                                 SBI_EID_RFENCE);
        s_rfence_present =
            (probe.error == 0 && probe.value != 0U) ? 1 : 0;
    }
    return s_rfence_present > 0;
}

} // namespace

namespace kernel::smp {

/// @brief Force the RFENCE presence answer for tests (issue #389).
/// @param present Nonzero pretends RFENCE present, zero forces the
///        S-soft fallback path (mirrors `sbi_probe_extension`).
void set_sbi_rfence_for_test(bool present) {
    s_rfence_override = present ? 1 : 0;
}

/// @brief Clear the RFENCE test override (restore probed behavior).
void clear_sbi_rfence_for_test() {
    s_rfence_override = -1;
}

} // namespace kernel::smp

namespace kernel {

void VMM::smp_tlb_shootdown_hook(uint64_t va, uint16_t context_id, bool all) {
    (void)context_id;
    // Pre-gate the parked harts cannot drain (RFENCE would block until
    // they enable interrupts — after this hook's caller): local was
    // already flushed by the choke — fan out only when live.
    if (!kernel::smp::scheduler_started()) {
        return;
    }
    const uint64_t up_count =
        static_cast<uint64_t>(kernel::smp::ap_count());
    if (up_count == 0U) {
        return;
    }
    if (rfence_usable()) {
        uint64_t mask = 0;
        for (uint64_t cpu = 1; cpu <= up_count; ++cpu) {
            if (cpu >= CONFIG_MAX_CPUS) {
                break;
            }
            const uint64_t hart =
                kernel::smp::ap_hart(static_cast<uint8_t>(cpu));
            if (hart >= 64U) {
                mask = ~0ULL;
                break;
            }
            mask |= (1ULL << hart);
        }
        if (mask != ~0ULL) {
            const uint64_t start = all ? 0U : va;
            const uint64_t size = all ? 0U : arch::PAGE_SIZE;
            SbiRet ret = sbi_call4(SBI_EID_RFENCE, SBI_RFENCE_SFENCE_VMA,
                                   mask, 0U, start, size);
            if (ret.error == 0U) {
                return;
            }
        }
        // Probed present but unusable mask/send: fall through to the
        // mailbox path (report failure upstream by delivering anyway —
        // never silently drop the shootdown).
    }
    for (uint64_t cpu = 1; cpu <= up_count; ++cpu) {
        if (cpu >= CONFIG_MAX_CPUS) {
            break;
        }
        if (all) {
            kernel::smp::request_full_fence(cpu);
        } else {
            if (!kernel::smp::queue_shootdown(cpu, va)) {
                kernel::smp::flush_shootdowns(cpu);
                kernel::smp::queue_shootdown(cpu, va);
            }
            kernel::smp::flush_shootdowns(cpu);
        }
    }
}

} // namespace kernel
