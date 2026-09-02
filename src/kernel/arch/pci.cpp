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

/// @file pci.cpp
/// @brief PCI bus enumeration — arch-independent (uses HAL config space
/// access).

#include <kernel/arch/pci.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/address.hpp>
#include <logger.hpp>

using namespace kernel;

namespace {

static uint64_t msix_page_align_down(uint64_t x) {
    return x & ~0xFFFULL;
}
static uint64_t msix_page_align_up(uint64_t x) {
    return (x + 0xFFF) & ~0xFFFULL;
}

/// Fixed-size buffer for discovered devices.
arch::PciDeviceInfo g_devices[arch::PCI_MAX_DEVICES_FOUND];
size_t g_device_count = 0;

/// Write decimal uint64 to buffer, returns bytes written (excluding null).
static size_t fmt_dec(char *buf, size_t size, uint64_t n) {
    if (size == 0)
        return 0;
    char tmp[24];
    int p = 24;
    tmp[--p] = '\0';
    if (n == 0)
        tmp[--p] = '0';
    else
        while (n) {
            tmp[--p] = '0' + static_cast<char>(n % 10);
            n /= 10;
        }
    size_t i = 0;
    while (tmp[p] && i < size - 1)
        buf[i++] = tmp[p++];
    if (i < size)
        buf[i] = '\0';
    return i;
}

/// Write hex uint64 with fixed digit count to buffer, returns bytes written.
static size_t fmt_hex(char *buf, size_t size, uint64_t n, int digits) {
    if (size == 0)
        return 0;
    size_t i = 0;
    for (int d = digits - 1; d >= 0 && i < size - 1; --d) {
        uint8_t nibble = static_cast<uint8_t>((n >> (d * 4)) & 0xF);
        buf[i++] =
            static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    if (i < size)
        buf[i] = '\0';
    return i;
}

/// Probe a single BDF; if present, read its info and store it.
void probe_bdf(arch::PciBdf bdf) {
    if (!arch::pci_device_exists(bdf))
        return;
    if (g_device_count >= arch::PCI_MAX_DEVICES_FOUND) {
        Logger::warn("PCI: device buffer full, stopping scan");
        return;
    }
    g_devices[g_device_count] = arch::pci_read_device_info(bdf);
    ++g_device_count;
}

/// Probe all functions of a device, if it is multi-function.
void probe_device(arch::PciBdf bdf) {
    probe_bdf(bdf);
    arch::PciBdf func_bdf = bdf;
    for (uint8_t fn = 1; fn < arch::PCI_MAX_FUNCTIONS; ++fn) {
        func_bdf.function = fn;
        if (arch::pci_device_exists(func_bdf)) {
            probe_bdf(func_bdf);
        }
    }
}

/// Probe all devices on a given bus.
void probe_bus(uint8_t bus) {
    arch::PciBdf bdf{};
    bdf.bus = bus;
    bdf.function = 0;
    for (uint8_t dev = 0; dev < arch::PCI_MAX_DEVICES; ++dev) {
        bdf.device = dev;
        arch::PciBdf bdf0 = bdf;
        if (!arch::pci_device_exists(bdf0))
            continue;

        uint8_t header_type = arch::pci_config_readb(
            arch::pci_make_addr(bdf0, arch::PCI_HEADER_TYPE));

        if (header_type & arch::PCI_HEADER_TYPE_MULTI) {
            probe_device(bdf0);
        } else {
            probe_bdf(bdf0);
        }

        // If this is a PCI-to-PCI bridge, scan the secondary bus.
        uint8_t class_reg =
            arch::pci_config_readb(arch::pci_make_addr(bdf0, arch::PCI_CLASS));
        uint8_t subclass_reg = arch::pci_config_readb(
            arch::pci_make_addr(bdf0, arch::PCI_SUBCLASS));
        if (class_reg == 0x06 && subclass_reg == 0x04) {
            uint8_t secondary = arch::pci_config_readb(
                arch::pci_make_addr(bdf0, arch::PCI_SECONDARY_BUS));
            probe_bus(secondary);
        }
    }
}

static const char *pci_class_name(uint8_t class_code) {
    switch (class_code) {
    case 0x00:
        return "Legacy";
    case 0x01:
        return "Storage";
    case 0x02:
        return "Network";
    case 0x03:
        return "Display";
    case 0x04:
        return "Multimedia";
    case 0x05:
        return "Memory";
    case 0x06:
        return "Bridge";
    case 0x07:
        return "Comm";
    case 0x08:
        return "Peripheral";
    case 0x09:
        return "Input";
    case 0x0A:
        return "Docking";
    case 0x0B:
        return "Processor";
    case 0x0C:
        return "SerialBus";
    case 0x0D:
        return "Wireless";
    case 0x0E:
        return "I2O";
    case 0x0F:
        return "Satellite";
    case 0x10:
        return "Encrypt";
    case 0x11:
        return "SignalProc";
    default:
        return "Unknown";
    }
}

static const char *pci_subclass_name(uint8_t class_code, uint8_t subclass) {
    if (class_code == 0x01) {
        switch (subclass) {
        case 0x00:
            return "SCSI";
        case 0x01:
            return "IDE";
        case 0x02:
            return "Floppy";
        case 0x03:
            return "IPI";
        case 0x04:
            return "RAID";
        case 0x05:
            return "ATA";
        case 0x06:
            return "SATA";
        case 0x07:
            return "SAS";
        case 0x08:
            return "NVM";
        case 0x09:
            return "UFS";
        default:
            return "Other";
        }
    }
    if (class_code == 0x02) {
        switch (subclass) {
        case 0x00:
            return "Ethernet";
        case 0x01:
            return "TokenRing";
        case 0x02:
            return "FDDI";
        case 0x03:
            return "ATM";
        case 0x04:
            return "ISDN";
        case 0x05:
            return "WorldFip";
        case 0x06:
            return "PICMG";
        case 0x07:
            return "Infiniband";
        case 0x08:
            return "Fabric";
        default:
            return "Other";
        }
    }
    if (class_code == 0x03) {
        switch (subclass) {
        case 0x00:
            return "VGA";
        case 0x01:
            return "XGA";
        case 0x02:
            return "3D";
        default:
            return "Other";
        }
    }
    if (class_code == 0x06) {
        switch (subclass) {
        case 0x00:
            return "HostBridge";
        case 0x01:
            return "ISA";
        case 0x02:
            return "EISA";
        case 0x03:
            return "MCABridge";
        case 0x04:
            return "PCI2PCI";
        case 0x05:
            return "PCMCIA";
        case 0x06:
            return "NuBus";
        case 0x07:
            return "CardBus";
        case 0x08:
            return "RACEway";
        case 0x09:
            return "PCI2PCI(Transparent)";
        case 0x0A:
            return "InfiniLink";
        default:
            return "Other";
        }
    }
    if (class_code == 0x0C) {
        switch (subclass) {
        case 0x00:
            return "FireWire";
        case 0x01:
            return "ACCESSbus";
        case 0x02:
            return "SSA";
        case 0x03:
            return "USB";
        case 0x04:
            return "FibreChannel";
        case 0x05:
            return "SMBus";
        case 0x06:
            return "InfiniBand";
        case 0x07:
            return "IPMI";
        case 0x08:
            return "SERCOS";
        case 0x09:
            return "CANbus";
        default:
            return "Other";
        }
    }
    return "";
}

} // anonymous namespace

namespace arch {

size_t pci_scan_all() {
    g_device_count = 0;
    probe_bus(0);
    Logger::info("PCI: scanned, %d devices found", g_device_count);
    // Pre-map MSI-X tables so their page-table pages are established at boot
    // (before the test-isolation baseline) — a test-time MsixCap::create
    // reuses the cached KVA instead of allocating fresh PT pages per test
    // (ResourceTracker-clean, iommu probe-once pattern, issue #10).
    pci_msix_premap_all();
#ifdef CONFIG_DEBUG
    pci_dump_tree();
#endif
    return g_device_count;
}

void pci_dump_tree() {
    uint8_t current_bus = 0xFF;
    for (size_t i = 0; i < g_device_count; ++i) {
        const auto &d = g_devices[i];
        if (d.bdf.bus != current_bus) {
            current_bus = d.bdf.bus;
            Logger::raw_write("PCI Bus ");
            Logger::print_dec(d.bdf.bus);
            Logger::raw_write(":\n");
        }
        Logger::raw_write("  ");
        Logger::print_dec(d.bdf.bus);
        Logger::raw_write(":");
        Logger::print_dec(d.bdf.device);
        Logger::raw_write(".");
        Logger::print_dec(d.bdf.function);
        Logger::raw_write("  [");
        Logger::print_hex(d.vendor_id);
        Logger::raw_write(":");
        Logger::print_hex(d.device_id);
        Logger::raw_write("]  ");
        const char *cn = pci_class_name(d.class_code);
        Logger::raw_write(cn);
        Logger::raw_write(" (");
        Logger::print_hex(d.class_code);
        Logger::raw_write("/");
        Logger::print_hex(d.subclass);
        const char *sn = pci_subclass_name(d.class_code, d.subclass);
        if (sn[0]) {
            Logger::raw_write(" ");
            Logger::raw_write(sn);
        }
        Logger::raw_write(")\n");
    }
}

void pci_print_tree(char *buffer, size_t size) {
    if (size == 0)
        return;
    size_t pos = 0;
    uint8_t current_bus = 0xFF;
    for (size_t i = 0; i < g_device_count && pos + 80 < size; ++i) {
        const auto &d = g_devices[i];
        if (d.bdf.bus != current_bus) {
            current_bus = d.bdf.bus;
            char *p = buffer + pos;
            *p++ = 'P';
            *p++ = 'C';
            *p++ = 'I';
            *p++ = ' ';
            *p++ = 'B';
            *p++ = 'u';
            *p++ = 's';
            *p++ = ' ';
            pos = p - buffer;
            pos += fmt_dec(buffer + pos, size - pos, d.bdf.bus);
            if (pos + 3 >= size)
                break;
            buffer[pos++] = ':';
            buffer[pos++] = '\n';
        }
        // BDF
        pos += fmt_dec(buffer + pos, size - pos, d.bdf.bus);
        if (pos + 1 >= size)
            break;
        buffer[pos++] = ':';
        pos += fmt_dec(buffer + pos, size - pos, d.bdf.device);
        if (pos + 1 >= size)
            break;
        buffer[pos++] = '.';
        pos += fmt_dec(buffer + pos, size - pos, d.bdf.function);
        // vendor:device
        const char *fmt = "  [";
        while (*fmt && pos + 1 < size)
            buffer[pos++] = *fmt++;
        pos += fmt_hex(buffer + pos, size - pos, d.vendor_id, 4);
        if (pos + 1 >= size)
            break;
        buffer[pos++] = ':';
        pos += fmt_hex(buffer + pos, size - pos, d.device_id, 4);
        if (pos + 1 >= size)
            break;
        buffer[pos++] = ']';
        buffer[pos++] = ' ';
        // class/subclass name
        const char *cn = pci_class_name(d.class_code);
        while (*cn && pos + 1 < size)
            buffer[pos++] = *cn++;
        const char *fmt2 = " (";
        while (*fmt2 && pos + 1 < size)
            buffer[pos++] = *fmt2++;
        pos += fmt_hex(buffer + pos, size - pos, d.class_code, 2);
        if (pos + 1 >= size)
            break;
        buffer[pos++] = '/';
        pos += fmt_hex(buffer + pos, size - pos, d.subclass, 2);
        const char *sn = pci_subclass_name(d.class_code, d.subclass);
        if (sn[0]) {
            buffer[pos++] = ' ';
            while (*sn && pos + 1 < size)
                buffer[pos++] = *sn++;
        }
        if (pos + 1 >= size)
            break;
        buffer[pos++] = ')';
        buffer[pos++] = '\n';
    }
    buffer[pos] = '\0';
}

const PciDeviceInfo *pci_devices() {
    return g_devices;
}

size_t pci_device_count() {
    return g_device_count;
}

const PciDeviceInfo *pci_find_device(uint8_t class_code, uint8_t subclass) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].class_code == class_code &&
            g_devices[i].subclass == subclass) {
            return &g_devices[i];
        }
    }
    return nullptr;
}

