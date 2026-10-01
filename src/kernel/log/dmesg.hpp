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
#include <kernel/memory/pmm_errors.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/task/scheduler_errors.hpp>
#include <kernel/task/task_errors.hpp>
#include <kernel/ipc/ipc_errors.hpp>
#include <kernel/ipc/buffer_pool_errors.hpp>
#include <kernel/arch/pci_errors.hpp>
#include <kernel/syscall/syscall_errors.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/nexios_config.h>
#include <lib/atomic.hpp>

// Forward declarations of the dmesg unit tests (global scope; friends of
// DmesgService so they can exercise the private ring buffer directly).
// Defined in src/kernel/test/test_dmesg.cpp.
void test_dmesg_initially_empty();
void test_dmesg_push_once();
void test_dmesg_push_and_pop();
void test_dmesg_push_multiple_fifo();
void test_dmesg_pop_empty();
void test_dmesg_clear();
void test_dmesg_for_each();
void test_dmesg_for_each_empty();
void test_dmesg_head_tail_indices();
void test_dmesg_overflow();
void test_dmesg_subsystem_names();
void test_dmesg_base_error_strings();
void test_dmesg_error_string_dispatch();
void test_dmesg_suppression_toggle();
void test_dmesg_timestamp_and_task_id();

namespace kernel::log {

/// @brief Severity of a dmesg entry (issue #234). Stored per entry so the
/// renderer never re-derives it from the code (misclassification would mask
/// faults as INFO — SIL 3 S1 guard: severity travels with the record).
enum class LogSeverity : uint8_t {
    DEBUG = 0, ///< Verbose/trace, debug targets only.
    INFO = 1,  ///< Action succeeded, confirmational.
    WARN = 2,  ///< Unusual, system stable, maybe user action needed.
    ERROR = 3, ///< Operation failed, system handled it via an action.
    FATAL = 4, ///< Unrecoverable, safe state + halt, restart required.
};

/// @brief Subsystem identifier used in dmesg entries (maps to per-subsystem
/// error_string).
enum class ErrorSubsystem : uint8_t {
    BASE = 0,    ///< Generic kernel errors.
    SYNC = 1,    ///< Synchronisation primitives.
    VFS = 2,     ///< Virtual file system (incl. vfsd daemon).
    MEMPOOL = 3, ///< Memory pool allocator.
    SCHED = 4,   ///< Scheduler.
    IPC = 5,     ///< Inter-process communication.
    SYSCALL = 6, ///< System call interface.
    NET = 7,     ///< Network stack / NIC drivers (issue #234).
    ELF = 8,     ///< ELF loader (issue #234).
    USER = 9,    ///< User task lifecycle reports (issue #234).
    DAEMON = 10, ///< Daemon lifecycle events (issue #234).
    PMM = 11,    ///< Physical memory manager (issue #234 full taxonomy).
    VMM = 12,    ///< Virtual memory manager (issue #234 full taxonomy).
    TASK = 13,   ///< Task creation/lifecycle errors (issue #234).
    BUFPOOL = 14, ///< Zero-copy buffer pool (issue #234 full taxonomy).
    DRIVER = 15, ///< Device drivers + PCI (issue #234 full taxonomy).
    INIT = 16,   ///< Boot/init/init-rc events (issue #234 full taxonomy).
    TIMING = 17, ///< Deadlines/budgets/admission/WCET (issue #234).
    TEST = 18,   ///< Selftest infrastructure: leaks, count drift (#234).
};

/// @brief Canonical error-nr stride: each subsystem owns [base, base+1000).
/// New subsystems number from these decimal bases; legacy BASE event
/// families (0xDAxx daemon, 0xDBxx ELF, 0xDCxx task-end) keep decoding
/// through base_error_string — see docs/specs/dmesg.md §5 for old→new map.
constexpr uint64_t kDmesgStride = 1000;
constexpr uint64_t kDmesgBase_BASE = 0;
constexpr uint64_t kDmesgBase_SYNC = 1000;
constexpr uint64_t kDmesgBase_VFS = 2000;
constexpr uint64_t kDmesgBase_MPOOL = 3000;
constexpr uint64_t kDmesgBase_SCHED = 4000;
constexpr uint64_t kDmesgBase_IPC = 5000;
constexpr uint64_t kDmesgBase_SYSCALL = 6000;
constexpr uint64_t kDmesgBase_NET = 7000;
constexpr uint64_t kDmesgBase_ELF = 8000;
constexpr uint64_t kDmesgBase_USER = 9000;
constexpr uint64_t kDmesgBase_DAEMON = 10000;
constexpr uint64_t kDmesgBase_PMM = 11000;
constexpr uint64_t kDmesgBase_VMM = 12000;
constexpr uint64_t kDmesgBase_TASK = 13000;
constexpr uint64_t kDmesgBase_BUFPOOL = 14000;
constexpr uint64_t kDmesgBase_DRIVER = 15000;
constexpr uint64_t kDmesgBase_INIT = 16000;
constexpr uint64_t kDmesgBase_TIMING = 17000;
constexpr uint64_t kDmesgBase_TEST = 18000;

/// @brief Panic record: pushed by panic() as BASE/FATAL (issue #234).
/// Outside every canonical window — decodes through base_error_string.
constexpr uint64_t kDmesgPanicCode = 0xFA57;

/// @brief Canonical base for a subsystem (kDmesgBase_* above).
inline uint64_t subsystem_base(ErrorSubsystem subsys) noexcept {
    switch (subsys) {
    case ErrorSubsystem::BASE:
        return kDmesgBase_BASE;
    case ErrorSubsystem::SYNC:
        return kDmesgBase_SYNC;
    case ErrorSubsystem::VFS:
        return kDmesgBase_VFS;
    case ErrorSubsystem::MEMPOOL:
        return kDmesgBase_MPOOL;
    case ErrorSubsystem::SCHED:
        return kDmesgBase_SCHED;
    case ErrorSubsystem::IPC:
        return kDmesgBase_IPC;
    case ErrorSubsystem::SYSCALL:
        return kDmesgBase_SYSCALL;
    case ErrorSubsystem::NET:
        return kDmesgBase_NET;
    case ErrorSubsystem::ELF:
        return kDmesgBase_ELF;
    case ErrorSubsystem::USER:
        return kDmesgBase_USER;
    case ErrorSubsystem::DAEMON:
        return kDmesgBase_DAEMON;
    case ErrorSubsystem::PMM:
        return kDmesgBase_PMM;
    case ErrorSubsystem::VMM:
        return kDmesgBase_VMM;
    case ErrorSubsystem::TASK:
        return kDmesgBase_TASK;
    case ErrorSubsystem::BUFPOOL:
        return kDmesgBase_BUFPOOL;
    case ErrorSubsystem::DRIVER:
        return kDmesgBase_DRIVER;
    case ErrorSubsystem::INIT:
        return kDmesgBase_INIT;
    case ErrorSubsystem::TIMING:
        return kDmesgBase_TIMING;
    case ErrorSubsystem::TEST:
        return kDmesgBase_TEST;
    default:
        return kDmesgBase_BASE;
    }
}

/// @brief True when error_nr lies in the subsystem's canonical window
/// [base, base + stride). Legacy BASE event codes (0xDAxx and friends)
/// are NOT canonical — they decode through the compat shims.
inline bool is_canonical_nbr(ErrorSubsystem subsys,
                             uint64_t error_nr) noexcept {
    const uint64_t base = subsystem_base(subsys);
    return error_nr >= base && error_nr < base + kDmesgStride &&
           subsys != ErrorSubsystem::BASE;
}

namespace catalog {
// Forward declarations — defined in dmesg_catalog.hpp (included at the
// end of this header). Lets error_string/default_severity_for resolve
// new-subsystem records without a circular include.
struct DmesgRecord;
const DmesgRecord *catalog_lookup(ErrorSubsystem subsys,
                                  uint64_t error_nr) noexcept;
const char *catalog_text(ErrorSubsystem subsys, uint64_t error_nr) noexcept;
LogSeverity lookup_severity(ErrorSubsystem subsys, uint64_t code) noexcept;
} // namespace catalog

/// @brief A single dmesg log entry.
struct LogEntry {
    static constexpr size_t kMessageCap = 96;
    uint64_t timestamp = 0;   ///< Tick count at log time (1 tick = 1 ms).
    uint64_t wall_ms = 0;     ///< Wall-clock ms since boot epoch, 0 = n/a
                              ///< (renderer falls back to tick count).
    uint64_t task_id = 0;     ///< ID of the task that logged the entry.
    ErrorSubsystem subsystem = ErrorSubsystem::BASE; ///< Origin subsystem.
    LogSeverity severity = LogSeverity::INFO; ///< Entry severity (#234).
    uint64_t error_code = 0;  ///< Subsystem-specific error code.
    uintptr_t context = 0;    ///< Optional context (e.g. address involved).
    char message[kMessageCap]; ///< Human-readable description (owned copy).
};

/// Capacity of the kernel dmesg ring buffer (from Kconfig).
constexpr size_t DMESG_CAPACITY = CONFIG_DMESG_CAPACITY;

/// @brief Stack buffer capacity for one rendered dmesg line (issue #234).
constexpr size_t DMESG_RENDER_CAP = 256;

/// @brief Wall-clock milliseconds since the Unix epoch for log entries.
/// Defined in dmesg.cpp (reads the boot epoch + tick count); returns 0
/// while no wall time is available (early boot) — the renderer then falls
/// back to the boot-tick count per the issue-#234 time rule.
uint64_t current_wall_ms() noexcept;

/// @brief Short name for a LogSeverity.
inline const char *severity_name(LogSeverity sev) {
    switch (sev) {
    case LogSeverity::DEBUG:
        return "DEBUG";
    case LogSeverity::INFO:
        return "INFO";
    case LogSeverity::WARN:
        return "WARN";
    case LogSeverity::ERROR:
        return "ERROR";
    case LogSeverity::FATAL:
        return "FATAL";
    default:
        return "UNK";
    }
}

/// @brief Default severity for legacy severity-less pushes: INFO for
/// confirmational codes (BASE info ranges, code 0), ERROR fail-closed
/// otherwise. New producers should pass an explicit severity (or the
/// catalog severity) instead of relying on this fallback.
inline LogSeverity default_severity_for(ErrorSubsystem subsys, uint64_t code);

/// @brief Sole owner of the kernel dmesg ring.
///
/// The ring buffer is a private nested implementation detail (not public API);
/// producers write through push(), the dmesg_task drains via pop(), and
/// readers iterate with for_each().  The dmesg test functions are declared
/// friends so the unit tests can exercise the buffer internals directly.
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
        return buffer_.push(subsys, err_code, default_severity_for(subsys,
                                                                  err_code),
                            msg, ctx);
    }

    /// @brief Push a structured entry with explicit severity (issue #234).
    /// @return true unless an entry was overwritten.
    bool push(ErrorSubsystem subsys, uint64_t err_code, LogSeverity sev,
              const char *msg, uintptr_t ctx = 0) noexcept {
        return buffer_.push(subsys, err_code, sev, msg, ctx);
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

    // ---- Test friends (dmesg unit tests exercise the buffer directly) ----
    friend void ::test_dmesg_initially_empty();
    friend void ::test_dmesg_push_once();
    friend void ::test_dmesg_push_and_pop();
    friend void ::test_dmesg_push_multiple_fifo();
    friend void ::test_dmesg_pop_empty();
    friend void ::test_dmesg_clear();
    friend void ::test_dmesg_for_each();
    friend void ::test_dmesg_for_each_empty();
    friend void ::test_dmesg_head_tail_indices();
    friend void ::test_dmesg_overflow();
    friend void ::test_dmesg_subsystem_names();
    friend void ::test_dmesg_base_error_strings();
    friend void ::test_dmesg_error_string_dispatch();
    friend void ::test_dmesg_suppression_toggle();
    friend void ::test_dmesg_timestamp_and_task_id();

    /// @brief Lock-free single-producer single-consumer ring buffer for
    /// structured kernel logs.  Private implementation detail of the service.
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
        bool push(ErrorSubsystem subsys, uint64_t err_code, LogSeverity sev,
                  const char *msg, uintptr_t ctx = 0) {
            if (s_suppressed_)
                return true;
            const uint64_t ts = arch::Timer::ticks();
            const uint64_t tid = kernel::Scheduler::current_task()
                                     ? kernel::Scheduler::current_task()->id
                                     : 0;

            size_t h = atomic_load(&head, __ATOMIC_RELAXED);
            size_t t = atomic_load(&tail, __ATOMIC_ACQUIRE);

            size_t next_h = (h + 1) & MASK;
            bool overwrote = false;

            if (next_h == t) {
                overwrote = true;
                atomic_store(&tail, (t + 1) & MASK, __ATOMIC_RELEASE);
            }

            buffer[h] = LogEntry{};
            buffer[h].timestamp = ts;
            buffer[h].wall_ms = current_wall_ms();
            buffer[h].task_id = tid;
            buffer[h].subsystem = subsys;
            buffer[h].severity = sev;
            buffer[h].error_code = err_code;
            buffer[h].context = ctx;
            // Owned copy (SIL3): the entry stores a bounded char array, so
            // producers may pass transient/ring buffers (e.g. the ELF loader's
            // message slots) without dangling once the source is reused.
            size_t i = 0;
            if (msg) {
                while (msg[i] && i < LogEntry::kMessageCap - 1) {
                    buffer[h].message[i] = msg[i];
                    ++i;
                }
            }
            buffer[h].message[i] = '\0';

            atomic_store(&head, next_h, __ATOMIC_RELEASE);
            return !overwrote;
        }

        /// @brief Pop the oldest entry.
        /// @return true if an entry was available.
        bool pop(LogEntry &entry) {
            size_t t = atomic_load(&tail, __ATOMIC_RELAXED);
            size_t h = atomic_load(&head, __ATOMIC_ACQUIRE);

            if (t == h)
                return false;

            entry = buffer[t];
            atomic_store(&tail, (t + 1) & MASK, __ATOMIC_RELEASE);
            return true;
        }

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
        bool empty() const {
            return atomic_load(&head, __ATOMIC_ACQUIRE) ==
                   atomic_load(&tail, __ATOMIC_ACQUIRE);
        }

        /// @brief Return the number of entries currently in the buffer.
        size_t size() const {
            size_t h = atomic_load(&head, __ATOMIC_ACQUIRE);
            size_t t = atomic_load(&tail, __ATOMIC_ACQUIRE);
            return (h - t) & MASK;
        }

        /// @brief Discard all entries (reset tail to head).
        void clear() {
            size_t h = atomic_load(&head, __ATOMIC_RELAXED);
            atomic_store(&tail, h, __ATOMIC_RELEASE);
        }

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

/// @brief True when a base-subsystem code is an informational event rather
///        than a fault: daemon lifecycle (0xDA00-0xDAFF), background ELF
///        loader (0xDB00-0xDBFF), and user task-end reports (0xDC00-0xDCFF)
///        ranges, plus the canonical INFO records of issue #234 (daemon
///        restarted/ensured/terminated/restarting/up, ELF started/completed/
///        canceled, user task exited). Used by legacy renderers and the
///        severity fallback to print INFO instead of ERROR. A new INFO
///        family MUST extend both this predicate and base_error_string
///        below, or its lines render as ERR=<stale enum>.
inline bool base_code_is_info(uint64_t code) {
    if ((code & ~0xFFULL) == 0xDA00 || (code & ~0xFFULL) == 0xDB00 ||
        (code & ~0xFFULL) == 0xDC00) {
        return true;
    }
    switch (code) {
    case kDmesgBase_DAEMON + 2:
    case kDmesgBase_DAEMON + 3:
    case kDmesgBase_DAEMON + 4:
    case kDmesgBase_DAEMON + 5:
    case kDmesgBase_DAEMON + 6:
    case kDmesgBase_ELF + 1:
    case kDmesgBase_ELF + 2:
    case kDmesgBase_ELF + 3:
    case kDmesgBase_USER + 1:
        return true;
    default:
        return false;
    }
}

/// @brief Default severity for legacy severity-less pushes (issue #234).
inline LogSeverity default_severity_for(ErrorSubsystem subsys, uint64_t code) {
    return catalog::lookup_severity(subsys, code);
}

/// @brief Return a human-readable string for a base-subsystem error code.
///        The code space is split into kernel::Error values (0–9) and
///        a custom range for event codes:
///          0xDA00 – 0xDAFF  daemon lifecycle events
///          0xDB00 – 0xDBFF  background ELF loader events
///          0xDC00 – 0xDCFF  user task-end reports (clean exit / fault)
inline const char *base_error_string(uint64_t code) {
    // Panic record (pushed by panic() as BASE/FATAL, issue #234).
    if (code == kDmesgPanicCode) {
        return "Kernel panic";
    }
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
        case 0xDB0C:
            return "ELF authenticity mismatch: task terminated";
        case 0xDB0D:
            return "ELF load failed: authenticity baseline";
        default:
            break;
        }
        return "ELF loader event";
    }
    if ((code & ~0xFFULL) == 0xDC00) {
        switch (code) {
        case 0xDC01:
            return "Task exited";
        case 0xDC02:
            return "Task faulted";
        default:
            break;
        }
        return "Task end event";
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
    case ErrorSubsystem::NET:
        return "NET";
    case ErrorSubsystem::ELF:
        return "ELF";
    case ErrorSubsystem::USER:
        return "USERSPACE";
    case ErrorSubsystem::DAEMON:
        return "DAEMON";
    case ErrorSubsystem::PMM:
        return "PMM";
    case ErrorSubsystem::VMM:
        return "VMM";
    case ErrorSubsystem::TASK:
        return "TASK";
    case ErrorSubsystem::BUFPOOL:
        return "BUFPOOL";
    case ErrorSubsystem::DRIVER:
        return "DRIVER";
    case ErrorSubsystem::INIT:
        return "INIT";
    case ErrorSubsystem::TIMING:
        return "TIMING";
    case ErrorSubsystem::TEST:
        return "TEST";
    default:
        return "UNK";
    }
}

