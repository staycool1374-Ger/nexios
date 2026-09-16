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

/// @file timer_wheel.cpp
/// @brief Per-CPU O(1) event-timer wheel (issue #17, v0.4.7).

#include <kernel/time/timer_wheel.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>

namespace kernel {
namespace time {

sync::SpinLock TimerWheel::lock_;
TimerWheel::CpuWheel TimerWheel::wheels_[CONFIG_MAX_CPUS];
bool TimerWheel::initialized_ = false;

void TimerWheel::ensure_init_locked() noexcept {
    if (initialized_) {
        return;
    }
    for (uint32_t cpu_idx = 0; cpu_idx < CONFIG_MAX_CPUS; ++cpu_idx) {
        CpuWheel& wheel = wheels_[cpu_idx];
        for (uint32_t slot_idx = 0; slot_idx < kMaxTimersPerCpu;
             ++slot_idx) {
            Slot& slot = wheel.slots[slot_idx];
            slot.armed = false;
            slot.generation = kInvalidGeneration;
            slot.expiry_ns = 0;
            slot.callback = nullptr;
            slot.context = nullptr;
            slot.next_free = slot_idx + 1;
        }
        wheel.slots[kMaxTimersPerCpu - 1].next_free = kInvalidSlot;
        wheel.free_head = 0;
        wheel.live_count = 0;
    }
    initialized_ = true;
}

bool TimerWheel::arm(uint32_t cpu, uint64_t expiry_ns,
                     ExpireCallback callback, void* context,
                     Handle* out) noexcept {
    if (cpu >= CONFIG_MAX_CPUS || callback == nullptr || out == nullptr) {
        return false;
    }
    // SIL 3 fail-closed (audit #17): production services only the BSP
    // wheel — Scheduler::on_tick() returns early for APs and ap_tick()
    // has no TimerWheel hook (dispatch-only). An armed AP timer would
    // report success yet never expire, so reject it outright until AP
    // servicing lands.
    if (cpu != 0) {
        return false;
    }
    sync::IrqSpinLockGuard guard(lock_);
    ensure_init_locked();
    CpuWheel& wheel = wheels_[cpu];
    if (wheel.free_head == kInvalidSlot) {
        return false;
    }
    const uint32_t slot_idx = wheel.free_head;
    Slot& slot = wheel.slots[slot_idx];
    wheel.free_head = slot.next_free;
    ++slot.generation;
    if (slot.generation == kInvalidGeneration) {
        ++slot.generation;
    }
    slot.armed = true;
    slot.expiry_ns = expiry_ns;
    slot.callback = callback;
    slot.context = context;
    slot.next_free = kInvalidSlot;
    ++wheel.live_count;
    out->cpu = static_cast<uint8_t>(cpu);
    out->slot = static_cast<uint8_t>(slot_idx);
    out->generation = slot.generation;
    return true;
}

bool TimerWheel::cancel(const Handle& handle) noexcept {
    if (handle.cpu >= CONFIG_MAX_CPUS || handle.slot >= kMaxTimersPerCpu) {
        return false;
    }
    sync::IrqSpinLockGuard guard(lock_);
    ensure_init_locked();
    CpuWheel& wheel = wheels_[handle.cpu];
    Slot& slot = wheel.slots[handle.slot];
    if (!slot.armed || slot.generation != handle.generation) {
        return false;
    }
    slot.armed = false;
    slot.callback = nullptr;
    slot.context = nullptr;
    slot.next_free = wheel.free_head;
    wheel.free_head = handle.slot;
    --wheel.live_count;
    return true;
}

uint32_t TimerWheel::on_tick(uint64_t now_ns, uint32_t cpu) noexcept {
    if (cpu >= CONFIG_MAX_CPUS) {
        return 0;
    }
    ExpireCallback callbacks[kMaxExpirePerTick];
    void* contexts[kMaxExpirePerTick];
    uint32_t fire_count = 0;
    {
        // ISR-safe: IF is already clear on the tick path; try_lock skips
        // on contention instead of spinning on a task-held lock
        // (TlbShootdown precedent). Skipped expiries stay armed for the
        // next tick. The IRQ-off window covers only the bounded scan:
        // callbacks fire after restore (audit #17). Straight-line
        // pairing: no returns between try_lock and unlock, so the manual
        // unlock cannot leak the lock.
        arch::IrqGuard irq_guard{};
        if (!lock_.try_lock()) {
            return 0;
        }
        ensure_init_locked();
        CpuWheel& wheel = wheels_[cpu];
        uint8_t due_idx[kMaxTimersPerCpu];
        uint32_t due_count = 0;
        for (uint32_t slot_idx = 0; slot_idx < kMaxTimersPerCpu;
             ++slot_idx) {
            const Slot& slot = wheel.slots[slot_idx];
            if (slot.armed && slot.expiry_ns <= now_ns) {
                due_idx[due_count] = static_cast<uint8_t>(slot_idx);
                ++due_count;
            }
        }
        // Earliest-first selection of at most kMaxExpirePerTick entries.
        uint8_t fire_idx[kMaxExpirePerTick];
        bool taken[kMaxTimersPerCpu] = {};
        while (fire_count < kMaxExpirePerTick && fire_count < due_count) {
            uint32_t best_pos = kInvalidSlot;
            uint64_t best_expiry = 0;
            for (uint32_t pos = 0; pos < due_count; ++pos) {
                if (taken[pos]) {
                    continue;
                }
                const uint64_t cand =
                    wheel.slots[due_idx[pos]].expiry_ns;
                if (best_pos == kInvalidSlot || cand < best_expiry) {
                    best_pos = pos;
                    best_expiry = cand;
                }
            }
            if (best_pos == kInvalidSlot) {
                break;
            }
            taken[best_pos] = true;
            fire_idx[fire_count] = due_idx[best_pos];
            ++fire_count;
        }
        for (uint32_t fire_pos = 0; fire_pos < fire_count; ++fire_pos) {
            Slot& slot = wheel.slots[fire_idx[fire_pos]];
            callbacks[fire_pos] = slot.callback;
            contexts[fire_pos] = slot.context;
            slot.armed = false;
            slot.callback = nullptr;
            slot.context = nullptr;
            slot.next_free = wheel.free_head;
            wheel.free_head = fire_idx[fire_pos];
            --wheel.live_count;
        }
        lock_.unlock();
    }
    for (uint32_t fire_pos = 0; fire_pos < fire_count; ++fire_pos) {
        callbacks[fire_pos](contexts[fire_pos]);
    }
    return fire_count;
}

uint32_t TimerWheel::live_count(uint32_t cpu) noexcept {
    if (cpu >= CONFIG_MAX_CPUS) {
        return 0;
    }
    sync::IrqSpinLockGuard guard(lock_);
    ensure_init_locked();
    return wheels_[cpu].live_count;
}

void TimerWheel::snapshot_reset() noexcept {
    sync::IrqSpinLockGuard guard(lock_);
    ensure_init_locked();
    for (uint32_t cpu_idx = 0; cpu_idx < CONFIG_MAX_CPUS; ++cpu_idx) {
        CpuWheel& wheel = wheels_[cpu_idx];
        for (uint32_t slot_idx = 0; slot_idx < kMaxTimersPerCpu;
             ++slot_idx) {
            Slot& slot = wheel.slots[slot_idx];
            slot.armed = false;
            ++slot.generation;
            if (slot.generation == kInvalidGeneration) {
                ++slot.generation;
            }
            slot.expiry_ns = 0;
            slot.callback = nullptr;
            slot.context = nullptr;
            slot.next_free = slot_idx + 1;
        }
        wheel.slots[kMaxTimersPerCpu - 1].next_free = kInvalidSlot;
        wheel.free_head = 0;
        wheel.live_count = 0;
    }
}

} // namespace time
} // namespace kernel
