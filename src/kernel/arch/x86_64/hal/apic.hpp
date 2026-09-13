#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/msr.hpp>

namespace arch {

/// @brief x86_64 APIC driver — Local APIC (xAPIC + x2APIC), I/O APIC, timer.
class APIC {
public:
    /// @brief Initialise the local APIC (x2APIC MSR or xAPIC MMIO).
    /// @return true if APIC was successfully enabled.
    static bool init();

    /// @brief Send End-Of-Interrupt (no-op if APIC not initialised).
    static void eoi();

    /// @brief Whether the APIC was successfully initialised.
    static bool is_enabled() { return enabled_; }

    /// @brief Whether the APIC timer (TSC-deadline or periodic) is active.
    static bool is_timer_active() { return timer_active_; }

    /// @brief Initialise the APIC timer.
    /// @param frequency_hz Desired tick frequency in Hz.
    static void timer_init(uint32_t frequency_hz);

    /// @brief Start the APIC timer (call after timer_init).
    static void timer_start();

    /// @brief Stop the APIC timer.
    static void timer_stop();

    /// @brief Program the APIC timer to fire once after `ns` nanoseconds.
    /// Uses TSC-deadline MSR when available; falls back to one-shot bus-clock
    /// mode.  The interrupt is delivered at APIC_TIMER_VECTOR.
    /// @param ns  Delay in nanoseconds (0 = no-op).
    static void set_timer_oneshot(uint64_t ns);

    /// @brief Program the APIC timer to fire every `ns` nanoseconds.
    /// In TSC-deadline mode the ISR re-arms at `periodic_ns_`; in periodic
    /// bus-clock mode the hardware auto-re-arms.
    /// @param ns  Period in nanoseconds (0 = stop timer).
    static void set_timer_periodic(uint64_t ns);

    /// @brief Read the current APIC timer count.
    static uint32_t timer_current_count();

    /// @brief Check whether the CPU supports an APIC.
    static bool is_apic_supported();

    /// @brief Read this CPU's LAPIC ID (bits 24-31 of the ID register).
    /// @return LAPIC ID, or 0 if the APIC is not initialised.
    static uint32_t lapic_id();

    /// @brief IPI delivery modes for send_ipi (issue #25, Phase B2).
    enum class IpiMode : uint8_t {
        INIT_ASSERT,   ///< INIT, level assert (AP reset vector arm).
        INIT_DEASSERT, ///< INIT, level deassert (completes the INIT pulse).
        SIPI,          ///< Startup IPI — vector byte is the 4 KiB page index.
        FIXED,         ///< Fixed delivery — vector runs the IDT handler.
    };

    /// @brief Send an inter-processor interrupt to a LAPIC.
    /// INIT/SIPI drive AP bring-up; FIXED targets a running CPU's IDT.
    /// @param lapic_id Destination LAPIC ID (8-bit xAPIC, 32-bit x2APIC).
    /// @param vector   Vector byte (SIPI page index / FIXED IDT vector).
    /// @param mode     Delivery mode.
    /// @return true when the local APIC accepted the command
    ///         (delivery-status clear within the bounded poll).
    static bool send_ipi(uint32_t lapic_id, uint8_t vector, IpiMode mode);

    /// @brief Enable only the LOCAL APIC on an application processor.
    /// Programs SVR + masked LVTs + TPR 0 (the BSP-owned I/O APIC routing
    /// is left untouched).  Called by ap_main on each woken AP.
    static void init_ap();

    /// @brief Map APIC/I/O-APIC MMIO pages via VMM.
    /// Must be called after VMM::init() but before APIC::init().
    static bool map_mmio();

    /// @brief Mask or unmask a legacy IRQ in the I/O APIC.
    /// @param irq  Legacy IRQ number (0-15).
    /// @param mask true = mask, false = unmask.
    static void mask_irq(uint8_t irq, bool mask);

    static constexpr uint8_t APIC_TIMER_VECTOR = 64;

private:
    // ─── MMIO base addresses ──────────────────────────────────────────────
    static constexpr uint32_t LAPIC_PHYS      = 0xFEE00000;
    static constexpr uint32_t IOAPIC_PHYS     = 0xFEC00000;

