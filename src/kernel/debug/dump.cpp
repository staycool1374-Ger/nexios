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

/// @file dump.cpp
/// @brief Diagnostic dump implementation — scheduler, CPU registers, task
/// state.

#include <kernel/debug/dump.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/io.hpp>

namespace kernel {
namespace debug {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

using L = Logger;
using S = Scheduler;

/// @brief Return a string representation of a TaskState.
static const char *state_str(TaskState s) {
    switch (s) {
    case TaskState::READY:
        return "READY";
    case TaskState::RUNNING:
        return "RUNNING";
    case TaskState::BLOCKED:
        return "BLOCKED";
    case TaskState::WAITING:
        return "WAITING";
    case TaskState::TERMINATED:
        return "TERMINATED";
    default:
        return "?";
    }
}

/// @brief Print a key-value pair in hex.
static void pkv(const char *key, uint64_t v) {
    L::raw_write("[DUMP]   ");
    L::raw_write(key);
    L::raw_write(": ");
    L::print_hex(v);
    L::raw_write("\n");
}

/// @brief Print a key-value pair in decimal.
static void pkd(const char *key, uint64_t v) {
    L::raw_write("[DUMP]   ");
    L::raw_write(key);
    L::raw_write(": ");
    L::print_dec(v);
    L::raw_write("\n");
}

/// @brief Print two hex key-value pairs (arch-neutral: consumed by all
///        three dump_cpu_regs() arms).
static void pkv2(const char *k1, uint64_t v1, const char *k2, uint64_t v2) {
    L::raw_write("[DUMP]   ");
    L::raw_write(k1);
    L::raw_write(": ");
    L::print_hex(v1);
    L::raw_write("  ");
    L::raw_write(k2);
    L::raw_write(": ");
    L::print_hex(v2);
    L::raw_write("\n");
}

/// @brief Print two decimal key-value pairs.
static void pkd2(const char *k1, uint64_t v1, const char *k2, uint64_t v2) {
    L::raw_write("[DUMP]   ");
    L::raw_write(k1);
    L::raw_write(": ");
    L::print_dec(v1);
    L::raw_write("  ");
    L::raw_write(k2);
    L::raw_write(": ");
    L::print_dec(v2);
    L::raw_write("\n");
}

/// @brief Print three hex key-value pairs (arch-neutral: consumed by all
///        three dump_cpu_regs() arms).
static void pkv3(const char *k1, uint64_t v1, const char *k2, uint64_t v2,
                 const char *k3, uint64_t v3) {
    L::raw_write("[DUMP]   ");
    L::raw_write(k1);
    L::raw_write(": ");
    L::print_hex(v1);
    L::raw_write("  ");
    L::raw_write(k2);
    L::raw_write(": ");
    L::print_hex(v2);
    L::raw_write("  ");
    L::raw_write(k3);
    L::raw_write(": ");
    L::print_hex(v3);
    L::raw_write("\n");
}

/// @brief Dump scheduler indices, counts, flags, and deferred-switch globals.
void dump_scheduler_info() {
    arch::IrqGuard irq_guard{};

    L::raw_write("[DUMP] === scheduler ===\n");

    auto cidx = S::current_index();
    auto cnt = S::task_count();
    auto *cur = S::current_task();
    auto *idle = S::get_idle_task();
    auto *shell = S::get_shell_task();

    pkd("current_index", cidx);
    pkd("task_count", cnt);
    pkd("next_task_id", Scheduler::SwSlots::next_task_id());

    L::raw_write("[DUMP]   current_task_id: ");
    if (cur && cur->magic == TaskControlBlock::TCB_MAGIC) {
        L::print_dec(cur->id);
        L::raw_write("  state: ");
        L::raw_write(state_str(cur->state));
    } else {
        L::raw_write("(none)");
    }
    L::raw_write("\n");

    L::raw_write("[DUMP]   idle_task_id: ");
    if (idle) {
        L::print_dec(idle->id);
    } else {
        L::raw_write("(none)");
    }
    L::raw_write("  shell_task_id: ");
    if (shell) {
        L::print_dec(shell->id);
    } else {
        L::raw_write("(none)");
    }
    L::raw_write("\n");

    L::raw_write("[DUMP]   reschedule: ");
    L::raw_write(S::needs_switch() ? "yes" : "no");
    L::raw_write("  preempt: ");
    L::raw_write(S::is_preemptible() ? "yes" : "no");
    L::raw_write("\n");

    // sporadic count – public accessor added alongside this file
    pkd("sporadic_count", S::sporadic_count());

    // deferred-switch globals set by switch_to_task
    pkv("save_rsp_to", reinterpret_cast<uint64_t>(Scheduler::SwSlots::save_rsp_to()));
    pkv("load_rsp_from", Scheduler::SwSlots::load_rsp_from());
    pkv("load_cr3_from", Scheduler::SwSlots::load_cr3_from());
    pkd("next_switch_id", Scheduler::SwSlots::next_task_id());

    // ISR / corruption tracking (own CPU's nesting slot, issue #25 C1)
    pkd("isr_nesting",
        __atomic_load_n(&isr_nesting_own(), __ATOMIC_ACQUIRE));
    pkd("corruption_count", scheduler_corruption_count);

    L::raw_write("[DUMP] === end scheduler ===\n");
}

/// @brief Dump all CPU GPRs + CR0/CR2/CR3/CR4 (x86_64); stub on other archs.
#if defined(CONFIG_ARCH_X86_64)

void dump_cpu_regs() {
    arch::IrqGuard irq_guard{};

    // read GPRs via inline asm (split into 3 blocks due to register pressure)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    asm volatile("mov %%rax, %0\n"
                 "mov %%rbx, %1\n"
                 "mov %%rcx, %2\n"
                 "mov %%rdx, %3\n"
                 "mov %%rsi, %4\n"
                 "mov %%rdi, %5\n"
                 "mov %%rbp, %6\n"
                 : "=r"(rax), "=r"(rbx), "=r"(rcx), "=r"(rdx), "=r"(rsi),
                   "=r"(rdi), "=r"(rbp));

    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    asm volatile("mov %%r8,  %0\n"
                 "mov %%r9,  %1\n"
                 "mov %%r10, %2\n"
                 "mov %%r11, %3\n"
                 "mov %%r12, %4\n"
                 "mov %%r13, %5\n"
                 "mov %%r14, %6\n"
                 "mov %%r15, %7\n"
                 : "=r"(r8), "=r"(r9), "=r"(r10), "=r"(r11), "=r"(r12),
                   "=r"(r13), "=r"(r14), "=r"(r15));

