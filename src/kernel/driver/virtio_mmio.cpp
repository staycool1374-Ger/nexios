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

/// @file virtio_mmio.cpp
/// @brief Virtio MMIO (v2) transport discovery (issue #350) — QEMU `virt`
/// exposes virtio devices as MMIO windows enumerated in the DTB
/// (`compatible = "virtio,mmio"`). Non-x86 definition of
/// virtio_find_device(); the PCI definition in virtio_pci.cpp is
/// x86-only. Fail-closed throughout: no DTB / no node / bad window /
/// bad IRQ / bad identity → false, and the drivers keep their bounded
/// polling fallback.

#include <kernel/arch/virtio.hpp>
#include <kernel/boot/dtb.hpp>
#include <kernel/log/dmesg.hpp>
#include <logger.hpp>

#if defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/gic.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/plic.hpp>
#endif

using namespace kernel;

namespace arch {

#if !defined(CONFIG_ARCH_X86_64)
namespace {

// 32-bit volatile MMIO read (the transport window is boot-mapped).
uint32_t mmio_read32(uint64_t virt_base, uint32_t reg) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *ptr =
        reinterpret_cast<volatile uint32_t *>(virt_base + reg);
    return *ptr;
}

// Validate one candidate window and fill the transport on an identity
// match (magic + v1/v2 + wanted device). Shared by the DTB path and the
// architected slot fallback. Fail-closed: any mismatch → false.
bool try_mmio_slot(uint64_t base, uint16_t irq, uint16_t slot,
                   uint16_t mmio_id, uint16_t want_id,
                   VirtioTransport &transport) {
    uint64_t virt = virtio_mmio_virt(base);
    if (virt == 0) return false;
    if (mmio_read32(virt, VIRTIO_MMIO_MAGIC) != VIRTIO_MMIO_MAGIC_VALUE)
        return false;
    // QEMU `virt` exposes the legacy (v1) interface — Version reads 1
    // (GDB-proven on riscv64: magic + version + device ID all live).
    uint32_t version = mmio_read32(virt, VIRTIO_MMIO_VERSION);
    if (version != VIRTIO_MMIO_VERSION_MODERN &&
        version != VIRTIO_MMIO_VERSION_LEGACY)
        return false;
    if (mmio_read32(virt, VIRTIO_MMIO_DEVICE_ID) != mmio_id) return false;

    transport = VirtioTransport{};
    transport.mmio = true;
    transport.mmio_base_phys = base;
    transport.mmio_irq = irq;
    transport.mmio_slot = slot;
    transport.mmio_version = static_cast<uint16_t>(version);
    transport.device_id = want_id;
    transport.modern = true;
    transport.notify_off_multiplier = 0;
    transport.common_cfg.virt_addr = virt;
    transport.common_cfg.phys_addr = base;
    transport.common_cfg.length = VIRTIO_MMIO_REGION_SIZE;
    transport.notify_cfg = transport.common_cfg;
    transport.isr_cfg = transport.common_cfg;
    transport.device_cfg.virt_addr = virt + VIRTIO_MMIO_CONFIG;
    transport.device_cfg.phys_addr = base + VIRTIO_MMIO_CONFIG;
    transport.device_cfg.length = VIRTIO_MMIO_REGION_SIZE;

    Logger::info("virtio-mmio: slot=%d base=0x%lx irq=%d dev=%d", slot,
                 static_cast<uint64_t>(base), irq, mmio_id);
    return true;
}

} // namespace
#endif

