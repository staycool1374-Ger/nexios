#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/irq_delivery.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/kernel.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <signal.hpp>

#include <kernel/arch/hal/irq_storm.hpp>
#include <kernel/log/dmesg.hpp>

namespace arch {

static bool gic_is_v3 = false;
/// @brief BOOT_ONLY latch: init() programs hardware exactly once.
static bool g_gic_initialized = false;
/// @brief Last acknowledged INTID (EOI ownership, issue #349). Sentinel
/// ~0ULL = none yet: no valid INTID ever equals it, so a pre-first-IRQ
/// eoi() mismatches like any stale call instead of closing SGI 0.
static uint64_t g_last_acked = ~0ULL;
/// @brief EOI calls for a non-acked vector (diagnostic).
static uint64_t g_unmatched_eoi = 0;
/// @brief Spurious INTIDs observed (>= 1020, diagnostic).
static uint64_t g_spurious_count = 0;
/// @brief Per-tick same-INTID storm limiter (never fed the timer).
static IrqStormLimiter g_storm{};

/// @brief Initialise the GIC distributor, CPU interface, and redistributor.
/// Full GICv2 bring-up: version from GICD_PIDR2 (TYPER holds no version),
/// distributor reset, priority/target/edge programming, timer PPI enable.
/// BOOT_ONLY: repeat calls return without touching live state.
void ArchInterruptController::init() {
    if (g_gic_initialized) {
        return;
    }
    volatile uint32_t *d = gicd_reg(0);
    volatile uint32_t *c = gicc_reg(0);

    // Version from PIDR2, not TYPER (TYPER[15:11] is not a version field).
    gic_is_v3 = gic_version() >= 3;
    // Clamp the programmed window: ITLinesNumber+1 banks, max 256 INTIDs.
    uint32_t it_lines = (d[GICD_TYPER / 4] & 0x1FU) + 1;
    uint32_t window = it_lines * 32;
    if (window > GIC_MAX_INTID) {
        window = GIC_MAX_INTID;
    }

    // === Distributor init ===
    // NOTE: QEMU boots the kernel at Secure EL1, so every IAR read is a
    // secure access and Group 1 interrupts are hidden unless AckCtl is set
    // (GICC_IAR then returns 1022).  Tag ALL interrupts Group 0 — the
    // kernel's own group — and do not enable Group 1 delivery.
    d[GICD_CTLR / 4] = 0;
    dsb_sy();
    for (uint32_t w = 0; w < 8; ++w) {
        d[GICD_ICENABLER / 4 + w] = 0xFFFFFFFF;
    }
    dsb_sy();
    for (uint32_t w = 0; w < 8; ++w) {
        d[GICD_IGROUPR / 4 + w] = 0;
    }
    dsb_sy();
    // Priority: 0xA0 for the whole window (all lower than PMR 0xFF).
    for (uint32_t i = 0; i < window; ++i) {
        volatile uint8_t *p =
            reinterpret_cast<volatile uint8_t *>(d) + GICD_IPRIORITY + i;
        *p = 0xA0;
    }
    dsb_sy();
    // Targets: SPI routing defaults to no CPU; pin SPIs to CPU0.
    for (uint32_t i = 32; i < window; ++i) {
        volatile uint8_t *t =
            reinterpret_cast<volatile uint8_t *>(d) + GICD_ITARGETSR + i;
        *t = 0x01;
    }
    dsb_sy();
    // Edge config: clear all 2-bit fields (level), then set edge for the
    // virtio-mmio SPIs only (QEMU virt DT flags them edge-high).
    for (uint32_t i = 0; i < window; i += 16) {
        d[GICD_ICFGR / 4 + i / 16] = 0;
    }
    dsb_sy();
    for (uint32_t i = INTID_VIRTIO_BASE;
         i < INTID_VIRTIO_BASE + INTID_VIRTIO_COUNT && i < window; ++i) {
        volatile uint32_t *w =
            &d[GICD_ICFGR / 4 + (i / 16)];
        *w |= static_cast<uint32_t>(0x2U << ((i % 16) * 2));
    }
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
        d[GICD_CTLR / 4] = GICD_CTLR_ENABLE_GRP1;
        dsb_sy();
        if ((d[GICD_CTLR / 4] & GICD_CTLR_ENABLE_GRP1) == 0) {
            return;
        }
    } else {
        c[GICC_CTLR / 4] = 0;
        dsb_sy();
        c[GICC_PMR / 4] = 0xFF;
        dsb_sy();
        d[GICD_ISENABLER / 4] = (1U << 30);
        dsb_sy();
        d[GICD_CTLR / 4] = GICD_CTLR_ENABLE;
        dsb_sy();
        if ((d[GICD_CTLR / 4] & GICD_CTLR_ENABLE) == 0) {
            return;
        }
        c[GICC_CTLR / 4] = GICC_CTLR_ENABLE;
        dsb_sy();
        if ((c[GICC_CTLR / 4] & GICC_CTLR_ENABLE) == 0) {
            return;
        }
    }
    isb();
    g_gic_initialized = true;
}
void ArchInterruptController::eoi(uint8_t vector) {
    // EOI ownership: only the last acknowledged INTID may be closed.
    if (static_cast<uint64_t>(vector) != g_last_acked) {
        ++g_unmatched_eoi;
        return;
    }
    if (gic_is_v3)
        gic_v3_write_eoir(vector);
    else
        gicc_reg(GICC_EOIR)[0] = vector;
    dsb_sy();
}

