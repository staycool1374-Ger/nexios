#pragma once

/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file object_sizing.hpp
/// @brief Fixed-size table for retype-carvable objects (F1, issue #554).
///        Compile-time only, no code: every carvable kind declares its
///        fixed byte size here so retype can reject short ranges with a
///        typed error instead of building a partial object (spec §4.2).

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/cap/cap_types.hpp>

namespace kernel::cap {

/// @brief Page-backed TCB storage (F4 #557 consumer wires the schedulable
///        object; covers the worst-case sizeof(TaskControlBlock), 8256 B
///        on aarch64 — uniform page count on all three arches).
constexpr size_t kObjectBytesTcb = 4 * CONFIG_PAGE_SIZE;
/// @brief Single-page address-space root (F5 #558 consumer).
constexpr size_t kObjectBytesVSpace = CONFIG_PAGE_SIZE;
/// @brief Single-page page-table page (F5 #558 consumer).
constexpr size_t kObjectBytesPageTable = CONFIG_PAGE_SIZE;
/// @brief Single-page sync notification object (§8.1/§5 consumer).
constexpr size_t kObjectBytesNotification = CONFIG_PAGE_SIZE;
/// @brief Single-page scheduling context (M13 #190 consumer).
constexpr size_t kObjectBytesSchedContext = CONFIG_PAGE_SIZE;
/// @brief Largest single carved object (bounds the zeroing work, INV-RT-4).
constexpr size_t kObjectBytesMax = kObjectBytesTcb;

/// @brief Compile-time size descriptor for one carvable kind.
struct ObjectSizing {
    size_t bytes = 0;      ///< fixed object size (0 when variable_size)
    size_t align = 0;      ///< required alignment (PAGE_SIZE when carvable)
    bool carvable = false; ///< true for the six retype target kinds
    bool variable_size = false; ///< true only for Frame (explicit size)
};

/// @brief Fixed size descriptor for @p kind (F1, issue #554).
///        Frame is the only variable-size kind; the five fixed kinds
///        report their compile-time size.  Every other CapType (Endpoint,
///        CNode, Mmio, Irq, IoMmuDma, Msix, Wdog, Untyped, Task, Null)
///        is not carvable.
/// @note The single-page VSpace/PageTable/Notification/SchedContext
///       sizes are declarations only: each consumer (F4/F5/M13) must add
///       its own static_assert(sizeof(Struct) <= kObjectBytesX) when its
///       struct lands.  Only TCB is pinned here (carved_object.cpp).
constexpr ObjectSizing object_sizing(CapType kind) noexcept {
    switch (kind) {
    case CapType::Frame:
        return ObjectSizing{0, CONFIG_PAGE_SIZE, true, true};
    case CapType::TCB:
        return ObjectSizing{kObjectBytesTcb, CONFIG_PAGE_SIZE, true,
                            false};
    case CapType::VSpace:
        return ObjectSizing{kObjectBytesVSpace, CONFIG_PAGE_SIZE, true,
                            false};
    case CapType::PageTable:
        return ObjectSizing{kObjectBytesPageTable, CONFIG_PAGE_SIZE,
                            true, false};
    case CapType::Notification:
        return ObjectSizing{kObjectBytesNotification, CONFIG_PAGE_SIZE,
                            true, false};
    case CapType::SchedContext:
        return ObjectSizing{kObjectBytesSchedContext, CONFIG_PAGE_SIZE,
                            true, false};
    default:
        return ObjectSizing{0, 0, false, false};
    }
}

static_assert(kObjectBytesTcb % CONFIG_PAGE_SIZE == 0,
              "TCB object size must be PAGE-aligned");
static_assert(kObjectBytesVSpace % CONFIG_PAGE_SIZE == 0,
              "VSpace object size must be PAGE-aligned");
static_assert(kObjectBytesPageTable % CONFIG_PAGE_SIZE == 0,
              "PageTable object size must be PAGE-aligned");
static_assert(kObjectBytesNotification % CONFIG_PAGE_SIZE == 0,
              "Notification object size must be PAGE-aligned");
static_assert(kObjectBytesSchedContext % CONFIG_PAGE_SIZE == 0,
              "SchedContext object size must be PAGE-aligned");

} // namespace kernel::cap
