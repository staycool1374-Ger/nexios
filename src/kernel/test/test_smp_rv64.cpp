/*
 * NexIOS RTOS — SMP multicore parity tests, RISC-V (issue #351 slice-5)
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

/// @file test_smp_rv64.cpp
/// @brief #351 slice-5 per-arch SMP groups (riscv64): single-hart-safe
///        groups (live-DTB discovery, send contract, guard predicates)
///        register into `smp`; live-2-hart groups (bring-up, IPI
///        routing, cross-hart dispatch, shootdown fill/unmap/apply,
///        affinity migration, balancer pull) register into
///        `smp_multicpu` (harness gates -smp 2). Housing rule (C2):
///        no live-hart group runs single-hart.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/ipi.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_sched_helpers.hpp>

using namespace kernel;

#if defined(CONFIG_ARCH_RISCV64)

#include <kernel/arch/riscv64/hal/smp.hpp>

namespace {

// Native ELF machine for synthetic headers (mirrors test_elf_loader.cpp).
constexpr uint16_t kNativeEm = 0xF3;

// Shootdown probe layout: code page @0x400000, RW data page @0x402000
// ([0]=canary, [8]=GO flag, [16]=READY marker). Words are objdump-
// verified (riscv64-elf-gcc -march=rv64imafd, no RVC/Zbb on target).
constexpr uint64_t kCodeVa = 0x400000ULL;
constexpr uint64_t kDataVa = 0x402000ULL;
constexpr uint64_t kCanary = 0x5AULL;
constexpr uint64_t kProbeMarker = 0xBEEFULL;

inline size_t emit_shootdown_probe(uint32_t *code) {
    code[0] = 0x00000297U;  // auipc t0,0
    code[1] = 0x02C2B283U;  // ld t0,44(t0) (data VA placeholder)
    code[2] = 0x0000C337U;  // lui t1,0xc
    code[3] = 0xEEF3031BU;  // addiw t1,t1,-273 (0xBEEF)
    code[4] = 0x0062B823U;  // sd t1,16(t0) (READY)
    code[5] = 0x0082B383U;  // ld t2,8(t0) (GO)
    code[6] = 0xFE038EE3U;  // beqz t2,.-4 (spin)
    code[7] = 0x0002BE03U;  // ld t3,0(t0) (re-touch: fault or stale)
    code[8] = 0x0FF8F513U;  // andi a0,t3,0xff (exit = low byte)
    code[9] = 0x00600893U;  // li a7,6 (EXIT)
    code[10] = 0x00000073U; // ecall
    return 11;
}

uint64_t build_shootdown_probe_elf(elf::ELF64Header *hdr, uint8_t *data) {
    __builtin_memset(hdr, 0, sizeof(elf::ELF64Header));
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_EXEC;
    hdr->machine = kNativeEm;
    hdr->version = 1;
    hdr->entry = kCodeVa;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 2;
    auto *ph0 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header));
    ph0->type = elf::PT_LOAD;
    ph0->flags = elf::PF_R | elf::PF_X;
    ph0->offset = sizeof(elf::ELF64Header) +
                  2 * sizeof(elf::ELF64ProgramHeader);
    ph0->vaddr = kCodeVa;
    ph0->paddr = kCodeVa;
    ph0->filesz = 0x1000;
    ph0->memsz = 0x1000;
    ph0->align = 0x1000;
    auto *ph1 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader));
    ph1->type = elf::PT_LOAD;
    ph1->flags = elf::PF_R | elf::PF_W;
    ph1->offset = ph0->offset + 0x1000;
    ph1->vaddr = kDataVa;
    ph1->paddr = kDataVa;
    ph1->filesz = 0x100;
    ph1->memsz = 0x1000;
    ph1->align = 0x1000;
    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
    uint32_t *code = reinterpret_cast<uint32_t *>(data + ph0->offset);
    size_t words = emit_shootdown_probe(code);
    for (size_t i = words; i < 0x1000 / 4; ++i)
        code[i] = 0x00000013U; // addi x0,x0,0 (NOP sled)
    uint64_t *rw = reinterpret_cast<uint64_t *>(data + ph1->offset);
    for (size_t i = 0; i < 0x100 / 8; ++i)
        rw[i] = static_cast<uint64_t>(i);
    return ph1->offset + 0x100;
}

// Patch one u64 of the LOADED image through the kernel HHDM alias.
void patch_loaded_u64(TaskControlBlock *t, uint64_t uva, uint64_t value) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(uva, t->get_page_table());
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *alias = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    __builtin_memcpy(alias, &value, sizeof(value));
}

uint64_t read_loaded_u64(TaskControlBlock *t, uint64_t uva) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(uva, t->get_page_table());
    if (phys == 0)
        return 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint8_t *alias = reinterpret_cast<const uint8_t *>(
        arch::HHDM_OFFSET + phys);
    uint64_t v = 0;
    __builtin_memcpy(&v, alias, sizeof(v));
    return v;
}

// Progress-aware spin: exits when the observed counter advances.
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

// Bounded yield-spin for probes that terminate by themselves
// (runelf template: TERMINATED-required, no vacuous pass).
void spin_until_done(TaskControlBlock *t, int bound = 400000) {
    for (int i = 0; i < bound; ++i) {
        if (!TaskControlBlock::is_valid(t) ||
            t->state == TaskState::TERMINATED ||
            t->state == TaskState::REAPED)
            break;
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
}

} // namespace

// Runmode: kernel
// Testidea: #351 S5 — DTB discovery parse on the LIVE OpenSBI DTB
// (riscv64 has no embedded fallback; dtb_from_boot_info validates the
// firmware blob).
// Input: kernel::boot::dtb_from_boot_info().
// Expect: walk succeeds; count >= up_cpu_count()+APs... precisely:
// count >= ap_count()+1; all methods UNKNOWN (riscv HSM-implicit, no
// enable-method); psci_method false (no psci node on riscv virt).
// Depends: dtb_walk_cpus, dtb_psci_method (S1), ap_count.
JARVIS_TEST(smp_rv64_dtb_discovery, "PRE: none | POST: none") {
    const void *fdt = kernel::boot::dtb_from_boot_info();
    JARVIS_ASSERT(fdt != nullptr);
    kernel::boot::DtbCpuEntry entries[8];
    int n = kernel::boot::dtb_walk_cpus(fdt, entries, 8);
    JARVIS_ASSERT(n >= 1);
    JARVIS_ASSERT_FMT(static_cast<uint64_t>(n) >=
                          kernel::smp::ap_count() + 1U,
                      "DTB describes %d CPUs, %u booted", n,
                      kernel::smp::ap_count() + 1U);
    for (int i = 0; i < n; ++i)
        JARVIS_ASSERT(entries[i].method ==
                      kernel::boot::CpuEnableMethod::UNKNOWN);
    char method[16];
    JARVIS_ASSERT(!kernel::boot::dtb_psci_method(fdt, method,
                                                 sizeof(method)));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — shootdown send contract (BSP never targeted:
// logical 0 refused by design on rv64, same as aa64).
// Input: out-of-range + logical-0 sends (both IPI legs).
// Expect: all refused (false, no crash); AP leg covered live in
// smp_multicpu (send accepted + applied).
// Depends: kernel::smp::send_resched_ipi/send_shootdown_ipi (S4).
JARVIS_TEST(smp_rv64_shootdown_send_contract, "PRE: none | POST: none") {
    JARVIS_ASSERT(!kernel::smp::send_resched_ipi(CONFIG_MAX_CPUS));
    JARVIS_ASSERT(!kernel::smp::send_shootdown_ipi(CONFIG_MAX_CPUS));
    JARVIS_ASSERT(!kernel::smp::send_resched_ipi(0));
    JARVIS_ASSERT(!kernel::smp::send_shootdown_ipi(0));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — rv64 guard backend is a documented stub (no
// kslot window; create_user takes the HHDM fallback with slot == 0).
// The arch-neutral predicate itself is covered on aa64/x86; here the
// STABLE contract is pinned: slot stays 0 (any future window must
// update this test), and the predicate on a slotless task is false
// everywhere (no guard range exists).
// Input: create_user() task.
// Expect: kstack_slot_va_ == 0 (stub backend, stable); predicate
// false for slot VA, kernel_stack, and an arbitrary VA.
// Depends: create_user HHDM fallback (documented dual-path, task.cpp).
JARVIS_TEST(smp_rv64_guard_predicate, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB);
    JARVIS_ASSERT(t != nullptr);
    auto cleanup = ScopeGuard([&]() {
        t->cleanup();
        delete t;
    });
    JARVIS_ASSERT_FMT(t->kstack_slot_va_ == 0,
                      "rv64 kslot stub changed: slot 0x%lx (update test)",
                      t->kstack_slot_va_);
    JARVIS_ASSERT(!kernel::is_stack_guard_fault(*t, t->kstack_slot_va_));
    JARVIS_ASSERT(!kernel::is_stack_guard_fault(
        *t, reinterpret_cast<uint64_t>(t->kernel_stack)));
    JARVIS_ASSERT(!kernel::is_stack_guard_fault(*t, 0x40008000ULL));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — HSM bring-up live proof (housing violation
// detector: fails loud without a second hart).
// Input: -smp 2 boot (harness-gated class).
// Expect: ap_count() == 1; per_cpu[1] identity sane (cpu_id == 1,
// hart_id distinct from BSP, matches DTB cpu[1] reg).
// Depends: kernel::smp HSM bring-up state (S3), live DTB cross-check.
JARVIS_TEST(smp_rv64_mp_bringup_live, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(kernel::smp::ap_count() == 1,
                      "ap_count %u, want 1 (-smp 2 class)",
                      kernel::smp::ap_count());
    JARVIS_ASSERT(arch::per_cpu[1].cpu_id == 1);
    JARVIS_ASSERT(arch::per_cpu[1].hart_id != arch::per_cpu[0].hart_id);
    const void *fdt = kernel::boot::dtb_from_boot_info();
    JARVIS_ASSERT(fdt != nullptr);
    kernel::boot::DtbCpuEntry entries[8];
    int n = kernel::boot::dtb_walk_cpus(fdt, entries, 8);
    JARVIS_ASSERT(n >= 2);
    JARVIS_ASSERT(arch::per_cpu[1].hart_id == entries[1].id);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — IPI routing: AP-targeted sends accepted on both
// legs (resched mailbox + SBI hart-mask; shootdown twin). No
// self-IPI (logical 0 refused by design).
// Input: -smp 2 boot.
// Expect: send_resched_ipi(1) + send_shootdown_ipi(1) true; 0/MAX
// false. No effect asserted (mailbox/ack unreadable test-side;
// dispatch proof lives in the cross-CPU + shootdown tests).
// Depends: kernel::smp IPI legs (S3/S4).
JARVIS_TEST(smp_rv64_mp_ipi_routing, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT(kernel::smp::send_resched_ipi(1));
    JARVIS_ASSERT(kernel::smp::send_shootdown_ipi(1));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — cross-hart dispatch: task pinned hart1-only must
// progress (proves IPI kick + remote dispatch), then re-pinned hart0.
// Input: spinner task, set_affinity_err hart masks.
// Expect: FAILS LOUD while sched_up_cpus()==1 (same product NEED as
// aa64 — no vacuous hart0-only pass); with the fix, per-leg progress.
// Depends: set_affinity_err, add_task, bounded progress spin.
namespace {
volatile uint64_t g_rv64_xcpu_progress = 0;
volatile bool g_rv64_xcpu_stop = false;
void rv64_xcpu_spin() {
    for (;;) {
        __atomic_fetch_add(&g_rv64_xcpu_progress, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_rv64_xcpu_stop, __ATOMIC_RELAXED))
            break;
    }
}
} // namespace
JARVIS_TEST(smp_rv64_mp_sched_cross_cpu, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu; "
                      "cross-hart dispatch impossible",
                      Scheduler::up_cpu_count());
    g_rv64_xcpu_progress = 0;
    g_rv64_xcpu_stop = false;
    auto *t = TaskControlBlock::create(rv64_xcpu_spin, 5, 100);
    JARVIS_ASSERT(t != nullptr);
    auto perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin hart1 err %d",
                      static_cast<int>(perr));
    Scheduler::add_task(*t);
    spin_until_progress(&g_rv64_xcpu_progress, 0);
    uint64_t leg1 = __atomic_load_n(&g_rv64_xcpu_progress,
                                    __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_FMT(TaskControlBlock::is_valid(t) &&
                          t->state != TaskState::TERMINATED,
                      "probe should still run (leg1=%lu)", leg1);
    JARVIS_ASSERT_FMT(leg1 > 0, "no hart1 progress in bound");
    perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin hart0 err %d",
                      static_cast<int>(perr));
    spin_until_progress(&g_rv64_xcpu_progress, leg1);
    uint64_t leg2 = __atomic_load_n(&g_rv64_xcpu_progress,
                                    __ATOMIC_ACQUIRE);
    JARVIS_ASSERT_FMT(leg2 > leg1, "no hart0 progress (%lu->%lu)", leg1,
                      leg2);
    __atomic_store_n(&g_rv64_xcpu_stop, true, __ATOMIC_RELAXED);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 + #389 — live shootdown E2E (positive): ELF probe
// pinned hart1 fills its data page (TLB fill on hart1); supervisor
// unmaps + sends the SHOOTDOWN IPI; AP applies SFENCE; probe re-touch
// faults -> TERMINATED with non-canary exit (invalidation landed).
// Input: probe ELF (objdump-verified words), hart1 pin, supervisor
// unmap + send_shootdown_ipi(1) + GO flag.
// Expect: probe TERMINATED; exit_code != canary (fault, not stale).
// Depends: elf::load, VMM unmap_in_pml4, S4 shootdown leg, AP apply
// (S3). NEED-gated on sched_up_cpus (fill must land on hart1).
JARVIS_TEST(smp_rv64_mp_shootdown_apply, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu; hart1 "
                      "fill impossible",
                      Scheduler::up_cpu_count());
    elf::ELF64Header hdr{};
    uint8_t img[8192];
    uint64_t sz = build_shootdown_probe_elf(&hdr, img);
    auto *t = kernel::elf::load(&hdr, img, sz);
    JARVIS_ASSERT(t != nullptr);
    patch_loaded_u64(t, kDataVa, kDataVa);       // placeholder -> data VA
    patch_loaded_u64(t, kDataVa + 0, kCanary);    // canary
    auto perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin hart1 err %d",
                      static_cast<int>(perr));
    Scheduler::add_task(*t);
    // Wait for the READY store (TLB filled on hart1).
    for (int i = 0; i < 20000; ++i) {
        if (read_loaded_u64(t, kDataVa + 16) == kProbeMarker)
            break;
        arch::pause();
    }
    JARVIS_ASSERT_FMT(read_loaded_u64(t, kDataVa + 16) == kProbeMarker,
                      "probe never filled (READY missing)");
    VMM::unmap_page_in_pml4(kDataVa, t->get_page_table());
    JARVIS_ASSERT(kernel::smp::send_shootdown_ipi(1));
    patch_loaded_u64(t, kDataVa + 8, 1); // GO
    spin_until_done(t);
    JARVIS_ASSERT_FMT(TaskControlBlock::is_valid(t) &&
                          t->state == TaskState::TERMINATED,
                      "probe not TERMINATED after shootdown");
    JARVIS_ASSERT_FMT(t->exit_code != kCanary,
                      "stale read survived shootdown (exit 0x%lx)",
                      t->exit_code);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 + #389 — negative control: same vehicle, unmap
// WITHOUT the shootdown IPI; the stale TLB entry lets the re-touch
// succeed, and the probe exits with the canary (discriminates the
// test: a passing positive is meaningless if this also faults).
// Input: probe ELF, hart1 pin, unmap only, GO flag.
// Expect: probe TERMINATED with exit_code == canary (stale proven).
// A fault here fails loud (no silent reinterpretation).
// Depends: same as shootdown_apply. NEED-gated on up>1 (same fill).
JARVIS_TEST(smp_rv64_mp_shootdown_nohook_stale, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu; hart1 "
                      "fill impossible",
                      Scheduler::up_cpu_count());
    elf::ELF64Header hdr{};
    uint8_t img[8192];
    uint64_t sz = build_shootdown_probe_elf(&hdr, img);
    auto *t = kernel::elf::load(&hdr, img, sz);
    JARVIS_ASSERT(t != nullptr);
    patch_loaded_u64(t, kDataVa, kDataVa);
    patch_loaded_u64(t, kDataVa + 0, kCanary);
    auto perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT_FMT(perr == errors::SCHED_ERR_OK, "pin hart1 err %d",
                      static_cast<int>(perr));
    Scheduler::add_task(*t);
    for (int i = 0; i < 20000; ++i) {
        if (read_loaded_u64(t, kDataVa + 16) == kProbeMarker)
            break;
        arch::pause();
    }
    JARVIS_ASSERT_FMT(read_loaded_u64(t, kDataVa + 16) == kProbeMarker,
                      "probe never filled (READY missing)");
    VMM::unmap_page_in_pml4(kDataVa, t->get_page_table());
    // Deliberately NO shootdown IPI: the stale entry must survive.
    patch_loaded_u64(t, kDataVa + 8, 1); // GO
    spin_until_done(t);
    JARVIS_ASSERT_FMT(TaskControlBlock::is_valid(t) &&
                          t->state == TaskState::TERMINATED,
                      "probe not TERMINATED");
    JARVIS_ASSERT_FMT(t->exit_code == kCanary,
                      "expected stale canary 0x%lx, got 0x%lx (fault "
                      "without shootdown?)",
                      kCanary, t->exit_code);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — affinity migration matrix (live legs) + denial
// contract: hart0->hart1->hart0 with progress each leg; empty mask
// denied.
// Input: spinner task, set_affinity_err masks.
// Expect: FAILS LOUD while sched_up_cpus()==1 (same NEED gate); with
// the fix, per-leg progress; mask 0x0 rejected; clean reap.
// Depends: set_affinity_err, add_task, bounded progress spin.
namespace {
volatile uint64_t g_rv64_mig_progress = 0;
volatile bool g_rv64_mig_stop = false;
void rv64_mig_spin() {
    for (;;) {
        __atomic_fetch_add(&g_rv64_mig_progress, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&g_rv64_mig_stop, __ATOMIC_RELAXED))
            break;
    }
}
} // namespace
JARVIS_TEST(smp_rv64_mp_affinity_migrate, "PRE: -smp 2 | POST: none") {
    JARVIS_ASSERT_FMT(Scheduler::up_cpu_count() > 1,
                      "NEED product: sched_up_cpus stuck at %lu; migration "
                      "to hart1 impossible",
                      Scheduler::up_cpu_count());
    g_rv64_mig_progress = 0;
    g_rv64_mig_stop = false;
    auto *t = TaskControlBlock::create(rv64_mig_spin, 5, 100);
    JARVIS_ASSERT(t != nullptr);
    auto perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    Scheduler::add_task(*t);
    spin_until_progress(&g_rv64_mig_progress, 0);
    uint64_t p0 = __atomic_load_n(&g_rv64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p0 > 0);
    perr = Scheduler::set_affinity_err(*t, 0x2);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    spin_until_progress(&g_rv64_mig_progress, p0);
    uint64_t p1 = __atomic_load_n(&g_rv64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p1 > p0);
    perr = Scheduler::set_affinity_err(*t, 0x1);
    JARVIS_ASSERT(perr == errors::SCHED_ERR_OK);
    spin_until_progress(&g_rv64_mig_progress, p1);
    uint64_t p2 = __atomic_load_n(&g_rv64_mig_progress, __ATOMIC_ACQUIRE);
    JARVIS_ASSERT(p2 > p1);
    perr = Scheduler::set_affinity_err(*t, 0x0);
    JARVIS_ASSERT_FMT(perr != errors::SCHED_ERR_OK,
                      "empty mask must be denied");
    __atomic_store_n(&g_rv64_mig_stop, true, __ATOMIC_RELAXED);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #351 S5 — balancer pull (mirrors the x86 idle_pull
// mechanics: 3 queued tasks on hart0, hart1 idle, one balancer tick).
// Input: 3 BLOCKED queued tasks pinned hart0, balancer_tick() under IrqGuard.
// Expect: no move (all on 0) or exactly one move (counters +1, moved
// task re-pinned 0x2 and queued on 1) — same either/or contract as x86.
// Depends: Scheduler::balancer_tick, is_queued_on, get_affinity,
// migration_count (issue #61).
JARVIS_TEST(smp_rv64_mp_balancer_pull, "PRE: -smp 2 | POST: none") {
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
        auto prep = [&](TaskControlBlock *t) {
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
        for (int i = 0; i < 3; ++i) {
            bool q0 = Scheduler::is_queued_on(*all[i], 0);
            bool q1 = Scheduler::is_queued_on(*all[i], 1);
            JARVIS_ASSERT(q0 != q1);
            if (q1) {
                ++on_one;
                JARVIS_ASSERT(Scheduler::get_affinity(*all[i]) == 0x2);
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

void register_smp_rv64_tests() {
    JARVIS_REGISTER_TEST(smp_rv64_dtb_discovery);
    JARVIS_REGISTER_TEST(smp_rv64_shootdown_send_contract);
    JARVIS_REGISTER_TEST(smp_rv64_guard_predicate);
}

void register_smp_rv64_multicpu_tests() {
    JARVIS_REGISTER_TEST(smp_rv64_mp_bringup_live);
    JARVIS_REGISTER_TEST(smp_rv64_mp_ipi_routing);
    JARVIS_REGISTER_TEST(smp_rv64_mp_sched_cross_cpu);
    JARVIS_REGISTER_TEST(smp_rv64_mp_shootdown_apply);
    JARVIS_REGISTER_TEST(smp_rv64_mp_shootdown_nohook_stale);
    JARVIS_REGISTER_TEST(smp_rv64_mp_affinity_migrate);
    JARVIS_REGISTER_TEST(smp_rv64_mp_balancer_pull);
}

#else // !CONFIG_ARCH_RISCV64

void register_smp_rv64_tests() {}
void register_smp_rv64_multicpu_tests() {}

#endif // CONFIG_ARCH_RISCV64
