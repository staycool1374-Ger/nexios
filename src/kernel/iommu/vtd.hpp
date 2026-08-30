/*
 * NexIOS RTOS — IOMMU DMA protection
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

/// @file vtd.hpp
/// @brief Intel VT-d entry bit layouts (spec §9) — single source of truth
/// for the second-level / root / context entries programmed by IoMmuManager.
/// Bit fidelity is the security boundary: the unit tests assert field
/// placement by direct table inspection.

#pragma once

#include <types.hpp>

namespace kernel::iommu::vtd {

// ---------------------------------------------------------------------------
// Second-level page-table entry (4-level, 4 KiB pages, spec §9.3)
// ---------------------------------------------------------------------------
/// @brief Read permission bit (bit 0).
constexpr uint64_t kSlEntryRead = 1ULL << 0;
/// @brief Write permission bit (bit 1).
constexpr uint64_t kSlEntryWrite = 1ULL << 1;
/// @brief Execute permission bit (bit 2).
constexpr uint64_t kSlEntryExec = 1ULL << 2;
/// @brief Physical page address mask (bits 51:12 of the entry).
constexpr uint64_t kSlEntryAddrMask = 0x000FFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// Root-table entry (16 bytes, spec §9.1)
// ---------------------------------------------------------------------------
/// @brief Present bit (bit 0).
constexpr uint64_t kRootEntryPresent = 1ULL << 0;
/// @brief Context-table pointer mask (bits 51:12 of the entry).
constexpr uint64_t kRootEntryCtpMask = 0x000FFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// Legacy context-table entry (16 bytes, spec §9.3)
// ---------------------------------------------------------------------------
/// @brief Present bit (bit 0).
constexpr uint64_t kCtePresent = 1ULL << 0;
/// @brief Fault Processing Disable bit (bit 1).
constexpr uint64_t kCteFpd = 1ULL << 1;
/// @brief Translation Type T = 1 (bits 3:2): translate requests through ASR.
///        T = 0 passthrough is reserved for kernel-owned devices (phase-2).
constexpr uint64_t kCteTranslate = 1ULL << 2;
/// @brief Address Space Root mask (bits 63:12).
constexpr uint64_t kCteAsrMask = 0xFFFFFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// Table geometry
// ---------------------------------------------------------------------------
/// @brief Entries per table (4 KiB page / 8-byte entries).
constexpr size_t kEntriesPerTable = 512;
/// @brief Second-level walk depth: L4 -> L3 -> L2 -> L1 (leaf).
constexpr size_t kSlLevels = 4;

} // namespace kernel::iommu::vtd
