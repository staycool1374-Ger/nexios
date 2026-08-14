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

/// @file dmesg.hpp
/// @brief Lock-free SPSC structured kernel log (dmesg) — LogEntry, DmesgBuffer,
/// DmesgService singleton, error-string helpers.

#include <types.hpp>
#include <lib/error.hpp>
#include <lib/utils.hpp>
#include <kernel/sync/sync_errors.hpp>
#include <kernel/vfs/vfs_errors.hpp>
#include <kernel/memory/mempool_errors.hpp>
#include <kernel/task/scheduler_errors.hpp>
#include <kernel/ipc/ipc_errors.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/nexios_config.h>
#include <lib/atomic.hpp>

namespace kernel::log {

/// @brief Subsystem identifier used in dmesg entries (maps to per-subsystem
/// error_string).
enum class ErrorSubsystem : uint8_t {
    BASE = 0,    ///< Generic kernel errors.
    SYNC = 1,    ///< Synchronisation primitives.
    VFS = 2,     ///< Virtual file system.
    MEMPOOL = 3, ///< Memory pool allocator.
    SCHED = 4,   ///< Scheduler.
    IPC = 5,     ///< Inter-process communication.
    SYSCALL = 6, ///< System call interface.
};

/// @brief A single dmesg log entry.
struct LogEntry {
    static constexpr size_t kMessageCap = 96;
    uint64_t timestamp;       ///< Tick count at log time.
    uint64_t task_id;         ///< ID of the task that logged the entry.
    ErrorSubsystem subsystem; ///< Subsystem that generated the error.
    uint64_t error_code;      ///< Subsystem-specific error code.
    uintptr_t context;   ///< Optional context pointer (e.g. address involved).
    char message[kMessageCap]; ///< Human-readable description (owned copy).
};

/// @brief Lock-free single-producer single-consumer ring buffer for structured
/// kernel logs.
/// @tparam Capacity  Must be a power of two.
template <size_t Capacity> class DmesgBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    LogEntry buffer[Capacity];           ///< Fixed-size entry array.
    alignas(64) volatile size_t head{0}; ///< Write index (producer).
    alignas(64) volatile size_t tail{0}; ///< Read index (consumer).

  public:
    /// @brief Push an entry (overwrites oldest if full).
    /// @return true unless an entry was overwritten.
    bool push(ErrorSubsystem subsys, uint64_t err_code, const char *msg,
              uintptr_t ctx = 0);
    /// @brief Pop the oldest entry.
    /// @return true if an entry was available.
    bool pop(LogEntry &entry);

    /// @brief Iterate over all entries without removing them.
    template <typename Fn> void for_each(Fn &&fn) const {
        size_t t = atomic_load(&tail, __ATOMIC_ACQUIRE);
        size_t h = atomic_load(&head, __ATOMIC_ACQUIRE);

        while (t != h) {
            fn(buffer[t]);
            t = (t + 1) & MASK;
        }
    }

    /// @brief Check whether the buffer is empty.
    bool empty() const;
    /// @brief Return the number of entries currently in the buffer.
    size_t size() const;
    /// @brief Discard all entries (reset tail to head).
    void clear();

    /// @brief Suppress future pushes (release mode: quiet boot).
    static void set_suppressed(bool v) {
        s_suppressed_ = v;
    }
    /// @brief Check whether pushes are currently suppressed.
    static bool is_suppressed() {
        return s_suppressed_;
    }

    size_t head_index() const {
        return head;
    }
    size_t tail_index() const {
        return tail;
    }

  private:
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static inline bool s_suppressed_ =
#ifdef CONFIG_DEBUG
        false
#else
        true
#endif
        ;
};

/// Capacity of the global dmesg ring buffer (from Kconfig).
constexpr size_t DMESG_CAPACITY = CONFIG_DMESG_CAPACITY;

/// @brief Sole owner of the kernel dmesg ring.
///
/// Replaces the former mutable global object `g_dmesg`. The buffer lives as a
/// private member; producers write through push(), the dmesg_task drains via
/// pop(), and readers iterate with for_each(). All access is routed through
/// this service so the buffer's lifecycle and suppression policy are
/// encapsulated.
class DmesgService {
  public:
    DmesgService(const DmesgService &) = delete;
    DmesgService &operator=(const DmesgService &) = delete;

    /// @brief Get the singleton service instance.
    static DmesgService &instance() noexcept;

    /// @brief Push a structured entry. Thread-safe (SPSC atomics).
    /// @return true unless an entry was overwritten.
    bool push(ErrorSubsystem subsys, uint64_t err_code, const char *msg,
              uintptr_t ctx = 0) noexcept {
        return buffer_.push(subsys, err_code, msg, ctx);
    }

    /// @brief Pop the oldest entry (dmesg_task consumer side).
    /// @return true if an entry was available.
    bool pop(LogEntry &entry) noexcept {
        return buffer_.pop(entry);
    }

    /// @brief Iterate over all entries without removing them.
    template <typename Fn> void for_each(Fn &&fn) const {
        buffer_.for_each(::forward<Fn>(fn));
    }

    /// @brief Discard all entries.
    void clear() noexcept {
        buffer_.clear();
    }

    /// @brief Check whether the buffer is empty.
    bool empty() const noexcept {
        return buffer_.empty();
    }

