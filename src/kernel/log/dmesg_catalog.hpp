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

/// @file dmesg_catalog.hpp
/// @brief Central dmesg record catalog (issue #234): per-subsystem tables
/// pairing canonical error-nr, severity, and text. Producers push the
/// canonical number; renderers resolve severity/text through catalog_lookup.
/// Legacy 0xDAxx/0xDBxx/0xDCxx BASE codes keep decoding through
/// base_error_string (field-log compat) — see docs/specs/dmesg.md §5.

#pragma once
#include <kernel/log/dmesg.hpp>

namespace kernel::log::catalog {

/// @brief One catalogued dmesg record: canonical error-nr (subsystem base
/// + offset), fixed severity, and literal text with cause.
struct DmesgRecord {
    ErrorSubsystem subsystem;
    uint64_t error_nr;
    LogSeverity severity;
    const char *text;
};

/// @brief Daemon lifecycle records (canonical base kDmesgBase_DAEMON).
constexpr DmesgRecord kDmesgCatalog_DAEMON[] = {
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 1, LogSeverity::ERROR,
     "Daemon exited"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 2, LogSeverity::INFO,
     "Daemon restarted"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 3, LogSeverity::INFO,
     "Daemon ensured"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 4, LogSeverity::INFO,
     "Daemon terminated"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 5, LogSeverity::INFO,
     "Daemon restarting"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 6, LogSeverity::INFO,
     "Daemon up"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 7, LogSeverity::ERROR,
     "Daemon restart limit reached, giving up"},
    {ErrorSubsystem::DAEMON, kDmesgBase_DAEMON + 8, LogSeverity::ERROR,
     "Daemon restart failed"},
};

/// @brief ELF loader records (canonical base kDmesgBase_ELF).
constexpr DmesgRecord kDmesgCatalog_ELF[] = {
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 1, LogSeverity::INFO,
     "ELF load started"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 2, LogSeverity::INFO,
     "ELF load completed"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 3, LogSeverity::INFO,
     "ELF load canceled"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 4, LogSeverity::ERROR,
     "ELF load failed: invalid elf-file"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 5, LogSeverity::ERROR,
     "ELF load failed: not enough memory"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 6, LogSeverity::ERROR,
     "ELF load failed: file not found"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 7, LogSeverity::ERROR,
     "ELF load failed: read error"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 8, LogSeverity::WARN,
     "ELF load rejected: already loading"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 9, LogSeverity::WARN,
     "ELF load rejected: not loading"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 10, LogSeverity::ERROR,
     "ELF load failed: missing shared lib"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 11, LogSeverity::ERROR,
     "ELF load failed: layout conflict"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 12, LogSeverity::ERROR,
     "ELF load failed: dep cycle/depth"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 13, LogSeverity::ERROR,
     "ELF load failed: invalid shared lib"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 14, LogSeverity::FATAL,
     "ELF authenticity mismatch: task terminated"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 15, LogSeverity::ERROR,
     "ELF load failed: authenticity baseline"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 16, LogSeverity::ERROR,
     "ELF load rejected: segment past heap base"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 17, LogSeverity::ERROR,
     "ELF load rejected: no red-zone page"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 18, LogSeverity::ERROR,
     "ELF load rejected: heap overrun"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 19, LogSeverity::ERROR,
     "ELF shared view: null argument"},
    {ErrorSubsystem::ELF, kDmesgBase_ELF + 20, LogSeverity::ERROR,
     "ELF shared view: image not in closure"},
};

/// @brief User task-end records (canonical base kDmesgBase_USER).
constexpr DmesgRecord kDmesgCatalog_USER[] = {
    {ErrorSubsystem::USER, kDmesgBase_USER + 1, LogSeverity::INFO,
     "Task exited"},
    {ErrorSubsystem::USER, kDmesgBase_USER + 2, LogSeverity::ERROR,
     "Task faulted"},
    {ErrorSubsystem::USER, kDmesgBase_USER + 3, LogSeverity::ERROR,
     "Task terminated by signal"},
    {ErrorSubsystem::USER, kDmesgBase_USER + 4, LogSeverity::INFO,
     "Task started"},
};

/// @brief Network records (canonical base kDmesgBase_NET). No producer
/// pushes these yet — the space is reserved so drivers report into a
/// stable numbering from day one.
constexpr DmesgRecord kDmesgCatalog_NET[] = {
    {ErrorSubsystem::NET, kDmesgBase_NET + 1, LogSeverity::INFO,
     "Network link up"},
    {ErrorSubsystem::NET, kDmesgBase_NET + 2, LogSeverity::WARN,
     "Network link down"},
    {ErrorSubsystem::NET, kDmesgBase_NET + 3, LogSeverity::ERROR,
     "Network transmit timeout"},
    {ErrorSubsystem::NET, kDmesgBase_NET + 4, LogSeverity::WARN,
     "Network ARP resolution failed"},
    {ErrorSubsystem::NET, kDmesgBase_NET + 5, LogSeverity::ERROR,
     "Network packet too large"},
};