    uint64_t rsp, rflags;
    asm volatile("mov %%rsp, %0\n"
                 "pushfq\n"
                 "pop %1\n"
                 : "=r"(rsp), "=r"(rflags)::"memory");

    uint64_t rip{};
    asm volatile("call 1f; 1: pop %0" : "=r"(rip));

    uint64_t cr0 = arch::read_cr0();
    uint64_t cr2 = arch::read_cr2();
    uint64_t cr3 = arch::read_cr3();
    uint64_t cr4 = arch::read_cr4();

    L::raw_write("[DUMP] === cpu regs ===\n");
    pkv3("rax", rax, "rbx", rbx, "rcx", rcx);
    pkv3("rdx", rdx, "rsi", rsi, "rdi", rdi);
    pkv3("rbp", rbp, "rsp", rsp, "rip", rip);
    pkv3("r8", r8, "r9", r9, "r10", r10);
    pkv3("r11", r11, "r12", r12, "r13", r13);
    pkv2("r14", r14, "r15", r15);
    pkv2("rflags", rflags, "cr0", cr0);
    pkv2("cr2", cr2, "cr3", cr3);
    pkv2("cr4", cr4, "", 0);
    L::raw_write("[DUMP] === end cpu regs ===\n");
}

#elif defined(CONFIG_ARCH_AARCH64)

void dump_cpu_regs() {
    arch::IrqGuard irq_guard{};

    // Sysregs first: a nested trap would overwrite ELR/SPSR/FAR, so the
    // guard above precedes every mrs read.
    uint64_t elr_el1{};
    asm volatile("mrs %0, elr_el1" : "=r"(elr_el1));
    uint64_t spsr_el1{};
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr_el1));
    uint64_t far_el1{};
    asm volatile("mrs %0, far_el1" : "=r"(far_el1));
    uint64_t ttbr0 = arch::read_ttbr0_el1();
    uint64_t ttbr1 = arch::read_ttbr1_el1();

    // Single-asm capture: all GPRs are stacked before any compiler-chosen
    // scratch write (a multi-block mov split could corrupt unread regs).
    // Slots 0..30 = x0..x30, slot 31 = entry SP. Only caller-saved scratch
    // (x10/x11/x13/x14/x15) is written; SP is restored by the loop.
    uint64_t gpr_values[32]{};
    asm volatile(
        "stp x30, xzr, [sp, #-16]!\n"
        "stp x28, x29, [sp, #-16]!\n"
        "stp x26, x27, [sp, #-16]!\n"
        "stp x24, x25, [sp, #-16]!\n"
        "stp x22, x23, [sp, #-16]!\n"
        "stp x20, x21, [sp, #-16]!\n"
        "stp x18, x19, [sp, #-16]!\n"
        "stp x16, x17, [sp, #-16]!\n"
        "stp x14, x15, [sp, #-16]!\n"
        "stp x12, x13, [sp, #-16]!\n"
        "stp x10, x11, [sp, #-16]!\n"
        "stp x8,  x9,  [sp, #-16]!\n"
        "stp x6,  x7,  [sp, #-16]!\n"
        "stp x4,  x5,  [sp, #-16]!\n"
        "stp x2,  x3,  [sp, #-16]!\n"
        "stp x0,  x1,  [sp, #-16]!\n"
        "mov x13, %0\n"
        "add x14, sp, #256\n"
        "mov x15, #16\n"
        "1:\n"
        "    ldp x10, x11, [sp], #16\n"
        "    stp x10, x11, [x13], #16\n"
        "    subs x15, x15, #1\n"
        "    b.ne 1b\n"
        "sub x13, x13, #256\n"
        "str x14, [x13, #248]\n"
        :
        : "r"(gpr_values)
        : "x10", "x11", "x13", "x14", "x15", "memory");

    L::raw_write("[DUMP] === cpu regs ===\n");
    pkv3("x0", gpr_values[0], "x1", gpr_values[1], "x2", gpr_values[2]);
    pkv3("x3", gpr_values[3], "x4", gpr_values[4], "x5", gpr_values[5]);
    pkv3("x6", gpr_values[6], "x7", gpr_values[7], "x8", gpr_values[8]);
    pkv3("x9", gpr_values[9], "x10", gpr_values[10], "x11", gpr_values[11]);
    pkv3("x12", gpr_values[12], "x13", gpr_values[13], "x14", gpr_values[14]);
    pkv3("x15", gpr_values[15], "x16", gpr_values[16], "x17", gpr_values[17]);
    pkv3("x18", gpr_values[18], "x19", gpr_values[19], "x20", gpr_values[20]);
    pkv3("x21", gpr_values[21], "x22", gpr_values[22], "x23", gpr_values[23]);
    pkv3("x24", gpr_values[24], "x25", gpr_values[25], "x26", gpr_values[26]);
    pkv3("x27", gpr_values[27], "x28", gpr_values[28], "x29", gpr_values[29]);
    pkv2("x30", gpr_values[30], "sp", gpr_values[31]);
    pkv3("elr", elr_el1, "spsr", spsr_el1, "far", far_el1);
    pkv2("ttbr0", ttbr0, "ttbr1", ttbr1);
    L::raw_write("[DUMP] === end cpu regs ===\n");
}

