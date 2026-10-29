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

/// @file ring_buffer.hpp
/// @brief Lock-free SPSC character ring buffer for kernel console (g_klog).

#include <types.hpp>
#include <lib/atomic.hpp>

namespace kernel {
namespace log {

/// @brief Lock-free single-producer single-consumer character ring buffer.
class RingBuffer {
  public:
    static constexpr size_t BUFFER_SIZE = 32768; ///< Total capacity in bytes.

    /// @brief Write a single character (discards if full).
    /// Issue #386 item 1: the slot claim is a CAS loop, not a plain
    /// read + store. Two producers (BSP + AP, or an IRQ-preempted task
    /// plus its preempter) racing the old sequence claimed the SAME slot
    /// and the second store clobbered the first byte (torn dmesg
    /// entries). The CAS publishes exactly one winner per slot; the
    /// loser re-reads the fresh index and retries, so every byte lands
    /// exactly once. Uncontended (SPSC) cost is one CAS — no locks, no
    /// added branches on the fast path beyond the pre-existing
    /// full-check.
    void putchar(char c) {
        size_t w = atomic_load(&write_pos_, __ATOMIC_RELAXED);
        for (;;) {
            size_t next = (w + 1) % BUFFER_SIZE;
            if (next == atomic_load(&read_pos_, __ATOMIC_ACQUIRE))
                return;
            if (__atomic_compare_exchange_n(&write_pos_, &w, next, false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
                break;
        }
        buf_[w] = c;
    }

    /// @brief Write a null-terminated string.
    void puts(const char *s) {
        while (*s)
            putchar(*s++);
    }

    /// @brief Read up to @p size bytes into @p dst.
    /// @return number of bytes read.
    size_t read(char *dst, size_t size) {
        size_t r = atomic_load(&read_pos_, __ATOMIC_RELAXED);
        size_t w = atomic_load(&write_pos_, __ATOMIC_ACQUIRE);
        size_t written = 0;
        while (r != w && written < size) {
            *dst++ = buf_[r];
            r = (r + 1) % BUFFER_SIZE;
            ++written;
        }
        atomic_store(&read_pos_, r, __ATOMIC_RELEASE);
        return written;
    }

    /// @brief Discard all buffered data.
    void clear() {
        atomic_store(&read_pos_, atomic_load(&write_pos_, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
    }

    /// @brief Check whether the buffer contains no data.
    bool empty() const {
        return atomic_load(&read_pos_, __ATOMIC_ACQUIRE) ==
               atomic_load(&write_pos_, __ATOMIC_ACQUIRE);
    }

    RingBuffer() : buf_{} {
    }

  private:
    char buf_[BUFFER_SIZE];                     ///< Data storage.
    alignas(64) volatile size_t write_pos_ = 0; ///< Producer index.
    alignas(64) volatile size_t read_pos_ = 0;  ///< Consumer index.
};

/// @brief Sole owner of the kernel log (klog) ring.
///
/// Replaces the former mutable global object `g_klog`. The character ring
/// lives as a private member; the kernel console producer writes via
/// putchar()/puts(), and consumers (SYS_KLOG readers, tests) drain via
/// read()/clear().
class KlogService {
  public:
    KlogService(const KlogService &) = delete;
    KlogService &operator=(const KlogService &) = delete;

    /// @brief Get the singleton service instance.
    static KlogService &instance() noexcept;

    /// @brief Write a single character (discards if full).
    void putchar(char c) noexcept {
        buffer_.putchar(c);
    }

    /// @brief Write a null-terminated string.
    void puts(const char *s) noexcept {
        buffer_.puts(s);
    }

    /// @brief Read up to @p size bytes into @p dst.
    /// @return number of bytes read.
    size_t read(char *dst, size_t size) noexcept {
        return buffer_.read(dst, size);
    }

    /// @brief Discard all buffered data.
    void clear() noexcept {
        buffer_.clear();
    }

    /// @brief Check whether the buffer contains no data.
    bool empty() const noexcept {
        return buffer_.empty();
    }

  private:
    /// @brief Private ctor — only instance() may create the service.
    KlogService() = default;

    /// @brief The encapsulated character ring (no external linkage).
    RingBuffer buffer_{};
};

} // namespace log
} // namespace kernel
