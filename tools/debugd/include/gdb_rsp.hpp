/*
 * NexIOS RTOS — debugd Phase 3 (issue #224)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * GDB Remote Serial Protocol parser core (spec docs/specs/debugd.md §5).
 * Header-only, zero-heap: compiles under host g++ AND the freestanding
 * cross toolchain. Allowed headers only: <span> <string_view> <array>
 * <cstdint> <cstddef> (all C++20 freestanding).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace debugd {

/// @brief Hard packet-size cap (spec §5). Never grown; oversize input is
///        rejected, never reallocated.
constexpr std::size_t kMaxPacket = 4096;

/// @brief Target architectures for register-blob layouts (spec §5).
enum class Arch : std::uint8_t { kX86_64, kAArch64, kRiscv64 };

/// @brief G-packet blob sizes in bytes (standard GDB layouts):
///        x86_64 Linux: 16 GPRs + rip + eflags + segregs;
///        AArch64: x0-x30 + sp + pc + cpsr; RISC-V: x0-x31 + pc.
constexpr std::size_t RegBlobSize(Arch arch) noexcept {
    switch (arch) {
    case Arch::kX86_64:
        return 16 * 8 + 8 + 4 + 6 * 4;
    case Arch::kAArch64:
        return 34 * 8;
    case Arch::kRiscv64:
        return 33 * 8;
    }
    return 0;
}

/// @brief Byte-feed result (spec §5 framing). Note on kBadChecksum:
///        oversize rejection stays in kData (packet continues) while a
///        checksum mismatch drops to Idle — both converge on the next
///        '$' (restart) or '#' terminator, so callers may treat the
///        code uniformly and always answer '-'.
enum class FeedResult : std::uint8_t {
    kNeedMore,    // packet incomplete, feed more bytes
    kPacket,      // complete $data#cs with valid checksum (see packet())
    kAck,         // '+' from the peer
    kNack,        // '-' from the peer (caller retransmits last reply)
    kInterrupted, // Ctrl-C (0x03) — stop the current continue
    kBadChecksum, // #cs mismatch (caller sends '-' and resyncs)
};

/// @brief Incremental RSP packet parser. Garbage resyncs: any '$'
///        restarts a packet, anything outside a packet is ignored
///        except '+', '-', and 0x03.
class RspParser {
  public:
    RspParser() noexcept { reset(); }

    void reset() noexcept {
        state_ = State::kIdle;
        len_ = 0;
        checksum_ = 0;
        digits_ = 0;
    }

    FeedResult feed(char c) noexcept {
        switch (state_) {
        case State::kIdle:
            if (c == '$') {
                state_ = State::kData;
                len_ = 0;
                checksum_ = 0;
                digits_ = 0;
            } else if (c == '+') {
                return FeedResult::kAck;
            } else if (c == '-') {
                return FeedResult::kNack;
            } else if (c == '\x03') {
                return FeedResult::kInterrupted;
            }
            return FeedResult::kNeedMore;
        case State::kData:
            if (c == '$') { // nested start: restart, never nest
                len_ = 0;
                checksum_ = 0;
                digits_ = 0;
                return FeedResult::kNeedMore;
            }
            if (c == '#') {
                state_ = State::kSum0;
                digits_ = 0;
                return FeedResult::kNeedMore;
            }
            if (len_ >= kMaxPacket)
                return FeedResult::kBadChecksum; // oversize: reject, no grow
            buf_[len_++] =
                static_cast<char>(static_cast<unsigned char>(c));
            checksum_ =
                static_cast<std::uint8_t>(checksum_ + static_cast<std::uint8_t>(c));
            return FeedResult::kNeedMore;
        case State::kSum0:
        case State::kSum1: {
            if (c == '$') { // start beats checksum: restart (resync rule)
                state_ = State::kData;
                len_ = 0;
                checksum_ = 0;
                digits_ = 0;
                return FeedResult::kNeedMore;
            }
            int v = HexVal(c);
            if (v < 0) {
                reset();
                return FeedResult::kNeedMore; // garbage in checksum: resync
            }
            digits_ = static_cast<std::uint8_t>((digits_ << 4) | v);
            if (state_ == State::kSum0) {
                state_ = State::kSum1;
                return FeedResult::kNeedMore;
            }
            // Terminal: drop to Idle but PRESERVE len_/buf_ so packet()
            // stays valid until the next feed (reset() would wipe it).
            bool ok = (digits_ == checksum_);
            state_ = State::kIdle;
            return ok ? FeedResult::kPacket : FeedResult::kBadChecksum;
        }
        }
        return FeedResult::kNeedMore;
    }

    /// @brief Payload of the last kPacket (valid until next feed/reset).
    std::string_view packet() const noexcept {
        return std::string_view(buf_.data(), len_);
    }

    static int HexVal(char c) noexcept {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    /// @brief Checksum of a payload (sum of bytes mod 256).
    static std::uint8_t Checksum(std::string_view payload) noexcept {
        std::uint8_t sum = 0;
        for (char c : payload)
            sum = static_cast<std::uint8_t>(
                sum + static_cast<std::uint8_t>(c));
        return sum;
    }

  private:
    enum class State : std::uint8_t { kIdle, kData, kSum0, kSum1 };
    State state_ = State::kIdle;
    std::array<char, kMaxPacket> buf_{};
    std::size_t len_ = 0;
    std::uint8_t checksum_ = 0;
    std::uint8_t digits_ = 0;
};

/// @brief Parsed RSP command (spec §5 subset).
struct Command {
    enum class Type : std::uint8_t {
        kUnknown,
        kStopQuery,   // ?
        kReadRegs,    // g
        kWriteRegs,   // G<hex>
        kReadMem,     // m<addr>,<len>
        kWriteMem,    // M<addr>,<len>:<hex>
        kContinue,    // c[addr]
        kStep,        // s[addr]
        kBreakSet,    // Z0,addr,kind
        kBreakClear,  // z0,addr,kind
        kQSupported,  // qSupported...
        kSetThread,   // H<op><tid>
        kDetach,      // D
        kKill,        // k (detach + terminate target, never kernel kill)
    };
    Type type = Type::kUnknown;
    std::uint64_t addr = 0; // m/M/c/s/Z0/z0 address (or thread op for H)
    std::uint64_t len = 0;  // m/M length, Z0/z0 kind
    std::string_view data{}; // G hex / M hex payload (view into packet)
};

/// @brief Parse an unsigned hex integer; returns false on empty/garbage
///        or more than 16 digits (64-bit addresses never wrap silently).
inline bool ParseHexUint(std::string_view s, std::uint64_t &out) noexcept {
    if (s.empty() || s.size() > 16)
        return false;
    std::uint64_t v = 0;
    for (char c : s) {
        int d = RspParser::HexVal(c);
        if (d < 0)
            return false;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    out = v;
    return true;
}

/// @brief Parse a packet payload into a Command. `payload` must outlive
///        the returned Command (data views into it).
inline bool ParseCommand(std::string_view payload, Command &out) noexcept {
    out = Command{};
    if (payload.empty())
        return false;
    char op = payload[0];
    std::string_view rest = payload.substr(1);
    switch (op) {
    case '?':
        out.type = Command::Type::kStopQuery;
        return rest.empty();
    case 'g':
        out.type = Command::Type::kReadRegs;
        return rest.empty();
    case 'G':
        out.type = Command::Type::kWriteRegs;
        out.data = rest;
        return !rest.empty();
    case 'm': {
        auto comma = rest.find(',');
        if (comma == std::string_view::npos)
            return false;
        out.type = Command::Type::kReadMem;
        return ParseHexUint(rest.substr(0, comma), out.addr) &&
               ParseHexUint(rest.substr(comma + 1), out.len) && out.len > 0;
    }
    case 'M': {
        auto comma = rest.find(',');
        auto colon = rest.find(':');
        if (comma == std::string_view::npos || colon == std::string_view::npos ||
            colon < comma)
            return false;
        out.type = Command::Type::kWriteMem;
        out.data = rest.substr(colon + 1);
        return ParseHexUint(rest.substr(0, comma), out.addr) &&
               ParseHexUint(rest.substr(comma + 1, colon - comma - 1),
                            out.len) &&
               out.len > 0 && out.data.size() == out.len * 2;
    }
    case 'c':
        out.type = Command::Type::kContinue;
        return rest.empty() || ParseHexUint(rest, out.addr);
    case 's':
        out.type = Command::Type::kStep;
        return rest.empty() || ParseHexUint(rest, out.addr);
    case 'Z':
    case 'z': {
        // Z0,addr,kind / z0,addr,kind (only type 0 specified).
        if (rest.size() < 2 || rest[0] != '0' || rest[1] != ',')
            return false;
        auto comma = rest.find(',', 2);
        if (comma == std::string_view::npos)
            return false;
        out.type =
            (op == 'Z') ? Command::Type::kBreakSet : Command::Type::kBreakClear;
        return ParseHexUint(rest.substr(2, comma - 2), out.addr) &&
               ParseHexUint(rest.substr(comma + 1), out.len);
    }
    case 'q':
        if (rest.substr(0, 9) == "Supported") {
            out.type = Command::Type::kQSupported;
            return true;
        }
        return false;
    case 'H':
        if (rest.size() < 2)
            return false;
        out.type = Command::Type::kSetThread;
        out.addr = static_cast<std::uint64_t>(rest[0]); // op char
        out.data = rest.substr(1);                      // thread id
        return true;
    case 'D':
        out.type = Command::Type::kDetach;
        return rest.empty();
    case 'k':
        out.type = Command::Type::kKill;
        return rest.empty();
    default:
        return false;
    }
}

/// @brief Encode bytes as lowercase hex into `out`; returns chars written.
inline std::size_t EncodeHex(std::span<const std::uint8_t> in,
                             std::span<char> out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    if (out.size() < in.size() * 2)
        return 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[2 * i] = kDigits[in[i] >> 4];
        out[2 * i + 1] = kDigits[in[i] & 0xF];
    }
    return in.size() * 2;
}

/// @brief Decode hex into bytes; returns bytes written, 0 on error.
inline std::size_t DecodeHex(std::string_view in,
                             std::span<std::uint8_t> out) noexcept {
    if (in.size() % 2 != 0 || out.size() < in.size() / 2)
        return 0;
    for (std::size_t i = 0; i < in.size() / 2; ++i) {
        int hi = RspParser::HexVal(in[2 * i]);
        int lo = RspParser::HexVal(in[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return in.size() / 2;
}

/// @brief Fixed qSupported reply (spec §5: PacketSize + swbreak only).
inline std::string_view QSupportedReply() noexcept {
    return "PacketSize=1000;qXfer:features:read-;swbreak+";
}

} // namespace debugd
