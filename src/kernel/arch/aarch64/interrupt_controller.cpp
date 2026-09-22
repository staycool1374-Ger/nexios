#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/kernel.hpp>
#include <kernel/task/scheduler.hpp>
#include <signal.hpp>

namespace arch {

static bool gic_is_v3 = false;

/// @brief Initialise the GIC distributor, CPU interface, and redistributor.
/// Detects GICv2 vs GICv3 from GICD_TYPER. Enables Group 1 interrupts
/// and the timer PPI (INTID 30). Performs full distributor reset.
void ArchInterruptController::init() {
    volatile uint32_t *d = gicd_reg(0);
    volatile uint32_t *c = gicc_reg(0);

    // Detect GIC version from GICD_TYPER
    uint32_t typer = d[GICD_TYPER / 4];
    gic_is_v3 = ((typer >> 11) & 0x1F) >= 3;

    // === Distributor init ===
    // NOTE: QEMU boots the kernel at Secure EL1, so every IAR read is a
    // secure access and Group 1 interrupts are hidden unless AckCtl is set
    // (GICC_IAR then returns 1022).  Tag ALL interrupts Group 0 — the
    // kernel's own group — and do not enable Group 1 delivery.
    d[GICD_CTLR / 4] = 0;
    dsb_sy();
    d[GICD_ICENABLER / 4] = 0xFFFFFFFF;
    d[GICD_ICENABLER / 4 + 1] = 0xFFFFFFFF;
    dsb_sy();
    d[GICD_IGROUPR / 4] = 0;
    d[GICD_IGROUPR / 4 + 1] = 0;
    dsb_sy();

    if (gic_is_v3) {
        volatile uint32_t *rr = gicr_rd_reg(0);
        volatile uint32_t *rs = gicr_sgi_reg(0);

        uint32_t waker = rr[GICR_WAKER / 4];
        waker &= ~(1U << 0);
        rr[GICR_WAKER / 4] = waker;
        dsb_sy();
        for (uint32_t poll = 0; poll < GICR_WAKER_MAX_POLLS; ++poll) {
            if (rr[GICR_WAKER / 4] & GICR_WAKER_CHILDREN_ONLINE) break;
            dsb_sy();
        }

        rs[GICR_IGROUPR0 / 4] |= 0xFFFF0000U;
        dsb_sy();

        gic_v3_set_sre(true);
        gic_v3_set_pmr(0xFF);
        gic_v3_set_igrpen1(true);
        isb();

        rs[GICR_ISENABLER0 / 4] = (1U << 30);
        dsb_sy();
    } else {
        c[GICC_CTLR / 4] = 0;
        dsb_sy();
        c[GICC_PMR / 4] = 0xFF;
        dsb_sy();
        d[GICD_ISENABLER / 4] = (1U << 30);
        dsb_sy();
    }

    d[GICD_CTLR / 4] = GICD_CTLR_ENABLE;
    dsb_sy();
    if (!gic_is_v3) {
        c[GICC_CTLR / 4] = GICC_CTLR_ENABLE;
        dsb_sy();
    }
    isb();
}
void ArchInterruptController::eoi(uint8_t vector) {
    if (gic_is_v3)
        gic_v3_write_eoir(vector);
    else
        gicc_reg(GICC_EOIR)[0] = vector;
    dsb_sy();
}

/// @brief Redistributor wake state (issue #198): true when the GICv3
///        redistributor reports Children_Online, or when no redistributor
///        exists (GICv2 path). Polarity follows the GICv3 spec (online = 1);
///        the pre-#198 poll loop tested the inverted bit.
/// @return true when interrupt delivery hardware is usable.
bool gic_redist_ready() {
    if (!gic_is_v3) {
        return true;
    }
    volatile uint32_t *rr = gicr_rd_reg(0);
    return (rr[GICR_WAKER / 4] & GICR_WAKER_CHILDREN_ONLINE) != 0;
}

/// @brief Valid IRQ window for mask/unmask (issue #198): the snapshot covers
///        distributor words 0..1 and the IDT has 64 slots, so lines >= 64
///        have no defined state and must not touch MMIO.
inline constexpr uint8_t GIC_MAX_IRQ = 64;

void ArchInterruptController::mask(uint8_t irq) {
    if (irq >= GIC_MAX_IRQ) {
        return;
    }
    if (gic_is_v3 && irq < 32) {
        gicr_sgi_reg(GICR_ICENABLER0)[0] = (1U << irq);
    } else {
        gicd_reg(GICD_ICENABLER)[irq / 32] = (1U << (irq % 32));
    }
    dsb_sy();
}

void ArchInterruptController::unmask(uint8_t irq) {
    if (irq >= GIC_MAX_IRQ) {
        return;
    }
    if (gic_is_v3 && irq < 32) {
        gicr_sgi_reg(GICR_ISENABLER0)[0] = (1U << irq);
    } else {
        gicd_reg(GICD_ISENABLER)[irq / 32] = (1U << (irq % 32));
    }
    dsb_sy();
}

IrqState ArchInterruptController::snapshot() {
    IrqState s{};
    s.gic_mask = gicd_reg(GICD_ISENABLER)[0];
    s.gic_mask |= static_cast<uint64_t>(gicd_reg(GICD_ISENABLER)[1]) << 32;
    return s;
}

void ArchInterruptController::restore(const IrqState &state) {
    gicd_reg(GICD_ICENABLER)[0] = 0xFFFFFFFF;
    gicd_reg(GICD_ICENABLER)[1] = 0xFFFFFFFF;
    dsb_sy();
    gicd_reg(GICD_ISENABLER)[0] = state.gic_mask & 0xFFFFFFFF;
    gicd_reg(GICD_ISENABLER)[1] = (state.gic_mask >> 32) & 0xFFFFFFFF;
    dsb_sy();
}

/// @brief IRQ handler called from vectors.S.
/// Dispatches all non-spurious INTIDs via the IDT table.
extern "C" void handle_gic_irq(void) {
    uint64_t intid = gic_is_v3 ? gic_v3_read_iar()
                               : gicc_reg(GICC_IAR)[0];

    // >= 1020: spurious (1023) or the GICv2/GICv3 "no interrupt available
    // for the enabled groups" special values (1020-1022).  EOI and return —
    // never dispatch or fall through to the ack path below.
    if (intid >= 1020)
        goto ack;  // spurious / group-disabled

    if (intid == 30) {
        uint64_t elr{};
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        IDT::handle_interrupt(static_cast<uint64_t>(InterruptVector::TIMER), 0,
                              elr);
    } else if (intid >= 32 && intid < 64) {
        // SPI / PPI: use INTID directly as vector (IDT has 64 slots)
        uint64_t elr{};
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        IDT::handle_interrupt(intid, 0, elr);
    }
    // INTIDs 0-15 (SGIs), 16-29 (unused PPIs), 31 (PMU), ≥64 are ignored

ack:
    if (gic_is_v3)
        gic_v3_write_eoir(intid);
    else
        gicc_reg(GICC_EOIR)[0] = static_cast<uint32_t>(intid);
}

namespace {
// Post-mortem latch for EL0 faults (GDB-observable; zero-initialized
// statics need no dynamic init).
volatile uint64_t g_last_el0_esr = 0;
volatile uint64_t g_last_el0_far = 0;
volatile uint64_t g_last_el0_elr = 0;
} // namespace

/// @brief EL0 synchronous-fault handler called from vectors.S (issues
///        #184/#28).  Non-SVC faults from EL0 land here.  Latch ESR/FAR/ELR
///        for post-mortem GDB, mark the faulting task TERMINATED
///        (exit_code = -SIGSEGV), and switch off it — mirroring the sys_exit
///        self-termination path.  Runs with interrupts masked (vector
///        entry); must not re-enable IRQs, and must NOT call
///        Scheduler::terminate (ISR-unsafe: switch_to_task resolves the
///        live exception RSP as the save owner — see
///        syscall_handlers_misc.cpp).  The asm caller applies the pending
///        switch via irq_context_switch_common; if no task is current (no
///        user context to kill) park as a last resort.
///        A parent BLOCKED in waitpid IS woken (issue #217:
///        wake_waiting_parent delivers status + PID before the switch);
///        polled waiters (wait_for_termination_safe) observe TERMINATED.
extern "C" void aarch64_el0_fault_handler() {
    uint64_t esr = 0;
    uint64_t far = 0;
    uint64_t elr = 0;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far));
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    g_last_el0_esr = esr;
    g_last_el0_far = far;
    g_last_el0_elr = elr;
    kernel::TaskControlBlock *t = kernel::Scheduler::current_task();
    if (t != nullptr && kernel::TaskControlBlock::is_valid(t) &&
        t->state != kernel::TaskState::TERMINATED) {
        t->state = kernel::TaskState::TERMINATED;
        t->exit_code = static_cast<uint64_t>(
            -static_cast<int64_t>(kernel::Signal::SIGSEGV));
        // Issue #217: wake a parent blocked in waitpid BEFORE switching
        // away (wake needs parent_id, which it clears) so the woken
        // parent can be selected as the switch successor.
        kernel::Scheduler::wake_waiting_parent(*t);
        kernel::Scheduler::switch_away_from_terminating(*t);
        return;
    }
    for (;;) {
        arch::pause();
    }
}