// The PCI finder (virtio_pci.cpp) owns the virtio_find_device symbol on
// x86_64; this TU contributes the symbol only elsewhere.
#if !defined(CONFIG_ARCH_X86_64)
bool virtio_find_device(uint16_t device_id, VirtioTransport &transport) {
    uint16_t mmio_id = virtio_mmio_device_id(device_id);
    if (mmio_id == 0) return false;

    // Primary: DTB discovery (works where firmware passes a DTB).
    const void *fdt = kernel::boot::dtb_from_boot_info();
    if (fdt) {
        int nodes[kernel::boot::DTB_MAX_MATCHES];
        int found = kernel::boot::dtb_find_compatible_nodes(
            fdt, "virtio,mmio", nodes, kernel::boot::DTB_MAX_MATCHES);
        for (int idx = 0; idx < found; ++idx) {
            uint64_t base = 0;
            uint64_t size = 0;
            if (!kernel::boot::dtb_node_reg(fdt, nodes[idx], base, size))
                continue;
            // Window check: inside the boot-mapped DTB leaf, at least
            // one 0x200 register block, slot-aligned. End-contained:
            // the whole touched window must fit (audit S3 hardening).
            if (base < VIRTIO_MMIO_DTB_BASE ||
                base + VIRTIO_MMIO_REGION_SIZE >
                    VIRTIO_MMIO_DTB_BASE + VIRTIO_MMIO_DTB_WINDOW)
                continue;
            if (size < VIRTIO_MMIO_REGION_SIZE) continue;
            if ((base % VIRTIO_MMIO_REGION_SIZE) != 0) continue;

            uint32_t cells[4];
            int cell_count = 0;
            if (!kernel::boot::dtb_node_cells(fdt, nodes[idx], "interrupts",
                                             cells, 4, cell_count))
                continue;
            uint16_t irq = 0;
#if defined(CONFIG_ARCH_AARCH64)
            // GIC SPI triplet: <0 spi flags> → INTID = 32 + spi.
            if (cell_count != 3 || cells[0] != 0) continue;
            uint32_t intid = 32 + cells[1];
            if (intid < INTID_VIRTIO_BASE ||
                intid >= INTID_VIRTIO_BASE + INTID_VIRTIO_COUNT)
                continue;
            irq = static_cast<uint16_t>(intid);
#elif defined(CONFIG_ARCH_RISCV64)
            // PLIC single cell: source ID directly.
            if (cell_count != 1) continue;
            if (cells[0] < IRQ_VIRTIO0 ||
                cells[0] >= IRQ_VIRTIO0 + IRQ_VIRTIO_COUNT)
                continue;
            irq = static_cast<uint16_t>(cells[0]);
#else
            continue;
#endif

            if (try_mmio_slot(base, irq, static_cast<uint16_t>(idx),
                              mmio_id, device_id, transport))
                return true;
        }
    } else {
        Logger::info("virtio-mmio: no validated DTB — slot probing");
    }

    // Fallback: architected slot table. The aarch64 `-kernel` path
    // passes no DTB (proven: x0 == 0 at higherhalf_entry, no FDT magic
    // in RAM); the QEMU `virt` slot layout is stable board knowledge.
    // Unpopulated slots read as zero → magic mismatch → skip.
    for (uint32_t slot = 0; slot < VIRTIO_MMIO_SLOT_COUNT; ++slot) {
        uint64_t base = VIRTIO_MMIO_SLOT_BASE +
                        static_cast<uint64_t>(slot) * VIRTIO_MMIO_SLOT_STRIDE;
#if defined(CONFIG_ARCH_AARCH64)
        uint32_t intid = INTID_VIRTIO_BASE + slot;
        if (intid >= INTID_VIRTIO_BASE + INTID_VIRTIO_COUNT) continue;
        uint16_t irq = static_cast<uint16_t>(intid);
#elif defined(CONFIG_ARCH_RISCV64)
        uint32_t src = IRQ_VIRTIO0 + slot;
        if (src >= IRQ_VIRTIO0 + IRQ_VIRTIO_COUNT) continue;
        uint16_t irq = static_cast<uint16_t>(src);
#else
        continue;
#endif
        if (try_mmio_slot(base, irq, static_cast<uint16_t>(slot), mmio_id,
                          device_id, transport))
            return true;
    }

    Logger::info("virtio-mmio: no matching device (wanted %d)", device_id);
    return false;
}
#endif // !CONFIG_ARCH_X86_64

} // namespace arch
