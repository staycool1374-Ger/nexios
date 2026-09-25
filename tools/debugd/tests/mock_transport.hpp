/*
 * NexIOS RTOS — debugd Phase 3 (issue #224)
 * Copyright (C) 2026 Arnold Hasshold
 *
 * Mock ITransport (spec §7): scripted byte source with short-read,
 * timeout, and noise injection. The mock is deliberately nastier than
 * a UART: every framing assumption the parser makes is attacked here.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace debugd {
namespace test {

/// @brief Scripted byte-stream transport. Each call serves at most
///        `max_chunk` bytes (short-read injection: max_chunk == 1
///        forces byte-at-a-time delivery); once `fail_after` bytes
///        have been served (or the script is exhausted) calls fail
///        (timeout). All writes are captured for assertion.
class MockTransport {
  public:
    explicit MockTransport(
        std::string_view script, std::size_t max_chunk = 1,
        std::size_t fail_after = static_cast<std::size_t>(-1)) noexcept
        : script_(script), max_chunk_(max_chunk), fail_after_(fail_after) {}

    /// @brief True iff `dst` was fully filled (short reads return false
    ///        after serving what they could — callers must loop).
    bool read_exact(std::span<std::uint8_t> dst) noexcept {
        std::size_t want = dst.size() < max_chunk_ ? dst.size() : max_chunk_;
        if (want == 0)
            return true;
        for (std::size_t i = 0; i < want; ++i) {
            if (served_ >= fail_after_ || pos_ >= script_.size())
                return false;
            dst[i] = static_cast<std::uint8_t>(script_[pos_++]);
            ++served_;
        }
        return want == dst.size();
    }

    bool write_all(std::span<const std::uint8_t> src) noexcept {
        if (written_ + src.size() > out_.size())
            return false;
        for (std::size_t i = 0; i < src.size(); ++i)
            out_[written_++] = static_cast<char>(src[i]);
        return true;
    }

    std::string_view written() const noexcept {
        return std::string_view(out_.data(), written_);
    }

    std::size_t served() const noexcept { return served_; }

  private:
    std::string_view script_;
    std::size_t max_chunk_;
    std::size_t fail_after_;
    std::size_t pos_ = 0;
    std::size_t served_ = 0;
    std::array<char, 8192> out_{};
    std::size_t written_ = 0;
};

} // namespace test
} // namespace debugd