void pci_parse_bars(PciDeviceInfo &info) {
    // Zero out all BARs first
    for (uint8_t i = 0; i < 6; ++i)
        info.bars[i] = {};
    info.bar_count = 0;

    for (uint8_t i = 0; i < 6; ++i) {
        uint32_t reg = PCI_BAR0 + i * 4;
        uint32_t raw = pci_config_readl(
            pci_make_addr(info.bdf, static_cast<uint8_t>(reg)));

        if (raw == 0)
            continue;

        PciBar &bar = info.bars[i];
        if (raw & 1) {
            // I/O BAR
            bar.type = PciBarType::IO;
            bar.address = static_cast<uint64_t>(raw & ~3);
            bar.prefetchable = false;
            // Size probe: write all 1s, read back, restore
            pci_config_writel(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)), 0xFFFFFFFF);
            uint32_t mask = pci_config_readl(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)));
            pci_config_writel(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)), raw);
            bar.size = static_cast<uint64_t>(~(mask & 0xFFFFFFFC)) + 1;
        } else {
            // Memory BAR
            bool is_64 = (raw & 6) == 4;
            bar.prefetchable = (raw & 8) != 0;
            bar.type = is_64 ? PciBarType::MEMORY_64 : PciBarType::MEMORY_32;

            // Size probe
            pci_config_writel(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)), 0xFFFFFFFF);
            uint32_t mask_low = pci_config_readl(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)));
            pci_config_writel(
                pci_make_addr(info.bdf, static_cast<uint8_t>(reg)), raw);

            if (is_64 && i < 5) {
                // Read high 32 bits for 64-bit BAR
                uint32_t addr_high = pci_config_readl(
                    pci_make_addr(info.bdf, static_cast<uint8_t>(reg + 4)));
                bar.address = (static_cast<uint64_t>(addr_high) << 32) |
                              (raw & 0xFFFFFFF0);
                uint64_t size_mask =
                    static_cast<uint64_t>(mask_low) & 0xFFFFFFF0;
                bar.size = (~size_mask) + 1;
                ++i; // skip next BAR register
            } else {
                bar.address = static_cast<uint64_t>(raw & 0xFFFFFFF0);
                bar.size = static_cast<uint64_t>(~(mask_low & 0xFFFFFFF0)) + 1;
            }
        }
        ++info.bar_count;
    }
}

