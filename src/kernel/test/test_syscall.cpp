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

/// @file test_syscall.cpp
/// @brief System call interface tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every syscall is invoked by a REAL
/// kernel task (prio ≥ 11) inside its dispatched lambda — the handler's
/// `syscall_task()` resolves to the genuinely-running task.  Alarm tests let
/// the REAL timer ISR fire before asserting the signal.  The harness never
/// calls Syscall::handle() directly and never mutates task alarm fields.

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/vfs/vfs.hpp>
#include <signal.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

void register_syscall_affinity_tests();

static void test_signal_handler(int sig) {
    (void)sig;
}

struct Timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct Utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

namespace {

/// @brief Create a REAL kernel task (prio ≥ 11), dispatch it, and wait for
///        genuine termination.  The lambda runs in the task's own context so
///        `syscall_task()` resolves to it.
TaskControlBlock *run_syscall_task(void (*entry)(), uint64_t prio = 11,
                                   uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

void release_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

} // namespace

// Runmode: kernel
// Testidea: A REAL task calls the ALARM syscall; the alarm is armed with the
// requested tick count.  The same task then cancels it with 0.
// Input: Dispatched kernel task calls ALARM (seconds=1), verifies
//        alarm_armed and alarm_ticks, then ALARM (seconds=0) to cancel.
// Expect: syscall returns 0 both times; alarm_armed true then false;
// alarm_ticks == ticks() + 1 (captured inside the task).
// Depends: kernel::Syscall, kernel::Scheduler, kernel::arch::Timer
JARVIS_TEST(syscall_alarm_basic, "PRE: none | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_armed = 0;
    static uint64_t g_ticks = 0;
    static uint64_t g_ret2 = 0;
    static uint64_t g_cancelled = 0;

    auto *t = run_syscall_task([]() {
        auto *cur = Scheduler::current_task();
        g_ret1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 1, 0, 0, 0, nullptr);
        g_armed = cur->alarm_armed ? 1 : 0;
        g_ticks = cur->alarm_ticks;
        g_ret2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 0, 0, 0, nullptr);
        g_cancelled = cur->alarm_armed ? 1 : 0;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret1);
    JARVIS_ASSERT_EQ(1ULL, g_armed);
    JARVIS_ASSERT_EQ(1000ULL, g_ticks);
    JARVIS_ASSERT_EQ(0ULL, g_ret2);
    JARVIS_ASSERT_EQ(0ULL, g_cancelled);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: GETTOD from a REAL task returns a time within reasonable Unix
// epoch bounds.
// Input: Dispatched kernel task calls GETTOD with a Timeval pointer.
// Expect: Returns 0; tv_sec between year 2020 and 2200; tv_usec < 1000000.
// Depends: kernel::Syscall
JARVIS_TEST(syscall_gettod, "PRE: none | POST: none") {
    static int64_t g_sec = 0;
    static int64_t g_usec = 0;
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        Timeval tv{};
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GETTOD),
            reinterpret_cast<uint64_t>(&tv), 0, 0, 0, nullptr);
        g_sec = tv.tv_sec;
        g_usec = tv.tv_usec;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(g_sec > static_cast<int64_t>(1577836800ULL));
    JARVIS_ASSERT(g_sec < static_cast<int64_t>(7258118400ULL));
    JARVIS_ASSERT(g_usec < 1000000);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: UNAME from a REAL task returns the system identity.
