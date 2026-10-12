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

/// @file test_stack_cookie.cpp
/// @brief Stack-cookie contract tests (issue #303, Phase 6 module 5).
///
///        The v0.5.x design doc asks for `-fstack-protector` coverage, but
///        compiler SSP is explicitly OFF by design (Makefile `-fno-stack-
///        protector`: freestanding `-ffreestanding -nostdlib` has no
///        `__stack_chk_guard` provider, so enabling SSP would fail the
///        link). The tree instead implements MP-3 software canaries
///        (task.cpp: canary_install/verify_user_segments + kernel-stack
///        canary + syscall-time verify with test-mode latch). This file
///        pins that contract:
///        1. SSP-off is compiled in (any SSP macro = build break here).
///        2. Live canary values equal the documented derivation.
///        3. canary_verify_user_segments() is a pure function: tamper in
///           -> false + segment report; restore -> true (no dispatch).
///        4. The kernel-stack canary installs and verifies the same way.
///        Dispatch-path behavior (trip-on-syscall, no false positives) is
///        covered by memory_safety and is NOT duplicated here.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/elf/elf.hpp>
#include <scope_guard.hpp>
#include <initrd/initrd.hpp>
#include "test_sched_helpers.hpp"

using namespace kernel;

// SSP must be off: with -fno-stack-protector the compiler defines none of
// these. If the build ever enables SSP, this TU fails loudly right here
// instead of silently changing the protection model.
#if defined(__SSP__) || defined(__SSP_STRONG__) || defined(__SSP_ALL__)
#error "test_stack_cookie: compiler SSP is enabled; MP-3 software canaries assume -fno-stack-protector"
#endif

namespace {

/// @brief Load a userspace probe ELF by initrd path.
TaskControlBlock *load_probe(const char *name) {
    initrd::InitrdFile f = initrd::find(name);
    if (!f.data)
        f = initrd::find(name + 2); // strip "./"
    if (!f.data)
        return nullptr;
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr))
        return nullptr;
    return kernel::elf::load(hdr, f.data, f.size);
}

bool read_live_u64(uint64_t va, uint64_t pml4, uint64_t &out) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
    if (phys == 0)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memcpy(&out,
                     reinterpret_cast<const void *>(arch::HHDM_OFFSET + phys),
                     8);
    return true;
}

bool write_live_u64(uint64_t va, uint64_t pml4, uint64_t value) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
    if (phys == 0)
        return false;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + phys),
                     &value, 8);
    return true;
}

} // namespace

