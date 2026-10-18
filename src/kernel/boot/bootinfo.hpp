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

/// @file bootinfo.hpp
/// @brief Boot information structure — memory regions, DTB, multiboot, command
/// line.

#include <types.hpp>

/// Maximum number of memory regions that can be reported.
#define BOOTINFO_MAX_MEM_REGIONS 8

/// @brief Boot-time information passed from the early boot stage to the kernel.
struct BootInfo {
    /// @brief A single physical memory region reported by the bootloader.
    struct MemRegion {
        uint64_t base; ///< Physical base address.
        uint64_t size; ///< Size in bytes.
        uint32_t type; ///< 1 = usable, 2 = reserved, 3 = ACPI, 4 = MMIO.
    };

    MemRegion mem_regions[BOOTINFO_MAX_MEM_REGIONS]; ///< Memory region array.
    int num_mem_regions;     ///< Number of valid entries in mem_regions.
    uint64_t total_mem_size; ///< Total usable RAM in bytes.
    uint64_t dtb_ptr;        ///< Device-tree blob pointer (0 if not available).
    uint64_t multiboot_magic; ///< Multiboot magic value (0 if not available).
    uint64_t multiboot_info;  ///< Multiboot info structure pointer.
    char cmdline[256];        ///< Boot command-line string.
    int hart_id;              ///< Hart ID (RISC-V 64 only).

    BootInfo()
        : num_mem_regions(0), total_mem_size(0), dtb_ptr(0), multiboot_magic(0),
          multiboot_info(0), hart_id(0) {
        cmdline[0] = '\0';
    }

    /// @brief Add a memory region if the array is not full.
    /// @param base  Physical base address.
    /// @param size  Size in bytes.
    /// @param type  Region type (1 = usable, 2 = reserved, 3 = ACPI, 4 = MMIO).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void add_region(uint64_t base, uint64_t size, uint32_t type) {
        if (num_mem_regions < BOOTINFO_MAX_MEM_REGIONS) {
            mem_regions[num_mem_regions].base = base;
            mem_regions[num_mem_regions].size = size;
            mem_regions[num_mem_regions].type = type;
            num_mem_regions++;
            if (type == 1)
                total_mem_size += size;
        }
    }
};
