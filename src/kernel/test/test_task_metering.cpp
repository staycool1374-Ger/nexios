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

/// @file test_task_metering.cpp
/// @brief Per-task execution-time metering tests (issue #21, v0.4.8).
///
/// DRIVEN: every property is reached through REAL dispatched tasks on real
/// timer ticks — the harness never calls Scheduler::on_tick() to fake time
/// and never mutates task accounting fields (v0.3.10 rework discipline).
/// The SYS_TIMES copy-out path is driven through a REAL Ring-3 probe task
/// (issue #143 fixture pattern), x86_64 only.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/nexios_config.h>
#include "test_sched_helpers.hpp"

using namespace kernel;

namespace {

/// @brief Create a real kernel task, dispatch it, and wait for genuine
///        termination.  Returns the TCB for post-mortem reads + cleanup.
static TaskControlBlock *run_meter_task(void (*entry)(), uint64_t prio = 11,
                                        uint64_t period = 10) {
    auto *t = TaskControlBlock::create(entry, prio, period);
    if (t == nullptr)
        return nullptr;
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    return t;
}

/// @brief Release a completed task TCB (mirrors test_ipc_blocking.cpp).
static void release_meter_task(TaskControlBlock *t) {
    if (t == nullptr)
        return;
    kernel::test::terminate_if_live(t);
}

#if defined(CONFIG_ARCH_X86_64)
// ---------------------------------------------------------------------------
// Ring-3 TIMES probe fixture (issue #143 pattern, TIMES-shaped).
//
// A REAL Ring-3 task performs two real int $0x80 TIMES calls against a REAL
// private PML4, so safe_copy_to_user takes the user-task path with ambient
// tables that actually map the probed pages.  Struct slots live in the data
// page at caller-chosen offsets (arg1 is full 64-bit).
// ---------------------------------------------------------------------------
/// @brief Code VA: the auto-installed yield-stub page, content replaced.
constexpr uint64_t kMeterProbeCodeVa = 0x40000000ULL;
/// @brief Data VA: fresh user page mapped by the fixture (RW, NX).
constexpr uint64_t kMeterProbeDataVa = 0x50001000ULL;
/// @brief Bounded-join budget (converted to FAIL, never a hang).
constexpr uint64_t kMeterProbeJoinSpins = 10000000ULL;
/// @brief Fixture priority: above the harness so ticks dispatch it.
constexpr uint64_t kMeterProbePrio = 20;

/// @brief Emit one (mov edx,data; mov eax,num; mov ebx,arg0; movabs
///        rcx,arg1; int $0x80; mov [rdx+disp],rax) call sequence at @p code.
static uint8_t *emit_meter_syscall(uint8_t *code, uint32_t num, uint32_t arg0,
                                   uint64_t arg1, int8_t ret_disp) {
    *code++ = 0xBA;
    uint32_t data_lo = static_cast<uint32_t>(kMeterProbeDataVa);
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

/// @brief Run two TIMES calls from a REAL dispatched user task; read back
///        the return slots + data page.
/// @return true if the probe reached EXIT(0) with the data page intact.
static bool run_meter_probe(uint32_t arg0_a, uint64_t arg1_a, uint32_t arg0_b,
                            uint64_t arg1_b, uint64_t *out_ret_a,
                            uint64_t *out_ret_b, uint8_t *out_data) {
    constexpr uint32_t k_times =
        static_cast<uint32_t>(SyscallNumber::TIMES);
    auto *fixture = TaskControlBlock::create_user(kernel::test::forever_entry,
                                                  kMeterProbePrio, 10, 32768);
    if (!fixture) {
        return false;
    }
    uint64_t data_phys = PMM::alloc_user_page();
    if (data_phys) {
        VMM::map_page_in_pml4(kMeterProbeDataVa, data_phys, true, false,
                              fixture->page_table_);
    }
    uint64_t code_phys = VMM::virt_to_phys_in_pml4(kMeterProbeCodeVa,
                                                   fixture->page_table_);
    uint64_t data_check = VMM::virt_to_phys_in_pml4(kMeterProbeDataVa,
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
    code = emit_meter_syscall(code, k_times, arg0_a, arg1_a, 0);
    code = emit_meter_syscall(code, k_times, arg0_b, arg1_b, 8);
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
         i < kMeterProbeJoinSpins && fixture->state != TaskState::TERMINATED;
         ++i) {
        arch::pause();
    }
    data_check = VMM::virt_to_phys_in_pml4(kMeterProbeDataVa,
                                           fixture->page_table_);
    bool exited = (fixture->state == TaskState::TERMINATED);
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
    if (TaskControlBlock::is_valid(fixture) &&
        (fixture->state != TaskState::TERMINATED ||
         fixture->exit_code == 0)) {
        Scheduler::terminate(*fixture, 0);
    }
    Scheduler::drain_zombie_list();
    return observed && clean_exit;
}

/// @brief Little-endian u64 load from a byte buffer at @p off.
static uint64_t meter_load_u64(const uint8_t *buf, uint64_t off) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(buf[off + i]) << (i * 8);
    return v;
}
#endif // CONFIG_ARCH_X86_64

} // namespace

