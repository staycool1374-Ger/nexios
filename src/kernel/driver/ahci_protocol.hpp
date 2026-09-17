#pragma once

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

/// @file ahci_protocol.hpp
/// @brief AHCI register layouts, command structures, and FIS types.
///        Shared between kernel-mode driver and userspace (ring-3) driver
///        server. No kernel dependencies — standalone portable header.

#pragma once

#include <types.hpp>

namespace kernel::ahci {

// ──────────────────────────────────────────────
//  Generic Host Bus Adapter (HBA) Registers
//  Base: ABAR (AHCI Base Address Register, PCI BAR 5)
// ──────────────────────────────────────────────

/// HBA register offsets (32-bit unless noted)
constexpr uint32_t HBA_CAP = 0x00;     // Host Capabilities
constexpr uint32_t HBA_GHC = 0x04;     // Global Host Control
constexpr uint32_t HBA_IS = 0x08;      // Interrupt Status
constexpr uint32_t HBA_PI = 0x0C;      // Ports Implemented
constexpr uint32_t HBA_VS = 0x10;      // Version
constexpr uint32_t HBA_CCC_CTL = 0x14; // Command Completion Coalescing Control
constexpr uint32_t HBA_CCC_PORTS = 0x18; // CCC Ports
constexpr uint32_t HBA_EM_LOC = 0x1C;    // Enclosure Management Location
constexpr uint32_t HBA_EM_CTL = 0x20;    // EM Control
constexpr uint32_t HBA_CAP2 = 0x24;      // Host Capabilities Extended
constexpr uint32_t HBA_BOHC = 0x28;      // BIOS/OS Handoff Control

/// CAP bits
constexpr uint32_t CAP_NP = 0x0000001F;    // Number of Ports (bits 0-4)
constexpr uint32_t CAP_SXS = 0x00000020;   // Supports External SATA
constexpr uint32_t CAP_EMS = 0x00000040;   // Enclosure Management Supported
constexpr uint32_t CAP_CCC = 0x00000080;   // Command Completion Coalescing
constexpr uint32_t CAP_NCQ = 0x00010000;   // Supports Native Command Queuing
constexpr uint32_t CAP_64BIT = 0x00080000; // 64-bit addressing supported
constexpr uint32_t CAP_SNCQ = 0x00100000;  // Supports NCQ via SNotification
constexpr uint32_t CAP_SSS = 0x00200000;   // Staggered Spin-up Supported
constexpr uint32_t CAP_SALP = 0x08000000;  // Supports Aggressive Link PM
constexpr uint32_t CAP_SAL = 0x10000000;   // Supports Active LED
constexpr uint32_t CAP_ISS = 0x0F000000; // Interface Speed Support (bits 24-27)
constexpr uint32_t CAP_SNZO = 0x40000000; // Supports Non-Zero DMA Offsets
constexpr uint32_t CAP_SAM = 0x80000000;  // Supports AHCI Mode Only

/// GHC bits
constexpr uint32_t GHC_HR = 0x00000001;   // HBA Reset
constexpr uint32_t GHC_IE = 0x00000002;   // Interrupt Enable
constexpr uint32_t GHC_MRSM = 0x00000004; // MSI Revert to Single Message
constexpr uint32_t GHC_AE = 0x80000000;   // AHCI Enable

/// CAP2 bits
constexpr uint32_t CAP2_BOH = 0x00000001;  // BIOS/OS Handoff
constexpr uint32_t CAP2_NVMP = 0x00000002; // NVMHCI Present
constexpr uint32_t CAP2_APST = 0x00000004; // Automatic Partial→Slumber
constexpr uint32_t CAP2_SDS = 0x00000008;  // Supports Device Sleep
constexpr uint32_t CAP2_SADM = 0x00000010; // Supports Aggressive Device Sleep
constexpr uint32_t CAP2_DESO = 0x00000020; // DevSleep Exit from Slumber Only

/// BOHC bits
constexpr uint32_t BOHC_BOS = 0x00000001; // BIOS Own Semaphore
constexpr uint32_t BOHC_OOS = 0x00000002; // OS Own Semaphore
constexpr uint32_t BOHC_SOO = 0x00000004; // SATA BIOS/OS Ownership
constexpr uint32_t BOHC_OOC = 0x00000008; // OS Ownership Change

/// Version register encoding
constexpr uint32_t VS_MAJOR_MASK = 0xFFFF0000;
constexpr uint32_t VS_MINOR_MASK = 0x0000FFFF;

// ──────────────────────────────────────────────
//  Port Registers (per port, 0x80 bytes each)
//  Port i starts at offset 0x100 + i * 0x80
// ──────────────────────────────────────────────

/// Per-port register offsets (32-bit unless noted)
constexpr uint32_t PORT_BASE = 0x100;
constexpr uint32_t PORT_STRIDE = 0x80;

constexpr uint32_t PORT_CLB = 0x00;  // Command List Base Address (low 32)
constexpr uint32_t PORT_CLBU = 0x04; // Command List Base Address (upper 32)
constexpr uint32_t PORT_FB = 0x08;   // FIS Base Address (low 32)
constexpr uint32_t PORT_FBU = 0x0C;  // FIS Base Address (upper 32)
constexpr uint32_t PORT_IS = 0x10;   // Interrupt Status
constexpr uint32_t PORT_IE = 0x14;   // Interrupt Enable
constexpr uint32_t PORT_CMD = 0x18;  // Command and Status
constexpr uint32_t PORT_RESERVED0 = 0x1C; // (reserved)
constexpr uint32_t PORT_TFD = 0x20;       // Task File Data
constexpr uint32_t PORT_SIG = 0x24;       // Signature

/// PORT_SIG device signatures (AHCI 1.3.1 §3.3.8).
constexpr uint32_t PORT_SIG_SATA = 0x00000101;  // SATA drive
constexpr uint32_t PORT_SIG_ATAPI = 0xEB140101; // ATAPI (CD/DVD) device
constexpr uint32_t PORT_SIG_PM = 0x96690101;    // Port multiplier
constexpr uint32_t PORT_SIG_SEMB = 0xC33C0101;  // Enclosure management
constexpr uint32_t PORT_SSTS = 0x28;      // Serial ATA Status (SCR0)
constexpr uint32_t PORT_SCTL = 0x2C;      // Serial ATA Control (SCR2)
constexpr uint32_t PORT_SERR = 0x30;      // Serial ATA Error (SCR1)
constexpr uint32_t PORT_SACT = 0x34;      // Serial ATA Active (SCR3)
constexpr uint32_t PORT_CI = 0x38;        // Command Issue
constexpr uint32_t PORT_SNTF = 0x3C;      // Serial ATA Notification (SCR4)
constexpr uint32_t PORT_FBS = 0x40;       // FIS-based Switching
constexpr uint32_t PORT_DEVSLP = 0x44;    // Device Sleep
constexpr uint32_t PORT_RESERVED1 = 0x48; // (reserved)
constexpr uint32_t PORT_VS = 0x70;        // Vendor Specific

// ──────────────────────────────────────────────
//  Port Register Bit Definitions
// ──────────────────────────────────────────────

/// PORT_IS bits (Interrupt Status)
constexpr uint32_t PORT_IS_DHRS = 0x00000001; // Device to Host Register FIS
constexpr uint32_t PORT_IS_PSS = 0x00000002;  // PIO Setup FIS
constexpr uint32_t PORT_IS_DSS = 0x00000004;  // DMA Setup FIS
constexpr uint32_t PORT_IS_SDBS = 0x00000008; // Set Device Bits FIS
constexpr uint32_t PORT_IS_UFS = 0x00000010;  // Unknown FIS
constexpr uint32_t PORT_IS_DPS = 0x00000020;  // Descriptor Processed
constexpr uint32_t PORT_IS_PCS = 0x00000040;  // Port Connect Change
constexpr uint32_t PORT_IS_DMPS = 0x00000080; // Device Mechanical Presence
constexpr uint32_t PORT_IS_PRCS = 0x00000100; // PhyRdy Change Status
constexpr uint32_t PORT_IS_IPMS = 0x00000400; // Incorrect Port Multiplier
constexpr uint32_t PORT_IS_OFS = 0x00000800;  // Overflow Status
constexpr uint32_t PORT_IS_INFS = 0x00001000; // Interface Non-Fatal Error
constexpr uint32_t PORT_IS_IFS = 0x00002000;  // Interface Fatal Error
constexpr uint32_t PORT_IS_HBDS = 0x00004000; // Host Bus Data Error
constexpr uint32_t PORT_IS_HBFS = 0x00008000; // Host Bus Fatal Error
constexpr uint32_t PORT_IS_TFES = 0x00010000; // Task File Error Status
constexpr uint32_t PORT_IS_CPDS = 0x00020000; // Cold Port Detect

/// PORT_CMD bits
constexpr uint32_t CMD_ST = 0x00000001;   // Start (port DMA engine)
constexpr uint32_t CMD_SUD = 0x00000002;  // Spin-Up Device
constexpr uint32_t CMD_POD = 0x00000004;  // Power On Device
constexpr uint32_t CMD_CLO = 0x00000008;  // Command List Override
constexpr uint32_t CMD_FRE = 0x00000010;  // FIS Receive Enable
constexpr uint32_t CMD_CCS = 0x000001F0;  // Current Command Slot (bits 4-8)
constexpr uint32_t CMD_MPSS = 0x00000200; // Mechanical Presence Switch
constexpr uint32_t CMD_FR = 0x00004000;   // FIS Receive Running
constexpr uint32_t CMD_CR = 0x00008000;   // Command List Running
constexpr uint32_t CMD_CPS = 0x00010000;  // Cold Presence State
constexpr uint32_t CMD_PMA = 0x00020000;  // Port Multiplier Attached
constexpr uint32_t CMD_HP = 0x00040000;   // Hot-Plug Capable
constexpr uint32_t CMD_MPSP = 0x00080000; // Mechanical Presence Switch Attached
constexpr uint32_t CMD_CPD = 0x00100000;  // Cold Plug Detect Capable
constexpr uint32_t CMD_ESP = 0x00200000;  // External SATA Port
constexpr uint32_t CMD_FBSCP = 0x00400000; // FIS-based Switching Capable
constexpr uint32_t CMD_APSTE = 0x00800000; // Auto Partial→Slumber Transitions
constexpr uint32_t CMD_ATAPI = 0x01000000; // Device is ATAPI
constexpr uint32_t CMD_DLAE = 0x04000000;  // Drive LED on ATAPI Enable
constexpr uint32_t CMD_ALPE = 0x08000000;  // Aggressive Link PM Enable
constexpr uint32_t CMD_ASP = 0x10000000;   // Aggressive Slumber/Partial
constexpr uint32_t CMD_ICC = 0xF0000000;   // Interface Communication Control

/// PORT_TFD bits (Task File Data)
constexpr uint32_t TFD_BSY = 0x00000080; // Busy
constexpr uint32_t TFD_DRQ = 0x00000040; // Data Request
constexpr uint32_t TFD_ERR = 0x00000100; // Error

/// PORT_SSTS bits (SATA Status)
constexpr uint32_t SSTS_DET_MASK = 0x0000000F; // Device Detection
constexpr uint32_t SSTS_DET_NODEVICE = 0x00;   // No device detected
constexpr uint32_t SSTS_DET_DETACHED = 0x01;   // Device presence, phy offline
constexpr uint32_t SSTS_DET_ONLINE = 0x03;     // Device present, phy online
constexpr uint32_t SSTS_DET_OFFLINE =
    0x04; // Device presence, phy offline (sleep)
constexpr uint32_t SSTS_SPD_MASK = 0x000000F0; // Negotiated Speed (bits 4-7)
constexpr uint32_t SSTS_IPM_MASK =
    0x00000F00; // Interface Power Management (bits 8-11)

/// PORT_SERR bits (SATA Error)
constexpr uint32_t SERR_DIAG_X = 0x00000001;  // Exchanged
constexpr uint32_t SERR_DIAG_F = 0x00000002;  // Unknown FIS
constexpr uint32_t SERR_DIAG_T = 0x00000004;  // Transport error
constexpr uint32_t SERR_DIAG_S = 0x00000008;  // Link sequence error
constexpr uint32_t SERR_DIAG_H = 0x00000010;  // Handshake error
constexpr uint32_t SERR_DIAG_C = 0x00000020;  // CRC error
constexpr uint32_t SERR_DIAG_D = 0x00000040;  // Disparity error
constexpr uint32_t SERR_DIAG_B = 0x00000080;  // Buffer over/underrun
constexpr uint32_t SERR_DIAG_W = 0x00000100;  // Wake-up
constexpr uint32_t SERR_DIAG_I = 0x00000200;  // Phy Internal Error
constexpr uint32_t SERR_DIAG_N = 0x00000400;  // PhyRdy Change
constexpr uint32_t SERR_DIAG_P = 0x00010000;  // Phy Internal Error
constexpr uint32_t SERR_DIAG_C1 = 0x01000000; // CRC Error (non-transmit)

/// PORT_IE (Interrupt Enable) uses same bit positions as PORT_IS.

// ──────────────────────────────────────────────
//  Command List
//  Each entry is a Command Header (32 bytes).
//  32 entries per port (max).
// ──────────────────────────────────────────────

/// Maximum command slots per port
constexpr uint32_t AHCI_MAX_CMDS = 32;

/// Command Header (32 bytes, AHCI 1.3.1 §4.2.2).
struct CmdHeader {
    // DW0 low
    uint16_t opts; // CFL[4:0] + flags A[5] W[6] P[7] R[8] B[9] C[10]
    // DW0 high
    uint16_t prdtl; // Physical Region Descriptor Table Length (entry count)
    // DW1
    uint32_t prdbc; // Physical Region Descriptor Byte Count (transferred)
    // DW2
    uint32_t ctba; // Command Table Descriptor Base Address (low 32)
    // DW3
    uint32_t ctbau; // Command Table Descriptor Base Address (upper 32)
    // DW4–DW7: Reserved
    uint32_t rsvd1[4];
} __attribute__((packed));

/// Command Header flags for attrs field
constexpr uint16_t CMDHDR_CFL_MASK = 0x001F;
constexpr uint16_t CMDHDR_ATAPI = 0x0020;    // Device is ATAPI
constexpr uint16_t CMDHDR_WRITE = 0x0040;    // Write (host→device)
constexpr uint16_t CMDHDR_CLR_BUSY = 0x0080; // Clear Busy upon R_OK
constexpr uint16_t CMDHDR_PREFETCH = 0x0100; // Prefetchable
constexpr uint16_t CMDHDR_RESET = 0x0200;    // Reset
constexpr uint16_t CMDHDR_BIST = 0x0400;     // BIST
constexpr uint16_t CMDHDR_CMP_PMA = 0x0800;  // Completion for Port Multiplier

// ──────────────────────────────────────────────
//  Command Table (128 bytes header + up to 8K PRD entries)
// ──────────────────────────────────────────────

/// Command FIS (FIS Type 0x27, 5 DWORDS = 20 bytes)
struct CmdFIS {
    // DW0
    uint8_t fis_type;  // 0x27
    uint8_t pm_port_c; // bit 7 = C (is command), bits 0-3 = PM port
    uint8_t command;   // ATA command (READ DMA EXT = 0x25, etc.)
    uint8_t features;
    // DW1
    uint8_t lba_lo;
    uint8_t lba_mid;
    uint8_t lba_hi;
    uint8_t device; // master=0x40, slave=0x50, LBA=0xE0
    // DW2
    uint8_t lba_lo_exp;
    uint8_t lba_mid_exp;
    uint8_t lba_hi_exp;
    uint8_t features_exp;
    // DW3
    uint8_t count_lo; // sector count (low byte)
    uint8_t count_hi; // sector count (high byte)
    uint8_t icc;      // Isochronous Command Completion
    uint8_t control;
    // DW4
    uint32_t aux; // auxiliary
} __attribute__((packed));

static_assert(sizeof(CmdFIS) == 20, "CmdFIS must be 20 bytes");

/// FIS types
constexpr uint8_t FIS_TYPE_REG_H2D = 0x27;   // Register FIS - Host to Device
constexpr uint8_t FIS_TYPE_REG_D2H = 0x34;   // Register FIS - Device to Host
constexpr uint8_t FIS_TYPE_DMA_ACT = 0x39;   // DMA Activate FIS
constexpr uint8_t FIS_TYPE_DMA_SETUP = 0x41; // DMA Setup FIS
constexpr uint8_t FIS_TYPE_DATA = 0x46;      // Data FIS
constexpr uint8_t FIS_TYPE_BIST = 0x58;      // BIST Activate FIS
constexpr uint8_t FIS_TYPE_PIO_SETUP = 0x5F; // PIO Setup FIS
constexpr uint8_t FIS_TYPE_DEV_BITS = 0xA1;  // Set Device Bits FIS

/// PRD entry (16 bytes)
struct PrdHbaEntry {
    uint32_t dba;  // Data Base Address (low 32)
    uint32_t dbau; // Data Base Address (upper 32, if CAP & CAP_64BIT)
    uint32_t reserved;
    uint32_t byte_count; // Byte count (22 bits: bits 0-21), bit 31 = Interrupt
                         // on Completion
} __attribute__((packed));

constexpr uint32_t PRD_BYTE_COUNT_MASK = 0x00FFFFFF;
constexpr uint32_t PRD_IOC = 0x80000000; // Interrupt on Completion

/// Maximum bytes per PRD entry (4 MB minus 1)
constexpr uint32_t PRD_MAX_BYTES = 0x00400000;

/// Maximum PRD entries per command table
constexpr uint32_t AHCI_MAX_PRD = 256;

/// Command Table (aligned to 128 bytes)
/// Contains command FIS + ATAPI command + PRD entries
struct CmdTable {
    CmdFIS cfis;                   // 20 bytes
    uint8_t atapi_cmd[16];         // ATAPI command (if ATAPI)
    uint8_t rsvd0[92];             // padding to 128 bytes
    PrdHbaEntry prd[AHCI_MAX_PRD]; // PRD entries
} __attribute__((packed));

static_assert(sizeof(CmdTable) == 128 + 256 * sizeof(PrdHbaEntry),
              "CmdTable size mismatch");

// ──────────────────────────────────────────────
//  Received FIS Structure (from PORT_FB)
//  Aligned to 256 bytes, holds shadow registers + D2H + DMA + PIO + SDB
// ──────────────────────────────────────────────

/// Received FIS (RFIS) structure — DMA'd by HBA on each FIS receive
struct __attribute__((packed)) ReceivedFis {
    uint8_t rsvd0[0x20];   // Shadow register buffer (0x00–0x1F)
    uint32_t d2h_fis[2];   // D2H Register FIS (offset 0x20)
    uint32_t dma_setup[2]; // DMA Setup FIS (offset 0x28)
    uint32_t pio_setup[2]; // PIO Setup FIS (offset 0x30)
    uint32_t sdb_fis[2];   // Set Device Bits FIS (offset 0x38)
    uint8_t rsvd1[0xA0];   // (offset 0x40–0xDF)
    uint16_t vendor[0x10]; // Vendor-specific (0xE0–0xFF)
};

static_assert(sizeof(ReceivedFis) == 0x100, "ReceivedFis must be 256 bytes");

// ──────────────────────────────────────────────
//  ATA Commands (subset relevant to AHCI/SATA)
// ──────────────────────────────────────────────

constexpr uint8_t ATA_CMD_READ_DMA_EXT = 0x25;
constexpr uint8_t ATA_CMD_WRITE_DMA_EXT = 0x35;
constexpr uint8_t ATA_CMD_READ_FPDMA_QUEUED = 0x60;  // NCQ read
constexpr uint8_t ATA_CMD_WRITE_FPDMA_QUEUED = 0x61; // NCQ write
constexpr uint8_t ATA_CMD_IDENTIFY = 0xEC;
constexpr uint8_t ATA_CMD_SET_FEATURES = 0xEF;
constexpr uint8_t ATA_CMD_FLUSH_CACHE = 0xE7;
constexpr uint8_t ATA_CMD_FLUSH_CACHE_EXT = 0xEA;
constexpr uint8_t ATA_CMD_STANDBY_IMMEDIATE = 0xE0;
constexpr uint8_t ATA_CMD_IDLE_IMMEDIATE = 0xE1;

/// Device bits
constexpr uint8_t ATA_DEV_LBA = 0xE0; // LBA mode
constexpr uint8_t ATA_DEV_MASTER = 0x00;
constexpr uint8_t ATA_DEV_SLAVE = 0x10;

// ──────────────────────────────────────────────
//  Port Multiplier definitions (for NCQ tag encoding)
// ──────────────────────────────────────────────

/// NCQ: tag is encoded in bits 3-7 of the PM_PORT_C byte in CmdFIS
constexpr uint8_t NCQ_TAG_SHIFT = 3;
constexpr uint8_t NCQ_TAG_MASK = 0x1F;

} // namespace kernel::ahci