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

/// @file scheduler_integrity.cpp
/// @brief Centralized scheduler context-switch validators (issue #323).

#include <kernel/task/scheduler_integrity.hpp>
#include <kernel/task/scheduler.hpp>
#include <assert.hpp>
#include <logger.hpp>

extern "C" {
extern char _stack_start[];
extern char _stack_end[];
}

namespace kernel {
namespace sched_verify {

bool rsp_in_stack_range(uint64_t rsp, const TaskControlBlock *t,
                        const char *label) {
    auto base = reinterpret_cast<uint64_t>(t->kernel_stack);
    auto top = t->kernel_stack_top;
    if (rsp >= base && rsp <= top)
        return true;
    if (rsp < base)
        return false;
    Logger::raw_write("[SCHED] ");
    Logger::raw_write(label);
    Logger::raw_write(": task id=");
    Logger::print_dec(t->id);
    Logger::raw_write(" rsp=0x");
    Logger::print_hex(rsp);
    Logger::raw_write(" above stack top 0x");
    Logger::print_hex(top);
    Logger::raw_write("\n");
    return false;
}

bool validate_switch(TaskControlBlock *current, TaskControlBlock *next,
                     const char *label) {
    if (!current) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current=null\n");
        return false;
    }
    if (!next) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next=null\n");
        return false;
    }
    if (current->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current magic invalid\n");
        return false;
    }
    auto caddr = reinterpret_cast<uint64_t>(current);
    auto naddr = reinterpret_cast<uint64_t>(next);
    if (caddr < CONFIG_HHDM_OFFSET) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current low 0x");
        Logger::print_hex(caddr);
        Logger::raw_write("\n");
        return false;
    }
    if (naddr < CONFIG_HHDM_OFFSET) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next low 0x");
        Logger::print_hex(naddr);
        Logger::raw_write("\n");
        return false;
    }
    if (current->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current magic=0x");
        Logger::print_hex(current->magic);
        Logger::raw_write("\n");
        return false;
    }
    if (next->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": next magic=0x");
        Logger::print_hex(next->magic);
        Logger::raw_write("\n");
        return false;
    }
    if (!Scheduler::is_idle_task(next) &&
        !rsp_in_stack_range(task_stack_ptr(next), next, label)) {
        if (task_stack_ptr(next) <
            reinterpret_cast<uint64_t>(next->kernel_stack)) {
            return true;
        }
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": current id=");
        Logger::print_dec(current->id);
        Logger::raw_write(" rsp=0x");
        Logger::print_hex(task_stack_ptr(current));
        Logger::raw_write(" state=");
        Logger::print_dec(static_cast<uint64_t>(current->state));
        Logger::raw_write(" kstack=[0x");
        Logger::print_hex(reinterpret_cast<uint64_t>(current->kernel_stack));
        Logger::raw_write("-0x");
        Logger::print_hex(current->kernel_stack_top);
        Logger::raw_write("]\n");
        return false;
    }
    return true;
}

void report_corruption(const char *label) {
    (void)label;
    __atomic_fetch_add(&scheduler_corruption_count, 1, __ATOMIC_RELEASE);
#ifdef CONFIG_DEBUG
    ENSURE(false && "scheduler corruption detected — see log above");
#endif
}

bool validate_task_ptr(const TaskControlBlock *t,
                       const char *label) noexcept {
    if (!t) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": task=null\n");
        return false;
    }
    auto addr = reinterpret_cast<uint64_t>(t);
    if (addr < CONFIG_HHDM_OFFSET) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": task low 0x");
        Logger::print_hex(addr);
        Logger::raw_write("\n");
        return false;
    }
    if (t->magic != TaskControlBlock::TCB_MAGIC) [[unlikely]] {
        Logger::raw_write("[SCHED] ");
        Logger::raw_write(label);
        Logger::raw_write(": task magic=0x");
        Logger::print_hex(t->magic);
        Logger::raw_write("\n");
        return false;
    }
    return true;
}

bool is_boot_stack_rsp(uint64_t rsp) noexcept {
    const uint64_t base = reinterpret_cast<uint64_t>(_stack_start);
    const uint64_t end = reinterpret_cast<uint64_t>(_stack_end);
    return rsp >= base && rsp < end;
}

#if defined(CONFIG_ARCH_X86_64)
bool validate_iret_frame(const TaskControlBlock &next, uint64_t nbase,
                         uint64_t f_rflags, uint64_t rip, uint64_t cs,
                         uint64_t rsp, uint64_t ss) noexcept {
    const bool ring0 = (cs == 0x8);
    const bool ring3 = (cs == 0x1B);
    if (rip == 0 || (!ring0 && !ring3))
        return false;
    if ((ring0 || ring3) && (f_rflags & 0x2) == 0)
        return false;
    if (ring3 && (ss != 0x23 || rsp == 0))
        return false;
    if (ring0) {
        const bool in_own = rsp >= nbase && rsp <= next.kernel_stack_top;
        const bool harness_boot =
            (&next == kernel::Scheduler::get_harness_task() &&
             rsp >= reinterpret_cast<uint64_t>(_stack_start) &&
             rsp < reinterpret_cast<uint64_t>(_stack_end));
        if (!in_own && !harness_boot)
            return false;
    }
    return true;
}
#endif

} // namespace sched_verify
} // namespace kernel
