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

/// @file shootdown_ipi.hpp
/// @brief Batched TLB-shootdown IPI delivery (issue #159, x86_64 only).
///
/// IPIs carry no payload, so batches travel via shared-memory per-target
/// slots: the sender fills a slot, release-publishes its count, and sends
/// ONE shootdown IPI (SHOOTDOWN_BATCH_VECTOR); the target ISR applies
/// every entry, records them for order checks, and EOIs (EOI stays with
/// the registering handler, mirroring the 0x72 probe pattern).
///
/// Protocol (single-CPU safe, cross-CPU correct): flush_target() polls
/// the slot drained before refilling, so an ISR can never observe a
/// half-filled batch.  Delivery to non-up CPUs is accepted-but-unapplied
/// (same shape as the SCHED mailbox); tests target self only.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

class ShootdownBatch {
  public:
    /// @brief Batch slot capacity (entries per IPI).
    static constexpr size_t BATCH_SLOTS = 16;
    /// @brief Max log entries kept for order checks.
    static constexpr size_t APPLIED_LOG = 64;

    /// @brief One shootdown entry (VA + owning PCID).
    struct BatchEntry {
        uint64_t va;
        uint16_t pcid;
    };

    /// @brief Queue one entry for @p cpu (no IPI yet).
    /// @return true when queued; false when the slot is full (caller
    ///         flushes first, then retries) or arguments invalid.
    static bool queue_remote(uint64_t cpu, uint64_t va, uint16_t pcid);
    /// @brief Deliver @p cpu's queued entries in bounded batches.
    ///        Polls each batch drained before refilling (never overwrites
    ///        a live batch).  Entries stay queued on send failure.
    ///        Callers must not hold IF=0 across the call for a self
    ///        target (the delivery ISR cannot land until sti).
    /// @return Number of IPIs accepted by the APIC for this call.
    static uint64_t flush_target(uint64_t cpu);
    /// @brief ISR entry: apply @p cpu's slot (bounded), record order,
    ///        clear the published count.  Lock-free, allocation-free;
    ///        caller performs exactly one EOI per run.
    static void handle_cpu(uint64_t cpu);
    /// @brief Total IPIs accepted (wrapper-side, deterministic).
    static uint64_t sends_for_test();
    /// @brief Total entries applied by handler runs.
    static uint64_t applied_for_test();
    /// @brief Applied VA log (ascending-check source for tests).
    static uint64_t applied_va_for_test(uint64_t idx);
    /// @brief Reset slots, counters and log (test isolation).
    static void reset_for_test();

  private:
    struct CpuSlot {
        BatchEntry entries[BATCH_SLOTS];
        uint64_t count;
    };

    static CpuSlot slots_[CONFIG_MAX_CPUS];
    static uint64_t sends_;
    static uint64_t applied_;
    static uint64_t applied_log_[APPLIED_LOG];
    static uint64_t applied_log_count_;
};

} // namespace arch
