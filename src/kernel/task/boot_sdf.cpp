#include <kernel/task/boot_sdf.hpp>

#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                            \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET

#include <kernel/task/task.hpp>
#include <kernel/arch/hal/device_regions.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/iommu.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/arch/pci.hpp>

namespace kernel {
namespace sdf {
namespace {

/// @brief Mint one CapSpec into the task CSpace (all-or-nothing caller).
/// @return slot index or -1. Created caps are disposed by the caller on
///         any later failure (rollback) or owned by the CSpace on success.
int mint_one(TaskControlBlock &task, const CapSpec &spec) {
    task.ensure_cspace();
    cap::CNode *space = task.get_cspace();
    if (space == nullptr) {
        return -1;
    }
    KernelObject *obj = nullptr;
    cap::CapType type = cap::CapType::Null;
    switch (spec.kind) {
    case CapKind::IRQ: {
        auto *cap =
            cap::IrqCap::create(static_cast<uint8_t>(spec.vector));
        if (cap == nullptr) {
            return -1;
        }
        obj = cap;
        type = cap::CapType::Irq;
        break;
    }
    case CapKind::MMIO: {
        const arch::PciBarType bar_type =
            (spec.io_port != 0) ? arch::PciBarType::IO
                                : arch::PciBarType::MEMORY_32;
        auto *cap = cap::MmioCap::create(spec.addr, spec.size, bar_type);
        if (cap == nullptr) {
            return -1;
        }
        obj = cap;
        type = cap::CapType::Mmio;
        break;
    }
    case CapKind::DMA: {
        auto *cap = cap::IoMmuDmaCap::create();
        if (cap == nullptr) {
            return -1;
        }
        obj = cap;
        type = cap::CapType::IoMmuDma;
        break;
    }
    case CapKind::FRAME: {
        const size_t pages =
            (spec.size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
        auto *cap =
            cap::FrameCap::create(spec.addr, pages, true);
        if (cap == nullptr) {
            return -1;
        }
        obj = cap;
        type = cap::CapType::Frame;
        break;
    }
    case CapKind::MSIX:
    case CapKind::SHM:
        // Reserved rows (MSI-X entry claim, SHM channels): wired when the
        // first PD needs them; never silently mapped to another kind.
        return -1;
    }
    const cap::CapRights rights =
        cap::CapRights::READ | cap::CapRights::WRITE;
    const int slot = space->install(obj, type, rights, nullptr);
    if (slot < 0) {
        obj->release();
        return -1;
    }
    return slot;
}

void rollback(TaskControlBlock &task, const int *slots, uint64_t count) {
    task.ensure_cspace();
    cap::CNode *space = task.get_cspace();
    if (space == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (slots[i] >= 0) {
            space->remove(static_cast<uint32_t>(slots[i]));
        }
    }
}

} // namespace

uint64_t sdf_row(PdRole role, CapSpec *out) {
    if (out == nullptr) {
        return 0;
    }
    switch (role) {
    case PdRole::SERIAL: {
        // RX-only PD (planner S1): TX stays kernel (console + panic).
        uint64_t n = 0;
#if defined(CONFIG_ARCH_X86_64)
        out[n++] = CapSpec{CapKind::MMIO, arch::device::COM1_PORT, 8, 0, 1};
#elif defined(CONFIG_ARCH_AARCH64)
        out[n++] = CapSpec{CapKind::MMIO, arch::device::PL011_BASE,
                           arch::device::PL011_SIZE, 0, 0};
#elif defined(CONFIG_ARCH_RISCV64)
        out[n++] = CapSpec{CapKind::MMIO, arch::device::UART0_BASE, 0x1000,
                           0, 0};
#endif
        return n;
    }
    case PdRole::KBD: {
#if defined(CONFIG_ARCH_X86_64)
        out[0] = CapSpec{CapKind::IRQ, 0, 0, 33, 0};
        out[1] = CapSpec{CapKind::MMIO, arch::device::KBD_DATA_PORT, 5, 0,
                         1};
        return 2;
#else
        // No PS/2 off-x86: RX stays UART-decoder (planner S2 stub PD).
        (void)out;
        return 0;
#endif
    }
    case PdRole::RTC: {
#if defined(CONFIG_ARCH_X86_64)
        out[0] = CapSpec{CapKind::MMIO, arch::device::CMOS_INDEX_PORT, 2, 0,
                         1};
        return 1;
#elif defined(CONFIG_ARCH_AARCH64)
        out[0] = CapSpec{CapKind::MMIO, arch::device::PL031_BASE,
                         arch::device::PL031_SIZE, 0, 0};
        return 1;
#elif defined(CONFIG_ARCH_RISCV64)
        out[0] = CapSpec{CapKind::MMIO, arch::device::GOLDFISH_RTC_BASE,
                         arch::device::GOLDFISH_RTC_SIZE, 0, 0};
        return 1;
#else
        return 0;
#endif
    }
    case PdRole::FB:
        // Device caps only (planner S4): scanout RAM as frames, control
        // registers per backend. Emulation policy stays #51 term-srv;
        // zero IRQ by design (fb is polled). Concrete BARs are dispatched
        // per backend at compose time — the row shape is declared here.
        return 0;
    case PdRole::BLOCK:
    case PdRole::NET:
        // Rows dispatch on live PCI/virtio topology at compose time
        // (planner S5/S6); the shape (MSI-X/INTID + DMA + SHM) is declared
        // in docs/specs/driver-pds.md. No static row yet.
        return 0;
    }
    return 0;
}

bool sdf_compose(TaskControlBlock &task, PdRole role) {
    CapSpec specs[kMaxCapsPerRole] = {};
    const uint64_t count = sdf_row(role, specs);
    if (count == 0 || count > kMaxCapsPerRole) {
        return false;
    }
    int slots[kMaxCapsPerRole] = {-1, -1, -1, -1, -1, -1};
    for (uint64_t i = 0; i < count; ++i) {
        const int slot = mint_one(task, specs[i]);
        if (slot < 0) {
            rollback(task, slots, i);
            return false;
        }
        slots[i] = slot;
    }
    return true;
}

bool sdf_rebind(TaskControlBlock &task, PdRole role) {
    // Restart path: disposal (revoke + drain) is owned by the caller;
    // re-derive only — never inherit a domain, vector, or mapping.
    return sdf_compose(task, role);
}

bool pd_owns_device(PdRole role) {
    switch (role) {
    case PdRole::SERIAL:
        return CONFIG_DRIVER_PD_SERIAL != 0;
    case PdRole::KBD:
        return CONFIG_DRIVER_PD_KBD != 0;
    case PdRole::RTC:
        return CONFIG_DRIVER_PD_RTC != 0;
    case PdRole::FB:
        return CONFIG_DRIVER_PD_FB != 0;
    case PdRole::BLOCK:
        return CONFIG_DRIVER_PD_BLOCK != 0;
    case PdRole::NET:
        return CONFIG_DRIVER_PD_NET != 0;
    }
    return false;
}

} // namespace sdf
} // namespace kernel

#endif // any CONFIG_DRIVER_PD_*