PciDeviceInfo pci_read_device_info(PciBdf bdf) {
    PciDeviceInfo info = {};
    info.bdf = bdf;
    info.vendor_id = pci_read_vendor(bdf);
    info.device_id = pci_read_device(bdf);
    uint64_t addr_base = pci_make_addr(bdf, 0);
    info.revision = pci_config_readb(addr_base + PCI_REVISION);
    info.prog_if = pci_config_readb(addr_base + PCI_PROG_IF);
    info.subclass = pci_config_readb(addr_base + PCI_SUBCLASS);
    info.class_code = pci_config_readb(addr_base + PCI_CLASS);
    info.header_type = pci_config_readb(addr_base + PCI_HEADER_TYPE);
    pci_parse_bars(info);
    return info;
}

// --- MSI vector allocator ---
// Vectors available for MSI/MSI-X: 48-127, 129-255.
// (0-31 = CPU exceptions, 32-47 = PIC IRQs, 0x80 = syscall)
static bool g_vector_used[256] = {};
static bool g_vector_init = false;

static void init_vector_alloc() {
    if (g_vector_init)
        return;
    for (int i = 0; i < 32; ++i)
        g_vector_used[i] = true; // CPU exceptions
    for (int i = 32; i < 48; ++i)
        g_vector_used[i] = true; // PIC IRQs
    g_vector_used[0x80] = true;  // SYSCALL
    g_vector_used[64] = true;    // xAPIC scheduler timer (APIC_TIMER_VECTOR)
    g_vector_used[0xFF] = true;  // APIC spurious interrupt vector
    g_vector_init = true;
}

