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

/// @file test_driver.cpp
/// @brief Driver framework tests.

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <kernel/driver/driver.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/cap_types.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/arch/serial.hpp>
#include <kernel/arch/keyboard.hpp>

using namespace kernel;

namespace {

/// @brief Single IOCD request/response from a dispatched kernel task
///        (fixture-safe: the daemon replies to args[0], so the caller
///        passes its own live pid; a bogus args[0] would strand the
///        send_sync). Returns the reply result, or -99 on transport
///        failure. data0_out is filled when non-null.
int64_t iocd_roundtrip(uint64_t self_pid, uint64_t type, uint64_t arg1,
                       uint64_t *data0_out) {
    const uint64_t iocd_pid = kernel::iocd::get_iocd_pid();
    if (iocd_pid == 0)
        return -99;
    kernel::iocd::Msg req{};
    req.type = type;
    req.args[0] = self_pid;
    req.args[1] = arg1;
    kernel::Message send{};
    send.sender_id = self_pid;
    send.type = type;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    kernel::Message reply{};
    if (!kernel::IPC::send_sync(iocd_pid, send, reply))
        return -99;
    if (reply.data_size < sizeof(kernel::iocd::Reply))
        return -99;
    kernel::iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    if (data0_out != nullptr)
        *data0_out = rpl.data[0];
    return rpl.result;
}

} // namespace

