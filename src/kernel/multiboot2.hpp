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

/// @file multiboot2.hpp
/// @brief Multiboot2 structures for boot information parsing.

#pragma once

#include <types.hpp>

/// @brief Generic Multiboot2 tag header.
struct Multiboot2Tag {
    uint32_t type;
    uint32_t size;
};

/// @brief Multiboot2 information structure header.
struct Multiboot2Info {
    uint32_t total_size;
    uint32_t reserved;
};

/// @brief Framebuffer information tag from Multiboot2.
struct FramebufferTag {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type_specific;
    uint8_t reserved[2];
};

/// @brief Entry in the memory map from Multiboot2.
struct MemoryMapEntry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

/// @brief Memory map tag containing variable-length entries.
struct MemoryMapTag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    MemoryMapEntry entries[];
};
extern "C" {
/// @brief Multiboot2 magic value (0x36D76289 if booted by Multiboot2).
extern constinit uint64_t multiboot_magic;
/// @brief Physical pointer to the Multiboot2 info structure.
extern constinit uint64_t multiboot_info_ptr;
}

/// @brief Finds a Multiboot2 tag by type.
/// @param type The tag type to search for.
/// @return Physical address of the tag, or 0 if not found.
/// @note Issue #180: the walk is bounded — a corrupt tag stream (zero-size
///       non-terminal tag: `addr += (0+7)&~7` advances 0) must fail closed
///       instead of hanging the caller with no panic text. Firmware tag
///       counts are < 20 and info structures < 32 KiB, so the caps below
///       never trigger on valid firmware.
inline uint64_t mb2_find_tag(uint32_t type) {
    // Maximum tags walked and info bytes accepted (issue #180).
    constexpr uint64_t kMaxTags = 64;
    constexpr uint64_t kMaxInfoBytes = 256ULL * 1024ULL;
    if (multiboot_magic != 0x36D76289)
        return 0;

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *info = reinterpret_cast<Multiboot2Info *>(
        static_cast<uint64_t>(multiboot_info_ptr));

    uint64_t total = info->total_size;
    if (total == 0 || total > kMaxInfoBytes)
        return 0;
    uint64_t end = multiboot_info_ptr + total;
    if (end < multiboot_info_ptr)
        return 0;
    uint64_t addr = multiboot_info_ptr + 8;
    for (uint64_t tags = 0; tags < kMaxTags; ++tags) {
        if (addr + sizeof(Multiboot2Tag) > end)
            return 0;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *tag = reinterpret_cast<Multiboot2Tag *>(addr);
        if (tag->type == 0)
            break;
        if (tag->type == type)
            return addr;
        if (tag->size < sizeof(Multiboot2Tag))
            return 0;
        uint64_t next = addr + ((tag->size + 7) & ~7ULL);
        if (next <= addr || next > end)
            return 0;
        addr = next;
    }
    return 0;
}