    /// @brief Return the number of entries currently in the buffer.
    size_t size() const noexcept {
        return buffer_.size();
    }

    /// @brief Suppress future pushes (release mode: quiet boot).
    void set_suppressed(bool v) noexcept {
        buffer_.set_suppressed(v);
    }

    /// @brief Check whether pushes are currently suppressed.
    bool is_suppressed() const noexcept {
        return buffer_.is_suppressed();
    }

    size_t head_index() const noexcept {
        return buffer_.head_index();
    }

    size_t tail_index() const noexcept {
        return buffer_.tail_index();
    }

  private:
    /// @brief Private ctor — only instance() may create the service.
    DmesgService() = default;

    /// @brief The encapsulated ring buffer (no external linkage).
    DmesgBuffer<DMESG_CAPACITY> buffer_{};
};

/// @brief Shorthand: push an entry to the kernel dmesg service.
inline bool dmesg_push(ErrorSubsystem subsys, uint64_t err, const char *msg,
                       uintptr_t ctx = 0) noexcept {
    return DmesgService::instance().push(subsys, err, msg, ctx);
}

/// @brief Shorthand: push a base-subsystem error to the kernel dmesg service.
inline bool dmesg_push_base(uint64_t err, const char *msg,
                            uintptr_t ctx = 0) noexcept {
    return DmesgService::instance().push(ErrorSubsystem::BASE, err, msg, ctx);
}

/// @brief Return a human-readable string for a base-subsystem error code.
///        The code space is split into kernel::Error values (0–9) and
///        a custom range for event codes:
///          0xDA00 – 0xDAFF  daemon lifecycle events
///          0xDB00 – 0xDBFF  background ELF loader events
inline const char *base_error_string(uint64_t code) {
    // Custom event ranges
    if ((code & ~0xFFULL) == 0xDA00) {
        switch (code) {
        case 0xDA01:
            return "Daemon exited";
        case 0xDA02:
            return "Daemon restarted";
        case 0xDA03:
            return "Daemon ensured";
        case 0xDA04:
            return "Daemon terminated";
        case 0xDA05:
            return "Daemon restarting";
        case 0xDA06:
            return "Daemon up";
        default:
            break;
        }
        return "Daemon event";
    }
    if ((code & ~0xFFULL) == 0xDB00) {
        switch (code) {
        case 0xDB01:
            return "ELF load started";
        case 0xDB02:
            return "ELF load completed";
        case 0xDB03:
            return "ELF load canceled";
        case 0xDB04:
            return "ELF load failed: invalid elf-file";
        case 0xDB05:
            return "ELF load failed: not enough memory";
        case 0xDB06:
            return "ELF load failed: file not found";
        case 0xDB07:
            return "ELF load failed: read error";
        case 0xDB08:
            return "ELF load rejected: already loading";
        case 0xDB09:
            return "ELF load rejected: not loading";
        default:
            break;
        }
        return "ELF loader event";
    }
    // Standard kernel::Error range (0–9)
    switch (static_cast<kernel::Error>(code)) {
    case kernel::Error::OK:
        return "OK";
    case kernel::Error::OOM:
        return "Out of memory";
    case kernel::Error::INVALID_ARG:
        return "Invalid argument";
    case kernel::Error::NOT_FOUND:
        return "Not found";
    case kernel::Error::ALREADY_EXISTS:
        return "Already exists";
    case kernel::Error::TIMEOUT:
        return "Timeout";
    case kernel::Error::BUSY:
        return "Busy";
    case kernel::Error::NOT_IMPLEMENTED:
        return "Not implemented";
    case kernel::Error::IO_ERROR:
        return "I/O error";
    case kernel::Error::CORRUPTED:
        return "Corrupted";
    }
    return "Unknown base error";
}

/// @brief Return a short name for an ErrorSubsystem.
inline const char *subsystem_name(ErrorSubsystem s) {
    switch (s) {
    case ErrorSubsystem::BASE:
        return "BASE";
    case ErrorSubsystem::SYNC:
        return "SYNC";
    case ErrorSubsystem::VFS:
        return "VFS";
    case ErrorSubsystem::MEMPOOL:
        return "MPOOL";
    case ErrorSubsystem::SCHED:
        return "SCHED";
    case ErrorSubsystem::IPC:
        return "IPC";
    case ErrorSubsystem::SYSCALL:
        return "SYSCALL";
    default:
        return "UNK";
    }
}

/// @brief Dispatch an error code to the correct subsystem's error_string.
inline const char *error_string(ErrorSubsystem subsys, uint64_t code) {
    switch (subsys) {
    case ErrorSubsystem::BASE:
        return base_error_string(code);
    case ErrorSubsystem::SYNC:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SyncError>(code));
    case ErrorSubsystem::VFS:
        return kernel::errors::error_string(
            static_cast<kernel::errors::VfsError>(code));
    case ErrorSubsystem::MEMPOOL:
        return kernel::errors::error_string(
            static_cast<kernel::errors::MemPoolError>(code));
    case ErrorSubsystem::SCHED:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SchedulerError>(code));
    case ErrorSubsystem::IPC:
        return kernel::errors::error_string(
            static_cast<kernel::errors::IpcError>(code));
    case ErrorSubsystem::SYSCALL:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SyscallError>(code));
    default:
        return "UNKNOWN";
    }
}

} // namespace kernel::log