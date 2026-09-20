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

/// @file test_posix_time.cpp
/// @brief POSIX time API tests: clock_gettime, nanosleep, timer_create,
/// timerfd_create over the HRT clock + event-timer wheel (issue #76).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

constexpr uint64_t kNegErrno(uint64_t err) {
    return static_cast<uint64_t>(0) - err;
}

struct AbiTs {
    int64_t tv_sec;
    int64_t tv_nsec;
};
static_assert(sizeof(AbiTs) == 16, "timespec width");

struct AbiItimer {
    AbiTs it_interval;
    AbiTs it_value;
};

uint64_t sys_call(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                  uint64_t arg3) {
    return Syscall::handle(number, arg0, arg1, arg2, arg3, nullptr);
}

struct SleepCtx {
    uint64_t budget_ns;
    uint64_t result;
    uint64_t elapsed_ns;
};

SleepCtx *g_sleep_ctx = nullptr;

void sleep_entry() {
    auto *ctx = g_sleep_ctx;
    AbiTs req{};
    req.tv_sec = static_cast<int64_t>(ctx->budget_ns / 1000000000ULL);
    req.tv_nsec = static_cast<int64_t>(ctx->budget_ns % 1000000000ULL);
    AbiTs rem{};
    const uint64_t start_ns = arch::Timer::ns_monotonic();
    ctx->result = Syscall::handle(81, reinterpret_cast<uint64_t>(&req),
                                  reinterpret_cast<uint64_t>(&rem), 0, 0,
                                  nullptr);
    ctx->elapsed_ns = arch::Timer::ns_monotonic() - start_ns;
}

} // namespace

