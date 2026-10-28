#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/aarch64/hal/percpu.hpp>

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
inline constexpr uint32_t GICD_PIDR2     = 0xFE8;  // peripheral ID2 (version field)

// ─── GICv3 Redistributor RD frame ────────────────────────────────────────
inline constexpr uint32_t GICR_WAKER      = 0x0014;
/// @brief GICR_WAKER.Children_Online bit: redistributor finished its wake-up.
inline constexpr uint32_t GICR_WAKER_CHILDREN_ONLINE = 1U << 1;
/// @brief Bounded poll budget for redistributor wake-up (issue #198, INV-1:
///        boot hangs are forbidden — expiry surfaces via gic_redist_ready()
///        as CTRL_TIMEOUT from early_irq_init, which then skips Timer::init).
inline constexpr uint32_t GICR_WAKER_MAX_POLLS = 1000000;

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
inline constexpr uint32_t INTID_RTC     = 34;   // RTC (PL031)
inline constexpr uint32_t INTID_PCIE0   = 35;   // PCIe INTx
inline constexpr uint32_t INTID_PCIE1   = 36;
inline constexpr uint32_t INTID_PCIE2   = 37;
inline constexpr uint32_t INTID_PCIE3   = 38;
inline constexpr uint32_t INTID_VIRTIO_BASE  = 48;  // virtio-mmio devices
inline constexpr uint32_t INTID_VIRTIO_COUNT = 32; // 48..79
/// @brief Highest INTID the kernel addresses (8 enable banks x 32).
inline constexpr uint32_t GIC_MAX_INTID = 256;

/// @brief GICD_SGIR target-list filter: deliver to the listed CPUs.
inline constexpr uint32_t GICD_SGIR_FILTER_TARGET_LIST = 0U << 24;
/// @brief GICD_SGIR INTID mask (bits [3:0]: SGI 0-15).
inline constexpr uint32_t GICD_SGIR_INTID_MASK = 0xFU;
/// @brief GICD SGI clear/set-pending registers (one per SGI, CPU mask).
inline constexpr uint32_t GICD_CPENDSGIR = 0x0F10;
inline constexpr uint32_t GICD_SPENDSGIR = 0x0F20;
/// @brief GICD_SGIR CPU-target-list shift (bit n = CPU interface n).
inline constexpr uint32_t GICD_SGIR_TARGET_SHIFT = 16;

/// @brief SGI INTIDs reserved for SMP (#351 slice-2). GIC INTID N
/// dispatches to IDT vector N — so SGIs must avoid the named IDT vectors
/// (TIMER=0, KEYBOARD=1, SYSCALL=2) and INTID 30 (remapped to vector 0
/// by the timer path). 8/9/10 land on free IDT slots.
inline constexpr uint32_t SMP_SCHED_SGI = 9;
/// @brief SGI INTID reserved for TLB-shootdown batch apply (#351 s2).
/// 1:1-mapped from the x86 SHOOTDOWN_BATCH_VECTOR role.
inline constexpr uint32_t SMP_SHOOTDOWN_SGI = 10;
/// @brief SGI INTID for the boot-time loopback probe + QE use (#351 s2).
/// Kept registered after boot (slice-5 owns the test text).
inline constexpr uint32_t SMP_LOOPBACK_SGI = 8;

/// @brief SGI INTIDs the kernel uses (loopback + scheduler + shootdown).
/// Shared enable mask for distributor (v2) / redistributor (v3) PPI banks.
inline constexpr uint32_t SMP_SGI_ENABLE_MASK =
    (1U << SMP_LOOPBACK_SGI) | (1U << SMP_SCHED_SGI) |
    (1U << SMP_SHOOTDOWN_SGI);

/// @brief GICR_TYPER register offset (64-bit affinity + processor number).
inline constexpr uint32_t GICR_TYPER = 0x0008;
/// @brief Per-CPU redistributor stride (128 KiB: 64 KiB RD + 64 KiB SGI).
inline constexpr uint64_t GICR_STRIDE = 0x20000ULL;

