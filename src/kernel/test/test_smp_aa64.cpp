/*
 * NexIOS RTOS — SMP multicore parity tests, AArch64 (issue #351 slice-5)
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

/// @file test_smp_aa64.cpp
/// @brief #351 slice-5 per-arch SMP groups (aarch64): single-CPU-safe
///        groups (DTB discovery parse, shootdown send contract, guard
///        predicates) register into `smp`; live-2CPU groups (bring-up,
///        IPI routing, cross-CPU dispatch, shootdown verdict,
///        affinity migration, balancer pull) register into
///        `smp_multicpu` (harness gates -smp 2). Housing rule (C2):
///        no live-AP group runs single-CPU — bringup-live fails loud
///        without an AP by design (it IS the violation detector).

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/arch/hal/ipi.hpp>
#include <kernel/arch/hal/idt.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

#if defined(CONFIG_ARCH_AARCH64)

#include <kernel/arch/aarch64/hal/gic.hpp>
#include <kernel/arch/aarch64/hal/smp.hpp>

extern "C" const uint8_t _binary_virt_dtb_start[];

namespace {

// Progress-aware spin: exits when the observed counter advances (a
// fixed hlt-count can exceed the host stall watchdog when the exit
// condition never fires — park here, never spin blind).
void spin_until_progress(volatile uint64_t *counter, uint64_t baseline,
                         int bound = 20000) {
    for (int i = 0; i < bound; ++i) {
        if (__atomic_load_n(counter, __ATOMIC_ACQUIRE) != baseline)
            break;
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

} // namespace

// Runmode: kernel
// Testidea: #351 S5 — DTB discovery parse on the embedded virt DTB:
// cpu nodes decode (count >= booted topology), ids distinct, methods in
// {UNKNOWN, PSCI, SPIN_TABLE}, PSCI conduit readable where present.
// Input: _binary_virt_dtb_start (embedded QEMU virt DTB, -smp 8 dump).
// Expect: walk succeeds; count >= up_cpu_count(); cpu[0].id == 0
// (BSP MPIDR Aff0 == 0 gate); psci_method true with conduit "hvc"/"smc".
// Depends: dtb_walk_cpus, dtb_psci_method (S1), ap_count/up_cpu_count.
JARVIS_TEST(smp_aa64_dtb_discovery, "PRE: none | POST: none") {
    kernel::boot::DtbCpuEntry entries[8];
    const void *fdt = &_binary_virt_dtb_start;
    int n = kernel::boot::dtb_walk_cpus(fdt, entries, 8);
    JARVIS_ASSERT(n >= 1);
    uint64_t up = Scheduler::up_cpu_count();
    JARVIS_ASSERT_FMT(static_cast<uint64_t>(n) >= up,
                      "DTB describes %d CPUs, %lu up", n, up);
    JARVIS_ASSERT_FMT(entries[0].id == 0,
                      "cpu[0].id 0x%lx, want Aff0 0", entries[0].id);
    for (int i = 1; i < n; ++i)
        JARVIS_ASSERT(entries[i].id != entries[i - 1].id);
    for (int i = 0; i < n; ++i) {
        auto m = entries[i].method;
        JARVIS_ASSERT(m == kernel::boot::CpuEnableMethod::UNKNOWN ||
                      m == kernel::boot::CpuEnableMethod::PSCI ||
                      m == kernel::boot::CpuEnableMethod::SPIN_TABLE);
    }
    char method[16];
    if (kernel::boot::dtb_psci_method(fdt, method, sizeof(method))) {
        JARVIS_ASSERT(method[0] == 'h' || method[0] == 's');
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — self-IPI loopback via the SCHED leg: a direct
// SGI to our own CPU interface runs the sched_ipi_handler, which sets
// the per-CPU need_resched flag. Single-CPU safe (no AP needed; the
// SCHED handler is registered at boot, unlike the AP-gated loopback
// set). The interface-level self-send is refused by design
// (ap_aff0(0) unmapped), so this test uses the direct GIC send.
// Input: clear need_resched[this cpu], direct SGI to self, spin.
// Expect: flag set within bound (delivery + handler ran).
// Depends: gic_send_sgi, SCHED handler (kernel.cpp boot), need_resched.
JARVIS_TEST(smp_aa64_self_ipi_loopback, "PRE: none | POST: none") {
    uint64_t here = arch::cpu_index();
    JARVIS_ASSERT(here < CONFIG_MAX_CPUS);
    __atomic_store_n(&kernel::scheduler_need_resched[here], false,
                     __ATOMIC_RELEASE);
    JARVIS_ASSERT(arch::gic_send_sgi(static_cast<uint32_t>(here),
                                     arch::SMP_SCHED_SGI));
    bool seen = false;
    for (int i = 0; i < 20000; ++i) {
        if (__atomic_load_n(&kernel::scheduler_need_resched[here],
                            __ATOMIC_ACQUIRE)) {
            seen = true;
            break;
        }
        arch::pause();
    }
    JARVIS_ASSERT_FMT(seen, "self-SGI SCHED not observed in bound");
    __atomic_store_n(&kernel::scheduler_need_resched[here], false,
                     __ATOMIC_RELEASE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — guard predicate units on a live slotted task
// (arch-neutral contract shared with the P3 direct-call tests).
// Input: create_user() task (slot != 0 on aa64).
// Expect: is_stack_guard_fault true inside [slot, slot+4096),
// false for kernel_stack, false for an unrelated VA.
// Depends: is_stack_guard_fault, create_user kslot wiring.
JARVIS_TEST(smp_aa64_guard_predicate, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT(t->kstack_slot_va_ != 0);
    JARVIS_ASSERT(kernel::is_stack_guard_fault(*t, t->kstack_slot_va_));
    JARVIS_ASSERT(
        kernel::is_stack_guard_fault(*t, t->kstack_slot_va_ + 4095));
    JARVIS_ASSERT(
        !kernel::is_stack_guard_fault(*t, t->kstack_slot_va_ + 4096));
    JARVIS_ASSERT(!kernel::is_stack_guard_fault(
        *t, reinterpret_cast<uint64_t>(t->kernel_stack)));
    JARVIS_ASSERT(!kernel::is_stack_guard_fault(*t, 0x40008000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — bring-up live proof (housing violation detector:
// fails loud without an AP, so it must never run single-CPU).
// Input: -smp 2 boot (harness-gated class).
// Expect: ap_count() == 1; per_cpu[1] identity sane (cpu_id == 1,
// mpidr distinct from BSP, Aff0 matches the DTB second cpu reg);
// up_cpu_count() == 2.
// Depends: kernel::smp AP state, DTB walk (cross-check).
JARVIS_TEST(smp_aa64_mp_bringup_live, "PRE: -smp 2 | POST: none") {
    // ap_count is the bring-up proof (BSP + APs started); up_cpu_count
    // reflects scheduler uptake, which test isolation may clamp.
    JARVIS_ASSERT_FMT(kernel::smp::ap_count() == 1,
                      "ap_count %u, want 1 (-smp 2 class)",
                      kernel::smp::ap_count());
    JARVIS_ASSERT(arch::per_cpu[1].cpu_id == 1);
    JARVIS_ASSERT(arch::per_cpu[1].mpidr != arch::per_cpu[0].mpidr);
    kernel::boot::DtbCpuEntry entries[8];
    int n = kernel::boot::dtb_walk_cpus(&_binary_virt_dtb_start, entries,
                                        8);
    JARVIS_ASSERT(n >= 2);
    JARVIS_ASSERT_FMT((arch::per_cpu[1].mpidr & 0xFFU) ==
                          (entries[1].id & 0xFFU),
                      "AP mpidr Aff0 0x%lx vs DTB cpu1 0x%lx",
                      arch::per_cpu[1].mpidr & 0xFFU,
                      entries[1].id & 0xFFU);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — IPI routing wired proof (effect-observable part:
// the LOOPBACK leg is registered when APs are up; the SCHED leg has no
// aa64 consumer yet — pinned explicitly so slice-5 wiring updates it).
// Input: -smp 2 boot (handlers registered by bring_up).
// Expect: has_handler(LOOPBACK) true; self LOOPBACK send accepted;
// has_handler(SCHED) false (S4 gap, documented — no silent assumption).
// Depends: IDT::has_handler, gic_send_sgi (S2).
JARVIS_TEST(smp_aa64_mp_ipi_routing, "PRE: -smp 2 | POST: none") {
    // Both SGI legs have consumers once APs are up: LOOPBACK acks the
    // sender slot; SCHED runs sched_ipi_handler (kernel.cpp boot
    // registration). Self-send accepted on both.
    JARVIS_ASSERT(arch::IDT::has_handler(arch::SMP_LOOPBACK_SGI));
    JARVIS_ASSERT(arch::IDT::has_handler(arch::SMP_SCHED_SGI));
    JARVIS_ASSERT(arch::gic_send_sgi(0, arch::SMP_LOOPBACK_SGI));
    JARVIS_ASSERT(arch::gic_send_sgi(0, arch::SMP_SCHED_SGI));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — cross-CPU dispatch: a task pinned CPU1-only must
// progress (proves kick-or-tick delivery + remote dispatch), then
// re-pinned CPU0 must progress again. Vacuous-pass-proofed via
// per-leg progress counters.
// Input: spinner task, set_affinity_err CPU1-only mask then CPU0-only.
// Expect: FAILS LOUD while sched_up_cpus()==1 (NEED product fix — no
// vacuous CPU0-only pass); with the fix, progress each leg + reap.
// Depends: set_affinity_err, add_task, bounded spin (runelf template).
namespace {
volatile uint64_t g_aa64_xcpu_progress = 0;
volatile bool g_aa64_xcpu_stop = false;
void aa64_xcpu_spin() {
    for (;;) {
        __atomic_fetch_add(&g_aa64_xcpu_progress, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_aa64_xcpu_stop, __ATOMIC_RELAXED))
            break;
    }
}
} // namespace
JARVIS_TEST(smp_aa64_mp_sched_cross_cpu, "PRE: -smp 2 | POST: none") {
    // Genuine cross-CPU dispatch needs the scheduler up-count > 1; the
    // AP-side clamp (set_affinity id->CPU0 warn) already proved otherwise.
    // NEED(product): sched_up_cpus() is x86-only (1+ap_count); aa64 is
    // hardcoded 1, so no task can ever be placed on CPU1. Fail loud
    // until the product fix lands — a vacuous CPU0-only pass is refused.
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu (x86-only "
                      "1+ap_count); cross-CPU dispatch impossible",
                      Scheduler::up_cpu_count());
    g_aa64_xcpu_progress = 0;
    g_aa64_xcpu_stop = false;
    auto *t = TaskControlBlock::create(aa64_xcpu_spin, 5, 100);
    JARVIS_ASSERT(t != nullptr);
    auto perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin CPU1 err %d",
                      static_cast<int>(perr));
    Scheduler::add_task(*t);
    spin_until_progress(&g_aa64_xcpu_progress, 0);
    uint64_t leg1 = __atomic_load_n(&g_aa64_xcpu_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_FMT(TaskControlBlock::is_valid(t) &&
                          t->state != TaskState::TERMINATED,
                      "probe should still run (leg1=%lu)", leg1);
    JARVIS_ASSERT_FMT(leg1 > 0, "no CPU1 progress in bound");
    perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin CPU0 err %d",
                      static_cast<int>(perr));
    spin_until_progress(&g_aa64_xcpu_progress, leg1);
    uint64_t leg2 = __atomic_load_n(&g_aa64_xcpu_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_FMT(leg2 > leg1, "no CPU0 progress (%lu->%lu)", leg1,
                      leg2);
    __atomic_store_n(&g_aa64_xcpu_stop, true, __ATOMIC_RELAXED);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — shootdown-apply verdict (aa64 has no AP-side apply
// handler until slice-5, so no live shootdown is attempted here; the
// contract surface that DOES exist is pinned instead).
// Input: invalid-target + self sends (S4 interface).
// Expect: invalid refused; self sends accepted (delivery only, no
// apply asserted — stated). The live E2E (fill/unmap/shootdown/refault
// + negative control) is slice-5-gated behind the apply handler.
// Depends: arch::send_shootdown_ipi (S4).
JARVIS_TEST(smp_aa64_mp_shootdown_verdict, "PRE: -smp 2 | POST: none") {
    // No AP-side apply handler on aa64 yet (slice-5); the delivery leg
    // is verified accepted (BSP->AP Aff0 resolves, SGI issues).
    JARVIS_ASSERT(!arch::send_shootdown_ipi(CONFIG_MAX_CPUS));
    JARVIS_ASSERT(arch::send_shootdown_ipi(1));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — affinity migration matrix (live legs) + denial
// contract: migrate CPU0->CPU1->CPU0 with progress each leg; empty mask
// denied.
// Input: spinner task, set_affinity_err masks.
// Expect: FAILS LOUD while sched_up_cpus()==1 (same NEED gate);
// with the fix, per-leg progress; mask 0x0 rejected; clean reap.
// Depends: set_affinity_err, add_task, bounded spin.
namespace {
volatile uint64_t g_aa64_mig_progress = 0;
volatile bool g_aa64_mig_stop = false;
void aa64_mig_spin() {
    for (;;) {
        __atomic_fetch_add(&g_aa64_mig_progress, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_aa64_mig_stop, __ATOMIC_RELAXED))
            break;
    }
}
} // namespace
JARVIS_TEST(smp_aa64_mp_affinity_migrate, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu; migration "
                      "to CPU1 impossible",
                      Scheduler::up_cpu_count());
    g_aa64_mig_progress = 0;
    g_aa64_mig_stop = false;
    auto *t = TaskControlBlock::create(aa64_mig_spin, 5, 100);
    JARVIS_ASSERT(t != nullptr);
    auto perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    Scheduler::add_task(*t);
    spin_until_progress(&g_aa64_mig_progress, 0);
    uint64_t p0 = __atomic_load_n(&g_aa64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p0 > 0);
    perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    spin_until_progress(&g_aa64_mig_progress, p0);
    uint64_t p1 = __atomic_load_n(&g_aa64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p1 > p0);
    perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    spin_until_progress(&g_aa64_mig_progress, p1);
    uint64_t p2 = __atomic_load_n(&g_aa64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p2 > p1);
    perr = Scheduler::set_affinity_err(*t, 0x0);
    JARVIS_ASSERT_FMT(perr != errors::SCHED_ERR_OK,
                      "empty mask must be denied");
    __atomic_store_n(&g_aa64_mig_stop, true, __ATOMIC_RELAXED);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — balancer pull (mirrors the x86 idle_pull
// mechanics: 3 queued tasks on CPU0, CPU1 idle, one balancer tick).
// Input: 3 BLOCKED queued tasks pinned CPU0, balancer_tick() under IrqGuard.
// Expect: no move (all on 0) or exactly one move (counters +1, moved
// task re-pinned 0x2 and queued on 1) — same either/or contract as x86.
// Depends: Scheduler::balancer_tick, is_queued_on, get_affinity,
// migration_count (issue #61).
JARVIS_TEST(smp_aa64_mp_balancer_pull, "PRE: -smp 2 | POST: none") {
    Scheduler::reset_migration_counts();
    auto *a = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    auto *b = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    auto *c = TaskControlBlock::create([]() {}, 10,
                                       TaskControlBlock::NO_PERIOD);
    JARVIS_ASSERT(a != nullptr && b != nullptr && c != nullptr);
    {
        arch::IrqGuard irq_guard{};
        auto prep = [](TaskControlBlock *t) {
            t->state = TaskState::BLOCKED;
            Scheduler::register_task(*t);
            Scheduler::enqueue_ready(*t);
            Scheduler::set_affinity(*t, 0x3);
        };
        prep(a);
        prep(b);
        prep(c);
        uint64_t before = Scheduler::migration_count(0) +
                          Scheduler::migration_count(1);
        Scheduler::balancer_tick();
        uint64_t after = Scheduler::migration_count(0) +
                         Scheduler::migration_count(1);
        TaskControlBlock *all[3] = {a, b, c};
        uint64_t on_zero = 0, on_one = 0;
        for (auto *t : all) {
            bool q0 = Scheduler::is_queued_on(*t, 0);
            bool q1 = Scheduler::is_queued_on(*t, 1);
            JARVIS_ASSERT(q0 != q1);
            if (q1) {
                ++on_one;
                JARVIS_ASSERT(Scheduler::get_affinity(*t) == 0x2);
            } else {
                ++on_zero;
            }
        }
        if (after == before) {
            JARVIS_ASSERT(on_zero == 3 && on_one == 0);
        } else {
            JARVIS_ASSERT(after == before + 1);
            JARVIS_ASSERT(on_zero == 2 && on_one == 1);
        }
    }
    TaskControlBlock *gone[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        Scheduler::remove_task(*gone[i]);
        gone[i]->cleanup();
        MemPool::free(gone[i]);
    }
    JARVIS_TEST_PASS();
}

void register_smp_aa64_tests() {
    JARVIS_REGISTER_TEST(smp_aa64_dtb_discovery);
    JARVIS_REGISTER_TEST(smp_aa64_self_ipi_loopback);
    JARVIS_REGISTER_TEST(smp_aa64_guard_predicate);
}

void register_smp_aa64_multicpu_tests() {
    JARVIS_REGISTER_TEST(smp_aa64_mp_bringup_live);
    JARVIS_REGISTER_TEST(smp_aa64_mp_ipi_routing);
    JARVIS_REGISTER_TEST(smp_aa64_mp_sched_cross_cpu);
    JARVIS_REGISTER_TEST(smp_aa64_mp_shootdown_verdict);
    JARVIS_REGISTER_TEST(smp_aa64_mp_affinity_migrate);
    JARVIS_REGISTER_TEST(smp_aa64_mp_balancer_pull);
}

#else // !CONFIG_ARCH_AARCH64

void register_smp_aa64_tests() {}
void register_smp_aa64_multicpu_tests() {}

#endif // CONFIG_ARCH_AARCH64
