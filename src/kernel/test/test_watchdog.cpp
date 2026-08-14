/// @file test_watchdog.cpp
/// @brief Watchdog timer implementation tests.

#include <kernel/test/test_watchdog.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>

namespace kernel::test {

static uint64_t g_watchdog_deadline_ns = 0;
static uint64_t g_watchdog_task_id = 0;
static char g_test_name[128] = {};

// ---------------------------------------------------------------------------
// PIT hardware backup for watchdog timing
//
// PIT channel 0 is always running in mode 3 at 1000 Hz (divisor = 1193).
// We latch and read it at each check to measure elapsed counter units even
// when interrupts are disabled.  This provides a pure-hardware fallback
// timing source independent of TSC calibration.
//
// PIT channel 1 is used as a ~55ms one-shot that we reload each check.
// ---------------------------------------------------------------------------
namespace {
constexpr uint16_t PIT_CTL = 0x43;
constexpr uint16_t PIT_CH0 = 0x40;
constexpr uint16_t PIT_CH1 = 0x41;
constexpr uint16_t PIT1_INIT = 0xFFFF; // max count (~55 ms)

// Latched PIT channel 0 state for elapsed-time accumulation.
uint16_t s_pit0_prev = 0;
uint64_t s_pit0_acc = 0; // accumulated counter units
uint16_t s_pit0_div = 0;

void pit0_latch_start(uint16_t div) {
    arch::outb(PIT_CTL, 0x00); // latch counter 0
    s_pit0_prev = arch::inb(PIT_CH0) | ((uint16_t)arch::inb(PIT_CH0) << 8);
    s_pit0_acc = 0;
    s_pit0_div = div;
}

/// @return elapsed ns since last pit0_latch_start(), or 0 if no divisor set.
uint64_t pit0_elapsed_ns() {
    if (s_pit0_div == 0)
        return 0;

    arch::outb(PIT_CTL, 0x00);
    uint16_t cur = arch::inb(PIT_CH0) | ((uint16_t)arch::inb(PIT_CH0) << 8);

    uint16_t delta;
    if (cur <= s_pit0_prev) {
        delta = s_pit0_prev - cur;
    } else {
        delta = s_pit0_prev + (s_pit0_div - cur);
    }
    s_pit0_acc += delta;
    s_pit0_prev = cur;

    // Each counter unit = 2 PIT ticks = 2/1193182 seconds.
    // ns = acc * 2 * 1e9 / 1193182 = acc * 1000000000 / 596591
    return (s_pit0_acc * 1000000000ULL) / 596591ULL;
}

void pit1_arm() {
    arch::outb(PIT_CTL, 0x70); // counter 1, mode 0 (one-shot), LSB/MSB, binary
    arch::outb(PIT_CH1, PIT1_INIT & 0xFF);
    arch::outb(PIT_CH1, (PIT1_INIT >> 8) & 0xFF);
}

/// @return true if PIT channel 1 one-shot has expired (counter reached 0).
bool pit1_expired() {
    arch::outb(PIT_CTL, 0x40); // latch counter 1
    uint16_t val = arch::inb(PIT_CH1) | ((uint16_t)arch::inb(PIT_CH1) << 8);
    return val == 0;
}
} // anonymous namespace

void watchdog_arm(uint64_t ms, const char *test_name) {
    // 1. TSC-based deadline
    uint64_t now_ns = arch::Timer::ns();
    g_watchdog_deadline_ns = now_ns + (uint64_t)ms * 1000000ULL;

    // 2. PIT channel 0 elapsed-time baseline (hardware fallback)
    pit0_latch_start(1193); // divisor for 1000 Hz PIT

    // 3. PIT channel 1 one-shot (~55 ms hardware countdown)
    pit1_arm();

    // 4. Task / name tracking
    auto *task = Scheduler::current_task();
    g_watchdog_task_id = task ? task->id : 0;
    for (int i = 0; i < 127; ++i) {
        char c = test_name[i];
        g_test_name[i] = c;
        if (c == '\0')
            break;
    }
    g_test_name[127] = '\0';
}

void watchdog_disarm() {
    g_watchdog_deadline_ns = 0;
}

void watchdog_check_inline() {
    if (g_watchdog_deadline_ns == 0)
        return;

    // --- Primary check: TSC-based elapsed time ---
    {
        uint64_t now_ns = arch::Timer::ns();
        if (now_ns > 0 && now_ns >= g_watchdog_deadline_ns) {
            auto *task = Scheduler::current_task();
            if (task && task->id == g_watchdog_task_id) {
                watchdog_panic();
            }
        }
    }

    // --- Fallback: PIT channel 0 accumulated time (when TSC freq is 0) ---
    {
        uint64_t pit_ns = pit0_elapsed_ns();
        if (pit_ns > 0 && pit_ns >= g_watchdog_deadline_ns) {
            auto *task = Scheduler::current_task();
            if (task && task->id == g_watchdog_task_id) {
                watchdog_panic();
            }
        }
    }

    // --- Reload PIT channel 1 one-shot for next interval ---
    if (pit1_expired()) {
        pit1_arm();
    }
}

void watchdog_panic() {
    g_watchdog_deadline_ns = 0;

    Logger::raw_write("\n[WATCHDOG] Test timed out while running \"");
    Logger::raw_write(g_test_name);
    Logger::raw_write("\":\n");

    auto *task = Scheduler::current_task();
    if (task) {
        Logger::raw_write("  task_id=");
        Logger::print_dec(task->id);
        Logger::raw_write(" state=");
        Logger::print_dec(static_cast<uint64_t>(task->state));
        Logger::raw_write(" context.ip=0x");
#if defined(CONFIG_ARCH_X86_64)
        Logger::print_hex(task->context.rip);
#elif defined(CONFIG_ARCH_AARCH64)
        Logger::print_hex(task->context.elr_el1);
#endif
        Logger::raw_write("\n");
    } else {
        Logger::raw_write("  (no current task)\n");
    }

    uint64_t rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    Logger::raw_write("  current_rip=0x");
    Logger::print_hex(rip);
    Logger::raw_write("\n");

    Logger::raw_write("[WATCHDOG] Halting.\n");
    arch::qemu_debug_exit(1);
    while (1) {
        arch::hlt();
    }
}

} // namespace kernel::test
