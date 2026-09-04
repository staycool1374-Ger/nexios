/*
 * NexIOS RTOS — Capability-Based Access Control (CSpace)
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

/// @file shm_ring.hpp
/// @brief Shared-memory ring protocol (issue #106 Part B).  A SPSC ring
/// living in the physical frames of a FrameCap shared by two user tasks via
/// SYS_FRAME_MAP.  Page 0 holds the SharedRingHeader (magic/version/capacity +
/// cache-line-aligned head/tail); data pages 1..N-1 hold the ring payload.
/// Zero-syscall after mapping: producer/consumer access the shared frames
/// directly.  Head/tail follow the SPSCRing acquire/release protocol (single
/// producer / single consumer — never written by the other side).

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/io.hpp>

namespace kernel::shm {

/// @brief Magic identifying a valid shared ring header (0x4E4558 'NEX' +
///        0x52494E47 'RING').
constexpr uint64_t SHM_RING_MAGIC = 0x4E455852494E47ULL;
/// @brief Protocol version.
constexpr uint64_t SHM_RING_VERSION = 1;

/// @brief Shared ring header — lives at the start of page 0 of the frame cap.
///        The head (producer-write-only) and tail (consumer-write-only) are
///        cache-line aligned (64 bytes) to avoid false sharing on future SMP.
///        layout is fixed and documented (docs/specs/shm.md).
struct alignas(64) SharedRingHeader {
    /// @brief Magic for sanity (0 when not initialised).
    uint64_t magic = 0;
    /// @brief Protocol version.
    uint64_t version = 0;
    /// @brief Ring capacity in bytes = (frame_count - 1) * PAGE_SIZE
    ///        (must be a power of two — see shm_ring_init).
    uint64_t capacity = 0;
    /// @brief Element size in bytes (fixed-size elements).
    uint64_t element_size = 0;
    /// @brief Producer index (bytes).  Written only by the producer.
    alignas(64) uint64_t head = 0;
    /// @brief Consumer index (bytes).  Written only by the consumer.
    alignas(64) uint64_t tail = 0;
};

/// @brief Initialises a shared ring header at @p header (must be the start of
///        page 0 of a frame cap with @p frame_count pages).  @p element_size
///        must be a positive multiple of 8.  The ring data capacity is
///        (frame_count - 1) * PAGE_SIZE; it is rounded DOWN to a power of two
///        so the head/tail wrap mask works.  Called by the producer once,
///        before either side maps/uses the ring.
/// @param header      Pointer to the header area (page-0 start).
/// @param frame_count Number of pages in the backing FrameCap (>= 2).
/// @param element_size Fixed element size in bytes.
inline void shm_ring_init(SharedRingHeader *header, uint64_t frame_count,
                          uint64_t element_size) {
    if (!header || frame_count < 2 || element_size == 0 ||
        (element_size % 8) != 0)
        return;
    uint64_t raw = (frame_count - 1) * arch::PAGE_SIZE;
    // Round down to a power of two.
    uint64_t cap = 1;
    while (cap * 2 <= raw)
        cap *= 2;
    __atomic_store_n(&header->magic, SHM_RING_MAGIC, __ATOMIC_RELEASE);
    __atomic_store_n(&header->version, SHM_RING_VERSION, __ATOMIC_RELEASE);
    __atomic_store_n(&header->capacity, cap, __ATOMIC_RELEASE);
    __atomic_store_n(&header->element_size, element_size, __ATOMIC_RELEASE);
    __atomic_store_n(&header->head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->tail, 0, __ATOMIC_RELEASE);
}

/// @brief Whether @p header points at an initialised ring.
inline bool shm_ring_valid(const SharedRingHeader *header) {
    return header &&
           __atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) == SHM_RING_MAGIC &&
           __atomic_load_n(&header->version, __ATOMIC_ACQUIRE) ==
               SHM_RING_VERSION;
}

/// @brief Whether the ring has room for one more element.
inline bool shm_ring_has_space(const SharedRingHeader *header) {
    if (!header)
        return false;
    uint64_t cap = __atomic_load_n(&header->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&header->element_size, __ATOMIC_ACQUIRE);
    if (cap == 0 || esz == 0)
        return false;
    uint64_t h = __atomic_load_n(&header->head, __ATOMIC_RELAXED);
    uint64_t t = __atomic_load_n(&header->tail, __ATOMIC_ACQUIRE);
    uint64_t used = (h >= t) ? (h - t) : (h + cap - t);
    return used + esz <= cap;
}

/// @brief Whether the ring has at least one element.
inline bool shm_ring_has_data(const SharedRingHeader *header) {
    if (!header)
        return false;
    uint64_t h = __atomic_load_n(&header->head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&header->tail, __ATOMIC_RELAXED);
    return h != t;
}

/// @brief Data pointer for element @p idx (0-based) at @p data_base (start of
///        page 1).  Caller must have verified space/data via the has_* helpers.
inline uint8_t *shm_ring_elem(const SharedRingHeader *header,
                              uint8_t *data_base, uint64_t idx) {
    uint64_t cap = __atomic_load_n(&header->capacity, __ATOMIC_ACQUIRE);
    uint64_t esz = __atomic_load_n(&header->element_size, __ATOMIC_ACQUIRE);
    uint64_t off = (idx * esz) & (cap - 1);
    return data_base + off;
}

/// @brief Producer push: copy @p src (element_size bytes) into the ring at the
///        head.  Returns false when full or not initialised.  Head is
///        published with release after the payload write (SPSC protocol).
inline bool shm_ring_push(SharedRingHeader *header, uint8_t *data_base,
                          const uint8_t *src) {
    if (!header || !data_base || !src)
        return false;
    if (!shm_ring_has_space(header))
        return false;
    uint64_t esz = __atomic_load_n(&header->element_size, __ATOMIC_ACQUIRE);
    uint64_t h = __atomic_load_n(&header->head, __ATOMIC_RELAXED);
    uint8_t *dst = shm_ring_elem(header, data_base, h / esz);
    for (uint64_t i = 0; i < esz; ++i)
        dst[i] = src[i];
    __atomic_store_n(&header->head, h + esz, __ATOMIC_RELEASE);
    return true;
}

/// @brief Consumer pop: copy the element at the tail into @p dst.
///        Returns false when empty or not initialised.  Tail is published with
///        release after the payload read (SPSC protocol).
inline bool shm_ring_pop(SharedRingHeader *header, uint8_t *data_base,
                         uint8_t *dst) {
    if (!header || !data_base || !dst)
        return false;
    if (!shm_ring_has_data(header))
        return false;
    uint64_t esz = __atomic_load_n(&header->element_size, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&header->tail, __ATOMIC_RELAXED);
    const uint8_t *src = shm_ring_elem(header, data_base, t / esz);
    for (uint64_t i = 0; i < esz; ++i)
        dst[i] = src[i];
    __atomic_store_n(&header->tail, t + esz, __ATOMIC_RELEASE);
    return true;
}

} // namespace kernel::shm