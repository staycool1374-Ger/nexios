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

/// @file ramfb.cpp
/// @brief QEMU ramfb fallback backend (issue #394 Slice B).

#include <kernel/driver/ramfb.hpp>

#if !defined(CONFIG_ARCH_X86_64)

#include <kernel/driver/fb_backing.hpp>
#include <kernel/driver/fw_cfg.hpp>
#include <logger.hpp>

namespace kernel::ramfb {

bool ramfb_probe(uint32_t &w, uint32_t &h) {
    w = 0;
    h = 0;
    if (!fwcfg::fw_cfg_probe()) {
        return false;
    }
    uint16_t selector = 0;
    if (!fwcfg::fw_cfg_find_file("etc/ramfb", selector)) {
        return false;
    }
    // The GPU path runs first and releases (or never claims) the backing
    // on failure, so ramfb claims it here. backing_init is idempotent —
    // a leftover init is reused, never double-allocated.
    if (!kernel::fb::backing_init(kernel::fb::FB_MAX_WIDTH,
                                  kernel::fb::FB_MAX_HEIGHT)) {
        return false;
    }
    uint64_t phys = 0;
    if (!kernel::fb::backing_phys(0, phys) || phys == 0) {
        return false;
    }
    uint32_t width = kernel::fb::FB_MAX_WIDTH;
    uint32_t height = kernel::fb::FB_MAX_HEIGHT;
    uint64_t span = static_cast<uint64_t>(width) * height * 4;
    if (span > kernel::fb::backing_bytes()) {
        return false;
    }
    RamfbConfig cfg = make_ramfb_config(phys, width, height);
    if (!fwcfg::fw_cfg_dma_write(selector, &cfg, sizeof(cfg))) {
        Logger::error("ramfb: etc/ramfb write failed");
        return false;
    }
    w = width;
    h = height;
    return true;
}

} // namespace kernel::ramfb

#endif // !CONFIG_ARCH_X86_64