#elif defined(CONFIG_ARCH_RISCV64)

void dump_cpu_regs() {
    arch::IrqGuard irq_guard{};

    // Sysregs first: a nested trap would overwrite sepc/stval, so the
    // guard above precedes every csrr read.
    uint64_t sepc{};
    asm volatile("csrr %0, sepc" : "=r"(sepc));
    uint64_t stval{};
    asm volatile("csrr %0, stval" : "=r"(stval));
    uint64_t satp = arch::read_satp();

    // Single-asm capture: all GPRs are stored before any compiler-chosen
    // scratch write. Slots 0..30 = x1..x31, slot 1 (x2) is fixed up to the
    // entry SP. Only caller-saved scratch (t0-t2) is written; SP is
    // restored by the loop. x0 is hardwired zero and omitted.
    uint64_t gpr_values[32]{};
    asm volatile(
        "addi sp, sp, -256\n"
        "sd x1,   0(sp)\n"
        "sd x2,   8(sp)\n"
        "sd x3,  16(sp)\n"
        "sd x4,  24(sp)\n"
        "sd x5,  32(sp)\n"
        "sd x6,  40(sp)\n"
        "sd x7,  48(sp)\n"
        "sd x8,  56(sp)\n"
        "sd x9,  64(sp)\n"
        "sd x10, 72(sp)\n"
        "sd x11, 80(sp)\n"
        "sd x12, 88(sp)\n"
        "sd x13, 96(sp)\n"
        "sd x14,104(sp)\n"
        "sd x15,112(sp)\n"
        "sd x16,120(sp)\n"
        "sd x17,128(sp)\n"
        "sd x18,136(sp)\n"
        "sd x19,144(sp)\n"
        "sd x20,152(sp)\n"
        "sd x21,160(sp)\n"
        "sd x22,168(sp)\n"
        "sd x23,176(sp)\n"
        "sd x24,184(sp)\n"
        "sd x25,192(sp)\n"
        "sd x26,200(sp)\n"
        "sd x27,208(sp)\n"
        "sd x28,216(sp)\n"
        "sd x29,224(sp)\n"
        "sd x30,232(sp)\n"
        "sd x31,240(sp)\n"
        "addi t0, sp, 256\n"
        "sd t0, 8(sp)\n"
        "mv t2, %0\n"
        "li t0, 0\n"
        "1:\n"
        "    ld t1, 0(sp)\n"
        "    sd t1, 0(t2)\n"
        "    addi sp, sp, 8\n"
        "    addi t2, t2, 8\n"
        "    addi t0, t0, 8\n"
        "    li t1, 256\n"
        "    blt t0, t1, 1b\n"
        "addi t2, t2, -256\n"
        "sd sp, 8(t2)\n"
        :
        : "r"(gpr_values)
        : "t0", "t1", "t2", "memory");

    L::raw_write("[DUMP] === cpu regs ===\n");
    pkv3("x1", gpr_values[0], "x2", gpr_values[1], "x3", gpr_values[2]);
    pkv3("x4", gpr_values[3], "x5", gpr_values[4], "x6", gpr_values[5]);
    pkv3("x7", gpr_values[6], "x8", gpr_values[7], "x9", gpr_values[8]);
    pkv3("x10", gpr_values[9], "x11", gpr_values[10], "x12", gpr_values[11]);
    pkv3("x13", gpr_values[12], "x14", gpr_values[13], "x15", gpr_values[14]);
    pkv3("x16", gpr_values[15], "x17", gpr_values[16], "x18", gpr_values[17]);
    pkv3("x19", gpr_values[18], "x20", gpr_values[19], "x21", gpr_values[20]);
    pkv3("x22", gpr_values[21], "x23", gpr_values[22], "x24", gpr_values[23]);
    pkv3("x25", gpr_values[24], "x26", gpr_values[25], "x27", gpr_values[26]);
    pkv3("x28", gpr_values[27], "x29", gpr_values[28], "x30", gpr_values[29]);
    // sp duplicates x2 by contract (issue #306); x0 omitted (hardwired zero).
    pkv2("x31", gpr_values[30], "sp", gpr_values[1]);
    pkv3("sepc", sepc, "stval", stval, "satp", satp);
    L::raw_write("[DUMP] === end cpu regs ===\n");
}