uint8_t pci_alloc_vector() {
    init_vector_alloc();
    for (uint16_t v = 48; v < 256; ++v) {
        if (v == 0x80)
            continue;
        if (!g_vector_used[v]) {
            g_vector_used[v] = true;
            return static_cast<uint8_t>(v);
        }
    }
    return 0;
}

void pci_free_vector(uint8_t vec) {
    if (vec < 48 || vec == 0x80)
        return;
    g_vector_used[vec] = false;
}

// --- Capability list walking ---

uint8_t pci_find_capability(PciBdf bdf, uint8_t cap_id) {
    // Phantom devices (no vendor) read back 0xFF/0xFFFF in config space,
    // which can fake a CAP_LIST bit and a non-null CAP_PTR — the walk below
    // would then loop forever (readb(0xFF+1) truncates to 0x00, reads 0xFF
    // again).  Fail closed on a non-existent device (CODING_STYLE §6:
    // fully bounded loops).
    if (bdf.bus >= arch::PCI_MAX_BUSES || bdf.device >= arch::PCI_MAX_DEVICES ||
        bdf.function >= arch::PCI_MAX_FUNCTIONS)
        return 0;
    if (!pci_device_exists(bdf))
        return 0;

    // Check if the device supports capabilities
    uint16_t status = pci_config_readw(pci_make_addr(bdf, PCI_STATUS));
    if (!(status & PCI_STATUS_CAP_LIST))
        return 0;

    uint8_t offset = pci_config_readb(pci_make_addr(bdf, PCI_CAP_PTR));
    // A capability list is a bounded chain (max 256 entries of 8-bit next
    // pointers, and the standard guarantees acyclic, eventually-null walk).
    // Still, guard against a hardware that mis-reports a cyclic pointer.
    for (unsigned step = 0; offset != 0 && step < 256; ++step) {
        uint8_t id = pci_config_readb(pci_make_addr(bdf, offset));
        if (id == cap_id)
            return offset;
        offset = pci_config_readb(pci_make_addr(bdf, offset + 1));
    }
    return 0;
}