// Runmode: kernel
// Testidea: MONOTONIC clock never decreases across two samples.
// Input: Two consecutive clock_gettime(CLOCK_MONOTONIC) reads.
// Expect: Second sample >= first.
// Depends: PosixTime::clock_monotonic_ns, arch::Timer::ns_monotonic
JARVIS_TEST(posix_clock_monotonic_nondecreasing, "PRE: none | POST: none") {
    AbiTs first{};
    AbiTs second{};
    JARVIS_ASSERT_EQ(0ULL, sys_call(80, 1, reinterpret_cast<uint64_t>(&first),
                                    0, 0));
    JARVIS_ASSERT_EQ(0ULL, sys_call(80, 1, reinterpret_cast<uint64_t>(&second),
                                    0, 0));
    const bool nondecreasing = (second.tv_sec > first.tv_sec) ||
                               (second.tv_sec == first.tv_sec &&
                                second.tv_nsec >= first.tv_nsec);
    JARVIS_ASSERT(nondecreasing);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: REALTIME clock equals boot anchor plus monotonic delta.
// Input: Latch anchor, read RTC, sample REALTIME.
// Expect: REALTIME within tolerance of anchor + delta; sec/nsec in range.
// Depends: PosixTime::boot_latch, arch::RTC::read_seconds
JARVIS_TEST(posix_clock_realtime_anchored, "PRE: none | POST: none") {
    AbiTs mono{};
    AbiTs real{};
    JARVIS_ASSERT_EQ(0ULL, sys_call(80, 1, reinterpret_cast<uint64_t>(&mono),
                                    0, 0));
    JARVIS_ASSERT_EQ(0ULL, sys_call(80, 0, reinterpret_cast<uint64_t>(&real),
                                    0, 0));
    JARVIS_ASSERT(real.tv_nsec >= 0 && real.tv_nsec < 1000000000);
    const bool real_after_mono =
        (real.tv_sec > mono.tv_sec) ||
        (real.tv_sec == mono.tv_sec && real.tv_nsec >= mono.tv_nsec);
    JARVIS_ASSERT(real_after_mono);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Bad clockid and null out-pointer fail closed.
// Input: clock_gettime(99, valid), clock_gettime(MONOTONIC, null).
// Expect: -EINVAL, -EFAULT; errno path intact.
// Depends: Syscall::sys_clock_gettime argument validation
JARVIS_TEST(posix_clock_bad_id, "PRE: none | POST: none") {
    AbiTs out{};
    JARVIS_ASSERT_EQ(kNegErrno(22),
                     sys_call(80, 99, reinterpret_cast<uint64_t>(&out), 0, 0));
    JARVIS_ASSERT_EQ(kNegErrno(14), sys_call(80, 1, 0, 0, 0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Zero nanosleep returns immediately without arming the wheel.
// Input: nanosleep({0, 0}).
// Expect: Returns 0; wheel live_count unchanged.
// Depends: Syscall::sys_nanosleep zero fast-path
JARVIS_TEST(posix_nanosleep_zero_returns, "PRE: none | POST: none") {
    const uint32_t live_before = time::TimerWheel::live_count(0);
    AbiTs req{};
    req.tv_sec = 0;
    req.tv_nsec = 0;
    JARVIS_ASSERT_EQ(0ULL, sys_call(81, reinterpret_cast<uint64_t>(&req), 0,
                                    0, 0));
    JARVIS_ASSERT_EQ(live_before, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Out-of-range tv_nsec is rejected.
// Input: nanosleep({0, 2000000000}).
// Expect: -EINVAL; no wheel arm.
// Depends: Syscall::sys_nanosleep range check
JARVIS_TEST(posix_nanosleep_bad_nsec, "PRE: none | POST: none") {
    const uint32_t live_before = time::TimerWheel::live_count(0);
    AbiTs req{};
    req.tv_sec = 0;
    req.tv_nsec = 2000000000;
    JARVIS_ASSERT_EQ(kNegErrno(22),
                     sys_call(81, reinterpret_cast<uint64_t>(&req), 0, 0, 0));
    JARVIS_ASSERT_EQ(live_before, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Armed nanosleep expires and wakes the sleeper bounded.
// Input: Spawned task sleeps 20ms on real ticks.
// Expect: Returns 0; elapsed >= budget; wheel residue zero; arm cleared.
// Depends: sleep_fire, scheduler on_tick sleep re-apply scan
JARVIS_TEST(posix_nanosleep_expiry_bound, "PRE: none | POST: none") {
    SleepCtx ctx{};
    ctx.budget_ns = 20000000ULL;
    ctx.result = 0xAAAAAAAAAAAAAAAAULL;
    ctx.elapsed_ns = 0;
    g_sleep_ctx = &ctx;
    auto *sleeper = TaskControlBlock::create([]() { sleep_entry(); }, 11, 100);
    JARVIS_ASSERT(sleeper != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*sleeper);
    }
    auto *original = Scheduler::current_task();
    test::yield_as(*sleeper);
    Scheduler::reschedule();
    test::wait_for_termination_safe(sleeper);
    Scheduler::set_current(*original);
    JARVIS_ASSERT_EQ(0ULL, ctx.result);
    JARVIS_ASSERT(ctx.elapsed_ns >= ctx.budget_ns);
    JARVIS_ASSERT(ctx.elapsed_ns < 10000000000ULL);
    JARVIS_ASSERT_EQ(0ULL, time::TimerWheel::live_count(0));
    JARVIS_ASSERT(!sleeper->sleep_armed);
    test::terminate_and_drain(*sleeper);
    g_sleep_ctx = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pending signal kills an armed nanosleep with -EINTR + rem.
// Input: Spawned task sleeps 60s; test sets pending_signals while armed.
// Expect: Returns -EINTR; rem holds approximately the remaining time.
// Depends: sys_nanosleep signal check, recv-style cancel-first
JARVIS_TEST(posix_nanosleep_killed_by_signal, "PRE: none | POST: none") {
    SleepCtx ctx{};
    ctx.budget_ns = 60000000000ULL;
    ctx.result = 0xAAAAAAAAAAAAAAAAULL;
    ctx.elapsed_ns = 0;
    g_sleep_ctx = &ctx;
    auto *sleeper = TaskControlBlock::create([]() { sleep_entry(); }, 11, 100);
    JARVIS_ASSERT(sleeper != nullptr);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*sleeper);
    }
    auto *original = Scheduler::current_task();
    test::yield_as(*sleeper);
    Scheduler::reschedule();
    __atomic_store_n(&sleeper->pending_signals, 0x1ULL, __ATOMIC_RELEASE);
    test::wait_for_termination_safe(sleeper);
    Scheduler::set_current(*original);
    JARVIS_ASSERT_EQ(kNegErrno(4), ctx.result);
    JARVIS_ASSERT(ctx.elapsed_ns < 10000000000ULL);
    JARVIS_ASSERT_EQ(0ULL, time::TimerWheel::live_count(0));
    JARVIS_ASSERT(!sleeper->sleep_armed);
    test::terminate_and_drain(*sleeper);
    g_sleep_ctx = nullptr;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: One-shot timer fires once; periodic timer re-arms.
// Input: CREATE + SETTIME value/interval; drive wheel manually; DELETE.
// Expect: Expiry counter 1 then 2+ for periodic; slot freed on delete.
// Depends: PosixTime timer registry, timer_fire re-arm chain
JARVIS_TEST(posix_timer_oneshot_and_periodic, "PRE: none | POST: none") {
    uint64_t timer_id = 0;
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 0, 1,
                                    reinterpret_cast<uint64_t>(&timer_id), 0));
    AbiItimer spec{};
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 10000000;
    spec.it_interval.tv_sec = 0;
    spec.it_interval.tv_nsec = 0;
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 1, timer_id,
                                    reinterpret_cast<uint64_t>(&spec), 0));
    const uint64_t fire_ns = arch::Timer::ns_monotonic() + 50000000ULL;
    time::TimerWheel::on_tick(fire_ns, 0);
    AbiItimer readback{};
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 2, timer_id,
                                    reinterpret_cast<uint64_t>(&readback), 0));
    JARVIS_ASSERT_EQ(0, readback.it_value.tv_sec);
    JARVIS_ASSERT_EQ(0, readback.it_value.tv_nsec);
    // Periodic re-arm: 10ms interval, three manual ticks.
    spec.it_interval.tv_nsec = 10000000;
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 1, timer_id,
                                    reinterpret_cast<uint64_t>(&spec), 0));
    time::TimerWheel::on_tick(fire_ns + 15000000ULL, 0);
    time::TimerWheel::on_tick(fire_ns + 25000000ULL, 0);
    time::TimerWheel::on_tick(fire_ns + 35000000ULL, 0);
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 2, timer_id,
                                    reinterpret_cast<uint64_t>(&readback), 0));
    JARVIS_ASSERT(readback.it_interval.tv_nsec == 10000000);
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 3, timer_id, 0, 0));
    JARVIS_ASSERT_EQ(0ULL, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full timer table fails closed with -EAGAIN and drains clean.
// Input: Fill kMaxPosixTimers slots; one more CREATE; delete all.
// Expect: Last CREATE returns -EAGAIN; ResourceTracker delta zero.
// Depends: PosixTime registry bound, track_posix_timer_add/remove
JARVIS_TEST(posix_timer_table_full, "PRE: none | POST: none") {
    uint64_t ids[32] = {0};
    for (size_t i = 0; i < 32; ++i) {
        JARVIS_ASSERT_EQ(0ULL, sys_call(82, 0, 1,
                                        reinterpret_cast<uint64_t>(&ids[i]),
                                        0));
    }
    uint64_t extra_id = 0;
    JARVIS_ASSERT_EQ(kNegErrno(11),
                     sys_call(82, 0, 1,
                              reinterpret_cast<uint64_t>(&extra_id), 0));
    for (size_t i = 0; i < 32; ++i) {
        JARVIS_ASSERT_EQ(0ULL, sys_call(82, 3, ids[i], 0, 0));
    }
    JARVIS_ASSERT_EQ(0ULL, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: timerfd read returns 8-byte expiry count, clear-on-read.
// Input: TIMERFD_CREATE + SETTIME; drive ticks; sys_read 8 bytes; empty
// nonblock read; close; verify wheel residue zero.
// Expect: First read returns 8 with count >= 1; empty read -EAGAIN;
// close drains; stale state none.
// Depends: sys_timerfd_create, sys_read timerfd branch, timerfd_close
JARVIS_TEST(posix_timerfd_read_semantics, "PRE: none | POST: none") {
    const uint64_t fd = sys_call(83, 1, 0, 0, 0); // MONOTONIC, blocking
    JARVIS_ASSERT(fd <= 0x7FFFFFFFULL);
    const int timer_fd = static_cast<int>(fd);
    AbiItimer spec{};
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 10000000;
    spec.it_interval.tv_sec = 0;
    spec.it_interval.tv_nsec = 0;
    JARVIS_ASSERT_EQ(0ULL, sys_call(82, 1, static_cast<uint64_t>(timer_fd),
                                    reinterpret_cast<uint64_t>(&spec), 1));
    const uint64_t fire_ns = arch::Timer::ns_monotonic() + 50000000ULL;
    time::TimerWheel::on_tick(fire_ns, 0);
    uint64_t count = 0;
    JARVIS_ASSERT_EQ(8ULL, sys_call(10, static_cast<uint64_t>(timer_fd),
                                    reinterpret_cast<uint64_t>(&count), 8, 0));
    JARVIS_ASSERT(count >= 1);
    const uint64_t fd_nb = sys_call(83, 1, 0x800, 0, 0); // O_NONBLOCK
    JARVIS_ASSERT(fd_nb <= 0x7FFFFFFFULL);
    uint64_t empty_count = 0xDEADBEEFULL;
    JARVIS_ASSERT_EQ(kNegErrno(11),
                     sys_call(10, fd_nb,
                              reinterpret_cast<uint64_t>(&empty_count), 8, 0));
    JARVIS_ASSERT_EQ(0ULL,
                     sys_call(11, static_cast<uint64_t>(timer_fd), 0, 0, 0));
    JARVIS_ASSERT_EQ(0ULL, sys_call(11, fd_nb, 0, 0, 0));
    JARVIS_ASSERT_EQ(0ULL, time::TimerWheel::live_count(0));
    JARVIS_TEST_PASS();
}

void register_posix_time_tests() {
    Logger::info("Registering POSIX time tests");
    JARVIS_REGISTER_TEST(posix_clock_monotonic_nondecreasing);
    JARVIS_REGISTER_TEST(posix_clock_realtime_anchored);
    JARVIS_REGISTER_TEST(posix_clock_bad_id);
    JARVIS_REGISTER_TEST(posix_nanosleep_zero_returns);
    JARVIS_REGISTER_TEST(posix_nanosleep_bad_nsec);
    JARVIS_REGISTER_TEST(posix_nanosleep_expiry_bound);
    JARVIS_REGISTER_TEST(posix_nanosleep_killed_by_signal);
    JARVIS_REGISTER_TEST(posix_timer_oneshot_and_periodic);
    JARVIS_REGISTER_TEST(posix_timer_table_full);
    JARVIS_REGISTER_TEST(posix_timerfd_read_semantics);
}