#endif

/// @brief Dump all fields of a single task.
/// @param task_id  Task ID to dump.
void dump_task_info(uint64_t task_id) {
    arch::IrqGuard irq_guard{};

    // capture live deferred-switch globals for side-channel info
    uint64_t save_to = reinterpret_cast<uint64_t>(Scheduler::SwSlots::save_rsp_to());
    uint64_t next_id = Scheduler::SwSlots::next_task_id();

    auto *t = S::find_task(task_id);
    if (!t) {
        L::raw_write("[DUMP] --- task id=");
        L::print_dec(task_id);
        L::raw_write(" ---\n");
        L::raw_write("[DUMP]   NOT FOUND\n");
        L::raw_write("[DUMP] --- end task ---\n");
        return;
    }

    bool valid = (t->magic == TaskControlBlock::TCB_MAGIC);

    L::raw_write("[DUMP] --- task id=");
    L::print_dec(t->id);
    L::raw_write(" ---\n");

    pkv("magic", t->magic);
    if (valid) {
        L::raw_write("[DUMP]   magic: VALID\n");
    } else {
        L::raw_write("[DUMP]   magic: INVALID (probably freed)\n");
    }

    if (!valid) {
        L::raw_write("[DUMP] --- end task id=");
        L::print_dec(t->id);
        L::raw_write(" ---\n");
        return;
    }

    L::raw_write("[DUMP]   state: ");
    L::raw_write(state_str(t->state));
    L::raw_write("\n");

    pkd2("priority", t->priority, "base_priority", t->base_priority);
    pkd2("parent_id", t->parent_id, "children", t->num_children);
    pkd2("period_ticks", t->period_ticks, "deadline_ticks", t->deadline_ticks);
    pkd2("executed_ticks", t->executed_ticks, "remaining_ticks",
         t->remaining_ticks);

    pkv("kernel_stack", reinterpret_cast<uint64_t>(t->kernel_stack));
    pkv("kernel_stack_top", t->kernel_stack_top);

    // context registers (the ones that matter for the crash)
#if defined(CONFIG_ARCH_X86_64)
    pkv("context.rsp", t->context.rsp);
    pkv("context.rip", t->context.rip);
    pkv2("context.cs", t->context.cs, "context.ss", t->context.ss);
    pkv("context.rflags", t->context.rflags);
#elif defined(CONFIG_ARCH_AARCH64)
    pkv("context.sp_el0", t->context.sp_el0);
    pkv("context.elr_el1", t->context.elr_el1);
    pkv("context.spsr_el1", t->context.spsr_el1);
#endif

    // check whether the saved context stack pointer is within its own kernel
    // stack (arch-specific field name)
    uint64_t base = reinterpret_cast<uint64_t>(t->kernel_stack);
    uint64_t top = t->kernel_stack_top;
#if defined(CONFIG_ARCH_X86_64)
    uint64_t saved_sp = t->context.rsp;
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t saved_sp = t->context.sp_el0;
#else
    uint64_t saved_sp = t->context.sp;
#endif
    bool rsp_in_stack = (base > 0 && top > base && saved_sp >= base &&
                         saved_sp <= top);

    L::raw_write("[DUMP]   rsp_in_stack: ");
    L::raw_write(rsp_in_stack ? "yes" : "NO (dangerous!)");
    L::raw_write("\n");

    // memory / page tables
    pkv("page_table", t->get_page_table());

    // ready-queue status
    L::raw_write("[DUMP]   in_ready_queue: ");
    L::raw_write(t->in_ready_queue_ ? "yes" : "no");
    L::raw_write("  rq_priority: ");
    L::print_dec(t->rq_priority_);
    L::raw_write("\n");

    // signals / alarm
    pkv("pending_signals", t->pending_signals);
    L::raw_write("[DUMP]   alarm: ");
    L::print_dec(t->alarm_ticks);
    L::raw_write(" ticks, ");
    L::raw_write(t->alarm_armed ? "armed" : "not armed");
    L::raw_write("\n");

    // IPC blocked status
    L::raw_write("[DUMP]   blocked_on_queue: ");
    L::raw_write(t->blocked_on_queue ? "yes" : "none");
    L::raw_write("\n");

    // sporadic server
    L::raw_write("[DUMP]   sporadic_server: ");
    L::raw_write(t->sporadic_server ? "active" : "none");
    L::raw_write("\n");

    pkv("exit_code", t->exit_code);

    // show whether deferred-switch globals point to this task
    bool is_switch_target = false;
#if defined(CONFIG_ARCH_X86_64)
    is_switch_target = is_switch_target ||
                       (save_to == reinterpret_cast<uint64_t>(&t->context.rsp));
#elif defined(CONFIG_ARCH_AARCH64)
    is_switch_target = is_switch_target ||
                       (save_to ==
                        reinterpret_cast<uint64_t>(&t->context.sp_el0));
#else
    is_switch_target = is_switch_target ||
                       (save_to == reinterpret_cast<uint64_t>(&t->context.sp));
#endif
    is_switch_target = is_switch_target || (next_id != 0 && next_id == t->id);
    L::raw_write("[DUMP]   deferred_switch_target: ");
    L::raw_write(is_switch_target ? "yes" : "no");
    L::raw_write("\n");

#ifdef CONFIG_DEBUG
    // dump the debug switch ring buffer
    L::raw_write("[DUMP]   debug_switch_ring:\n");
    for (size_t i = 0; i < TaskControlBlock::DEBUG_SWITCH_RING_SIZE; ++i) {
        auto &rec = t->debug_switch_ring[i];
        L::raw_write("[DUMP]     [");
        L::print_dec(i);
        L::raw_write("] entry=0x");
        L::print_hex(rec.entry_addr);
        L::raw_write(" exit_rip=0x");
        L::print_hex(rec.exit_rip);
        L::raw_write("\n");
        L::raw_write("[DUMP]          id=");
        L::print_dec(rec.thread_id);
        L::raw_write(" ticks=");
        L::print_dec(rec.consumed_ticks);
#if defined(CONFIG_ARCH_X86_64)
        L::raw_write(" ctx.rip=0x");
        L::print_hex(rec.regs.rip);
        L::raw_write(" ctx.rsp=0x");
        L::print_hex(rec.regs.rsp);
#elif defined(CONFIG_ARCH_AARCH64)
        L::raw_write(" ctx.elr=0x");
        L::print_hex(rec.regs.elr_el1);
        L::raw_write(" ctx.sp=0x");
        L::print_hex(rec.regs.sp_el0);
#endif
        L::raw_write("\n");
    }
#endif

    L::raw_write("[DUMP] --- end task id=");
    L::print_dec(t->id);
    L::raw_write(" ---\n");
}

