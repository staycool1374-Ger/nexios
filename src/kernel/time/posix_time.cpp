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

/// @file posix_time.cpp
/// @brief POSIX time/timer registries over the HRT clock + wheel (issue #76).

#include <kernel/time/posix_time.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/timer.hpp>
#include <kernel/arch/hal/rtc.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/resource_tracker.hpp>

namespace kernel {
namespace time {

namespace {

constexpr uint64_t kNsPerSec = 1000000000ULL;
constexpr uint64_t kUint64Max = 0xFFFFFFFFFFFFFFFFULL;

} // namespace

sync::SpinLock PosixTime::lock_;
PosixTime::TimerSlot PosixTime::timers_[kMaxPosixTimers];
PosixTime::TimerFdSlot PosixTime::timerfds_[kMaxTimerfds];
bool PosixTime::anchor_latched_ = false;
uint64_t PosixTime::anchor_ns_ = 0;
uint64_t PosixTime::mono_at_anchor_ns_ = 0;

uint64_t PosixTime::sat_add(uint64_t left, uint64_t right) noexcept {
    if (kUint64Max - left < right) {
        return kUint64Max;
    }
    return left + right;
}

bool PosixTime::timespec_to_ns(const TimespecU &spec,
                               uint64_t &out_ns) noexcept {
    if (spec.sec < 0 || spec.nsec < 0 || spec.nsec >= 1000000000) {
        return false;
    }
    const auto secs = static_cast<uint64_t>(spec.sec);
    if (secs > kUint64Max / kNsPerSec) {
        out_ns = kUint64Max;
        return true;
    }
    uint64_t total = secs * kNsPerSec;
    const auto nsec = static_cast<uint64_t>(spec.nsec);
    if (kUint64Max - total < nsec) {
        out_ns = kUint64Max;
        return true;
    }
    out_ns = total + nsec;
    return true;
}

TimespecU PosixTime::ns_to_timespec(uint64_t total_ns) noexcept {
    TimespecU out{};
    uint64_t secs = total_ns / kNsPerSec;
    if (secs > static_cast<uint64_t>(0x7FFFFFFFFFFFFFFFLL)) {
        out.sec = 0x7FFFFFFFFFFFFFFFLL;
        out.nsec = 999999999;
        return out;
    }
    out.sec = static_cast<int64_t>(secs);
    out.nsec = static_cast<int64_t>(total_ns % kNsPerSec);
    return out;
}

void PosixTime::boot_latch(uint64_t epoch_sec) noexcept {
    sync::IrqSpinLockGuard guard(lock_);
    if (anchor_latched_) {
        return;
    }
    if (epoch_sec > kUint64Max / kNsPerSec) {
        anchor_ns_ = kUint64Max;
    } else {
        anchor_ns_ = epoch_sec * kNsPerSec;
    }
    mono_at_anchor_ns_ = arch::Timer::ns_monotonic();
    anchor_latched_ = true;
}

uint64_t PosixTime::clock_monotonic_ns() noexcept {
    return arch::Timer::ns_monotonic();
}

uint64_t PosixTime::clock_realtime_ns() noexcept {
    if (!anchor_latched_) {
        boot_latch(arch::RTC::read_seconds());
    }
    const uint64_t now = arch::Timer::ns_monotonic();
    uint64_t delta = 0;
    if (now >= mono_at_anchor_ns_) {
        delta = now - mono_at_anchor_ns_;
    }
    return sat_add(anchor_ns_, delta);
}

void PosixTime::anchor_reset_for_test() noexcept {
    sync::IrqSpinLockGuard guard(lock_);
    anchor_latched_ = false;
    anchor_ns_ = 0;
    mono_at_anchor_ns_ = 0;
}

bool PosixTime::decode_timer_id(uint64_t timer_id, uint32_t &out_index,
                                uint32_t &out_gen) noexcept {
    const uint32_t index =
        static_cast<uint32_t>(timer_id & 0xFFULL);
    const uint32_t gen =
        static_cast<uint32_t>((timer_id >> kTimerIdIndexBits) & 0xFFFFFFULL);
    if (index >= kMaxPosixTimers || gen == TimerWheel::kInvalidGeneration) {
        return false;
    }
    out_index = index;
    out_gen = gen;
    return true;
}

uint64_t PosixTime::encode_timer_id(uint32_t index, uint32_t gen) noexcept {
    return (static_cast<uint64_t>(gen & 0xFFFFFFU) << kTimerIdIndexBits) |
           static_cast<uint64_t>(index);
}

uint32_t PosixTime::bump_gen(uint32_t old_gen) noexcept {
    uint32_t next = old_gen + 1;
    if (next == TimerWheel::kInvalidGeneration) {
        ++next;
    }
    return next;
}

void PosixTime::free_timer_slot_locked(uint32_t index) noexcept {
    TimerSlot &slot = timers_[index];
    if (slot.armed) {
        TimerWheel::cancel(slot.wheel_h);
        slot.armed = false;
    }
    slot.used = false;
    slot.gen = bump_gen(slot.gen);
    slot.expiries = 0;
    test::ResourceTracker::instance().track_posix_timer_remove();
}

void PosixTime::free_timerfd_slot_locked(uint32_t index) noexcept {
    TimerFdSlot &slot = timerfds_[index];
    if (slot.armed) {
        TimerWheel::cancel(slot.wheel_h);
        slot.armed = false;
    }
    slot.used = false;
    slot.gen = bump_gen(slot.gen);
    slot.expiries = 0;
    // Wake any waiter with EBADF: the scan observes waiter_dead and wakes
    // the task; the reader then sees the generation mismatch (fail-closed).
    if (slot.waiter_armed && slot.waiter != nullptr) {
        slot.waiter_dead = true;
        slot.waiter_done = true;
    }
    test::ResourceTracker::instance().track_posix_timerfd_remove();
}

int64_t PosixTime::timer_create(uint64_t clock_id, uint64_t owner_id,
                                uint32_t owner_gen, uint64_t &out_id) noexcept {
    if (clock_id != static_cast<uint64_t>(PosixClock::MONOTONIC)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    for (uint32_t i = 0; i < kMaxPosixTimers; ++i) {
        TimerSlot &slot = timers_[i];
        if (slot.used) {
            continue;
        }
        slot.used = true;
        slot.gen = bump_gen(slot.gen);
        slot.owner_id = owner_id;
        slot.owner_gen = owner_gen;
        slot.armed = false;
        slot.periodic = false;
        slot.period_ns = 0;
        slot.expiry_ns = 0;
        slot.expiries = 0;
        slot.wheel_h = {0, 0, TimerWheel::kInvalidGeneration};
        out_id = encode_timer_id(i, slot.gen);
        test::ResourceTracker::instance().track_posix_timer_add();
        return 0;
    }
    return -kPosixErrAgain;
}

int64_t PosixTime::timer_settime(uint64_t timer_id, const ItimerspecU &spec,
                                 uint64_t owner_id,
                                 uint32_t owner_gen) noexcept {
    uint32_t index = 0;
    uint32_t gen = 0;
    if (!decode_timer_id(timer_id, index, gen)) {
        return -kPosixErrInvalid;
    }
    uint64_t value_ns = 0;
    uint64_t interval_ns = 0;
    if (!timespec_to_ns(spec.value, value_ns) ||
        !timespec_to_ns(spec.interval, interval_ns)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerSlot &slot = timers_[index];
    if (!slot.used || slot.gen != gen || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return -kPosixErrInvalid;
    }
    if (slot.armed) {
        TimerWheel::cancel(slot.wheel_h);
        slot.armed = false;
    }
    // Zero value = disarm (no arm, slot retained).
    if (value_ns == 0) {
        slot.periodic = false;
        slot.period_ns = 0;
        return 0;
    }
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    uint64_t expiry_ns = sat_add(now_ns, value_ns);
    if (expiry_ns < now_ns) {
        expiry_ns = kUint64Max;
    }
    void *context = reinterpret_cast<void *>(static_cast<uintptr_t>(index));
    if (!TimerWheel::arm(0, expiry_ns, timer_fire, context, &slot.wheel_h)) {
        return -kPosixErrAgain;
    }
    slot.armed = true;
    slot.periodic = (interval_ns != 0);
    slot.period_ns = interval_ns;
    slot.expiry_ns = expiry_ns;
    return 0;
}

int64_t PosixTime::timer_gettime(uint64_t timer_id, ItimerspecU &out_spec,
                                 uint64_t owner_id,
                                 uint32_t owner_gen) noexcept {
    uint32_t index = 0;
    uint32_t gen = 0;
    if (!decode_timer_id(timer_id, index, gen)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    const TimerSlot &slot = timers_[index];
    if (!slot.used || slot.gen != gen || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return -kPosixErrInvalid;
    }
    ItimerspecU out{};
    if (slot.armed) {
        const uint64_t now_ns = arch::Timer::ns_monotonic();
        uint64_t remaining = 0;
        if (slot.expiry_ns > now_ns) {
            remaining = slot.expiry_ns - now_ns;
        }
        out.value = ns_to_timespec(remaining);
    }
    if (slot.periodic) {
        out.interval = ns_to_timespec(slot.period_ns);
    }
    out_spec = out;
    return 0;
}

int64_t PosixTime::timer_delete(uint64_t timer_id, uint64_t owner_id,
                                uint32_t owner_gen) noexcept {
    uint32_t index = 0;
    uint32_t gen = 0;
    if (!decode_timer_id(timer_id, index, gen)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerSlot &slot = timers_[index];
    if (!slot.used || slot.gen != gen || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return -kPosixErrInvalid;
    }
    free_timer_slot_locked(index);
    return 0;
}

int64_t PosixTime::timerfd_slot_alloc(uint64_t clock_id, uint64_t owner_id,
                                      uint32_t owner_gen) noexcept {
    if (clock_id != static_cast<uint64_t>(PosixClock::MONOTONIC)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    for (uint32_t i = 0; i < kMaxTimerfds; ++i) {
        TimerFdSlot &slot = timerfds_[i];
        if (slot.used) {
            continue;
        }
        slot.used = true;
        slot.gen = bump_gen(slot.gen);
        slot.owner_id = owner_id;
        slot.owner_gen = owner_gen;
        slot.armed = false;
        slot.periodic = false;
        slot.period_ns = 0;
        slot.expiry_ns = 0;
        slot.expiries = 0;
        slot.wheel_h = {0, 0, TimerWheel::kInvalidGeneration};
        slot.waiter = nullptr;
        slot.waiter_gen = 0;
        slot.waiter_armed = false;
        slot.waiter_done = false;
        slot.waiter_dead = false;
        test::ResourceTracker::instance().track_posix_timerfd_add();
        return static_cast<int64_t>(i);
    }
    return -kPosixErrAgain;
}

uint32_t PosixTime::timerfd_slot_gen_for_owner(uint32_t index,
                                               uint64_t owner_id,
                                               uint32_t owner_gen) noexcept {
    if (index >= kMaxTimerfds) {
        return 0;
    }
    sync::IrqSpinLockGuard guard(lock_);
    const TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return 0;
    }
    return slot.gen;
}

int64_t PosixTime::timerfd_settime(uint32_t index, uint32_t slot_gen,
                                   const ItimerspecU &spec, uint64_t owner_id,
                                   uint32_t owner_gen) noexcept {
    if (index >= kMaxTimerfds) {
        return -kPosixErrInvalid;
    }
    uint64_t value_ns = 0;
    uint64_t interval_ns = 0;
    if (!timespec_to_ns(spec.value, value_ns) ||
        !timespec_to_ns(spec.interval, interval_ns)) {
        return -kPosixErrInvalid;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || slot.gen != slot_gen || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return -kPosixErrInvalid;
    }
    if (slot.armed) {
        TimerWheel::cancel(slot.wheel_h);
        slot.armed = false;
    }
    if (value_ns == 0) {
        slot.periodic = false;
        slot.period_ns = 0;
        return 0;
    }
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    uint64_t expiry_ns = sat_add(now_ns, value_ns);
    if (expiry_ns < now_ns) {
        expiry_ns = kUint64Max;
    }
    void *context = reinterpret_cast<void *>(static_cast<uintptr_t>(index));
    if (!TimerWheel::arm(0, expiry_ns, timerfd_fire, context, &slot.wheel_h)) {
        return -kPosixErrAgain;
    }
    slot.armed = true;
    slot.periodic = (interval_ns != 0);
    slot.period_ns = interval_ns;
    slot.expiry_ns = expiry_ns;
    return 0;
}

int64_t PosixTime::timerfd_consume(uint32_t index, uint32_t slot_gen,
                                   uint64_t owner_id, uint32_t owner_gen,
                                   uint64_t &out_count) noexcept {
    if (index >= kMaxTimerfds) {
        return -kPosixErrBadFd;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || slot.gen != slot_gen || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return -kPosixErrBadFd;
    }
    if (slot.expiries == 0) {
        return -kPosixErrAgain;
    }
    out_count = slot.expiries;
    slot.expiries = 0;
    return 0;
}

bool PosixTime::timerfd_waiter_arm(uint32_t index, uint32_t slot_gen,
                                   TaskControlBlock &task) noexcept {
    if (index >= kMaxTimerfds) {
        return false;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || slot.gen != slot_gen) {
        return false;
    }
    slot.waiter = &task;
    slot.waiter_gen = task.generation;
    slot.waiter_armed = true;
    slot.waiter_done = false;
    slot.waiter_dead = false;
    return true;
}

void PosixTime::timerfd_waiter_clear(uint32_t index, uint32_t slot_gen,
                                     const TaskControlBlock &task) noexcept {
    if (index >= kMaxTimerfds) {
        return;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (slot.gen != slot_gen || slot.waiter != &task) {
        return;
    }
    slot.waiter = nullptr;
    slot.waiter_armed = false;
    slot.waiter_done = false;
    slot.waiter_dead = false;
}

void PosixTime::timerfd_slot_free(uint32_t index, uint32_t slot_gen,
                                  uint64_t owner_id,
                                  uint32_t owner_gen) noexcept {
    if (index >= kMaxTimerfds) {
        return;
    }
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || slot.owner_id != owner_id ||
        slot.owner_gen != owner_gen) {
        return;
    }
    // Generation 0 is the invalid sentinel (bump_gen never yields it):
    // accept it as an owner-validated wildcard for the create-path
    // rollback, which otherwise leaks the just-allocated slot.
    if (slot_gen != 0 && slot.gen != slot_gen) {
        return;
    }
    free_timerfd_slot_locked(index);
}

bool PosixTime::sleep_arm(TaskControlBlock &task,
                          uint64_t budget_ns) noexcept {
    if (budget_ns == 0) {
        return true;
    }
    __atomic_store_n(&task.sleep_expired, false, __ATOMIC_RELEASE);
    task.sleep_gen = task.generation;
    // Defensive cancel-first: never overwrite a live arm (unreachable
    // under single-arm discipline, closed by construction — recv pattern).
    if (__atomic_load_n(&task.sleep_armed, __ATOMIC_ACQUIRE)) {
        sleep_cancel(task);
    }
    const uint64_t now_ns = arch::Timer::ns_monotonic();
    uint64_t expiry_ns = sat_add(now_ns, budget_ns);
    if (expiry_ns < now_ns) {
        expiry_ns = kUint64Max;
    }
    task.sleep_expiry_ns = expiry_ns;
    if (!TimerWheel::arm(0, expiry_ns, sleep_fire, &task,
                         &task.sleep_handle)) {
        __atomic_store_n(&task.sleep_armed, false, __ATOMIC_RELEASE);
        return false;
    }
    __atomic_store_n(&task.sleep_armed, true, __ATOMIC_RELEASE);
    return true;
}

void PosixTime::sleep_cancel(TaskControlBlock &task) noexcept {
    if (!__atomic_load_n(&task.sleep_armed, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&task.sleep_armed, false, __ATOMIC_RELEASE);
    TimerWheel::cancel(task.sleep_handle);
}

void PosixTime::drain_owner(TaskControlBlock &task) noexcept {
    sleep_cancel(task);
    sync::IrqSpinLockGuard guard(lock_);
    const uint64_t owner_id = task.id;
    const uint32_t owner_gen = task.generation;
    for (uint32_t i = 0; i < kMaxPosixTimers; ++i) {
        TimerSlot &slot = timers_[i];
        if (slot.used && slot.owner_id == owner_id &&
            slot.owner_gen == owner_gen) {
            free_timer_slot_locked(i);
        }
    }
    for (uint32_t i = 0; i < kMaxTimerfds; ++i) {
        TimerFdSlot &slot = timerfds_[i];
        if (slot.used && slot.owner_id == owner_id &&
            slot.owner_gen == owner_gen) {
            free_timerfd_slot_locked(i);
        }
        // Clear waiter registrations held by the dying task (§12.3: no
        // waiter may outlive its task — the scan validates magic + gen,
        // this clear is the deterministic teardown).
        if (slot.waiter_armed && slot.waiter == &task) {
            slot.waiter = nullptr;
            slot.waiter_armed = false;
            slot.waiter_done = false;
            slot.waiter_dead = false;
        }
    }
}

void PosixTime::reapply_wakes(uint32_t sched_cpu) noexcept {
    sync::IrqSpinLockGuard guard(lock_);
    for (uint32_t i = 0; i < kMaxTimerfds; ++i) {
        TimerFdSlot &slot = timerfds_[i];
        if (!slot.waiter_armed || !slot.waiter_done || slot.waiter == nullptr) {
            continue;
        }
        TaskControlBlock *waiter = slot.waiter;
        if (!TaskControlBlock::is_valid(waiter)) {
            slot.waiter = nullptr;
            slot.waiter_armed = false;
            continue;
        }
        if (waiter->generation != slot.waiter_gen) {
            slot.waiter = nullptr;
            slot.waiter_armed = false;
            continue;
        }
        if (Scheduler::queue_target(*waiter) != sched_cpu) {
            continue;
        }
        if (waiter->state == TaskState::BLOCKED) {
            waiter->state = TaskState::READY;
        } else if (waiter->state != TaskState::READY) {
            // RUNNING/TERMINATED/REAPED: never queue (H2 + dead-state
            // filter — mirrors the recv_timeout re-apply guard).
            continue;
        }
        Scheduler::enqueue_ready(*waiter);
        __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        // Consumed: the reader re-arms on its next blocking read.
        slot.waiter = nullptr;
        slot.waiter_armed = false;
        slot.waiter_done = false;
        slot.waiter_dead = false;
    }
}

void PosixTime::sleep_fire(void *context) noexcept {
    auto *task = static_cast<TaskControlBlock *>(context);
    if (!TaskControlBlock::is_valid(task)) {
        return;
    }
    if (task->generation != task->sleep_gen) {
        return;
    }
    if (!__atomic_load_n(&task->sleep_armed, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&task->sleep_expired, true, __ATOMIC_RELEASE);
}

void PosixTime::timer_fire(void *context) noexcept {
    const auto raw = reinterpret_cast<uintptr_t>(context);
    if (raw >= kMaxPosixTimers) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(raw);
    sync::IrqSpinLockGuard guard(lock_);
    TimerSlot &slot = timers_[index];
    if (!slot.used || !slot.armed) {
        return;
    }
    if (slot.expiries < kUint64Max) {
        ++slot.expiries;
    }
    if (!slot.periodic) {
        slot.armed = false;
        return;
    }
    // Periodic re-arm chain: next expiry from the last one (no drift
    // accumulation), saturating. Wheel-full keeps the slot armed with a
    // past expiry — the next tick's fire attempt re-arms (bounded retry
    // by tick rate, no backlog growth).
    uint64_t next_ns = sat_add(slot.expiry_ns, slot.period_ns);
    if (next_ns < slot.expiry_ns) {
        next_ns = kUint64Max;
    }
    void *ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(index));
    if (TimerWheel::arm(0, next_ns, timer_fire, ctx, &slot.wheel_h)) {
        slot.expiry_ns = next_ns;
    }
}

void PosixTime::timerfd_fire(void *context) noexcept {
    const auto raw = reinterpret_cast<uintptr_t>(context);
    if (raw >= kMaxTimerfds) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(raw);
    sync::IrqSpinLockGuard guard(lock_);
    TimerFdSlot &slot = timerfds_[index];
    if (!slot.used || !slot.armed) {
        return;
    }
    if (slot.expiries < kUint64Max) {
        ++slot.expiries;
    }
    if (!slot.periodic) {
        slot.armed = false;
    } else {
        uint64_t next_ns = sat_add(slot.expiry_ns, slot.period_ns);
        if (next_ns < slot.expiry_ns) {
            next_ns = kUint64Max;
        }
        void *ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(index));
        if (TimerWheel::arm(0, next_ns, timerfd_fire, ctx, &slot.wheel_h)) {
            slot.expiry_ns = next_ns;
        }
    }
    if (slot.waiter_armed && slot.waiter != nullptr) {
        slot.waiter_done = true;
    }
}

void PosixTime::snapshot_reset() noexcept {
    sync::IrqSpinLockGuard guard(lock_);
    for (uint32_t i = 0; i < kMaxPosixTimers; ++i) {
        TimerSlot &slot = timers_[i];
        slot.used = false;
        slot.gen = bump_gen(slot.gen);
        slot.armed = false;
        slot.periodic = false;
        slot.expiries = 0;
    }
    for (uint32_t i = 0; i < kMaxTimerfds; ++i) {
        TimerFdSlot &slot = timerfds_[i];
        slot.used = false;
        slot.gen = bump_gen(slot.gen);
        slot.armed = false;
        slot.periodic = false;
        slot.expiries = 0;
        slot.waiter = nullptr;
        slot.waiter_armed = false;
        slot.waiter_done = false;
        slot.waiter_dead = false;
    }
    anchor_latched_ = false;
    anchor_ns_ = 0;
    mono_at_anchor_ns_ = 0;
    test::ResourceTracker::instance().track_posix_timer_reset();
    test::ResourceTracker::instance().track_posix_timerfd_reset();
}

} // namespace time
} // namespace kernel
