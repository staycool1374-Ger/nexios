#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/hal/io.hpp>

/// @brief AArch64 GICv2/v3 register definitions and accessors.
///
/// Memory map (QEMU virt):
///   GICD  = 0x0800_0000  (distributor)
///   GICC  = 0x0801_0000  (GICv2 CPU interface)
///   GICR  = 0x080A_0000  (GICv3 redistributor RD frame, core 0)
///   GICR_SGI = 0x080B_0000  (GICv3 SGI/PPI frame, core 0)
///
/// INTID ranges (GICv3):
///   0-15   SGI   (software-generated, IPI)
///   16-31  PPI   (private per-core: timer, PMU, etc.)
///   32-1019 SPI  (shared: UART, virtio, RTC, etc.)
namespace arch {

// ─── MMIO base addresses ─────────────────────────────────────────────────
inline constexpr uint64_t GICD_BASE    = 0x08000000ULL;
inline constexpr uint64_t GICC_BASE    = 0x08010000ULL;   // GICv2 CPU I/F
inline constexpr uint64_t GICR_RD_BASE = 0x080A0000ULL;   // GICv3 RD frame
inline constexpr uint64_t GICR_SGI_BASE = 0x080B0000ULL;  // GICv3 SGI/PPI

// ─── Distributor (GICD) registers ────────────────────────────────────────
inline constexpr uint32_t GICD_CTLR      = 0x0000;
inline constexpr uint32_t GICD_TYPER     = 0x0004;
inline constexpr uint32_t GICD_IIDR      = 0x0008;
inline constexpr uint32_t GICD_IGROUPR   = 0x0080;
inline constexpr uint32_t GICD_ISENABLER = 0x0100;
inline constexpr uint32_t GICD_ICENABLER = 0x0180;
inline constexpr uint32_t GICD_ICPEND    = 0x0280;
inline constexpr uint32_t GICD_ICACTIVE  = 0x0380;
inline constexpr uint32_t GICD_IPRIORITY = 0x0400;
inline constexpr uint32_t GICD_ITARGETSR = 0x0800;
inline constexpr uint32_t GICD_ICFGR     = 0x0C00;
inline constexpr uint32_t GICD_NSACR     = 0x0E00;
inline constexpr uint32_t GICD_SGIR      = 0x0F00;

// ─── GICv3 Redistributor RD frame ────────────────────────────────────────
inline constexpr uint32_t GICR_WAKER      = 0x0014;

// ─── GICv3 SGI/PPI frame ─────────────────────────────────────────────────
inline constexpr uint32_t GICR_IGROUPR0   = 0x0080;
inline constexpr uint32_t GICR_ISENABLER0 = 0x0100;
inline constexpr uint32_t GICR_ICENABLER0 = 0x0180;
inline constexpr uint32_t GICR_IPRIORITY0 = 0x0400;

// ─── GICv2 CPU Interface (GICC) registers ────────────────────────────────
inline constexpr uint32_t GICC_CTLR = 0x0000;
inline constexpr uint32_t GICC_PMR  = 0x0004;
inline constexpr uint32_t GICC_IAR  = 0x000C;
inline constexpr uint32_t GICC_EOIR = 0x0010;

/// @brief GICC_CTLR enable bits (QEMU GICv2 with security extensions).
inline constexpr uint32_t GICC_CTLR_ENABLE     = 1U << 0;  ///< Group 0 / common enable
inline constexpr uint32_t GICC_CTLR_ENABLE_GRP1 = 1U << 1; ///< Group 1 delivery
/// @brief GICD_CTLR enable bits (NS view with security extensions).
inline constexpr uint32_t GICD_CTLR_ENABLE      = 1U << 0;
inline constexpr uint32_t GICD_CTLR_ENABLE_GRP1 = 1U << 1;

// ─── GICv3 system register accessors (ICC_*_EL1) ─────────────────────────
inline void gic_v3_write_eoir(uint64_t intid) {
    asm volatile("msr ICC_EOIR1_EL1, %0" : : "r"(intid) : "memory");
}

inline uint64_t gic_v3_read_iar() {
    uint64_t intid = 0;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(intid));
    return intid;
}

inline void gic_v3_set_pmr(uint64_t mask) {
    asm volatile("msr ICC_PMR_EL1, %0" : : "r"(mask) : "memory");
}

inline void gic_v3_set_igrpen1(bool enable) {
    asm volatile("msr ICC_IGRPEN1_EL1, %0" : : "r"(enable ? 1UL : 0UL) : "memory");
}

inline void gic_v3_set_sre(bool enable) {
    uint64_t sre = 0;
    asm volatile("mrs %0, ICC_SRE_EL1" : "=r"(sre));
    if (enable) sre |= 1;
    else sre &= ~1UL;
    asm volatile("msr ICC_SRE_EL1, %0" : : "r"(sre) : "memory");
}

// ─── MMIO accessors ──────────────────────────────────────────────────────
// All GIC access goes through the TTBR1 higher-half window (boot.S maps the
// 0x08000000 2MiB block at HHDM_OFFSET + 0x08000000).  Raw-physical access
// would depend on the boot identity map, which does not exist in user page
// tables — the first GIC read under a user TTBR0 would fault.
inline volatile uint32_t *gicd_reg(uint32_t offset) {
    return reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                 GICD_BASE + offset);
}

inline volatile uint32_t *gicc_reg(uint32_t offset) {
    return reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                 GICC_BASE + offset);
}

inline volatile uint32_t *gicr_rd_reg(uint32_t offset) {
    return reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                 GICR_RD_BASE + offset);
}

inline volatile uint32_t *gicr_sgi_reg(uint32_t offset) {
    return reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                                 GICR_SGI_BASE + offset);
}

// ─── QEMU virt INTID map (SPI range) ─────────────────────────────────────
inline constexpr uint32_t INTID_UART0   = 33;   // PL011 UART
inline constexpr uint32_t INTID_RTC     = 34;   // MC146818 RTC
inline constexpr uint32_t INTID_VIRTIO0 = 36;   // virtio-mmio devices
inline constexpr uint32_t INTID_VIRTIO1 = 37;
inline constexpr uint32_t INTID_VIRTIO2 = 38;
inline constexpr uint32_t INTID_VIRTIO3 = 39;
inline constexpr uint32_t INTID_VIRTIO4 = 40;

} // namespace arch
