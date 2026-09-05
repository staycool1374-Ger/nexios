/*
 * NexIOS RTOS — Sampling Profiler
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

/// @file sampler.cpp
/// @brief Statistical sampling profiler — timer-interrupt PC sampling.
/// Lightweight alternative to -finstrument-functions: records instruction
/// pointers at configurable sampling rate without perturbing scheduler timing.

#include <kernel/profiling/sampler.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/idt.hpp>
#include <kernel/core/global_state.hpp>

namespace kernel {
namespace profiling {

// Sampling buffer - power of 2 for fast modulo
static constexpr size_t SAMPLE_BUFFER_SIZE = 4096;
static uint64_t sample_buffer[SAMPLE_BUFFER_SIZE];
static volatile size_t sample_head = 0;
static volatile size_t sample_count = 0;

// Configuration
static volatile uint32_t sample_every_n_ticks = 10; // Sample every N timer ticks
static volatile bool sampler_enabled = false;

// Function address lookup (populated from ELF symbols at boot)
static constexpr size_t MAX_FUNCTIONS = 16384;
static uint64_t func_starts[MAX_FUNCTIONS];
static uint64_t func_ends[MAX_FUNCTIONS];
static uint32_t func_count = 0;

void Sampler::init() {
    sample_head = 0;
    sample_count = 0;
    sampler_enabled = false;
    sample_every_n_ticks = 10; // Default: sample every 10 ticks (~1000 Hz / 10 = 100 samples/sec)
}

void Sampler::set_sample_rate(uint32_t every_n_ticks) {
    sample_every_n_ticks = every_n_ticks ? every_n_ticks : 1;
}

void Sampler::set_enabled(bool enabled) {
    sampler_enabled = enabled;
}

bool Sampler::is_enabled() {
    return sampler_enabled;
}

void Sampler::add_function(uint64_t start, uint64_t end) {
    if (func_count < MAX_FUNCTIONS) {
        func_starts[func_count] = start;
        func_ends[func_count] = end;
        func_count++;
    }
}

void Sampler::record_sample(uint64_t ip) {
    if (!sampler_enabled) return;
    
    // Guard against uninitialized state - use safe division
    // Use volatile read to prevent optimization issues
    uint32_t rate = sample_every_n_ticks;
    if (rate <= 1) {
        // rate == 0 or rate == 1: sample every tick, no modulo needed
    } else {
        // rate > 1: sample at interval, use modulo with safe check
        uint64_t ticks = arch::Timer::ticks();
        if (ticks % rate != 0) return;
    }
    
    // Store sample atomically
    size_t idx = __atomic_fetch_add(&sample_head, 1, __ATOMIC_RELAXED) % SAMPLE_BUFFER_SIZE;
    sample_buffer[idx] = ip;
    __atomic_fetch_add(&sample_count, 1, __ATOMIC_RELAXED);
}

void Sampler::dump_to_serial() {
#if defined(CONFIG_ARCH_X86_64)
    // Output format: SMPL <count> <ip1> <ip2> ...
    arch::outb(arch::COM1, 'S');
    arch::outb(arch::COM1, 'M');
    arch::outb(arch::COM1, 'P');
    arch::outb(arch::COM1, 'L');
    
    size_t count = sample_count;
    if (count > SAMPLE_BUFFER_SIZE) count = SAMPLE_BUFFER_SIZE;
    
    // Output count as 4 bytes little-endian
    for (int i = 0; i < 4; i++) {
        while ((arch::inb(arch::COM1 + 5) & 0x20) == 0) ;
        arch::outb(arch::COM1, (count >> (i * 8)) & 0xFF);
    }
    
    // Output samples (each as 8 bytes little-endian)
    size_t start = (sample_head > count) ? sample_head - count : 0;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % SAMPLE_BUFFER_SIZE;
        uint64_t ip = sample_buffer[idx];
        for (int j = 0; j < 8; j++) {
            while ((arch::inb(arch::COM1 + 5) & 0x20) == 0) ;
            arch::outb(arch::COM1, (ip >> (j * 8)) & 0xFF);
        }
    }
#else
    (void)sample_count;
    (void)sample_head;
    (void)sample_buffer;
#endif
}

size_t Sampler::get_sample_count() {
    return sample_count;
}

void Sampler::clear() {
    sample_head = 0;
    sample_count = 0;
}

void Sampler::register_from_symbol_table(const uint8_t* symtab_data, size_t symtab_size) {
    // Parse symbol table entries (simple format: start,end,name\0)
    // This is called during boot from kernel.cpp after ELF parsing
    const char* data = reinterpret_cast<const char*>(symtab_data);
    const char* end = data + symtab_size;
    
    while (data < end && func_count < MAX_FUNCTIONS) {
        uint64_t start = 0, end_addr = 0;
        
        // Parse: start,end,name\0
        // Simple ASCII hex parsing
        while (data < end && (*data == ' ' || *data == '\n' || *data == '\t')) data++;
        if (data >= end) break;
        
        // Parse start
        while (data < end && *data != ',') {
            char c = *data++;
            if (c >= '0' && c <= '9') start = start * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') start = start * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') start = start * 16 + (c - 'A' + 10);
        }
        if (data >= end) break;
        data++; // skip ','
        
        // Parse end
        while (data < end && *data != ',') {
            char c = *data++;
            if (c >= '0' && c <= '9') end_addr = end_addr * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') end_addr = end_addr * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') end_addr = end_addr * 16 + (c - 'A' + 10);
        }
        if (data >= end) break;
        data++; // skip ','
        
        // Parse name (skip it, we don't store names yet)
        while (data < end && *data != '\0') data++;
        if (data >= end) break;
        data++; // skip '\0'
        
        if (start != 0 && end_addr > start) {
            func_starts[func_count] = start;
            func_ends[func_count] = end_addr;
            func_count++;
        }
    }
}

// Find function containing address, return index or UINT32_MAX
uint32_t Sampler::find_function(uint64_t ip) {
    for (uint32_t i = 0; i < func_count; i++) {
        if (ip >= func_starts[i] && ip < func_ends[i]) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace profiling
} // namespace kernel