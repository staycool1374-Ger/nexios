#include <kernel/arch/interrupt_controller.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/timer.hpp>

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
        for (int t = 1000000; t > 0; --t) {
            if (!(rr[GICR_WAKER / 4] & (1U << 1))) break;
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

void ArchInterruptController::mask(uint8_t irq) {
    if (gic_is_v3 && irq < 32) {
        gicr_sgi_reg(GICR_ICENABLER0)[0] = (1U << irq);
    } else {
        gicd_reg(GICD_ICENABLER)[irq / 32] = (1U << (irq % 32));
    }
    dsb_sy();
}

void ArchInterruptController::unmask(uint8_t irq) {
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

} // namespace arch
