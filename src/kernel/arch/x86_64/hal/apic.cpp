#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/cpuid.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/address.hpp>
#include <logger.hpp>

namespace arch {

// ─── Static members ───────────────────────────────────────────────────────
APIC::Mode APIC::mode_          = MODE_NONE;
bool        APIC::enabled_      = false;
bool        APIC::timer_active_ = false;
uint64_t    APIC::timer_tsc_delta_ = 0;
uint64_t    APIC::periodic_ns_ = 0;
uint32_t    APIC::bus_freq_hz_  = 0;
uint32_t    APIC::tsc_deadline_supported_ = 0;

// ─── CPUID caps ──────────────────────────────────────────────────────────
struct Caps { bool apic, x2apic, tsc_deadline; };
static Caps caps() {
    Caps c{};
    auto r = cpuid(1);
    c.apic         = (r.edx >> 9) & 1;
    c.x2apic       = (r.ecx >> 21) & 1;
    c.tsc_deadline = (r.ecx >> 24) & 1;
    return c;
}

// ─── MMIO helpers ────────────────────────────────────────────────────────
static constexpr uint64_t LAPIC_PHYS  = 0xFEE00000;
static constexpr uint64_t IOAPIC_PHYS = 0xFEC00000;

// x2APIC MSR base: each APIC register at MSR (0x800 + mmio_offset/16)
static inline uint32_t x2apic_msr(uint32_t mmio_offset) {
    return 0x800 + (mmio_offset >> 4);
}

// I/O APIC constants (mirrored from APIC class to keep file‑static helpers)
static constexpr uint32_t IOAPIC_REDIR_BASE  = 0x10;
static constexpr uint32_t IOAPIC_MASKED_BIT  = 0x10000;

static volatile uint32_t *lapic_ptr(uint32_t off) {
    return reinterpret_cast<volatile uint32_t *>(
        arch::HHDM_OFFSET + LAPIC_PHYS + off);
}

static void lapic_wr(uint32_t off, uint32_t v) { *lapic_ptr(off) = v; }
static uint32_t lapic_rd(uint32_t off) { return *lapic_ptr(off); }

static volatile uint32_t *ioapic_ptr(uint32_t reg) {
    return reinterpret_cast<volatile uint32_t *>(
        arch::HHDM_OFFSET + IOAPIC_PHYS + reg);
}

static void ioapic_wr(uint32_t sel, uint32_t v) {
    *ioapic_ptr(0x00) = sel;
    *ioapic_ptr(0x10) = v;
}

static void ioapic_redirect(uint8_t irq, uint8_t vector, bool masked) {
    uint32_t entry = vector;
    if (masked) entry |= IOAPIC_MASKED_BIT;
    ioapic_wr(IOAPIC_REDIR_BASE + irq * 2,     entry);
    ioapic_wr(IOAPIC_REDIR_BASE + irq * 2 + 1, 0);
}

// ─── Public API ───────────────────────────────────────────────────────────

bool APIC::is_apic_supported() { return caps().apic; }

bool APIC::map_mmio() {
    auto map = [](uint64_t phys) {
        kernel::VMM::map_page(arch::HHDM_OFFSET + phys, phys, false);
    };
    map(LAPIC_PHYS);
    map(IOAPIC_PHYS);
    return true;
}

bool APIC::init() {
    auto c = caps();
    if (!c.apic) {
        kernel::Logger::warn("APIC: not supported");
        return false;
    }

    tsc_deadline_supported_ = c.tsc_deadline;

    // ── 1. Enable the local APIC ──────────────────────────────────────────
    // Try x2APIC (MSR) first, fall back to xAPIC (MMIO).
    bool x2 = c.x2apic;
    uint64_t base = rdmsr(MSR_APIC_BASE);
    base |= APIC_BASE_ENABLE;
    if (x2) base |= APIC_BASE_X2APIC;
    else    base &= ~APIC_BASE_X2APIC;
    wrmsr(MSR_APIC_BASE, base);
    mode_ = x2 ? MODE_X2 : MODE_XAPIC;

    auto wr = [&](uint32_t off, uint32_t v) {
        if (x2) wrmsr(x2apic_msr(off), v);
        else    lapic_wr(off, v);
    };
    auto rd = [&](uint32_t off) -> uint32_t {
        if (x2) return static_cast<uint32_t>(rdmsr(x2apic_msr(off)));
        else    return lapic_rd(off);
    };

    // ── 2. Spurious vector + enable ──────────────────────────────────────
    wr(REG_SPURIOUS, SPURIOUS_VECTOR | SPURIOUS_ENABLE);

    // ── 3. Mask all LVT entries ──────────────────────────────────────────
    wr(REG_LVT_TIMER,   LVT_MASKED);
    wr(REG_LVT_THERMAL, LVT_MASKED);
    wr(REG_LVT_PERFMON, LVT_MASKED);
    wr(REG_LVT_LINT0,   LVT_MASKED);
    wr(REG_LVT_LINT1,   LVT_MASKED);
    wr(REG_LVT_ERROR,   LVT_MASKED);

    // ── 4. Clear error status ────────────────────────────────────────────
    wr(REG_ESR, 0);
    rd(REG_ESR);

    // ── 5. TPR = 0 (accept all interrupt priorities) ─────────────────────
    wr(REG_TPR, 0);

    // ── 6. I/O APIC: route legacy IRQs ───────────────────────────────────
    arch::ioapic_redirect(0, 32, false);
    arch::ioapic_redirect(1, 33, false);
    for (int i = 2; i < 16; ++i)
        arch::ioapic_redirect(i, 32 + i, true);

    enabled_ = true;

    kernel::Logger::info("APIC: %s mode enabled, I/O APIC routing IRQ0→32 IRQ1→33",
                         x2 ? "x2APIC" : "xAPIC");
    return true;
}

void APIC::eoi() {
    if (!enabled_) return;
    if (mode_ == MODE_X2)
        wrmsr(x2apic_msr(REG_EOI), 0);
    else
        lapic_wr(REG_EOI, 0);
}

void APIC::mask_irq(uint8_t irq, bool mask) {
    if (!enabled_ || irq >= 16) return;
    uint32_t entry = 32 + irq;
    if (mask) entry |= IOAPIC_MASKED_BIT;
    arch::ioapic_wr(IOAPIC_REDIR_BASE + irq * 2,     entry);
    arch::ioapic_wr(IOAPIC_REDIR_BASE + irq * 2 + 1, 0);
}

// ─── APIC timer ───────────────────────────────────────────────────────────

// Helper: compute TSC delta for a given nanosecond interval.
// Returns 0 if tsc_freq is unknown.
static uint64_t ns_to_tsc_delta(uint64_t ns) {
    uint64_t freq = arch::Timer::tsc_freq_hz();
    if (freq == 0) {
        kernel::Logger::warn("APIC timer: TSC frequency unknown");
        return 0;
    }
    // delta = (ns * freq) / 1_000_000_000
    // Use double-word arithmetic via __udivdi3/libgcc helpers.  The product
    // can exceed 2^64, so decompose: a = ns / 1e9, b = ns % 1e9.
    uint64_t a = ns / 1000000000ULL;
    uint64_t b = ns % 1000000000ULL;
    // ns*freq = (a*1e9 + b)*freq = a*freq*1e9 + b*freq
    // Divide each term by 1e9:
    // result = a*freq + (b*freq) / 1e9
    // The second term may still overflow 64-bit: b*freq can be up to
    // 1e9 * 4e9 = 4e18 < 2^64 (~1.8e19), so it fits in 64 bits.
    return a * freq + (b * freq) / 1000000000ULL;
}

void APIC::x2_write(uint32_t off, uint32_t v) { wrmsr(x2apic_msr(off), v); }
uint32_t APIC::x2_read(uint32_t off) { return static_cast<uint32_t>(rdmsr(x2apic_msr(off))); }

uint32_t APIC::lapic_id() {
    if (!enabled_) return 0;
    uint32_t id = (mode_ == MODE_X2) ? x2_read(REG_ID) : lapic_rd(REG_ID);
    return id >> 24;
}

void APIC::timer_init(uint32_t frequency_hz) {
    if (!enabled_) return;
    auto wr = [&](uint32_t off, uint32_t v) {
        if (mode_ == MODE_X2) wrmsr(x2apic_msr(off), v);
        else lapic_wr(off, v);
    };

    if (tsc_deadline_supported_) {
        wr(REG_LVT_TIMER, APIC_TIMER_VECTOR | LVT_TIMER_TSCDEADLINE);
        kernel::Logger::info("APIC timer: TSC-deadline, %lu Hz", frequency_hz);
    } else {
        bus_freq_hz_ = calibrate_bus_hz();
        if (bus_freq_hz_ == 0) {
            kernel::Logger::warn("APIC timer: calibration failed, keeping PIT");
            return;
        }
        kernel::Logger::info("APIC timer: periodic, bus=%lu Hz target=%lu Hz",
                             bus_freq_hz_, frequency_hz);
        uint32_t count = static_cast<uint32_t>(
            (bus_freq_hz_ / frequency_hz) & 0xFFFFFFFFULL);
        if (count == 0)
            count = 1;
        wr(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);
        wr(REG_LVT_TIMER, APIC_TIMER_VECTOR | LVT_TIMER_PERIODIC);
        // Periodic bus-clock mode needs an initial count loaded into
        // REG_TIMER_INITCNT; the counter then counts down and auto-reloads
        // on each period (no re-arm in the ISR needed).  Without this the
        // timer never starts and ticks freeze after the first IRQ.
        wr(REG_TIMER_INITCNT, count);
    }
    timer_active_ = true;
    periodic_ns_  = 1000000000ULL / frequency_hz;

    // Mask I/O APIC IRQ0 (PIT) — the APIC timer now drives the system tick.
    arch::ioapic_wr(IOAPIC_REDIR_BASE + 0 * 2, 32 | IOAPIC_MASKED_BIT);
    arch::ioapic_wr(IOAPIC_REDIR_BASE + 0 * 2 + 1, 0);
}

void APIC::set_timer_oneshot(uint64_t ns) {
    if (!enabled_ || ns == 0) return;
    periodic_ns_ = 0;  // disarm re-arm

    if (tsc_deadline_supported_) {
        uint64_t delta = ns_to_tsc_delta(ns);
        if (delta == 0) return;
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + delta);
    } else {
        // Bus-clock one-shot: write initial count, clear periodic bit
        if (bus_freq_hz_ == 0) return;
        uint32_t count = static_cast<uint32_t>((bus_freq_hz_ * ns) / 1000000000ULL);
        if (count == 0) count = 1;
        if (mode_ == MODE_X2) {
            x2_write(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);
            x2_write(REG_LVT_TIMER, APIC_TIMER_VECTOR);
            x2_write(REG_TIMER_INITCNT, count);
        } else {
            lapic_wr(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);
            lapic_wr(REG_LVT_TIMER, APIC_TIMER_VECTOR);
            lapic_wr(REG_TIMER_INITCNT, count);
        }
    }
}

