/// @file test_apic_tpr.cpp
/// @brief Tests for APIC TPR-based interrupt prioritization (issue #26).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/nexios_config.h>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/apic.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/bootparams.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/hal/tpr_guard.hpp>
#include <kernel/irq_delivery.hpp>
#endif

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

// Blockable live-probe vector: class 0x70 (== TPR_CLASS_IPC), free
// (timer 0xE0, syscall 0x80, self-test 0xEF, sched 0xEC, spurious
// 0xFF are the taken vectors).
constexpr uint8_t kTprProbeVector = 0x71;

// Set by the probe handler; polled by the blocking test.
volatile uint64_t g_tpr_ipi_seen = 0;

// TSC at handler entry (diagnostic: entry delay vs the send
// timestamps whether a tick healed TPR before delivery).
volatile uint64_t g_tpr_ipi_tsc = 0;

// Runmode: kernel
// Testidea: Per-CPU TPR shadow defaults to ACCEPT_ALL with frozen gs:0x40.
/* Pseudocode: read per_cpu[cpu].tpr_shadow == 0; assert field offset 0x40. */
// Input: none (percpu_init_* ran at boot).
// Expect: shadow ACCEPT_ALL on BSP; &tpr_shadow == page + 0x40.
// Depends: arch::per_cpu, percpu_init_bsp/ap
JARVIS_TEST(apic_tpr_shadow_defaults, "PRE: isolate | POST: none") {
    arch::PerCpu &pc = arch::per_cpu[0];
    const auto *base = reinterpret_cast<const uint64_t *>(&pc);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.tpr_shadow) ==
                  base + 8);
    JARVIS_ASSERT(sizeof(arch::PerCpu) == 4096);
    JARVIS_ASSERT(pc.tpr_shadow == arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: set/raise/restore round-trip; sub-class bits forced zero.
/* Pseudocode: set IPC -> get==0x70; raise SCHED -> 0xD0; raise PIC (no-op);
   restore ACCEPT_ALL; set(0xFF...)->false; set(0xE0)->false. */
// Input: none.
// Expect: monotonic raise/restore honored; over-MAX rejected; low bits 0.
// Depends: arch::APIC TPR accessors
JARVIS_TEST(apic_tpr_set_raise_restore, "PRE: isolate | POST: none") {
    if (!arch::APIC::is_enabled()) {
        Logger::warn("APIC not enabled — skipping");
        JARVIS_TEST_PASS();
        return;
    }
    // Deterministic window: no ISR may interleave between the MMIO
    // accesses (a tick would only re-assert the tracked shadow, but cli
    // removes all doubt and the window is a few MMIO reads).
    arch::IrqGuard irq_guard{};
    JARVIS_ASSERT(arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_IPC));
    JARVIS_ASSERT(arch::APIC::get_tpr_class() == arch::APIC::TPR_CLASS_IPC);
    // Sub-class bits forced to zero.
    JARVIS_ASSERT(arch::APIC::set_tpr_class(0x71));
    JARVIS_ASSERT(arch::APIC::get_tpr_class() == arch::APIC::TPR_CLASS_IPC);
    // Monotonic raise: lowering via raise() is a no-op.
    arch::APIC::tpr_raise(arch::APIC::TPR_CLASS_PIC);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() == arch::APIC::TPR_CLASS_IPC);
    arch::APIC::tpr_raise(arch::APIC::TPR_CLASS_SCHED);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_SCHED);
    // Monotonic restore: raising via restore() is a no-op.
    arch::APIC::tpr_restore(arch::APIC::TPR_CLASS_IPC);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    // Over-MAX inputs are rejected fail-closed (HW untouched).
    JARVIS_ASSERT(!arch::APIC::set_tpr_class(0xE0));
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    arch::APIC::tpr_raise(0xE0);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    arch::APIC::tpr_restore(0xE0);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    // Leave HW as found (cookbook Rule 5 analogue for interrupt state).
    arch::APIC::tpr_restore(arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PPR read is diagnostic-only and never mutates TPR.
/* Pseudocode: set IPC; read PPR twice; get_tpr_class still IPC. */
// Input: none.
// Expect: PPR reads stable; TPR unchanged.
// Depends: arch::APIC::get_ppr
JARVIS_TEST(apic_tpr_ppr_read_only, "PRE: isolate | POST: none") {
    if (!arch::APIC::is_enabled()) {
        Logger::warn("APIC not enabled — skipping");
        JARVIS_TEST_PASS();
        return;
    }
    // cli window: an interleaving tick ISR would legitimately raise the
    // PPR to the timer class, so exact-value assertions need stillness.
    arch::IrqGuard irq_guard{};
    JARVIS_ASSERT(arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_IPC));
    uint8_t ppr0 = arch::APIC::get_ppr();
    uint8_t ppr1 = arch::APIC::get_ppr();
    JARVIS_ASSERT(ppr0 == ppr1);
    JARVIS_ASSERT(ppr0 == arch::APIC::TPR_CLASS_IPC);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    arch::APIC::tpr_restore(arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: TprGuard raises on scope entry, restores on exit (nested).
/* Pseudocode: guard(PIC) -> class PIC; nested guard(IPC) -> IPC; inner
   exit -> PIC; outer exit -> ACCEPT_ALL. */
// Input: none.
// Expect: LIFO class restore; shadow tracks HW at each step.
// Depends: arch::TprGuard
JARVIS_TEST(apic_tpr_guard_nesting, "PRE: isolate | POST: none") {
    if (!arch::APIC::is_enabled()) {
        Logger::warn("APIC not enabled — skipping");
        JARVIS_TEST_PASS();
        return;
    }
    arch::IrqGuard irq_guard{};
    {
        arch::TprGuard outer(arch::APIC::TPR_CLASS_PIC);
        JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                      arch::APIC::TPR_CLASS_PIC);
        {
            arch::TprGuard inner(arch::APIC::TPR_CLASS_IPC);
            JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                          arch::APIC::TPR_CLASS_IPC);
        }
        JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                      arch::APIC::TPR_CLASS_PIC);
    }
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_ASSERT(arch::per_cpu_current()->tpr_shadow ==
                  arch::APIC::TPR_CLASS_ACCEPT_ALL);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Timer vector reservation moved with APIC_TIMER_VECTOR.