// --- MSI enable ---

uint8_t pci_enable_msi(PciBdf bdf, uint8_t apic_id) {
    uint8_t cap = pci_find_capability(bdf, PCI_CAP_ID_MSI);
    if (cap == 0)
        return 0;

    uint16_t ctrl = pci_config_readw(pci_make_addr(bdf, cap + 2));
    bool is_64 = (ctrl & PCI_MSI_CTRL_64BIT) != 0;

    uint8_t vec = pci_alloc_vector();
    if (vec == 0)
        return 0;

    // Message Address Register
    uint32_t addr = PCI_MSI_ADDR_BASE | (static_cast<uint32_t>(apic_id) << 12);
    pci_config_writel(pci_make_addr(bdf, cap + 4), addr);

    // Message Data Register
    uint16_t data = PCI_MSI_DATA_FIXED | vec;
    uint8_t data_off =
        is_64 ? static_cast<uint8_t>(cap + 12) : static_cast<uint8_t>(cap + 8);
    pci_config_writel(pci_make_addr(bdf, data_off), data);

    // Upper Address (64-bit only)
    if (is_64) {
        pci_config_writel(pci_make_addr(bdf, cap + 8), 0);
    }

    // Enable MSI, set MME = 0 (single message)
    ctrl = (ctrl & ~PCI_MSI_CTRL_MME_MASK) | PCI_MSI_CTRL_ENABLE;
    pci_config_writel(pci_make_addr(bdf, cap + 2), ctrl);

    Logger::info("MSI: enabled on %d:%d.%d vector=%d", bdf.bus, bdf.device,
                 bdf.function, vec);
    return vec;
}

// --- MSI-X ---

/// @brief Cached MSI-X table info per device (probe-once, iommu pattern).
///        The table pages are mapped at boot by pci_msix_premap_all() so a
///        test-time create reuses the cached KVA — no per-test page-table
///        allocation, ResourceTracker-clean (issue #10).
namespace {
constexpr size_t kMsixCacheSize = arch::PCI_MAX_DEVICES_FOUND;
struct MsixCacheEntry {
    bool valid = false;
    arch::PciBdf bdf{};
    arch::PciMsixTableInfo tbl{};
};
MsixCacheEntry g_msix_cache[kMsixCacheSize];
} // namespace

