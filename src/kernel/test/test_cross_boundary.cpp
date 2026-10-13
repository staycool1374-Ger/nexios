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

/// @file test_cross_boundary.cpp
/// @brief Cross-boundary flow tests (issue #303, Phase 6 module 1).
///
///        Kernel-task vfsd/iocd/capability coverage already exists
///        (servers_vfsd, servers_vfsd_auth, servers_iocd, cap_ipc,
///        cap_mmio, cap_mmio_user). This file adds the missing Ring-3
///        half and the live-daemon half:
///        1. Ring-3 open/read/write/close through the vfsd send_sync
///           path (staged file in, reply file out, byte-verified).
///        2. Live iocd device registration + IRQ-event forwarding over
///           IPC (the daemon's two implemented handlers, exercised for
///           real — not the -1 stubs).
///        NOT covered (confirmed gaps, see the #303 tracker): iocd
///        keyboard->shell delivery (the shell polls arch::Keyboard
///        directly; the daemon's KEYBOARD_READ handler returns -1) and
///        capability-gated MMIO *from iocd* (MMIO_MAP handler is a -1
///        stub; kernel-side gating is covered by cap_mmio*).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/test/test_isolate.hpp>
#include "test_ring3_vehicle.hpp"
#include "test_sched_helpers.hpp"

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

constexpr size_t kCapSize = 2048;
constexpr const char *kXboundPath = "/tmp/libc_xbound.txt";
constexpr const char *kXboundReplyPath = "/tmp/libc_xbound_reply.txt";
// 24 B prefix the image memcmps + 8 B filler = the 32 B it expects.
constexpr const char *kXboundContent = "XBOUND-0123456789ABCDEFGH12345678";

void stage_xbound_files() {
    test::ring3::write_data_file(
        kXboundPath, reinterpret_cast<const uint8_t *>(kXboundContent),
        32);
    test::ring3::write_data_file(kXboundReplyPath, nullptr, 0);
}

} // namespace

// Runmode: kernel
// Testidea: Ring-3 task opens/reads/writes/closes real files through the
// vfsd IPC path (every VFS syscall authorizes via vfsd send_sync).
// Input: Stage the 32 B input file + empty reply file; dispatch; the
// image reads/verifies/writes; read the reply back kernel-side.
// Expect: open fds >= 0, 32 B read with prefix verified in-image
// (rok=1), 16 B reply written; reply file exactly "XBOUND-REPLY-42\n".
// Depends: SYS_OPEN/READ/WRITE/CLOSE + vfsd daemon + vehicle header
JARVIS_TEST(cross_boundary_ring3_vfsd_file_chain,
            "PRE: vfsd | POST: none") {
    char capture[kCapSize]{};
    test::ring3::Outcome o =
        test::ring3::run(stage_xbound_files, nullptr, capture, kCapSize);
    JARVIS_ASSERT(o.joined);
    uint64_t v[3] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(capture, "LIBC_VERIFY: xbound", v,
                                        3, kCapSize));
    JARVIS_ASSERT_FMT(static_cast<int64_t>(v[0]) >= 0,
                      "Ring-3 open failed (fd marker %lu)", v[0]);
    JARVIS_ASSERT_FMT(v[1] == 32, "Ring-3 read %lu B, want 32", v[1]);
    JARVIS_ASSERT_FMT(v[2] == 1, "Ring-3 content check failed");
    uint64_t w[2] = {};
    JARVIS_ASSERT(test::ring3::parse_tag_line(
        capture, "LIBC_VERIFY: xbound_reply", w, 2, kCapSize));
    JARVIS_ASSERT_FMT(static_cast<int64_t>(w[0]) >= 0,
                      "Ring-3 reply open failed");
    JARVIS_ASSERT_FMT(w[1] == 16, "Ring-3 reply wrote %lu B, want 16",
                      w[1]);
    char content[32]{};
    int64_t n = test::ring3::read_file(kXboundReplyPath, content, sizeof(content));
    JARVIS_ASSERT_FMT(n == 16, "reply file is %ld B, want 16", n);
    const char *want = "XBOUND-REPLY-42\n";
    for (int64_t i = 0; i < n; ++i)
        JARVIS_ASSERT_FMT(content[static_cast<size_t>(i)] == want[i],
                          "reply byte %ld mismatch", i);
    test::ring3::unlink_path(kXboundPath);
    test::ring3::unlink_path(kXboundReplyPath);
    JARVIS_TEST_PASS();
}