    // ─── Local APIC register offsets ──────────────────────────────────────
    static constexpr uint32_t REG_ID           = 0x020;
    static constexpr uint32_t REG_VERSION      = 0x030;
    static constexpr uint32_t REG_TPR          = 0x080;
    static constexpr uint32_t REG_APR          = 0x090;
    static constexpr uint32_t REG_PPR          = 0x0A0;
    static constexpr uint32_t REG_EOI          = 0x0B0;
    static constexpr uint32_t REG_LDR          = 0x0D0;
    static constexpr uint32_t REG_DFR          = 0x0E0;
    static constexpr uint32_t REG_SPURIOUS     = 0x0F0;
    static constexpr uint32_t REG_ESR          = 0x280;
    static constexpr uint32_t REG_ICR_LOW      = 0x300;
    static constexpr uint32_t REG_ICR_HIGH     = 0x310;
    static constexpr uint32_t REG_LVT_TIMER    = 0x320;
    static constexpr uint32_t REG_LVT_THERMAL  = 0x330;
    static constexpr uint32_t REG_LVT_PERFMON  = 0x340;
    static constexpr uint32_t REG_LVT_LINT0    = 0x350;
    static constexpr uint32_t REG_LVT_LINT1    = 0x360;
    static constexpr uint32_t REG_LVT_ERROR    = 0x370;
    static constexpr uint32_t REG_TIMER_DIVIDE = 0x3E0;
    static constexpr uint32_t REG_TIMER_INITCNT = 0x380;
    static constexpr uint32_t REG_TIMER_CURCNT = 0x390;

    // ─── I/O APIC registers ───────────────────────────────────────────────
    static constexpr uint32_t IOAPIC_REGSEL = 0x00;
    static constexpr uint32_t IOAPIC_DATA   = 0x10;
    static constexpr uint32_t IOAPIC_ID     = 0x00;
    static constexpr uint32_t IOAPIC_VER    = 0x01;
    static constexpr uint32_t IOAPIC_REDIR  = 0x10;

    // ─── MSR addresses ────────────────────────────────────────────────────
    static constexpr uint32_t MSR_APIC_BASE    = 0x1B;
    static constexpr uint32_t MSR_TSC_DEADLINE = 0x6E0;

    // ─── Bit flags ────────────────────────────────────────────────────────
    static constexpr uint32_t APIC_BASE_ENABLE  = 0x800;
    static constexpr uint32_t APIC_BASE_X2APIC  = 0x400;
    static constexpr uint32_t LVT_MASKED        = 0x10000;
    static constexpr uint32_t LVT_TIMER_PERIODIC = 0x20000;
    static constexpr uint32_t LVT_TIMER_TSCDEADLINE = 0x40000;
    static constexpr uint32_t SPURIOUS_ENABLE   = 0x100;
    static constexpr uint32_t SPURIOUS_VECTOR   = 0xFF;
    static constexpr uint32_t TIMER_DIVIDE_1    = 0x0B;
    static constexpr uint32_t IOAPIC_MASKED     = 0x10000;

    // ─── Access mode ──────────────────────────────────────────────────────
    enum Mode { MODE_NONE, MODE_X2, MODE_XAPIC };
    static Mode mode_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static bool enabled_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static bool timer_active_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static uint64_t timer_tsc_delta_;
    // NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
    static uint64_t periodic_ns_;
    static uint32_t bus_freq_hz_;
    static uint32_t tsc_deadline_supported_;

    static void x2_write(uint32_t off, uint32_t v);
    static uint32_t x2_read(uint32_t off);
    static void xapic_write(uint32_t off, uint32_t v);
    static uint32_t xapic_read(uint32_t off);
    static void ioapic_write(uint32_t reg_sel, uint32_t v);
    static uint32_t ioapic_read(uint32_t reg_sel);
    static void ioapic_redirect(uint8_t irq, uint8_t vector, bool mask);
    static uint32_t calibrate_bus_hz();
    /// @brief Program SVR + masked LVTs + ESR clear + TPR 0 on the LOCAL
    ///        APIC (shared by init() and init_ap(); never touches I/O APIC).
    static void enable_local();
};

} // namespace arch
