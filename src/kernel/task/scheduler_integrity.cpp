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
#include <kernel/task/scheduler_switch_internal.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/debug/crash_dump.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/context_id.hpp>
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

uint64_t live_root_tagged() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    return arch::read_cr3();
#elif defined(CONFIG_ARCH_RISCV64)
    return arch::read_satp();
#elif defined(CONFIG_ARCH_AARCH64)
    return arch::read_ttbr0_el1();
#else
    return 0;
#endif
}

namespace {
// Frame base must lie strictly inside the task's kstack (V6: frame base
// uses [base, top); the top edge itself belongs to the next frame).
bool frame_in_stack(uint64_t frame_base, const TaskControlBlock *t) {
    if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC ||
        t->kernel_stack == nullptr || t->kernel_stack_top == 0) {
        return false;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(t->kernel_stack);
    return frame_base >= base && frame_base < t->kernel_stack_top;
}
} // namespace

TaskControlBlock *resolve_task_by_root(uint64_t root) noexcept {
    const auto &all = kernel::Scheduler::all_tasks();
    for (auto *t = all.first_ptr(); t != nullptr; t = all.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC || !t->is_user_ ||
            t->get_page_table() == 0) {
            continue;
        }
        if (arch::context_id_tag(t->get_page_table(), t->context_id()) ==
            root) {
            return t;
        }
    }
    return nullptr;
}

TaskControlBlock *resolve_stack_owner(uint64_t frame_base) noexcept {
    const auto &all = kernel::Scheduler::all_tasks();
    for (auto *t = all.first_ptr(); t != nullptr; t = all.next_ptr(t)) {
        if (frame_in_stack(frame_base, t)) {
            return t;
        }
    }
    // Boot-stack runner: the harness owns frames on the linker boot stack.
    if (is_boot_stack_rsp(frame_base)) {
        auto *h = kernel::Scheduler::get_harness_task();
        if (h != nullptr && h->magic == TaskControlBlock::TCB_MAGIC) {
            return h;
        }
    }
    return nullptr;
}

SchedTrapOwner validate_trap_frame(uint64_t frame_base,
                                   const char *site) noexcept {
    (void)site;
    SchedTrapOwner out{nullptr, nullptr, true, true};
    auto *cur = kernel::Scheduler::current_task();
    if (cur != nullptr && cur->magic == TaskControlBlock::TCB_MAGIC &&
        frame_in_stack(frame_base, cur)) {
        out.frame_owner = cur;
        // A U-trap attributed to a kernel current is never legitimate:
        // kernel tasks take no user faults (the M1 misattribution shape).
        if (!cur->is_user_) {
            out.trapped = resolve_task_by_root(live_root_tagged());
            out.unresolved = (out.trapped == nullptr);
            return out;
        }
        if (cur->get_page_table() != 0) {
            const uint64_t expected = arch::context_id_tag(
                cur->get_page_table(), cur->context_id());
            if (live_root_tagged() == expected) {
                out.trapped = cur;
                out.foreign = false;
                out.unresolved = false;
                return out;
            }
            // Same stack, wrong address space: the frame is cur's but the
            // hart runs foreign tables (stale satp switch).
            out.trapped = resolve_task_by_root(live_root_tagged());
            out.unresolved = (out.trapped == nullptr);
            return out;
        }
        // User task without a page table yet: stack match is the only
        // signal; root cannot be verified — fail closed.
        return out;
    }
    out.frame_owner = resolve_stack_owner(frame_base);
    out.trapped = resolve_task_by_root(live_root_tagged());
    out.unresolved = (out.trapped == nullptr);
    return out;
}

