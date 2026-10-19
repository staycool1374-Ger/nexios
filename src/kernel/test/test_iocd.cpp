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

/// @file test_iocd.cpp
/// @brief I/O completion detection tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/daemon/daemon_mgr.hpp>

using namespace kernel;

namespace {

// Raw iocd round-trip over live IPC: sends type with (pid, bar) args,
// returns the daemon's reply result, or -99 on transport failure (which
// fails every == -1/== 0 assertion below with a visible value).
int64_t iocd_policy_roundtrip(uint64_t type, uint64_t pid, uint64_t bar) {
    auto *cur = Scheduler::current_task();
    if (cur == nullptr)
        return -99;
    const uint64_t iocd_pid = iocd::get_iocd_pid();
    if (iocd_pid == 0)
        return -99;
    iocd::Msg req{};
    req.type = type;
    req.args[0] = pid;
    req.args[1] = bar;
    Message send{};
    send.sender_id = cur->id;
    send.type = type;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    Message reply{};
    if (!IPC::send_sync(iocd_pid, send, reply))
        return -99;
    if (reply.data_size < sizeof(iocd::Reply))
        return -99;
    iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    return rpl.result;
}

} // namespace

// Runmode: kernel
// Testidea: IOCD boots and registers with the kernel
// Input: None (boot sequence)
// Expect: iocd::get_iocd_pid() returns a non-zero PID and the task exists
// Depends: kernel/driver/iocd
JARVIS_TEST(iocd_boots_and_registers, "PRE: vfsd, iocd | POST: iocd") {
    uint64_t pid = iocd::get_iocd_pid();
    Logger::info("[TEST:iocd_boots_and_registers] pid=%u vfsd_pid=%u", pid,
                 vfsd::get_vfsd_pid());
    JARVIS_ASSERT(pid != 0);
    uint64_t vfsd_pid = vfsd::get_vfsd_pid();
    JARVIS_ASSERT(vfsd_pid != 0);
    auto *vfsd_task = Scheduler::find_task(vfsd_pid);
    JARVIS_ASSERT(vfsd_task != nullptr);
    auto *task = Scheduler::find_task(pid);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Keyboard IRQ is converted to an IPC event
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_keyboard_irq_to_event, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Serial IRQ is converted to an IPC event
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_serial_irq_to_event, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MMIO grant lifecycle against the live daemon (issue #303
//           item 4): register → MAP → reuse → UNMAP → UNMAP-fails →
//           UNREGISTER (device exit drops grants) → MAP/UNMAP fail.
// Input: harness registers itself ("test-mmio"), MAPs bar 0 over live
//        IPC, releases, unregisters. State collected before asserts;
//        the unregister restores the daemon (zero slot/record residue).
// Expect: register 0; MAP 0; re-MAP 0 (idempotent reuse); UNMAP 0;
//         second UNMAP -1; UNREGISTER 0; post-exit MAP -1, UNMAP -1.
// Depends: iocd MMIO policy handler + UNREGISTER_DEVICE + live IPC.
JARVIS_TEST(iocd_mmio_unmap_on_exit, "PRE: vfsd, iocd | POST: iocd") {
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT(me != nullptr);
    const uint64_t pid = me->id;

    // Register self (slot freed by the UNREGISTER leg below).
    iocd::Msg reg{};
    reg.type = iocd::IOCD_REGISTER_DEVICE;
    reg.args[0] = pid;
    reg.args[1] = 0;
    __builtin_memcpy(&reg.args[2], "test-mmio", 10);
    Message sreg{};
    sreg.sender_id = pid;
    sreg.type = iocd::IOCD_REGISTER_DEVICE;
    sreg.data_size = sizeof(reg);
    __builtin_memcpy(sreg.data, &reg, sizeof(reg));
    Message rreg{};
    JARVIS_ASSERT(IPC::send_sync(iocd::get_iocd_pid(), sreg, rreg));
    iocd::Reply reg_reply{};
    __builtin_memcpy(&reg_reply, rreg.data, sizeof(reg_reply));
    JARVIS_ASSERT_FMT(reg_reply.result == 0, "register failed: %ld",
                      reg_reply.result);

    const int64_t map_first =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_MAP, pid, 0);
    const int64_t map_reuse =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_MAP, pid, 0);
    const int64_t unmap_once =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_UNMAP, pid, 0);
    const int64_t unmap_again =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_UNMAP, pid, 0);
    const int64_t unreg =
        iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE, pid, 0);
    const int64_t map_after_exit =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_MAP, pid, 0);
    const int64_t unmap_after_exit =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_UNMAP, pid, 0);

    JARVIS_ASSERT_FMT(map_first == 0, "MAP failed: %ld", map_first);
    JARVIS_ASSERT_FMT(map_reuse == 0, "re-MAP failed: %ld", map_reuse);
    JARVIS_ASSERT_FMT(unmap_once == 0, "UNMAP failed: %ld", unmap_once);
    JARVIS_ASSERT_FMT(unmap_again == -1, "second UNMAP not -1: %ld",
                      unmap_again);
    JARVIS_ASSERT_FMT(unreg == 0, "UNREGISTER failed: %ld", unreg);
    JARVIS_ASSERT_FMT(map_after_exit == -1, "post-exit MAP not -1: %ld",
                      map_after_exit);
    JARVIS_ASSERT_FMT(unmap_after_exit == -1, "post-exit UNMAP not -1: %ld",
                      unmap_after_exit);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: MMIO policy fail-closed against the live daemon (issue #303
//           item 4): out-of-range bar, record-less UNMAP, and MAP after
//           the pid leaves the registry all return -1 with zero daemon
//           state change. NOTE: args[0] is always the live harness pid —
//           the daemon replies to args[0], never to the IPC sender, so a
//           bogus args[0] would strand this task's send_sync (documented
//           in iocd.hpp; same self-asserted trust model as KEYBOARD_READ).
// Input: MAP bar 0xFF; UNMAP bar 0 (no record); REGISTER self →
//        UNREGISTER self → MAP bar 0 (unregistered again).
// Expect: all replies -1 except the register/unregister pair (0/0); the
//         transient registration is removed — pollution-free.
// Depends: iocd MMIO policy handler + live IPC.
JARVIS_TEST(iocd_unauthorized_mmio_rejected, "PRE: vfsd, iocd | POST: iocd") {
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT(me != nullptr);
    const uint64_t pid = me->id;

    const int64_t bad_bar =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_MAP, pid, 0xFF);
    const int64_t no_record =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_UNMAP, pid, 0);

    // Transient registration round-trip (slot freed immediately).
    iocd::Msg reg{};
    reg.type = iocd::IOCD_REGISTER_DEVICE;
    reg.args[0] = pid;
    reg.args[1] = 0;
    __builtin_memcpy(&reg.args[2], "test-rej", 9);
    Message sreg{};
    sreg.sender_id = pid;
    sreg.type = iocd::IOCD_REGISTER_DEVICE;
    sreg.data_size = sizeof(reg);
    __builtin_memcpy(sreg.data, &reg, sizeof(reg));
    Message rreg{};
    int64_t reg_result = -99;
    if (IPC::send_sync(iocd::get_iocd_pid(), sreg, rreg) &&
        rreg.data_size >= sizeof(iocd::Reply)) {
        iocd::Reply rr{};
        __builtin_memcpy(&rr, rreg.data, sizeof(rr));
        reg_result = rr.result;
    }
    const int64_t unreg =
        iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE, pid, 0);
    const int64_t map_after_exit =
        iocd_policy_roundtrip(iocd::IOCD_MMIO_MAP, pid, 0);

    JARVIS_ASSERT_FMT(bad_bar == -1, "out-of-range MAP not -1: %ld", bad_bar);
    JARVIS_ASSERT_FMT(no_record == -1, "record-less UNMAP not -1: %ld",
                      no_record);
    JARVIS_ASSERT_FMT(reg_result == 0, "register failed: %ld", reg_result);
    JARVIS_ASSERT_FMT(unreg == 0, "UNREGISTER failed: %ld", unreg);
    JARVIS_ASSERT_FMT(map_after_exit == -1, "post-exit MAP not -1: %ld",
                      map_after_exit);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Multiple device handlers registered with IOCD
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_multiple_device_handlers, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - IRQ affinity routing works correctly
// Input: None (stub test)
// Expect: Passes (stub)
// Depends: kernel/task, kernel/ipc, kernel/driver
JARVIS_TEST(iocd_irq_affinity, "PRE: none | POST: iocd") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: IOCD restarts after crash (TERMINATED → cleanup → respawn)
// Input: Kill iocd task (set TERMINATED), reap, then restart_stale_daemons
// Expect: New iocd PID is non-zero and different from original; new task exists
// Depends: kernel/task, kernel/ipc, kernel/iocd, kernel/daemon
// DISABLED: deliberately kills daemon manually
#if 0
JARVIS_TEST(iocd_crash_restarts) {
    uint64_t old_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(old_pid != 0);

    auto* old_task = Scheduler::find_task(old_pid);
    JARVIS_ASSERT(old_task != nullptr);

    old_task->state = TaskState::TERMINATED;
    Scheduler::reap_orphans();
    JARVIS_ASSERT_EQ(0ULL, iocd::get_iocd_pid());

    daemon::restart_stale_daemons();

    uint64_t new_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(new_pid != 0);
    JARVIS_ASSERT(new_pid != old_pid);

    auto* new_task = Scheduler::find_task(new_pid);
    JARVIS_ASSERT(new_task != nullptr);
    JARVIS_ASSERT(new_task->state == TaskState::READY ||
                  new_task->state == TaskState::RUNNING);

    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Registers all IOCD tests with the test framework
// Input: None
// Expect: All IOCD tests registered
// Depends: test framework
void register_iocd_tests() {
    Logger::info("Registering I/O daemon tests");
    JARVIS_REGISTER_TEST(iocd_boots_and_registers);
    JARVIS_REGISTER_TEST(iocd_keyboard_irq_to_event);
    JARVIS_REGISTER_TEST(iocd_serial_irq_to_event);
    JARVIS_REGISTER_TEST(iocd_mmio_unmap_on_exit);
    JARVIS_REGISTER_TEST(iocd_unauthorized_mmio_rejected);
    JARVIS_REGISTER_TEST(iocd_multiple_device_handlers);
    JARVIS_REGISTER_TEST(iocd_irq_affinity);
    // DISABLED: deliberately kills daemon manually, leaving daemon lifecycle
    // in a non-standard state that snapshot_restore cannot clean up
    // JARVIS_REGISTER_TEST(iocd_crash_restarts);
}
