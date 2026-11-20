#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/hal/msr.hpp>
#include <kernel/arch/apic.hpp>
#include <constants.hpp>

#include <kernel/arch/cpu_context.hpp>

namespace arch {

// Issue #589 (owner option (a)): VERIFY-ONLY body — must observe NO STORE.
// The own-CPU slot is asm-owned (isr_stubs.asm gs:0x20 RMW) and
// percpu_init_bsp already installed 0; this function exists to VERIFY the
// baseline, never to set it. The returned live value is logged at the
// handover call site so future drift is boot-visible.
uint64_t normalize_isr_nesting_baseline() noexcept {
    return kernel::isr_nesting_own();
}

// Per-CPU array (CONFIG_MAX_CPUS pages, 4 KiB each).
PerCpu per_cpu[CONFIG_MAX_CPUS];

void percpu_init_bsp(uint64_t bsp_lapic_id) {
    // Initialize BSP (CPU 0) per-CPU state.
    PerCpu &pc = per_cpu[0];
    pc.cpu_id = 0;
    pc.lapic_id = bsp_lapic_id;
    pc.user_rsp = 0;
    pc.kernel_rsp = 0;
    pc.isr_nesting_depth = 0;
    pc.irq_entry_tsc = 0;
    pc.fpu_owner = nullptr;
    pc.current_task = nullptr;
    pc.tpr_shadow = APIC::TPR_CLASS_ACCEPT_ALL; // issue #26

    // Set GS_BASE MSR to point at this CPU's PerCpu page.
    arch::wrmsr(arch::MSR_GS_BASE, reinterpret_cast<uint64_t>(&per_cpu[0]));
}

void percpu_init_ap(uint64_t logical_id, uint64_t lapic_id) {
    if (logical_id >= CONFIG_MAX_CPUS) {
        // Out of bounds - this should be caught by the caller.
        return;
    }

    PerCpu &pc = per_cpu[logical_id];
    pc.cpu_id = logical_id;
    pc.lapic_id = lapic_id;
    pc.user_rsp = 0;
    pc.kernel_rsp = 0;
    pc.isr_nesting_depth = 0;
    pc.irq_entry_tsc = 0;
    pc.fpu_owner = nullptr;
    pc.current_task = nullptr;
    pc.tpr_shadow = APIC::TPR_CLASS_ACCEPT_ALL; // issue #26

    // Set GS_BASE for this AP to point at its PerCpu page.
    arch::wrmsr(arch::MSR_GS_BASE, reinterpret_cast<uint64_t>(&per_cpu[logical_id]));
}

} // namespace arch