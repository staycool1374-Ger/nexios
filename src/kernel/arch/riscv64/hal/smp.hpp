#pragma once

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

/// @file smp.hpp
/// @brief RISC-V64 SMP AP bring-up (issue #351, slice-3).  The BSP
///        discovers harts from the DTB (slice-1 helpers), then wakes each
///        secondary with SBI HSM hart_start (start_addr = the MMU-off AP
///        entry stub; no spin-table or CLINT fallback exists — absent HSM
///        support fails closed).  IPIs use the legacy SBI hart-mask call
///        (C2: no CLINT mapping, probe-then-fail-closed); the single
///        S-mode software interrupt fans out via a per-hart mailbox
///        (reschedule / shootdown / loopback).  TLB shootdown applies
///        per-batch-entry SFENCE.VMA with a bounded full-fence fallback.
///        Woken APs run ap_main() (per-hart + PLIC + timer init) and idle
///        with timer ticks live (Phase C scheduling lands in slice-4/5).
///        Single-CPU runs are a silent no-op.  RISC-V-only.

#pragma once

#include <types.hpp>

namespace kernel::smp {

// ─── SBI extension/function IDs (RISC-V SBI v0.2+ calling convention:
//     a7 = EID, a6 = FID, args in a0-a5; legacy: a7 = function ID) ─────────
/// @brief SBI base extension (probing + implementation queries).
inline constexpr uint64_t SBI_EID_BASE = 0x10UL;
/// @brief Base FID: probe whether an extension ID is available.
inline constexpr uint64_t SBI_BASE_PROBE_EXTENSION = 3UL;
/// @brief SBI HSM extension ("HSM"): hart state management.
inline constexpr uint64_t SBI_EID_HSM = 0x48534DUL;
/// @brief HSM FID: start a stopped hart (hartid, start_addr, opaque).
inline constexpr uint64_t SBI_HSM_HART_START = 0UL;
/// @brief HSM FID: query hart state (hartid) -> status code.
inline constexpr uint64_t SBI_HSM_HART_GET_STATUS = 2UL;
/// @brief Legacy (v0.1) SBI IPI function: send_ipi(hart_mask).
/// Fallback only (see sbi_send_ipi): used when the v0.2 IPI extension
/// probe fails. Present in QEMU virt OpenSBI by contract.
inline constexpr uint64_t SBI_LEGACY_SEND_IPI = 4UL;
/// @brief SBI IPI extension ("sPI"): modern hart-mask IPI delivery.
/// Preferred when probed present (fail-closed probe, explicit mask base).
inline constexpr uint64_t SBI_EID_IPI = 0x735049ULL;
/// @brief IPI FID: send_ipi(hart_mask, hart_mask_base).
inline constexpr uint64_t SBI_IPI_SEND_IPI = 0UL;

/// @brief SBI HSM hart states (get_status return values).
inline constexpr uint64_t SBI_HSM_STATE_STARTED = 0UL;

/// @brief IPI mailbox bits (per-hart; S-soft fans out by bit).
inline constexpr uint64_t SMP_IPI_RESCHED = 1ULL << 0U;
/// @brief IPI mailbox bit: TLB-shootdown batch ready.
inline constexpr uint64_t SMP_IPI_SHOOTDOWN = 1ULL << 1U;
/// @brief IPI mailbox bit: bring-up loopback probe (slice-3 verify).
inline constexpr uint64_t SMP_IPI_LOOPBACK = 1ULL << 2U;

/// @brief Number of APs woken AND parked (0 on single-CPU runs).
uint8_t ap_count();

/// @brief Bring up application processors (called once at boot, after
///        Timer::init, under interrupt guard).  Fail-closed: HSM absent,
///        unusable DTB, unknown enable-method, or an AP start timeout
///        parks with zero APs or panics naming the hart ID — never
///        half-woken.  Single-hart topologies are a silent no-op.
void bring_up();

/// @brief Publish the scheduler start-gate (BSP only, after bring_up()
///        finishes parking verified APs, before the idle loop).  APs spin
///        parked until this lands.
void publish_scheduler_ready();

/// @brief One-shot cross-hart IPI verification (issue #351, slice-3).
/// Called once from the BSP idle-loop entry (pre-sti: the BSP needs no
/// IRQs of its own — it sends via SBI and polls shared ack flags while
/// the APs are IRQ-live in their idle loops).  Panics on delivery
/// failure (fail-closed); silent on success.  Single-hart runs skip.
void verify_booted_aps();

/// @brief Send a reschedule IPI to a parked AP (BSP-side).
/// Sets the AP's mailbox RESCHED bit, then the legacy SBI hart-mask ecall.
/// @param logical_id Target AP index (1..CONFIG_MAX_CPUS-1).
/// @return true when the SBI call was accepted, false otherwise.
bool send_resched_ipi(uint64_t logical_id);

/// @brief Queue one TLB-shootdown entry for an AP (BSP-side, no IPI yet).
/// @param logical_id Target AP index (1..CONFIG_MAX_CPUS-1).
/// @param addr Virtual address to fence (per-batch-entry SFENCE.VMA).
/// @return true when queued, false when the batch is full (caller flushes
///         first, then retries) or arguments invalid.
bool queue_shootdown(uint64_t logical_id, uint64_t addr);

/// @brief Deliver an AP's queued shootdown batch (BSP-side, bounded).
/// Polls each batch drained before refilling (never overwrites a live
/// batch); entries stay queued on send failure.  A full ring falls back
/// to a single full fence on the target (bounded, never unbounded loops).
/// @param logical_id Target AP index (1..CONFIG_MAX_CPUS-1).
/// @return Number of IPIs accepted for this call.
uint64_t flush_shootdowns(uint64_t logical_id);

/// @brief ISR entry: apply the calling hart's shootdown slot (bounded).
/// Drains the ring with per-entry SFENCE.VMA and clears the published
/// count. Lock-free, allocation-free; the S-soft handler clears sip.SSIP.
void apply_shootdowns();

/// @brief Applied-entry counter for test readback (slice-5 QE owns the
/// per-entry batch tests; this counter lets them observe AP-side apply
/// without extra instrumentation).
/// @return Total entries applied by AP ISR runs since boot.
uint64_t shootdown_applied();

/// @brief S-mode software-IRQ demux (issue #351 slice-3).
/// Called from handle_plic_trap AFTER it clears sip.SSIP (the line stays
/// asserted until cleared). Fans out the per-hart mailbox in fixed order:
/// loopback ack, shootdown apply, reschedule drain.
void handle_sswi_ipi();

/// @brief Application-hart C entry (called from the ap_entry stub with
///        MMU on, own stack live, tp set).  Never returns.
/// @param logical_id Index into arch::per_cpu[] (1..CONFIG_MAX_CPUS-1).
/// @param ap_hart This AP's mhartid value (matches its DTB reg).
extern "C" void ap_main(uint64_t logical_id, uint64_t ap_hart);

/// @brief AP entry stub (assembly, .text.boot, MMU-off safe).
/// HSM hart_start target: a0 = hartid, a1 = opaque (PA of the AP's
/// ApParam handoff block).  Installs the boot page tables, switches to
/// the AP's stack + tp, then calls ap_main().  Address is VA == PA
/// (low .boot region).
extern "C" void rv64_ap_entry();

} // namespace kernel::smp