// Runmode: kernel
// Testidea: Tests that an existing driver ("keyboard") can be found by name
// and that a nonexistent driver returns nullptr
// Input: "keyboard" (existing driver name), "nonexistent_driver_xyz"
// (non-existing driver name)
// Expect: DriverRegistry::find("keyboard") returns a non-null pointer with
// name matching "keyboard"; find("nonexistent_driver_xyz") returns nullptr
// Depends: DriverRegistry
JARVIS_TEST(driver_registry_find, "PRE: iocd | POST: none") {
    auto *drv = DriverRegistry::find("keyboard");
    JARVIS_ASSERT(drv != nullptr);
    JARVIS_ASSERT_EQ(0, strcmp(drv->name, "keyboard"));
    drv = DriverRegistry::find("nonexistent_driver_xyz");
    JARVIS_ASSERT(drv == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the driver registry contains at least 4 drivers and
// that both "keyboard" and "timer" are present
// Input: DriverRegistry::count() and DriverRegistry::get(i) for each index
// 0..cnt-1
// Expect: count >= 4, has_kbd == true, has_timer == true
// Depends: DriverRegistry
JARVIS_TEST(driver_registry_count, "PRE: iocd | POST: none") {
    size_t cnt = DriverRegistry::count();
    JARVIS_ASSERT(cnt >= 4);
    bool has_kbd = false, has_timer = false;
    for (size_t i = 0; i < cnt; ++i) {
        auto *d = DriverRegistry::get(i);
        if (d) {
            if (strcmp(d->name, "keyboard") == 0)
                has_kbd = true;
            if (strcmp(d->name, "timer") == 0)
                has_timer = true;
        }
    }
    JARVIS_ASSERT(has_kbd);
    JARVIS_ASSERT(has_timer);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the IOCD server boots and registers its PID, mirroring
// the vfsd boot pin: the recorded PID is non-zero and resolves to a live
// task. Fails loud if the daemon never registered (fail-closed boot).
// Input: iocd::get_iocd_pid() + Scheduler::find_task(pid)
// Expect: pid != 0 and the task lookup succeeds
// Depends: IOCD server, Scheduler
JARVIS_TEST(iocd_server_boots, "PRE: iocd | POST: none") {
    uint64_t pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(pid != 0);
    auto *task = Scheduler::find_task(pid);
    JARVIS_ASSERT(task != nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Keyboard path integrated in IOCD end-to-end over live IPC: a
// REAL dispatched kernel task drains the daemon byte queue, then proves the
// empty-queue contract (result 0 + data 0). Serve-path proof (the daemon
// responds, never blocks); decode tables are pinned separately by
// hal_keyboard_decode (24 tests), so this test owns the IPC round-trip.
// Input: dispatched task drives IOCD_KEYBOARD_READ until empty, then one
// final read; harness joins and asserts
// Expect: drain terminates with result 0; final read result 0, data[0] 0;
// zero tracker delta (harness)
// Depends: IOCD server, live IPC, Scheduler
JARVIS_TEST(keyboard_driver_in_iocd, "PRE: iocd | POST: none") {
    static int64_t g_kbd_drain_result = -99;
    static uint64_t g_kbd_drain_data = 1;
    static int64_t g_kbd_final_result = -99;
    static uint64_t g_kbd_final_data = 1;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            uint64_t data = 0;
            int64_t last = -99;
            for (int i = 0; i < 64; ++i) {
                last = iocd_roundtrip(self, iocd::IOCD_KEYBOARD_READ, 0,
                                      &data);
                if (last != 1)
                    break;
            }
            g_kbd_drain_result = last;
            g_kbd_drain_data = data;
            g_kbd_final_result =
                iocd_roundtrip(self, iocd::IOCD_KEYBOARD_READ, 0, &data);
            g_kbd_final_data = data;
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    kernel::test::terminate_if_live(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_FMT(g_kbd_drain_result == 0, "drain end: %ld",
                      g_kbd_drain_result);
    JARVIS_ASSERT_EQ(0ULL, g_kbd_drain_data);
    JARVIS_ASSERT_FMT(g_kbd_final_result == 0, "empty read: %ld",
                      g_kbd_final_result);
    JARVIS_ASSERT_EQ(0ULL, g_kbd_final_data);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Serial path via IOCD fails closed: the daemon's serial handlers
// are unimplemented stubs that deny every request. The test pins the
// fail-closed contract (never silent success, never a hang) from a REAL
// dispatched kernel task over live IPC.
// Input: dispatched task drives IOCD_SERIAL_READ + IOCD_SERIAL_WRITE;
// harness joins and asserts
// Expect: both replies arrive with result -1
// Depends: IOCD server, live IPC, Scheduler
JARVIS_TEST(serial_driver_in_iocd, "PRE: iocd | POST: none") {
    static int64_t g_ser_read = -99;
    static int64_t g_ser_write = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            g_ser_read = iocd_roundtrip(self, iocd::IOCD_SERIAL_READ, 0,
                                        nullptr);
            g_ser_write = iocd_roundtrip(self, iocd::IOCD_SERIAL_WRITE, 0,
                                         nullptr);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    kernel::test::terminate_if_live(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_FMT(g_ser_read == -1, "serial read: %ld", g_ser_read);
    JARVIS_ASSERT_FMT(g_ser_write == -1, "serial write: %ld", g_ser_write);
    JARVIS_TEST_PASS();
}

namespace {

/// @brief Daemon round-trip from any kernel task context (fixture-safe:
///        takes the sender pid explicitly since the daemon replies to
///        args[0]). Returns the reply result, or -99 on transport failure.
int64_t drv_mmio_roundtrip(uint64_t self_pid, uint64_t type, uint64_t bar) {
    const uint64_t iocd_pid = kernel::iocd::get_iocd_pid();
    if (iocd_pid == 0)
        return -99;
    kernel::iocd::Msg req{};
    req.type = type;
    req.args[0] = self_pid;
    req.args[1] = bar;
    kernel::Message send{};
    send.sender_id = self_pid;
    send.type = type;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    kernel::Message reply{};
    if (!kernel::IPC::send_sync(iocd_pid, send, reply))
        return -99;
    if (reply.data_size < sizeof(kernel::iocd::Reply))
        return -99;
    kernel::iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    return rpl.result;
}

uint64_t g_drv_phys = 0;
uint64_t g_drv_map_va = 0;
uint64_t g_drv_sys_unmap = 0;
int64_t g_drv_reg = -99;
int64_t g_drv_map = -99;
int64_t g_drv_unmap = -99;
int64_t g_drv_unreg = -99;

} // namespace

// Runmode: kernel
// Testidea: Driver MMIO end-to-end across both layers (issue #303 item
//           4): a fixture driver registers with the live iocd, gets a
//           policy grant for bar 0, installs a REAL MEMORY MmioCap for a
//           PMM test page, maps it via SYS_MMIO_MAP in its own cspace,
//           then unwinds everything (UNMAP, daemon UNMAP, UNREGISTER).
//           The daemon authorizes WHO/WHICH (policy); the kernel
//           enforces WHAT via caps — no cap passes through the daemon
//           (documented split; kernel PTE teardown stays with drain_task).
// Input: dispatched fixture task drives register → daemon-MAP → cap
//        install → SYS_MMIO_MAP → SYS_MMIO_UNMAP → daemon-UNMAP →
//        UNREGISTER; harness joins and asserts.
// Expect: all daemon results 0; VA in the user MMIO window; sys unmap 0;
//         MmioUserMap live_count 0; zero tracker delta (harness).
// Depends: iocd policy handler, MmioCap, MmioUserMap, live IPC.
JARVIS_TEST(driver_server_mmio_via_caps, "PRE: iocd | POST: none") {
    g_drv_phys = PMM::alloc_user_page();
    JARVIS_ASSERT(g_drv_phys != 0);
    g_drv_map_va = 0;
    g_drv_sys_unmap = 0;
    g_drv_reg = -99;
    g_drv_map = -99;
    g_drv_unmap = -99;
    g_drv_unreg = -99;

    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            const uint64_t self = cur ? cur->id : 0;
            // Policy leg: register + grant over live IPC.
            iocd::Msg reg{};
            reg.type = iocd::IOCD_REGISTER_DEVICE;
            reg.args[0] = self;
            reg.args[1] = 0;
            __builtin_memcpy(&reg.args[2], "drv-mmio", 9);
            Message sreg{};
            sreg.sender_id = self;
            sreg.type = iocd::IOCD_REGISTER_DEVICE;
            sreg.data_size = sizeof(reg);
            __builtin_memcpy(sreg.data, &reg, sizeof(reg));
            Message rreg{};
            if (IPC::send_sync(iocd::get_iocd_pid(), sreg, rreg) &&
                rreg.data_size >= sizeof(iocd::Reply)) {
                iocd::Reply rr{};
                __builtin_memcpy(&rr, rreg.data, sizeof(rr));
                g_drv_reg = rr.result;
            }
            g_drv_map = drv_mmio_roundtrip(self, iocd::IOCD_MMIO_MAP, 0);
            // Mechanism leg: real cap + syscall dispatch in own cspace.
            if (g_drv_reg == 0 && g_drv_map == 0 && cur != nullptr) {
                cur->ensure_cspace();
                cap::CNode *cs = cur->get_cspace();
                auto *mmio = cap::MmioCap::create(
                    g_drv_phys, arch::PAGE_SIZE,
                    arch::PciBarType::MEMORY_32);
                if (mmio != nullptr) {
                    int s = cs->install(mmio, cap::CapType::Mmio,
                                        cap::CapRights::WRITE);
                    if (s >= 0) {
                        uint64_t h = cap::encode_handle(
                            cs->cspace_id, static_cast<uint32_t>(s),
                            cs->slot_gen(static_cast<uint32_t>(s)));
                        g_drv_map_va = Syscall::handle(
                            static_cast<uint64_t>(SyscallNumber::MMIO_MAP),
                            h, 0, 0, 0, nullptr);
                        if (g_drv_map_va != static_cast<uint64_t>(-1)) {
                            g_drv_sys_unmap = Syscall::handle(
                                static_cast<uint64_t>(
                                    SyscallNumber::MMIO_UNMAP),
                                g_drv_map_va, 0, 0, 0, nullptr);
                        }
                        cs->remove(static_cast<uint32_t>(s));
                    }
                    mmio->release();
                }
            }
            g_drv_unmap =
                drv_mmio_roundtrip(self, iocd::IOCD_MMIO_UNMAP, 0);
            g_drv_unreg = drv_mmio_roundtrip(
                self, iocd::IOCD_UNREGISTER_DEVICE, 0);
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->get_page_table() != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_FMT(g_drv_reg == 0, "register: %ld", g_drv_reg);
    JARVIS_ASSERT_FMT(g_drv_map == 0, "daemon MAP: %ld", g_drv_map);
    JARVIS_ASSERT_FMT(g_drv_map_va != static_cast<uint64_t>(-1),
                      "SYS_MMIO_MAP failed");
    JARVIS_ASSERT_FMT(
        g_drv_map_va >= CONFIG_USER_MMIO_VA_BASE,
        "VA 0x%lx outside user MMIO window", g_drv_map_va);
    JARVIS_ASSERT_FMT(g_drv_sys_unmap == 0, "SYS_MMIO_UNMAP: %lu",
                      g_drv_sys_unmap);
    JARVIS_ASSERT_FMT(g_drv_unmap == 0, "daemon UNMAP: %ld", g_drv_unmap);
    JARVIS_ASSERT_FMT(g_drv_unreg == 0, "UNREGISTER: %ld", g_drv_unreg);
    JARVIS_ASSERT_EQ(static_cast<size_t>(0), cap::MmioUserMap::live_count());
    PMM::free_page(g_drv_phys);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-08 — Serial::puts/putchar return promptly on a live COM1
//           (the bounded THRE polls must not hang and must emit).
// Input: Serial::puts with newline + mixed case + digits
// Expect: returns without hanging
// Depends: arch::Serial
JARVIS_TEST(serial_putchar_bounded_ok, "PRE: iocd | POST: none") {
    arch::Serial::puts("test-driver\n");
    arch::Serial::puts("ABCabc0129\n");
    arch::Serial::putchar('X');
    arch::Serial::putchar('\n');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: FLAW-10 — Keyboard::init() must return (bounded PS/2 drain) and
//           leave the ring empty with modifiers cleared.
// Input: Keyboard::init(); getchar into a buffer; check mods
// Expect: returns; ring empty (getchar false); mods cleared
// Depends: arch::Keyboard
JARVIS_TEST(keyboard_init_bounded_drain, "PRE: iocd | POST: none") {
    arch::Keyboard::init();
    char c;
    bool got = arch::Keyboard::getchar(c);
    JARVIS_ASSERT(!got); // ring drained by init
    JARVIS_ASSERT(!arch::Keyboard::is_shifted());
    JARVIS_ASSERT(!arch::Keyboard::is_ctrl());
    JARVIS_ASSERT(!arch::Keyboard::is_alt());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all driver tests with the test framework
// Input: None
// Expect: All driver tests are registered (no direct assertion, only
// logging)
// Depends: Test framework, Logger, Scheduler, DriverRegistry
void register_driver_tests() {
    Logger::info("Registering driver tests");
    JARVIS_REGISTER_TEST(driver_registry_find);
    JARVIS_REGISTER_TEST(driver_registry_count);
    JARVIS_REGISTER_TEST(iocd_server_boots);
    JARVIS_REGISTER_TEST(keyboard_driver_in_iocd);
    JARVIS_REGISTER_TEST(serial_driver_in_iocd);
    JARVIS_REGISTER_TEST(driver_server_mmio_via_caps);
    JARVIS_REGISTER_TEST(serial_putchar_bounded_ok);
    JARVIS_REGISTER_TEST(keyboard_init_bounded_drain);
}
