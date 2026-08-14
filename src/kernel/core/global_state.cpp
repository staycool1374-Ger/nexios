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

/// @file global_state.cpp
/// @brief Single definition point for all cross-TU kernel globals.
///
/// Every symbol defined here has exactly one owner file (this one).  Symbols
/// are defined at the SAME scope as their historical extern declarations so
/// existing consumers keep linking; the gs:: accessors are the sanctioned way
/// to read/write them.

#include <kernel/core/global_state.hpp>
#include <kernel/arch/timer.hpp>

// ---------------------------------------------------------------------------
// Storage — defined at the scope matching the extern declarations.
// ---------------------------------------------------------------------------

// Multiboot2 globals are declared `extern "C"` at GLOBAL scope in
// multiboot2.hpp (mb2_find_tag and the boot path reference them).
extern "C" {
uint64_t multiboot_magic = 0;
uint64_t multiboot_info_ptr = 0;
}

namespace kernel {

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
BootInfo g_boot_info{};

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t g_boot_epoch = 0; // RTC read_seconds() at boot

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
CanaryTrip g_canary_trip{};

// The SMAP recovery IP keeps the extern "C" symbol the fault handler uses.
extern "C" {
uint64_t g_user_access_recover_ip = 0;
}

namespace test {
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
const char *g_current_class = nullptr;
bool g_filter_bench = false;
bool g_class_auto_shutdown = false;
bool g_vfs_touched = false;
uint64_t g_kernel_entry_ns = 0;
} // namespace test

} // namespace kernel

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

namespace kernel {
namespace gs {

// -- BootState ---------------------------------------------------------------

BootInfo &boot_info() noexcept {
    return g_boot_info;
}

uint64_t get_boot_epoch() noexcept {
    return g_boot_epoch;
}

bool try_set_boot_epoch(uint64_t epoch, const WriteContext &ctx) noexcept {
    return verify_and_write(g_boot_epoch, epoch, WriteClass::BOOT_ONLY, ctx,
                            "boot_epoch");
}

uint64_t get_multiboot_magic() noexcept {
    return multiboot_magic;
}

uint64_t get_multiboot_info_ptr() noexcept {
    return multiboot_info_ptr;
}

bool try_set_multiboot(uint64_t magic, uint64_t info_ptr,
                       const WriteContext &ctx) noexcept {
    if (ctx.phase != StatePhase::BOOT)
        return false;
    multiboot_magic = magic;
    multiboot_info_ptr = info_ptr;
    return true;
}

// -- FaultState --------------------------------------------------------------

CanaryTrip &canary_trip() noexcept {
    return g_canary_trip;
}

void set_canary_trip(uint64_t task_id, uint8_t segment, uint64_t rip) noexcept {
    g_canary_trip.task_id = task_id;
    g_canary_trip.segment = segment;
    g_canary_trip.rip = rip;
    ++g_canary_trip.count;
}

void reset_canary_trip() noexcept {
    g_canary_trip = CanaryTrip{};
}

uint64_t &user_access_recover_ip() noexcept {
    return g_user_access_recover_ip;
}

// -- TestState ---------------------------------------------------------------

const char *get_current_class() noexcept {
    return test::g_current_class;
}

void set_current_class(const char *name) noexcept {
    test::g_current_class = name;
}

bool get_filter_bench() noexcept {
    return test::g_filter_bench;
}

void set_filter_bench(bool v) noexcept {
    test::g_filter_bench = v;
}

bool get_class_auto_shutdown() noexcept {
    return test::g_class_auto_shutdown;
}

void set_class_auto_shutdown(bool v) noexcept {
    test::g_class_auto_shutdown = v;
}

bool get_vfs_touched() noexcept {
    return test::g_vfs_touched;
}

void mark_vfs_touched(bool v) noexcept {
    test::g_vfs_touched = v;
}

uint64_t get_kernel_entry_ns() noexcept {
    return test::g_kernel_entry_ns;
}

void set_kernel_entry_ns() noexcept {
    test::g_kernel_entry_ns = arch::Timer::ns();
}

// ---------------------------------------------------------------------------
// VfsState definitions
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
kernel::fat32::Fat32Partition *g_fat32_partition = nullptr;

kernel::fat32::Fat32Partition *get_fat32_partition() noexcept {
    return g_fat32_partition;
}

bool try_set_fat32_partition(kernel::fat32::Fat32Partition *p) noexcept {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    // Legal range: null, or any kernel-half address (>= HHDM base).  User
    // pointers (low canonical) and garbage are rejected.
    if (addr != 0 && addr < CONFIG_HHDM_OFFSET)
        return false;
    g_fat32_partition = p;
    return true;
}

// ---------------------------------------------------------------------------
// NetState definitions
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
::net::Nic *g_nic = nullptr;

::net::Nic *get_nic() noexcept {
    return g_nic;
}

bool try_set_nic(::net::Nic *nic) noexcept {
    const uint64_t addr = reinterpret_cast<uint64_t>(nic);
    if (addr != 0 && addr < CONFIG_HHDM_OFFSET)
        return false;
    g_nic = nic;
    return true;
}

// -- Audit ring (CONFIG_DEBUG only) ------------------------------------------

#ifdef CONFIG_DEBUG
namespace {
constexpr size_t kAuditDepth = 64;
struct AuditEntry {
    const char *name;
    bool accepted;
    uint64_t tick;
};
AuditEntry g_audit[kAuditDepth];
uint64_t g_audit_idx = 0;
} // namespace

void audit_write(const char *name, bool accepted) noexcept {
    uint64_t i = g_audit_idx++ % kAuditDepth;
    g_audit[i].name = name;
    g_audit[i].accepted = accepted;
    g_audit[i].tick = arch::Timer::ticks();
}
#else
void audit_write(const char *name, bool accepted) noexcept {
    (void)name;
    (void)accepted;
}
#endif

} // namespace gs
} // namespace kernel
