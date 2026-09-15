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

/// @file tlb_shootdown.hpp
/// @brief Lazy TLB shootdown request queue + deferred-free quarantine
///        (issue #158).  POLICY side: requests are recorded, coalesced
///        and applied locally; freed pages park in quarantine until
///        their shootdown applied or a tick timeout fires.  Cross-CPU
///        IPI delivery is #159's scope — this module never sends IPIs.
///
///        Opt-in only: the default PMM free path and the #157 immediate
///        unmap flushes are untouched.  All state is bounded static
///        arrays; all loops bounded; no allocation, ever.

#pragma once

#include <types.hpp>

namespace kernel {

class TlbShootdown {
  public:
    /// @brief Max queued shootdown requests.
    static constexpr size_t MAX_PENDING = 64;
    /// @brief Max quarantined pages.
    static constexpr size_t MAX_QUARANTINE = 64;
    /// @brief Max quarantine releases per on_tick call (RT bound).
    static constexpr uint64_t MAX_PER_TICK = 8;
    /// @brief Default quarantine timeout in ticks.
    static constexpr uint64_t DEFAULT_TIMEOUT_TICKS = 100;
    /// @brief Single-PCID group size that upgrades per-VA flushes to
    ///        one context purge (issue #158).
    static constexpr uint64_t PURGE_GROUP_THRESHOLD = 8;

    /// @brief Module error codes (fail-closed, never panics).
    enum class Error : uint64_t {
        OK = 0,
        QUEUE_FULL = 1,
        QUARANTINE_FULL = 2,
        INVALID = 3,
    };

    /// @brief Pending shootdown request (VA + owning PCID).
    struct Request {
        uint64_t va;
        uint16_t pcid;
    };

    /// @brief Record a shootdown request (deduplicates exact pairs).
    /// @param va Page-aligned virtual address.
    /// @param pcid Owning PCID (0 = untagged/kernel).
    /// @return OK, INVALID (unaligned VA), or QUEUE_FULL (no eviction;
    ///         caller keeps its immediate-flush fallback).
    static Error request(uint64_t va, uint16_t pcid) noexcept;
    /// @brief Coalesce pending requests and apply them locally
    ///        (per-VA flush, or one context purge per large same-PCID
    ///        group).  Clears applied entries.
    static void coalesce_and_apply() noexcept;
    /// @brief Park an allocated page in quarantine (deferred free).
    ///        The page stays bitmap-allocated (no track_pmm_free) until
    ///        released; never touches the PMM bitmap itself.
    /// @param phys Page-aligned physical address of an allocated page.
    /// @return OK, INVALID (unallocated/misaligned), or QUARANTINE_FULL
    ///         (page stays allocated; caller frees it directly).
    static Error hold(uint64_t phys) noexcept;
    /// @brief Apply pending shootdowns, then return every held page to
    ///        the PMM free list.
    static void release_all() noexcept;
    /// @brief Tick hook: force-release expired quarantine entries
    ///        (bounded MAX_PER_TICK per call).  ISR-safe: try-lock,
    ///        skip on contention, never blocks or allocates.
    /// @param now Current value of arch::Timer::ticks().
    static void on_tick(uint64_t now) noexcept;
    /// @brief Queued request count (introspection/test only).
    static uint64_t pending_count() noexcept;
    /// @brief Quarantined page count (introspection/test only).
    static uint64_t quarantine_count() noexcept;
    /// @brief Override the quarantine timeout (test hook).
    static void set_timeout_ticks_for_test(uint64_t ticks) noexcept;
    /// @brief Clear queue + quarantine + timeout (test isolation).
    ///        Does NOT free held pages (PMM rewind owns them); prevents
    ///        stale-flush counter noise across tests.
    static void reset() noexcept;

  private:
    struct Held {
        uint64_t phys;
        uint64_t deadline;
    };

    static uint64_t timeout_ticks_;
    static Request pending_[MAX_PENDING];
    static uint64_t pending_count_;
    static Held quarantine_[MAX_QUARANTINE];
    static uint64_t quarantine_count_;
};

} // namespace kernel
