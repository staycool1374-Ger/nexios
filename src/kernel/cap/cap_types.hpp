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

/// @file cap_types.hpp
/// @brief CSpace capability type, rights and slot definitions (ROADMAP 0.4.1).

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/memory/kernel_object.hpp>

namespace kernel::cap {

/// @brief Capability target types (iteration-1 slice).
///        Null = empty slot; Task/Endpoint/Frame/CNode are live targets.
enum class CapType : uint8_t {
    Null = 0,
    Task = 1,
    Endpoint = 2,
    Frame = 3,
    CNode = 4,
    Untyped = 5, ///< retype target (ROADMAP 0.4.1 item 3)
    Mmio = 6,    ///< MMIO BAR range (v0.4.2, issue #3)
};

/// @brief Rights bitmap, checked by the operation that consumes the cap.
enum CapRights : uint32_t {
    CAP_RIGHT_READ = 1u << 0,  ///< endpoint recv / frame read-mapping
    CAP_RIGHT_WRITE = 1u << 1, ///< endpoint send / frame write-mapping
    CAP_RIGHT_GRANT = 1u << 2, ///< may grant (copy into another CSpace)
    CAP_RIGHT_COPY = 1u << 3,  ///< may copy (duplicate into own CSpace)
};

/// @brief Bits allocated to the cspace id in a capability handle.
constexpr uint32_t CAP_HANDLE_IDBITS = 8;

/// @brief Bits allocated to the slot index in a capability handle.
///        Derived from CONFIG_CSLOT_COUNT (power of two preferred).
constexpr uint32_t cap_slot_bits() noexcept {
    uint32_t bits = 0;
    uint32_t slots = static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
    while ((1u << bits) < slots)
        ++bits;
    return bits;
}

constexpr uint32_t CAP_SLOT_BITS = cap_slot_bits();
constexpr uint32_t CAP_SLOT_MASK = (1u << CAP_SLOT_BITS) - 1u;

/// @brief One slot in a CNode: a strong reference (acquire()) to the target.
struct CSlot {
    KernelObject *obj = nullptr; ///< target; null when unoccupied
    CapType type = CapType::Null;
    uint32_t rights = 0;
    uint32_t gen = 0; ///< slot generation; stale-handle detection
    bool occupied = false;
};

/// @brief Encodes a capability handle from its fields.
///        Layout: [gen : (IDBITS+SLOTBITS)] [cspace_id : IDBITS]
///                [slot_index : SLOTBITS]
constexpr uint64_t encode_handle(uint32_t cspace_id, uint32_t slot_index,
                                 uint32_t gen) noexcept {
    constexpr uint32_t shift = CAP_HANDLE_IDBITS + CAP_SLOT_BITS;
    return (static_cast<uint64_t>(gen) << shift) |
           (static_cast<uint64_t>(cspace_id & 0xFFu) << CAP_SLOT_BITS) |
           (static_cast<uint64_t>(slot_index) & CAP_SLOT_MASK);
}

/// @brief Extracts the slot index from a capability handle.
constexpr uint32_t handle_slot(uint64_t handle) noexcept {
    return static_cast<uint32_t>(handle & CAP_SLOT_MASK);
}

/// @brief Extracts the cspace id from a capability handle.
constexpr uint32_t handle_cspace(uint64_t handle) noexcept {
    return static_cast<uint32_t>((handle >> CAP_SLOT_BITS) & 0xFFu);
}

/// @brief Extracts the generation from a capability handle.
constexpr uint32_t handle_gen(uint64_t handle) noexcept {
    return static_cast<uint32_t>(handle >> (CAP_HANDLE_IDBITS + CAP_SLOT_BITS));
}

/// @brief True if the slot index fits this CNode's table.
constexpr bool slot_index_valid(uint32_t idx) noexcept {
    return idx < static_cast<uint32_t>(CONFIG_CSLOT_COUNT);
}

} // namespace kernel::cap
