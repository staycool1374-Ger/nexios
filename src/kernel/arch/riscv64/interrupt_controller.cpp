#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/riscv64/hal/plic.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <signal.hpp>

namespace arch {

void ArchInterruptController::init() {
    auto *threshold = reinterpret_cast<volatile uint32_t *>(PLIC_THRESHOLD);
    *threshold = 0;
    // Default priority 1 for the QEMU virt wired lines (priority 0 = never
    // interrupt). Lines stay disabled here — drivers enable their own IRQ
    // when their IDT handler is registered (no unsolicited delivery).
    *plic_priority_reg(IRQ_UART) = 1;
    *plic_priority_reg(IRQ_KEYBOARD) = 1;
    *plic_priority_reg(IRQ_VIRTIO0) = 1;
    *plic_priority_reg(IRQ_VIRTIO1) = 1;
    *plic_priority_reg(IRQ_VIRTIO2) = 1;
    *plic_priority_reg(IRQ_VIRTIO3) = 1;
    *plic_priority_reg(IRQ_VIRTIO4) = 1;
    asm volatile("fence iorw, iorw" : : : "memory");
    asm volatile("csrs sie, %0" : : "r"((uint64_t)(1ULL << 9)) : "memory");
}

void ArchInterruptController::eoi(uint8_t vector) {
    plic_complete(vector);
}

/// @brief Valid IRQ window for mask/unmask (issue #198): QEMU virt exposes
///        a single 32-line enable word; lines >= 32 have no defined register
///        and must not touch MMIO.
inline constexpr uint8_t PLIC_MAX_IRQ = 32;

void ArchInterruptController::mask(uint8_t irq) {
    if (irq >= PLIC_MAX_IRQ) {
        return;
    }
    // Single-core early precondition (INV/CONC): no lock needed; the fence
    // orders the MMIO write before any subsequent claim/complete.
    auto *enable = reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE);
    enable[irq / 32] &= ~(1U << (irq % 32));
    asm volatile("fence iorw, iorw" : : : "memory");
}

void ArchInterruptController::unmask(uint8_t irq) {
    if (irq >= PLIC_MAX_IRQ) {
        return;
    }
    auto *enable = reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE);
    enable[irq / 32] |= (1U << (irq % 32));
    asm volatile("fence iorw, iorw" : : : "memory");
}

IrqState ArchInterruptController::snapshot() {
    IrqState s{};
    auto *threshold = reinterpret_cast<volatile uint32_t *>(PLIC_THRESHOLD);
    s.plic_threshold = *threshold;
    auto *enable = reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE);
    s.plic_enable_first = enable[0];
    return s;
}

void ArchInterruptController::restore(const IrqState &state) {
    auto *threshold = reinterpret_cast<volatile uint32_t *>(PLIC_THRESHOLD);
    *threshold = state.plic_threshold;
    auto *enable = reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE);
    enable[0] = state.plic_enable_first;
    asm volatile("fence iorw, iorw" : : : "memory");
}

extern "C" void handle_plic_trap(uint64_t scause, uint64_t sepc,
                                 uint64_t *regs) {
    (void)regs;
    if (scause & (1ULL << 63)) {
        uint64_t code = scause & ~(1ULL << 63);
        if (code == 5) {
            IDT::handle_interrupt(
                static_cast<uint64_t>(InterruptVector::TIMER), 0, sepc);
        } else if (code == 9) {
            uint32_t intid = plic_claim();
            if (intid != 0) {
                IDT::handle_interrupt(intid, 0, sepc);
                plic_complete(intid);
            }
        }
    }
}

