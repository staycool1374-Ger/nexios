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

/// @file test_stubs.cpp
/// @brief Stub test registration functions for architectures that lack x86-64
///        features (PCI, VirtIO, FPU, GDT, IDT, PIC, etc.).

#if !defined(CONFIG_ARCH_X86_64)

#include <logger.hpp>

/// @brief Stub — not supported on non-x86 architectures.
void register_pci_tests() {
    kernel::Logger::info(
        "Skipped: register_pci_tests (not supported on this arch)");
}

/// @brief Stub — PML4 page-table tests are x86_64-specific.
void register_pml4_clone_tests() {
    kernel::Logger::info(
        "Skipped: register_pml4_clone_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_virtio_tests() {
    kernel::Logger::info(
        "Skipped: register_virtio_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_fpu_tests() {
    kernel::Logger::info(
        "Skipped: register_fpu_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_fpu_sse_tests() {
    kernel::Logger::info(
        "Skipped: register_fpu_sse_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_fpu_clone_tests() {
    kernel::Logger::info(
        "Skipped: register_fpu_clone_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_fpu_multi_tests() {
    kernel::Logger::info(
        "Skipped: register_fpu_multi_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_fpu_xmm_all_tests() {
    kernel::Logger::info(
        "Skipped: register_fpu_xmm_all_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_task_tests() {
    kernel::Logger::info(
        "Skipped: register_task_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_process_tests() {
    kernel::Logger::info(
        "Skipped: register_process_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_gdt_tests() {
    kernel::Logger::info(
        "Skipped: register_gdt_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_idt_tests() {
    kernel::Logger::info(
        "Skipped: register_idt_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_pic_tests() {
    kernel::Logger::info(
        "Skipped: register_pic_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_rtc_tests() {
    kernel::Logger::info(
        "Skipped: register_rtc_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_shell_interaction_tests() {
    kernel::Logger::info(
        "Skipped: register_shell_interaction_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_buildsystem_tests() {
    kernel::Logger::info(
        "Skipped: register_buildsystem_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_block_device_tests() {
    kernel::Logger::info(
        "Skipped: register_block_device_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_vfsd_authorization_tests() {
    kernel::Logger::info(
        "Skipped: register_vfsd_authorization_tests (not supported on this arch)");
}

/// @brief Stub — not supported on non-x86 architectures.
void register_buffer_pool_tests() {
    kernel::Logger::info(
        "Skipped: register_buffer_pool_tests (not supported on this arch)");
}

#endif // !defined(CONFIG_ARCH_X86_64)
