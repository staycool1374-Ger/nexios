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

/// @file dtb.cpp
/// @brief Validated device-tree access (issue #350). See dtb.hpp.

#include <kernel/boot/dtb.hpp>
#include <kernel/boot/bootinfo.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/memory/vmm.hpp>
#include <fdt/libfdt.h>
#include <fdt/fdt.h>
#include <string.hpp>

namespace kernel::boot {

namespace {

// Usable RAM or firmware-reserved RAM (the DTB is firmware-placed live
// boot data and may sit in a reserved carve-out). MMIO/ACPI rejected.
bool dtb_region_usable(uint32_t type) {
    return type == 1 || type == 2;
}

// Overflow-safe containment: [ptr, ptr + span) inside [base, base + size).
bool dtb_span_inside(uint64_t ptr, uint64_t span, uint64_t base,
                     uint64_t size) {
    if (span > size) return false;
    if (ptr < base) return false;
    return ptr - base <= size - span;
}

} // namespace

const void *dtb_from_boot_info() {
    const BootInfo &info = kernel::gs::boot_info();
    uint64_t ptr = info.dtb_ptr;
    if (ptr == 0) return nullptr;
    int region_count = info.num_mem_regions;
    if (region_count < 0 || region_count > BOOTINFO_MAX_MEM_REGIONS)
        return nullptr;
    const BootInfo::MemRegion *home = nullptr;
    for (int idx = 0; idx < region_count; ++idx) {
        const BootInfo::MemRegion &region = info.mem_regions[idx];
        if (!dtb_region_usable(region.type)) continue;
        if (dtb_span_inside(ptr, sizeof(struct fdt_header), region.base,
                            region.size)) {
            home = &region;
            break;
        }
    }
    if (!home) return nullptr;
    const void *dtb =
        reinterpret_cast<const void *>(ptr + arch::HHDM_OFFSET);
    if (fdt_check_layout(dtb) != 0) return nullptr;
    const struct fdt_header *hdr = (const struct fdt_header *)dtb;
    uint32_t total = 0;
    __builtin_memcpy(&total, &hdr->totalsize, sizeof(total));
    total = __builtin_bswap32(total);
    if (!dtb_span_inside(ptr, total, home->base, home->size)) return nullptr;
    if (fdt_check_structure(dtb) != 0) return nullptr;
    return dtb;
}

int dtb_find_compatible_nodes(const void *fdt, const char *compat,
                              int *out_offsets, int max_offsets) {
    if (!fdt || !compat || !out_offsets || max_offsets <= 0)
        return -FDT_ERR_BADOFFSET;
    if (fdt_check_layout(fdt) != 0 || fdt_check_structure(fdt) != 0)
        return -FDT_ERR_BADSTRUCTURE;
    int found = 0;
    // Iterator-stack DFS (no recursion, no allocation). Tree depth is
    // already proven <= FDT_MAX_DEPTH; each level holds the next
    // unvisited sibling at that depth. A step cap terminates the walk
    // even if the walk and the proof ever disagree.
    struct Level {
        int next;
    };
    Level levels[FDT_MAX_DEPTH + 2];
    auto visit_node = [&](int node) {
        int prop_len = 0;
        const void *prop =
            fdt_getprop_namelen(fdt, node, "compatible", &prop_len);
        if (!prop || prop_len <= 0) return;
        // Audit S3: bounded compare — the property may lack a NUL within
        // bounds, so plain strcmp could over-read past prop_len.
        const char *str = (const char *)prop;
        int rest = prop_len;
        while (rest > 0) {
            int idx = 0;
            while (compat[idx] != '\0' && idx < rest && str[idx] == compat[idx])
                ++idx;
            if (compat[idx] == '\0' &&
                (idx >= rest || str[idx] == '\0')) {
                if (found < max_offsets) out_offsets[found++] = node;
                break;
            }
            int adv = 0;
            while (adv < rest && str[adv] != '\0') ++adv;
            ++adv; // skip NUL (clamped below)
            if (adv > rest) break;
            str += adv;
            rest -= adv;
        }
    };
    visit_node(0); // root
    int depth = 1;
    levels[1].next = fdt_first_child(fdt, 0);
    int steps = 0;
    constexpr int kMaxSteps = 16384;
    while (depth >= 1) {
        if (++steps > kMaxSteps || found >= max_offsets) break;
        if (levels[depth].next < 0) {
            --depth;
            continue;
        }
        int node = levels[depth].next;
        levels[depth].next = fdt_next_sibling(fdt, node);
        visit_node(node);
        if (depth + 1 <= FDT_MAX_DEPTH) {
            int child = fdt_first_child(fdt, node);
            if (child >= 0) {
                ++depth;
                levels[depth].next = child;
            }
        }
    }
    return found;
}

bool dtb_node_reg(const void *fdt, int node, uint64_t &base,
                  uint64_t &size) {
    base = 0;
    size = 0;
    if (!fdt) return false;
    if (fdt_check_layout(fdt) != 0 || fdt_check_structure(fdt) != 0)
        return false;
    uint32_t addr_cells = 2;
    uint32_t size_cells = 2;
    int cell_len = 0;
    const void *cell_prop =
        fdt_getprop_namelen(fdt, 0, "#address-cells", &cell_len);
    if (cell_prop && cell_len == 4) {
        uint32_t val = 0;
        __builtin_memcpy(&val, cell_prop, sizeof(val));
        addr_cells = __builtin_bswap32(val);
    }
    cell_prop = fdt_getprop_namelen(fdt, 0, "#size-cells", &cell_len);
    if (cell_prop && cell_len == 4) {
        uint32_t val = 0;
        __builtin_memcpy(&val, cell_prop, sizeof(val));
        size_cells = __builtin_bswap32(val);
    }
    if (addr_cells < 1 || addr_cells > 2) return false;
    if (size_cells < 1 || size_cells > 2) return false;
    int reg_len = 0;
    const void *reg_prop = fdt_getprop_namelen(fdt, node, "reg", &reg_len);
    if (!reg_prop) return false;
    uint32_t need = (addr_cells + size_cells) * 4;
    if (reg_len < (int)need) return false;
    const uint8_t *bytes = (const uint8_t *)reg_prop;
    uint64_t reg_base = 0;
    uint64_t reg_size = 0;
    for (uint32_t idx = 0; idx < addr_cells; ++idx) {
        uint32_t cell = 0;
        __builtin_memcpy(&cell, bytes + idx * 4, sizeof(cell));
        reg_base = (reg_base << 32) | __builtin_bswap32(cell);
    }
    for (uint32_t idx = 0; idx < size_cells; ++idx) {
        uint32_t cell = 0;
        __builtin_memcpy(&cell, bytes + (addr_cells + idx) * 4,
                         sizeof(cell));
        reg_size = (reg_size << 32) | __builtin_bswap32(cell);
    }
    base = reg_base;
    size = reg_size;
    return true;
}

bool dtb_node_cells(const void *fdt, int node, const char *prop_name,
                    uint32_t *out_cells, int max_cells, int &cell_count) {
    cell_count = 0;
    if (!fdt || !prop_name || !out_cells || max_cells <= 0) return false;
    if (fdt_check_layout(fdt) != 0 || fdt_check_structure(fdt) != 0)
        return false;
    int prop_len = 0;
    const void *prop = fdt_getprop_namelen(fdt, node, prop_name, &prop_len);
    if (!prop || prop_len <= 0) return false;
    if ((prop_len % 4) != 0) return false;
    int count = prop_len / 4;
    if (count > max_cells) return false;
    const uint8_t *bytes = (const uint8_t *)prop;
    for (int idx = 0; idx < count; ++idx) {
        uint32_t cell = 0;
        __builtin_memcpy(&cell, bytes + idx * 4, sizeof(cell));
        out_cells[idx] = __builtin_bswap32(cell);
    }
    cell_count = count;
    return true;
}

} // namespace kernel::boot