/// @brief Find this CPU's redistributor RD frame by MPIDR Aff0 (#351 s2).
/// Spec-correct TYPER match (never a stride assumption): bounded scan over
/// CONFIG_MAX_CPUS frames, first Aff0 match wins, 0 on no match.
/// @param aff0 MPIDR Aff0 of the calling CPU.
/// @return HHDM-aliased RD frame address, or 0 when not found.
inline uint64_t gicr_frame_for_aff0(uint64_t aff0) {
    for (uint64_t idx = 0; idx < CONFIG_MAX_CPUS; ++idx) {
        volatile uint64_t *typer = reinterpret_cast<volatile uint64_t *>(
            arch::HHDM_OFFSET + GICR_RD_BASE + idx * GICR_STRIDE + GICR_TYPER);
        const uint64_t typer_val = *typer;
        if (((typer_val >> 32U) & 0xFFU) == (aff0 & 0xFFU)) {
            return arch::HHDM_OFFSET + GICR_RD_BASE + idx * GICR_STRIDE;
        }
    }
    return 0;
}

/// @brief GIC version from GICD_PIDR2[7:4] (2 = v2, >= 3 = v3).
/// 0/unreadable fails safe to v2; GICD_TYPER[15:11] is NOT a version field.
inline uint32_t gic_version() {
    volatile uint32_t *pidr2 =
        reinterpret_cast<volatile uint32_t *>(arch::HHDM_OFFSET +
                                              GICD_BASE + GICD_PIDR2);
    uint32_t ver = (*pidr2 >> 4) & 0xFU;
    return (ver == 0) ? 2U : ver;
}

/// @brief Byte offset of an INTID's IPRIORITY byte.
inline constexpr uint32_t gicd_ipriority_off(uint32_t intid) {
    return GICD_IPRIORITY + intid;
}

/// @brief Byte offset of an INTID's ITARGETSR byte (CPU0 = 0x01).
inline constexpr uint32_t gicd_itargetsr_off(uint32_t intid) {
    return GICD_ITARGETSR + intid;
}

/// @brief Word offset and 2-bit shift of an INTID's ICFGR field.
inline constexpr uint32_t gicd_icfgr_word(uint32_t intid) {
    return GICD_ICFGR + (intid / 16) * 4;
}
inline constexpr uint32_t gicd_icfgr_shift(uint32_t intid) {
    return (intid % 16) * 2;
}

/// @brief Word index of an INTID's enable bank (0-7).
inline constexpr uint32_t gicd_bank(uint32_t intid) { return intid / 32; }

/// @brief Send a software-generated interrupt to one CPU (#351 slice-2).
/// v2 path: GICD_SGIR with target-list filter (bit n = CPU interface n,
/// valid for the 8-CPU CONFIG_MAX_CPUS build).  v3 path: ICC_SGI1R_EL1
/// with Aff1 = 0, TargetList = (1 << cpu) (single-cluster QEMU virt:
/// Aff3 == Aff2 == 0, Aff0 selects the list bit).  Fail-closed false on
/// out-of-range cpu/intid; never programs reserved INTIDs.
/// @param target_cpu GIC target (Aff0 / CPU interface, 0..CONFIG_MAX_CPUS-1).
/// @param intid SGI INTID (kernel set: SMP_LOOPBACK/SCHED/SHOOTDOWN_SGI).
/// @return true when the SGI was issued, false when rejected.
inline bool gic_send_sgi(uint32_t target_cpu, uint32_t intid) {
    if (target_cpu >= CONFIG_MAX_CPUS || intid > 15U) {
        return false;
    }
    // Only the kernel SGI set may be programmed (INTIDs 8/9/10 land on
    // free IDT vectors; 0/1/2 would alias TIMER/KEYBOARD/SYSCALL).
    if (intid != SMP_LOOPBACK_SGI && intid != SMP_SCHED_SGI &&
        intid != SMP_SHOOTDOWN_SGI) {
        return false;
    }
    if (gic_version() >= 3U) {
        const uint64_t sgi_value =
            (static_cast<uint64_t>(intid & 0xFU) << 24) |
            (static_cast<uint64_t>(1U << target_cpu));
        asm volatile("msr ICC_SGI1R_EL1, %0" : : "r"(sgi_value) : "memory");
        asm volatile("isb");
    } else {
        const uint32_t sgir_value =
            GICD_SGIR_FILTER_TARGET_LIST |
            ((1U << target_cpu) << GICD_SGIR_TARGET_SHIFT) |
            (intid & GICD_SGIR_INTID_MASK);
        gicd_reg(GICD_SGIR)[0] = sgir_value;
        dsb_sy();
    }
    return true;
}

