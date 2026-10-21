#pragma once

#include <types.hpp>
#include <constants.hpp>

/// @brief RISC-V 64 QEMU virt memory-map literals (issue #387: single
/// arch-owned source — the kernel and the arch tests share these,
/// never raw address literals).
/// @note QEMU virt: RAM starts at 0x80000000. The HHDM RAM window maps
///       PA 0x80000000-0x8FFFFFFF as 128 x 2 MiB block leaves under one
///       L0 slot (boot.S). Kernel image + initrd + boot reservations
///       split L0-slot blocks L1[1..5] at boot (#376 harden); L1[6]
///       (boot stack slack) and L1[7] (boot stack) are boot-reserved —
///       scratch windows must live at L1[8] and above.
namespace arch {

/// @brief QEMU virt DRAM base (physical).
inline constexpr uint64_t RISCV_DRAM_BASE = 0x80000000ULL;
/// @brief Sv39 L1 block size (2 MiB) — the HHDM window granularity.
inline constexpr uint64_t RISCV_L1_BLOCK_SIZE = 0x200000ULL;
/// @brief Sv39 L1 block index shift (log2 of the block size).
inline constexpr unsigned RISCV_L1_BLOCK_SHIFT = 21;
/// @brief Page-table index geometry: 512 entries per level (L0 shift 30).
inline constexpr unsigned RISCV_L0_SHIFT = 30;
inline constexpr uint64_t RISCV_TABLE_INDEX_MASK = 0x1FFULL;
/// @brief Base virtual address of the HHDM RAM window.
inline constexpr uint64_t RISCV_HHDM_RAM_BASE = HHDM_OFFSET + RISCV_DRAM_BASE;
/// @brief L0 slot covering the HHDM RAM window (derivation pinned: any
///        HHDM/DRAM move breaks the assert here, not in a test).
inline constexpr size_t RISCV_HHDM_RAM_L0 =
    static_cast<size_t>((RISCV_HHDM_RAM_BASE >> 30) & 0x1FF);
static_assert(RISCV_HHDM_RAM_L0 == 258, "HHDM RAM window L0 slot moved");

} // namespace arch
