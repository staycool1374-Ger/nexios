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
/// @brief AArch64 SMP AP bring-up (issue #351, slice-2).  The BSP discovers
///        CPUs from the DTB (slice-1 helpers), then wakes each secondary
///        with PSCI_CPU_ON (SMC/HVC conduit from the psci node) as primary
///        or the spin-table cpu-release-addr hold-loop as fallback.  Woken
///        APs run ap_main() (per-CPU + GIC init) and idle with timer ticks
///        (Phase C scheduling lands in slice-4/5).  Single-CPU runs are a
///        silent no-op.  AArch64-only.

#include <types.hpp>
#include <kernel/arch/aarch64/hal/gic.hpp>

namespace kernel::smp {

/// @brief PSCI version for the CPU_ON primitive (32-bit calling convention).
/// PSCI 0.2+ is required; the version is probed at boot and anything older
/// (or absent) fails the bring-up closed (0 APs, never half-woken).
inline constexpr uint32_t PSCI_VERSION_MIN = 0x00010000U; // 1.0
/// @brief PSCI function IDs (SMC32 calling convention).
inline constexpr uint32_t PSCI_FN_VERSION = 0x84000000U;
inline constexpr uint32_t PSCI_FN_CPU_ON = 0xC4000003U;
/// @brief PSCI return codes we distinguish (all others fail closed alike).
inline constexpr uint64_t PSCI_RET_SUCCESS = 0U;

/// @brief Number of APs woken AND parked (0 on single-CPU runs).
uint8_t ap_count();

/// @brief True once the scheduler start-gate is published (issue #389).
/// Fan-out targets park pre-gate (no drain yet), so the shootdown hook
/// fans out only when true — matching the x86/rv64 contract.
/// @return True after publish_scheduler_ready(), false before.
bool scheduler_started();

/// @brief Applied-shootdown counter for test readback (issue #389).
/// Mirrors the rv64 `shootdown_applied()` observable; incremented once
/// per SGI-10 purge by the shootdown handler. Lets QE observe AP-side
/// apply end to end (`s_sgi_ack` slots stay file-static, GDB-only).
/// @return Total shootdown purges applied by AP handlers since boot.
uint64_t shootdown_applied();

/// @brief Record one AP-side shootdown purge (SGI-10 handler path).
void note_shootdown_applied();

/// @brief DTB reg (MPIDR Aff0) for a parked AP (issue #351 slice-2).
/// Maps the compressed logical index to the GIC target (Aff0); identity
/// only for contiguous topologies. 0xFF when unmapped (fail-closed).
uint64_t ap_aff0(uint8_t logical);
/// @brief Aff0 for any logical CPU incl. the BSP (issue #390).
/// Logical 0 resolves to the BSP identity (`arch::per_cpu[0].mpidr`,
/// published by `percpu_init_bsp`); 1..N forward to `ap_aff0`.
/// 0xFF when unmapped (fail-closed skip).
uint64_t cpu_aff0(uint8_t logical);
/// @brief Total shootdown SGI sends refused by the transport
/// (issue #390 S3). Monotonic, never gates control flow.
uint64_t shootdown_send_failures();
/// @brief Record one refused shootdown SGI send.
void note_shootdown_send_failure();

/// @brief Bring up application processors (called once at boot, after
///        Timer::init, under interrupt guard).  Fail-closed: any malformed
///        state (no DTB, unusable conduit, AP start timeout) parks with
///        zero APs or panics naming the MPIDR — never half-woken.
///        Single-CPU topologies (DTB cpu count <= 1) are a silent no-op.
void bring_up();

/// @brief Publish the scheduler start-gate (BSP only, after bring_up()
/// finishes parking verified APs, before the idle loop).  APs spin
/// parked until this lands.
void publish_scheduler_ready();

/// @brief One-shot cross-CPU SGI verification (issue #351, slice-2).
/// Called once from the BSP idle-loop entry (post-gate, IRQs live, APs
/// IRQ-live): sends each parked AP the loopback SGI and polls its ack.
/// Panics on delivery failure (fail-closed); silent on success.
/// Never called during bring_up (the BSP is IRQ-masked there and the APs
/// are pre-gate — probing then would deadlock by design).
void verify_booted_aps();

/// @brief Application-processor C entry (called from the ap_entry stub with
///        MMU on, own stack live, TPIDR_EL1 set).  Never returns.
/// @param logical_id Index into arch::per_cpu[] (1..CONFIG_MAX_CPUS-1).
/// @param ap_mpidr This AP's MPIDR_EL1 value (matches its DTB reg).
extern "C" void ap_main(uint64_t logical_id, uint64_t ap_mpidr);

/// @brief AP entry stub (assembly, .boot.text, MMU-off safe).
/// PSCI_CPU_ON / spin-table release target.  x0 = context id (logical id).
/// Installs the published kernel TTBRs, enables the MMU, switches to the
/// AP's stack + TPIDR, then calls ap_main().  Address is VA == PA.
extern "C" void aa64_ap_entry();

} // namespace kernel::smp