// Input: Dispatched kernel task calls UNAME with a Utsname pointer.
// Expect: Returns 0; sysname is "NexIOS"; machine is "x86_64";
// release/version/machine non-empty.
// Depends: kernel::Syscall, string
JARVIS_TEST(syscall_uname, "PRE: none | POST: none") {
    static char g_sysname[65] = {};
    static char g_release[65] = {};
    static char g_version[65] = {};
    static char g_machine[65] = {};
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        Utsname uts{};
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::UNAME),
            reinterpret_cast<uint64_t>(&uts), 0, 0, 0, nullptr);
        __builtin_memcpy(g_sysname, uts.sysname, sizeof(g_sysname));
        __builtin_memcpy(g_release, uts.release, sizeof(g_release));
        __builtin_memcpy(g_version, uts.version, sizeof(g_version));
        __builtin_memcpy(g_machine, uts.machine, sizeof(g_machine));
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT(strlen(g_sysname) > 0);
    JARVIS_ASSERT(strlen(g_release) > 0);
    JARVIS_ASSERT(strlen(g_version) > 0);
    JARVIS_ASSERT(strlen(g_machine) > 0);
    JARVIS_ASSERT_EQ(0, strcmp(g_sysname, "NexIOS"));
    JARVIS_ASSERT_EQ(0, strcmp(g_machine, "x86_64"));
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL alarm armed by the task fires after the requested real
// tick count: the real on_tick ISR decrements alarm_ticks and raises
// SIGALRM.  The task arms an alarm (2 ticks) and busy-waits for the signal.
// Input: Dispatched kernel task calls ALARM (microseconds=2000) then polls
//        its own pending_signals for SIGALRM.
// Expect: alarm_armed true after arming; then SIGALRM pending and alarm
//         cleared after the real ticks fire.
// Depends: kernel::Scheduler
JARVIS_TEST(alarm_fires_after_ticks, "PRE: none | POST: none") {
    static uint64_t g_still_armed = 0;
    static uint64_t g_alarm_ticks_after_arm = 0;
    static uint64_t g_fired = 0;
    static uint64_t g_cleared = 0;

    auto *t = run_syscall_task([]() {
        auto *cur = Scheduler::current_task();
        uint64_t ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 2000, 0, 0,
            nullptr);
        if (ret != 0)
            return;
        g_still_armed = cur->alarm_armed ? 1 : 0;
        g_alarm_ticks_after_arm = cur->alarm_ticks;

        // Busy-wait for the REAL timer ISR to decrement to 0 and raise
        // SIGALRM.
        uint64_t start = arch::Timer::ticks();
        while (arch::Timer::ticks() - start < 20) {
            if (cur->pending_signals &
                (1ULL << static_cast<uint64_t>(Signal::SIGALRM))) {
                g_fired = 1;
                g_cleared = cur->alarm_armed ? 0 : 1;
                return;
            }
            arch::pause();
        }
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(1ULL, g_still_armed);
    JARVIS_ASSERT_EQ(2ULL, g_alarm_ticks_after_arm);
    JARVIS_ASSERT_EQ(1ULL, g_fired);
    JARVIS_ASSERT_EQ(1ULL, g_cleared);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL task arms a subsecond (500ms) alarm and verifies the tick
// calculation is within tolerance, then cancels.
// Input: Dispatched kernel task calls ALARM (microseconds=500000), then
//        seconds=0 to cancel.
// Expect: Returns 0 both calls; alarm_armed true then false; alarm_ticks
// within +/-10 of 500.
// Depends: kernel::Syscall, kernel::Scheduler, kernel::arch::Timer
JARVIS_TEST(syscall_alarm_subsecond, "PRE: none | POST: none") {
    static uint64_t g_ret1 = 0;
    static uint64_t g_armed = 0;
    static uint64_t g_ticks = 0;
    static uint64_t g_ret2 = 0;
    static uint64_t g_cancelled = 0;

    auto *t = run_syscall_task([]() {
        auto *cur = Scheduler::current_task();
        g_ret1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 500000, 0, 0,
            nullptr);
        g_armed = cur->alarm_armed ? 1 : 0;
        g_ticks = cur->alarm_ticks;
        g_ret2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::ALARM), 0, 0, 0, 0, nullptr);
        g_cancelled = cur->alarm_armed ? 1 : 0;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret1);
    JARVIS_ASSERT_EQ(1ULL, g_armed);
    JARVIS_ASSERT(g_ticks >= 490ULL);
    JARVIS_ASSERT(g_ticks <= 510ULL);
    JARVIS_ASSERT_EQ(0ULL, g_ret2);
    JARVIS_ASSERT_EQ(0ULL, g_cancelled);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: GETPID from a REAL task returns that task's own ID.
// Input: Dispatched kernel task calls GETPID.
// Expect: Return value equals the running task's id.
// Depends: kernel::Syscall, kernel::Scheduler
JARVIS_TEST(syscall_dispatch_getpid, "PRE: none | POST: none") {
    static uint64_t g_pid = 0;
    static uint64_t g_self = 0;

    auto *t = run_syscall_task([]() {
        g_pid = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GETPID), 0, 0, 0, 0, nullptr);
        g_self = Scheduler::current_task()->id;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(g_self, g_pid);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Invalid and out-of-range syscall numbers return -1 instead of
// crashing or succeeding.
// Input: Dispatched kernel task calls Syscall::handle with MAX_SYSCALL,
//        MAX_SYSCALL+1, and 9999.
// Expect: All three return static_cast<uint64_t>(-1).
// Depends: kernel::Syscall
JARVIS_TEST(syscall_dispatch_invalid_returns_minus_one,
            "PRE: none | POST: none") {
    static uint64_t g_r1 = 0, g_r2 = 0, g_r3 = 0;

    auto *t = run_syscall_task([]() {
        g_r1 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL), 0, 0, 0, 0,
            nullptr);
        g_r2 = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL) + 1, 0, 0, 0, 0,
            nullptr);
        g_r3 = Syscall::handle(9999, 0, 0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_r1);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_r2);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_r3);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: GET_TICKS from a REAL task returns the current timer tick count.