void APIC::set_timer_periodic(uint64_t ns) {
    if (!enabled_ || ns == 0) return;
    periodic_ns_ = ns;

    if (tsc_deadline_supported_) {
        // TSC-deadline: set first deadline; ISR re-arms at periodic_ns_
        uint64_t delta = ns_to_tsc_delta(ns);
        if (delta == 0) return;
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + delta);
    } else {
        // Bus-clock periodic: write initial count, set periodic LVT
        if (bus_freq_hz_ == 0) return;
        uint32_t count = static_cast<uint32_t>((bus_freq_hz_ * ns) / 1000000000ULL);
        if (count == 0) count = 1;
        if (mode_ == MODE_X2) {
            x2_write(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);
            x2_write(REG_LVT_TIMER, APIC_TIMER_VECTOR | LVT_TIMER_PERIODIC);
            x2_write(REG_TIMER_INITCNT, count);
        } else {
            lapic_wr(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);
            lapic_wr(REG_LVT_TIMER, APIC_TIMER_VECTOR | LVT_TIMER_PERIODIC);
            lapic_wr(REG_TIMER_INITCNT, count);
        }
    }
}

void APIC::timer_start() {
    if (!timer_active_) return;
    if (periodic_ns_ == 0) return;  // not in periodic mode
    if (tsc_deadline_supported_) {
        uint64_t delta = ns_to_tsc_delta(periodic_ns_);
        if (delta == 0) return;
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + delta);
    }
    // Bus-clock periodic: auto-re-arms in hardware — no-op
}
void APIC::timer_stop() {
    if (!enabled_) return;
    if (tsc_deadline_supported_)
        wrmsr(MSR_TSC_DEADLINE, 0);
    if (mode_ == MODE_X2)
        x2_write(REG_LVT_TIMER, LVT_MASKED);
    else
        lapic_wr(REG_LVT_TIMER, LVT_MASKED);
    timer_active_ = false;
    periodic_ns_  = 0;
}

