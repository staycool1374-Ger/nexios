#include <kernel/task/boot_sdf.hpp>

#if CONFIG_DRIVER_PD_SERIAL || CONFIG_DRIVER_PD_KBD ||                        \
    CONFIG_DRIVER_PD_RTC || CONFIG_DRIVER_PD_FB ||                            \
    CONFIG_DRIVER_PD_BLOCK || CONFIG_DRIVER_PD_NET ||                         \
    CONFIG_VFS_SRV || CONFIG_BLOCK_SRV || CONFIG_TERM_SRV ||                  \
    CONFIG_LOG_SRV || CONFIG_CAP_BROKER || CONFIG_WDOG_MON ||                 \
    CONFIG_PROCFS_SRV || CONFIG_SERIAL_TX_SHM

#include <kernel/task/task.hpp>
#include <kernel/arch/hal/device_regions.hpp>
#include <kernel/cap/cap.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/cap/irq.hpp>
#include <kernel/cap/iommu.hpp>
#include <kernel/cap/frame.hpp>
#include <kernel/arch/pci.hpp>
#include <kernel/arch/virtio.hpp>
#include <kernel/iommu/iommu.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/ipc/shm_ring.hpp>
#include <services/terminal/framebuffer.hpp>

namespace kernel {
namespace sdf {
namespace {

// Captured block topology (issue #47 S2): set once by
// capture_block_topology() before reboot_from_table()'s IrqGuard.
CapSpec g_block_mmio[kMaxCapsPerRole] = {};
uint64_t g_block_mmio_count = 0;
bool g_block_dma = false;

// TX-ring backing store (issue #51 step-c-prime): per-role ring phys +
// page count for the SDF-composed SHM channel. Written by the SHM mint
// path at boot compose + restart rebind (fresh frames every time — never
// inherited), read by the kernel TX producer. Roles ≥ 6 never carry SHM.
uint64_t g_shm_tx_phys[6] = {};
uint64_t g_shm_tx_pages[6] = {};
// Composed DMA_REGION window records (issue #47 step-b1): (phys, pages)
// per role+index, published only after the whole row composes; cleared
// on any (re)compose start of that role.
uint64_t g_role_frame_phys[kPdRoleCount][kMaxCapsPerRole] = {};
uint64_t g_role_frame_pages[kPdRoleCount][kMaxCapsPerRole] = {};
// Last SHM mint record: mint_one stages (phys, pages) here; sdf_compose
// copies them into the role cells only after the whole row composes
// (all-or-nothing — a failed row leaves the role cells cleared, and the
// producer fail-closes to the raw path).
uint64_t g_last_shm_phys = 0;
uint64_t g_last_shm_pages = 0;
// Last DMA_REGION mint record (issue #47 step-b1): same staging contract
// as SHM, but no ring header is initialised (raw sector/queue window).
// sdf_compose snapshots these per index right after each successful
// mint_one, so rows with one DMA_REGION window publish it on success.
uint64_t g_last_dma_phys = 0;
uint64_t g_last_dma_pages = 0;

/// @brief Mint one CapSpec into the task CSpace (all-or-nothing caller).
/// @return slot index or -1. Created caps are disposed by the caller on
///         any later failure (rollback) or owned by the CSpace on success.
int mint_one(TaskControlBlock &task, const CapSpec &spec, uint32_t *gen_out) {
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
        if (spec.io_port != 0) {
            // IO ports are not memory-mapped (grant ledger only), so
            // the exact range is kept: widening to whole pages would
            // widen the grant bound (least-privilege regression).
            auto *cap = cap::MmioCap::create(spec.addr, spec.size,
                                             bar_type);
            if (cap == nullptr) {
                return -1;
            }
            obj = cap;
            type = cap::CapType::Mmio;
            break;
        }
        // Window may start mid-page (issue #47 step-b1: virtio slots
        // stride 0x200); the map primitive needs whole pages, so the
        // cap covers [align_down(addr), align_up(addr+size)) and the
        // PD applies the intra-page offset (addr & 0xFFF) itself.
        const uint64_t mask = static_cast<uint64_t>(arch::PAGE_SIZE) - 1;
        const uint64_t base = spec.addr & ~mask;
        if (spec.size == 0 || spec.addr + spec.size < spec.addr) {
            return -1;
        }
        const uint64_t end = (spec.addr + spec.size + mask) & ~mask;
        if (end <= base) {
            return -1;
        }
        auto *cap = cap::MmioCap::create(base, end - base, bar_type);
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
        // Non-owning pin over foreign-lifetime RAM (issue #51 console PD
        // scanout grant): the backing outlives every PD epoch, so the cap
        // must never free the frames (FrameCap::create_pinned) — PD death
        // still revokes the PD's mappings via the dispose closure.
        // Fail-closed unless the whole span is user pages (a MmioCap is
        // the only correct wrapper for true device MMIO; wrapping kernel
        // RAM would hand the PD supervisor memory).
        const size_t pages =
            (spec.size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
        if (spec.addr == 0 || pages == 0) {
            return -1;
        }
        for (size_t i = 0; i < pages; ++i) {
            if (!PMM::is_user_page(spec.addr + i * arch::PAGE_SIZE)) {
                return -1;
            }
        }
        auto *cap = cap::FrameCap::create_pinned(spec.addr, pages);
        if (cap == nullptr) {
            return -1;
        }
        obj = cap;
        type = cap::CapType::Frame;
        break;
    }
    case CapKind::SHM: {
        // TX-ring channel (issue #51 step-c-prime): allocate fresh user
        // frames, pin them in a FrameCap, and install it in the PD CSpace.
        // spec.size carries the total byte size; the page count rounds up
        // (header page + data pages, minimum 2). The header is initialised
        // here via the HHDM alias while the pages are still pristine —
        // the PD validates it on MAP and never trusts a torn header.
        // FrameCap::dispose frees the frames (is_user), so rollback and
        // PD-death teardown reclaim them; the kernel producer holds no
        // mapping (HHDM alias only) and re-resolves phys per epoch.
        if (spec.size == 0) {
            return -1;
        }
        const size_t pages =
            (spec.size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
        if (pages < 2) {
            return -1;
        }
        const uint64_t phys = PMM::alloc_user_contiguous(pages);
        if (phys == 0) {
            return -1;
        }
        auto *cap = cap::FrameCap::create(phys, pages, true);
        if (cap == nullptr) {
            for (size_t i = 0; i < pages; ++i) {
                PMM::free_page(phys + i * arch::PAGE_SIZE);
            }
            return -1;
        }
        auto *hdr = reinterpret_cast<shm::SharedRingHeader *>(
            arch::HHDM_OFFSET + phys);
        shm::shm_ring_init(hdr, pages, kSerialTxRingElemSize);
        if (!shm::shm_ring_valid(hdr)) {
            cap->release();
            return -1;
        }
        g_last_shm_phys = phys;
        g_last_shm_pages = pages;
        obj = cap;
        type = cap::CapType::Frame;
        break;
    }
    case CapKind::DMA_REGION: {
        // Shared data window (issue #47 step-b1): fresh user frames
        // pinned in a FrameCap, installed in the PD CSpace; the kernel
        // side uses the HHDM alias. Raw window — no ring header.
        if (spec.size == 0) {
            return -1;
        }
        const size_t pages =
            (spec.size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
        if (pages == 0) {
            return -1;
        }
        const uint64_t phys = PMM::alloc_user_contiguous(pages);
        if (phys == 0) {
            return -1;
        }
        auto *cap = cap::FrameCap::create(phys, pages, true);
        if (cap == nullptr) {
            for (size_t i = 0; i < pages; ++i) {
                PMM::free_page(phys + i * arch::PAGE_SIZE);
            }
            return -1;
        }
        g_last_dma_phys = phys;
        g_last_dma_pages = pages;
        obj = cap;
        type = cap::CapType::Frame;
        break;
    }
    case CapKind::MSIX:
        // Reserved row (MSI-X entry claim): wired when the first PD needs
        // it; never silently mapped to another kind.
        return -1;
    }
    const cap::CapRights rights =
        cap::CapRights::READ | cap::CapRights::WRITE;
    const int slot = space->install(obj, type, rights, gen_out);
    // install() acquired the slot's own reference: drop the creator ref
    // so task teardown (CNode::dispose -> slot release) is the last
    // release and runs dispose() deterministically.  Without this the
    // composed cap survives its task with refcount 1 — FrameCaps pin
    // their user frames forever (issue #51 QE finding: +4 PMM/+2 Cap/
    // +2 MemPool in serial_tx_sdf_compose_shm ON).  All failure legs of
    // install() leave the creator ref held, so the release is
    // unconditional; matches every other install() call site.
    obj->release();
    if (slot < 0) {
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

bool sdf_shm_tx_phys(PdRole role, uint64_t &phys_out, uint64_t &pages_out) {
    const uint64_t role_idx = static_cast<uint64_t>(role);
    if (role_idx >= 6 || g_shm_tx_pages[role_idx] == 0) {
        return false;
    }
    phys_out = g_shm_tx_phys[role_idx];
    pages_out = g_shm_tx_pages[role_idx];
    return phys_out != 0;
}

bool sdf_role_frame(PdRole role, uint64_t idx, uint64_t &phys_out,
                    uint64_t &pages_out) {
    const uint64_t role_idx = static_cast<uint64_t>(role);
    if (role_idx >= kPdRoleCount || idx >= kMaxCapsPerRole) {
        return false;
    }
    if (g_role_frame_pages[role_idx][idx] == 0) {
        return false;
    }
    phys_out = g_role_frame_phys[role_idx][idx];
    pages_out = g_role_frame_pages[role_idx][idx];
    return phys_out != 0;
}

uint64_t sdf_row(PdRole role, CapSpec *out) {
    if (out == nullptr) {
        return 0;
    }
    switch (role) {
    case PdRole::SERIAL: {
        // Owned PD (issue #51 step-c): TX+RX via polled UART. The kernel
        // keeps only the panic raw_write fallback (Logger::raw_write
        // bypasses Terminal entirely, so panic never reaches the PD path).
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
#if CONFIG_SERIAL_TX_SHM
        // Step-c-prime: SHM TX-ring channel endpoint (issue #51 option A).
        // The SHM spec is always last (index 1); pd_bind conveys the UART
        // cap (index 0) while pd_bind_shm conveys the ring FrameCap
        // (index 1) via a dedicated MAP_SHM round.
        out[n++] = CapSpec{CapKind::SHM, 0,
                           kSerialTxRingPages * arch::PAGE_SIZE, 0, 0};
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
    case PdRole::FB: {
        // Console PD row (issue #51): #48 owns the device cap, #51 the
        // emulation policy. Fixed indices so the client never guesses:
        //   0 = SHM text ring (kernel → console PD, element {len, char[7]})
        //   1 = DMA_REGION status page (PD render counters, raw window)
        //   2..N = scanout, split into slot-sized chunks (a 1024x768x32
        //          scanout is 3 MiB; a user MMIO/frame slot holds 2 MiB).
        //          MMIO chunks for true device memory (x86 VGA VRAM),
        //          FRAME pins for RAM-backed scanouts (virtio-gpu/ramfb —
        //          SYS_MMIO_MAP refuses RAM, SYS_FRAME_MAP refuses
        //          kernel pages, so the kind follows the backing).
        // Degraded compose (no framebuffer backend) yields the two
        // channel caps only — the client then refuses to bind (a blind
        // console sink is never bound); the kernel keeps its own path.
        uint64_t n = 0;
        out[n++] = CapSpec{CapKind::SHM, 0,
                           kConsoleRingPages * arch::PAGE_SIZE, 0, 0};
        out[n++] = CapSpec{CapKind::DMA_REGION, 0,
                           kConsoleStatusPages * arch::PAGE_SIZE, 0, 0};
        const uint64_t phys = service::Framebuffer::scanout_phys();
        const uint64_t bytes = service::Framebuffer::scanout_bytes();
        if (phys != 0 && bytes != 0) {
            const bool ram = service::Framebuffer::scanout_ram_backed();
            const uint64_t chunk =
                ram ? kConsoleFrameChunk : kConsoleMmioChunk;
            uint64_t chunks = (bytes + chunk - 1) / chunk;
            if (chunks > kConsoleMaxScanoutChunks) {
                // More chunks than the row can carry would hand the PD a
                // truncated scanout: fail closed (no device cap) instead
                // of binding a console that paints half a frame.
                return n;
            }
            const CapKind kind = ram ? CapKind::FRAME : CapKind::MMIO;
            for (uint64_t i = 0; i < chunks && n < kMaxCapsPerRole; ++i) {
                const uint64_t off = i * chunk;
                uint64_t size = bytes - off;
                if (size > chunk) {
                    size = chunk;
                }
                out[n++] = CapSpec{kind, phys + off, size, 0, 0};
            }
        }
        return n;
    }
    case PdRole::BLOCK:
    case PdRole::NET:
        // Rows dispatch on live PCI/virtio topology at compose time
        // (planner S5/S6); the shape (MSI-X/INTID + DMA + SHM) is declared
        // in docs/specs/driver-pds.md. No static row yet.
        return 0;
    case PdRole::VFS_SRV:
        // No ambient authority: the vfs-srv row IS the empty set (planner
        // SDF rules). Compose succeeds with zero caps; the PD talks to
        // clients over its endpoint only.
        return 0;
    case PdRole::BLOCK_SRV: {
        // Block-storage server row (issue #47 step-b1): one static
        // arch-switched shape + one shared data window. No IRQ cap in
        // b1 (polled completion; IRQ delegation is step-b2).
        // x86_64: ATA PIO command blocks (primary 0x1F0 + secondary
        // 0x170, 8 ports each, IO-type) + 4-page shared window.
        // aarch64/riscv64: first captured virtio transport window
        // (page-aligned by capture_block_topology) + 4-page window.
        // Degraded (no controller captured off-x86) composes zero caps
        // — allowed explicitly, logged by the caller.
        uint64_t n = 0;
#if defined(CONFIG_ARCH_X86_64)
        if (n < kMaxCapsPerRole) {
            out[n++] =
                CapSpec{CapKind::MMIO, 0x1F0, 8, 0, 1};
        }
        if (n < kMaxCapsPerRole) {
            out[n++] =
                CapSpec{CapKind::MMIO, 0x170, 8, 0, 1};
        }
#else
        if (g_block_mmio_count > 0 && n < kMaxCapsPerRole) {
            out[n++] = g_block_mmio[0];
        }
#endif
        // Shared sector/queue window: 4 pages (see block_srv.hpp
        // kBlockRegionPages; the PD mirrors the constants).
        if (n < kMaxCapsPerRole) {
            out[n++] = CapSpec{CapKind::DMA_REGION, 0,
                               4 * arch::PAGE_SIZE, 0, 0};
        }
#if !defined(CONFIG_ARCH_X86_64)
        if (n == 1) {
            // Only the window, no transport: degraded, compose capless.
            return 0;
        }
#endif
        return n;
    }
    case PdRole::TERM_SRV:
        // Emulation policy only (issue #51): serial TX/RX + fb present are
        // borrowed kernel mechanisms, never owned caps. The row is the
        // empty set by structural rule (see sdf_policy_only).
        return 0;
    case PdRole::LOG_SRV:
        // Log sink policy only (issue #51): Klog/Dmesg sink + drop/meter.
        // No device caps ever; same structural rule as TERM_SRV.
        return 0;
    case PdRole::CAP_BROKER:
        // Delegation policy only (issue #240): broker-allows /
        // kernel-enforces over a cached per-CNode word. No device caps
        // ever; the row is the empty set by structural rule.
        return 0;
    case PdRole::WDOG_MON:
        // Supervision policy only (issue #390): monitor-composes-policy
        // over the kernel tick-scan + death pulses + watchdogd base.
        // No device caps ever; the row is the empty set by rule.
        return 0;
    case PdRole::PROCFS_SRV:
        // Render policy only (issue #54): renders stay in-kernel
        // pre-flip (vfsd PROCFS_RENDER=7 probe retains sole ownership).
        // No device caps ever; the row is the empty set by rule.
        return 0;
    }
    return 0;
}

bool sdf_zero_caps_allowed(PdRole role) {
    // Explicit allowlist: VFS_SRV + TERM_SRV + LOG_SRV + CAP_BROKER +
    // WDOG_MON + PROCFS_SRV (empty sets by design) and degraded
    // BLOCK_SRV (no controller/IOMMU captured) may start capless.
    // Every other zero-cap row is a compose failure.
    return role == PdRole::VFS_SRV || role == PdRole::TERM_SRV ||
           role == PdRole::LOG_SRV || role == PdRole::BLOCK_SRV ||
           role == PdRole::CAP_BROKER || role == PdRole::WDOG_MON ||
           role == PdRole::PROCFS_SRV;
}

bool sdf_policy_only(PdRole role) {
    // Structural no-double-ownership rule (issues #47/#51, #240, #390,
    // #54): policy-only roles must never hold device caps. The #48 SDF
    // boundary test consumes this predicate directly.
    return role == PdRole::VFS_SRV || role == PdRole::TERM_SRV ||
           role == PdRole::LOG_SRV || role == PdRole::CAP_BROKER ||
           role == PdRole::WDOG_MON || role == PdRole::PROCFS_SRV;
}

bool capture_block_topology() {
    g_block_mmio_count = 0;
    g_block_dma = false;
    for (uint64_t i = 0; i < kMaxCapsPerRole; ++i) {
        g_block_mmio[i] = CapSpec{CapKind::MMIO, 0, 0, 0, 0};
    }
    uint64_t n = 0;
    auto push_mmio = [&](uint64_t phys, uint64_t size) {
        if (n >= kMaxCapsPerRole || phys == 0 || size == 0) {
            return;
        }
        // Store the EXACT window (issue #47 step-b1): alignment to
        // whole pages happens at mint time; the row keeps the true
        // base so the PD can apply the intra-page offset.
        g_block_mmio[n++] =
            CapSpec{CapKind::MMIO, phys, size, 0, 0};
    };
#if defined(CONFIG_ARCH_X86_64)
    // AHCI HBA: class 0x01/subclass 0x06, BAR5 = ABAR.
    if (const arch::PciDeviceInfo *ahci =
            arch::pci_find_device(0x01, 0x06);
        ahci != nullptr) {
        arch::PciDeviceInfo info = *ahci;
        arch::pci_parse_bars(info);
        if (info.bar_count > 5) {
            push_mmio(info.bars[5].address, info.bars[5].size);
        }
    }
#endif
    // Virtio-blk transport window (any arch with a finder).
    {
        arch::VirtioTransport transport{};
        if (arch::virtio_find_device(arch::VIRTIO_DEVICE_BLOCK,
                                     transport) ||
            arch::virtio_find_device(arch::VIRTIO_DEVICE_BLOCK_LEGACY,
                                     transport)) {
            push_mmio(transport.common_cfg.phys_addr,
                      transport.common_cfg.length);
        }
    }
    g_block_mmio_count = n;
    g_block_dma = kernel::iommu::IoMmuManager::probe_hardware();
    return true;
}

// Composed cap slots per role (issue #51 step-c): sdf_compose persists the
// minted (slot, generation) pairs so kernel→PD BIND can convey the PD's own
// cap handles back to it (the PD cannot scan its CSpace; slots are otherwise
// undiscoverable). Written at boot compose + restart rebind, read by the
// serial TX client. Boot-time only content; test snapshot/restore rewinds
// MemPool but never recomposes PDs, so no stale-slot path exists.
int g_role_slots[kPdRoleCount][kMaxCapsPerRole] = {};
uint32_t g_role_gens[kPdRoleCount][kMaxCapsPerRole] = {};
uint64_t g_role_slot_count[kPdRoleCount] = {};

bool sdf_role_cap(PdRole role, uint64_t idx, int &slot, uint32_t &gen) {
    const uint64_t role_idx = static_cast<uint64_t>(role);
    if (role_idx >= kPdRoleCount || idx >= kMaxCapsPerRole) {
        return false;
    }
    if (idx >= g_role_slot_count[role_idx]) {
        return false;
    }
    slot = g_role_slots[role_idx][idx];
    if (slot < 0) {
        return false;
    }
    gen = g_role_gens[role_idx][idx];
    return true;
}

bool sdf_compose(TaskControlBlock &task, PdRole role) {
    CapSpec specs[kMaxCapsPerRole] = {};
    const uint64_t count = sdf_row(role, specs);
    if (count > kMaxCapsPerRole) {
        return false;
    }
    if (sdf_policy_only(role) && count != 0) {
        // Structural no-double-ownership (issues #47/#51): a policy-only
        // row must never carry device caps. Fail closed, never compose.
        return false;
    }
    if (count == 0) {
        // Zero-cap rows compose empty only on the explicit allowlist
        // (planner S2); anything else is a fail-closed compose failure.
        return sdf_zero_caps_allowed(role);
    }
    int slots[kMaxCapsPerRole] = {-1, -1, -1, -1, -1, -1};
    uint32_t gens[kMaxCapsPerRole] = {};
    // Fail-closed TX-ring record (issue #51 step-c-prime): a fresh compose
    // clears the role cells first; they are rewritten only after the whole
    // row composes. A failed (re)compose therefore leaves no stale ring
    // for the producer to push into.
    const uint64_t role_idx0 = static_cast<uint64_t>(role);
    if (role_idx0 < 6) {
        g_shm_tx_phys[role_idx0] = 0;
        g_shm_tx_pages[role_idx0] = 0;
    }
    if (role_idx0 < kPdRoleCount) {
        for (uint64_t i = 0; i < kMaxCapsPerRole; ++i) {
            g_role_frame_phys[role_idx0][i] = 0;
            g_role_frame_pages[role_idx0][i] = 0;
        }
    }
    g_last_shm_phys = 0;
    g_last_shm_pages = 0;
    g_last_dma_phys = 0;
    g_last_dma_pages = 0;
    uint64_t dma_phys[kMaxCapsPerRole] = {};
    uint64_t dma_pages[kMaxCapsPerRole] = {};
    for (uint64_t i = 0; i < count; ++i) {
        const int slot = mint_one(task, specs[i], &gens[i]);
        if (slot < 0) {
            rollback(task, slots, i);
            return false;
        }
        slots[i] = slot;
        if (specs[i].kind == CapKind::DMA_REGION) {
            dma_phys[i] = g_last_dma_phys;
            dma_pages[i] = g_last_dma_pages;
        }
    }
    // Persist the composed (slot, gen) pairs for kernel→PD BIND handle
    // conveyance (issue #51 step-c, extended to all roles by #47
    // step-b1: BLOCK_SRV at index 7 needs persistence for its CAP_GET
    // rounds). Serial TX only needs roles 0..5, but persistence is
    // uniform now — no per-role exception.
    const uint64_t role_idx = static_cast<uint64_t>(role);
    if (role_idx < kPdRoleCount) {
        for (uint64_t i = 0; i < count; ++i) {
            g_role_slots[role_idx][i] = slots[i];
            g_role_gens[role_idx][i] = gens[i];
            g_role_frame_phys[role_idx][i] = dma_phys[i];
            g_role_frame_pages[role_idx][i] = dma_pages[i];
        }
        g_role_slot_count[role_idx] = count;
        // Publish the TX-ring backing store (issue #51 step-c-prime):
        // exactly one SHM spec per row, recorded only on full success.
        // SIL3 audit #47 step-b1: g_shm_tx_* hold 6 entries (roles ≥ 6
        // never carry SHM) while slot/frame tables span kPdRoleCount —
        // keep the publish inside the 6-entry bound.
        if (role_idx < 6) {
            for (uint64_t i = 0; i < count; ++i) {
                if (specs[i].kind == CapKind::SHM) {
                    g_shm_tx_phys[role_idx] = g_last_shm_phys;
                    g_shm_tx_pages[role_idx] = g_last_shm_pages;
                    break;
                }
            }
        }
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
    case PdRole::VFS_SRV:
        return CONFIG_VFS_SRV != 0;
    case PdRole::BLOCK_SRV:
        return CONFIG_BLOCK_SRV != 0;
    case PdRole::TERM_SRV:
    case PdRole::LOG_SRV:
    case PdRole::CAP_BROKER:
    case PdRole::WDOG_MON:
    case PdRole::PROCFS_SRV:
        // Policy-only PDs never own devices (structural rule).
        return false;
    }
    return false;
}

} // namespace sdf
} // namespace kernel

#endif // any CONFIG_DRIVER_PD_* / CONFIG_VFS_SRV / CONFIG_BLOCK_SRV / CONFIG_TERM_SRV / CONFIG_LOG_SRV / CONFIG_CAP_BROKER / CONFIG_WDOG_MON / CONFIG_PROCFS_SRV
