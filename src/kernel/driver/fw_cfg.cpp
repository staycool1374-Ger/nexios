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

/// @file fw_cfg.cpp
/// @brief Minimal QEMU fw_cfg MMIO driver (issue #394 Slice B). Boot-time,
/// polled, bounded: no interrupts, no allocation, no scheduler interaction.

#include <kernel/driver/fw_cfg.hpp>

#if !defined(CONFIG_ARCH_X86_64)

#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <lib/atomic.hpp>
#include <string.hpp>

namespace kernel::fwcfg {

namespace {

// --- Register access (volatile, exact widths) ---

inline volatile uint8_t *data_reg() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<volatile uint8_t *>(arch::fw_cfg_virt() +
                                                arch::FW_CFG_DATA);
}

inline volatile uint16_t *selector_reg() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<volatile uint16_t *>(arch::fw_cfg_virt() +
                                                 arch::FW_CFG_SELECTOR);
}

inline volatile uint64_t *dma_reg() {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<volatile uint64_t *>(arch::fw_cfg_virt() +
                                                 arch::FW_CFG_DMA);
}

inline void select(uint16_t selector) {
    *selector_reg() = arch::fw_cfg_be16(selector);
    kernel::atomic_fence();
}

inline uint8_t read_byte() {
    uint8_t v = *data_reg();
    kernel::atomic_fence();
    return v;
}

inline void read_bytes(void *dst, uint32_t len) {
    auto *out = static_cast<uint8_t *>(dst);
    for (uint32_t i = 0; i < len; ++i) {
        out[i] = read_byte();
    }
}

inline uint32_t read_be32() {
    uint32_t v = 0;
    uint8_t *p = reinterpret_cast<uint8_t *>(&v);
    for (uint32_t i = 0; i < 4; ++i) {
        p[i] = read_byte();
    }
    uint32_t host = 0;
    __builtin_memcpy(&host, p, sizeof(host));
    // Bytes arrived big-endian; host is little-endian.
    return __builtin_bswap32(host);
}

// --- fw_cfg DMA (spec: 16-byte aligned Access struct, BE fields) ---
//
// One page holds the Access struct (+0) and the payload staging area
// (+64).  Staging is required: VMM::virt_to_phys_err only translates
// page-aligned addresses, but DMA payloads (e.g. a stack ramfb config)
// are arbitrarily aligned — the physical address is base + offset,
// computed manually, never translated per-byte.

struct FwCfgDmaAccess {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed, aligned(16)));
static_assert(sizeof(FwCfgDmaAccess) == 16, "fw_cfg DMA access is 16 B");

alignas(4096) uint8_t g_dma_page[4096] = {};
constexpr uint32_t kDmaPayloadOff = 64;

/// @brief Physical address helper (fail-closed 0).
uint64_t phys_of(const void *p) {
    if (!p)
        return 0;
    uint64_t phys = 0;
    if (VMM::virt_to_phys_err(reinterpret_cast<uint64_t>(p), phys) !=
        errors::VMM_ERR_OK) {
        return 0;
    }
    return phys;
}

} // namespace

bool fw_cfg_probe() {
    if (arch::fw_cfg_virt() == 0)
        return false;
    select(arch::FW_CFG_SIG);
    char sig[5] = {};
    read_bytes(sig, 4);
    // "QEMU" — exact order (audit trap: transposed magic passes review,
    // fails only live).
    return sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' &&
           sig[3] == 'U';
}

bool fw_cfg_find_file(const char *name, uint16_t &selector) {
    selector = 0;
    if (!name || !*name)
        return false;
    size_t want = 0;
    while (want < 56 && name[want] != '\0') {
        ++want;
    }
    if (want == 0 || want >= 56 || name[want] != '\0')
        return false;
    select(arch::FW_CFG_FILE_DIR);
    uint32_t count = read_be32();
    if (count == 0 || count > arch::FW_CFG_MAX_DIR_FILES)
        return false;
    // Bounded directory scan: count * 64 B, static scratch (8 KiB).
    struct DirEntry {
        uint32_t size;
        uint16_t select;
        uint16_t reserved;
        char filename[56];
    } __attribute__((packed));
    static_assert(sizeof(DirEntry) == 64, "fw_cfg dir entry is 64 B");
    alignas(8) static uint8_t dir_buf[128 * 64] = {};
    read_bytes(dir_buf, count * sizeof(DirEntry));
    for (uint32_t i = 0; i < count; ++i) {
        DirEntry e{};
        __builtin_memcpy(&e, dir_buf + i * sizeof(e), sizeof(e));
        if (__builtin_bswap16(e.select) == 0)
            continue; // unused slot
        size_t j = 0;
        while (j < want && e.filename[j] == name[j]) {
            ++j;
        }
        if (j == want && e.filename[j] == '\0') {
            selector = __builtin_bswap16(e.select);
            return true;
        }
    }
    return false;
}

bool fw_cfg_dma_write(uint16_t selector, const void *src, uint32_t len) {
    if (selector == 0 || !src || len == 0 || len > sizeof(g_dma_page) - kDmaPayloadOff)
        return false;
    uint64_t page_phys = phys_of(g_dma_page);
    if (!page_phys)
        return false;
    memcpy(g_dma_page + kDmaPayloadOff, src, len);
    auto *acc = reinterpret_cast<FwCfgDmaAccess *>(g_dma_page);
    // Control bit 3 (SELECT) takes the file index from the UPPER 16 bits
    // — the selector-register write above is NOT enough (spec: SELECT
    // overrides it with control[31:16]). A zero index targets file 0
    // ("QEMU", 4 B, read-only) and the 28 B write is rejected with ERROR.
    acc->control = arch::fw_cfg_be32(arch::FW_CFG_DMA_SELECT |
                                     arch::FW_CFG_DMA_WRITE |
                                     (static_cast<uint32_t>(selector) << 16));
    acc->length = arch::fw_cfg_be32(len);
    acc->address = arch::fw_cfg_be64(page_phys + kDmaPayloadOff);
    kernel::atomic_fence();
    select(selector);
    *dma_reg() = arch::fw_cfg_be64(page_phys);
    kernel::atomic_fence();
    int timeout = arch::FW_CFG_DMA_TIMEOUT_ITERS;
    uint32_t ctrl = 0;
    // Volatile read: the device writes control asynchronously — a plain
    // load could be hoisted out of the loop by the compiler.
    volatile uint32_t *ctrl_reg =
        reinterpret_cast<volatile uint32_t *>(&acc->control);
    do {
        ctrl = __builtin_bswap32(*ctrl_reg);
        kernel::atomic_fence();
        --timeout;
        // Fast-fail on device ERROR (a set ERROR bit never clears to 0 —
        // without this the loop always runs to timeout on rejection).
    } while (ctrl != 0 && (ctrl & arch::FW_CFG_DMA_ERROR) == 0 &&
             timeout > 0);
    if (timeout <= 0)
        return false;
    return (ctrl & arch::FW_CFG_DMA_ERROR) == 0;
}

} // namespace kernel::fwcfg

#endif // !CONFIG_ARCH_X86_64