/// @brief Redistributor wake state (issue #198): true when the GICv3
///        redistributor reports Children_Online, or when the GICv2 path
///        read-back proves the distributor and CPU interface are enabled.
/// @return true when interrupt delivery hardware is usable.
bool gic_redist_ready() {
    if (!gic_is_v3) {
        volatile uint32_t *d = gicd_reg(0);
        volatile uint32_t *c = gicc_reg(0);
        return ((d[GICD_CTLR / 4] & GICD_CTLR_ENABLE) != 0) &&
               ((c[GICC_CTLR / 4] & GICC_CTLR_ENABLE) != 0);
    }
    volatile uint32_t *rr = gicr_rd_reg(0);
    volatile uint32_t *d = gicd_reg(0);
    return ((rr[GICR_WAKER / 4] & GICR_WAKER_CHILDREN_ONLINE) != 0) &&
           ((d[GICD_CTLR / 4] & GICD_CTLR_ENABLE_GRP1) != 0);
}

void ArchInterruptController::mask(uint8_t irq) {
    // uint8_t is always < GIC_MAX_INTID (256) by construction.
    if (gic_is_v3 && irq < 32) {
        gicr_sgi_reg(GICR_ICENABLER0)[0] = (1U << irq);
    } else {
        gicd_reg(GICD_ICENABLER)[irq / 32] = (1U << (irq % 32));
    }
    dsb_sy();
}

void ArchInterruptController::unmask(uint8_t irq) {
    // uint8_t is always < GIC_MAX_INTID (256) by construction.
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
    for (uint32_t w = 2; w < 8; ++w) {
        s.gic_enable_hi[w - 2] = gicd_reg(GICD_ISENABLER)[w];
    }
    return s;
}

uint16_t ArchInterruptController::max_vector() {
    // Same clamp as init(): ITLinesNumber+1 banks, max 256 INTIDs.
    volatile uint32_t *d = gicd_reg(0);
    uint32_t it_lines = (d[GICD_TYPER / 4] & 0x1FU) + 1;
    uint32_t window = it_lines * 32;
    if (window > GIC_MAX_INTID) {
        window = GIC_MAX_INTID;
    }
    return static_cast<uint16_t>(window - 1);
}

bool ArchInterruptController::line_enabled(uint8_t irq) {
    // uint8_t is always < GIC_MAX_INTID (256) by construction.
    return (gicd_reg(GICD_ISENABLER)[irq / 32] & (1U << (irq % 32))) != 0;
}

void ArchInterruptController::restore(const IrqState &state) {
    for (uint32_t w = 0; w < 8; ++w) {
        gicd_reg(GICD_ICENABLER)[w] = 0xFFFFFFFF;
    }
    dsb_sy();
    gicd_reg(GICD_ISENABLER)[0] = state.gic_mask & 0xFFFFFFFF;
    gicd_reg(GICD_ISENABLER)[1] = (state.gic_mask >> 32) & 0xFFFFFFFF;
    for (uint32_t w = 2; w < 8; ++w) {
        gicd_reg(GICD_ISENABLER)[w] = state.gic_enable_hi[w - 2];
    }
    dsb_sy();
}

