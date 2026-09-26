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

/// @file debug_regs.cpp
/// @brief Debugger register codecs (issue #225, spec docs/specs/debugd.md
///        §3). Maps the per-arch user-trap frame at the fixed kstack-top
///        slot to/from the GDB register blob layouts pinned by
///        tools/debugd (164/272/264 bytes).
///
/// Slot rule (all arches): a user-mode trap lands its frame at
/// (kstack_top - U_FRAME_SIZE) — x86 RSP0, aarch64 SP_EL1, riscv64 the
/// scheduler_load_kstack_top slot. S-mode ticks use current-sp-relative
/// frames below it and never clobber the slot, so the last U-trap frame
/// of a non-running task is stable and readable. Writes land live: the
/// task resumes FROM that frame (sret/iret/eret restore from it).
/// Every read validates the frame's user-mode magic first and fails
/// closed (EAGAIN) on mismatch — never decodes garbage.

#include <kernel/debug/debug_regs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/page_table.hpp>

namespace kernel::debug {

#if defined(CONFIG_ARCH_X86_64)
// isr_stubs.asm isr_common: CPU IRET frame (ss/rsp/rflags/cs/rip) +
// stub err/vec + 15 GPRs (r15..rax) = 22 qwords = 176 bytes. Layout
// from frame base: [0]=rax [1]=rbx [2]=rcx(=rip for syscall frames)
// [3]=rdx [4]=rsi [5]=rdi [6]=rbp [7]=r8 [8]=r9 [9]=r10 [10]=r11
// (=rflags for syscall frames) [11]=r12 [12]=r13 [13]=r14 [14]=r15
// [15]=vec-or-cs-magic [16]=err [17]=rip [18]=cs [19]=rflags
// [20]=user rsp [21]=ss. Syscall frames (syscall_entry.asm, 136 bytes)
// carry the same GPRs but NO user rsp (it lives transiently in gs:0x00):
// their [15] is the 0xFFFFFFFF80000000 magic and [2]/[10] are the
// syscall rip/rflags. Blob rsp slot reads 0 for syscall frames
// (documented GDB degradation, not an error).
constexpr size_t kX86TickFrameQwords = 22;
constexpr size_t kX86SyscallFrameQwords = 17;
constexpr uint64_t kX86SyscallCsMagic = 0xFFFFFFFF80000000ULL;
constexpr size_t kX86BlobQwords = 23; // 164B GDB x86_64 minimal + pad
#elif defined(CONFIG_ARCH_AARCH64)
// vectors.S save_all/restore_all: x0-x30 (0-240), sp_el0+elr (248-255),
// spsr (264); frame is 288 bytes. EL0 sync AND EL0 IRQ share the layout
// (both funnel through save_all), so no kind split is needed.
constexpr size_t kAarch64FrameQwords = 36;
constexpr size_t kAarch64BlobQwords = 34; // 272B: x0-x30, sp, pc, cpsr
#elif defined(CONFIG_ARCH_RISCV64)
// syscall_entry.S: 37-qword frame (OFF_* layout, SAVE_SIZE 296). One
// layout for every trap class (ecall, fault, interrupt) — no kind
// split. OFF_A0=72 (idx 9) .. OFF_SEPC=248 (idx 31).
constexpr size_t kRiscvFrameQwords = 37;
constexpr size_t kRiscvBlobQwords = 33; // 264B: x0-x31 initially zero, pc
#endif

size_t debug_blob_bytes() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    return 164;
#elif defined(CONFIG_ARCH_AARCH64)
    return kAarch64BlobQwords * 8;
#elif defined(CONFIG_ARCH_RISCV64)
    return kRiscvBlobQwords * 8;
#else
    return 0;
#endif
}

uint64_t *debug_frame_slot(const TaskControlBlock &tcb) noexcept {
    if (tcb.kernel_stack_top == 0)
        return nullptr;
#if defined(CONFIG_ARCH_X86_64)
    constexpr size_t kBytes = kX86TickFrameQwords * 8;
#elif defined(CONFIG_ARCH_AARCH64)
    constexpr size_t kBytes = kAarch64FrameQwords * 8;
#elif defined(CONFIG_ARCH_RISCV64)
    constexpr size_t kBytes = kRiscvFrameQwords * 8;
#else
    constexpr size_t kBytes = 0;
#endif
    if (kBytes == 0 || tcb.kernel_stack_top < kBytes)
        return nullptr;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return reinterpret_cast<uint64_t *>(tcb.kernel_stack_top - kBytes);
}

