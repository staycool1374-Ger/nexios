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

/// @file cache.hpp
/// @brief Data-cache maintenance for DMA (issue #354 Slice C).
/// Clean (write-back) before a device reads memory, invalidate after a
/// device writes memory and before the CPU reads it (see dma.hpp contract).
/// x86_64 is cache-coherent with DMA but the helpers are still correct
/// there; QEMU virt is coherent on both non-x86 arches, so the riscv64
/// fence-only fallback is exact under emulation (#307 retains the
/// non-coherent-silicon residual).

#include <types.hpp>
#include <kernel/nexios_config.h>

namespace arch {

// NOLINTBEGIN(readability-identifier-length)
inline void cache_clean(const void *addr, size_t len) {
    if (!addr || !len)
        return;
#if defined(CONFIG_ARCH_AARCH64)
    uint64_t ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xFU);
    uint64_t start = reinterpret_cast<uint64_t>(addr) & ~(line - 1);
    uint64_t end = reinterpret_cast<uint64_t>(addr) + len;
    for (uint64_t p = start; p < end; p += line) {
        asm volatile("dc cvac, %0" : : "r"(p) : "memory");
    }
    asm volatile("dsb sy" : : : "memory");
    asm volatile("isb");
#elif defined(CONFIG_ARCH_RISCV64)
#if defined(__riscv_zicbom)
    // Block size comes from the DTB (riscv,cbom-block-size); the kernel
    // build does not enable Zicbom today, so this arm is forward-proofing.
    uint64_t start = reinterpret_cast<uint64_t>(addr);
    uint64_t end = start + len;
    for (uint64_t p = start; p < end; p += 64) {
        asm volatile("cbo.clean 0(%0)" : : "r"(p) : "memory");
    }
    asm volatile("fence iorw, iorw" : : : "memory");
#else
    // No Zicbom in the build (-march=rv64imafdc): no cache-maintenance
    // instruction exists — fence orders the (coherent-under-QEMU) accesses.
    // Non-coherent silicon stays a #307 residual, documented in dma.hpp.
    (void)addr;
    (void)len;
    asm volatile("fence iorw, iorw" : : : "memory");
#endif
#else
    // x86_64: clflush per 64 B line (de-facto line size; stepping 64 hits
    // every line of any power-of-2 size <= 64 at least once), mfence after.
    uint64_t start = reinterpret_cast<uint64_t>(addr) & ~63ULL;
    uint64_t end = reinterpret_cast<uint64_t>(addr) + len;
    for (uint64_t p = start; p < end; p += 64) {
        asm volatile("clflush (%0)" : : "r"(p) : "memory");
    }
    asm volatile("mfence" : : : "memory");
#endif
}

inline void cache_invalidate(const void *addr, size_t len) {
    if (!addr || !len)
        return;
#if defined(CONFIG_ARCH_AARCH64)
    uint64_t ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xFU);
    uint64_t start = reinterpret_cast<uint64_t>(addr) & ~(line - 1);
    uint64_t end = reinterpret_cast<uint64_t>(addr) + len;
    for (uint64_t p = start; p < end; p += line) {
        asm volatile("dc ivac, %0" : : "r"(p) : "memory");
    }
    asm volatile("dsb sy" : : : "memory");
    asm volatile("isb");
#elif defined(CONFIG_ARCH_RISCV64)
#if defined(__riscv_zicbom)
    uint64_t start = reinterpret_cast<uint64_t>(addr);
    uint64_t end = start + len;
    for (uint64_t p = start; p < end; p += 64) {
        asm volatile("cbo.inval 0(%0)" : : "r"(p) : "memory");
    }
    asm volatile("fence iorw, iorw" : : : "memory");
#else
    (void)addr;
    (void)len;
    asm volatile("fence iorw, iorw" : : : "memory");
#endif
#else
    // x86_64 is DMA-coherent; clflush doubles as the invalidate path.
    cache_clean(addr, len);
#endif
}

inline void cache_clean_invalidate(const void *addr, size_t len) {
    if (!addr || !len)
        return;
#if defined(CONFIG_ARCH_AARCH64)
    uint64_t ctr = 0;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xFU);
    uint64_t start = reinterpret_cast<uint64_t>(addr) & ~(line - 1);
    uint64_t end = reinterpret_cast<uint64_t>(addr) + len;
    for (uint64_t p = start; p < end; p += line) {
        asm volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    asm volatile("dsb sy" : : : "memory");
    asm volatile("isb");
#elif defined(CONFIG_ARCH_RISCV64)
#if defined(__riscv_zicbom)
    uint64_t start = reinterpret_cast<uint64_t>(addr);
    uint64_t end = start + len;
    for (uint64_t p = start; p < end; p += 64) {
        asm volatile("cbo.flush 0(%0)" : : "r"(p) : "memory");
    }
    asm volatile("fence iorw, iorw" : : : "memory");
#else
    (void)addr;
    (void)len;
    asm volatile("fence iorw, iorw" : : : "memory");
#endif
#else
    cache_clean(addr, len);
#endif
}
// NOLINTEND(readability-identifier-length)

} // namespace arch
