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
/// @brief AArch64 I/O primitives — MMIO, port I/O stubs, barrier, and FP
/// enable.

#pragma once

#include <types.hpp>

// CONFIG_TCG_BARRIERS: 1 = TCG-safe (dmb sy), 0 = hardware (dsb sy).
// Anything else is a typo that the "undefined identifier == 0" rule would
// silently turn into a hardware build — reject it at compile time.
#ifndef CONFIG_TCG_BARRIERS
#error "CONFIG_TCG_BARRIERS is not defined (nexios_config.h must define 0 or 1)"
#endif
#if CONFIG_TCG_BARRIERS != 0 && CONFIG_TCG_BARRIERS != 1
#error "CONFIG_TCG_BARRIERS must be 1 (TCG: dmb sy) or 0 (hardware: dsb sy)"
#endif

namespace arch {

/// @brief Memory-mapped I/O write (8-bit).
/// @param[in] addr Destination address.
/// @param[in] val Value to write.
inline void mmio_write8(volatile void *addr, uint8_t val) {
    asm volatile("strb %w1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
/// @brief Memory-mapped I/O read (8-bit).
/// @param[in] addr Source address.
/// @return Value read.
inline uint8_t mmio_read8(const volatile void *addr) {
    uint8_t val{};
    asm volatile("ldrb %w0, [%1]" : "=r"(val) : "r"(addr) : "memory");
    return val;
}
/// @brief Memory-mapped I/O write (32-bit).
/// @param[in] addr Destination address.
/// @param[in] val Value to write.
inline void mmio_write32(volatile void *addr, uint32_t val) {
    asm volatile("str %w1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
/// @brief Memory-mapped I/O read (32-bit).
/// @param[in] addr Source address.
/// @return Value read.
inline uint32_t mmio_read32(const volatile void *addr) {
    uint32_t val{};
    asm volatile("ldr %w0, [%1]" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @param[in] val Value (ignored).
inline void outb(uint16_t port, uint8_t val) {
    (void)port;
    (void)val;
}
/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @return Always 0.
inline uint8_t inb(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @param[in] val Value (ignored).
inline void outw(uint16_t port, uint16_t val) {
    (void)port;
    (void)val;
}
/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @return Always 0.
inline uint16_t inw(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @param[in] val Value (ignored).
inline void outl(uint16_t port, uint32_t val) {
    (void)port;
    (void)val;
}
/// @brief x86 port I/O stub — not applicable on AArch64.
/// @param[in] port Legacy port number (ignored).
/// @return Always 0.
inline uint32_t inl(uint16_t port) {
    (void)port;
    return 0;
}
/// @brief I/O wait (no-op on AArch64).
inline void io_wait() {
    asm volatile("nop");
}

/// @brief Halt the CPU (WFI — Wait For Interrupt).
inline void hlt() {
    asm volatile("wfi");
}
/// @brief Yield the CPU (hint to microarchitecture).
inline void pause() {
    asm volatile("yield");
}
/// @brief Whether aarch64 PAN (FEAT_PAN) is available and enabled.  Written
/// once by arch::pan_init() during BOOT (single CPU, IRQs masked); read-only
/// thereafter from any context (incl. IRQ epilogue) — plain read is safe.
extern bool g_pan_supported;

/// @brief Detect FEAT_PAN (ID_AA64MMFR1_EL1.PAN[23:20] != 0), enable PAN via
/// SCTLR_EL1.PAN (bit 23), and default PSTATE.PAN to deny (clac).  Called
/// once from higherhalf_entry on the boot CPU.
/// @return true if PAN was enabled.
bool pan_init();

/// @brief Set AC-equivalent (PAN clear) — permit kernel access to EL0 data.
/// Toggles PSTATE.PAN (bit 22 of the PAN sysreg S3_0_C4_C2_4) to 0.
/// No-op when PAN is unsupported (degraded mode matches the old stub).
inline void stac() {
    if (!g_pan_supported)
        return;
    uint64_t pan{};
    asm volatile("mrs %0, s3_0_c4_c2_4" : "=r"(pan));
    pan &= ~(1ULL << 22);
    asm volatile("msr s3_0_c4_c2_4, %0" : : "r"(pan) : "memory");
}
/// @brief Clear AC-equivalent (PAN set) — default-deny; further kernel deref
/// of EL0-accessible pages faults.  Toggles PSTATE.PAN to 1.
inline void clac() {
    if (!g_pan_supported)
        return;
    uint64_t pan{};
    asm volatile("mrs %0, s3_0_c4_c2_4" : "=r"(pan));
    pan |= (1ULL << 22);
    asm volatile("msr s3_0_c4_c2_4, %0" : : "r"(pan) : "memory");
}
/// @brief Read the AC-equivalent PSTATE bit, normalized to the x86_64
/// contract: bit 18 set means "user-access override active" (PAN clear).
/// Raw PSTATE.PAN is bit 22 of the PAN sysreg with inverted polarity.
inline uint64_t read_rflags() {
    if (!g_pan_supported)
        return 0;
    uint64_t pan{};
    asm volatile("mrs %0, s3_0_c4_c2_4" : "=r"(pan));
    return (pan & (1ULL << 22)) == 0 ? (1ULL << 18) : 0;
}
/// @brief Disable IRQ interrupts (set DAIF bit).
inline void cli() {
    asm volatile("msr daifset, #2" : : : "memory");
}
/// @brief Enable IRQ interrupts (clear DAIF bit).
inline void sti() {
    asm volatile("msr daifclr, #2" : : : "memory");
}

/// @brief Check whether IRQ interrupts are currently enabled.
/// @return true if IRQs are enabled (DAIF bit 7 clear).
inline bool interrupts_enabled() {
    uint64_t daif{};
    asm volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1 << 7)) == 0;
}

/// @brief Read the cycle counter (CNTPCT_EL0).
/// @return Counter value.
/// @note Analogous to x86 RDTSC.
inline uint64_t rdtsc() {
    uint64_t val{};
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/// @brief Exit QEMU with a given exit code via ARM Semihosting.
/// @param[in] code Exit code.
/// @warning This function does not return.
inline void qemu_debug_exit(uint8_t code) {
    // ARM Semihosting SYS_EXIT (0x18) with ADP_Stopped_ApplicationExit
    uint32_t params[2] = {0x18, code};
    uint64_t addr = reinterpret_cast<uint64_t>(params);
    __asm__ volatile("mov x0, #0x18\n\t"
                     "mov x1, %0\n\t"
                     "hlt #0xf000"
                     :
                     : "r"(addr)
                     : "x0", "x1", "memory");
    while (1) {
        asm volatile("wfi");
    }
}

/// @brief Full data synchronisation barrier (DSB SY).
inline void dsb_sy() {
    asm volatile("dsb sy" : : : "memory");
}
/// @brief Instruction synchronisation barrier (ISB).
inline void isb() {
    asm volatile("isb" : : : "memory");
}
/// @brief Data barrier for TLB / address-space handoff paths (issue #309b).
///        Emits `dmb sy` when CONFIG_TCG_BARRIERS == 1 (QEMU TCG, the
///        default) and `dsb sy` when == 0 (silicon). always_inline keeps
///        the TCG instruction stream byte-identical to the pre-#309b code.
/// @note Silicon proof for the == 0 build: issue #397.
[[gnu::always_inline]] inline void system_barrier() {
#if CONFIG_TCG_BARRIERS
    asm volatile("dmb sy" : : : "memory");
#else
    asm volatile("dsb sy" : : : "memory");
#endif
}
/// @brief TLB invalidate by VA (EL1, VMID-aware).
/// @param[in] vaddr Virtual address to invalidate.
inline void tlbi_vae1(uint64_t vaddr) {
    asm volatile("tlbi vae1, %0" : : "r"(vaddr) : "memory");
}
/// @brief TLB invalidate by ASID (EL1, issue #352).  Drops every entry
///        tagged with @p asid.  ASID occupies Xt[63:48] (DDI0601 TLBI
///        fieldset; low 48 bits RES0), so the raw ID is shifted.
///        Barriers go through system_barrier() (dmb under TCG, dsb on HW).
/// @param[in] asid Address-space ID to invalidate (0 invalidates nothing
///        specific; callers pass nonzero user IDs).
inline void tlbi_aside1(uint16_t asid) {
    system_barrier();
    asm volatile("tlbi aside1, %0"
                 :
                 : "r"(static_cast<uint64_t>(asid) << 48)
                 : "memory");
    system_barrier();
    isb();
}
/// @brief TLB invalidate all (EL1).
inline void tlbi_alle1() {
    system_barrier();
    asm volatile("tlbi vmalle1" : : : "memory");
    system_barrier();
    isb();
}
/// @brief Read TTBR0_EL1 (user page table base).
/// @return Physical address of the user page table.
inline uint64_t read_ttbr0_el1() {
    uint64_t v{};
    asm volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}
/// @brief Read TTBR1_EL1 (kernel page table base).
/// @return Physical address of the kernel page table.
inline uint64_t read_ttbr1_el1() {
    uint64_t v{};
    asm volatile("mrs %0, ttbr1_el1" : "=r"(v));
    return v;
}
/// @brief Read CR3 alias (maps to TTBR0_EL1).
/// @return Physical address of the user page table.
inline uint64_t read_cr3() {
    return read_ttbr0_el1();
}
/// @brief CR0 stub — not applicable on AArch64.
/// @return Always 0.
inline uint64_t read_cr0() {
    return 0;
}
/// @brief CR0 write stub — not applicable on AArch64.
inline void write_cr0(uint64_t) {
}
/// @brief CR4 stub — not applicable on AArch64.
/// @return Always 0.
inline uint64_t read_cr4() {
    return 0;
}
/// @brief Write TTBR0_EL1 (user page table base).
/// @param[in] v Physical address of the user page table.
inline void write_ttbr0_el1(uint64_t v) {
    asm volatile("msr ttbr0_el1, %0" : : "r"(v) : "memory");
    isb();
}
/// @brief Write TPIDR_EL0 (user thread-local-storage base, issue #74).
/// @param[in] v User VA of the thread block (0 clears). Trailing isb so the
/// write is observed before ERET to EL0. Never touches EL1 state.
inline void write_tpidr_el0(uint64_t v) {
    asm volatile("msr tpidr_el0, %0" : : "r"(v) : "memory");
    isb();
}
/// @brief Write TTBR1_EL1 (kernel page table base).
/// @param[in] v Physical address of the kernel page table.
inline void write_ttbr1_el1(uint64_t v) {
    asm volatile("msr ttbr1_el1, %0" : : "r"(v) : "memory");
}
/// @brief Write CR3 alias (switches TTBR0_EL1) and invalidate TLB.
/// Safe after higher-half migration: kernel runs via TTBR1_EL1, so
/// TTBR0_EL1 can point to any user's page table without faulting.
/// @param[in] v Physical address of the user page table.
inline void write_cr3(uint64_t v) {
    write_ttbr0_el1(v);
    // Deliberately redundant barrier pair (own barrier + tlbi_alle1's):
    // do NOT collapse — the TCG binary must stay byte-identical.
    system_barrier();
    tlbi_alle1();
    system_barrier();
    isb();
}
/// @brief Read the kernel page-table root (TTBR1_EL1, issue #352).
/// @return Physical address of the kernel page table.
/// @note Distinct from read_cr3() (TTBR0/user) — callers that need the
///       kernel half must use this, never the CR3 alias.
inline uint64_t kernel_page_table_base() {
    return read_ttbr1_el1();
}
/// @brief Read the user page-table root (TTBR0_EL1, issue #352).
/// @return Physical address of the current user page table.
inline uint64_t user_page_table_base() {
    return read_ttbr0_el1();
}
// BARRIER NOTE (issue #309b): the aarch64 TLB / address-space paths
// (tlbi_aside1, tlbi_alle1, write_cr3) take their data barrier from
// arch::system_barrier(), which emits `dmb sy` for QEMU TCG
// (CONFIG_TCG_BARRIERS == 1, the default) and `dsb sy` for silicon
// (CONFIG_TCG_BARRIERS == 0).  The previous note claimed dsb was replaced
// throughout this file — that was wrong: dsb_sy() always emits `dsb sy`
// in both build kinds.  Flipping the default to 0 requires the RPi4
// evidence tracked in issue #397.

/// @brief Enable FP/SIMD by clearing the FPEN trap in CPACR_EL1.
inline void fp_enable() {
    uint64_t cpacr{};
    asm volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3 << 20); // FPEN: trap disabled
    asm volatile("msr cpacr_el1, %0" : : "r"(cpacr) : "memory");
    asm volatile("isb");
}

/// @brief AArch64 I/O wrapper class providing static methods for port I/O stubs
/// and CPU control. All methods delegate to the namespace-level functions in
/// this file.
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
