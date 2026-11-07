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
#include <kernel/task/boot_sdf.hpp>
#include <kernel/ipc/shm_ring.hpp>

using namespace kernel;

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
// Testidea: STUB - Verifies the IOCD server boots successfully
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, IPC
JARVIS_TEST(iocd_server_boots, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the keyboard driver is integrated in IOCD
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, keyboard driver, IPC
JARVIS_TEST(keyboard_driver_in_iocd, "PRE: iocd | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: STUB - Verifies the serial driver is integrated in IOCD
// Input: None (stub)
// Expect: JARVIS_TEST_PASS() (stub, no actual assertion yet)
// Depends: IOCD server, serial driver, IPC
JARVIS_TEST(serial_driver_in_iocd, "PRE: iocd | POST: none") {
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
// Testidea: SHM TX ring protocol on real frames (issue #51 step-c-prime):
// init a 4-page ring via the HHDM alias, push 64 ordered {len,data}
// elements, pop all back and verify order + payload byte-exact.
// Input: 64 elements with distinct leading bytes
// Expect: all pops succeed in FIFO order with intact payloads
// Depends: PMM contiguous alloc, shm_ring protocol, HHDM alias
JARVIS_TEST(serial_tx_ring_push_drain_order, "PRE: none | POST: none") {
    constexpr size_t kPages = 4;
    const uint64_t phys = PMM::alloc_user_contiguous(kPages);
    JARVIS_ASSERT(phys != 0);
    auto *hdr = reinterpret_cast<kernel::shm::SharedRingHeader *>(
        arch::HHDM_OFFSET + phys);
    uint8_t *data_base = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET +
                                                      phys +
                                                      arch::PAGE_SIZE);
    kernel::shm::shm_ring_init(hdr, kPages,
                               kernel::sdf::kSerialTxRingElemSize);
    JARVIS_ASSERT(kernel::shm::shm_ring_valid(hdr));
    for (uint64_t i = 0; i < 64; ++i) {
        uint8_t elem[kernel::sdf::kSerialTxRingElemSize] = {};
        elem[0] = 7;
        for (uint64_t b = 0; b < 7; ++b) {
            elem[1 + b] = static_cast<uint8_t>((i * 7 + b) & 0xFF);
        }
        JARVIS_ASSERT_FMT(
            kernel::shm::shm_ring_push(hdr, data_base, elem),
            "push %lu failed", i);
    }
    for (uint64_t i = 0; i < 64; ++i) {
        uint8_t elem[kernel::sdf::kSerialTxRingElemSize] = {};
        JARVIS_ASSERT_FMT(
            kernel::shm::shm_ring_pop(hdr, data_base, elem),
            "pop %lu failed", i);
        JARVIS_ASSERT_FMT(elem[0] == 7, "elem %lu len %u", i, elem[0]);
        for (uint64_t b = 0; b < 7; ++b) {
            JARVIS_ASSERT_FMT(
                elem[1 + b] == static_cast<uint8_t>((i * 7 + b) & 0xFF),
                "elem %lu byte %lu mismatch", i, b);
        }
    }
    JARVIS_ASSERT(!kernel::shm::shm_ring_has_data(hdr));
    for (size_t p = 0; p < kPages; ++p) {
        PMM::free_page(phys + p * arch::PAGE_SIZE);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SHM TX ring backpressure (issue #51 step-c-prime): fill the
// ring to capacity, prove the next push fails closed with head/tail
// unmoved, then drain fully and prove empty.
// Input: capacity + 1 pushes on a 4-page ring
// Expect: exactly capacity pushes succeed; the overflow push returns
// false; head/tail identical across the failed push; drain empties
// Depends: PMM contiguous alloc, shm_ring protocol, HHDM alias
JARVIS_TEST(serial_tx_ring_drop_full, "PRE: none | POST: none") {
    constexpr size_t kPages = 4;
    const uint64_t phys = PMM::alloc_user_contiguous(kPages);
    JARVIS_ASSERT(phys != 0);
    auto *hdr = reinterpret_cast<kernel::shm::SharedRingHeader *>(
        arch::HHDM_OFFSET + phys);
    uint8_t *data_base = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET +
                                                      phys +
                                                      arch::PAGE_SIZE);
    kernel::shm::shm_ring_init(hdr, kPages,
                               kernel::sdf::kSerialTxRingElemSize);
    JARVIS_ASSERT(kernel::shm::shm_ring_valid(hdr));
    // shm_ring_init rounds capacity DOWN to a power of two:
    // (4-1) * 4096 = 12288 -> 8192 bytes -> 1024 elements.
    uint64_t raw = 3 * arch::PAGE_SIZE;
    uint64_t cap = 1;
    while (cap * 2 <= raw) {
        cap *= 2;
    }
    const uint64_t n_elem = cap / kernel::sdf::kSerialTxRingElemSize;
    uint8_t elem[kernel::sdf::kSerialTxRingElemSize] = {};
    elem[0] = 1;
    elem[1] = 0xAA;
    for (uint64_t i = 0; i < n_elem; ++i) {
        JARVIS_ASSERT_FMT(
            kernel::shm::shm_ring_push(hdr, data_base, elem),
            "fill push %lu failed", i);
    }
    const uint64_t head_before = hdr->head;
    const uint64_t tail_before = hdr->tail;
    JARVIS_ASSERT(!kernel::shm::shm_ring_push(hdr, data_base, elem));
    JARVIS_ASSERT_FMT(hdr->head == head_before, "head moved on full push");
    JARVIS_ASSERT_FMT(tail_before == hdr->tail, "tail moved on full push");
    uint64_t drained = 0;
    while (kernel::shm::shm_ring_pop(hdr, data_base, elem)) {
        ++drained;
    }
    JARVIS_ASSERT_FMT(drained == n_elem, "drained %lu want %lu", drained,
                      n_elem);
    JARVIS_ASSERT(!kernel::shm::shm_ring_has_data(hdr));
    for (size_t p = 0; p < kPages; ++p) {
        PMM::free_page(phys + p * arch::PAGE_SIZE);
    }
    JARVIS_TEST_PASS();
}

#if CONFIG_SERIAL_TX_SHM
namespace {

uint64_t g_tx_shape_count = 0;
uint64_t g_tx_shape_kind1 = 99;
uint64_t g_tx_shape_size1 = 0;
uint64_t g_tx_compose_ok = 0;
uint64_t g_tx_phys_ok = 0;
uint64_t g_tx_slot_ok = 0;

} // namespace
#endif

// Runmode: kernel
// Testidea: SHM TX SDF row shape (issue #51 step-c-prime): the SERIAL row
// carries the UART cap at index 0 plus exactly one SHM spec at index 1
// sized kSerialTxRingPages.
// Input: sdf_row(SERIAL)
// Expect: count == 2; specs[0].kind == MMIO; specs[1].kind == SHM with
// byte size kSerialTxRingPages * PAGE_SIZE
// Depends: boot_sdf row table (knob-gated; compiled only when the SHM
// TX knob is on — the OFF build has no SHM spec by design)
#if CONFIG_SERIAL_TX_SHM
JARVIS_TEST(serial_tx_sdf_row_shape, "PRE: none | POST: none") {
    kernel::sdf::CapSpec specs[kernel::sdf::kMaxCapsPerRole] = {};
    const uint64_t count =
        kernel::sdf::sdf_row(kernel::sdf::PdRole::SERIAL, specs);
    g_tx_shape_count = count;
    JARVIS_ASSERT_FMT(count == 2, "SERIAL row count %lu want 2", count);
    JARVIS_ASSERT(specs[0].kind == kernel::sdf::CapKind::MMIO);
    g_tx_shape_kind1 = static_cast<uint64_t>(specs[1].kind);
    g_tx_shape_size1 = specs[1].size;
    JARVIS_ASSERT(specs[1].kind == kernel::sdf::CapKind::SHM);
    JARVIS_ASSERT_FMT(
        specs[1].size ==
            kernel::sdf::kSerialTxRingPages * arch::PAGE_SIZE,
        "SHM size %lu", specs[1].size);
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: SHM TX compose lifecycle (issue #51 step-c-prime): compose a
// SERIAL row on a fixture task, prove the ring record + slot persistence
// land, tear the task down (CSpace dispose frees the ring frames — the
// harness ResourceTracker fails on any delta).
// Input: dispatched fixture task runs sdf_compose(SERIAL)
// Expect: compose true; sdf_shm_tx_phys true with kSerialTxRingPages;
// sdf_role_cap(idx 1) true; record persists until the next compose
// Depends: boot_sdf compose, FrameCap dispose, PMM user-contiguous alloc
#if CONFIG_SERIAL_TX_SHM
JARVIS_TEST(serial_tx_sdf_compose_shm, "PRE: none | POST: none") {
    g_tx_compose_ok = 0;
    g_tx_phys_ok = 0;
    g_tx_slot_ok = 0;
    auto *t = TaskControlBlock::create(
        []() {
            auto *cur = Scheduler::current_task();
            if (cur == nullptr) {
                Scheduler::terminate(*Scheduler::current_task(), 1);
                return;
            }
            if (kernel::sdf::sdf_compose(*cur,
                                         kernel::sdf::PdRole::SERIAL)) {
                g_tx_compose_ok = 1;
            }
            uint64_t phys = 0;
            uint64_t pages = 0;
            if (kernel::sdf::sdf_shm_tx_phys(kernel::sdf::PdRole::SERIAL,
                                             phys, pages) &&
                phys != 0 &&
                pages == kernel::sdf::kSerialTxRingPages) {
                g_tx_phys_ok = 1;
            }
            int slot = -1;
            uint32_t gen = 0;
            if (kernel::sdf::sdf_role_cap(kernel::sdf::PdRole::SERIAL, 1,
                                          slot, gen) &&
                slot >= 0) {
                g_tx_slot_ok = 1;
            }
            Scheduler::terminate(*cur, 0);
        },
        11, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->get_page_table() != 0);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(g_tx_compose_ok == 1, "sdf_compose(SERIAL) failed");
    JARVIS_ASSERT_FMT(g_tx_phys_ok == 1, "shm_tx_phys record missing");
    JARVIS_ASSERT_FMT(g_tx_slot_ok == 1, "role_cap idx1 missing");
    JARVIS_TEST_PASS();
}
#endif

// Runmode: kernel
// Testidea: Registers all driver tests with the test framework
// Input: None
// Expect: All driver tests are registered (no direct assertion, only
// logging)
// Depends: Test framework, DriverRegistry
void register_driver_tests() {
    Logger::info("Registering SHM TX ring tests");
    JARVIS_REGISTER_TEST(serial_tx_ring_push_drain_order);
    JARVIS_REGISTER_TEST(serial_tx_ring_drop_full);
#if CONFIG_SERIAL_TX_SHM
    JARVIS_REGISTER_TEST(serial_tx_sdf_row_shape);
    JARVIS_REGISTER_TEST(serial_tx_sdf_compose_shm);
#endif
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