/// @brief Post-mortem readback of the last latched EL0 fault ESR (tests).
extern "C" uint64_t aarch64_last_el0_esr() { return g_last_el0_esr; }

/// @brief EL1 unexpected-sync-fault handler called from vectors.S (issue
///        #214).  Any EL1 sync fault is a kernel bug — the old skip-and-eret
///        behavior masked them (issue #209).  Dumps ESR/FAR/ELR over the
///        lock-free UART path (bounded poll, byte-drop; no Logger format
///        strings — %lx hangs) and panics in debug builds.  In release
///        builds returns and the asm stub resumes with the legacy skip.
///        Runs with interrupts masked (vector entry); must not re-enable
///        IRQs, touch the scheduler, or (in debug) return.
extern "C" void aarch64_el1_unexpected_fault(uint64_t esr, uint64_t far,
                                             uint64_t elr) {
    arch::Serial::puts("\nEL1 sync fault: ESR=0x");
    for (int i = 60; i >= 0; i -= 4)
        arch::Serial::putchar("0123456789ABCDEF"[(esr >> i) & 0xF]);
    arch::Serial::puts(" FAR=0x");
    for (int i = 60; i >= 0; i -= 4)
        arch::Serial::putchar("0123456789ABCDEF"[(far >> i) & 0xF]);
    arch::Serial::puts(" ELR=0x");
    for (int i = 60; i >= 0; i -= 4)
        arch::Serial::putchar("0123456789ABCDEF"[(elr >> i) & 0xF]);
    arch::Serial::puts("\n");
#ifdef CONFIG_DEBUG
    panic("EL1 sync fault");
#else
    (void)esr;
    (void)far;
    (void)elr;
#endif
}

} // namespace arch