/* Pseudocode: assert APIC_TIMER_VECTOR == 0xE0; claim_slot(0xE0) < 0. */
// Input: none.
// Expect: tick vector unclaimable; class 0xE above TPR_CLASS_MAX.
// Depends: arch::APIC, IrqDelivery::claim_slot
JARVIS_TEST(apic_tpr_timer_vector_reserved, "PRE: isolate | POST: none") {
    // INV-TPR5: the tick sits at class 0xE, above every raised class.
    JARVIS_ASSERT(arch::APIC::APIC_TIMER_VECTOR == 0xE0);
    JARVIS_ASSERT((arch::APIC::APIC_TIMER_VECTOR & 0xF0U) >
                  arch::APIC::TPR_CLASS_MAX);
    JARVIS_ASSERT((arch::APIC::SCHED_VECTOR & 0xF0U) >
                  arch::APIC::TPR_CLASS_MAX);
    // The tick vector is unclaimable (moved with the symbol, #26 audit).
    JARVIS_ASSERT(IrqDelivery::claim_slot(arch::APIC::APIC_TIMER_VECTOR) <
                  0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The TPR blocks live delivery below threshold and holds the
//           IRQ pending: with TPR raised to IPC a self-IPI on the
//           class-0x70 probe vector does NOT run its handler; after
//           lowering to ACCEPT_ALL the pending IRQ delivers (held, not
//           lost).  The raised window runs with the APIC tick STOPPED:
//           the tick ISR heals TPR to ACCEPT_ALL on every entry (by
//           design, #26), so a running tick would clear the raised
//           class within ~1 ms and the IPI would legitimately deliver.
//           Waits use raw TSC (ticks are frozen while stopped); the
//           tick is re-armed BEFORE asserting (cookbook Rule 5).
// Input: IDT handler on 0x71 + self-IPI under TPR IPC (tick stopped)
//        then ACCEPT_ALL (tick re-armed).
// Expect: No delivery within 50 ms while raised; delivery within
//         100 ms after lowering.  HW + tick restored before asserting.
// Depends: arch::APIC TPR + send_ipi + timer_stop/init/start,
//         IDT::register_handler_raw
JARVIS_TEST(apic_tpr_blocks_ipi_below_threshold, "PRE: isolate | POST: none") {
    if (!arch::APIC::is_enabled()) {
        Logger::warn("APIC not enabled — skipping");
        JARVIS_TEST_PASS();
        return;
    }
    uint64_t freq = arch::Timer::tsc_freq_hz();
    JARVIS_ASSERT(freq > 0);
    uint64_t boot_hz = BootParams::instance().timer_hz;
    JARVIS_ASSERT(boot_hz != 0);
    arch::IDT::register_handler_raw(
        kTprProbeVector, [](uint64_t, uint64_t, uint64_t) {
            g_tpr_ipi_tsc = arch::rdtsc();
            g_tpr_ipi_seen = 1;
            arch::APIC::eoi();
        });
    uint32_t bsp = arch::APIC::lapic_id();

    // Baseline: ACCEPT_ALL delivers the probe promptly.
    g_tpr_ipi_seen = 0;
    JARVIS_ASSERT(arch::APIC::send_ipi(bsp, kTprProbeVector,
                                       arch::APIC::IpiMode::FIXED));
    uint64_t start = arch::rdtsc();
    uint64_t limit = freq / 10;
    while (g_tpr_ipi_seen == 0) {
        if (arch::rdtsc() - start > limit) {
            break;
        }
        asm volatile("pause");
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_tpr_ipi_seen);

    // Raised with the tick stopped (no ISR can heal the class):
    // the same IPI must stay pending (handler must not run).
    arch::APIC::timer_stop();
    JARVIS_ASSERT(arch::APIC::set_tpr_class(arch::APIC::TPR_CLASS_IPC));
    JARVIS_ASSERT(arch::APIC::get_tpr_class() ==
                  arch::APIC::TPR_CLASS_IPC);
    g_tpr_ipi_seen = 0;
    g_tpr_ipi_tsc = 0;
    uint64_t sent_at = arch::rdtsc();
    JARVIS_ASSERT(arch::APIC::send_ipi(bsp, kTprProbeVector,
                                       arch::APIC::IpiMode::FIXED));
    limit = freq / 20;
    while (g_tpr_ipi_seen == 0) {
        if (arch::rdtsc() - sent_at > limit) {
            break;
        }
        asm volatile("pause");
    }
    bool held = (g_tpr_ipi_seen == 0);
    uint64_t entry_delta = (g_tpr_ipi_tsc >= sent_at)
                               ? (g_tpr_ipi_tsc - sent_at)
                               : 0;
    // Re-arm tick + lower TPR BEFORE asserting (Rule 5): the class
    // must never leak raised and the tick must never stay dead.
    arch::APIC::timer_init(static_cast<uint32_t>(boot_hz));
    arch::APIC::timer_start();
    arch::APIC::tpr_restore(arch::APIC::TPR_CLASS_ACCEPT_ALL);
    Logger::info("tpr_block: held=%u entry_delta_tsc=%lu", held ? 1 : 0,
                 entry_delta);
    JARVIS_ASSERT(held);

    // Lowered: the held IRQ delivers (pending, not dropped).
    start = arch::rdtsc();
    limit = freq / 10;
    while (g_tpr_ipi_seen == 0) {
        if (arch::rdtsc() - start > limit) {
            break;
        }
        asm volatile("pause");
    }
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), g_tpr_ipi_seen);
    JARVIS_TEST_PASS();
}

void register_apic_tpr_tests() {
    Logger::info("Registering APIC TPR tests");
    JARVIS_REGISTER_TEST(apic_tpr_shadow_defaults);
    JARVIS_REGISTER_TEST(apic_tpr_set_raise_restore);
    JARVIS_REGISTER_TEST(apic_tpr_ppr_read_only);
    JARVIS_REGISTER_TEST(apic_tpr_guard_nesting);
    JARVIS_REGISTER_TEST(apic_tpr_timer_vector_reserved);
    JARVIS_REGISTER_TEST(apic_tpr_blocks_ipi_below_threshold);
}

#endif // CONFIG_ARCH_X86_64