/// @brief IRQ handler called from vectors.S.
/// Dispatches acknowledged INTIDs via the IDT table. Spurious values
/// (>= 1020) are never EOI'd and never dispatched; the timer INTID is
/// never fed to the storm limiter.
extern "C" void handle_gic_irq(void) {
    uint64_t intid = gic_is_v3 ? gic_v3_read_iar()
                               : gicc_reg(GICC_IAR)[0];

    // Spurious / "no interrupt for the enabled groups" (1020-1023):
    // count and return without EOI (no active state to close).
    if (intid >= 1020) {
        ++g_spurious_count;
        return;
    }

    if (intid == 30) {
        uint64_t elr{};
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        IDT::handle_interrupt(static_cast<uint64_t>(InterruptVector::TIMER), 0,
                              elr);
    } else if (intid < GIC_MAX_INTID) {
        // Storm guard: mask persistently re-asserting lines (not the tick).
        if (g_storm.observe(intid, arch::Timer::ticks())) {
            ArchInterruptController::mask(static_cast<uint8_t>(intid));
            g_storm.note_masked();
            kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::BASE,
                                        kernel::log::kDmesgBase_BASE + 64,
                                        kernel::log::LogSeverity::ERROR,
                                        "irq storm masked", intid);
        } else {
            uint64_t elr{};
            asm volatile("mrs %0, elr_el1" : "=r"(elr));
            // User-space delivery (issue #354): a consumed vector skips the
            // generic handler; EOI stays single-choked at the tail below.
            if (!kernel::IrqDelivery::isr_entry(static_cast<uint8_t>(intid))) {
                IDT::handle_interrupt(intid, 0, elr);
            }
        }
    }
    // INTIDs >= 256 cannot occur (uint8_t dispatch window); ignore.

    g_last_acked = intid;
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
        // Issue #226: debugger stop routing (spec §4). EC 0x3C (BRK
        // executed) reports a breakpoint at ELR; EC 0x22 (Software Step)
        // reports step completion; other ECs report a fault with FAR.
        // Attached targets park; unattached keep the terminate path below.
        if (__atomic_load_n(&t->debugger_id, __ATOMIC_ACQUIRE) != 0) {
            uint64_t ec = (esr >> 26) & 0x3FULL;
            uint64_t stop_kind = static_cast<uint64_t>(
                kernel::debug::StopKind::FAULT);
            uint64_t stop_addr = far;
            if (ec == 0x3C) {
                stop_kind = static_cast<uint64_t>(
                    kernel::debug::StopKind::BREAKPOINT);
                stop_addr = elr;
            } else if (ec == 0x22) {
                stop_kind = static_cast<uint64_t>(
                    kernel::debug::StopKind::STEP);
                stop_addr = elr;
            }
            if (kernel::debug::debug_route_fault(*t, stop_kind, ec,
                                                 stop_addr)) {
                return;
            }
        }
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
///        #214).  Fault recovery for safe_copy_from/to_user is checked
///        FIRST (issue #235; mirrors riscv64_exception_dispatch and x86
///        kernel.cpp:1664): while a guarded copy runs, ANY EL1 fault
///        redirects ELR_EL1 to the recovery label so the copy returns
///        false instead of panicking.  Returns 0 when recovery was
///        applied (asm stub restores + erets WITHOUT the legacy skip),
///        1 otherwise.  Any other EL1 sync fault is a kernel bug — the
///        old skip-and-eret behavior masked them (issue #209).  Dumps
///        ESR/FAR/ELR over the lock-free UART path (bounded poll,
///        byte-drop; no Logger format strings — %lx hangs) and panics
///        in debug builds.  In release builds returns 1 and the asm stub
///        resumes with the legacy skip.  Runs with interrupts masked
///        (vector entry); must not re-enable IRQs or touch the
///        scheduler.  Recovery resume is safe: the guarded copy's
///        recovery label needs no caller-saved state (same contract the
///        x86/riscv recovery paths rely on).
extern "C" uint64_t aarch64_el1_unexpected_fault(uint64_t esr, uint64_t far,
                                                 uint64_t elr) {
    uint64_t recover = kernel::g_user_access_recover_ip;
    if (recover != 0) {
        kernel::g_user_access_recover_ip = 0;
        asm volatile("msr elr_el1, %0" ::"r"(recover));
        return 0;
    }
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
    return 1;
}

} // namespace arch
