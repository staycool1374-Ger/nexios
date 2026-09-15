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

/// @file shootdown_ipi.cpp
/// @brief Batched TLB-shootdown IPI delivery (issue #159, x86_64 only).

#include <kernel/arch/x86_64/hal/shootdown_ipi.hpp>
#include <kernel/arch/x86_64/hal/apic.hpp>
#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/x86_64/hal/page_table_impl.hpp>

namespace arch {

ShootdownBatch::CpuSlot ShootdownBatch::slots_[CONFIG_MAX_CPUS] = {};
uint64_t ShootdownBatch::sends_ = 0;
uint64_t ShootdownBatch::applied_ = 0;
uint64_t ShootdownBatch::applied_log_[ShootdownBatch::APPLIED_LOG] = {};
uint64_t ShootdownBatch::applied_log_count_ = 0;

bool ShootdownBatch::queue_remote(uint64_t cpu, uint64_t va,
                                  uint16_t pcid) {
    if (cpu >= CONFIG_MAX_CPUS)
        return false;
    if ((va & (ArchPageTable::PAGE_SIZE - 1)) != 0)
        return false;
    // Single producer per target slot at a time (the flushing caller
    // owns it; mirrors the mailbox single-publisher discipline).  The
    // guard freezes the LOCAL ISR so it cannot interleave the fill.
    // Insertion keeps the slot sorted ascending by VA (bounded
    // insertion shift) so every batch applies deterministically.
    arch::IrqGuard irq_guard{};
    CpuSlot &slot = slots_[cpu];
    uint64_t count =
        __atomic_load_n(&slot.count, __ATOMIC_ACQUIRE);
    if (count >= BATCH_SLOTS)
        return false;
    uint64_t pos = count;
    while (pos > 0 && slot.entries[pos - 1].va > va) {
        slot.entries[pos] = slot.entries[pos - 1];
        --pos;
    }
    slot.entries[pos].va = va;
    slot.entries[pos].pcid = pcid;
    __atomic_store_n(&slot.count, count + 1, __ATOMIC_RELEASE);
    return true;
}

uint64_t ShootdownBatch::flush_target(uint64_t cpu) {
    if (cpu >= CONFIG_MAX_CPUS)
        return 0;
    uint32_t lapic = (cpu == cpu_index()) ? APIC::lapic_id()
                                          : per_cpu[cpu].lapic_id;
    uint64_t sends = 0;
    CpuSlot &slot = slots_[cpu];
    // NOTE: callers must not hold IF=0 across this call for a self
    // target (the delivery ISR cannot land until sti).
    for (;;) {
        if (__atomic_load_n(&slot.count, __ATOMIC_ACQUIRE) == 0)
            break;
        if (!APIC::send_ipi(lapic, APIC::SHOOTDOWN_BATCH_VECTOR,
                            APIC::IpiMode::FIXED))
            break; // entries stay queued
        __atomic_fetch_add(&sends_, 1ULL, __ATOMIC_RELAXED);
        ++sends;
        // Wait for the serving CPU to drain (bounded: a down target
        // never drains — keep the rest queued instead of spinning).
        uint64_t spins = 0;
        while (__atomic_load_n(&slot.count, __ATOMIC_ACQUIRE) != 0) {
            if (++spins > 1000000ULL)
                return sends;
            arch::pause();
        }
    }
    return sends;
}

void ShootdownBatch::handle_cpu(uint64_t cpu) {
    if (cpu >= CONFIG_MAX_CPUS)
        return;
    // Runs IF=0 (ISR): no locks, no allocation.  CAS-clear so a
    // concurrent producer append is never wiped (retry covers the
    // delta; bounded — see below).
    CpuSlot &slot = slots_[cpu];
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint64_t count =
            __atomic_load_n(&slot.count, __ATOMIC_ACQUIRE);
        if (count == 0)
            return;
        if (count > BATCH_SLOTS)
            count = BATCH_SLOTS;
        for (uint64_t i = 0; i < count; ++i) {
            ArchPageTable::tlb_flush(slot.entries[i].va);
            if (applied_log_count_ < APPLIED_LOG) {
                applied_log_[applied_log_count_] = slot.entries[i].va;
                ++applied_log_count_;
            }
        }
        __atomic_fetch_add(&applied_, count, __ATOMIC_RELAXED);
        uint64_t expect = count;
        if (__atomic_compare_exchange_n(&slot.count, &expect, 0ULL,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return;
        }
        // Lost the race (producer appended meanwhile): loop covers the
        // delta; after 3 attempts the remainder stays queued for the
        // next flush — never lost, never unbounded here.
    }
}

uint64_t ShootdownBatch::sends_for_test() {
    return __atomic_load_n(&sends_, __ATOMIC_RELAXED);
}

uint64_t ShootdownBatch::applied_for_test() {
    return __atomic_load_n(&applied_, __ATOMIC_RELAXED);
}

uint64_t ShootdownBatch::applied_va_for_test(uint64_t idx) {
    if (idx >= APPLIED_LOG)
        return 0;
    return applied_log_[idx];
}

void ShootdownBatch::reset_for_test() {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        slots_[c].count = 0;
        for (size_t i = 0; i < BATCH_SLOTS; ++i) {
            slots_[c].entries[i].va = 0;
            slots_[c].entries[i].pcid = 0;
        }
    }
    sends_ = 0;
    applied_ = 0;
    applied_log_count_ = 0;
    for (size_t i = 0; i < APPLIED_LOG; ++i)
        applied_log_[i] = 0;
}

} // namespace arch