/// @brief Returns the cached table info for @p bdf, or nullptr.
static const arch::PciMsixTableInfo *msix_cache_lookup(const arch::PciBdf &bdf) {
    for (size_t i = 0; i < kMsixCacheSize; ++i) {
        if (g_msix_cache[i].valid && g_msix_cache[i].bdf == bdf)
            return &g_msix_cache[i].tbl;
    }
    return nullptr;
}

/// @brief Stores @p tbl for @p bdf in the cache (no-op when full).
static void msix_cache_store(const arch::PciBdf &bdf,
                             const arch::PciMsixTableInfo &tbl) {
    // Dedupe by BDF: a repeated pci_scan_all() (test re-probe) must not grow
    // duplicate cache entries for the same device.
    for (size_t i = 0; i < kMsixCacheSize; ++i) {
        if (g_msix_cache[i].valid && g_msix_cache[i].bdf == bdf)
            return;
    }
    for (size_t i = 0; i < kMsixCacheSize; ++i) {
        if (!g_msix_cache[i].valid) {
            g_msix_cache[i].valid = true;
            g_msix_cache[i].bdf = bdf;
            g_msix_cache[i].tbl = tbl;
            return;
        }
    }
}

void pci_msix_premap_all() {
    for (size_t i = 0; i < g_device_count; ++i) {
        arch::PciMsixTableInfo tbl{};
        if (arch::pci_msix_table_info(g_devices[i].bdf, tbl))
            msix_cache_store(g_devices[i].bdf, tbl);
    }
}

bool pci_msix_table_info(PciBdf bdf, PciMsixTableInfo &out) {
    out = PciMsixTableInfo{};
    // Reuse the boot-time cached mapping when present (probe-once).
    const arch::PciMsixTableInfo *cached = msix_cache_lookup(bdf);
    if (cached) {
        out = *cached;
        return true;
    }

    uint8_t cap = pci_find_capability(bdf, PCI_CAP_ID_MSIX);
    if (cap == 0)
        return false;

    // Message Control: TSIZE (bits 10:0) = number of entries - 1.
    uint16_t ctrl = pci_config_readw(pci_make_addr(bdf, cap + 2));
    uint16_t entry_count =
        static_cast<uint16_t>((ctrl & PCI_MSIX_CTRL_TSIZE_MASK) + 1U);

    // Table BIR/Offset (cap + 4): bits 2:0 = BAR index, rest = offset.
    uint32_t tbl_reg = pci_config_readl(pci_make_addr(bdf, cap + 4));
    uint8_t bir = static_cast<uint8_t>(tbl_reg & 0x7U);
    uint64_t tbl_offset = static_cast<uint64_t>(tbl_reg & ~0x7U);

    // BDF bounds + BAR validation (issue #4 gotcha: validate every indexing
    // site).  The table must live in a memory BAR that fits its size.
    if (bdf.bus >= arch::PCI_MAX_BUSES || bdf.device >= arch::PCI_MAX_DEVICES ||
        bdf.function >= arch::PCI_MAX_FUNCTIONS || bir >= 6)
        return false;

    PciDeviceInfo info = pci_read_device_info(bdf);
    if (info.bars[bir].address == 0 ||
        info.bars[bir].type == PciBarType::IO)
        return false;

    uint64_t bar_phys = info.bars[bir].address;
    uint64_t table_phys = bar_phys + tbl_offset;
    uint64_t table_end =
        table_phys + static_cast<uint64_t>(entry_count) * 16U;

    // The whole table must fit inside the BAR.
    if (tbl_offset >= info.bars[bir].size ||
        table_end > bar_phys + info.bars[bir].size)
        return false;

    // Map every page the table spans into the kernel direct map (virtio_pci /
    // iommu precedent) so entry access is a bounded KVA MMIO read/write, never
    // a raw physical write (CODING_STYLE §4 — no primitive reinterpret_cast to
    // a physical address).
    uint64_t map_start = msix_page_align_down(table_phys);
    uint64_t map_end = msix_page_align_up(table_end);
    for (uint64_t page = map_start; page < map_end; page += arch::PAGE_SIZE) {
        VMM::map_page(kernel::HHDM_OFFSET + page, page, /*user=*/false);
    }

    out.bir = bir;
    out.entry_count = entry_count;
    out.table_phys = table_phys;
    out.table_kva = kernel::HHDM_OFFSET + table_phys;
    out.bar_size = info.bars[bir].size;
    return true;
}