bool validate_syscall_frame(const TaskControlBlock *cur,
                            const uint64_t *regs) noexcept {
    (void)regs;
    if (cur == nullptr || cur->magic != TaskControlBlock::TCB_MAGIC) {
        return false;
    }
    const uint64_t sp = sched_switch::current_sp();
    if (sp == 0) {
        return false;
    }
    // Harness on the boot stack: kernel test runner issuing direct
    // syscalls (S-mode ecall path routes here too when delegated).
    if (cur == kernel::Scheduler::get_harness_task() &&
        is_boot_stack_rsp(sp)) {
        return true;
    }
    if (!cur->is_user_) {
        return false;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(cur->kernel_stack);
    return sp >= base && sp < cur->kernel_stack_top;
}

__attribute__((weak)) bool trap_owner_hook(uint64_t expected_id,
                                           uint64_t trapped_id,
                                           uint64_t site) noexcept {
    (void)expected_id;
    (void)trapped_id;
    (void)site;
    return false;
}

TrapOwnerDisposition trap_owner_trip(const SchedTrapOwner &o, uint64_t sepc,
                                              uint64_t scause, uint64_t stval,
                                              uint64_t frame_base,
                                              uint64_t site) noexcept {
    auto *cur = kernel::Scheduler::current_task();
    const uint64_t expected =
        (cur != nullptr && cur->magic == TaskControlBlock::TCB_MAGIC)
            ? cur->id
            : 0;
    const bool trapped_valid =
        (o.trapped != nullptr &&
         o.trapped->magic == TaskControlBlock::TCB_MAGIC);
    const uint64_t trapped_id = trapped_valid ? o.trapped->id : 0;
    const uint64_t frame_owner_id =
        (o.frame_owner != nullptr &&
         o.frame_owner->magic == TaskControlBlock::TCB_MAGIC)
            ? o.frame_owner->id
            : 0;
    const uint64_t live = live_root_tagged();
    const uint64_t slot_top = __atomic_load_n(
        &kernel::Scheduler::SwSlots::load_kstack_top(), __ATOMIC_RELAXED);
    kernel::gs::note_trap_owner_trip(expected, trapped_id, site, frame_owner_id,
                                     slot_top, live);
    // Repair helper: repoint current at the faulting task so the caller
    // kills/dispatches the right TCB. Refuses dead tasks (a second fault
    // on an already-TERMINATED task must not resurrect it — PARK).
    auto repair = [&]() -> TrapOwnerDisposition {
        if (!trapped_valid || !o.trapped->is_user_ ||
            o.trapped->state == kernel::TaskState::TERMINATED) {
            return TrapOwnerDisposition::PARK;
        }
        kernel::Scheduler::set_current_task(o.trapped);
        return TrapOwnerDisposition::PROCEED;
    };
    if (trap_owner_hook(expected, trapped_id, site)) {
        return repair();
    }
    // Benign shape: the frame sits on the resolved task's own stack AND
    // the live root is its tagged root — only the published current
    // label lagged (stale-current no-switch resume). Repairing makes the
    // state exact, so proceed in every build; halting here would wedge
    // green runs on a self-healing shape. The latch still records it.
    const bool root_matches =
        trapped_valid && o.trapped->is_user_ &&
        o.trapped->get_page_table() != 0 &&
        live == arch::context_id_tag(o.trapped->get_page_table(),
                                     o.trapped->context_id());
    if (trapped_valid && o.frame_owner == o.trapped && root_matches) {
        return repair();
    }
#if defined(CONFIG_DEBUG)
    {
        kernel::debug::FirstChanceFrame frame{};
#if defined(CONFIG_ARCH_X86_64)
        frame.arch = kernel::debug::FirstChanceArch::X86_64;
#elif defined(CONFIG_ARCH_AARCH64)
        frame.arch = kernel::debug::FirstChanceArch::AARCH64;
#else
        frame.arch = kernel::debug::FirstChanceArch::RISCV64;
#endif
        frame.cause = kernel::debug::FirstChanceCause::TRAP_OWNER;
        frame.vector = scause;
        frame.error = site;
        frame.addr = stval;
        frame.pc = sepc;
        frame.sp = frame_base;
        frame.pgtbl0 = live;
        frame.pgtbl1 = 0;
        kernel::debug::first_chance_halt(frame);
        // Production crash_halt never returns; the test seam may. Fall
        // through to the release repair below if it does.
    }
#endif
    return repair();
}

} // namespace sched_verify
} // namespace kernel
