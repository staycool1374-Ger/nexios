/// @file priority_map.hpp
/// @brief Two-level bitmap for O(1) highest-priority lookup.

#pragma once

#include <types.hpp>
#include <kernel/nexios_config.h>
#include <kernel/arch/hal/bits.hpp>

namespace kernel {

/// @brief Two-level bitmap supporting O(1) highest-priority find.
/// Supports up to 128 priority levels (0–127) as two 64-bit words.
class PriorityMap {
    static_assert(CONFIG_PRIORITY_CEILING <= 128,
                  "O(1) scheduler supports at most 128 priority levels");

    uint64_t bitmap_hi_; ///< Bitmap for priorities 64–127
    uint64_t bitmap_lo_; ///< Bitmap for priorities 0–63

  public:
    /// @brief Constructs an empty PriorityMap.
    PriorityMap() : bitmap_hi_(0), bitmap_lo_(0) {
    }

    /// @brief Sets the bit for a given priority level.
    void set(uint64_t prio) noexcept {
        if (prio >= 64) {
            bitmap_hi_ |= (1ULL << (prio - 64));
        } else {
            bitmap_lo_ |= (1ULL << prio);
        }
    }

    /// @brief Clears the bit for a given priority level.
    void clear(uint64_t prio) noexcept {
        if (prio >= 64) {
            bitmap_hi_ &= ~(1ULL << (prio - 64));
        } else {
            bitmap_lo_ &= ~(1ULL << prio);
        }
    }

    /// @brief Returns the highest set priority level, or 0 if none.
    uint64_t get_highest_priority() const noexcept {
        if (bitmap_hi_) {
            return 64 + hal::bits::find_highest_bit(bitmap_hi_);
        }
        if (bitmap_lo_) {
            return hal::bits::find_highest_bit(bitmap_lo_);
        }
        return 0;
    }

    /// @brief Highest set level in [1, prio) — the starvation-breaker
    ///        window (issue #242).  Level 0 is the idle band (forcing the
    ///        idle task can never relieve a starved supervisor), so it is
    ///        excluded by construction.  O(1): two masked word ops.
    /// @return Level in [1, prio), or STARVABLE_NONE when no such level
    ///         is set.
    static constexpr uint64_t STARVABLE_NONE = ~0ULL;
    uint64_t highest_starvable_below(uint64_t prio) const noexcept {
        if (prio <= 1) {
            return STARVABLE_NONE;
        }
        uint64_t lo_mask = (prio >= 64)
                               ? 0xFFFFFFFFFFFFFFFEULL
                               : (((1ULL << prio) - 1ULL) & ~1ULL);
        uint64_t hi_mask =
            (prio <= 64) ? 0ULL : ((1ULL << (prio - 64)) - 1ULL);
        uint64_t masked_hi = bitmap_hi_ & hi_mask;
        if (masked_hi) {
            return 64 + hal::bits::find_highest_bit(masked_hi);
        }
        uint64_t masked_lo = bitmap_lo_ & lo_mask;
        if (masked_lo) {
            return hal::bits::find_highest_bit(masked_lo);
        }
        return STARVABLE_NONE;
    }

    /// @brief Returns true if the given priority level is set.
    bool is_set(uint64_t prio) const noexcept {
        if (prio >= 64) {
            return (bitmap_hi_ & (1ULL << (prio - 64))) != 0;
        }
        return (bitmap_lo_ & (1ULL << prio)) != 0;
    }

    /// @brief Returns the raw upper 64-bit word.
    uint64_t raw_hi() const noexcept {
        return bitmap_hi_;
    }
    /// @brief Returns the raw lower 64-bit word.
    uint64_t raw_lo() const noexcept {
        return bitmap_lo_;
    }

    /// @brief Restores bitmap state from raw words (for snapshot restore).
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void set_raw(uint64_t hi, uint64_t lo) noexcept {
        bitmap_hi_ = hi;
        bitmap_lo_ = lo;
    }

    /// @brief Returns true if no priority levels are set.
    bool empty() const noexcept {
        return bitmap_hi_ == 0 && bitmap_lo_ == 0;
    }

    /// @brief Clears all priority levels.
    void clear_all() noexcept {
        bitmap_hi_ = 0;
        bitmap_lo_ = 0;
    }
};

} // namespace kernel