/// @brief Driver records (canonical base kDmesgBase_DRIVER): probe,
/// transport, queue, request and DMA failures across ahci/virtio/pci.
constexpr DmesgRecord kDmesgCatalog_DRIVER[] = {
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 1, LogSeverity::ERROR,
     "Driver probe failed: no device"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 2, LogSeverity::ERROR,
     "Driver transport init failed"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 3, LogSeverity::ERROR,
     "Driver queue setup failed"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 4, LogSeverity::ERROR,
     "Driver request timeout"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 5, LogSeverity::ERROR,
     "Driver request failed"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 6, LogSeverity::ERROR,
     "Driver out of memory"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 7, LogSeverity::ERROR,
     "Driver feature negotiation rejected"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 8, LogSeverity::ERROR,
     "Driver DMA buffer failed"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 9, LogSeverity::WARN,
     "Driver using fallback configuration"},
    {ErrorSubsystem::DRIVER, kDmesgBase_DRIVER + 10, LogSeverity::WARN,
     "Driver scan truncated: table full"},
};

/// @brief Init records (canonical base kDmesgBase_INIT): init-rc, fstab,
/// daemon-startup and shell-creation milestones and failures.
constexpr DmesgRecord kDmesgCatalog_INIT[] = {
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 1, LogSeverity::WARN,
     "Init: rc line has arguments, skipping"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 2, LogSeverity::WARN,
     "Init: rc file not found"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 3, LogSeverity::ERROR,
     "Init: rc invalid ELF"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 4, LogSeverity::ERROR,
     "Init: rc rejected (not ET_EXEC)"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 5, LogSeverity::WARN,
     "Init: rc path too long"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 6, LogSeverity::ERROR,
     "Init: rc load not accepted"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 7, LogSeverity::ERROR,
     "Init: rc load failed"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 8, LogSeverity::ERROR,
     "Init: rc policy rejected"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 9, LogSeverity::ERROR,
     "Init: rc admission denied"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 10, LogSeverity::ERROR,
     "Init: daemon wait timeout"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 11, LogSeverity::ERROR,
     "Init: daemon missing"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 12, LogSeverity::ERROR,
     "Init: shell task creation failed"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 13, LogSeverity::ERROR,
     "Init: task definition invalid ELF"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 14, LogSeverity::ERROR,
     "Init: task creation failed"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 15, LogSeverity::ERROR,
     "Init: fstab mount failed"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 16, LogSeverity::INFO,
     "Init: daemons ready"},
    {ErrorSubsystem::INIT, kDmesgBase_INIT + 17, LogSeverity::INFO,
     "Init: shell started"},
};

/// @brief Timing records (canonical base kDmesgBase_TIMING): deadline,
/// budget, admission, bound and overrun events.
constexpr DmesgRecord kDmesgCatalog_TIMING[] = {
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 1, LogSeverity::ERROR,
     "Timing: deadline missed"},
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 2, LogSeverity::ERROR,
     "Timing: budget exhausted"},
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 3, LogSeverity::WARN,
     "Timing: admission denied"},
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 4, LogSeverity::WARN,
     "Timing: Liu-Leyland bound exceeded"},
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 5, LogSeverity::WARN,
     "Timing: WCET overrun"},
    {ErrorSubsystem::TIMING, kDmesgBase_TIMING + 6, LogSeverity::ERROR,
     "Timing: watchdog expired"},
};

/// @brief Selftest records (canonical base kDmesgBase_TEST): leak and
/// count-drift detectors of the test infrastructure itself.
constexpr DmesgRecord kDmesgCatalog_TEST[] = {
    {ErrorSubsystem::TEST, kDmesgBase_TEST + 1, LogSeverity::ERROR,
     "Selftest: resource leak detected"},
    {ErrorSubsystem::TEST, kDmesgBase_TEST + 2, LogSeverity::WARN,
     "Selftest: test count drift"},
};

/// @brief Resolve a canonical (subsystem, error-nr) pair to its record.
/// @return Pointer to the matching record, or nullptr when the pair is not
///         catalogued (caller falls back to ERROR/unknown-text).
inline const DmesgRecord *catalog_lookup(ErrorSubsystem subsys,
                                         uint64_t error_nr) noexcept {    const DmesgRecord *table = nullptr;
    size_t count = 0;
    switch (subsys) {
    case ErrorSubsystem::DAEMON:
        table = kDmesgCatalog_DAEMON;
        count = sizeof(kDmesgCatalog_DAEMON) / sizeof(kDmesgCatalog_DAEMON[0]);
        break;
    case ErrorSubsystem::ELF:
        table = kDmesgCatalog_ELF;
        count = sizeof(kDmesgCatalog_ELF) / sizeof(kDmesgCatalog_ELF[0]);
        break;
    case ErrorSubsystem::USER:
        table = kDmesgCatalog_USER;
        count = sizeof(kDmesgCatalog_USER) / sizeof(kDmesgCatalog_USER[0]);
        break;
    case ErrorSubsystem::NET:
        table = kDmesgCatalog_NET;
        count = sizeof(kDmesgCatalog_NET) / sizeof(kDmesgCatalog_NET[0]);
        break;
    case ErrorSubsystem::DRIVER:
        table = kDmesgCatalog_DRIVER;
        count =
            sizeof(kDmesgCatalog_DRIVER) / sizeof(kDmesgCatalog_DRIVER[0]);
        break;
    case ErrorSubsystem::INIT:
        table = kDmesgCatalog_INIT;
        count = sizeof(kDmesgCatalog_INIT) / sizeof(kDmesgCatalog_INIT[0]);
        break;
    case ErrorSubsystem::TIMING:
        table = kDmesgCatalog_TIMING;
        count =
            sizeof(kDmesgCatalog_TIMING) / sizeof(kDmesgCatalog_TIMING[0]);
        break;
    case ErrorSubsystem::TEST:
        table = kDmesgCatalog_TEST;
        count = sizeof(kDmesgCatalog_TEST) / sizeof(kDmesgCatalog_TEST[0]);
        break;
    default:
        return nullptr;
    }
    for (size_t idx = 0; idx < count; ++idx) {
        if (table[idx].error_nr == error_nr) {
            return &table[idx];
        }
    }
    return nullptr;
}

