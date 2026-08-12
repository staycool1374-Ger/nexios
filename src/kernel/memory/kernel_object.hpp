#pragma once

/*
 * NexIOS RTOS — Intrusive shared-reference counting for kernel objects
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

/// @brief Intrusive shared-reference-counted base for kernel objects.
///
/// Every reference-counted kernel object derives from this base.  It provides:
///   - a virtual `dispose()` hook invoked exactly once, on the LAST release
///     (the 1→0 transition), so each object self-describes its final teardown;
///   - a virtual `revoke()` hook for capability-backed objects (ROADMAP 0.4.1
///     CSpace): after revoke(), new `acquire()` calls are refused and the
///     object's capability references are invalidated;
///   - an atomic reference count (RELAXED acquire, ACQ_REL release);
///   - intrusive per-task object-list links so a TaskControlBlock can track
///     every object it owns and drive teardown by walking its list.
///
/// Ownership classes (see docs):
///   - **Private-owned heap** (e.g. SporadicServer): the TCB list holds the
///     only long-lived reference; transient holders pin with ScopedRef.
///   - **Shared heap** (e.g. PipeBuffer, future capability objects / CSpace
///     endpoints): every holder calls acquire()/release(); dispose() runs on
///     the last release regardless of which CPU or task drops it.
///   - **Embedded objects** (per-task MessageQueue/Notify/EventGroup): are NOT
///     derived from this base — their storage lives inside the TCB block and
///     cannot outlive it, so a refcount would be a constant that invites
///     dangling-reference misuse.  Cross-task references into embedded objects
///     are raw and detached at owner teardown.
///
/// This base is the reference-counting primitive that ROADMAP 0.4.1 (CSpace,
/// capability lifecycle via SYS_CAP_GRANT/COPY/REVOKE) builds on: a capability
/// holder takes an acquire() reference; revoke() is the deterministic
/// revocation hook; dispose() is the last-reference teardown.
///
/// @note The virtual destructor is deliberately non-virtual (protected):
///       objects are freed through their own dispose() (which knows the exact
///       concrete type and storage class), never through a base pointer, so no
///       `delete` through the base is possible.  The vtable is emitted at the
///       derived key function (dispose()) — the kernel's first production
///       vtables.
class KernelObject {
  public:
    /// @brief Final teardown, invoked exactly once on the last release.
    ///        Each derived object implements its own disposal: pool-backed
    ///        heap objects MemPool::free(this); shared objects drop their
    ///        per-holder accounting first.  Must be CPU-agnostic (may run on
    ///        any CPU on SMP).
    virtual void dispose() noexcept = 0;

    /// @brief Revocation hook for capability-backed objects (ROADMAP 0.4.1
    ///        CSpace).  Default no-op.  A derived object overrides this to
    ///        mark itself revoked; after revoke(), acquire() refuses and the
    ///        object's capability references are invalidated.  Called under
    ///        the object's own serialization.
    virtual void revoke() noexcept { revoked_ = true; }

    /// @brief Takes a new reference.  Relaxed ordering is sufficient: the
    ///        reference count is a lifetime guard, not a payload ordering
    ///        barrier.  Refuses after revoke() (capability revocation).
    /// @return true if a reference was taken, false if the object is revoked
    ///         (caller must not use the pointer).
    bool acquire() noexcept {
        if (__atomic_load_n(&revoked_, __ATOMIC_RELAXED))
            return false;
        __atomic_fetch_add(&ref_count_, 1U, __ATOMIC_RELAXED);
        return true;
    }

    /// @brief Drops one reference.  The last releaser (the one that observes
    ///        the 1→0 transition) invokes dispose().
    /// @note Branch-free on the hot path; the double-release invariant is
    ///       asserted at the teardown site (TaskControlBlock::release_all_
    ///       objects), not here, so no scheduler timing perturbation.
    void release() noexcept {
        if (__atomic_fetch_sub(&ref_count_, 1U, __ATOMIC_ACQ_REL) == 1U) {
            dispose();
        }
    }

    /// @brief Returns the current reference count (debug invariant check).
    uint32_t refcount() const noexcept {
        return __atomic_load_n(&ref_count_, __ATOMIC_RELAXED);
    }

    /// @brief True after revoke() was called (capability revocation).
    bool revoked() const noexcept {
        return __atomic_load_n(&revoked_, __ATOMIC_RELAXED);
    }

    /// @brief True for objects referenced by more than one holder (shared
    ///        ownership class).  The TCB teardown only asserts refcount==1
    ///        for non-shared nodes; shared nodes may hold references beyond
    ///        the TCB's and survive its release.
    virtual bool is_shared() const noexcept { return false; }

    /// @brief Marks the object as pool-backed (factory protocol).  Stack-
    ///        allocated test instances stay unmarked and are never freed by
    ///        dispose().
    void mark_pool_backed() noexcept {
        __atomic_or_fetch(&flags_, kPoolBacked, __ATOMIC_RELAXED);
    }
    bool is_pool_backed() const noexcept {
        return (__atomic_load_n(&flags_, __ATOMIC_RELAXED) & kPoolBacked) != 0;
    }

    /// @brief Intrusive per-task object-list links.  The list is owned by the
    ///        TaskControlBlock and is only ever mutated outside IRQ context.
    KernelObject *task_obj_next_ = nullptr;
    KernelObject *task_obj_prev_ = nullptr;

  protected:
    KernelObject() noexcept = default;
    ~KernelObject() noexcept = default;

  private:
    static constexpr uint8_t kPoolBacked = 0x01;
    volatile uint32_t ref_count_{1};
    volatile uint8_t flags_{0};
    volatile bool revoked_{false};
};

/// @brief RAII scoped reference guard for KernelObject objects.
///
/// Acquires on construction and releases on destruction.  Used to pin a
/// shared/pool-backed object while a code path that is not serialized against
/// the owner's teardown dereferences it.  Safe on shared objects — that is
/// its purpose.  If acquire() refuses (revoked object), the pin is a no-op.
class ScopedRef {
  public:
    explicit ScopedRef(KernelObject *p) noexcept : p_(nullptr) {
        if (p && p->acquire()) {
            p_ = p;
        }
    }
    ~ScopedRef() noexcept {
        if (p_) {
            p_->release();
        }
    }
    ScopedRef(const ScopedRef &) = delete;
    ScopedRef &operator=(const ScopedRef &) = delete;

    /// @brief Whether the pin is live (acquire() succeeded).
    bool valid() const noexcept { return p_ != nullptr; }

  private:
    KernelObject *p_;
};

} // namespace kernel