extern "C" void handle_kernel_exception(uint64_t sepc, uint64_t scause,
                                        uint64_t stval) {
    const char msg[] = {'[', 'E', 'X', 'C', ']', ' ', 's',
                        'c', 'a', 'u', 's', 'e', '=', 0};
    for (const char *p = msg; *p; ++p) {
        uint64_t ch = (unsigned char)*p;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    uint64_t v = scause;
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t nibble = (v >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        uint64_t ch = (unsigned char)c;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    const char msg2[] = {' ', 's', 'e', 'p', 'c', '=', 0};
    for (const char *p = msg2; *p; ++p) {
        uint64_t ch = (unsigned char)*p;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    v = sepc;
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t nibble = (v >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        uint64_t ch = (unsigned char)c;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    const char msg3[] = {' ', 's', 't', 'v', 'a', 'l', '=', 0};
    for (const char *p = msg3; *p; ++p) {
        uint64_t ch = (unsigned char)*p;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    v = stval;
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t nibble = (v >> i) & 0xF;
        char c = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        uint64_t ch = (unsigned char)c;
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    {
        uint64_t ch = (unsigned char)'\n';
        asm volatile("mv a0, %0; li a7, 1; ecall"
                     :
                     : "r"(ch)
                     : "a0", "a7", "memory");
    }
    panic("riscv64: unhandled exception");
}

/// @brief Latched last U-mode fault CSRs for test readback.
static uint64_t g_last_u_scause = 0;
static uint64_t g_last_u_stval = 0;
static uint64_t g_last_u_sepc = 0;

/// @brief Terminate a U-mode faulting task (issue #206 M2, mirrors
///        aarch64_el0_fault_handler). Runs IRQ-masked from the trap entry;
///        must not re-enable IRQs and must NOT call Scheduler::terminate
///        (ISR-unsafe) — marks TERMINATED, wakes a waitpid parent, and arms
///        the deferred switch via switch_away_from_terminating. The asm
///        return path applies the pending switch. Parks only when there is
///        no live user task to kill.
/// @param frame Trap save area (OFF_* layout: SEPC=idx31, SCAUSE=idx33,
///        STVAL=idx34).
extern "C" void riscv64_u_fault_handler(uint64_t *frame) {
    uint64_t sepc = frame[31];
    uint64_t scause = frame[33];
    uint64_t stval = frame[34];
    g_last_u_scause = scause;
    g_last_u_stval = stval;
    g_last_u_sepc = sepc;
    kernel::TaskControlBlock *t = kernel::Scheduler::current_task();
    if (t != nullptr && kernel::TaskControlBlock::is_valid(t) &&
        t->state != kernel::TaskState::TERMINATED) {
        // Issue #226: debugger stop routing (spec §4). scause 3
        // (breakpoint: ebreak/c.ebreak, incl. emulated-step temps) reports
        // a breakpoint at sepc (the router consumes temps into STEP);
        // other causes report a fault with stval. Attached targets park;
        // unattached keep the terminate path below.
        if (__atomic_load_n(&t->debugger_id, __ATOMIC_ACQUIRE) != 0) {
            uint64_t stop_kind = static_cast<uint64_t>(
                kernel::debug::StopKind::FAULT);
            // stval carries the fault address only for address faults
            // (misaligned/page faults); illegal instructions leave it
            // zero — report the trap pc then (mirrors x86 rip/aarch64
            // ELR for non-address faults).
            uint64_t stop_addr = (stval != 0) ? stval : sepc;
            if (scause == 3) {
                stop_kind = static_cast<uint64_t>(
                    kernel::debug::StopKind::BREAKPOINT);
                stop_addr = sepc;
            }
            if (kernel::debug::debug_route_fault(*t, stop_kind, scause,
                                                 stop_addr)) {
                return;
            }
        }
        t->state = kernel::TaskState::TERMINATED;
        t->exit_code = static_cast<uint64_t>(
            -static_cast<int64_t>(kernel::Signal::SIGSEGV));
        kernel::Scheduler::wake_waiting_parent(*t);
        kernel::Scheduler::switch_away_from_terminating(*t);
        return;
    }
    for (;;) {
        arch::pause();
    }
}

/// @brief Post-mortem readback of the last latched U-mode fault (tests).
extern "C" uint64_t riscv64_last_u_scause() { return g_last_u_scause; }
extern "C" uint64_t riscv64_last_u_stval() { return g_last_u_stval; }
extern "C" uint64_t riscv64_last_u_sepc() { return g_last_u_sepc; }

/// @brief SPP-gated exception dispatch (issue #206 M2). U-mode (SPP==0)
///        faults terminate the task; S-mode faults keep the SBI-dump +
///        panic path (kernel bug).
/// @param frame Trap save area base (sp).
/// @param sstatus Live sstatus (SPP bit 8 discriminates).
extern "C" void riscv64_exception_dispatch(uint64_t *frame, uint64_t sstatus) {
    // Fault recovery for safe_copy_from/to_user (mirrors x86
    // kernel.cpp:1669): while a guarded copy runs, ANY fault redirects
    // SEPC to the recovery label so the copy returns false instead of
    // panicking (S-mode) or terminating (U-mode) the task. Checked first:
    // copies execute in S-mode task context.
    if (kernel::g_user_access_recover_ip != 0) {
        frame[31] = kernel::g_user_access_recover_ip; // OFF_SEPC slot
        kernel::g_user_access_recover_ip = 0;
        return;
    }
    if ((sstatus & (1ULL << 8)) == 0) {
        riscv64_u_fault_handler(frame);
        return;
    }
    handle_kernel_exception(frame[31], frame[33], frame[34]);
}

} // namespace arch