/// @brief Per-AP GIC CPU-interface enable (#351 slice-2).
/// GIC CPU interfaces are banked per CPU at shared addresses: the BSP
/// init in interrupt_controller.cpp only programs the BSP's bank, so each
/// AP must enable its own.  v2: GICC_PMR + GICC_CTLR (PPI enables live in
/// the shared distributor, BSP-owned).  v3: own redistributor frame
/// (wake + PPI 30 + SGIs) plus SRE/PMR/IGRPEN1 (system registers,
/// inherently per-CPU).  Runs on the AP itself (own bank/frame).
inline void gic_ap_init() {
    if (gic_version() >= 3U) {
        const uint64_t aff0 = mpidr_aff0();
        const uint64_t rd_frame = gicr_frame_for_aff0(aff0);
        if (rd_frame != 0U) {
            volatile uint32_t *rr =
                reinterpret_cast<volatile uint32_t *>(rd_frame);
            volatile uint32_t *rs = reinterpret_cast<volatile uint32_t *>(
                rd_frame + (GICR_SGI_BASE - GICR_RD_BASE));
            uint32_t waker = rr[GICR_WAKER / 4U];
            waker &= ~(1U << 0U);
            rr[GICR_WAKER / 4U] = waker;
            dsb_sy();
            for (uint32_t poll = 0; poll < GICR_WAKER_MAX_POLLS; ++poll) {
                if ((rr[GICR_WAKER / 4U] & GICR_WAKER_CHILDREN_ONLINE) !=
                    0U) {
                    break;
                }
                dsb_sy();
            }
            rs[GICR_ISENABLER0 / 4U] |=
                SMP_SGI_ENABLE_MASK | (1U << 30U);
            dsb_sy();
        }
        gic_v3_set_sre(true);
        gic_v3_set_pmr(0xFF);
        gic_v3_set_igrpen1(true);
        isb();
    } else {
        gicc_reg(GICC_PMR)[0] = 0xFF;
        dsb_sy();
        gicc_reg(GICC_CTLR)[0] = GICC_CTLR_ENABLE;
        dsb_sy();
    }
}

/// @brief Enable the kernel SGI set for SMP (#351 slice-2).
/// Called from bring_up() only (never on single-CPU runs, never from the
/// shared init path — the existing tests observe the BSP init bit-exact).
/// Enables only (no distributor clears: the SGI pending state is
/// sender-owned — a clear-all would need per-CPU ACKED-idle guarantees
/// this early context cannot provide).
/// v2: distributor (shared bank).  v3: own redistributor frame (the AP
/// frames are enabled by gic_ap_init on each AP).
inline void gic_enable_sgi() {
    if (gic_version() >= 3U) {
        const uint64_t rd_frame = gicr_frame_for_aff0(mpidr_aff0());
        if (rd_frame != 0U) {
            volatile uint32_t *rs = reinterpret_cast<volatile uint32_t *>(
                rd_frame + (GICR_SGI_BASE - GICR_RD_BASE));
            rs[GICR_ISENABLER0 / 4U] |= SMP_SGI_ENABLE_MASK;
            dsb_sy();
        }
    } else {
        gicd_reg(GICD_ISENABLER)[0] |= SMP_SGI_ENABLE_MASK;
        dsb_sy();
    }
}

} // namespace arch
