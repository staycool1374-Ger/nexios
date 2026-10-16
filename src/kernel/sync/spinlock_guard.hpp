#pragma once

#include <types.hpp>
#include <concepts.hpp>

/// @brief Tag type for SpinLockGuard adopt-lock constructor.
struct adopt_lock_t {};
/// @brief Tag value indicating the lock is already held by the caller.
constexpr adopt_lock_t adopt_lock{};

/// @brief RAII lock guard for any lock type with lock()/unlock() methods.
/// @tparam Lock Lock type satisfying kernel::Lockable (issue #348-E).
template <kernel::Lockable Lock> class [[nodiscard]] SpinLockGuard {
  public:
    /// @brief Acquire the lock on construction.
    explicit SpinLockGuard(Lock &lock) noexcept : lock_(lock), owns_(true) {
        lock_.lock();
    }

    /// @brief Adopt an already-acquired lock (caller must have called
    ///        lock() or try_lock() successfully before constructing).
    ///        The destructor will call unlock().
    explicit SpinLockGuard(Lock &lock, adopt_lock_t) noexcept
        : lock_(lock), owns_(true) {
    }

    ~SpinLockGuard() noexcept {
        if (owns_)
            lock_.unlock();
    }

    /// @brief True if this guard holds the lock and will release it.
    bool owns_lock() const noexcept { return owns_; }

    /// @brief Release the lock early (guard becomes a no-op on destruction).
    void unlock() noexcept {
        if (owns_) {
            lock_.unlock();
            owns_ = false;
        }
    }

    SpinLockGuard(const SpinLockGuard &) = delete;
    SpinLockGuard &operator=(const SpinLockGuard &) = delete;
    SpinLockGuard(SpinLockGuard &&) = delete;
    SpinLockGuard &operator=(SpinLockGuard &&) = delete;

  private:
    Lock &lock_;
    bool owns_ = false;
};
