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

/// @file test.cpp
/// @brief Kernel test framework implementation.

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/core/global_state.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_cleanup.hpp>
#include <kernel/test/test_watchdog.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/timer.hpp>

namespace kernel {
namespace test {

TestCase Registry::tests_[MAX_TESTS];
size_t Registry::count_ = 0;
size_t Registry::passed_ = 0;
size_t Registry::failed_ = 0;
size_t Registry::test_count_ = 0;
size_t Registry::test_failed_ = 0;
ClassSection Registry::sections_[MAX_CLASSES] = {};
size_t Registry::class_count_ = 0;
size_t Registry::expected_count_ = 0;
const char *Registry::current_name_ = nullptr;

// PfA-A: the harness injects one TestContext so the scheduler's ISR-visible
// test flags live here (not as scheduler globals).  Production boots with
// Scheduler::get_test_context() == nullptr.
static TestContext g_test_context;
static bool g_test_context_injected = false;

void set_kernel_entry_ns() {
    kernel::gs::set_kernel_entry_ns();
}

void Registry::init() {
    count_ = 0;
    passed_ = 0;
    failed_ = 0;
    test_count_ = 0;
    test_failed_ = 0;
    expected_count_ = 0;
}

void Registry::register_test(const TestCase& tc) {
    if (count_ >= MAX_TESTS) {
        Logger::error("Test registry full (%d tests)", MAX_TESTS);
        return;
    }
    tests_[count_++] = tc;
}

const TestCase* Registry::tests() { return tests_; }
size_t Registry::count() { return count_; }
size_t Registry::class_count() { return class_count_; }
const ClassSection* Registry::class_section(size_t i) {
    return (i < class_count_) ? &sections_[i] : nullptr;
}

void Registry::record_failure(const char* file, int line, const char* expr) {
    ++failed_;
    Logger::error("[TEST:FAIL] %s:%d: %s", file, line, expr);
}

void Registry::record_failure_fmt(const char* file, int line, const char* fmt, ...) {
    ++failed_;
    Logger::raw_write("\033[1;31m[TEST:FAIL] \033[0m");
    Logger::raw_write(file);
    Logger::raw_write(":");
    Logger::print_dec(line);
    Logger::raw_write(": ");
    __va_list args;
    va_start(args, fmt);
    Logger::vprint_raw(fmt, args);
    va_end(args);
    Logger::raw_write("\n");
}

void Registry::record_success() {
    ++passed_;
}

void Registry::record_test(bool passed) {
    ++test_count_;
    if (!passed) ++test_failed_;
}

void Registry::record_class_section(const char* name, size_t start, size_t count) {
    if (class_count_ >= MAX_CLASSES) return;
    sections_[class_count_].name  = name;
    sections_[class_count_].start = start;
    sections_[class_count_].count = count;
    ++class_count_;
}

void Registry::set_current_test_name(const char* name) { current_name_ = name; }

const char* Registry::current_test_name() { return current_name_; }

// Tag a task's `name` with the origin test case (and optional role tag) so that
// any leaked/orphaned task printed by the scheduler dump is traceable to the
// test that created it.  Truncates safely to CONFIG_TASK_NAME_LEN.
static void tag_task_name(TaskControlBlock& task, const char* tag) {
    const char* test = Registry::current_test_name();
    if (!test)
        return; // no active test — leave the default "task_N" name
    char buf[CONFIG_TASK_NAME_LEN];
    size_t pos = 0;
    if (tag) {
        for (const char* p = tag; *p && pos < CONFIG_TASK_NAME_LEN - 1; ++p)
            buf[pos++] = *p;
        if (pos < CONFIG_TASK_NAME_LEN - 1)
            buf[pos++] = ':';
    }
    for (const char* p = test; *p && pos < CONFIG_TASK_NAME_LEN - 1; ++p)
        buf[pos++] = *p;
    buf[pos] = '\0';
    __builtin_memcpy(task.name, buf, pos + 1);
}

TaskControlBlock* create_named_task(void (*entry)(), uint64_t priority,
                                     uint64_t period_ticks, const char* tag) {
    TaskControlBlock* t = TaskControlBlock::create(entry, priority, period_ticks);
    if (t)
        tag_task_name(*t, tag);
    return t;
}

void add_task_named(TaskControlBlock& task, const char* tag) {
    tag_task_name(task, tag);
    Scheduler::add_task(task);
}

void Registry::reset() {
    passed_ = 0;
    failed_ = 0;
    test_count_ = 0;
    test_failed_ = 0;
    expected_count_ = 0;
}

void Registry::clear() {
    count_ = 0;
    passed_ = 0;
    failed_ = 0;
    test_count_ = 0;
    test_failed_ = 0;
    expected_count_ = 0;
    class_count_ = 0;
}

size_t Registry::passed() { return passed_; }
size_t Registry::failed() { return failed_; }
size_t Registry::total() { return passed_ + failed_; }
size_t Registry::test_count() { return test_count_; }
size_t Registry::test_passed() { return test_count_ - test_failed_; }
size_t Registry::test_failed() { return test_failed_; }

void Registry::set_expected_count(size_t n) { expected_count_ = n; }
size_t Registry::expected_count() { return expected_count_; }

static void append_leak_detail(const ResourceCounters& before, const ResourceCounters& after) {
    struct LeakItem {
        const char* name;
        size_t before_val;
        size_t after_val;
    };
    LeakItem items[] = {
        {"MemPool0", before.mempool_used[0], after.mempool_used[0]},
        {"PMM", before.pmm_pages_used, after.pmm_pages_used},
        {"Tasks", before.tasks, after.tasks},
        {"BufPool", before.bufpool_entries, after.bufpool_entries},
        {"MsgQueues", before.msg_queues, after.msg_queues},
        {"Notifies", before.notifies, after.notifies},
        {"EventGroups", before.event_groups, after.event_groups},
        {"Drivers", before.drivers, after.drivers},
        {"PipeBufs", before.pipe_buffers, after.pipe_buffers},
        {"VNodes", before.vnodes, after.vnodes},
        {"OpenFDs", before.open_fds, after.open_fds},
    };
    bool first = true;
    for (auto& item : items) {
        if (item.after_val != item.before_val) {
            if (first) {
                Logger::raw_write(" [LEAK: ");
                first = false;
            } else {
                Logger::raw_write(", ");
            }
            Logger::raw_write(item.name);
            Logger::raw_write(" ");
            int diff = (int)item.after_val - (int)item.before_val;
            if (diff >= 0) {
                Logger::raw_write("+");
                Logger::print_dec(diff);
            } else {
                Logger::raw_write("-");
                Logger::print_dec(-diff);
            }
        }
    }
    if (!first) {
        Logger::raw_write("]");
    }
}

static void print_test_header(const TestCase& tc, const char* test_class, size_t test_num, size_t total_tests) {
    Logger::raw_write("S: ");
    if (test_class && test_class[0]) {
        Logger::raw_write(test_class);
        Logger::raw_write(" ");
    }
    Logger::raw_write(tc.suite);
    if (tc.suite[0]) Logger::raw_write("::");
    Logger::raw_write(tc.name);
    Logger::raw_write(" ");
    
    char num_buf[32];
    int pos = 0;
    size_t v = test_num;
    if (v == 0) num_buf[pos++] = '0';
    else {
        char rev[32];
        int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num_buf[pos++] = rev[--rp];
    }
    num_buf[pos++] = '/';
    v = total_tests;
    if (v == 0) num_buf[pos++] = '0';
    else {
        char rev[32];
        int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num_buf[pos++] = rev[--rp];
    }
    num_buf[pos++] = ':';
    num_buf[pos++] = ' ';
    num_buf[pos] = '\0';
    Logger::raw_write(num_buf);
}

static void run_one(const TestCase& tc, const char* test_class, size_t test_num, size_t total_tests) {
    ResourceCounters before_rsrc = {};
    kernel::test::ResourceTracker::instance().capture(before_rsrc);
    size_t before_fail = Registry::failed();

    if (tc.factory) {
        TestBase* t = tc.factory();
        t->execute();
        delete t;
    } else {
        tc.func();
    }

    ResourceCounters after_rsrc = {};
    kernel::test::ResourceTracker::instance().capture(after_rsrc);

    bool passed = (Registry::failed() == before_fail);

    print_test_header(tc, test_class, test_num, total_tests);
    Logger::raw_write(passed ? "PASS" : "FAIL");
    if (!passed) {
        append_leak_detail(before_rsrc, after_rsrc);
    }
    Logger::raw_write("\n");

    Registry::record_test(passed);
}

static const char* get_test_class(size_t test_index) {
    for (size_t ci = 0; ci < Registry::class_count(); ++ci) {
        auto* cs = Registry::class_section(ci);
        if (cs && test_index >= cs->start && test_index < cs->start + cs->count) {
            return cs->name;
        }
    }
    return "";
}

void run_all() {
    Registry::reset();
    size_t n = Registry::count();
    if (n == 0) {
        Logger::warn("No tests registered");
        return;
    }

    Logger::info("[TEST:RUN] Running %d test(s)", n);

    uint64_t start_ns = arch::Timer::ns();

    for (size_t i = 0; i < n; ++i) {
        auto& tc = Registry::tests()[i];
        const char* test_class = get_test_class(i);

        kernel::test::watchdog_arm(30000, tc.name);
        run_one(tc, test_class, i + 1, n);
        kernel::test::watchdog_disarm();
    }

    uint64_t end_ns = arch::Timer::ns();
    print_report(start_ns, end_ns);
}

void run_safe() {
    Registry::reset();
    size_t n = Registry::count();
    if (n == 0) {
        Logger::warn("No tests registered");
        return;
    }

    Logger::info("[TEST:RUN] Running test(s) (safe mode)");

    uint64_t start_ns = arch::Timer::ns();
    size_t run_index = 0;

    for (size_t i = 0; i < n; ++i) {
        auto& tc = Registry::tests()[i];
        if (tc.flags & TF_USER) continue;
        if (!(tc.flags & TF_RELEASE)) continue;

        ++run_index;
        const char* test_class = get_test_class(i);

        run_one(tc, test_class, run_index, n);
    }

    uint64_t end_ns = arch::Timer::ns();
    print_report(start_ns, end_ns);
}

void run_filtered(uint8_t required_flags, bool use_isolation) {
    Registry::reset();
    size_t n = Registry::count();
    if (n == 0) {
        Logger::warn("No tests registered");
        return;
    }

    size_t run_count = 0;
    Logger::info("[TEST:RUN] Running filtered test(s) (flags=%u)", (unsigned)required_flags);

    // Pre-count tests matching the filter — this is the expected count.
    // If post-run test_count_ differs, tests were silently lost.
    size_t expected = 0;
    for (size_t i = 0; i < n; ++i) {
        auto& tc = Registry::tests()[i];
        if (tc.flags & TF_USER) continue;
        if (required_flags && !(tc.flags & required_flags)) continue;
        if (kernel::gs::get_filter_bench() && (tc.flags & TF_BENCH)) continue;
        ++expected;
    }
    Registry::set_expected_count(expected);

    // Pre-condition: ensure deadline-monitor task exists before snapshot
    // sizing, so the monitor is counted in the task count from the start.
#if CONFIG_DEADLINE_MONITOR_TASK
    Scheduler::ensure_monitor();
#endif

    // Take a snapshot of the entire kernel state before the first test so
    // we can restore it between individual tests (full isolation).
    bool snapshot_ok = false;
    if (use_isolation && n > 0) {
        snapshot_ok = kernel::test::snapshot_create();
        if (!snapshot_ok) {
            Logger::warn("Test snapshot creation failed — isolation disabled");
        }
    }

    uint64_t start_ns = arch::Timer::ns();
    Logger::raw_write("[TEST_START] ns=");
    Logger::print_dec(start_ns);
    Logger::raw_write("\n");

    // PfA-A: inject the test context for this cycle.  set_test_active() and
    // the ISR-visible is_test_active() read through this context.
    if (!g_test_context_injected) {
        g_test_context = TestContext{};
        Scheduler::set_test_context(&g_test_context);
        g_test_context_injected = true;
    }
    Scheduler::set_test_active(true);

    for (size_t i = 0; i < n; ++i) {
        auto& tc = Registry::tests()[i];
        // Match: if required_flags is 0 (debug), run all non-user tests.
        // If required_flags has TF_RELEASE set, run only tests with TF_RELEASE.
        if (tc.flags & TF_USER) continue;
        if (required_flags && !(tc.flags & required_flags)) continue;
        if (kernel::gs::get_filter_bench() && (tc.flags & TF_BENCH)) continue;

        for (size_t ci = 0; ci < Registry::class_count(); ++ci) {
            auto* cs = Registry::class_section(ci);
            if (cs && i == cs->start) {
                Logger::raw_write("\n--- ");
                Logger::raw_write(cs->name);
                Logger::raw_write(" ---\n");
            }
        }

        ++run_count;
        const char* test_class = get_test_class(i);

        // Arm the per-test watchdog (30 seconds = 30000 ticks at 1 kHz)
        char wd_name[128];
        int wp = 0;
        const char* ws = tc.suite;
        while (*ws && wp < 60) wd_name[wp++] = *ws++;
        if (wp > 0 && tc.suite[0]) { wd_name[wp++] = ':'; wd_name[wp++] = ':'; }
        ws = tc.name;
        while (*ws && wp < 126) wd_name[wp++] = *ws++;
        wd_name[wp] = '\0';
        kernel::test::watchdog_check_inline();
        kernel::test::watchdog_arm(30000, wd_name);

        Logger::info("[RUN_PRE] vfsd_pid=%u iocd_pid=%u",
                     kernel::vfsd::get_vfsd_pid(), kernel::iocd::get_iocd_pid());
        run_one(tc, test_class, run_count, expected);

        kernel::test::watchdog_disarm();
        kernel::test::watchdog_check_inline();

        // Restore the snapshot after each test so the next test starts
        // from a clean slate (tasks, memory, daemons, page tables).
        if (snapshot_ok) {
            char test_buf[128];
            int pos = 0;
            const char* s = tc.suite;
            while (*s && pos < 120) test_buf[pos++] = *s++;
            if (pos > 0 && tc.suite[0]) { test_buf[pos++] = ':'; test_buf[pos++] = ':'; }
            s = tc.name;
            while (*s && pos < 126) test_buf[pos++] = *s++;
            test_buf[pos] = '\0';
            kernel::test::snapshot_restore(test_buf);
#if CONFIG_DEADLINE_MONITOR_TASK
            // snapshot_create() was called before set_test_active(true),
            // so the snapshot has s_test_active_ == false.  Restore it
            // to true to prevent the ISR from waking the deadline monitor
            // during tests.
#if CONFIG_DEADLINE_MONITOR_TASK
    Scheduler::set_test_active(true);
#endif
#endif
        }
    }

    uint64_t end_ns = arch::Timer::ns();

    if (snapshot_ok) {
        kernel::test::snapshot_destroy();
        kernel::test::reload_daemon_tasks();
        kernel::daemon::restart_stale_daemons();
    } else {
        kernel::test::test_cleanup_all();
    }

    if (run_count == 0) {
        Logger::warn("No tests matched flags 0x%x", required_flags);
    }

    print_report(start_ns, end_ns);

#if CONFIG_DEADLINE_MONITOR_TASK
    Scheduler::set_test_active(false);
#endif

    // Drain serial TX FIFO so the full test report is flushed to the
    // expect script before QEMU exits (fixes BUGS.md #012).
    {
        static constexpr uint16_t COM1       = 0x3F8;
        static constexpr uint16_t COM1_LSR   = 0x3FD;
        // Write a final newline so any buffered line is emitted.
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        arch::outb(COM1, '\n');
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        arch::outb(COM1, '\r');
        // Wait for THR empty (bit 5) then TSR empty (bit 6).
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        while ((arch::inb(COM1_LSR) & 0x40) == 0) { }
    }
}

void run_debug() {
    run_filtered(0, true);
}

void run_benchmarks() {
    run_filtered(TF_BENCH, true);
}

void run_release() {
    run_filtered(TF_RELEASE, false);
}

[[noreturn]] void shutdown_kernel(uint64_t result) {
    // Disable interrupts so no timer/IRQ can fire and produce more
    // serial output after the summary.
    arch::cli();
    // Drain serial TX FIFO before signalling QEMU to exit.
    {
        static constexpr uint16_t COM1       = 0x3F8;
        static constexpr uint16_t COM1_LSR   = 0x3FD;
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        arch::outb(COM1, '\n');
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        arch::outb(COM1, '\r');
        while ((arch::inb(COM1_LSR) & 0x20) == 0) { }
        while ((arch::inb(COM1_LSR) & 0x40) == 0) { }
    }
    // Signal QEMU to exit via multiple methods with io_wait() between
    // attempts to ensure each out* instruction completes.  Even if none
    // work (e.g. UEFI intercepts legacy ports), the Makefile kills QEMU
    // from the host side after the expect script exits.
    arch::io_wait();
    arch::outw(arch::QEMU_ACPI_PORT, 0x2000);
    arch::io_wait();
    arch::outw(arch::QEMU_SHUTDOWN_PORT, 0x2000);
    arch::io_wait();
    arch::qemu_debug_exit(static_cast<uint8_t>(result));
    arch::io_wait();
    // Keyboard controller reset (triple fault -> QEMU exit with -no-reboot)
    {
        uint8_t good;
        do {
            good = arch::inb(0x64);
        } while (good & 0x02);
        arch::io_wait();
        arch::outb(0x64, 0xFE);
    }
    arch::io_wait();
    // If QEMU doesn't exit, halt permanently.
    for (;;) {
        arch::hlt();
    }
    __builtin_unreachable();
}

void run_registered(uint8_t required_flags) {
    // When no explicit flags and the current class is a bench variant,
    // run benchmarks only.  For non-bench classes (e.g. "all", "safe"),
    // skip TF_BENCH tests to keep the suite within QEMU timeout.
    bool is_bench = kernel::gs::get_current_class() &&
                    kernel::gs::get_current_class()[0] == 'b' &&
                    kernel::gs::get_current_class()[1] == 'e';
    if (required_flags == 0 && !is_bench) {
        // Run non-benchmark tests only
        kernel::gs::set_filter_bench(true);
        run_filtered(0, true);
        kernel::gs::set_filter_bench(false);
    } else {
        run_filtered(required_flags, true);
    }
    if (kernel::gs::get_class_auto_shutdown()) {
        uint64_t result = (Registry::test_failed() == 0) ? 0 : 1;
        shutdown_kernel(result);
    }
}

void set_class_auto_shutdown(bool enabled) {
    kernel::gs::set_class_auto_shutdown(enabled);
}

void run_suite(const char* suite_name) {
    Registry::reset();
    size_t n = Registry::count();

    Logger::info("[TEST:RUN] Suite: %s", suite_name);

    // Take snapshot once before the suite for isolation
    bool snapshot_ok = true;
    if (n > 0) {
        snapshot_ok = kernel::test::snapshot_create();
        if (!snapshot_ok) {
            Logger::warn("Test snapshot creation failed — isolation disabled");
        }
    }

    uint64_t start_ns = arch::Timer::ns();
    size_t run = 0;

    for (size_t i = 0; i < n; ++i) {
        auto& tc = Registry::tests()[i];
        if (tc.suite[0] == '\0') continue;
        if (strcmp(tc.suite, suite_name) != 0) continue;

        ++run;
        const char* test_class = get_test_class(i);

        run_one(tc, test_class, run, n);

        // Restore kernel state between tests (tasks, MemPool, page tables)
        if (snapshot_ok) {
            char test_buf[128];
            int pos = 0;
            const char* s = tc.suite;
            while (*s && pos < 120) test_buf[pos++] = *s++;
            if (pos > 0 && tc.suite[0]) { test_buf[pos++] = ':'; test_buf[pos++] = ':'; }
            s = tc.name;
            while (*s && pos < 126) test_buf[pos++] = *s++;
            test_buf[pos] = '\0';
            kernel::test::snapshot_restore(test_buf);
        }
    }

    uint64_t end_ns = arch::Timer::ns();

    if (snapshot_ok) {
        kernel::test::snapshot_destroy();
        kernel::test::reload_daemon_tasks();
        kernel::daemon::restart_stale_daemons();
    }

    if (run == 0) {
        Logger::warn("No tests found for suite '%s'", suite_name);
    }

    print_report(start_ns, end_ns);
}

void print_report(uint64_t start_ns, uint64_t end_ns) {
    size_t tp = Registry::test_passed();
    size_t tf = Registry::test_failed();
    size_t tt = tp + tf;
    size_t exp = Registry::expected_count();
    uint64_t elapsed_ms = (end_ns > start_ns) ? ((end_ns - start_ns) / 1000000ULL) : 0;

    Logger::raw_write("[TEST_END] ns=");
    Logger::print_dec(end_ns);
    Logger::raw_write(" elapsed_ms=");
    Logger::print_dec(elapsed_ms);
    Logger::raw_write("\n");

    Logger::raw_write("\n");
    Logger::raw_write("==============================\n");
    Logger::raw_write(" TEST SUMMARY\n");
    Logger::raw_write("==============================\n");

    auto write_num = [](size_t n) {
        char buf[24];
        int pos = 23;
        buf[23] = '\0';
        if (n == 0) { Logger::raw_write("0"); return; }
        size_t v = n;
        while (v > 0 && pos > 0) { buf[--pos] = '0' + (v % 10); v /= 10; }
        Logger::raw_write(buf + pos);
    };

    Logger::raw_write("  PLANNED:    "); write_num(exp); Logger::raw_write("\n");
    Logger::raw_write("  EXECUTED:   "); write_num(tt); Logger::raw_write("\n");
    Logger::raw_write("  TIME_ELAPSED_MS: "); write_num(elapsed_ms); Logger::raw_write("\n");
    uint64_t entry_ns = kernel::gs::get_kernel_entry_ns();
    if (entry_ns > 0 && start_ns > entry_ns) {
        uint64_t boot_ms = (start_ns - entry_ns) / 1000000ULL;
        Logger::raw_write("  BOOT_TIME_MS:    "); write_num(boot_ms); Logger::raw_write("\n");
    }
    Logger::raw_write("  PASSED:     "); write_num(tp); Logger::raw_write("\n");
    Logger::raw_write("  FAILED:     "); write_num(tf); Logger::raw_write("\n");
    Logger::raw_write("==============================\n");

    if (exp != tt) {
        Logger::raw_write("[WARN] Expected ");
        write_num(exp);
        Logger::raw_write(" tests, but only ");
        write_num(tt);
        Logger::raw_write(" reported results — tests may have been lost\n");
    }

    Logger::raw_write("\n");
}

} // namespace test
} // namespace kernel
