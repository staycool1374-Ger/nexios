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

/// @file spinlock_order.hpp
/// @brief Lock-rank table + per-CPU held-lock bookkeeping (M2, #546 §6.2).
/// Lightweight on purpose: includable from spinlock.hpp with no scheduler
/// or task dependency (owner resolution lives in spinlock_order.cpp).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace kernel {
namespace sync {

/// @brief Lock rank, outer → inner.  Unranked locks are invisible to the
///        order check (documented per site; M8 re-audits the set).
enum class LockRank : uint8_t {
    Unranked = 0,
    Sched = 1, // scheduler_lock_ (outermost)
    Queue = 2, // MessageQueue / sync::Queue (endpoint/queue lock)
    Leaf = 3,  // DeathNotify::s_lock_, g_iopb_lock, ... (innermost)
};

/// @brief Always-on per-CPU held-lock depth (both builds).  SpinLock
///        take/release paths bump/drop it so the M2 park predicate and
///        the INV-C1 check work in release builds where the HeldLock
///        stack is compiled out.  Balanced pairs only: an unpaired
///        release is a no-op (never drives the counter negative).
///        Defined in spinlock_order.cpp (TU-static storage).
uint64_t spinlock_held_depth_own() noexcept;
void spinlock_held_bump() noexcept;
void spinlock_held_drop() noexcept;

#if defined(CONFIG_DEBUG)

/// @brief One held-lock record (own CPU slot only, ISR-serialized).
struct HeldLock {
    const void *addr = nullptr;
    uint64_t owner_id = 0;
    uint8_t rank = 0;
};

inline constexpr size_t kMaxHeldLocks = 8;

/// @brief Record a successful take (push).  Report-only order check:
///        taking rank R while a higher-or-equal rank is held logs once
///        per (outer, inner) pair; never panics, never blocks.
void spinlock_note_acquire(const void *addr, uint8_t rank) noexcept;

/// @brief Record a release (pop by address from the top).  An unpaired
///        release (e.g. init-time unlock of a never-locked lock) is a
///        silent no-op plus a one-shot DEBUG note — it must not corrupt
///        the stack or the always-on depth counter.
void spinlock_note_release(const void *addr) noexcept;

/// @brief INV-C1 entry check for reschedule()/BLOCKED transitions:
///        true iff the calling context holds no spinlock (owner-id
///        matched, not per-CPU counted — a preempted holder on the same
///        CPU is legal and must not fire).
bool spinlock_contract_check_clean(const char *where) noexcept;

/// @brief Test-isolation reset (clears stacks, counters, violations).
void spinlock_order_reset_for_test() noexcept;

/// @brief Held-lock census for INV-L1 / INV-C1 predicates + diagnostics.
uint64_t spinlock_held_by_current_count() noexcept;

/// @brief Find a live order-stack entry by lock address (diagnostic only).
///        Returns true with the recorded owner task id and rank when a
///        matching take has no paired release — i.e. the holder that must
///        release it.  Used by the park watchdog path to name an abandoned
///        holder without GDB.  False when no entry matches (balanced or
///        never-taken): the caller must not infer abandonment from false.
bool spinlock_find_holder(const void *addr, uint64_t *owner_out,
                          uint64_t *rank_out) noexcept;

/// @brief Diagnostics accessors (per (outer,inner) pair, overflow,
///        unpaired-release, park/switch counters live in park_lock).
uint64_t spinlock_violation_count(uint8_t outer_rank,
                                  uint8_t inner_rank) noexcept;
uint64_t spinlock_overflow_count() noexcept;
uint64_t spinlock_unpaired_release_count() noexcept;

#else // !CONFIG_DEBUG

inline void spinlock_note_acquire(const void *addr,
                                  uint8_t rank) noexcept {
    (void)addr;
    (void)rank;
    spinlock_held_bump();
}

inline void spinlock_note_release(const void *addr) noexcept {
    (void)addr;
    spinlock_held_drop();
}

inline bool spinlock_contract_check_clean(const char *where) noexcept {
    (void)where;
    return spinlock_held_by_current_count() == 0;
}

inline void spinlock_order_reset_for_test() noexcept {}
inline uint64_t spinlock_held_by_current_count() noexcept {
    return spinlock_held_depth_own();
}
inline bool spinlock_find_holder(const void *addr, uint64_t *owner_out,
                                 uint64_t *rank_out) noexcept {
    (void)addr;
    if (owner_out)
        *owner_out = 0;
    if (rank_out)
        *rank_out = 0;
    return false;
}
inline uint64_t spinlock_violation_count(uint8_t outer_rank,
                                         uint8_t inner_rank) noexcept {
    (void)outer_rank;
    (void)inner_rank;
    return 0;
}
inline uint64_t spinlock_overflow_count() noexcept { return 0; }
inline uint64_t spinlock_unpaired_release_count() noexcept { return 0; }

#endif // CONFIG_DEBUG

} // namespace sync
} // namespace kernel
