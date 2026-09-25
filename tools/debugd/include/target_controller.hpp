/*
 * NexIOS RTOS — debugd Phase 3 (issue #224)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * Task controller abstraction (spec docs/specs/debugd.md §6): pure
 * interface between RSP verbs and the §3 debug syscalls, breakpoint
 * shadow table, total error mapping. Header-only, zero-heap,
 * freestanding-clean (same allowed headers as gdb_rsp.hpp).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gdb_rsp.hpp"

namespace debugd {

/// @brief Kernel-style errno subset used across the controller boundary.
///        Values mirror POSIX numerics for host-test familiarity.
enum class DbgErr : int {
    kOk = 0,
    kGeneric = 1,   // E01
    kBadHandle = 9, // EBADF
    kNoTarget = 3,  // ESRCH
    kDenied = 1,    // EPERM (shares E01 wire code by design)
    kFault = 14,    // EFAULT (partial copies report bytes done)
    kBusy = 16,     // EBUSY (second attach)
};

/// @brief Total error mapping (spec §6): every errno has a defined RSP
///        encoding; empty reply means unsupported — never a dropped packet.
///        EFAULT is E0E (errno 14), NOT empty: a real fault must read as an
///        error, or the host mistakes it for "unsupported" and disables
///        memory access. (Partial copies bypass this via the int-count
///        path and report bytes done.)
inline std::string_view EncodeErrno(DbgErr e) noexcept {
    switch (e) {
    case DbgErr::kOk:
        return "OK";
    case DbgErr::kFault:
        return "E0E";
    default:
        return "E01";
    }
}

/// @brief Pure task-control interface (spec §6). Implementations bridge
///        to the §3 syscalls (production) or scripted fakes (tests).
class TargetTaskController {
  public:
    virtual ~TargetTaskController() = default;
    virtual DbgErr read_regs(Arch arch, std::span<std::uint8_t> out) = 0;
    virtual DbgErr write_regs(Arch arch, std::span<const std::uint8_t> in) = 0;
    /// @brief Returns bytes transferred (>= 0) or negative errno.
    virtual int read_mem(std::uint64_t va, std::span<std::uint8_t> out) = 0;
    virtual int write_mem(std::uint64_t va,
                          std::span<const std::uint8_t> in) = 0;
    virtual DbgErr break_at(std::uint64_t va) = 0;
    virtual DbgErr clear_break(std::uint64_t va) = 0;
    virtual DbgErr step() = 0;
    virtual DbgErr cont() = 0;
    /// @brief Last stop reason, GDB stop-reply format (e.g. "T05", "W00").
    virtual std::string_view stop_reason() = 0;

  protected:
    TargetTaskController() = default;
};

/// @brief Fixed-capacity breakpoint shadow table (spec §6). The kernel
///        shadow is authoritative at runtime (spec §4); this table is the
///        controller-side write-through cache. No heap, ever.
class BreakpointTable {
  public:
    static constexpr std::size_t kCapacity = 64;

    struct Entry {
        std::uint64_t va = 0;
        std::uint64_t orig_insn = 0; // shadowed instruction word
        bool used = false;
    };

    bool insert(std::uint64_t va, std::uint64_t orig) noexcept {
        if (find(va) != nullptr)
            return true; // idempotent re-insert
        for (auto &e : entries_) {
            if (!e.used) {
                e.va = va;
                e.orig_insn = orig;
                e.used = true;
                return true;
            }
        }
        return false; // table full: caller reports E01, never grows
    }

    bool remove(std::uint64_t va) noexcept {
        Entry *e = find(va);
        if (e == nullptr)
            return false;
        e->used = false;
        return true;
    }

    const Entry *find(std::uint64_t va) const noexcept {
        for (const auto &e : entries_) {
            if (e.used && e.va == va)
                return &e;
        }
        return nullptr;
    }

    Entry *find(std::uint64_t va) noexcept {
        for (auto &e : entries_) {
            if (e.used && e.va == va)
                return &e;
        }
        return nullptr;
    }

    std::size_t used() const noexcept {
        std::size_t n = 0;
        for (const auto &e : entries_)
            n += e.used ? 1 : 0;
        return n;
    }

  private:
    std::array<Entry, kCapacity> entries_{};
};

/// @brief Breakpoint instruction words per arch (spec §4, single source
///        of truth for the controller side).
constexpr std::uint32_t BreakInsn(Arch arch) noexcept {
    switch (arch) {
    case Arch::kX86_64:
        return 0xCC; // int3 (one byte, low 8 bits used)
    case Arch::kAArch64:
        return 0xD4200000; // brk #0
    case Arch::kRiscv64:
        return 0x00100073; // ebreak
    }
    return 0;
}

} // namespace debugd
