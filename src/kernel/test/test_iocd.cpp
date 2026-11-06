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
#include <kernel/arch/timer.hpp>
#include <kernel/test/test_sched_helpers.hpp>

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

namespace {

/// @brief Register the harness task as an IOCD device on @p line.
/// @return Daemon reply result, or -99 on transport failure.
int64_t iocd_register_harness(uint64_t pid, uint32_t line, const char *name) {
    iocd::Msg reg{};
    reg.type = iocd::IOCD_REGISTER_DEVICE;
    reg.args[0] = pid;
    reg.args[1] = line;
    size_t idx = 0;
    while (name[idx] != '\0' && idx < 55) {
        reinterpret_cast<char *>(&reg.args[2])[idx] = name[idx];
        ++idx;
    }
    reinterpret_cast<char *>(&reg.args[2])[idx] = '\0';
    Message send{};
    send.sender_id = pid;
    send.type = iocd::IOCD_REGISTER_DEVICE;
    send.data_size = sizeof(reg);
    __builtin_memcpy(send.data, &reg, sizeof(reg));
    Message reply{};
    if (!IPC::send_sync(iocd::get_iocd_pid(), send, reply)) {
        return -99;
    }
    if (reply.data_size < sizeof(iocd::Reply)) {
        return -99;
    }
    iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    return rpl.result;
}

/// @brief Outcome of one IRQ_EVENT collected without send_sync (the
///        daemon replies to args[0], which for IRQ_EVENT is the line
///        number per the daemon wire format — not a pid — so a send_sync
///        reply never routes back; see the kernel producer
///        Keyboard::try_forward_to_iocd and the cross_boundary precedent,
///        which uses a synthetic line for the same reason). Collection
///        is a bounded poll for the forwarded copy.
struct IrqOutcome {
    bool sent = false;        ///< Plain send accepted by the daemon queue.
    bool got_forward = false; ///< A type-201 forward arrived.
    uint64_t fwd_irq = 0;     ///< Forward args[0] (irq line).
    uint64_t fwd_payload = 0; ///< Forward args[1] (payload).
};

/// @brief Send an IRQ_EVENT over live IPC (harness context) and collect
///        the forwarded copy with a bounded poll. Never blocks.
///        Wire format (daemon + kernel producer): args[0] = irq line,
///        args[1] = payload. The daemon's reply goes to args[0] as a
///        pid, so for real lines it lands on an unrelated task (or the
///        harness for line 1) and is NOT collected here — the forward is
///        the handling proof; unmatched lines produce no forward.
/// @param line IRQ line to raise. @param payload Opaque payload.
/// @param out Filled with whatever arrived (got_forward stays false on
///        silence — callers assert presence or absence).
void iocd_raise_irq(uint32_t line, uint64_t payload, IrqOutcome &out) {
    out = IrqOutcome{};
    iocd::Msg req{};
    req.type = iocd::IOCD_IRQ_EVENT;
    req.args[0] = line;
    req.args[1] = payload;
    kernel::Message send{};
    send.sender_id = Scheduler::current_task()->id;
    send.type = iocd::IOCD_IRQ_EVENT;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    out.sent = IPC::send(iocd::get_iocd_pid(), send);
    if (!out.sent) {
        return;
    }
    const uint64_t deadline = arch::Timer::ticks() + 100;
    while (!out.got_forward && arch::Timer::ticks() < deadline) {
        kernel::Message got{};
        if (!IPC::recv(got)) {
            arch::pause();
            continue;
        }
        if (got.type != iocd::IOCD_IRQ_EVENT ||
            got.data_size < 24) {
            continue;
        }
        uint64_t got_irq = 0;
        uint64_t got_payload = 0;
        __builtin_memcpy(&got_irq, got.data + 8, sizeof(got_irq));
        if (got_irq != line) {
            continue;
        }
        __builtin_memcpy(&got_payload, got.data + 16, sizeof(got_payload));
        out.got_forward = (got_payload == payload);
        out.fwd_irq = got_irq;
        out.fwd_payload = got_payload;
    }
    // The daemon replies to every IRQ_EVENT at args[0] as a pid. When the
    // line number coincides with a live pid (line 1 is the harness task
    // itself in the test build), that reply lands in our own inbox as a
    // stray type-0 message; the next send_sync would consume it as its
    // reply (off-by-one: first_read got {0} instead of {1,'a'}). Settle:
    // bounded drain-until-quiet collects the residue so later send_sync
    // calls see only fresh replies. No send_sync is outstanding here, so
    // discarding everything is safe.
    const uint64_t settle = arch::Timer::ticks() + 20;
    uint64_t quiet = 0;
    while (arch::Timer::ticks() < settle && quiet < 5) {
        kernel::Message residue{};
        if (!IPC::recv(residue)) {
            ++quiet;
            arch::pause();
            continue;
        }
        quiet = 0;
    }
}

/// @brief KEYBOARD_READ over live IPC; @p byte_out gets data[0].
/// @return Daemon reply result, or -99 on transport failure.
int64_t iocd_keyboard_read(uint64_t pid, uint64_t &byte_out) {
    iocd::Msg req{};
    req.type = iocd::IOCD_KEYBOARD_READ;
    req.args[0] = pid;
    Message send{};
    send.sender_id = pid;
    send.type = iocd::IOCD_KEYBOARD_READ;
    send.data_size = sizeof(req);
    __builtin_memcpy(send.data, &req, sizeof(req));
    Message reply{};
    if (!IPC::send_sync(iocd::get_iocd_pid(), send, reply)) {
        return -99;
    }
    if (reply.data_size < sizeof(iocd::Reply)) {
        return -99;
    }
    iocd::Reply rpl{};
    __builtin_memcpy(&rpl, reply.data, sizeof(rpl));
    byte_out = rpl.data[0];
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
// Testidea: Keyboard IRQ converts to a queued IPC byte over the daemon
// path with zero hardware involved: the harness raises IRQ_EVENT{1,
// 0x1E} exactly as a keyboard ISR would (args[0]=line, args[1]=
// scancode, NONBLOCK like the real producer); the daemon decodes set-1
// 0x1E to 'a' into its byte queue AND forwards the raw event to the
// registered line-1 device; KEYBOARD_READ then delivers 'a' once.
// Collection is a bounded poll for the forward plus a settle drain: the
// daemon replies to every IRQ_EVENT at args[0] as a pid, and for line 1
// that pid is the harness task itself, so the reply lands in our own
// inbox — iocd_raise_irq drains it post-forward (without the settle the
// next send_sync consumes the stray {0} as its reply: off-by-one).
// Input: Register self on line 1; drain queue + inbox; NONBLOCK
// IRQ_EVENT{1, 0x1E}; poll forward; KEYBOARD_READ x2; UNREGISTER;
// IRQ_EVENT{1, 0x1E} again (decodes despite no consumer, then drain).
// Expect: register 0; forward arrives with args (1, 0x1E); first read
// result 1 data 'a'; second read 0; unregister 0; post-exit no forward
// yet still decodes (1, 'a').
// Depends: iocd IRQ_EVENT decode + device forward + KEYBOARD_READ,
// live IPC. NOTE: no i8042 injection — the IPC message IS what a
// keyboard ISR delivers; hardware injection is covered by the
// cross_boundary delivery test on x86_64.
// Pseudocode: setup + register + drains; drive + collect; unregister;
// drain queue + inbox; assert (cleanup first).
JARVIS_TEST(iocd_keyboard_irq_to_event, "PRE: iocd | POST: iocd") {
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT(me != nullptr);
    const uint64_t pid = me->id;

    JARVIS_ASSERT_EQ(0LL, iocd_register_harness(pid, 1, "test-kbd"));

    uint64_t ignored = 0;
    while (iocd_keyboard_read(pid, ignored) == 1) {
    }
    kernel::Message tossed{};
    while (IPC::recv(tossed)) {
    }

    IrqOutcome raised{};
    iocd_raise_irq(1, 0x1EU, raised);
    uint64_t first_byte = 0;
    const int64_t first_read = iocd_keyboard_read(pid, first_byte);
    uint64_t second_byte = 0xFF;
    const int64_t second_read = iocd_keyboard_read(pid, second_byte);
    const int64_t unreg = iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE,
                                                pid, 0);
    IrqOutcome orphan{};
    iocd_raise_irq(1, 0x1EU, orphan);
    uint64_t orphan_byte = 0;
    const int64_t orphan_read = iocd_keyboard_read(pid, orphan_byte);
    while (iocd_keyboard_read(pid, ignored) == 1) {
    }
    while (IPC::recv(tossed)) {
    }

    JARVIS_ASSERT_FMT(raised.sent, "IRQ_EVENT send refused");
    JARVIS_ASSERT_FMT(raised.got_forward, "no keyboard forward arrived");
    JARVIS_ASSERT_FMT(raised.fwd_irq == 1, "forward irq %lu != 1",
                      raised.fwd_irq);
    JARVIS_ASSERT_FMT(raised.fwd_payload == 0x1EU,
                      "forward payload 0x%lx != 0x1E", raised.fwd_payload);
    JARVIS_ASSERT_FMT(first_read == 1, "first KEYBOARD_READ: %ld",
                      first_read);
    JARVIS_ASSERT_FMT(first_byte == static_cast<uint64_t>('a'),
                      "decoded byte 0x%lx != 'a'", first_byte);
    JARVIS_ASSERT_FMT(second_read == 0, "queue not drained: %ld",
                      second_read);
    JARVIS_ASSERT_FMT(unreg == 0, "UNREGISTER failed: %ld", unreg);
    JARVIS_ASSERT_FMT(orphan.sent, "orphan IRQ_EVENT send refused");
    JARVIS_ASSERT_FMT(!orphan.got_forward, "orphan forward unexpected");
    JARVIS_ASSERT_FMT(orphan_read == 1, "orphan decode missing: %ld",
                      orphan_read);
    JARVIS_ASSERT_FMT(orphan_byte == static_cast<uint64_t>('a'),
                      "orphan byte 0x%lx != 'a'", orphan_byte);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Serial IRQ converts to a forwarded IPC event on a synthetic
// line (never a task id, so the daemon's reply-to-args[0] copy is
// dropped instead of polluting a task) while the SERIAL_READ/WRITE
// handlers fail closed (-1, unimplemented in the daemon): register,
// fire IRQ_EVENT, assert the forward arrives, then pin the -1 stub
// contract and the post-exit silence.
// Input: Register line 0xFFFFFFE4; SERIAL_READ + SERIAL_WRITE;
// IRQ_EVENT{line, 0x55} via poll-collect; UNREGISTER; IRQ_EVENT again.
// Expect: read -1 and write -1; forward arrives with args (line,
// 0x55); unregister 0; post-exit no forward.
// Depends: iocd IRQ_EVENT forward + SERIAL fail-closed stubs, live IPC.
// Pseudocode: setup + register + drain; drive + collect; unregister +
// drain; assert (cleanup first).
JARVIS_TEST(iocd_serial_irq_to_event, "PRE: iocd | POST: iocd") {
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT(me != nullptr);
    const uint64_t pid = me->id;
    constexpr uint32_t kSerialLine = 0xFFFFFFE4U;

    JARVIS_ASSERT_EQ(0LL, iocd_register_harness(pid, kSerialLine,
                                                "test-ser"));
    kernel::Message tossed{};
    while (IPC::recv(tossed)) {
    }

    const int64_t read_result =
        iocd_policy_roundtrip(iocd::IOCD_SERIAL_READ, pid, 0);
    const int64_t write_result =
        iocd_policy_roundtrip(iocd::IOCD_SERIAL_WRITE, pid, 0);
    IrqOutcome raised{};
    iocd_raise_irq(kSerialLine, 0x55U, raised);
    const int64_t unreg = iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE,
                                                pid, 0);
    IrqOutcome orphan{};
    iocd_raise_irq(kSerialLine, 0x55U, orphan);
    while (IPC::recv(tossed)) {
    }

    JARVIS_ASSERT_FMT(read_result == -1, "SERIAL_READ not -1: %ld",
                      read_result);
    JARVIS_ASSERT_FMT(write_result == -1, "SERIAL_WRITE not -1: %ld",
                      write_result);
    JARVIS_ASSERT_FMT(raised.sent, "IRQ_EVENT send refused");
    JARVIS_ASSERT_FMT(raised.got_forward, "no serial forward arrived");
    JARVIS_ASSERT_FMT(raised.fwd_irq == kSerialLine,
                      "forward irq 0x%lx != 0x%x", raised.fwd_irq,
                      kSerialLine);
    JARVIS_ASSERT_FMT(raised.fwd_payload == 0x55U,
                      "forward payload 0x%lx != 0x55", raised.fwd_payload);
    JARVIS_ASSERT_FMT(unreg == 0, "UNREGISTER failed: %ld", unreg);
    JARVIS_ASSERT_FMT(orphan.sent, "orphan IRQ_EVENT send refused");
    JARVIS_ASSERT_FMT(!orphan.got_forward, "orphan forward unexpected");
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
// Testidea: Two device handlers coexist on distinct synthetic IRQ lines
// with per-line dispatch, collected without send_sync (replies go to
// args[0]-as-pid and never route back): register harness on lines
// 0xFFFFFFF3 + 0xFFFFFFF5, raise each line plus an unregistered line,
// and verify exactly the two matching forwards arrive with payloads;
// removals drop exactly their line (post-exit silence).
// Input: Register both lines; IRQ_EVENT each + unmatched line;
// UNREGISTER x2 (+ a third, -1); IRQ_EVENT both again.
// Expect: sends accepted; both forwards arrive with line+payload;
// unmatched/post-exit lines produce no forward; unregisters 0/0/-1.
// Depends: iocd REGISTER/IRQ_EVENT/UNREGISTER device table, live IPC.
// Pseudocode: setup + register + drain; drive + collect; unregister +
// drain; assert (cleanup first).
JARVIS_TEST(iocd_multiple_device_handlers, "PRE: iocd | POST: iocd") {
    auto *me = Scheduler::current_task();
    JARVIS_ASSERT(me != nullptr);
    const uint64_t pid = me->id;
    constexpr uint32_t kLineA = 0xFFFFFFF3U;
    constexpr uint32_t kLineB = 0xFFFFFFF5U;
    constexpr uint32_t kLineNone = 0xFFFFFFF7U;

    JARVIS_ASSERT_EQ(0LL, iocd_register_harness(pid, kLineA, "test-mh-a"));
    JARVIS_ASSERT_EQ(0LL, iocd_register_harness(pid, kLineB, "test-mh-b"));
    kernel::Message tossed{};
    while (IPC::recv(tossed)) {
    }

    IrqOutcome line_a{};
    iocd_raise_irq(kLineA, 0x31U, line_a);
    IrqOutcome line_b{};
    iocd_raise_irq(kLineB, 0x32U, line_b);
    IrqOutcome line_none{};
    iocd_raise_irq(kLineNone, 0U, line_none);
    const int64_t unreg_one =
        iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE, pid, 0);
    const int64_t unreg_two =
        iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE, pid, 0);
    const int64_t unreg_gone =
        iocd_policy_roundtrip(iocd::IOCD_UNREGISTER_DEVICE, pid, 0);
    IrqOutcome after_a{};
    iocd_raise_irq(kLineA, 0U, after_a);
    IrqOutcome after_b{};
    iocd_raise_irq(kLineB, 0U, after_b);
    while (IPC::recv(tossed)) {
    }

