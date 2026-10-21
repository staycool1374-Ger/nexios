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

/// @file fw_cfg.hpp
/// @brief Minimal QEMU fw_cfg MMIO driver (issue #394 Slice B). Register
/// file only: no interrupts, no allocation, bounded polls. Wire order is
/// big-endian throughout (selector, DMA control/length/address, file
/// payloads); the helpers byte-swap from host order explicitly.

#include <types.hpp>
#include <kernel/nexios_config.h>

#if !defined(CONFIG_ARCH_X86_64)

#include <kernel/memory/address.hpp>

namespace arch {

// QEMU virt fw_cfg MMIO base: per-arch (DTB-proven — the aarch64 and
// riscv64 virt machines place it differently).
// aarch64 `virt`: fw-cfg@9020000. riscv64 `virt`: fw-cfg@10100000,
// inside the UART's 2MB Device leaf (no extra boot leaf needed).
#if defined(CONFIG_ARCH_AARCH64)
inline constexpr uint64_t FW_CFG_MMIO_BASE = 0x09020000ULL;
#else
inline constexpr uint64_t FW_CFG_MMIO_BASE = 0x10100000ULL;
#endif
// Register offsets (MMIO layout).
inline constexpr uint64_t FW_CFG_DATA = 0x00;     // u8 PIO data (advances)
inline constexpr uint64_t FW_CFG_SELECTOR = 0x08; // u16 BE file selector
inline constexpr uint64_t FW_CFG_DMA = 0x10;      // u64 BE DMA address
// Well-known selectors.
inline constexpr uint16_t FW_CFG_SIG = 0x0000;      // "QEMU" (4 bytes)
inline constexpr uint16_t FW_CFG_FILE_DIR = 0x0019; // file directory
// DMA control bits.
inline constexpr uint32_t FW_CFG_DMA_ERROR = 1U << 0;
inline constexpr uint32_t FW_CFG_DMA_SELECT = 1U << 3;
inline constexpr uint32_t FW_CFG_DMA_WRITE = 1U << 4;
// Bounds.
inline constexpr uint32_t FW_CFG_MAX_DIR_FILES = 128;
inline constexpr int FW_CFG_DMA_TIMEOUT_ITERS = 1000000;

/// @brief fw_cfg MMIO virtual base (HHDM alias / identity, same convention
/// as virtio_mmio_virt()).
inline uint64_t fw_cfg_virt() {
#if defined(CONFIG_ARCH_AARCH64)
    return kernel::HHDM_OFFSET + FW_CFG_MMIO_BASE;
#elif defined(CONFIG_ARCH_RISCV64)
    return FW_CFG_MMIO_BASE; // identity L0[0] window covers MMIO
#else
    return 0;
#endif
}

/// @brief Byte-swap helpers to big-endian wire order (constexpr-safe).
inline constexpr uint16_t fw_cfg_be16(uint16_t v) {
    return static_cast<uint16_t>(((v & 0xFFU) << 8) | ((v >> 8) & 0xFFU));
}
inline constexpr uint32_t fw_cfg_be32(uint32_t v) {
    return __builtin_bswap32(v);
}
inline constexpr uint64_t fw_cfg_be64(uint64_t v) {
    return __builtin_bswap64(v);
}

} // namespace arch

namespace kernel::fwcfg {

/// @brief Probe for QEMU fw_cfg ("QEMU" signature read-back).
/// @return true when the device answers.
bool fw_cfg_probe();

/// @brief Find a file in the fw_cfg directory by exact name.
/// @param name NUL-terminated file name (matched exactly, bounded).
/// @param selector Receives the file selector on success.
/// @return true on success.
bool fw_cfg_find_file(const char *name, uint16_t &selector);

/// @brief DMA-write @p len bytes from @p src into file @p selector.
/// @return true on completion without the ERROR bit.
bool fw_cfg_dma_write(uint16_t selector, const void *src, uint32_t len);

} // namespace kernel::fwcfg

#endif // !CONFIG_ARCH_X86_64