namespace {

// Synthetic IRQ line: never a real task id, so the daemon's reply-to-
// args[0] copy (main loop) lands on no task instead of polluting one.
constexpr uint64_t kTestIrqLine = 0xFFFFFFFEULL;
constexpr uint64_t kTestIrqToken = 0xABULL;

void drain_own_inbox() {
    Message out;
    while (IPC::recv(out))
        ;
}

} // namespace

// Runmode: kernel
// Testidea: The live iocd daemon registers a device and forwards an IRQ
// event to it over IPC (both handlers genuinely implemented userspace).
// Input: send_sync REGISTER (self as device pid, synthetic line), then
// async IRQ_EVENT; poll the inbox for the forwarded event.
// Expect: REGISTER reply result 0; forwarded event arrives with the line
// + token intact within 50 ticks.
// Depends: iocd daemon (userspace/iocd.c register + irq_event paths)
JARVIS_TEST(cross_boundary_iocd_register_and_irq_forward,
            "PRE: vfsd, iocd | POST: iocd") {
    uint64_t iocd_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT_FMT(iocd_pid != 0, "iocd not running");
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    drain_own_inbox();

    // 1. REGISTER_DEVICE via send_sync (the daemon replies to args[0],
    //    which doubles as our pid here).
    iocd::Msg req{};
    req.type = iocd::IOCD_REGISTER_DEVICE;
    req.args[0] = cur->id;
    req.args[1] = kTestIrqLine;
    __builtin_memcpy(&req.args[2], "XB1", 4);
    Message send{};
    send.sender_id = cur->id;
    send.type = iocd::IOCD_REGISTER_DEVICE;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    Message reply{};
    JARVIS_ASSERT_FMT(IPC::send_sync(iocd_pid, send, reply),
                      "REGISTER_DEVICE send_sync failed");
    JARVIS_ASSERT_FMT(reply.data_size >= sizeof(iocd::Reply),
                      "REGISTER reply too short (%lu)", reply.data_size);
    iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    JARVIS_ASSERT_FMT(rpl.result == 0, "REGISTER_DEVICE result %ld",
                      rpl.result);

    // 2. IRQ_EVENT async (no reply comes back to us by daemon design:
    //    the main loop answers args[0], i.e. the line — poll for the
    //    forwarded copy instead).
    iocd::Msg ev{};
    ev.type = iocd::IOCD_IRQ_EVENT;
    ev.args[0] = kTestIrqLine;
    ev.args[1] = kTestIrqToken;
    Message fev{};
    fev.sender_id = cur->id;
    fev.type = iocd::IOCD_IRQ_EVENT;
    fev.data_size = sizeof(ev);
    __builtin_memcpy(fev.data, &ev, sizeof(ev));
    JARVIS_ASSERT_FMT(IPC::send(iocd_pid, fev, IPC_NONBLOCK),
                      "IRQ_EVENT send rejected");

    // 3. The daemon needs timer ticks to run: spin (IRQs on, no
    //    reschedule — cookbook rule) until the forwarded event lands.
    uint64_t deadline = arch::Timer::ticks() + 50;
    bool found = false;
    uint64_t got_token = 0;
    Message out;
    while (!found && arch::Timer::ticks() < deadline) {
        while (IPC::recv(out)) {
            if (out.type != iocd::IOCD_IRQ_EVENT ||
                out.data_size < sizeof(iocd::Msg))
                continue;
            iocd::Msg fwd{};
            __builtin_memcpy(&fwd, out.data, sizeof(fwd));
            if (fwd.type == iocd::IOCD_IRQ_EVENT &&
                fwd.args[0] == kTestIrqLine) {
                found = true;
                got_token = fwd.args[1];
                break;
            }
        }
        arch::pause();
    }
    drain_own_inbox();
    JARVIS_ASSERT_FMT(found, "no forwarded IRQ event within 50 ticks");
    JARVIS_ASSERT_FMT(got_token == kTestIrqToken,
                      "forwarded token 0x%lx != 0x%lx", got_token,
                      kTestIrqToken);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The live iocd daemon decodes a keyboard scancode into its
//           byte queue and serves it via KEYBOARD_READ (issue #340 P0:
//           daemon side only — no kernel forward or shell changes yet).
// Input: send_sync REGISTER (self pid, IRQ line 1); async IRQ_EVENT
//        with scancode 0x1E ('a' press); tick-spin for daemon dispatch;
//        send_sync KEYBOARD_READ twice.
// Expect: First read result=1, data[0]='a'; second read result=0
//         (exactly-once, queue drained — suite stays IPC-quiescent).
// Depends: iocd daemon (userspace/iocd.c decode + queue + read paths)
JARVIS_TEST(cross_boundary_iocd_keyboard_read_delivery,
            "PRE: vfsd, iocd | POST: iocd") {
    uint64_t iocd_pid = iocd::get_iocd_pid();
    JARVIS_ASSERT_FMT(iocd_pid != 0, "iocd not running");
    auto *cur = Scheduler::current_task();
    JARVIS_ASSERT(cur != nullptr);
    drain_own_inbox();

    iocd::Msg req{};
    req.type = iocd::IOCD_REGISTER_DEVICE;
    req.args[0] = cur->id;
    req.args[1] = 1; // IRQ line 1: keyboard
    __builtin_memcpy(&req.args[2], "KBD", 4);
    Message send{};
    send.sender_id = cur->id;
    send.type = iocd::IOCD_REGISTER_DEVICE;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    Message reply{};
    JARVIS_ASSERT_FMT(IPC::send_sync(iocd_pid, send, reply),
                      "REGISTER_DEVICE send_sync failed");
    JARVIS_ASSERT_FMT(reply.data_size >= sizeof(iocd::Reply),
                      "REGISTER reply too short (%lu)", reply.data_size);

    // Inject 'a' press (set-1 0x1E) as the kernel IRQ path would.
    iocd::Msg ev{};
    ev.type = iocd::IOCD_IRQ_EVENT;
    ev.args[0] = 1;
    ev.args[1] = 0x1E;
    Message fev{};
    fev.sender_id = cur->id;
    fev.type = iocd::IOCD_IRQ_EVENT;
    fev.data_size = sizeof(ev);
    __builtin_memcpy(fev.data, &ev, sizeof(ev));
    JARVIS_ASSERT_FMT(IPC::send(iocd_pid, fev, IPC_NONBLOCK),
                      "IRQ_EVENT send rejected");

    // The daemon needs timer ticks to run (same bound as the forward
    // test); the queue is filled asynchronously.
    uint64_t deadline = arch::Timer::ticks() + 50;
    bool readable = false;
    iocd::Reply rpl{};
    while (!readable && arch::Timer::ticks() < deadline) {
        iocd::Msg rd{};
        rd.type = iocd::IOCD_KEYBOARD_READ;
        rd.args[0] = cur->id;
        Message sread{};
        sread.sender_id = cur->id;
        sread.type = iocd::IOCD_KEYBOARD_READ;
        sread.data_size = sizeof(rd);
        __builtin_memcpy(sread.data, &rd, sizeof(rd));
        Message rreply{};
        if (!IPC::send_sync(iocd_pid, sread, rreply))
            break;
        if (rreply.data_size < sizeof(iocd::Reply))
            break;
        __builtin_memcpy(&rpl, rreply.data, sizeof(rpl));
        if (rpl.result == 1)
            readable = true;
        else
            arch::pause();
    }
    JARVIS_ASSERT_FMT(readable, "no decoded byte within 50 ticks");
    JARVIS_ASSERT_FMT(rpl.data[0] == static_cast<uint64_t>('a'),
                      "decoded byte 0x%lx != 'a'", rpl.data[0]);

    // Exactly-once: second read is empty.
    iocd::Msg rd2{};
    rd2.type = iocd::IOCD_KEYBOARD_READ;
    rd2.args[0] = cur->id;
    Message sread2{};
    sread2.sender_id = cur->id;
    sread2.type = iocd::IOCD_KEYBOARD_READ;
    sread2.data_size = sizeof(rd2);
    __builtin_memcpy(sread2.data, &rd2, sizeof(rd2));
    Message rreply2{};
    JARVIS_ASSERT_FMT(IPC::send_sync(iocd_pid, sread2, rreply2),
                      "second KEYBOARD_READ send_sync failed");
    iocd::Reply rpl2{};
    __builtin_memcpy(&rpl2, rreply2.data, sizeof(rpl2));
    JARVIS_ASSERT_FMT(rpl2.result == 0, "queue not drained (result %ld)",
                      rpl2.result);
    drain_own_inbox();
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Registers all cross-boundary flow tests.
// Input: None
// Expect: All cross-boundary tests registered
// Depends: test framework
void register_cross_boundary_tests() {
    Logger::info("Registering cross-boundary tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(cross_boundary_ring3_vfsd_file_chain);
    JARVIS_REGISTER_TEST(cross_boundary_iocd_register_and_irq_forward);
    JARVIS_REGISTER_TEST(cross_boundary_iocd_keyboard_read_delivery); // #340 P0
#endif
}