    JARVIS_ASSERT_FMT(line_a.sent && line_b.sent && line_none.sent,
                      "IRQ_EVENT sends refused");
    JARVIS_ASSERT_FMT(line_a.got_forward, "line A forward missing");
    JARVIS_ASSERT_FMT(line_a.fwd_irq == kLineA, "forward line 0x%lx wrong",
                      line_a.fwd_irq);
    JARVIS_ASSERT_FMT(line_a.fwd_payload == 0x31U,
                      "forward payload 0x%lx wrong", line_a.fwd_payload);
    JARVIS_ASSERT_FMT(line_b.got_forward, "line B forward missing");
    JARVIS_ASSERT_FMT(line_b.fwd_irq == kLineB, "forward line 0x%lx wrong",
                      line_b.fwd_irq);
    JARVIS_ASSERT_FMT(line_b.fwd_payload == 0x32U,
                      "forward payload 0x%lx wrong", line_b.fwd_payload);
    JARVIS_ASSERT_FMT(!line_none.got_forward, "unmatched line forwarded");
    JARVIS_ASSERT_FMT(unreg_one == 0, "first UNREGISTER: %ld", unreg_one);
    JARVIS_ASSERT_FMT(unreg_two == 0, "second UNREGISTER: %ld", unreg_two);
    JARVIS_ASSERT_FMT(unreg_gone == -1, "third UNREGISTER not -1: %ld",
                      unreg_gone);
    JARVIS_ASSERT_FMT(!after_a.got_forward, "line A alive after exit");
    JARVIS_ASSERT_FMT(!after_b.got_forward, "line B alive after exit");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The iocd daemon task itself is CPU-pinnable: the empty mask
// clamps to CPU 0 and an explicit mask round-trips, with the entry mask
// restored (or canonicalized to 0x1 when the entry was the unset 0,
// which the setter cannot store — empty always means CPU 0).
// NOTE: per-IRQ-line routing has no daemon API (no op code in iocd.hpp;
// affinity is a Scheduler task property), so this pins the
// placeable-daemon contract — the closest existing knob.
// Input: Daemon pid lookup; get mask; set 0 (expect clamp 0x1); set
// entry-or-canonical and read back.
// Expect: daemon found; clamp holds; final mask equals the requested
// mask; no residue (final mask is entry, or canonical 0x1).
// Depends: Scheduler::get/set_affinity, iocd pid registry.
// Pseudocode: find daemon; record mask; set 0 + assert clamp; set
// entry-or-0x1 + assert; no residue.
JARVIS_TEST(iocd_irq_affinity, "PRE: iocd | POST: iocd") {
    const uint64_t pid = iocd::get_iocd_pid();
    JARVIS_ASSERT(pid != 0);
    auto *daemon = Scheduler::find_task(pid);
    JARVIS_ASSERT(daemon != nullptr);
    const uint64_t before = Scheduler::get_affinity(*daemon);

    Scheduler::set_affinity(*daemon, 0);
    const uint64_t clamped = Scheduler::get_affinity(*daemon);
    const uint64_t want_back = (before != 0) ? before : 0x1ULL;
    Scheduler::set_affinity(*daemon, want_back);
    const uint64_t restored = Scheduler::get_affinity(*daemon);

    JARVIS_ASSERT_FMT(clamped == 0x1ULL, "empty mask not clamped: 0x%lx",
                      clamped);
    JARVIS_ASSERT_FMT(restored == want_back, "affinity not kept: 0x%lx",
                      restored);
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