/// @brief Strip a canonical number to its raw table code. Raw (legacy)
/// codes pass through unchanged, so every pre-#234 push decodes exactly
/// as before; canonical pushes (base + raw) resolve to the same text.
inline uint64_t strip_canonical(ErrorSubsystem subsys, uint64_t code) {
    if (subsys == ErrorSubsystem::BASE) {
        return code;
    }
    const uint64_t base = subsystem_base(subsys);
    if (code >= base && code < base + kDmesgStride) {
        return code - base;
    }
    return code;
}

/// @brief Dispatch an error code to the correct subsystem's error_string.
/// Catalog event records take precedence over the raw tables, so event
/// numbers (e.g. TIMING deadline-missed) render their cause text while
/// plain table codes render the table text — both fail closed.
inline const char *error_string(ErrorSubsystem subsys, uint64_t code) {
    if (catalog::catalog_lookup(subsys, code) != nullptr) {
        return catalog::catalog_text(subsys, code);
    }
    const uint64_t raw = strip_canonical(subsys, code);
    switch (subsys) {
    case ErrorSubsystem::BASE:
        return base_error_string(raw);
    case ErrorSubsystem::SYNC:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SyncError>(raw));
    case ErrorSubsystem::VFS:
        return kernel::errors::error_string(
            static_cast<kernel::errors::VfsError>(raw));
    case ErrorSubsystem::MEMPOOL:
        return kernel::errors::error_string(
            static_cast<kernel::errors::MemPoolError>(raw));
    case ErrorSubsystem::SCHED:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SchedulerError>(raw));
    case ErrorSubsystem::IPC:
        return kernel::errors::error_string(
            static_cast<kernel::errors::IpcError>(raw));
    case ErrorSubsystem::SYSCALL:
        return kernel::errors::error_string(
            static_cast<kernel::errors::SyscallError>(raw));
    case ErrorSubsystem::PMM:
        return kernel::errors::error_string(
            static_cast<kernel::errors::PmmError>(raw));
    case ErrorSubsystem::VMM:
        return kernel::errors::error_string(
            static_cast<kernel::errors::VmmError>(raw));
    case ErrorSubsystem::TASK:
        return kernel::errors::error_string(
            static_cast<kernel::errors::TaskError>(raw));
    case ErrorSubsystem::BUFPOOL:
        return kernel::errors::error_string(
            static_cast<kernel::errors::BufPoolError>(raw));
    case ErrorSubsystem::DRIVER:
        return kernel::errors::error_string(
            static_cast<kernel::errors::PciError>(raw));
    case ErrorSubsystem::NET:
    case ErrorSubsystem::ELF:
    case ErrorSubsystem::USER:
    case ErrorSubsystem::DAEMON:
    case ErrorSubsystem::INIT:
    case ErrorSubsystem::TIMING:
    case ErrorSubsystem::TEST:
        return catalog::catalog_text(subsys, code);
    default:
        return "UNKNOWN";
    }
}

/// @brief Render one entry in the canonical issue-#234 line format:
/// `[DMESG <time>ms]: <TYPE> <CODEBASE> <error-nr> <err-text>: <msg>`
/// `  [task=<id> ctx=0x<hex>]`
/// <time> is wall_ms when set, else the boot-tick timestamp. The output
/// is always NUL-terminated and truncated to fit; bounded loops only.
/// @return Bytes written excluding the NUL (0 when out_cap == 0).
size_t format_dmesg_entry(char *out_buf, size_t out_cap,
                          const LogEntry &entry) noexcept;

/// @brief Shorthand: push an entry with explicit severity (issue #234).
inline bool dmesg_push_sev(ErrorSubsystem subsys, uint64_t err,
                           LogSeverity sev, const char *msg,
                           uintptr_t ctx = 0) noexcept {
    return DmesgService::instance().push(subsys, err, sev, msg, ctx);
}

} // namespace kernel::log

// Central record catalog (struct + per-subsystem arrays, issue req. 3).
// Included last so the catalog sees the complete dmesg types while the
// forward declarations above satisfy error_string/default_severity_for.
#include <kernel/log/dmesg_catalog.hpp>