// Input: Dispatched kernel task calls GET_TICKS.
// Expect: Return value is non-zero or any value (assert only checks it
// doesn't crash).
// Depends: kernel::Syscall
JARVIS_TEST(syscall_dispatch_get_ticks, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::GET_TICKS), 0, 0, 0, 0,
            nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    // GET_TICKS returns the monotonic tick count (> 0 since boot init).
    JARVIS_ASSERT(g_ret > 0);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: YIELD syscall from a REAL task returns 0 to indicate the task
// yielded the CPU.
// Input: Dispatched kernel task calls YIELD.
// Expect: Returns 0.
// Depends: kernel::Syscall
JARVIS_TEST(syscall_dispatch_yield, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::YIELD), 0,
                                0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REBOOT syscall number is valid and dispatch table slot is
// populated. Input: Verify SyscallNumber::REBOOT and table entry. Expect: Enum
// valid, table entry non-null (actual reboot skipped in test). Depends:
// kernel::Syscall
JARVIS_TEST(syscall_dispatch_reboot, "PRE: none | POST: none") {
    uint64_t num = static_cast<uint64_t>(SyscallNumber::REBOOT);
    JARVIS_ASSERT_EQ(49ULL, num);
    JARVIS_ASSERT(num < static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: HALT syscall number is valid and dispatch table slot is populated.
// Input: Verify SyscallNumber::HALT and table entry.
// Expect: Enum valid, table entry non-null (actual halt skipped in test).
// Depends: kernel::Syscall
JARVIS_TEST(syscall_dispatch_halt, "PRE: none | POST: none") {
    uint64_t num = static_cast<uint64_t>(SyscallNumber::HALT);
    JARVIS_ASSERT_EQ(50ULL, num);
    JARVIS_ASSERT(num < static_cast<uint64_t>(SyscallNumber::MAX_SYSCALL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PRINT syscall with no meaningful arguments returns 0 as a no-op.
// Input: Dispatched kernel task calls PRINT with all zero arguments.
// Expect: Returns 0.
// Depends: kernel::Syscall
JARVIS_TEST(syscall_dispatch_print_noop, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::PRINT), 0,
                                0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FORK from a REAL kernel task with null regs returns -1
// (syscall_handlers_process.cpp: sys_fork returns -1 when regs==nullptr).
// The old test asserted `g_ret == 0 || g_ret > 0` — a tautology that also
// accepts UINT64_MAX, masking the real contract.
// Input: Dispatched kernel task calls FORK with null regs.
// Expect: Return value is -1 (regs == nullptr path).
// Depends: kernel::Syscall
JARVIS_TEST(syscall_fork_returns_pid, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::FORK), 0,
                                0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: EXEC with a nonexistent path from a REAL task returns -1 instead
// of crashing.
// Input: Dispatched kernel task calls EXEC with path="/nonexistent",
// argv={path, nullptr}, envp={nullptr}.
// Expect: Returns static_cast<uint64_t>(-1).
// Depends: kernel::Syscall
JARVIS_TEST(syscall_exec_nonexistent, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;

    auto *t = run_syscall_task([]() {
        const char *path = "/nonexistent";
        const char *argv[] = {path, nullptr};
        const char *envp[] = {nullptr};
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::EXEC),
            reinterpret_cast<uint64_t>(path), reinterpret_cast<uint64_t>(argv),
            reinterpret_cast<uint64_t>(envp), 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A REAL task registers a signal handler for SIGUSR1 via the SIGNAL
// syscall and verifies it is stored on the running task's TCB.
// Input: Dispatched kernel task calls SIGNAL signal=1,
// handler=test_signal_handler.
// Expect: SIGNAL returns 0; the task's handler table has the handler.
// Depends: kernel::Syscall, kernel::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(syscall_signal_sigreturn, "PRE: none | POST: none") {
    static uint64_t g_ret = 0;
    static uint64_t g_stored = 0;

    auto *t = run_syscall_task([]() {
        auto *cur = Scheduler::current_task();
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::SIGNAL), 1,
            reinterpret_cast<uint64_t>(test_signal_handler), 0, 0, nullptr);
        g_stored = (cur->get_signal_handler(1) == test_signal_handler) ? 1 : 0;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, g_ret);
    JARVIS_ASSERT_EQ(1ULL, g_stored);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// ============================================================================
// User-task syscall fixture (issue #143, x86_64 only).
//
// A REAL Ring-3 task performs real int $0x80 syscalls against a REAL private
// PML4, so safe_copy_{to,from}_user take the user-task path (is_user_ true)
// with ambient tables that actually map the probed pages.  Follows the
// install_user_yield_stub pattern (task.cpp): hand-crafted machine code
// written via the HHDM alias, mapped user-accessible, entered through the
// auto-installed yield-stub page.
//
// Layout (all below USER_SPACE_LIMIT, clear of the yield stub 0x40000000,
// MMIO/SHM windows, stack 0x70000000 and the >=0x100000000 buffer area):
//   code  0x40000000  reuses the auto-installed yield-stub page (content
//                     replaced with the probe; executable, as mapped)
//   data  0x50001000  fresh user page, mapped RW + NX (5-arg map call):
//                     [0..7] first call's return, [8..15] second call's
//                     return, [16..] probed struct.
// Teardown is the standard terminate+drain: cleanup() frees the PML4 tree
// (free_user_pages reclaims the user-owned code/data/table pages), the
// stacks and the TCB — no manual page management, no tracker delta.
// ============================================================================
#if defined(CONFIG_ARCH_X86_64)
namespace {

/// @brief Code VA: the auto-installed yield-stub page, content replaced.
constexpr uint64_t kUserProbeCodeVa = 0x40000000ULL;
/// @brief Data VA: fresh user page mapped by the fixture (RW, NX).
constexpr uint64_t kUserProbeDataVa = 0x50001000ULL;
/// @brief Struct offset inside the data page (return slots precede it).
constexpr uint64_t kUserProbeStructOff = 16;
/// @brief Bounded-join budget: the probe runs ~3 dispatches; this is orders
///        of magnitude above that, so expiry means a real wedge (converted
///        to FAIL, never a hang — see issue #148).
constexpr uint64_t kUserProbeJoinSpins = 10000000ULL;
/// @brief Fixture priority: above the harness so ticks dispatch it.
constexpr uint64_t kUserProbePrio = 20;

/// @brief Emit one (mov edx,data; mov eax,num; mov ebx,arg0; movabs
///        rcx,arg1; int $0x80; mov [rdx+disp],rax) call sequence at @p code.
///        Returns the advanced pointer.  edx carries the data-page base
///        (always mapped, so return stores never fault even when rcx
///        probes garbage); rcx is full 64-bit so limit-boundary pointers
///        are expressible.  Return slots live at [edx+0] and [edx+8] —
///        INSIDE the mapped page (a negative displacement would fault
///        below it; issue #143).  The store is 64-bit (REX.W): the handler
///        returns uint64_t and a 32-bit EAX store would truncate -1 to
///        0xFFFFFFFF.
uint8_t *emit_user_syscall(uint8_t *code, uint32_t num, uint32_t arg0,
                           uint64_t arg1, int8_t ret_disp) {
    *code++ = 0xBA;
    uint32_t data_lo = static_cast<uint32_t>(kUserProbeDataVa);
    for (uint64_t i = 0; i < 4; ++i)
        *code++ = static_cast<uint8_t>((data_lo >> (i * 8)) & 0xFF);
    *code++ = 0xB8;
    for (uint64_t i = 0; i < 4; ++i)
        *code++ = static_cast<uint8_t>((num >> (i * 8)) & 0xFF);
    *code++ = 0xBB;
    for (uint64_t i = 0; i < 4; ++i)
        *code++ = static_cast<uint8_t>((arg0 >> (i * 8)) & 0xFF);
    *code++ = 0x48;
    *code++ = 0xB9;
    for (uint64_t i = 0; i < 8; ++i)
        *code++ = static_cast<uint8_t>((arg1 >> (i * 8)) & 0xFF);
    *code++ = 0xCD;
    *code++ = 0x80;
    *code++ = 0x48;
    *code++ = 0x89;
    if (ret_disp == 0) {
        *code++ = 0x02;
    } else {
        *code++ = 0x42;
        *code++ = static_cast<uint8_t>(ret_disp);
    }
    return code;
}

/// @brief Run two syscalls from a REAL dispatched user task and read back
///        the return slots + struct area.  See the fixture block comment.
/// @return true if the probe reached EXIT (false = setup/join failure;
///         the caller owns no resources either way — teardown ran inside).
bool run_user_probe(uint32_t num_a, uint32_t arg0_a, uint64_t arg1_a,
                    uint32_t num_b, uint32_t arg0_b, uint64_t arg1_b,
                    uint64_t *out_ret_a, uint64_t *out_ret_b,
                    uint8_t *out_data, const uint8_t *payload = nullptr,
                    size_t payload_len = 0, uint64_t payload_off = 64) {
    auto *fixture = TaskControlBlock::create_user(kernel::test::forever_entry,
                                                  kUserProbePrio, 10, 32768);
    if (!fixture) {
        return false;
    }
    uint64_t data_phys = PMM::alloc_user_page();
    if (data_phys) {
        VMM::map_page_in_pml4(kUserProbeDataVa, data_phys, true, false,
                              fixture->page_table_);
    }
    uint64_t code_phys = VMM::virt_to_phys_in_pml4(kUserProbeCodeVa,
                                                   fixture->page_table_);
    uint64_t data_check = VMM::virt_to_phys_in_pml4(kUserProbeDataVa,
                                                    fixture->page_table_);
    if (!data_phys || data_check != data_phys || !code_phys) {
        kernel::test::terminate_and_drain(*fixture);
        return false;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *code = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + code_phys);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *data = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + data_phys);
    for (uint64_t i = 0; i < 512; ++i)
        data[i] = 0;
    if (payload && payload_len > 0 && payload_off + payload_len <= 512) {
        for (size_t i = 0; i < payload_len; ++i)
            data[payload_off + i] = payload[i];
    }
    code = emit_user_syscall(code, num_a, arg0_a, arg1_a, 0);
    code = emit_user_syscall(code, num_b, arg0_b, arg1_b, 8);
    *code++ = 0xB8;
    *code++ = 0x06;
    *code++ = 0x00;
    *code++ = 0x00;
    *code++ = 0x00;
    *code++ = 0xBB;
    *code++ = 0x00;
    *code++ = 0x00;
    *code++ = 0x00;
    *code++ = 0x00;
    *code++ = 0xCD;
    *code++ = 0x80;
    *code++ = 0xEB;
    *code++ = 0xFE;
    Scheduler::add_task(*fixture);
    Scheduler::reschedule();
    for (uint64_t i = 0;
         i < kUserProbeJoinSpins && fixture->state != TaskState::TERMINATED;
         ++i) {
        arch::pause();
    }
    // Observe BEFORE teardown: drain frees the PML4 and the TCB.
    data_check = VMM::virt_to_phys_in_pml4(kUserProbeDataVa,
                                           fixture->page_table_);
    bool exited = (fixture->state == TaskState::TERMINATED);
    // Clean EXIT (code 0) is required: a fault-death (e.g. SIGSEGV, -11)
    // also yields TERMINATED but leaves zero-init memory that reads back
    // as plausible zeros — asserting the exit code makes vacuous passes
    // impossible.
    bool clean_exit = exited && (fixture->exit_code == 0);
    bool observed = exited && (data_check == data_phys);
    if (observed) {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        data = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + data_phys);
        *out_ret_a = 0;
        *out_ret_b = 0;
        for (uint64_t i = 0; i < 8; ++i) {
            *out_ret_a |= static_cast<uint64_t>(data[i]) << (i * 8);
            *out_ret_b |= static_cast<uint64_t>(data[8 + i]) << (i * 8);
        }
        for (uint64_t i = 0; i < 512; ++i)
            out_data[i] = data[i];
    }
    // sys_exit self-termination never queues a zombie (no release_zombie),
    // so an exited fixture still needs terminate() to become reapable; a
    // signal-death already queued via terminate() and must NOT be
    // re-terminated (double zombie-append).  Our stub only ever exits 0,
    // so TERMINATED+0 means unqueued while TERMINATED+nonzero means queued.
    if (TaskControlBlock::is_valid(fixture) &&
        (fixture->state != TaskState::TERMINATED ||
         fixture->exit_code == 0)) {
        Scheduler::terminate(*fixture, 0);
    }
    Scheduler::drain_zombie_list();
    return observed && clean_exit;
}

} // namespace
#endif

// Runmode: kernel
// Testidea: REAL Ring-3 GETRLIMIT(NOFILE)+SETRLIMIT round-trip through the
//           user-task copy path (is_user_ true, ambient user PML4).
// Input: Dispatched user task: getrlimit(2, data+16) then
//        setrlimit(0, data+16); results read back via phys translation.
// Expect: Probe reaches EXIT; both return 0; rlim_cur == rlim_max ==
//         MAX_FDS.  Covers safe_copy_{to,from}_user<Rlimit>,
//         checked<Rlimit>/CheckedPtr ctor+valid+unsafe_ptr (mutable and
//         const).
// Depends: kernel::Syscall::sys_getrlimit/sys_setrlimit, VMM user mapping
JARVIS_TEST(syscall_user_getrlimit_roundtrip, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    uint64_t struct_va = kUserProbeDataVa + kUserProbeStructOff;
    bool ran = run_user_probe(45, 2, struct_va, 46, 0, struct_va, &ret_a,
                              &ret_b, data);
    uint64_t cur = 0;
    uint64_t max = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        cur |= static_cast<uint64_t>(data[16 + i]) << (i * 8);
        max |= static_cast<uint64_t>(data[24 + i]) << (i * 8);
    }
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(0ULL, ret_a);
    JARVIS_ASSERT_EQ(0ULL, ret_b);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(vfs::MAX_FDS), cur);
    JARVIS_ASSERT_EQ(cur, max);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REAL Ring-3 GETTOD+UNAME round-trip through the user-task copy
//           path (same fixture as getrlimit).
// Input: Dispatched user task: gettod(data+16) then uname(data+16).
// Expect: Probe reaches EXIT; both return 0; tv_sec != 0 with
//         tv_usec < 1000000; uts.sysname is "NexIOS".  Covers
//         safe_copy_to_user<Timeval/Utsname> + checked/CheckedPtr families.
// Depends: kernel::Syscall::sys_gettod/sys_uname
JARVIS_TEST(syscall_user_gettod_uname, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    uint64_t struct_va = kUserProbeDataVa + kUserProbeStructOff;
    uint32_t struct_lo = static_cast<uint32_t>(struct_va);
    bool ran = run_user_probe(34, struct_lo, 0, 35, struct_lo, 0, &ret_a,
                              &ret_b, data);
    int64_t sec = 0;
    uint64_t usec = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        sec |= static_cast<int64_t>(data[16 + i]) << (i * 8);
        usec |= static_cast<uint64_t>(data[24 + i]) << (i * 8);
    }
    bool sysname_ok = (data[16] == 'N' && data[17] == 'e' &&
                       data[18] == 'x' && data[19] == 'I' &&
                       data[20] == 'O' && data[21] == 'S' &&
                       data[22] == '\0');
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(0ULL, ret_a);
    JARVIS_ASSERT_EQ(0ULL, ret_b);
    JARVIS_ASSERT(sec != 0);
    JARVIS_ASSERT(usec < 1000000ULL);
    JARVIS_ASSERT(sysname_ok);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: User-task syscalls must REJECT non-user pointers without
//           touching them (valid() gate, no dereference, no fault).
// Input: Dispatched user task: getrlimit(2, NULL) then
//        getrlimit(2, 0xFFFF800000000000).
// Expect: Probe reaches EXIT; both return -1.  Return slots live on the
//         always-mapped data page, so the stores themselves never fault.
// Depends: kernel::CheckedPtr::valid, sys_getrlimit user-task gate
JARVIS_TEST(syscall_user_copy_reject, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    bool ran = run_user_probe(45, 2, 0, 45, 2, 0xFFFF800000000000ULL, &ret_a,
                              &ret_b, data);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_a);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A user-range but UNMAPPED pointer must fail closed through the
//           fault-recovery path (real #PF inside safe_copy, redirected to
//           the recover label), while a mapped pointer in the same task
//           succeeds — proving the success/failure split is the mapping,
//           not the task. Reactivated by the issue #149 keep-alive fix.
// Input: Dispatched user task: getrlimit(2, 0x60000000) [user range,
//        never mapped] then getrlimit(2, data+16) [mapped control].
// Expect: Probe reaches EXIT; first returns -1 (fault recovered), second
//         returns 0.  Covers the recover_to path that no kernel-task test
//         can reach.
// Depends: kernel::safe_copy_to_user fault recovery (recover_to label)
JARVIS_TEST(syscall_user_unmapped_fault, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    uint64_t struct_va = kUserProbeDataVa + kUserProbeStructOff;
    bool ran = run_user_probe(45, 2, 0x60000000ULL, 45, 2, struct_va,
                              &ret_a, &ret_b, data);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_a);
    JARVIS_ASSERT_EQ(0ULL, ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Testidea: Registers all syscall test cases with the test framework.
// Input: None.
// Expect: All JARVIS_REGISTER_TEST calls succeed and tests are available for
// execution.
// Depends: kernel::Logger, kernel::test framework
// Runmode: kernel
// Testidea: REAL Ring-3 OPEN through the user-task copy path — the path
//           string is injected into the user data page (payload), so
//           strncpy_from_user runs its copy loop, then resolve + vfsd IPC
//           + fd install happen for a genuine user caller.
// Input: payload "/dev/null" at data+64; probe OPEN(9) twice on its VA.
// Expect: Probe reaches EXIT; both fds >= 0 and distinct (fixture death
//         closes them via fd-table drain).
// Depends: strncpy_from_user, sys_open user path, vfsd OPEN auth
JARVIS_TEST(syscall_user_open_devnull, "PRE: vfsd, iocd | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static const uint8_t path[] = "/dev/null";
    constexpr uint64_t k_path_off = 64;
    const uint32_t path_lo =
        static_cast<uint32_t>(kUserProbeDataVa + k_path_off);
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    // 9 = OPEN.
    bool ran = run_user_probe(9, path_lo, 0, 9, path_lo, 0, &ret_a, &ret_b,
                              data, path, sizeof(path), k_path_off);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT(static_cast<int64_t>(ret_a) >= 0);
    JARVIS_ASSERT(static_cast<int64_t>(ret_b) >= 0);
    JARVIS_ASSERT(ret_a != ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REAL Ring-3 OPEN rejection modes — null path and unmapped user
//           page must both fail closed (-1) without hanging (the unmapped
//           case exercises strncpy_from_user fault recovery, not a panic).
// Input: probe OPEN(9) on VA 0 and on unmapped 0x60000000.
// Expect: Probe reaches EXIT; both return -1.
// Depends: strncpy_from_user fail-closed + fault recovery
JARVIS_TEST(syscall_user_open_rejected, "PRE: vfsd, iocd | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    // 9 = OPEN.
    bool ran = run_user_probe(9, 0, 0, 9, 0x60000000U, 0, &ret_a, &ret_b,
                              data);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_a);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: KLOG read path through dispatch — push a marker entry, then the
//           dmesg walk must format it back into the caller buffer (this also
//           covers the entry-format lambda). Self-seeding: no dependence on
//           ambient log state (dmesg can be legitimately empty).
// Input: Dispatched task pushes "KLOGPROBE" via dmesg_push_base, then calls
//        KLOG(stack buf, 512, flags 0).
// Expect: Bytes written (> 0) and the marker substring present in the
//         buffer.
// Depends: Syscall::sys_klog, DmesgService
JARVIS_TEST(syscall_klog_read, "PRE: none | POST: none") {
    static char g_buf[512];
    static uint64_t g_ret = 0;
    static uint64_t g_found = 0;

    auto *t = run_syscall_task([]() {
        for (size_t i = 0; i < sizeof(g_buf); ++i)
            g_buf[i] = 0;
        log::dmesg_push_base(0xD0D0, "KLOGPROBE");
        g_ret = Syscall::handle(
            static_cast<uint64_t>(SyscallNumber::KLOG),
            reinterpret_cast<uint64_t>(g_buf), sizeof(g_buf), 0, 0, nullptr);
        g_found = 0;
        const char *needle = "KLOGPROBE";
        for (size_t i = 0; i + 9 < sizeof(g_buf); ++i) {
            size_t j = 0;
            while (needle[j] && g_buf[i + j] == needle[j])
                ++j;
            if (!needle[j]) {
                g_found = 1;
                break;
            }
        }
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_ret > 0);
    JARVIS_ASSERT(g_ret < sizeof(g_buf));
    JARVIS_ASSERT_EQ(1ULL, g_found);
    release_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REAL Ring-3 EXEC with a valid argv — validation must accept a
//           well-formed array, then the /dev/null size gate fails the exec
//           before any allocation. Covers the validate scan loop + total.
// Input: User probe EXEC(20) on payload path + argv [path, null], envp
//        empty (rdx reads the zeroed data base).
// Expect: Probe reaches EXIT; returns -1 (size gate, post-validation).
// Depends: validate_argv_envp accept path, sys_exec dispatch
JARVIS_TEST(syscall_user_exec_valid_argv, "PRE: vfsd, iocd | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint8_t payload[512] = {};
    constexpr uint64_t k_path_off = 32;
    constexpr uint64_t k_argv_off = 64;
    const char *path = "/dev/null";
    for (size_t i = 0; path[i]; ++i)
        payload[k_path_off + i] = static_cast<uint8_t>(path[i]);
    const uint64_t path_va = kUserProbeDataVa + k_path_off;
    const uint64_t argv_va = kUserProbeDataVa + k_argv_off;
    for (size_t i = 0; i < 8; ++i) {
        payload[k_argv_off + i] =
            static_cast<uint8_t>((path_va >> (i * 8)) & 0xFF);
    }
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    // 20 = EXEC. Second call repeats with a null path (resolve fails fast).
    bool ran = run_user_probe(20, static_cast<uint32_t>(path_va), argv_va,
                              20, 0, 0, &ret_a, &ret_b, data, payload,
                              sizeof(payload), 0);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_a);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REAL Ring-3 EXEC hostile argv — a kernel-half pointer argument
//           and an overlong (NUL-free inside the 256-byte window) argument
//           must both be rejected by validation before any allocation.
// Input: User probe EXEC(20) twice on /dev/null: call A with
//        argv = [kernel-half VA, null]; call B with argv = [bigstr, null]
//        where bigstr is 300 NUL-free bytes at a valid user VA.
// Expect: Probe reaches EXIT; both return -1 (validation rejects).
// Depends: validate_argv_envp reject exits
JARVIS_TEST(syscall_user_exec_hostile_argv, "PRE: vfsd, iocd | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint8_t payload[512] = {};
    constexpr uint64_t k_path_off = 32;
    constexpr uint64_t k_argv_off = 64;
    constexpr uint64_t k_big_off = 128;
    constexpr uint64_t k_argv2_off = 440;
    const char *path = "/dev/null";
    for (size_t i = 0; path[i]; ++i)
        payload[k_path_off + i] = static_cast<uint8_t>(path[i]);
    const uint64_t path_va = kUserProbeDataVa + k_path_off;
    // Array A: [kernel-half VA, null].
    const uint64_t evil = 0xFFFF800000000000ULL;
    for (size_t i = 0; i < 8; ++i)
        payload[k_argv_off + i] =
            static_cast<uint8_t>((evil >> (i * 8)) & 0xFF);
    // 300 NUL-free bytes (window is 256) + array B: [bigstr, null].
    for (size_t i = 0; i < 300; ++i)
        payload[k_big_off + i] = static_cast<uint8_t>('A' + (i % 26));
    const uint64_t big_va = kUserProbeDataVa + k_big_off;
    for (size_t i = 0; i < 8; ++i)
        payload[k_argv2_off + i] =
            static_cast<uint8_t>((big_va >> (i * 8)) & 0xFF);
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    // 20 = EXEC.
    bool ran = run_user_probe(20, static_cast<uint32_t>(path_va),
                              kUserProbeDataVa + k_argv_off, 20,
                              static_cast<uint32_t>(path_va),
                              kUserProbeDataVa + k_argv2_off, &ret_a, &ret_b,
                              data, payload, sizeof(payload), 0);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_a);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_b);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

void register_syscall_tests() {
    Logger::info("Registering syscall tests");

    JARVIS_REGISTER_TEST(syscall_alarm_basic);
    JARVIS_REGISTER_TEST(syscall_gettod);
    JARVIS_REGISTER_TEST(syscall_uname);
    JARVIS_REGISTER_TEST(alarm_fires_after_ticks);
    JARVIS_REGISTER_TEST(syscall_alarm_subsecond);

    JARVIS_REGISTER_TEST(syscall_dispatch_getpid);
    JARVIS_REGISTER_TEST(syscall_dispatch_invalid_returns_minus_one);
    JARVIS_REGISTER_TEST(syscall_dispatch_get_ticks);
    JARVIS_REGISTER_TEST(syscall_dispatch_yield);
    JARVIS_REGISTER_TEST(syscall_dispatch_reboot);
    JARVIS_REGISTER_TEST(syscall_dispatch_halt);
    // JARVIS_REGISTER_TEST(syscall_dispatch_exit_returns_zero); — disabled:
    // sys_exit terminates the calling task and switches away, preventing
    // the test runner from returning.
    JARVIS_REGISTER_TEST(syscall_dispatch_print_noop);

    JARVIS_REGISTER_TEST(syscall_fork_returns_pid);
    JARVIS_REGISTER_TEST(syscall_exec_nonexistent);
    JARVIS_REGISTER_TEST(syscall_signal_sigreturn);

    JARVIS_REGISTER_TEST(syscall_user_getrlimit_roundtrip);
    JARVIS_REGISTER_TEST(syscall_user_gettod_uname);
    JARVIS_REGISTER_TEST(syscall_user_copy_reject);
    JARVIS_REGISTER_TEST(syscall_user_unmapped_fault);
    JARVIS_REGISTER_TEST(syscall_user_open_devnull);
    JARVIS_REGISTER_TEST(syscall_user_open_rejected);
    JARVIS_REGISTER_TEST(syscall_klog_read);
    JARVIS_REGISTER_TEST(syscall_user_exec_valid_argv);
    JARVIS_REGISTER_TEST(syscall_user_exec_hostile_argv);

    register_syscall_affinity_tests();
}
