#include <kernel/arch/x86_64/hal/percpu.hpp>
#include <kernel/arch/hal/msr.hpp>
#include <constants.hpp>

namespace arch {

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

    // Set GS_BASE for this AP to point at its PerCpu page.
    arch::wrmsr(arch::MSR_GS_BASE, reinterpret_cast<uint64_t>(&per_cpu[logical_id]));
}

} // namespace arch