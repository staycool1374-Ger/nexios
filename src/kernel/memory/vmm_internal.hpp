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

/// @file vmm_internal.hpp
/// @brief VMM translation-unit-shared internals (issue #363): test-time
///        kernel-VA touch notifiers, throttled-diagnostic helpers, the
///        Sv39 HHDM slot constant, and page-offset/base masks. Single
///        definitions live in vmm.cpp; the other vmm_*.cpp TUs link
///        against these declarations.
///
/// Mask-home classification (issue #363 P3, owner decision 2):
/// HAL-specific encodings live in hal/ (x86 NX bit:
/// arch::ArchPageTable::NX). Kept here are TU-local shapes: the offset
/// masks (used by shared skeletons on all arches), kHhdmL0Idx and the
/// Sv39 index geometry (VMM-owned; the riscv HAL keeps its own private
/// copies — that duplication is a known residual, not a contract).
/// PAGE_FRAME_MASK and friends stay in vmm.hpp: they are VMM vocabulary
/// shared by skeletons and helpers alike.
///
/// Walk-ownership residual (issue #363 P4): arch leaf-codecs moved to the
/// per-arch TUs (vmm_arch_* helpers, guards-in-file); shared walk
/// skeletons stay VMM-private by design (snapshot pairing, ENSURE
/// ownership checks, drop-note throttling are VMM policy, and the HAL
/// walks current-root tables only). HAL attr_from_flags was deliberately
/// NOT reused: it adds SH/NS + AP policy bits the VMM map path omits,
/// so rewiring would change PTE bits (audited rationale).

#pragma once

#include <types.hpp>

namespace kernel {

// Test-time kernel-VA touch notifiers (issue #254 H2: eleven
// is_test_active() control-flow branches hoisted here, bit-identical).
// WHY the flags exist: test tasks legitimately map/unmap kernel-half VAs;
// the snapshot mechanism must know which half was touched so restore rewinds
// exactly that half (HHDM PD / identity PD / Sv39 L1).  Release stores pair
// with the acquire take in snapshot_restore (issues #60/#152).  WHY the
// warns stay: silent kernel-half mutation hides test pollution; each message
// names the operation + index domain so the polluting test is identifiable.
enum class TestTouchWarn {
    kMapL1,
    kMapPd,
    kUnmapL0,
    kUnmapPml4,
    kAccessPml4,
};

/// @brief Warn (once per boot) when a test touches kernel-half tables.
void warn_test_kernel_touch(bool gate, TestTouchWarn warn, uint64_t va,
                              size_t idx);
/// @brief Arm the HHDM/identity modification flags for the snapshot backend.
void arm_test_touch_flags(bool arm_hhdm, bool arm_identity);

// Cap for bounded throttled diagnostics (issue #254 H5: was a raw 24).
inline constexpr uint64_t kFupSkipLogCap = 24;

// Page-offset / block-base masks (issue #363 checkbox 2): single named
// definitions replacing inline 0xFFF/0x1FFFFF/0x3FFFFF literals. All arches
// use 4 KiB base pages; x86_64/aarch64 split 2 MiB huge leaves, the PDPT
// free path reasons about 1 GiB block bases.
inline constexpr uint64_t kPageOffsetMask = 0xFFFULL;
inline constexpr uint64_t kHuge2MOffsetMask = 0x1FFFFFULL;
inline constexpr uint64_t kHuge2MBaseMask = ~0x1FFFFFULL;
inline constexpr uint64_t kPdptBlockBaseMask = ~0x3FFFFFULL;
static_assert(kPageOffsetMask + 1 == CONFIG_PAGE_SIZE,
              "page offset mask must match the base page size");
static_assert(kHuge2MOffsetMask == 512 * CONFIG_PAGE_SIZE - 1,
              "2M offset mask must cover exactly one huge leaf");
static_assert(kHuge2MBaseMask == ~kHuge2MOffsetMask,
              "huge base mask must be the offset mask complement");
// NOTE: kPdptBlockBaseMask preserves the pre-split 22-bit shape verbatim
// (free_user_pages PDPT-block base). It is deliberately NOT re-derived
// from 1 GiB geometry here — any reinterpretation needs an owner decision
// (issue #363 follow-up), so no static_assert pins a geometry claim.

/// @brief Hex-value diagnostic writer (debugcon port IO only).
void write_hex_debugcon(uint64_t val);
/// @brief Bounded one-shot diagnostic for silently-dropped mappings.
void note_silent_drop_once(const char *tag, uint64_t val);

#if defined(CONFIG_ARCH_RISCV64)
// Issue #152: Sv39 L0 slot covered by the snapshot backend. The HHDM RAM
// window (boot.S: 128 × 2MB leaves for PA 0x80000000-0x8FFFFFFF) lives
// under L0[258] — the only HHDM L1 tests can split (L0[256/257] map
// phys below RAM and have no block leaves). Gating is exact-match so the
// armed flag always has a restore covering it (no silent holes). There
// is deliberately no identity slot: the boot identity map (L0[2]) is
// dead at runtime (L0[2] reads zero; the kernel runs via HHDM).
inline constexpr size_t kHhdmL0Idx = 258;
static_assert(
    ((CONFIG_HHDM_OFFSET + 0x80000000ULL) >> 30 & VMM::TABLE_MASK) ==
        kHhdmL0Idx,
    "HHDM RAM L0 moved");
#endif

} // namespace kernel
