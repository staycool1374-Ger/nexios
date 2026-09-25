/*
 * NexIOS RTOS — debugd Phase 3 (issue #224)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * Host unit tests for the RSP parser core + controller (spec §5–§6,
 * §11 host gate). Assert-based, zero dependencies, return-code gate.
 * Also doubles as the RSP conformance oracle for later phases.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gdb_rsp.hpp"
#include "target_controller.hpp"
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
using debugd::Command;
using debugd::FeedResult;
using debugd::RspParser;

// Feed a whole script byte-by-byte (worst-case short reads); returns the
// last terminal result and leaves the packet (if any) in the parser.
FeedResult FeedAll(RspParser &p, const char *script) {
    FeedResult r = FeedResult::kNeedMore;
    for (const char *c = script; *c != '\0'; ++c) {
        r = p.feed(*c);
        if (r != FeedResult::kNeedMore)
            break;
    }
    return r;
}

void TestFraming() {
    RspParser p;
    CHECK(FeedAll(p, "$?#3f") == FeedResult::kPacket);
    CHECK(p.packet() == "?");
}

void TestGarbageResync() {
    RspParser p;
    // Garbage, partial packet, nested start: must resync to the real one.
    CHECK(FeedAll(p, "xyz$?#3") == FeedResult::kNeedMore);
    CHECK(FeedAll(p, "$g#67") == FeedResult::kPacket);
    CHECK(p.packet() == "g");
}

void TestChecksumUpperLower() {
    RspParser p;
    CHECK(FeedAll(p, "$?#3F") == FeedResult::kPacket); // upper hex ok
    CHECK(p.packet() == "?");
}

void TestBadChecksum() {
    RspParser p;
    CHECK(FeedAll(p, "$?#00") == FeedResult::kBadChecksum);
    // Parser resyncs: next packet parses cleanly.
    CHECK(FeedAll(p, "$?#3f") == FeedResult::kPacket);
}

void TestAckNack() {
    RspParser p;
    CHECK(p.feed('+') == FeedResult::kAck);
    CHECK(p.feed('-') == FeedResult::kNack);
}

void TestCtrlC() {
    RspParser p;
    CHECK(p.feed('\x03') == FeedResult::kInterrupted);
}

void TestStopQuery() {
    Command c;
    CHECK(debugd::ParseCommand("?", c));
    CHECK(c.type == Command::Type::kStopQuery);
}

void TestReadRegs() {
    Command c;
    CHECK(debugd::ParseCommand("g", c));
    CHECK(c.type == Command::Type::kReadRegs);
    CHECK(debugd::ParseCommand("Gdeadbeef", c));
    CHECK(c.type == Command::Type::kWriteRegs);
    CHECK(c.data == "deadbeef");
    CHECK(!debugd::ParseCommand("G", c)); // empty payload rejected
}

void TestRegBlobSizes() {
    // Golden byte counts per arch (spec §5 layouts: x86_64 Linux GPRs
    // 16x8 + rip + eflags + 6 segment regs; AArch64 34x8; RISC-V 33x8).
    CHECK(debugd::RegBlobSize(Arch::kX86_64) == 164);
    CHECK(debugd::RegBlobSize(Arch::kAArch64) == 272);
    CHECK(debugd::RegBlobSize(Arch::kRiscv64) == 264);
}

void TestReadMem() {
    Command c;
    CHECK(debugd::ParseCommand("m41000000,10", c));
    CHECK(c.type == Command::Type::kReadMem);
    CHECK(c.addr == 0x41000000ULL);
    CHECK(c.len == 0x10);
    CHECK(!debugd::ParseCommand("m41000000,0", c)); // zero length rejected
    CHECK(!debugd::ParseCommand("mZZZZ,10", c));    // garbage rejected
    CHECK(!debugd::ParseCommand("m123456789abcdef01,10", c)); // >16 digits
}

void TestWriteMem() {
    Command c;
    CHECK(debugd::ParseCommand("M41000000,4:deadbeef", c));
    CHECK(c.type == Command::Type::kWriteMem);
    CHECK(c.addr == 0x41000000ULL);
    CHECK(c.len == 4);
    CHECK(c.data == "deadbeef");
    CHECK(!debugd::ParseCommand("M41000000,4:deadbee", c)); // short data
    CHECK(!debugd::ParseCommand("M41000000,4", c));         // missing colon
}

void TestContinueStep() {
    Command c;
    CHECK(debugd::ParseCommand("c", c));
    CHECK(c.type == Command::Type::kContinue);
    CHECK(debugd::ParseCommand("c41000000", c));
    CHECK(c.type == Command::Type::kContinue);
    CHECK(c.addr == 0x41000000ULL);
    CHECK(debugd::ParseCommand("s", c));
    CHECK(c.type == Command::Type::kStep);
}

void TestBreakpoints() {
    Command c;
    CHECK(debugd::ParseCommand("Z0,41000000,4", c));
    CHECK(c.type == Command::Type::kBreakSet);
    CHECK(c.addr == 0x41000000ULL);
    CHECK(c.len == 4);
    CHECK(debugd::ParseCommand("z0,41000000,4", c));
    CHECK(c.type == Command::Type::kBreakClear);
    CHECK(!debugd::ParseCommand("Z1,41000000,4", c)); // only type 0
}

void TestQSupported() {
    Command c;
    CHECK(debugd::ParseCommand("qSupported:swbreak+", c));
    CHECK(c.type == Command::Type::kQSupported);
    CHECK(debugd::QSupportedReply() == "PacketSize=1000;qXfer:features:read-;swbreak+");
}

void TestSetThread() {
    Command c;
    CHECK(debugd::ParseCommand("Hg0", c));
    CHECK(c.type == Command::Type::kSetThread);
    CHECK(debugd::ParseCommand("Hc-1", c));
    CHECK(c.type == Command::Type::kSetThread);
    CHECK(!debugd::ParseCommand("H", c)); // op+tid required
    // NOTE (Phase 4 obligation): the parser accepts any H<op><tid>
    // syntactically; the §5 "everything else E01" single-thread rule is
    // enforced at dispatch, not here.
}

void TestDetachKill() {
    Command c;
    CHECK(debugd::ParseCommand("D", c));
    CHECK(c.type == Command::Type::kDetach);
    CHECK(debugd::ParseCommand("k", c));
    CHECK(c.type == Command::Type::kKill);
}

void TestHexCodec() {
    const std::uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
    char hex[16] = {};
    CHECK(debugd::EncodeHex(bytes, hex) == 12);
    CHECK(std::string_view(hex, 12) == "deadbeef00ff");
    std::uint8_t back[6] = {};
    CHECK(debugd::DecodeHex("deadbeef00ff", back) == 6);
    CHECK(std::memcmp(back, bytes, 6) == 0);
    CHECK(debugd::DecodeHex("abc", back) == 0);   // odd length
    CHECK(debugd::DecodeHex("zz", back) == 0);    // bad digits
    CHECK(debugd::DecodeHex("dead", back) == 2);  // prefix ok
}

void TestOversizeRejected() {
    RspParser p;
    CHECK(p.feed('$') == FeedResult::kNeedMore);
    // kMaxPacket+8 data bytes with no '#': must reject, never grow.
    FeedResult r = FeedResult::kNeedMore;
    for (std::size_t i = 0; i < debugd::kMaxPacket + 8; ++i) {
        r = p.feed('A');
        if (r != FeedResult::kNeedMore)
            break;
    }
    CHECK(r == FeedResult::kBadChecksum);
}

void TestErrnoMap() {
    using debugd::DbgErr;
    using debugd::EncodeErrno;
    CHECK(EncodeErrno(DbgErr::kOk) == "OK");
    CHECK(EncodeErrno(DbgErr::kGeneric) == "E01");
    CHECK(EncodeErrno(DbgErr::kBadHandle) == "E01");
    CHECK(EncodeErrno(DbgErr::kNoTarget) == "E01");
    CHECK(EncodeErrno(DbgErr::kFault) == "E0E"); // EFAULT, never empty
}

void TestBreakpointTable() {
    debugd::BreakpointTable t;
    CHECK(t.used() == 0);
    CHECK(t.insert(0x1000, 0xABCDEF));
    CHECK(t.insert(0x1000, 0xABCDEF)); // idempotent
    CHECK(t.used() == 1);
    CHECK(t.find(0x1000) != nullptr);
    CHECK(t.find(0x2000) == nullptr);
    CHECK(t.find(0x1000)->orig_insn == 0xABCDEF);
    CHECK(t.remove(0x1000));
    CHECK(!t.remove(0x1000));
    CHECK(t.used() == 0);
    // Fill to capacity: 65th insert fails, never grows.
    for (std::uint64_t i = 0; i < debugd::BreakpointTable::kCapacity; ++i)
        CHECK(t.insert(0x10000 + i * 4, i));
    CHECK(!t.insert(0xDEAD, 0));
    CHECK(t.used() == debugd::BreakpointTable::kCapacity);
}

void TestBreakInsn() {
    CHECK(debugd::BreakInsn(Arch::kX86_64) == 0xCC);
    CHECK(debugd::BreakInsn(Arch::kAArch64) == 0xD4200000);
    CHECK(debugd::BreakInsn(Arch::kRiscv64) == 0x00100073);
}

// Minimal fake controller: proves the interface is implementable and
// exercises the mock transport round-trip ($?#3f through short reads).
class FakeController : public debugd::TargetTaskController {
  public:
    debugd::DbgErr read_regs(Arch, std::span<std::uint8_t> out) override {
        for (auto &b : out)
            b = 0xA5;
        return debugd::DbgErr::kOk;
    }
    debugd::DbgErr write_regs(Arch, std::span<const std::uint8_t>) override {
        return debugd::DbgErr::kOk;
    }
    int read_mem(std::uint64_t, std::span<std::uint8_t> out) override {
        for (auto &b : out)
            b = 0x5A;
        return static_cast<int>(out.size());
    }
    int write_mem(std::uint64_t, std::span<const std::uint8_t> in) override {
        return static_cast<int>(in.size());
    }
    debugd::DbgErr break_at(std::uint64_t va) override {
        return table_.insert(va, 0) ? debugd::DbgErr::kOk
                                    : debugd::DbgErr::kGeneric;
    }
    debugd::DbgErr clear_break(std::uint64_t va) override {
        return table_.remove(va) ? debugd::DbgErr::kOk
                                 : debugd::DbgErr::kGeneric;
    }
    debugd::DbgErr step() override { return debugd::DbgErr::kOk; }
    debugd::DbgErr cont() override { return debugd::DbgErr::kOk; }
    std::string_view stop_reason() override { return "T05"; }

    debugd::BreakpointTable table_;
};

void TestMockRoundTrip() {
    // Script with noise + short reads (1 byte/call): parser must resync
    // and deliver the real packet; controller serves it; reply captured.
    debugd::test::MockTransport t("xx$?#3f", 1);
    RspParser p;
    FeedResult r = FeedResult::kNeedMore;
    std::uint8_t byte = 0;
    while (r == FeedResult::kNeedMore) {
        CHECK(t.read_exact(std::span(&byte, 1)));
        r = p.feed(static_cast<char>(byte));
    }
    CHECK(r == FeedResult::kPacket);
    CHECK(p.packet() == "?");
    FakeController c;
    Command cmd;
    CHECK(debugd::ParseCommand(p.packet(), cmd));
    CHECK(cmd.type == Command::Type::kStopQuery);
    std::string_view reason = c.stop_reason();
    CHECK(reason == "T05");
    // Reply path through the mock: write_all captures bytes.
    const char reply[] = "$T05#b9";
    CHECK(t.write_all(std::span(
        reinterpret_cast<const std::uint8_t *>(reply), sizeof(reply) - 1)));
    CHECK(t.written() == "$T05#b9");
}

void TestMockTimeout() {
    debugd::test::MockTransport t("$?#3", 1, 2); // fail after 2 bytes
    std::uint8_t byte = 0;
    CHECK(t.read_exact(std::span(&byte, 1)));
    CHECK(t.read_exact(std::span(&byte, 1)));
    CHECK(!t.read_exact(std::span(&byte, 1))); // timeout: false
}

} // namespace

int main() {
    TestFraming();
    TestGarbageResync();
    TestChecksumUpperLower();
    TestBadChecksum();
    TestAckNack();
    TestCtrlC();
    TestStopQuery();
    TestReadRegs();
    TestRegBlobSizes();
    TestReadMem();
    TestWriteMem();
    TestContinueStep();
    TestBreakpoints();
    TestQSupported();
    TestSetThread();
    TestDetachKill();
    TestHexCodec();
    TestOversizeRejected();
    TestErrnoMap();
    TestBreakpointTable();
    TestBreakInsn();
    TestMockRoundTrip();
    TestMockTimeout();
    std::printf("debugd-host: PASS %d FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
