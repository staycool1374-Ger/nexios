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

/// @file urc.cpp
/// @brief Bounded prefix-registered async console queue (issue #407 item 2).
///        FIFO of frozen "[PREFIX] payload" lines; producers emit (ISR-safe),
///        only the shell task pops at prompt redraw.  Drop-newest on full
///        with a counted notice; unknown tag / null line / sealed table
///        are counted rejects, never ENSURE.  Static storage only.
///        Lock order: urc lock is a leaf (scheduler_lock_ -> urc lock is
///        the only allowed nesting; no call out of the guarded region).

#include <kernel/console/urc.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>

namespace kernel::console::urc {

namespace {

// One frozen line (content chars + NUL).
struct Slot {
    char text[kLineMax] = {};
    uint8_t len = 0;
    bool used = false;
};

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
static sync::SpinLock s_lock{};
static Slot s_slots[kQueueDepth] = {};
static size_t s_head = 0;
static size_t s_tail = 0;
static size_t s_count = 0;
static char s_prefix[kMaxPrefixes][kPrefixMax] = {};
static bool s_prefix_used[kMaxPrefixes] = {};
static bool s_sealed = false;
static size_t s_dropped = 0;
static size_t s_dropped_seen = 0;
static size_t s_truncated = 0;
static size_t s_rejected = 0;

/// @brief Seed the compile-time prefix table (idempotent).
///        Must run under s_lock.
void seed_prefixes() {
    static const char *const kSeed[] = {"ELF", "KILL", "TST"};
    static constexpr size_t kSeedCount = 3;
    static_assert(kSeedCount <= static_cast<size_t>(Tag::Count),
                  "urc seed table exceeds Tag::Count");
    static_assert(kSeedCount <= kMaxPrefixes, "urc seeds exceed table");
    for (size_t idx = 0; idx < kSeedCount; ++idx) {
        size_t pos = 0;
        for (const char *src = kSeed[idx];
             *src != '\0' && pos < kPrefixMax - 1; ++src)
            s_prefix[idx][pos++] = *src;
        s_prefix[idx][pos] = '\0';
        s_prefix_used[idx] = true;
    }
}

} // namespace

bool emit(Tag tag, const char *line) {
    sync::IrqSpinLockGuard guard(s_lock);
    if (!s_prefix_used[0])
        seed_prefixes();
    if (line == nullptr) {
        ++s_rejected;
        return false;
    }
    const auto id = static_cast<uint8_t>(tag);
    if (id >= static_cast<uint8_t>(Tag::Count) || !s_prefix_used[id]) {
        ++s_rejected;
        return false;
    }
    if (s_count >= kQueueDepth) {
        ++s_dropped; // drop-newest: the queued prefix of history survives
        return false;
    }
    Slot *slot = &s_slots[s_tail];
    size_t pos = 0;
    slot->text[pos++] = '[';
    for (size_t idx = 0; s_prefix[id][idx] != '\0' && pos < kLineMax - 1;
         ++idx)
        slot->text[pos++] = s_prefix[id][idx];
    slot->text[pos++] = ']';
    if (pos < kLineMax - 1)
        slot->text[pos++] = ' ';
    bool truncated = false;
    for (const char *src = line; *src != '\0'; ++src) {
        if (pos >= kLineMax - 1) {
            truncated = true;
            break;
        }
        slot->text[pos++] = *src;
    }
    if (truncated) {
        slot->text[kLineMax - 2] = '.';
        slot->text[kLineMax - 3] = '.';
        slot->text[kLineMax - 4] = '.';
        ++s_truncated;
    }
    slot->text[pos < kLineMax ? pos : kLineMax - 1] = '\0';
    slot->len = static_cast<uint8_t>(pos < kLineMax ? pos : kLineMax - 1);
    slot->used = true;
    s_tail = (s_tail + 1) % kQueueDepth;
    ++s_count;
    return true;
}

bool pop(char *out, size_t cap) {
    if (out == nullptr || cap == 0)
        return false;
    sync::IrqSpinLockGuard guard(s_lock);
    if (s_count == 0)
        return false;
    Slot *slot = &s_slots[s_head];
    size_t take = slot->len;
    if (take > cap - 1)
        take = cap - 1;
    for (size_t idx = 0; idx < take; ++idx)
        out[idx] = slot->text[idx];
    out[take] = '\0';
    slot->used = false;
    slot->len = 0;
    s_head = (s_head + 1) % kQueueDepth;
    --s_count;
    return true;
}

bool pending() {
    sync::IrqSpinLockGuard guard(s_lock);
    return s_count != 0;
}

size_t take_dropped() {
    sync::IrqSpinLockGuard guard(s_lock);
    const size_t delta = s_dropped - s_dropped_seen;
    s_dropped_seen = s_dropped;
    return delta;
}

size_t dropped_count() {
    sync::IrqSpinLockGuard guard(s_lock);
    return s_dropped;
}

size_t truncated_count() {
    sync::IrqSpinLockGuard guard(s_lock);
    return s_truncated;
}

size_t rejected_count() {
    sync::IrqSpinLockGuard guard(s_lock);
    return s_rejected;
}

uint8_t register_prefix(const char *prefix) {
    if (prefix == nullptr)
        return kNoPrefix;
    sync::IrqSpinLockGuard guard(s_lock);
    if (!s_prefix_used[0])
        seed_prefixes();
    if (s_sealed) {
        ++s_rejected;
        return kNoPrefix;
    }
    for (uint8_t id = 0; id < kMaxPrefixes; ++id) {
        if (!s_prefix_used[id]) {
            size_t pos = 0;
            for (const char *src = prefix;
                 *src != '\0' && pos < kPrefixMax - 1; ++src)
                s_prefix[id][pos++] = *src;
            s_prefix[id][pos] = '\0';
            s_prefix_used[id] = true;
            return id;
        }
    }
    ++s_rejected;
    return kNoPrefix;
}

void seal() {
    sync::IrqSpinLockGuard guard(s_lock);
    s_sealed = true;
}

bool sealed() {
    sync::IrqSpinLockGuard guard(s_lock);
    return s_sealed;
}

void reset() {
    sync::IrqSpinLockGuard guard(s_lock);
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    for (size_t idx = 0; idx < kQueueDepth; ++idx) {
        s_slots[idx].used = false;
        s_slots[idx].len = 0;
    }
    for (size_t idx = static_cast<size_t>(Tag::Count); idx < kMaxPrefixes;
         ++idx)
        s_prefix_used[idx] = false;
    s_sealed = false;
    s_dropped = 0;
    s_dropped_seen = 0;
    s_truncated = 0;
    s_rejected = 0;
}

} // namespace kernel::console::urc
