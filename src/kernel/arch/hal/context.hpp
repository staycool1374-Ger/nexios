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

/// @file context.hpp
/// @brief Architecture-specific CPU context (register save area) and context
/// switching.

#pragma once

#include <types.hpp>
#include <constants.hpp>

#include <kernel/nexios_config.h>

namespace arch {

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief x86-64 CPU context — general-purpose registers, interrupt vector,
///        error code, and interrupt-frame (rip, cs, rflags, rsp, ss).
struct ArchContext {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

/// @brief Manages x86-64 CPU context save/restore and stack initialisation.
class ArchContextManager {
  public:
    /// @brief Initialise a stack frame for a new task (x86-64).
    /// @param[in,out] stack_top Top of the stack (grows downward).
    /// @param entry Entry point function.
    /// @param cs Code segment selector.
    /// @param ss Stack segment selector.
    /// @param rflags Initial RFLAGS value.
    /// @param user_rsp Initial user-mode RSP.
    static inline void init_stack(uint64_t *stack_top, void (*entry)(),
                                  uint64_t cs, uint64_t ss, uint64_t rflags,
                                  uint64_t user_rsp) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        *--stack_top = ss;
        *--stack_top = user_rsp;
        *--stack_top = rflags;
        *--stack_top = cs;
        *--stack_top = reinterpret_cast<uint64_t>(entry);
        *--stack_top = 0;
        *--stack_top = 0;
        for (int i = 0; i < 15; ++i)
            *--stack_top = 0;
#pragma GCC diagnostic pop
    }

    /// @brief Save the current stack pointer into a context block.
    /// @param[out] ctx Destination context.
    /// @param rsp Current stack pointer value.
    static inline void save(ArchContext &ctx, uint64_t rsp) {
        ctx.rsp = rsp;
    }

    /// @brief Restore a stack pointer from a context block.
    /// @param ctx Source context.
    /// @param[out] rsp Output stack pointer.
    static inline void restore(const ArchContext &ctx, uint64_t &rsp) {
        rsp = ctx.rsp;
    }

    /// @brief Switch from one context to another (x86-64).
    /// @param[out] from Current context to save into.
    /// @param to Target context to restore.
    /// @param[out] rsp Output stack pointer set to target's RSP.
    static inline void switch_to(ArchContext &from, const ArchContext &to,
                                 uint64_t &rsp) {
        from.rsp = rsp;
        rsp = to.rsp;
    }
};

/// @cond
#elif defined(CONFIG_ARCH_AARCH64)
/// @endcond

/// @brief AArch64 CPU context — general-purpose registers x0–x30, SP_EL0,
///        ELR_EL1, SPSR_EL1, interrupt vector, and error code.
struct ArchContext {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29, x30;
    uint64_t sp_el0, elr_el1, spsr_el1;
    uint64_t vector, error_code;
};

/// @brief Manages AArch64 CPU context save/restore and stack initialisation.
class ArchContextManager {
  public:
    /// @brief Initialise a stack frame for a new task (AArch64).
    /// @param[in,out] stack_top Top of the stack (grows downward).
    /// @param entry Entry point function.
    /// @param psr Saved Processor State Register (SPSR).
    /// @param user_rsp Initial user-mode SP_EL0.
    static inline void init_stack(uint64_t *&stack_top, void (*entry)(),
                                  uint64_t /*cs*/, uint64_t /*ss*/,
                                  uint64_t psr, uint64_t user_rsp) {
        *--stack_top = 0;
        *--stack_top = 0;
        *--stack_top = psr;
        *--stack_top = reinterpret_cast<uint64_t>(entry);
        *--stack_top = user_rsp;
        for (int i = 0; i < 31; ++i)
            *--stack_top = 0;
    }

    /// @brief Save the current stack pointer into a context block.
    /// @param[out] ctx Destination context.
    /// @param rsp Current stack pointer value.
    static inline void save(ArchContext &ctx, uint64_t rsp) {
        ctx.sp_el0 = rsp;
    }

    /// @brief Restore a stack pointer from a context block.
    /// @param ctx Source context.
    /// @param[out] rsp Output stack pointer.
    static inline void restore(const ArchContext &ctx, uint64_t &rsp) {
        rsp = ctx.sp_el0;
    }

    /// @brief Switch from one context to another (AArch64).
    /// @param[out] from Current context to save into.
    /// @param to Target context to restore.
    /// @param[out] rsp Output stack pointer set to target's SP_EL0.
    static inline void switch_to(ArchContext &from, const ArchContext &to,
                                 uint64_t &rsp) {
        from.sp_el0 = rsp;
        rsp = to.sp_el0;
    }
};

/// @cond
#elif defined(CONFIG_ARCH_RISCV64)
/// @endcond

/// @brief RISC-V 64 CPU context — general-purpose registers, S-mode CSRs,
///        interrupt vector, and error code.
struct ArchContext {
    uint64_t ra, sp, gp, tp;
    uint64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t sepc, sstatus, stvec;
    uint64_t vector, error_code;
};

/// @brief Manages RISC-V 64 CPU context save/restore and stack initialisation.
class ArchContextManager {
  public:
    /// @brief Initialise a stack frame for a new task (RISC-V 64).
    /// @param[in,out] stack_top Top of the stack (grows downward).
    /// @param entry Entry point function.
    /// @param psr Initial SSTATUS value.
    /// @param user_rsp Initial user-mode SP.
    static inline void init_stack(uint64_t *stack_top, void (*entry)(),
                                  uint64_t /*cs*/, uint64_t /*ss*/,
                                  uint64_t psr, uint64_t user_rsp) {
        *--stack_top = 0;
        *--stack_top =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry));
        *--stack_top = user_rsp;
        *--stack_top = psr;
        for (int i = 0; i < 16; ++i)
            *--stack_top = 0;
    }

    /// @brief Save the current stack pointer into a context block.
    /// @param[out] ctx Destination context.
    /// @param rsp Current stack pointer value.
    static inline void save(ArchContext &ctx, uint64_t rsp) {
        ctx.sp = rsp;
    }

    /// @brief Restore a stack pointer from a context block.
    /// @param ctx Source context.
    /// @param[out] rsp Output stack pointer.
    static inline void restore(const ArchContext &ctx, uint64_t &rsp) {
        rsp = ctx.sp;
    }

    /// @brief Switch from one context to another (RISC-V 64).
    /// @param[out] from Current context to save into.
    /// @param to Target context to restore.
    /// @param[out] rsp Output stack pointer set to target's SP.
    static inline void switch_to(ArchContext &from, const ArchContext &to,
                                 uint64_t &rsp) {
        from.sp = rsp;
        rsp = to.sp;
    }
};

/// @cond
#else
#error "HAL: no context implementation for this architecture"
#endif
/// @endcond

} // namespace arch