bool debug_frame_is_user(const uint64_t *frame) noexcept {
    if (frame == nullptr)
        return false;
#if defined(CONFIG_ARCH_X86_64)
    // Tick/IRQ frame: CS.RPL == 3. Syscall frame: cs-magic at [15].
    if (frame[15] == kX86SyscallCsMagic)
        return true;
    return (frame[18] & 3ULL) == 3ULL;
#elif defined(CONFIG_ARCH_AARCH64)
    // SPSR M[3:0] == 0 is EL0t (user).
    return (frame[33] & 0xFULL) == 0;
#elif defined(CONFIG_ARCH_RISCV64)
    // sstatus SPP (bit 8) == 0 trapped from U-mode. OFF_SSTAT=256.
    return (frame[32] & 0x100ULL) == 0;
#else
    (void)frame;
    return false;
#endif
}

bool debug_read_regs(const TaskControlBlock &tcb, uint64_t *blob_out,
                     size_t blob_qwords) noexcept {
    uint64_t *frame = debug_frame_slot(tcb);
    if (frame == nullptr || !debug_frame_is_user(frame))
        return false;
#if defined(CONFIG_ARCH_X86_64)
    if (blob_qwords < kX86BlobQwords)
        return false;
    const bool syscall_frame = (frame[15] == kX86SyscallCsMagic);
    // GDB order: rax rbx rcx rdx rsi rdi rbp rsp r8-r15 rip eflags
    // cs ss ds es fs gs (+1 pad qword: kernel-only, always 0).
    blob_out[0] = frame[0];    // rax
    blob_out[1] = frame[1];    // rbx
    blob_out[2] = frame[2];    // rcx (= rip for syscall frames)
    blob_out[3] = frame[3];    // rdx
    blob_out[4] = frame[4];    // rsi
    blob_out[5] = frame[5];    // rdi
    blob_out[6] = frame[6];    // rbp
    blob_out[8] = frame[7];    // r8
    blob_out[9] = frame[8];    // r9
    blob_out[10] = frame[9];   // r10
    blob_out[11] = frame[10];  // r11 (= rflags for syscall frames)
    blob_out[12] = frame[11];  // r12
    blob_out[13] = frame[12];  // r13
    blob_out[14] = frame[13];  // r14
    blob_out[15] = frame[14];  // r15
    if (syscall_frame) {
        blob_out[7] = 0; // user rsp not saved by syscall_entry (see above)
        blob_out[16] = frame[2];  // rip (from rcx)
        blob_out[17] = frame[10]; // rflags (from r11)
        blob_out[18] = 0x23;      // user cs (ring 3 data selector stand-in)
        blob_out[19] = 0x2B;      // user ss stand-in
    } else {
        blob_out[7] = frame[20];  // user rsp
        blob_out[16] = frame[17]; // rip
        blob_out[17] = frame[19]; // rflags
        blob_out[18] = frame[18]; // cs
        blob_out[19] = frame[21]; // ss
    }
    blob_out[20] = 0; // ds (64-bit user: fixed 0)
    blob_out[21] = 0; // es
    blob_out[22] = 0; // fs
    return true;
#elif defined(CONFIG_ARCH_AARCH64)
    if (blob_qwords < kAarch64BlobQwords)
        return false;
    // Frame IS the blob layout (x0-x30, sp_el0, elr, spsr).
    for (size_t i = 0; i < kAarch64BlobQwords; ++i)
        blob_out[i] = frame[i];
    return true;
#elif defined(CONFIG_ARCH_RISCV64)
    if (blob_qwords < kRiscvBlobQwords)
        return false;
    // Blob: x0-x31 then pc. Frame OFF_* (idx = OFF/8): RA=0 SP=1 GP=2
    // TP=3 T0=4 T1=5 T2=6 S0=7 S1=8 A0=9 A1=10 A2=11 A3=12 A4=13 A5=14
    // A6=15 A7=16 S2=17 S3=18 S4=19 S5=20 S6=21 S7=22 S8=23 S9=24
    // S10=25 S11=26 T3=27 T4=28 T5=29 T6=30, pc=SEPC idx 31.
    blob_out[0] = 0; // x0 is always zero
    // Frame idx i holds x(i+1) (RA=x1 at 0 ... T6=x31 at 30): shift by one.
    for (size_t i = 1; i < 32; ++i)
        blob_out[i] = frame[i - 1];
    blob_out[32] = frame[31]; // pc (OFF_SEPC=248)
    return true;
#else
    (void)tcb;
    (void)blob_out;
    (void)blob_qwords;
    return false;
#endif
}