/// @brief Text for a (subsystem, error-nr) pair: catalog text when
/// catalogued, "UNKNOWN" fail-closed otherwise. Backs error_string()
/// for the event-driven subsystems (NET/ELF/USER/DAEMON/DRIVER/INIT/
/// TIMING/TEST).
inline const char *catalog_text(ErrorSubsystem subsys,
                                uint64_t error_nr) noexcept {
    const DmesgRecord *found = catalog_lookup(subsys, error_nr);
    return found ? found->text : "UNKNOWN";
}

/// @brief Severity for a pushed (subsystem, code) pair — the complete
/// issue-#234 taxonomy. Catalog event records first; code 0 (OK) is
/// confirmational INFO; BASE legacy event ranges keep their historic INFO
/// classification through base_code_is_info; explicitly listed routine /
/// degraded codes are WARN; everything else fails closed to ERROR. Only
/// explicit FATAL records (auth kill, panic) ever render FATAL.
inline LogSeverity lookup_severity(ErrorSubsystem subsys,
                                  uint64_t code) noexcept {
    const DmesgRecord *found = catalog_lookup(subsys, code);
    if (found) {
        return found->severity;
    }
    if (subsys == ErrorSubsystem::BASE) {
        return base_code_is_info(code) ? LogSeverity::INFO : LogSeverity::ERROR;
    }
    const uint64_t raw = strip_canonical(subsys, code);
    if (raw == 0) {
        return LogSeverity::INFO;
    }
    switch (subsys) {
    case ErrorSubsystem::SYNC:
        switch (raw) {
        case 3:
        case 7:
        case 9:
        case 12:
        case 13:
        case 14:
            return LogSeverity::WARN;
        default:
            return LogSeverity::ERROR;
        }
    case ErrorSubsystem::VFS:
        switch (raw) {
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            return LogSeverity::WARN;
        default:
            return LogSeverity::ERROR;
        }
    case ErrorSubsystem::SCHED:
        return raw == 12 ? LogSeverity::WARN : LogSeverity::ERROR;
    case ErrorSubsystem::IPC:
        return raw == 3 ? LogSeverity::WARN : LogSeverity::ERROR;
    case ErrorSubsystem::SYSCALL:
        switch (raw) {
        case 403:
        case 407:
        case 409:
        case 412:
        case 413:
        case 414:
        case 503:
        case 504:
        case 505:
        case 506:
        case 507:
        case 603:
            return LogSeverity::WARN;
        default:
            return LogSeverity::ERROR;
        }
    default:
        return LogSeverity::ERROR;
    }
}

/// @brief Compile-time catalog validator: every record lies in the event
/// number space and no number repeats within a table.
template <size_t Count>
constexpr bool catalog_table_valid(const DmesgRecord (&table)[Count]) {
    for (size_t idx = 0; idx < Count; ++idx) {
        if (table[idx].error_nr < 7000 || table[idx].error_nr >= 19000) {
            return false;
        }
        for (size_t inner = idx + 1; inner < Count; ++inner) {
            if (table[idx].subsystem == table[inner].subsystem &&
                table[idx].error_nr == table[inner].error_nr) {
                return false;
            }
        }
    }
    return true;
}

static_assert(catalog_table_valid(kDmesgCatalog_DAEMON), "DAEMON catalog");
static_assert(catalog_table_valid(kDmesgCatalog_ELF), "ELF catalog");
static_assert(catalog_table_valid(kDmesgCatalog_USER), "USER catalog");
static_assert(catalog_table_valid(kDmesgCatalog_NET), "NET catalog");
static_assert(catalog_table_valid(kDmesgCatalog_DRIVER), "DRIVER catalog");
static_assert(catalog_table_valid(kDmesgCatalog_INIT), "INIT catalog");
static_assert(catalog_table_valid(kDmesgCatalog_TIMING), "TIMING catalog");
static_assert(catalog_table_valid(kDmesgCatalog_TEST), "TEST catalog");

} // namespace kernel::log::catalog
