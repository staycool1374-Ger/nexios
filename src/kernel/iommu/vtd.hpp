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
/// @brief Translation Type (TT) field, bits 3:2 (spec §9.3):
///        TT=00b second-stage translate through ASR; TT=01b Device-TLB
///        (reserved unless ECAP.DT=1); TT=10b pass-through; TT=11b reserved.
constexpr uint64_t kCteTtMask = 3ULL << 2;
/// @brief TT=00b: translate untranslated requests through ASR.
constexpr uint64_t kCteTtTranslate = 0ULL;
/// @brief TT=10b: pass-through — untranslated requests pass through, SSPTPTR
///        ignored (kernel-owned devices).
constexpr uint64_t kCteTtPassthrough = 2ULL << 2;
/// @brief Address Space Root mask (bits 63:12).
constexpr uint64_t kCteAsrMask = 0xFFFFFFFFFFFFF000ULL;

// ---------------------------------------------------------------------------
// Table geometry
// ---------------------------------------------------------------------------
/// @brief Entries per table (4 KiB page / 8-byte entries).
constexpr size_t kEntriesPerTable = 512;
/// @brief Second-level walk depth: L4 -> L3 -> L2 -> L1 (leaf).
constexpr size_t kSlLevels = 4;

// ---------------------------------------------------------------------------
// Remapping-unit MMIO registers (spec §10.4) — phase-2 live enablement
// ---------------------------------------------------------------------------
/// @brief VT-d MMIO register offsets relative to the remapping-unit base
///        (spec §10.4, matching Linux intel-iommu.h and QEMU).  VER_REG at
///        0x0, CAP at 0x8, ECAP at 0x10, GCMD at 0x18, GSTS at 0x1c,
///        RTADDR at 0x20, CCMD at 0x28, FSTS at 0x34, FECTL at 0x38,
///        IQH at 0x80, IQT at 0x88, IQA at 0x90.
namespace mmio {
/// @brief Version register (0x0): major/minor version of the unit.
constexpr uint64_t kVerReg = 0x0;
/// @brief Capability register (0x8): presence/capability bits.
constexpr uint64_t kCapReg = 0x8;
/// @brief Extended capability register (0x10): QI presence (bit 1).
constexpr uint64_t kEcapReg = 0x10;
/// @brief Global Command register (0x18) — GCMD.
constexpr uint64_t kGcmd = 0x18;
/// @brief Global Status register (0x1c) — GSTS.
constexpr uint64_t kGsts = 0x1c;
/// @brief Root Table Address register (0x20) — RTADDR.
constexpr uint64_t kRtaddr = 0x20;
/// @brief Fault Status register (0x34) — FSTS.
constexpr uint64_t kFsts = 0x34;
/// @brief Fault Event Control register (0x38) — FECTL.
constexpr uint64_t kFectl = 0x38;
/// @brief Invalidation Queue Head (0x80) — IQH.
constexpr uint64_t kIqh = 0x80;
/// @brief Invalidation Queue Tail (0x88) — IQT.
constexpr uint64_t kIqt = 0x88;
/// @brief Invalidation Queue Address (0x90) — IQA.
constexpr uint64_t kIqa = 0x90;

// GCMD bits (spec §10.4.5)
/// @brief Translation Enable (GCMD.TE): 1 = translation active.
constexpr uint32_t kGcmdTe = 1U << 31;
/// @brief Set Root Table Pointer (GCMD.SRTP): re-points RTADDR.
constexpr uint32_t kGcmdSrtp = 1U << 30;
/// @brief Enable QI (GCMD.QIE): 1 = invalidation queue active.
constexpr uint32_t kGcmdQie = 1U << 26;

// GSTS bits (spec §10.4.6)
/// @brief Translation Enabled Status (GSTS.TES): 1 = TE took effect.
constexpr uint32_t kGstsTes = 1U << 31;
/// @brief Root Table Pointer Status (GSTS.RTPS): 1 = SRTP processed.
constexpr uint32_t kGstsRtps = 1U << 30;
/// @brief QI Enabled Status (GSTS.QIES): 1 = QIE took effect.
constexpr uint32_t kGstsQies = 1U << 26;

// RTADDR bits (spec §10.4.7)
/// @brief Root Table Type (RTT): 0 = legacy root table.
constexpr uint64_t kRtaddrRtt = 1ULL << 0;
/// @brief Translation Type (TT): 0 = legacy 2-level root.
constexpr uint64_t kRtaddrTt = 1ULL << 10;
/// @brief Root table physical address mask (bits 51:12).
constexpr uint64_t kRtaddrAddrMask = 0x000FFFFFFFFFF000ULL;

// IQA bits (spec §10.4.10)
/// @brief Queue Enable (QEN): 1 = invalidation queue active.
constexpr uint64_t kIqaQen = 1ULL << 0;
/// @brief Queue base physical address mask (bits 51:12).
constexpr uint64_t kIqaAddrMask = 0x000FFFFFFFFFF000ULL;

// FECTL bits (spec §10.4.19)
/// @brief Interrupt Mask (IM): 1 = fault events do not raise interrupts.
constexpr uint32_t kFectlIm = 1U << 31;
/// @brief Interrupt Pending (IP): read by software; cleared by writing.
constexpr uint32_t kFectlIpf = 1U << 30;

// FSTS read-clear bits (spec §10.4.20)
/// @brief Primary Fault Overflow (PFO): fault-log overflow.
constexpr uint32_t kFstsPfo = 1U << 0;
/// @brief Primary Pending Fault (PPF): a fault is pending.
constexpr uint32_t kFstsPpf = 1U << 1;
/// @brief Invalidation Queue Error (IQE): QI processing error.
constexpr uint32_t kFstsIqe = 1U << 4;
} // namespace mmio

// ---------------------------------------------------------------------------
// Queue-Invalidation descriptor layouts (spec §6.5) — phase-2 live flush
// ---------------------------------------------------------------------------
/// @brief Queue-Invalidation descriptor type field (bits 3:0): 0x1 =
///        context-cache invalidation.
constexpr uint64_t kQiTypeCtx = 0x1;
/// @brief Queue-Invalidation descriptor type field (bits 3:0): 0x2 =
///        IOTLB invalidation.
constexpr uint64_t kQiTypeIotlb = 0x2;
/// @brief Global-invalidation granule (bits 4:5 = 01): invalidates the
///        whole cache.  QEMU/Linux encode the granule in bits 4-5.
constexpr uint64_t kQiGranGlobal = 1ULL << 4;
/// @brief Descriptor count in one 4 KiB invalidation queue (16 B each).
constexpr size_t kQiEntries = 256;

} // namespace kernel::iommu::vtd
