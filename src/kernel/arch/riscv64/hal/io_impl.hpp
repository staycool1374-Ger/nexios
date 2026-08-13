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

/// @file io_impl.hpp
/// @brief RISC-V64 I/O HAL — MMIO load/store, CSR access, WFI, FPU enable,
///        and stub x86 port I/O.

#pragma once

#include <types.hpp>

namespace arch {

/// @brief MMIO write of an 8-bit value.
/// @param addr Destination address.
/// @param val  Value to write.
inline void mmio_write8(volatile void *addr, uint8_t val) {
    asm volatile("sb %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
}
/// @brief MMIO read of an 8-bit value.
/// @param addr Source address.
/// @return Value read.
inline uint8_t mmio_read8(const volatile void *addr) {
    uint8_t val{};
    asm volatile("lbu %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}
/// @brief MMIO write of a 32-bit value.
/// @param addr Destination address.
/// @param val  Value to write.
inline void mmio_write32(volatile void *addr, uint32_t val) {
    asm volatile("sw %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
}
/// @brief MMIO read of a 32-bit value.
/// @param addr Source address.
/// @return Value read.
inline uint32_t mmio_read32(const volatile void *addr) {
    uint32_t val{};
    asm volatile("lw %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

/// @brief Stub x86 outb (no-op on RISC-V).
inline void outb(uint16_t port, uint8_t val) {
    (void)port;
    (void)val;
}
/// @brief Stub x86 inb (no-op on RISC-V).
inline uint8_t inb(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief Stub x86 outw (no-op on RISC-V).
inline void outw(uint16_t port, uint16_t val) {
    (void)port;
    (void)val;
}
/// @brief Stub x86 inw (no-op on RISC-V).
inline uint16_t inw(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief Stub x86 outl (no-op on RISC-V).
inline void outl(uint16_t port, uint32_t val) {
    (void)port;
    (void)val;
}
/// @brief Stub x86 inl (no-op on RISC-V).
inline uint32_t inl(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief I/O delay (nop on RISC-V).
inline void io_wait() {
    asm volatile("nop");
}

/// @brief Halt CPU via WFI instruction.
inline void hlt() {
    asm volatile("wfi");
}
/// @brief CPU pause hint (nop on RISC-V).
inline void pause() {
    asm volatile("nop");
}
/// @brief Set AC-equivalent — no-op (no SMAP on RISC-V; shared stub).
inline void stac() {}
/// @brief Clear AC-equivalent — no-op (see stac()).
inline void clac() {}
/// @brief Read the AC-equivalent flag — no-op placeholder.
inline uint64_t read_rflags() {
    return 0;
}
/// @brief Disable interrupts (clear SIE bit in sstatus).
/// @brief Disable interrupts (clear SIE bit in sstatus).
inline void cli() {
    asm volatile("csrc sstatus, %0" : : "r"((uint64_t)(1 << 1)) : "memory");
}
/// @brief Enable interrupts (set SIE bit in sstatus).
inline void sti() {
    asm volatile("csrs sstatus, %0" : : "r"((uint64_t)(1 << 1)) : "memory");
}

/// @brief Check whether interrupts are enabled.
/// @return true if SIE in sstatus is set.
inline bool interrupts_enabled() {
    uint64_t sstatus{};
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return (sstatus & (1 << 1)) != 0;
}

/// @brief Read the mtime CSR (equivalent to x86 RDTSC).
/// @return 64-bit cycle count from the time CSR.
inline uint64_t rdtsc() {
    uint64_t val{};
    asm volatile("csrr %0, time" : "=r"(val));
    return val;
}

/// @brief Exit QEMU via SBI_SHUTDOWN ecall. Never returns.
/// @param code Exit code (ignored on RISC-V; SBI_SHUTDOWN takes none).
/// @warning Halts the CPU in an infinite WFI loop if ecall fails.
inline void qemu_debug_exit(uint8_t) {
    // SBI_SHUTDOWN via ECALL
    asm volatile("li a7, 8\n\t"
                 "ecall"
                 :
                 :
                 : "a7", "memory");
    while (1) {
        asm volatile("wfi");
    }
}

/// @brief Global TLB flush via sfence.vma.
inline void sfence_vma() {
    asm volatile("sfence.vma" : : : "memory");
}
/// @brief TLB flush for a specific virtual address.
/// @param vaddr Virtual address to flush.
inline void sfence_vma_va(uint64_t vaddr) {
    asm volatile("sfence.vma %0" : : "r"(vaddr) : "memory");
}

/// @brief Read the SATP CSR (supervisor address translation and protection).
/// @return Current SATP value.
inline uint64_t read_satp() {
    uint64_t v{};
    asm volatile("csrr %0, satp" : "=r"(v));
    return v;
}
/// @brief Write the SATP CSR with a full TLB flush.
/// @param v New SATP value.
inline void write_satp(uint64_t v) {
    asm volatile("csrw satp, %0" : : "r"(v) : "memory");
    sfence_vma();
}

/// @brief Read the page-table root physical address (x86 CR3 compat).
/// @return Physical address of the root page table.
inline uint64_t read_cr3() {
    return (read_satp() & 0xFFFFFFFFFFF) << 12;
}
/// @brief Write the page-table root with Sv39 mode (x86 CR3 compat) + TLB
/// flush.
/// @param v Physical address of the root page table.
inline void write_cr3(uint64_t v) {
    write_satp((8ULL << 60) | (v >> 12));
}
/// @brief Write the page-table root without TLB flush.
/// @param v SATP value (PPN already shifted).
inline void write_cr3_notlbi(uint64_t v) {
    write_satp((8ULL << 60) | v);
}

/// @brief Stub read_cr0 (no x86 CR0 on RISC-V).
/// @return Always 0.
inline uint64_t read_cr0() {
    return 0;
}
/// @brief Stub write_cr0 (no-op on RISC-V).
inline void write_cr0(uint64_t) {
}
/// @brief Stub read_cr4 (no x86 CR4 on RISC-V).
/// @return Always 0.
inline uint64_t read_cr4() {
    return 0;
}

/// @brief Enable the FPU by setting the FS field in sstatus to 0b11 (dirty).
inline void fp_enable() {
    // RISC-V: FS field in mstatus/sstatus, set to 0b11 (dirty)
    uint64_t sstatus{};
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= (3ULL << 13);
    asm volatile("csrw sstatus, %0" : : "r"(sstatus) : "memory");
}

/// @brief Wraps arch I/O primitives into a class interface for generic use.
class ArchIO {
  public:
    /// @brief Write 8-bit value to I/O port (no-op on RISC-V).
    static inline void outb(uint16_t port, uint8_t val) {
        arch::outb(port, val);
    }
    /// @brief Read 8-bit value from I/O port (returns 0 on RISC-V).
    static inline uint8_t inb(uint16_t port) {
        return arch::inb(port);
    }
    /// @brief Write 16-bit value to I/O port (no-op on RISC-V).
    static inline void outw(uint16_t port, uint16_t val) {
        arch::outw(port, val);
    }
    /// @brief Read 16-bit value from I/O port (returns 0 on RISC-V).
    static inline uint16_t inw(uint16_t port) {
        return arch::inw(port);
    }
    /// @brief Write 32-bit value to I/O port (no-op on RISC-V).
    static inline void outl(uint16_t port, uint32_t val) {
        arch::outl(port, val);
    }
    /// @brief Read 32-bit value from I/O port (returns 0 on RISC-V).
    static inline uint32_t inl(uint16_t port) {
        return arch::inl(port);
    }
    /// @brief I/O delay.
    static inline void io_wait() {
        arch::io_wait();
    }
    /// @brief Halt CPU (WFI).
    static inline void hlt() {
        arch::hlt();
    }
    /// @brief CPU pause hint.
    static inline void pause() {
        arch::pause();
    }
    /// @brief Disable interrupts.
    static inline void cli() {
        arch::cli();
    }
    /// @brief Enable interrupts.
    static inline void sti() {
        arch::sti();
    }
};

} // namespace arch