// Runmode: kernel
// Testidea: a task that runs accumulates exec_ns_total across switch-out.
// Input: Driven task spins ~2ms + waits 3 real ticks, then samples self.
// Expect: exec_ns_total > 0; exec_period_ns == exec_ns_total (period 10,
//         no reload fired yet).
// Depends: Scheduler::charge on tick + switch-out (issue #21)
JARVIS_TEST(meter_switch_delta_accumulates, "PRE: none | POST: none") {
    static volatile uint64_t g_total = 0;
    static volatile uint64_t g_period = 0;

    auto *t = run_meter_task([]() {
        auto *self = Scheduler::current_task();
        const uint64_t t0 = arch::Timer::ns_monotonic();
        while (arch::Timer::ns_monotonic() - t0 < 2000000ULL)
            arch::pause();
        while (self->executed_ticks < 3)
            arch::pause();
        g_total = self->exec_ns_total;
        g_period = self->exec_period_ns;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_total > 0);
    JARVIS_ASSERT_EQ(g_total, g_period);
    release_meter_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: ns-resolution twin of timer_ready_task_accumulates_no_cpu_time.
// Input: Parked prio-1 READY task (never dispatched) + real ticks elapse.
// Expect: parked exec_ns_total == 0 and exec_period_ns == 0.
// Depends: #154 discipline extended to ns charge (issue #21)
JARVIS_TEST(meter_ready_task_accumulates_no_ns, "PRE: none | POST: none") {
    auto *parked = TaskControlBlock::create([]() {}, 1,
                                            TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(parked != nullptr);
    JARVIS_ASSERT_EQ(0ULL, parked->exec_ns_total);
    Scheduler::add_task(*parked);

    auto *t = run_meter_task([]() {
        auto *self = Scheduler::current_task();
        while (self->executed_ticks < 3)
            arch::pause();
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, parked->exec_ns_total);
    JARVIS_ASSERT_EQ(0ULL, parked->exec_period_ns);
    release_meter_task(t);
    Scheduler::drain_zombie_list();
    release_meter_task(parked);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: period reload resets the per-period counter, not the total.
// Input: Periodic task (period 2 ticks) runs 6 real ticks.
// Expect: total > 0; period < total (a reload zeroed the period clock
//         while the lifetime total kept every quantum).
// Depends: Scheduler::on_tick period-reload reset (issue #21)
JARVIS_TEST(meter_period_reload_resets_period_keeps_total,
            "PRE: none | POST: none") {
    static volatile uint64_t g_total = 0;
    static volatile uint64_t g_period = 0;

    auto *t = run_meter_task(
        []() {
            auto *self = Scheduler::current_task();
            while (self->executed_ticks < 6)
                arch::pause();
            g_total = self->exec_ns_total;
            g_period = self->exec_period_ns;
        },
        11, 2);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_total > 0);
    JARVIS_ASSERT(g_period < g_total);
    release_meter_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_TIMES on self returns a growing total through Ring-3.
// Input: User probe: TIMES(0, data+16) then TIMES(0, data+48).
// Expect: Probe reaches EXIT(0); both return 0; second total >= first;
//         first total > 0; period <= total.
// Depends: Syscall TIMES + safe_copy_to_user path (issue #21)
JARVIS_TEST(meter_times_self_grows, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    constexpr uint64_t k_slot_a = kMeterProbeDataVa + 16;
    constexpr uint64_t k_slot_b = kMeterProbeDataVa + 48;
    bool ran = run_meter_probe(0, k_slot_a, 0, k_slot_b, &ret_a, &ret_b,
                               data);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(0ULL, ret_a);
    JARVIS_ASSERT_EQ(0ULL, ret_b);
    const uint64_t total_a = meter_load_u64(data, 16);
    const uint64_t period_a = meter_load_u64(data, 24);
    const uint64_t total_b = meter_load_u64(data, 48);
    JARVIS_ASSERT(total_a > 0);
    JARVIS_ASSERT(total_b >= total_a);
    JARVIS_ASSERT(period_a <= total_a);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SYS_TIMES on another pid works; bogus pid fails closed.
// Input: Parked peer task + user probe: TIMES(peer, data+16) then
//        TIMES(0xFFFFFFFE, data+48).
// Expect: First returns 0 with all-zero accounting (peer never ran);
//         second returns -1.
// Depends: Syscall TIMES pid lookup (issue #21)
JARVIS_TEST(meter_times_other_pid_and_esrch, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    auto *peer = TaskControlBlock::create([]() {}, 1,
                                          TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(peer != nullptr);
    Scheduler::add_task(*peer);

    uint64_t ret_a = 0;
    uint64_t ret_b = 0;
    uint8_t data[512] = {};
    constexpr uint64_t k_slot_a = kMeterProbeDataVa + 16;
    constexpr uint64_t k_slot_b = kMeterProbeDataVa + 48;
    constexpr uint32_t k_no_such_pid = 0xFFFFFFFEUL;
    bool ran = run_meter_probe(static_cast<uint32_t>(peer->id), k_slot_a,
                               k_no_such_pid, k_slot_b, &ret_a, &ret_b,
                               data);
    JARVIS_ASSERT(ran);
    JARVIS_ASSERT_EQ(0ULL, ret_a);
    JARVIS_ASSERT_EQ(0ULL, meter_load_u64(data, 16));
    JARVIS_ASSERT_EQ(0ULL, meter_load_u64(data, 24));
    JARVIS_ASSERT_EQ(0ULL, meter_load_u64(data, 32));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), ret_b);
    release_meter_task(peer);
    Scheduler::drain_zombie_list();
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: null user buffer fails closed (fuzz-safety).
// Input: Driven kernel task calls TIMES(0, nullptr) via the FULL path.
// Expect: Returns -1; no fault, harness survives.
// Depends: checked_ptr validation in sys_times (issue #21)
JARVIS_TEST(meter_times_null_buffer_fail_closed, "PRE: none | POST: none") {
    static volatile uint64_t g_ret = 0;

    auto *t = run_meter_task([]() {
        g_ret = Syscall::handle(static_cast<uint64_t>(SyscallNumber::TIMES),
                                0, 0, 0, 0, nullptr);
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-1), g_ret);
    release_meter_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: termination charges the final partial quantum (no loss).
// Input: Task spins, self-samples its total, then exits; harness reads
//        the TCB total post-mortem before drain.
// Expect: Sample > 0; post-mortem total >= sample.
// Depends: terminate/drain charge path (issue #21)
JARVIS_TEST(meter_terminate_charges_final_quantum, "PRE: none | POST: none") {
    static volatile uint64_t g_sample = 0;

    auto *t = run_meter_task([]() {
        auto *self = Scheduler::current_task();
        const uint64_t t0 = arch::Timer::ns_monotonic();
        while (arch::Timer::ns_monotonic() - t0 < 2000000ULL)
            arch::pause();
        while (self->executed_ticks < 3)
            arch::pause();
        g_sample = self->exec_ns_total;
    });
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(g_sample > 0);
    JARVIS_ASSERT(t->exec_ns_total >= g_sample);
    release_meter_task(t);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

void register_task_metering_tests() {
    Logger::info("Registering task_metering tests");
    JARVIS_REGISTER_TEST(meter_switch_delta_accumulates);
    JARVIS_REGISTER_TEST(meter_ready_task_accumulates_no_ns);
    JARVIS_REGISTER_TEST(meter_period_reload_resets_period_keeps_total);
    JARVIS_REGISTER_TEST(meter_times_self_grows);
    JARVIS_REGISTER_TEST(meter_times_other_pid_and_esrch);
    JARVIS_REGISTER_TEST(meter_times_null_buffer_fail_closed);
    JARVIS_REGISTER_TEST(meter_terminate_charges_final_quantum);
}
