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

/// @file tlb_shootdown.cpp
/// @brief Lazy TLB shootdown request queue + deferred-free quarantine
///        (issue #158).  See the header for the contract.

#include <kernel/memory/tlb_shootdown.hpp>
#include <kernel/memory/pmm.hpp>
#include <constants.hpp>
#include <kernel/arch/page_table.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>

namespace kernel {

uint64_t TlbShootdown::timeout_ticks_ = TlbShootdown::DEFAULT_TIMEOUT_TICKS;
TlbShootdown::Request TlbShootdown::pending_[TlbShootdown::MAX_PENDING] = {};
uint64_t TlbShootdown::pending_count_ = 0;
TlbShootdown::Held TlbShootdown::quarantine_[TlbShootdown::MAX_QUARANTINE] = {};
uint64_t TlbShootdown::quarantine_count_ = 0;
sync::SpinLock shootdown_lock_{};

TlbShootdown::Error TlbShootdown::request(uint64_t va, uint16_t pcid) noexcept {
    if ((va & (arch::PAGE_SIZE - 1)) != 0)
        return Error::INVALID;
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    for (uint64_t i = 0; i < pending_count_; ++i) {
        if (pending_[i].va == va && pending_[i].pcid == pcid)
            return Error::OK; // exact duplicate: already covered
    }
    if (pending_count_ >= MAX_PENDING)
        return Error::QUEUE_FULL;
    pending_[pending_count_].va = va;
    pending_[pending_count_].pcid = pcid;
    ++pending_count_;
    return Error::OK;
}

void TlbShootdown::coalesce_and_apply() noexcept {
    // Snapshot under lock; apply without holding it (INVLPG/purge take
    // no locks; keeps the critical section short).
    Request batch[MAX_PENDING];
    uint64_t count = 0;
    {
        sync::IrqSpinLockGuard guard(shootdown_lock_);
        count = pending_count_;
        for (uint64_t i = 0; i < count; ++i)
            batch[i] = pending_[i];
        pending_count_ = 0;
    }
    // Group contiguous same-PCID runs: a run at/above threshold earns
    // one context purge, otherwise per-VA flushes.  O(n^2) bounded by
    // MAX_PENDING (64^2 worst case, no allocation).
    bool consumed[MAX_PENDING] = {};
    for (uint64_t i = 0; i < count; ++i) {
        if (consumed[i])
            continue;
        uint64_t group = 1;
        for (uint64_t j = i + 1; j < count; ++j) {
            if (!consumed[j] && batch[j].pcid == batch[i].pcid)
                ++group;
        }
        if (group >= PURGE_GROUP_THRESHOLD) {
            arch::tlb_purge_context(batch[i].pcid);
            for (uint64_t j = i; j < count; ++j) {
                if (!consumed[j] && batch[j].pcid == batch[i].pcid)
                    consumed[j] = true;
            }
        } else {
            arch::ArchPageTable::tlb_flush(batch[i].va);
            consumed[i] = true;
        }
    }
}

TlbShootdown::Error TlbShootdown::hold(uint64_t phys) noexcept {
    if ((phys & (arch::PAGE_SIZE - 1)) != 0 || phys == 0)
        return Error::INVALID;
    if (!PMM::is_allocated(phys))
        return Error::INVALID;
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    if (quarantine_count_ >= MAX_QUARANTINE)
        return Error::QUARANTINE_FULL;
    quarantine_[quarantine_count_].phys = phys;
    quarantine_[quarantine_count_].deadline =
        arch::Timer::ticks() + timeout_ticks_;
    ++quarantine_count_;
    return Error::OK;
}

void TlbShootdown::release_all() noexcept {
    // Apply-first ordering: any queued shootdown for a held page lands
    // before the page returns to the free list (stale entries can never
    // meet recycled frames).
    coalesce_and_apply();
    uint64_t held[MAX_QUARANTINE];
    uint64_t count = 0;
    {
        sync::IrqSpinLockGuard guard(shootdown_lock_);
        count = quarantine_count_;
        for (uint64_t i = 0; i < count; ++i)
            held[i] = quarantine_[i].phys;
        quarantine_count_ = 0;
    }
    for (uint64_t i = 0; i < count; ++i)
        PMM::free_page(held[i]);
}

void TlbShootdown::on_tick(uint64_t now) noexcept {
    // ISR-safe: IF is already clear on the tick path; try_lock skips on
    // contention instead of spinning on a task-held lock (single-CPU
    // deadlock otherwise).  Bounded: at most MAX_PER_TICK releases.
    arch::IrqGuard irq_guard{};
    if (!shootdown_lock_.try_lock())
        return;
    uint64_t due[MAX_PER_TICK];
    uint64_t due_count = 0;
    uint64_t kept = 0;
    for (uint64_t i = 0; i < quarantine_count_; ++i) {
        bool expired = (now >= quarantine_[i].deadline);
        if (expired && due_count < MAX_PER_TICK) {
            due[due_count++] = quarantine_[i].phys;
        } else {
            quarantine_[kept] = quarantine_[i];
            ++kept;
        }
    }
    quarantine_count_ = kept;
    shootdown_lock_.unlock();
    // Apply + free outside the lock (PMM::free_page takes pmm_lock_;
    // leaf-last order, never nested).
    if (due_count > 0)
        coalesce_and_apply();
    for (uint64_t i = 0; i < due_count; ++i)
        PMM::free_page(due[i]);
}

uint64_t TlbShootdown::pending_count() noexcept {
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    return pending_count_;
}

uint64_t TlbShootdown::quarantine_count() noexcept {
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    return quarantine_count_;
}

void TlbShootdown::set_timeout_ticks_for_test(uint64_t ticks) noexcept {
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    timeout_ticks_ = ticks;
}

void TlbShootdown::reset() noexcept {
    sync::IrqSpinLockGuard guard(shootdown_lock_);
    pending_count_ = 0;
    quarantine_count_ = 0;
    timeout_ticks_ = DEFAULT_TIMEOUT_TICKS;
}

} // namespace kernel
