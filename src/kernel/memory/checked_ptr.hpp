#pragma once

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

#pragma once

#include <types.hpp>
#include <string.hpp>
#include <concepts.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/hal/io.hpp>

namespace kernel {

static constexpr uint64_t USER_SPACE_LIMIT = 0x0000800000000000ULL;

/// @brief Recovery address for safe user memory access.
///        Non-zero while we are inside a user memory copy attempt.
///        Page fault handler checks this and redirects here on fault.
extern "C" constinit uint64_t g_user_access_recover_ip;

/// @brief Check if a memory range lies entirely within user space.
/// @return true if the range is valid user-space memory.
// NOLINTBEGIN(performance-no-int-to-ptr,bugprone-narrowing-conversions)
static inline bool is_user_range(const void *user_ptr, uint64_t size) {
    if (!user_ptr)
        return false;
    uint64_t addr = reinterpret_cast<uint64_t>(user_ptr);
    if (addr >= USER_SPACE_LIMIT)
        return false;
    if (size == 0)
        return true;
    uint64_t end = addr + size - 1;
    if (end < addr)
        return false;
    if (end >= USER_SPACE_LIMIT)
        return false;
    return true;
}

/// @brief Checked pointer for safe user-space memory access.
/// @tparam T Element type (must be trivially copiable).
/// @note Provides bounds-validated load/store/copy operations that return
///       default values on failure instead of panicking.
template <kernel::TriviallyCopiable T> class CheckedPtr {
    uint64_t addr_{};
    uint64_t count_{};

  public:
    /// @brief Default constructor — null pointer, zero count.
    CheckedPtr() : addr_(0), count_(0) {
    }

    /// @brief Construct from a raw user-space pointer with element count.
    /// @param user_ptr Target user-space pointer.
    /// @param count Number of elements (default 1).
    CheckedPtr(T *user_ptr, uint64_t count = 1)
        : addr_(reinterpret_cast<uint64_t>(user_ptr)), count_(count) {
    }

    /// @brief Check if the pointer is valid (non-null, within user space).
    /// @note Zero-size operations with null pointer are valid (no-op).
    bool valid() const {
        if (count_ == 0)
            return true;
        if (addr_ == 0)
            return false;
        return is_user_range(reinterpret_cast<const void *>(addr_),
                             count_ * sizeof(T));
    }

    /// @brief Return the raw pointer without validation.
    /// @note Only safe when valid() has been checked beforehand.
    T *unsafe_ptr() const {
        return reinterpret_cast<T *>(addr_);
    }

    /// @brief Copy count elements from user space to kernel buffer.
    /// @return true on success.
    bool copy_from(T *kernel_dst) const {
        if (!valid())
            return false;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_cf);
        arch::stac();
        memcpy(kernel_dst, unsafe_ptr(), count_ * sizeof(T));
        arch::clac();
        g_user_access_recover_ip = 0;
        return true;

    recover_cf:
        arch::clac();
        g_user_access_recover_ip = 0;
        return false;
    }

    /// @brief Copy count elements from kernel buffer to user space.
    /// @return true on success.
    bool copy_to(const T *kernel_src) const {
        if (!valid())
            return false;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_ct);
        arch::stac();
        memcpy(unsafe_ptr(), kernel_src, count_ * sizeof(T));
        arch::clac();
        g_user_access_recover_ip = 0;
        return true;

    recover_ct:
        arch::clac();
        g_user_access_recover_ip = 0;
        return false;
    }

    /// @brief Read one element from user space at the given index.
    /// @return The element, or default if invalid.
    T read(size_t index = 0) const {
        if (!valid())
            return T{};
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_rd);
        arch::stac();
        T value = unsafe_ptr()[index];
        arch::clac();
        g_user_access_recover_ip = 0;
        return value;

    recover_rd:
        arch::clac();
        g_user_access_recover_ip = 0;
        return T{};
    }

    /// @brief Write one element to user space at the given index.
    /// @return true on success.
    bool write(const T &value, size_t index = 0) {
        if (!valid())
            return false;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_wr);
        arch::stac();
        unsafe_ptr()[index] = value;
        arch::clac();
        g_user_access_recover_ip = 0;
        return true;

    recover_wr:
        arch::clac();
        g_user_access_recover_ip = 0;
        return false;
    }
};