bool pci_program_msix_entry(PciBdf bdf, const PciMsixTableInfo &tbl,
                            uint16_t entry, uint8_t vector, uint8_t apic_id) {
    if (bdf.bus >= arch::PCI_MAX_BUSES || bdf.device >= arch::PCI_MAX_DEVICES ||
        bdf.function >= arch::PCI_MAX_FUNCTIONS)
        return false;
    if (tbl.table_kva == 0 || entry >= tbl.entry_count)
        return false;

    volatile arch::MsixTableEntry *e =
        reinterpret_cast<volatile arch::MsixTableEntry *>(tbl.table_kva +
                                                          static_cast<uint64_t>(entry) * 16U);
    e->msg_addr_low = PCI_MSI_ADDR_BASE | (static_cast<uint32_t>(apic_id) << 12);
    e->msg_addr_high = 0;
    e->msg_data = static_cast<uint32_t>(PCI_MSI_DATA_FIXED | vector);
    // Create-time fail-closed state: the entry stays masked until arming.
    e->vector_control = MSIX_ENTRY_MASK_BIT;
    return true;
}

bool pci_msix_entry_set_masked(const PciMsixTableInfo &tbl, uint16_t entry,
                               bool masked) {
    if (tbl.table_kva == 0 || entry >= tbl.entry_count)
        return false;
    volatile arch::MsixTableEntry *e =
        reinterpret_cast<volatile arch::MsixTableEntry *>(tbl.table_kva +
                                                          static_cast<uint64_t>(entry) * 16U);
    if (masked) {
        e->vector_control |= MSIX_ENTRY_MASK_BIT;
    } else {
        e->vector_control &= ~MSIX_ENTRY_MASK_BIT;
    }
    return true;
}

bool pci_msix_entry_masked(const PciMsixTableInfo &tbl, uint16_t entry) {
    if (tbl.table_kva == 0 || entry >= tbl.entry_count)
        return false;
    const volatile arch::MsixTableEntry *e =
        reinterpret_cast<const volatile arch::MsixTableEntry *>(
            tbl.table_kva + static_cast<uint64_t>(entry) * 16U);
    return (e->vector_control & MSIX_ENTRY_MASK_BIT) != 0;
}

uint8_t pci_enable_msix(PciBdf bdf, uint16_t entry, uint8_t apic_id) {
    uint8_t cap = pci_find_capability(bdf, PCI_CAP_ID_MSIX);
    if (cap == 0)
        return 0;

    PciMsixTableInfo tbl{};
    if (!pci_msix_table_info(bdf, tbl))
        return 0;

    uint8_t vec = pci_alloc_vector();
    if (vec == 0)
        return 0;

    if (!pci_program_msix_entry(bdf, tbl, entry, vec, apic_id)) {
        pci_free_vector(vec);
        return 0;
    }

    // Enable MSI-X and unmask function (the table entry itself stays masked
    // until a driver arms it).
    uint16_t ctrl = pci_config_readw(pci_make_addr(bdf, cap + 2));
    ctrl |= PCI_MSIX_CTRL_ENABLE;
    ctrl &= ~PCI_MSIX_CTRL_FUNCMASK;
    pci_config_writel(pci_make_addr(bdf, cap + 2), ctrl);
    // Legacy contract: pci_enable_msix hands back a LIVE vector to the caller
    // (raw kernel path, no arm step) — unmask the entry now so delivery works.
    pci_msix_entry_set_masked(tbl, entry, false);

    Logger::info("MSI-X: enabled on %d:%d.%d entry=%d vector=%d", bdf.bus,
                 bdf.device, bdf.function, entry, vec);
    return vec;
}

} // namespace arch