/// @brief Dump all valid tasks in the scheduler array.
void dump_all_tasks() {
    arch::IrqGuard irq_guard{};

    uint64_t count = S::task_count();
    L::raw_write("[DUMP] === all tasks (count=");
    L::print_dec(count);
    L::raw_write(") ===\n");

    S::TaskIter iter{};
    uint64_t dumped = 0;
    while (auto *t = iter.next()) {
        // print a separator between tasks
        if (dumped > 0) {
            L::raw_write("[DUMP]\n");
        }
        // dump inline using the same format as dump_task_info
        uint64_t save_to = reinterpret_cast<uint64_t>(Scheduler::SwSlots::save_rsp_to());
        uint64_t next_id = Scheduler::SwSlots::next_task_id();

        bool valid = (t->magic == TaskControlBlock::TCB_MAGIC);

        L::raw_write("[DUMP] --- task id=");
        L::print_dec(t->id);
        L::raw_write(" idx=");
        L::print_dec(iter.idx - 1); // TaskIter already advanced past this entry
        L::raw_write(" ---\n");

        if (!valid) {
            pkv("magic", t->magic);
            L::raw_write("[DUMP]   INVALID TCB (freed)\n");
            L::raw_write("[DUMP] --- end task ---\n");
            ++dumped;
            continue;
        }

        L::raw_write("[DUMP]   state: ");
        L::raw_write(state_str(t->state));
        L::raw_write("  priority: ");
        L::print_dec(t->priority);
        L::raw_write("  base: ");
        L::print_dec(t->base_priority);
        L::raw_write("\n");

        pkd2("parent", t->parent_id, "children", t->num_children);
        pkd2("exec", t->executed_ticks, "rem", t->remaining_ticks);

#if defined(CONFIG_ARCH_X86_64)
        pkv("ctx.rsp", t->context.rsp);
        pkv("ctx.rip", t->context.rip);
#elif defined(CONFIG_ARCH_AARCH64)
        pkv("ctx.sp", t->context.sp_el0);
        pkv("ctx.elr", t->context.elr_el1);
#else
        pkv("ctx.sp", t->context.sp);
        pkv("ctx.epc", t->context.sepc);
#endif

        uint64_t base = reinterpret_cast<uint64_t>(t->kernel_stack);
        uint64_t top = t->kernel_stack_top;
#if defined(CONFIG_ARCH_X86_64)
        uint64_t saved_sp = t->context.rsp;
#elif defined(CONFIG_ARCH_AARCH64)
        uint64_t saved_sp = t->context.sp_el0;
#else
        uint64_t saved_sp = t->context.sp;
#endif
        bool rsp_in_stack = (base > 0 && top > base && saved_sp >= base &&
                             saved_sp <= top);

        L::raw_write("[DUMP]   kstack=[0x");
        L::print_hex(base);
        L::raw_write("-0x");
        L::print_hex(top);
        L::raw_write("]  rsp_in_stack: ");
        L::raw_write(rsp_in_stack ? "yes" : "NO");
        L::raw_write("\n");

        pkv("pgtable", t->get_page_table());

        L::raw_write("[DUMP]   rq: ");
        L::raw_write(t->in_ready_queue_ ? "yes" : "no");
        L::raw_write("  sig=0x");
        L::print_hex(t->pending_signals);
        L::raw_write("  alarm=");
        L::print_dec(t->alarm_ticks);
        L::raw_write("  blk=");
        L::raw_write(t->blocked_on_queue ? "yes" : "no");
        L::raw_write("  spor=");
        L::raw_write(t->sporadic_server ? "yes" : "no");
        L::raw_write("\n");

        // deferred-switch target detection
        bool is_target = false;
#if defined(CONFIG_ARCH_X86_64)
        is_target = (save_to == reinterpret_cast<uint64_t>(&t->context.rsp)) ||
                    (next_id != 0 && next_id == t->id);
#elif defined(CONFIG_ARCH_AARCH64)
        is_target = (save_to ==
                     reinterpret_cast<uint64_t>(&t->context.sp_el0)) ||
                    (next_id != 0 && next_id == t->id);
#else
        is_target = (save_to ==
                     reinterpret_cast<uint64_t>(&t->context.sp)) ||
                    (next_id != 0 && next_id == t->id);
#endif
        L::raw_write("[DUMP]   deferred: ");
        L::raw_write(is_target ? "TARGET" : "no");
        L::raw_write("\n");

        L::raw_write("[DUMP] --- end task id=");
        L::print_dec(t->id);
        L::raw_write(" ---\n");
        ++dumped;
    }

    L::raw_write("[DUMP] === end all tasks (");
    L::print_dec(dumped);
    L::raw_write(" dumped) ===\n");
}

} // namespace debug
} // namespace kernel
