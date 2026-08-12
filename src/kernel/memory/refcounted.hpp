#pragma once

/*
 * NexIOS RTOS — Intrusive reference counting for pool-backed kernel objects
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

#include <types.hpp>

namespace kernel {

/// @brief Object-kind tags for kind-specific teardown hooks.
/// @note Used instead of RTTI (kernel is compiled -fno-rtti).
enum ObjectKind : uint8_t {
    KIND_NONE = 0,
    KIND_SPORADIC_SERVER = 1,
    KIND_COUNT,
};

/// @brief Intrusive reference-counted base for MemPool-backed kernel objects.
///
/// Every heap-allocated object owned by a task derives from this base, which
/// provides:
///   - an atomic reference count (acquire/release idiom);
///   - a stored disposer function invoked on the last release (defaults to
///     MemPool::free for pool-backed instances);
///   - intrusive per-task list links so the owning TaskControlBlock can track
///     every live object and drive teardown by walking its object list.
///
/// @note Stack-allocated instances (tests) leave the disposer null; an
///       accidental release() is then a no-op and never frees the block.
class RefCounted {
  public:
    /// @brief Function invoked when the last reference is released.
    using Disposer = void (*)(RefCounted *) noexcept;

    /// @brief Default disposer: returns the block to the kernel MemPool.
    static void dispose_via_mempool(RefCounted *self) noexcept;

    /// @brief Takes a new reference. Relaxed ordering is sufficient: the
    ///        reference count is only used as a lifetime guard, not as an
    ///        ordering barrier for the object's payload.
    void acquire() noexcept { __atomic_fetch_add(&ref_count_, 1U, __ATOMIC_RELAXED); }

    /// @brief Drops one reference. The last releaser (the one that observes
    ///        the transition to zero) invokes the disposer.
    /// @note Deliberately branch-free on the hot path (on_tick ScopedRef):
    ///       the double-release invariant is asserted at the teardown site
    ///       (TaskControlBlock::release_all_objects) instead, so no scheduler
    ///       timing perturbation is introduced here.
    void release() noexcept {
        if (__atomic_fetch_sub(&ref_count_, 1U, __ATOMIC_ACQ_REL) == 1U) {
            Disposer d = __atomic_load_n(&disposer_, __ATOMIC_RELAXED);
            if (d) {
                d(this);
            }
        }
    }

    /// @brief Restores the ownership reference after a memset-zero.
    void reset_refcount() noexcept { __atomic_store_n(&ref_count_, 1U, __ATOMIC_RELAXED); }

    /// @brief Installs the disposer (factory protocol, after reset_refcount).
    void set_disposer(Disposer d) noexcept { disposer_ = d; }

    /// @brief Kind tag for kind-specific teardown hooks.
    uint8_t kind() const noexcept { return kind_; }
    void set_kind(uint8_t k) noexcept { kind_ = k; }

    /// @brief Returns the current reference count (debug invariant check).
    uint32_t refcount() const noexcept {
        return __atomic_load_n(&ref_count_, __ATOMIC_RELAXED);
    }

    /// @brief Intrusive per-task object-list links. The list is owned by the
    ///        TaskControlBlock and is only ever mutated outside IRQ context.
    RefCounted *task_obj_next_ = nullptr;
    RefCounted *task_obj_prev_ = nullptr;

  protected:
    volatile uint32_t ref_count_{1};
    Disposer disposer_{nullptr};
    uint8_t kind_{KIND_NONE};
};

/// @brief RAII scoped reference guard for RefCounted objects.
///
/// Acquires on construction and releases on destruction.  Used to pin a
/// pool-backed object (e.g. a SporadicServer) while a code path that is not
/// serialized against the task's teardown dereferences it.
class ScopedRef {
  public:
    explicit ScopedRef(RefCounted *p) noexcept : p_(p) {
        if (p_) {
            p_->acquire();
        }
    }
    ~ScopedRef() noexcept {
        if (p_) {
            p_->release();
        }
    }
    ScopedRef(const ScopedRef &) = delete;
    ScopedRef &operator=(const ScopedRef &) = delete;

  private:
    RefCounted *p_;
};

} // namespace kernel
