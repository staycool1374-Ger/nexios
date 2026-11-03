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

// Step cap for the /cpus child walk (flat sibling list; 256 nodes is
// orders of magnitude above any real topology, still fully bounded).
constexpr int kMaxCpuWalkSteps = 256;

// PSCI compatible strings, newest first. Probe order is a contract:
// slices 2/3 boot the first match exclusively, never merge across nodes.
constexpr const char *kPsciCompatV1 = "arm,psci-1.0";
constexpr const char *kPsciCompatV02 = "arm,psci-0.2";
constexpr const char *kPsciCompatBase = "arm,psci";

// Bounded exact string compare inside a property payload (the property
// may lack a NUL within bounds; plain strcmp could over-read).
bool prop_is_str(const char *prop, int prop_len, const char *want) {
    if (prop == nullptr || want == nullptr || prop_len <= 0) return false;
    int idx = 0;
    while (want[idx] != '\0') {
        if (idx >= prop_len || prop[idx] != want[idx]) return false;
        ++idx;
    }
    return idx < prop_len && prop[idx] == '\0';
}

} // namespace

int dtb_walk_cpus(const void *fdt, DtbCpuEntry *out_entries, int max_entries) {
    if (fdt == nullptr || out_entries == nullptr || max_entries <= 0) {
        return 0;
    }
    if (fdt_check_layout(fdt) != 0 || fdt_check_structure(fdt) != 0) {
        return 0;
    }
    // /cpus lookup by name (the vendored libfdt has no path helper;
    // name length 4 covers "cpus" without the NUL by namelen contract).
    int cpus_node = fdt_subnode_offset_namelen(fdt, 0, "cpus", 4);
    if (cpus_node < 0) return 0;
    // #address-cells on /cpus is probed, never assumed: absent or
    // non-{1,2} means the topology is untrusted (skew -> halt upstream).
    int cells_len = 0;
    const void *cells_prop =
        fdt_getprop_namelen(fdt, cpus_node, "#address-cells", &cells_len);
    if (cells_prop == nullptr || cells_len != 4) return 0;
    uint32_t addr_cells = 0;
    __builtin_memcpy(&addr_cells, cells_prop, sizeof(addr_cells));
    addr_cells = __builtin_bswap32(addr_cells);
    if (addr_cells < 1 || addr_cells > 2) return 0;
    int found = 0;
    int steps = 0;
    int node = fdt_first_child(fdt, cpus_node);
    while (node >= 0) {
        if (steps >= kMaxCpuWalkSteps) return 0;
        ++steps;
        int next_node = fdt_next_sibling(fdt, node);
        int type_len = 0;
        const char *type_prop = static_cast<const char *>(
            fdt_getprop_namelen(fdt, node, "device_type", &type_len));
        if (type_prop == nullptr || !prop_is_str(type_prop, type_len, "cpu")) {
            node = next_node;
            continue; // Probe: only cpu children count, never names.
        }
        if (found >= max_entries) return 0; // Unrepresentable: halt.
        int reg_len = 0;
        const void *reg_prop =
            fdt_getprop_namelen(fdt, node, "reg", &reg_len);
        if (reg_prop == nullptr ||
            reg_len != static_cast<int>(addr_cells * 4)) {
            return 0; // Malformed cpu node: whole walk untrusted.
        }
        uint64_t cpu_id = 0;
        const uint8_t *reg_bytes = static_cast<const uint8_t *>(reg_prop);
        for (uint32_t cell_idx = 0; cell_idx < addr_cells; ++cell_idx) {
            uint32_t cell_val = 0;
            __builtin_memcpy(&cell_val, reg_bytes + cell_idx * 4,
                             sizeof(cell_val));
            cpu_id = (cpu_id << 32) | __builtin_bswap32(cell_val);
        }
        CpuEnableMethod method = CpuEnableMethod::UNKNOWN;
        int method_len = 0;
        const char *method_prop = static_cast<const char *>(
            fdt_getprop_namelen(fdt, node, "enable-method", &method_len));
        if (method_prop != nullptr) {
            if (prop_is_str(method_prop, method_len, "psci")) {
                method = CpuEnableMethod::PSCI;
            } else if (prop_is_str(method_prop, method_len, "spin-table")) {
                method = CpuEnableMethod::SPIN_TABLE;
            } else {
                return 0; // Unsupported method cannot be booted.
            }
        }
        uint64_t release_addr = 0;
        if (method == CpuEnableMethod::SPIN_TABLE) {
            int rel_len = 0;
            const void *rel_prop = fdt_getprop_namelen(fdt, node,
                                                       "cpu-release-addr",
                                                       &rel_len);
            if (rel_prop == nullptr || rel_len != 8) return 0;
            uint32_t rel_hi = 0;
            uint32_t rel_lo = 0;
            __builtin_memcpy(&rel_hi, rel_prop, sizeof(rel_hi));
            const uint8_t *rel_bytes = static_cast<const uint8_t *>(rel_prop);
            __builtin_memcpy(&rel_lo, rel_bytes + 4, sizeof(rel_lo));
            release_addr = (static_cast<uint64_t>(__builtin_bswap32(rel_hi))
                            << 32) |
                           __builtin_bswap32(rel_lo);
        }
        out_entries[found].id = cpu_id;
        out_entries[found].method = method;
        out_entries[found].release_addr = release_addr;
        ++found;
        node = next_node;
    }
    return found;
}

bool dtb_psci_method(const void *fdt, char *out_method, int max_len) {
    if (fdt == nullptr || out_method == nullptr || max_len <= 0) {
        return false;
    }
    if (fdt_check_layout(fdt) != 0 || fdt_check_structure(fdt) != 0) {
        return false;
    }
    const char *compat_list[] = {kPsciCompatV1, kPsciCompatV02,
                                 kPsciCompatBase};
    int psci_node = -1;
    for (const char *compat : compat_list) {
        int match = -1;
        int hits = dtb_find_compatible_nodes(fdt, compat, &match, 1);
        if (hits > 0) {
            psci_node = match;
            break;
        }
    }
    if (psci_node < 0) return false;
    int prop_len = 0;
    const char *prop = static_cast<const char *>(
        fdt_getprop_namelen(fdt, psci_node, "method", &prop_len));
    if (prop == nullptr || prop_len <= 1) return false;
    int copy_len = 0;
    while (copy_len < prop_len && copy_len < max_len - 1 &&
           prop[copy_len] != '\0') {
        out_method[copy_len] = prop[copy_len];
        ++copy_len;
    }
    if (copy_len >= prop_len || prop[copy_len] != '\0') return false;
    out_method[copy_len] = '\0';
    return true;
}

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
