/*
 * NexIOS RTOS — debugd Phase 4 (issue #232)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * Main RSP event loop (spec docs/specs/debugd.md §13): packets from the
 * transport feed the Phase-3 parser; valid packets are ACKed and
 * dispatched through TargetTaskController (§13.1–§13.6). Exactly one
 * outstanding request at a time; per-I/O bounded deadlines detach with
 * the target resumed; Ctrl-C (0x03) stop-requests only the outstanding
 * continue set and is a no-op heartbeat when idle. Header-only,
 * zero-heap, freestanding-clean (same allowed headers as gdb_rsp.hpp).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "gdb_rsp.hpp"
#include "target_controller.hpp"

namespace debugd {

/// @brief Frame a raw payload as $data#cs (two lowercase hex digits).
/// @return Bytes written (0 when out has no room for payload + 4).
inline std::size_t FramePacket(std::string_view payload,
                              std::span<char> out) noexcept {
    if (out.size() < payload.size() + 4)
        return 0;
    std::uint8_t sum = 0;
    out[0] = '$';
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out[1 + i] = payload[i];
        sum = static_cast<std::uint8_t>(sum +
                                        static_cast<std::uint8_t>(payload[i]));
    }
    constexpr char kHex[] = "0123456789abcdef";
    out[1 + payload.size()] = '#';
    out[2 + payload.size()] = kHex[(sum >> 4) & 0xF];
    out[3 + payload.size()] = kHex[sum & 0xF];
    return payload.size() + 4;
}

/// @brief Single-threaded RSP main loop over a byte transport and a
///        task controller. Transport concept: read_exact(span)->bool
///        (false = timeout/ EOF) and write_all(span)->bool.
template <typename Transport, typename Controller>
class DebugLoop {
  public:
    static constexpr std::size_t kMaxPayload = 512;
    static constexpr std::size_t kMaxOut = 1024;
    /// @brief Consecutive I/O timeouts before detach-with-resumed halt.
    static constexpr unsigned kMaxTimeouts = 3;

    explicit DebugLoop(Transport &transport, Controller &controller) noexcept
        : transport_(transport), controller_(controller) {}

    /// @brief Run until detach (D/k/timeout) or transport death.
    /// @return true on clean detach, false on timeout/transport failure.
    bool run() noexcept {
        RspParser parser{};
        bool running = true;
        bool clean_detach = false;
        while (running) {
            // Stop-event drain (§13.4): while a continue/step is
            // outstanding, poll the controller first — a pending stop
            // is framed and sent before reading more host bytes, so a
            // stop can never be reordered behind later packets.
            if (continue_outstanding_) {
                std::string_view reason{};
                if (controller_.poll_event(reason)) {
                    (void)send_payload(reason);
                    continue_outstanding_ = false;
                }
            }
            std::uint8_t byte = 0;
            if (!transport_.read_exact(std::span(&byte, 1))) {
                if (++timeouts_ >= kMaxTimeouts) {
                    controller_.detach();
                    return false;
                }
                continue;
            }
            timeouts_ = 0;
            FeedResult fed;
            if (byte == 0x03) {
                handle_interrupt();
                continue;
            }
            fed = parser.feed(static_cast<char>(byte));
            if (fed == FeedResult::kNeedMore)
                continue;
            if (fed == FeedResult::kAck || fed == FeedResult::kNack) {
                continue; // host-side ACK traffic: absorbed, never dispatched
            }
            if (fed == FeedResult::kInterrupted) {
                handle_interrupt();
                continue;
            }
            if (fed != FeedResult::kPacket) {
                send_raw("-"); // NACK malformed, drop without dispatch
                continue;
            }
            send_raw("+");
            Command cmd{};
            if (!ParseCommand(parser.packet(), cmd)) {
                send_empty();
                continue;
            }
            if (!dispatch(cmd, running, clean_detach)) {
                // D/kdetach path already replied inside dispatch.
                if (!running)
                    return clean_detach;
            }
        }
        return clean_detach;
    }

    /// @brief Outstanding continue set for Ctrl-C targeting (§13.3).
    bool continue_outstanding() const noexcept {
        return continue_outstanding_;
    }

  private:
    bool send_raw(std::string_view s) noexcept {
        std::array<std::uint8_t, 4> buf{};
        if (s.size() > buf.size())
            return false;
        for (std::size_t i = 0; i < s.size(); ++i)
            buf[i] = static_cast<std::uint8_t>(s[i]);
        return transport_.write_all(
            std::span<const std::uint8_t>(buf.data(), s.size()));
    }

    bool send_payload(std::string_view payload) noexcept {
        std::array<char, kMaxOut> frame{};
        std::size_t n = FramePacket(
            payload, std::span<char>(frame.data(), frame.size()));
        if (n == 0)
            return false;
        std::array<std::uint8_t, kMaxOut> bytes{};
        for (std::size_t i = 0; i < n; ++i)
            bytes[i] = static_cast<std::uint8_t>(frame[i]);
        return transport_.write_all(
            std::span<const std::uint8_t>(bytes.data(), n));
    }

    bool send_empty() noexcept { return send_payload(""); }

    void send_errno(DbgErr e) noexcept { (void)send_payload(EncodeErrno(e)); }

    /// @brief Ctrl-C / interrupt: stop-request only the outstanding
    ///        continue set; idle 0x03 is a no-op heartbeat (never
    ///        broadcast, never detach).
    void handle_interrupt() noexcept {
        if (!continue_outstanding_)
            return;
        if (controller_.stop_request() == DbgErr::kOk)
            continue_outstanding_ = false;
    }

    // Returns false when the session ended (reply already written).
    bool dispatch(const Command &cmd, bool &running,
                  bool &clean_detach) noexcept {
        using T = Command::Type;
        switch (cmd.type) {
        case T::kStopQuery:
            (void)send_payload(controller_.stop_reason());
            return true;
        case T::kDetach:
        case T::kKill: {
            (void)send_payload("OK");
            (void)controller_.detach();
            running = false;
            clean_detach = true;
            return false;
        }
        case T::kContinue: {
            if (controller_.cont() == DbgErr::kOk) {
                continue_outstanding_ = true;
                // The stop reply arrives via the event poll below on the
                // next loop turn (sel2 hook); nothing is written now.
                return true;
            }
            send_errno(DbgErr::kGeneric);
            return true;
        }
        case T::kStep: {
            if (controller_.step() == DbgErr::kOk) {
                continue_outstanding_ = true;
                return true;
            }
            send_errno(DbgErr::kGeneric);
            return true;
        }
        case T::kReadRegs: {
            std::array<std::uint8_t, 256> regs{};
            if (controller_.read_regs(Arch::kX86_64,
                                      std::span(regs)) != DbgErr::kOk) {
                send_errno(DbgErr::kGeneric);
                return true;
            }
            (void)send_payload("00");
            return true;
        }
        case T::kReadMem: {
            std::array<std::uint8_t, 256> mem{};
            std::size_t want = cmd.len < mem.size() ? cmd.len : mem.size();
            int got =
                controller_.read_mem(cmd.addr, std::span(mem.data(), want));
            if (got < 0) {
                send_errno(DbgErr::kGeneric);
                return true;
            }
            (void)send_payload("OK");
            return true;
        }
        case T::kBreakSet: {
            if (controller_.break_at(cmd.addr) == DbgErr::kOk)
                (void)send_payload("OK");
            else
                send_errno(DbgErr::kGeneric);
            return true;
        }
        case T::kBreakClear: {
            if (controller_.clear_break(cmd.addr) == DbgErr::kOk)
                (void)send_payload("OK");
            else
                send_errno(DbgErr::kGeneric);
            return true;
        }
        case T::kQSupported:
        case T::kSetThread:
            (void)send_payload("");
            return true;
        case T::kWriteRegs:
        case T::kWriteMem:
        case T::kUnknown:
            (void)send_empty();
            return true;
        }
        (void)send_empty();
        return true;
    }

    Transport &transport_;
    Controller &controller_;
    unsigned timeouts_ = 0;
    bool continue_outstanding_ = false;
};

} // namespace debugd