/// @brief Convenience factory for creating a CheckedPtr from a raw pointer.
template <typename T>
    requires kernel::TriviallyCopiable<T>
static inline CheckedPtr<T> checked(T *user_ptr, uint64_t count = 1) {
    return CheckedPtr<T>(user_ptr, count);
}

/// @brief Check if a user pointer points to a valid null-terminated string.
/// @return true if the string is within user space and has a null terminator.
/// @note  This function dereferences user memory.  The pointer must point
///        to mapped, readable pages.  Use is_user_range() for bounds-only
///        checks.
static inline bool is_user_string(const void *user_ptr,
                                  uint64_t max_len = 4096) {
    uint64_t addr = reinterpret_cast<uint64_t>(user_ptr);
    if (addr >= USER_SPACE_LIMIT || addr == 0)
        return false;
    uint64_t end = addr + max_len - 1;
    if (end < addr || end >= USER_SPACE_LIMIT)
        return false;
    // Check if the page is actually mapped before dereferencing.
    // After snapshot_restore clears kernel PML4 user entries, unmapped
    // user addresses would cause a Page Fault here.  virt_to_phys
    // returns 0 for unmapped pages without faulting.
    if (VMM::virt_to_phys(addr) == 0)
        return false;
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_str_chk);
    arch::stac();
    auto *p = static_cast<const volatile char *>(user_ptr);
    for (uint64_t i = 0; i < max_len; ++i) {
        if (p[i] == '\0') {
            arch::clac();
            g_user_access_recover_ip = 0;
            return true;
        }
    }
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;

recover_str_chk:
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;
}

/// @brief Copy a null-terminated string from user space to kernel buffer.
/// @return true on success, false if the source is not a valid user string.
/// @note Fault-recovery-safe: a #PF mid-loop redirects to recover_str (the
///       pointer passed is_user_string but a page beyond the probe could be
///       unmapped).  With SMAP active the loop runs with AC=1 (stac/clac).
static inline bool strncpy_from_user(char *dst, const char *src,
                                     uint64_t max_len) {
    if (!is_user_string(src, max_len))
        return false;
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_str);
    arch::stac();
    for (uint64_t i = 0; i < max_len; ++i) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            arch::clac();
            g_user_access_recover_ip = 0;
            return true;
        }
    }
    dst[max_len - 1] = '\0';
    arch::clac();
    g_user_access_recover_ip = 0;
    return true;

recover_str:
    arch::clac();
    g_user_access_recover_ip = 0;
    if (max_len > 0)
        dst[0] = '\0';
    return false;
}

/// @brief Safely copies memory from user-space to kernel buffer.
///        Uses fault recovery to handle invalid pointers gracefully.
/// @return true on success, false if a fault or range check failed.
template <kernel::TriviallyCopiable T>
static inline bool safe_copy_from_user(T *dst, const T *src, uint64_t count) {
    if (!is_user_range(src, count * sizeof(T)))
        return false;
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_from);
    arch::stac();
    memcpy(dst, src, count * sizeof(T));
    arch::clac();
    g_user_access_recover_ip = 0;
    return true;

recover_from:
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;
}

/// @brief Safely copies memory from kernel buffer to user-space.
///        Uses fault recovery to handle invalid pointers gracefully.
/// @return true on success, false if a fault or range check failed.
template <kernel::TriviallyCopiable T>
static inline bool safe_copy_to_user(T *dst, const T *src, uint64_t count) {
    if (!is_user_range(dst, count * sizeof(T)))
        return false;
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_to);
    arch::stac();
    memcpy(dst, src, count * sizeof(T));
    arch::clac();
    g_user_access_recover_ip = 0;
    return true;

recover_to:
    arch::clac();
    g_user_access_recover_ip = 0;
    return false;
}

} // namespace kernel
// NOLINTEND(performance-no-int-to-ptr,bugprone-narrowing-conversions)
