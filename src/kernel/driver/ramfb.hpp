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

/// @file ramfb.hpp
/// @brief QEMU ramfb fallback backend (issue #394 Slice B). `etc/ramfb`
/// config written via fw_cfg DMA; points the host scanout at the same
/// PMM backing the virtio-gpu path uses. Present is a no-op (the host
/// reads guest RAM directly).

#include <types.hpp>

#if !defined(CONFIG_ARCH_X86_64)

namespace kernel::ramfb {

// DRM XRGB8888 fourcc ("XR24").
inline constexpr uint32_t RAMFB_FOURCC_XRGB8888 = 0x34325258;

/// @brief `etc/ramfb` config (28 B, big-endian wire order).
struct RamfbConfig {
    uint64_t addr_be;
    uint32_t fourcc_be;
    uint32_t flags_be;
    uint32_t width_be;
    uint32_t height_be;
    uint32_t stride_be;
} __attribute__((packed));
static_assert(sizeof(RamfbConfig) == 28, "ramfb config is 28 B");

/// @brief Encode an `etc/ramfb` config (pure, QE-testable).
/// Wire order is big-endian; the swaps below run at compile time.
/// @param phys Guest-physical backing address (page-aligned, validated).
/// @param width Width in pixels; @param height Height in pixels (validated: fit in u32
/// when multiplied by 4 — the caller upholds the FB_MAX bound).
/// @return Big-endian wire encoding.
constexpr RamfbConfig make_ramfb_config(uint64_t phys, uint32_t width,
                                        uint32_t height) {
    RamfbConfig cfg{};
    cfg.addr_be = __builtin_bswap64(phys);
    cfg.fourcc_be = __builtin_bswap32(RAMFB_FOURCC_XRGB8888);
    cfg.flags_be = 0;
    cfg.width_be = __builtin_bswap32(width);
    cfg.height_be = __builtin_bswap32(height);
    cfg.stride_be = __builtin_bswap32(width * 4);
    return cfg;
}

/// @brief Probe ramfb: fw_cfg → `etc/ramfb` → point at the fb backing.
/// @param w Receives the width on success.
/// @param h Receives the height on success.
/// @return true when ramfb accepted the config.
bool ramfb_probe(uint32_t &w, uint32_t &h);

} // namespace kernel::ramfb

#endif // !CONFIG_ARCH_X86_64
