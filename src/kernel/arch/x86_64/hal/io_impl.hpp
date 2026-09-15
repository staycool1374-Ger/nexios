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
/// @brief x86_64 I/O port access, control register access, and CPU instruction
/// wrappers.

#pragma once

#include <types.hpp>
#include <kernel/arch/x86_64/hal/cpuid_impl.hpp>

namespace arch {

/// @brief Read CR0.
/// @return Current value of the CR0 register.
inline uint64_t read_cr0() {
    uint64_t v{};
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
/// @brief Read CR2 (page-fault linear address).
/// @return Current value of the CR2 register.
inline uint64_t read_cr2() {
    uint64_t v{};
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
/// @brief Read CR3 (top-level page-table physical address).
/// @return Current value of the CR3 register.
inline uint64_t read_cr3() {
    uint64_t v{};
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
/// @brief Read CR4.
/// @return Current value of the CR4 register.
inline uint64_t read_cr4() {
    uint64_t v{};
    asm volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
/// @brief Write CR3 (activate a new page table).
/// @param v Physical address of the new PML4 table.
inline void write_cr3(uint64_t v) {
    asm volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
/// @brief Write CR0.
/// @param v Value to write to CR0.
inline void write_cr0(uint64_t v) {
    asm volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
/// @brief Write CR4.
/// @param v Value to write to CR4.
inline void write_cr4(uint64_t v) {
    asm volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}
/// @brief CR4.PCIDE bit (17): process-context identifiers (issue #156).
inline constexpr uint64_t CR4_PCIDE = 1ULL << 17;

/// @brief INVPCID invalidation types (issue #157, Intel SDM Vol 2B).
enum class InvpcidType : uint32_t {
    SINGLE_ADDRESS = 0, ///< Invalidate VA + PCID (except globals).
    SINGLE_CONTEXT = 1, ///< Invalidate all of one PCID (except globals).
    ALL_INCL_GLOBAL = 2, ///< Invalidate everything incl. globals.
    ALL_RETAIN_GLOBAL = 3, ///< Invalidate everything except globals.
};

/// @brief 128-bit INVPCID descriptor: PCID in low 12 bits of qword 0,
///        page-aligned linear address in qword 1 (address ignored
///        except for SINGLE_ADDRESS).
struct InvpcidDesc {
    uint64_t pcid;
    uint64_t addr;
};

/// @brief Build a normalized INVPCID descriptor (issue #157).
/// @param type Invalidation type.
/// @param pcid PCID value (masked to 12 bits).
/// @param addr Linear address (page-masked; non-canonical bits cannot
///        survive the mask on canonical inputs).
/// @return Descriptor ready for invpcid_emit().
inline InvpcidDesc invpcid_build_desc(InvpcidType type, uint64_t pcid,
                                      uint64_t addr) {
    (void)type;
    InvpcidDesc desc{};
    desc.pcid = pcid & 0xFFFULL;
    desc.addr = addr & ~0xFFFULL;
    return desc;
}

/// @brief Execute INVPCID (issue #157).  Fail-safe: returns without
///        executing on CPUs without INVPCID support (never #UD).
///        Kernel-only instruction (#GP at CPL>0 — all callers ring 0).
///        Uses the r64 form (legal per SDM: type in range 0-3).
///        Precondition: when the descriptor carries a nonzero PCID
///        (types 0/1), CR4.PCIDE must be set — true on every path that
///        reaches here (boot/AP enable PCIDE before any task runs;
///        callers are post-bring-up unmap/teardown/purge paths).
/// @param type Invalidation type.
/// @param desc Normalized descriptor (see invpcid_build_desc).
inline void invpcid_emit(InvpcidType type, const InvpcidDesc &desc) {
    if (!has_invpcid())
        return;
    // 16-byte descriptor as the m128 operand (register-indirect would
    // render an unsized qword access — operand size mismatch).
    struct alignas(16) RawDesc {
        uint64_t lo;
        uint64_t hi;
    };
    RawDesc raw{desc.pcid, desc.addr};
    uint64_t type_value = static_cast<uint64_t>(type);
    asm volatile("invpcid %1, %0"
                 :
                 : "r"(type_value), "m"(raw)
                 : "memory");
}

/// @brief Set AC (alignment check / SMAP enable) — allows kernel access to
///        user pages while SMAP is active.  Must be paired with clac().
inline void stac() { asm volatile("stac" ::: "cc"); }
/// @brief Clear AC — SMAP faults on any further kernel deref of user pages.
inline void clac() { asm volatile("clac" ::: "cc"); }
/// @brief Read RFLAGS (AC = bit 18).
/// @return Current RFLAGS value.
inline uint64_t read_rflags() {
    uint64_t v{};
    asm volatile("pushfq; pop %0" : "=r"(v));
    return v;
}

/// @brief Initialise the x87 FPU (FNINIT instruction).
inline void fninit() {
    asm volatile("fninit");
}
/// @brief Load the MXCSR register (SSE control/status).
/// @param mxcsr Value to write to MXCSR.
inline void ldmxcsr(uint32_t mxcsr) {
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
}
/// @brief Save x87/SSE state to a 512-byte buffer (FXSAVE instruction).
/// @param buf 512-byte aligned buffer for the state save.
inline void fxsave(void *buf) {
    asm volatile("fxsave %0"
                 : "=m"(*static_cast<uint64_t (*)[64]>(buf))
                 :
                 : "memory");
}
/// @brief Restore x87/SSE state from a 512-byte buffer (FXRSTOR instruction).
/// @param buf 512-byte aligned buffer containing the saved state.
inline void fxrstor(const void *buf) {
    asm volatile("fxrstor %0"
                 :
                 : "m"(*static_cast<const uint64_t (*)[64]>(buf))
                 : "memory");
}

/// @brief Write a byte to an I/O port.
/// @param port Port address.
/// @param val Byte to write.
inline void outb(uint16_t port, uint8_t val) {
    arch_outb(port, val);
}
/// @brief Read a byte from an I/O port.
/// @param port Port address.
/// @return The byte read.
inline uint8_t inb(uint16_t port) {
    return arch_inb(port);
}
/// @brief Write a word (16-bit) to an I/O port.
/// @param port Port address.
/// @param val Word to write.
inline void outw(uint16_t port, uint16_t val) {
    arch_outw(port, val);
}
/// @brief Read a word (16-bit) from an I/O port.
/// @param port Port address.
/// @return The word read.
inline uint16_t inw(uint16_t port) {
    return arch_inw(port);
}
/// @brief Write a long (32-bit) to an I/O port.
/// @param port Port address.
/// @param val Long value to write.
inline void outl(uint16_t port, uint32_t val) {
    arch_outl(port, val);
}
/// @brief Read a long (32-bit) from an I/O port.
/// @param port Port address.
/// @return The long value read.
inline uint32_t inl(uint16_t port) {
    return arch_inl(port);
}
/// @brief I/O delay — write to an unused port to insert a small bus pause.
inline void io_wait() {
    outb(0x80, 0);
}
/// @brief Exit QEMU with a debug exit code (port 0x501).
/// @param code Exit code visible to the QEMU host.
inline void qemu_debug_exit(uint8_t code) {
    outb(0x501, code);
}
/// @brief Halt the CPU until the next interrupt.
inline void hlt() {
    arch_hlt();
}
/// @brief PAUSE instruction — hint to the CPU that this is a spin-wait loop.
inline void pause() {
    arch_pause();
}
/// @brief Clear the interrupt flag (disable interrupts).
inline void cli() {
    arch_cli();
}
/// @brief Set the interrupt flag (enable interrupts).
inline void sti() {
    arch_sti();
}

/// @brief Check whether interrupts are currently enabled.
/// @return true if the IF (interrupt flag) in RFLAGS is set.
inline bool interrupts_enabled() {
    uint64_t rflags = 0;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    return (rflags >> 9) & 1;
}

/// @brief Read the Time-Stamp Counter (RDTSC instruction).
/// @return Current 64-bit TSC value.
inline uint64_t rdtsc() {
    uint32_t tsc_low, tsc_high;
    asm volatile("rdtsc" : "=a"(tsc_low), "=d"(tsc_high));
    return (static_cast<uint64_t>(tsc_high) << 32) | tsc_low;
}

/// @brief Static wrapper class for x86_64 I/O operations.
/// Provides a uniform interface for use in templated or generic code.
class ArchIO {
  public:
    static inline void outb(uint16_t port, uint8_t val) {
        arch::outb(port, val);
    }
    static inline uint8_t inb(uint16_t port) {
        return arch::inb(port);
    }
    static inline void outw(uint16_t port, uint16_t val) {
        arch::outw(port, val);
    }
    static inline uint16_t inw(uint16_t port) {
        return arch::inw(port);
    }
    static inline void outl(uint16_t port, uint32_t val) {
        arch::outl(port, val);
    }
    static inline uint32_t inl(uint16_t port) {
        return arch::inl(port);
    }
    static inline void io_wait() {
        arch::io_wait();
    }
    static inline void hlt() {
        arch::hlt();
    }
    static inline void pause() {
        arch::pause();
    }
    static inline void cli() {
        arch::cli();
    }
    static inline void sti() {
        arch::sti();
    }
};

} // namespace arch
