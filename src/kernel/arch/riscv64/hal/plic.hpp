#pragma once

#include <types.hpp>
#include <kernel/arch/hal/io.hpp>

/// @brief RISC-V 64 PLIC (Platform-Level Interrupt Controller) register
/// definitions and accessors.
///
/// Memory map (QEMU virt):
///   PLIC_BASE     = 0x0C00_0000
///   Priority      = base + 0x0000_0000  (one 4-byte word per source)
///   Pending       = base + 0x0000_1000
///   Enable        = base + 0x0000_2000  (one bit per source)
///   Threshold     = base + 0x0020_0000
///   Claim/Complete= base + 0x0020_0004
///
/// IRQ map (QEMU virt):
///   0   = reserved
///   1-8 = virtio-mmio devices
///   9   = (unused)
///   10  = UART
///   11  = RTC (goldfish)
///   12  = keyboard (PS/2)
///   32-35 = PCIe INTx
namespace arch {

inline constexpr uint64_t PLIC_BASE     = 0x0C000000ULL;
inline constexpr uint64_t PLIC_PRIORITY = PLIC_BASE + 0x000000;
inline constexpr uint64_t PLIC_PENDING  = PLIC_BASE + 0x001000;
inline constexpr uint64_t PLIC_ENABLE   = PLIC_BASE + 0x002000;
inline constexpr uint64_t PLIC_THRESHOLD = PLIC_BASE + 0x200000;
inline constexpr uint64_t PLIC_CLAIM    = PLIC_BASE + 0x200004;

/// @brief S-mode context (hart 0) register block (issue #349). The kernel
/// runs in S-mode: all PLIC programming uses these, never the M-context
/// aliases above (OpenSBI owns context 0).
inline constexpr uint64_t PLIC_S_CONTEXT = 1;
inline constexpr uint64_t PLIC_ENABLE_S =
    PLIC_BASE + 0x002000 + PLIC_S_CONTEXT * 0x80;
inline constexpr uint64_t PLIC_THRESHOLD_S =
    PLIC_BASE + 0x200000 + PLIC_S_CONTEXT * 0x1000;
inline constexpr uint64_t PLIC_CLAIM_S = PLIC_THRESHOLD_S + 4;

/// @brief Highest source ID the kernel addresses (96 sources, 3 words).
inline constexpr uint32_t PLIC_MAX_SOURCES = 96;

/// @brief QEMU virt IRQ numbers.
inline constexpr uint32_t IRQ_VIRTIO0 = 1;
inline constexpr uint32_t IRQ_VIRTIO1 = 2;
inline constexpr uint32_t IRQ_VIRTIO2 = 3;
inline constexpr uint32_t IRQ_VIRTIO3 = 4;
inline constexpr uint32_t IRQ_VIRTIO4 = 5;
/// @brief virtio-mmio IRQ window size (issue #350: IRQs 1-8).
inline constexpr uint32_t IRQ_VIRTIO_COUNT = 8;
inline constexpr uint32_t IRQ_UART    = 10;
inline constexpr uint32_t IRQ_RTC     = 11;   // Goldfish RTC
inline constexpr uint32_t IRQ_KEYBOARD = 12;
inline constexpr uint32_t IRQ_PCI_BASE = 32;  // PCIe INTx 32-35

// ─── Accessors ───────────────────────────────────────────────────────────
inline volatile uint32_t *plic_priority_reg(uint32_t irq) {
    return reinterpret_cast<volatile uint32_t *>(PLIC_PRIORITY + irq * 4);
}

inline volatile uint32_t *plic_enable_reg(uint32_t irq) {
    return reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE + (irq / 32) * 4);
}

inline uint32_t plic_claim() {
    return *reinterpret_cast<volatile uint32_t *>(PLIC_CLAIM);
}

inline void plic_complete(uint32_t intid) {
    *reinterpret_cast<volatile uint32_t *>(PLIC_CLAIM) = intid;
}

// ─── S-context helpers (issue #349) ────────────────────────────────────

/// @brief Set an S-context source priority (0 = never interrupt).
inline void plic_set_priority(uint32_t irq, uint32_t prio) {
    if (irq == 0 || irq >= PLIC_MAX_SOURCES) {
        return;
    }
    *reinterpret_cast<volatile uint32_t *>(PLIC_PRIORITY + irq * 4) = prio;
}

/// @brief Read back an S-context source priority (diagnostic).
inline uint32_t plic_get_priority(uint32_t irq) {
    if (irq == 0 || irq >= PLIC_MAX_SOURCES) {
        return 0;
    }
    return *reinterpret_cast<volatile uint32_t *>(PLIC_PRIORITY + irq * 4);
}

/// @brief Set the S-context priority threshold.
inline void plic_set_threshold(uint32_t threshold) {
    *reinterpret_cast<volatile uint32_t *>(PLIC_THRESHOLD_S) = threshold;
}

/// @brief Read back the S-context priority threshold.
inline uint32_t plic_get_threshold() {
    return *reinterpret_cast<volatile uint32_t *>(PLIC_THRESHOLD_S);
}

/// @brief S-context enable word for an IRQ (words 0-2 cover 0-95).
inline volatile uint32_t *plic_enable_reg_s(uint32_t irq) {
    return reinterpret_cast<volatile uint32_t *>(PLIC_ENABLE_S +
                                                 (irq / 32) * 4);
}

/// @brief Claim the highest-priority pending S-context interrupt (0 = none).
inline uint32_t plic_claim_s() {
    return *reinterpret_cast<volatile uint32_t *>(PLIC_CLAIM_S);
}

/// @brief Complete an S-context interrupt (must match the claim).
inline void plic_complete_s(uint32_t intid) {
    *reinterpret_cast<volatile uint32_t *>(PLIC_CLAIM_S) = intid;
}

/// @brief S-context index for a hart's S-mode (M-mode takes even slots).
/// @param hart_id Hardware hart ID (matches the DTB cpu reg).
/// @return S-mode PLIC context number for that hart.
inline constexpr uint64_t plic_s_context_for_hart(uint64_t hart_id) {
    return 2U * hart_id + 1U;
}

/// @brief Set the priority threshold for a hart's S-context.
/// Each AP programs its own frame (thresholds are per-context banks); the
/// BSP frame stays owned by interrupt_controller.cpp init.
/// @param hart_id Hardware hart ID.
/// @param threshold Priority threshold (0 = deliver everything enabled).
inline void plic_threshold_s_for_hart(uint64_t hart_id, uint32_t threshold) {
    const uint64_t context = plic_s_context_for_hart(hart_id);
    *reinterpret_cast<volatile uint32_t *>(PLIC_BASE + 0x00200000UL +
                                           context * 0x1000UL) = threshold;
    asm volatile("fence iorw, iorw" : : : "memory");
}

} // namespace arch