uint32_t APIC::timer_current_count() {
    if (mode_ == MODE_X2) return x2_read(REG_TIMER_CURCNT);
    return lapic_rd(REG_TIMER_CURCNT);
}

// ─── Bus frequency calibration (periodic mode only) ──────────────────────
uint32_t APIC::calibrate_bus_hz() {
    // One-shot mode: write a known initial count, measure elapsed TSC
    auto wr = [&](uint32_t off, uint32_t v) {
        if (mode_ == MODE_X2) wrmsr(x2apic_msr(off), v);
        else lapic_wr(off, v);
    };
    auto rd = [&](uint32_t off) -> uint32_t {
        if (mode_ == MODE_X2)
            return static_cast<uint32_t>(rdmsr(x2apic_msr(off)));
        return lapic_rd(off);
    };

    wr(REG_LVT_TIMER, APIC_TIMER_VECTOR);
    wr(REG_TIMER_DIVIDE, TIMER_DIVIDE_1);

    constexpr uint32_t TEST_COUNT = 0x1000000;
    wr(REG_TIMER_INITCNT, TEST_COUNT);

    uint64_t ts0 = rdtsc();
    for (int i = 0; i < 1000000; ++i) {
        if (rd(REG_TIMER_CURCNT) <= TEST_COUNT / 2) break;
        asm volatile("pause");
    }
    uint64_t ts1 = rdtsc();
    uint32_t rem = rd(REG_TIMER_CURCNT);
    uint32_t elapsed = TEST_COUNT - rem;
    if (elapsed == 0) return 0;

    uint64_t tsc_freq = arch::Timer::tsc_freq_hz();
    if (tsc_freq == 0) tsc_freq = 2000000000ULL;
    return static_cast<uint32_t>((elapsed * tsc_freq) / (ts1 - ts0));
}

} // namespace arch
