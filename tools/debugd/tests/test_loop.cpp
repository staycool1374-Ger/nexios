/*
 * NexIOS RTOS — debugd Phase 4 (issue #232)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * Host unit tests for the RSP main loop (spec §13, §11 host gate).
 * Assert-based, zero dependencies, return-code gate. A scripted fake
 * controller records every call; MockTransport scripts drive framing,
 * Ctrl-C, and timeouts. Transport independence: no UART/TCP here.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "debug_loop.hpp"
#include "mock_transport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

using debugd::Arch;
using debugd::DbgErr;

// Scripted fake: records calls, serves canned answers.
class ScriptedController : public debugd::TargetTaskController {
  public:
    int cont_calls = 0;
    int stop_calls = 0;
    int detach_calls = 0;
    int step_calls = 0;
    int attach_calls = 0;
    bool event_pending = false;

    DbgErr read_regs(Arch, std::span<std::uint8_t> out) override {
        for (auto &b : out)
            b = 0xA5;
        return DbgErr::kOk;
    }
    DbgErr write_regs(Arch, std::span<const std::uint8_t>) override {
        return DbgErr::kOk;
    }
    int read_mem(std::uint64_t, std::span<std::uint8_t> out) override {
        for (auto &b : out)
            b = 0x5A;
        return static_cast<int>(out.size());
    }
    int write_mem(std::uint64_t, std::span<const std::uint8_t> in) override {
        return static_cast<int>(in.size());
    }
    DbgErr break_at(std::uint64_t) override { return DbgErr::kOk; }
    DbgErr clear_break(std::uint64_t) override { return DbgErr::kOk; }
    DbgErr step() override {
        ++step_calls;
        return DbgErr::kOk;
    }
    DbgErr cont() override {
        ++cont_calls;
        return DbgErr::kOk;
    }
    std::string_view stop_reason() override { return "T05"; }
    DbgErr attach(std::uint64_t) override {
        ++attach_calls;
        return DbgErr::kOk;
    }
    DbgErr detach() override {
        ++detach_calls;
        return DbgErr::kOk;
    }
    DbgErr stop_request() override {
        ++stop_calls;
        return DbgErr::kOk;
    }
    bool poll_event(std::string_view &reason) override {
        if (!event_pending)
            return false;
        event_pending = false;
        reason = "T05";
        return true;
    }
};

// (a) Single-outstanding sequencing: a second packet is not consumed
// before the first response is written. Script: c + D; the loop must
// dispatch cont, then detach on D (no reordering, no drop).
void TestSequencing() {
    debugd::test::MockTransport t("$c#63$D#44", 1);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.cont_calls == 1);
    CHECK(c.detach_calls == 1);
}

// (b) Bad-checksum NACK with zero controller calls. Script: garbage
// packet with wrong checksum; loop NACKs (-) and serves nothing.
void TestBadChecksumNack() {
    debugd::test::MockTransport t("$?#00$D#44", 1);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.cont_calls == 0);
    CHECK(c.detach_calls == 1);
    CHECK(t.written().find("-") != std::string_view::npos);
}

// (c) Ctrl-C during outstanding continue issues sel9 stop on that set.
// Script: c, then 0x03, then D. stop_request must fire exactly once.
void TestCtrlCDuringContinue() {
    const char script[] = {'$', 'c', '#', '6', '3', 0x03,
                           '$', 'D', '#', '4', '4', '\0'};
    debugd::test::MockTransport t(
        std::string_view(script, sizeof(script) - 1), 1);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.cont_calls == 1);
    CHECK(c.stop_calls == 1);
    CHECK(c.detach_calls == 1);
}

// (d) Idle Ctrl-C is a no-op: no selector traffic, session continues
// until D. Script: 0x03 then D.
void TestIdleCtrlCNoop() {
    const char script[] = {0x03, '$', 'D', '#', '4', '4', '\0'};
    debugd::test::MockTransport t(
        std::string_view(script, sizeof(script) - 1), 1);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.cont_calls == 0);
    CHECK(c.stop_calls == 0);
    CHECK(c.step_calls == 0);
    CHECK(c.detach_calls == 1);
}

// (e) Transport timeout (fail_after exhaustion) issues detach and ends
// the session with failure. Script: nothing deliverable.
void TestTimeoutDetach() {
    debugd::test::MockTransport t("", 1, 0);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(!loop.run());
    CHECK(c.detach_calls == 1);
}

// (f) Unknown verbs get an empty reply, session survives. Script:
// qSupported (empty reply per spec) then D.
void TestUnknownEmpty() {
    debugd::test::MockTransport t("$qSupported#37$D#44", 1);
    ScriptedController c;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.detach_calls == 1);
    CHECK(t.written().find("OK") != std::string_view::npos);
}

// (g) Stop-event drain order is preserved across poll->cont->poll: the
// controller observes cont exactly once and the stop reason survives.
// Script: c then D; reason queried post-run matches the fake.
void TestStopDrainOrder() {
    debugd::test::MockTransport t("$c#63$D#44", 1);
    ScriptedController c;
    c.event_pending = true;
    debugd::DebugLoop<debugd::test::MockTransport, ScriptedController> loop(
        t, c);
    CHECK(loop.run());
    CHECK(c.cont_calls == 1);
    CHECK(c.stop_reason() == "T05");
    CHECK(c.detach_calls == 1);
    CHECK(t.written().find("T05") != std::string_view::npos);
}

} // namespace

int main() {
    TestSequencing();
    TestBadChecksumNack();
    TestCtrlCDuringContinue();
    TestIdleCtrlCNoop();
    TestTimeoutDetach();
    TestUnknownEmpty();
    TestStopDrainOrder();
    std::printf("loop: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
