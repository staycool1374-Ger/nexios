/*
 * NexIOS RTOS — Sampling Profiler Header
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

/// @file sampler.hpp
/// @brief Statistical sampling profiler interface.

#pragma once

#include <types.hpp>

namespace kernel {
namespace profiling {

class Sampler {
public:
    /// @brief Initialize the sampler (called at boot).
    static void init();

    /// @brief Set sampling rate: sample every N timer ticks.
    /// @param every_n_ticks Sample interval in timer ticks (default 10).
    static void set_sample_rate(uint32_t every_n_ticks);

    /// @brief Enable or disable sampling.
    /// @param enabled true to enable, false to disable.
    static void set_enabled(bool enabled);

    /// @brief Check if sampler is enabled.
    /// @return true if enabled.
    static bool is_enabled();

    /// @brief Add a function range to the symbol table.
    /// @param start Function start address (inclusive).
    /// @param end Function end address (exclusive).
    static void add_function(uint64_t start, uint64_t end);

    /// @brief Record a sample (called from timer IRQ context).
    /// Only records at configured rate.
    /// @param ip Instruction pointer to record.
    static void record_sample(uint64_t ip);

    /// @brief Dump all collected samples to serial port (COM1).
    /// Format: SMPL <count> <ip1> <ip2> ...
    static void dump_to_serial();

    /// @brief Get total sample count.
    /// @return Number of samples collected.
    static size_t get_sample_count();

    /// @brief Clear all samples.
    static void clear();

    /// @brief Register functions from ELF symbol table.
    /// @param symtab_data Raw symbol table data.
    /// @param symtab_size Size of symbol table data.
    static void register_from_symbol_table(const uint8_t* symtab_data, size_t symtab_size);

    /// @brief Find function index containing given IP.
    /// @param ip Instruction pointer.
    /// @return Function index or UINT32_MAX if not found.
    static uint32_t find_function(uint64_t ip);

private:
    Sampler() = delete;
};

} // namespace profiling
} // namespace kernel