bool debug_write_regs(TaskControlBlock &tcb, const uint64_t *blob_in,
                      size_t blob_qwords) noexcept {
    uint64_t *frame = debug_frame_slot(tcb);
    if (frame == nullptr || !debug_frame_is_user(frame))
        return false;
#if defined(CONFIG_ARCH_X86_64)
    if (blob_qwords < kX86BlobQwords)
        return false;
    const bool syscall_frame = (frame[15] == kX86SyscallCsMagic);
    frame[0] = blob_in[0];
    frame[1] = blob_in[1];
    frame[2] = blob_in[2];
    frame[3] = blob_in[3];
    frame[4] = blob_in[4];
    frame[5] = blob_in[5];
    frame[6] = blob_in[6];
    frame[7] = blob_in[8];
    frame[8] = blob_in[9];
    frame[9] = blob_in[10];
    frame[10] = blob_in[11];
    frame[11] = blob_in[12];
    frame[12] = blob_in[13];
    frame[13] = blob_in[14];
    frame[14] = blob_in[15];
    if (syscall_frame) {
        frame[2] = blob_in[16];  // rip (via rcx)
        frame[10] = blob_in[17]; // rflags (via r11)
        // rsp/ss/cs NOT writable on syscall frames (not saved); the
        // resume path (sysret) reuses the live values. Documented.
    } else {
        frame[17] = blob_in[16]; // rip
        frame[19] = blob_in[17]; // rflags
        frame[20] = blob_in[7];  // user rsp
        // cs/ss/ds/es/fs/gs: execution mode is not debugger-mutable.
    }
    return true;
#elif defined(CONFIG_ARCH_AARCH64)
    if (blob_qwords < kAarch64BlobQwords)
        return false;
    for (size_t i = 0; i < 31; ++i)
        frame[i] = blob_in[i]; // x0-x30 (x0 write ignored by hardware)
    frame[31] = blob_in[31];   // sp_el0
    frame[32] = blob_in[32];   // elr (pc redirect)
    // spsr (frame[33]): mode bits not debugger-mutable; preserved.
    return true;
#elif defined(CONFIG_ARCH_RISCV64)
    if (blob_qwords < kRiscvBlobQwords)
        return false;
    for (size_t i = 0; i < 31; ++i)
        frame[i] = blob_in[i + 1]; // RA..T6 (x0 is never stored)
    frame[31] = blob_in[32];   // sepc (pc redirect)
    // sstatus (frame[32]): SPP/SPIE/SIE not debugger-mutable; preserved.
    return true;
#else
    (void)tcb;
    (void)blob_in;
    (void)blob_qwords;
    return false;
#endif
}

bool debug_step_arm(TaskControlBlock &tcb) noexcept {
    uint64_t *frame = debug_frame_slot(tcb);
    if (frame == nullptr || !debug_frame_is_user(frame))
        return false;
#if defined(CONFIG_ARCH_X86_64)
    // RFLAGS TF (bit 8) + IF masked (bit 9 clear) for exactly one user
    // instruction (issue #226): a timer tick preempting between resume
    // and the stepped insn would otherwise take a #DB inside the tick
    // handler (TF is global CPU state) and panic. IF returns at disarm;
    // the tick fires immediately after (pending) — no time distortion.
    // Syscall frames carry rflags in the r11 slot (frame[10]); tick
    // frames in the IRET slot (frame[19]).
    if (frame[15] == kX86SyscallCsMagic) {
        frame[10] |= (1ULL << 8);
        frame[10] &= ~(1ULL << 9);
    } else {
        frame[19] |= (1ULL << 8);
        frame[19] &= ~(1ULL << 9);
    }
    return true;
#else
    // aarch64, RISC-V and unknown arches: no hardware step used — the
    // caller emulates via a temp breakpoint (debug_step in debug_stop.cpp;
    // RISC-V additionally masks SIE). Rationale (issue #226): AArch64
    // instructions are fixed 4 bytes so next-insn emulation is exact, and
    // it avoids any dependence on the QEMU/silicon software-step debug
    // model (MDSCR_EL1.SS); the SPSR.SS/MDSCR path is reserved for future
    // use. Temp breakpoints persist across preemption (unlike one-shot
    // TF/SS state), so no interrupt masking is needed on aarch64.
    (void)tcb;
    return false;
#endif
}

void debug_step_disarm(TaskControlBlock &tcb) noexcept {
    uint64_t *frame = debug_frame_slot(tcb);
    if (frame == nullptr || !debug_frame_is_user(frame))
        return; // target gone: nothing to clear, completion proceeds
#if defined(CONFIG_ARCH_X86_64)
    if (frame[15] == kX86SyscallCsMagic) {
        frame[10] &= ~(1ULL << 8);
        frame[10] |= (1ULL << 9);
    } else {
        frame[19] &= ~(1ULL << 8);
        frame[19] |= (1ULL << 9);
    }
#else
    (void)tcb;
#endif
}

} // namespace kernel::debug