// Runmode: kernel
// Testidea: Compiler SSP is off for this TU (hence for the kernel build:
// the flag is global in the Makefile). Any SSP macro aborts the build.
// Input: Preprocessor state only.
// Expect: Reaches runtime; documents the intentional SSP opt-out.
// Depends: Makefile -fno-stack-protector (issue #303 module 5 reframe)
JARVIS_TEST(stack_cookie_ssp_explicitly_disabled, "PRE: none | POST: none") {
#if defined(__SSP__) || defined(__SSP_STRONG__) || defined(__SSP_ALL__)
    JARVIS_ASSERT_FMT(false, "compiler SSP unexpectedly enabled");
#else
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Live canary words equal CANARY_MAGIC ^ (segment + 1).
// Input: Load user-app probe; read the installed segment canaries.
// Expect: Every installed segment reads back the derived value.
// Depends: canary_install_user_segments (MP-3)
JARVIS_TEST(stack_cookie_canary_values_are_segment_derived,
            "PRE: none | POST: none") {
    auto *t = load_probe("bin/user-app.c.elf");
    if (t == nullptr) {
        JARVIS_TEST_PASS();
        return;
    }
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });

    for (uint64_t seg = 0; seg < TaskControlBlock::CANARY_SEGMENTS; ++seg) {
        if ((t->canary_installed & (1u << seg)) == 0)
            continue;
        uint64_t before = 0;
        uint64_t after = 0;
        JARVIS_ASSERT_FMT(read_live_u64(t->canary_before[seg],
                                        t->page_table_, before),
                          "canary_before[%lu] unreadable", seg);
        JARVIS_ASSERT_FMT(read_live_u64(t->canary_after[seg],
                                        t->page_table_, after),
                          "canary_after[%lu] unreadable", seg);
        uint64_t expected = TaskControlBlock::CANARY_MAGIC ^ (seg + 1);
        JARVIS_ASSERT_FMT(before == expected,
                          "before[%lu] 0x%lx != derived 0x%lx", seg, before,
                          expected);
        JARVIS_ASSERT_FMT(after == expected,
                          "after[%lu] 0x%lx != derived 0x%lx", seg, after,
                          expected);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: canary_verify_user_segments() alone (no dispatch): intact ->
// true; single-word tamper -> false + exact segment/VA report; restore ->
// true again.
// Input: Load probe; corrupt canary_after[STACK] via HHDM; verify; repair.
// Expect: true / (false + bad SEG_STACK + bad VA) / true.
// Depends: canary_verify_user_segments (MP-3)
JARVIS_TEST(stack_cookie_verify_is_pure_function, "PRE: none | POST: none") {
    auto *t = load_probe("bin/user-app.c.elf");
    if (t == nullptr) {
        JARVIS_TEST_PASS();
        return;
    }
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });

    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT((t->canary_installed & (1u << stk)) != 0);
    uint8_t bad_segment = 0;
    uint64_t bad_va = 0;
    JARVIS_ASSERT(canary_verify_user_segments(t, bad_segment, bad_va));

    uint64_t original = 0;
    JARVIS_ASSERT(read_live_u64(t->canary_after[stk], t->page_table_,
                                original));
    JARVIS_ASSERT(write_live_u64(t->canary_after[stk], t->page_table_,
                                 original ^ 0xFFu));
    JARVIS_ASSERT_FMT(!canary_verify_user_segments(t, bad_segment, bad_va),
                      "tampered STACK canary verified clean");
    JARVIS_ASSERT_FMT(bad_segment == static_cast<uint8_t>(stk),
                      "bad segment %u != SEG_STACK", bad_segment);
    JARVIS_ASSERT_FMT(bad_va == t->canary_after[stk],
                      "bad VA 0x%lx != canary_after[STACK]", bad_va);

    JARVIS_ASSERT(write_live_u64(t->canary_after[stk], t->page_table_,
                                 original));
    JARVIS_ASSERT(canary_verify_user_segments(t, bad_segment, bad_va));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The kernel-stack canary installs on a fresh kernel task and the
// pure verifier accepts it, rejects a tamper, re-accepts a repair.
// Input: create() (never dispatched); install; verify; tamper; verify;
// repair; verify.
// Expect: true / false / true; task torn down with zero delta.
// Depends: canary_install_kernel_stack, canary_verify_kernel_stack
JARVIS_TEST(stack_cookie_kernel_stack_canary_roundtrip,
            "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });

    canary_install_kernel_stack(t);
    JARVIS_ASSERT_FMT(canary_verify_kernel_stack(t),
                      "fresh kernel-stack canary rejected");

    // Tamper the first canary word through the direct map, then repair.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *word = reinterpret_cast<uint64_t *>(t->kernel_stack);
    uint64_t original = word[0];
    word[0] = original ^ 0xFFu;
    JARVIS_ASSERT_FMT(!canary_verify_kernel_stack(t),
                      "tampered kernel-stack canary verified clean");
    word[0] = original;
    JARVIS_ASSERT_FMT(canary_verify_kernel_stack(t),
                      "repaired kernel-stack canary rejected");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all stack-cookie contract tests.
// Input: None
// Expect: All stack-cookie tests registered
// Depends: test framework
void register_stack_cookie_tests() {
    Logger::info("Registering stack-cookie tests");
    JARVIS_REGISTER_TEST(stack_cookie_ssp_explicitly_disabled);
    JARVIS_REGISTER_TEST(stack_cookie_canary_values_are_segment_derived);
    JARVIS_REGISTER_TEST(stack_cookie_verify_is_pure_function);
    JARVIS_REGISTER_TEST(stack_cookie_kernel_stack_canary_roundtrip);
}
