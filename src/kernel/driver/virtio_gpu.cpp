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

/// @file virtio_gpu.cpp
/// @brief virtio-gpu control-queue driver over legacy-v1 virtio-MMIO
/// (issue #394 Slice A).  Boot-time, single-hart, no ISR: one in-flight
/// command, bounded polls, static storage only.

#include <kernel/driver/virtio_gpu.hpp>

#if !defined(CONFIG_ARCH_X86_64)

#include <kernel/arch/virtio.hpp>
#include <kernel/driver/fb_backing.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/vmm_errors.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <lib/atomic.hpp>
#include <logger.hpp>
#include <string.hpp>

namespace kernel::gpu {

namespace {

// --- Static driver state (BSS only, no PMM/MemPool) ---

arch::VirtioTransport g_transport{};
alignas(4096) uint8_t g_queue_chunk[8192] = {};
// Page-aligned staging: VMM::virt_to_phys_err rejects sub-page addresses,
// so commands/responses live here (copied in), never on the stack or in
// 16 B-aligned BSS.  Sizes cover the largest command (ATTACH_BACKING with
// FB_MAX_PAGES entries: 32 + 1000*16 = 16032) and response (display info).
alignas(4096) uint8_t g_cmd_buf[16384] = {};
alignas(4096) uint8_t g_resp_buf[4096] = {};
sync::SpinLock g_gpu_lock{};
arch::VirtqDesc *g_desc = nullptr;
arch::VirtqAvail *g_avail = nullptr;
arch::VirtqUsed *g_used = nullptr;
uint32_t g_width = 0;
uint32_t g_height = 0;
uint16_t g_avail_idx = 0;
bool g_ready = false;
bool g_attempted = false;

/// @brief Physical address of a driver-owned VA (fail-closed 0).
uint64_t phys_of(const void *p) {
    uint64_t phys = 0;
    if (!p)
        return 0;
    if (VMM::virt_to_phys_err(reinterpret_cast<uint64_t>(p), phys) !=
        errors::VMM_ERR_OK) {
        return 0;
    }
    return phys;
}

/// @brief Submit the command already staged in g_cmd_buf (see submit()).
/// @return true on transport success with the expected response type.
/// The response (if any) lands in g_resp_buf — read it via
/// virtio_gpu_resp() before the next submit().
bool submit_staged(uint32_t req_len, uint32_t resp_len,
                   uint32_t expected_type) {
    if (req_len < sizeof(GpuCtrlHdr) || req_len > sizeof(g_cmd_buf) ||
        resp_len < 4 || resp_len > sizeof(g_resp_buf)) {
        return false;
    }
    uint64_t req_phys = phys_of(g_cmd_buf);
    uint64_t resp_phys = phys_of(g_resp_buf);
    if (!req_phys || !resp_phys) {
        return false;
    }
    memset(g_resp_buf, 0, sizeof(g_resp_buf));

    // Single flight on the fixed desc[0]/desc[1] pair (polled completion —
    // every command finishes before the next starts, so reuse is safe).
    g_desc[0].addr = req_phys;
    g_desc[0].len = req_len;
    g_desc[0].flags = arch::VIRTIO_DESC_F_NEXT;
    g_desc[0].next = 1;
    g_desc[1].addr = resp_phys;
    g_desc[1].len = resp_len;
    g_desc[1].flags = arch::VIRTIO_DESC_F_WRITE;
    g_desc[1].next = 0;

    // Snapshot the used index BEFORE the avail push + notify (a completion
    // observed after notify races the push — blk H-3 pattern).
    uint16_t used_before = g_used->idx;
    g_avail->ring[g_avail_idx % GPU_QUEUE_SIZE] = 0;
    kernel::atomic_fence();
    g_avail->idx = static_cast<uint16_t>(g_avail_idx + 1);
    kernel::atomic_fence();
    arch::virtio_notify(g_transport, 0);
    ++g_avail_idx;

    int timeout = GPU_CMD_TIMEOUT_ITERS;
    while (g_used->idx == used_before && --timeout > 0) {
        kernel::atomic_fence();
        arch::pause();
    }
    if (timeout <= 0) {
        Logger::error("virtio-gpu: command timeout");
        return false;
    }
    uint16_t used_now = g_used->idx;
    // Unsigned wrap: used_now == 0 after 64k submits must index ring[7],
    // not ring[-1] via signed promotion (audit S3, blk shares the idiom).
    uint32_t echo =
        g_used->ring[(static_cast<uint32_t>(used_now) + GPU_QUEUE_SIZE - 1u) %
                     GPU_QUEUE_SIZE]
            .id;
    if (echo != 0) {
        Logger::error("virtio-gpu: completion id mismatch");
        return false;
    }
    uint32_t type = 0;
    __builtin_memcpy(&type, g_resp_buf, sizeof(type));
    return type == expected_type;
}

} // namespace

/// @brief Response staging buffer (valid until the next submit).
const void *virtio_gpu_resp() {
    return g_resp_buf;
}

namespace {
// submit() lives here (internal linkage): the ring helpers above are in the
// same TU-unique namespace, so this block shares them.
bool submit(const void *req, uint32_t req_len, uint32_t resp_len,
            uint32_t expected_type) {
    if (!req) {
        return false;
    }
    if (req != g_cmd_buf) {
        if (req_len > sizeof(g_cmd_buf)) {
            return false;
        }
        memcpy(g_cmd_buf, req, req_len);
    }
    return submit_staged(req_len, resp_len, expected_type);
}

} // namespace

bool virtio_gpu_probe(uint32_t &w, uint32_t &h) {
    w = 0;
    h = 0;
    if (g_attempted) {
        if (g_ready) {
            w = g_width;
            h = g_height;
            return true;
        }
        return false;
    }
    g_attempted = true;

    // DTB-primary discovery (issue #350 helpers): DeviceID register
    // filter selects the GPU transport (MMIO id 16).
    if (!arch::virtio_find_device(arch::VIRTIO_DEVICE_GPU, g_transport)) {
        return false;
    }
    if (!arch::virtio_init_transport(g_transport)) {
        Logger::error("virtio-gpu: transport init failed");
        return false;
    }
    if (!arch::virtio_negotiate_features(g_transport,
                                         arch::VIRTIO_F_VERSION_1)) {
        Logger::error("virtio-gpu: feature negotiation failed");
        return false;
    }

    // Legacy-v1 queue: one contiguous chunk (planner A8 geometry) —
    // desc at +0, avail right after, used at the single-sourced offset.
    uint64_t chunk_phys = phys_of(g_queue_chunk);
    if (!chunk_phys) {
        Logger::error("virtio-gpu: queue chunk unmapped");
        return false;
    }
    uint64_t avail_phys =
        chunk_phys + static_cast<uint64_t>(GPU_QUEUE_SIZE) * 16;
    uint64_t used_phys =
        chunk_phys + arch::virtio_legacy_used_off(GPU_QUEUE_SIZE);
    if (used_phys + 8 + GPU_QUEUE_SIZE * 8 > chunk_phys + sizeof(g_queue_chunk)) {
        Logger::error("virtio-gpu: queue geometry overflows chunk");
        return false;
    }
    g_desc = reinterpret_cast<arch::VirtqDesc *>(g_queue_chunk);
    g_avail = reinterpret_cast<arch::VirtqAvail *>(g_queue_chunk +
                                                  GPU_QUEUE_SIZE * 16);
    g_used = reinterpret_cast<arch::VirtqUsed *>(
        g_queue_chunk + arch::virtio_legacy_used_off(GPU_QUEUE_SIZE));
    memset(g_queue_chunk, 0, sizeof(g_queue_chunk));
    if (!arch::virtio_setup_queue(g_transport, 0, GPU_QUEUE_SIZE, chunk_phys,
                                  avail_phys, used_phys)) {
        Logger::error("virtio-gpu: queue setup failed");
        return false;
    }
    uint8_t status = arch::virtio_read_status(g_transport);
    arch::virtio_write_status(g_transport, status | arch::VIRTIO_STATUS_DRIVER_OK);

    SpinLockGuard<sync::SpinLock> guard(g_gpu_lock);

    // GET_DISPLAY_INFO → preferred mode from output 0.
    GpuCtrlHdr get{};
    get.type = GPU_CMD_GET_DISPLAY_INFO;
    if (!submit(&get, sizeof(get), sizeof(GpuDisplayInfo),
                GPU_RESP_OK_DISPLAY_INFO)) {
        Logger::error("virtio-gpu: GET_DISPLAY_INFO failed");
        return false;
    }
    const auto *info =
        reinterpret_cast<const GpuDisplayInfo *>(virtio_gpu_resp());
    if (!info->pmodes[0].enabled || info->pmodes[0].rect.width == 0 ||
        info->pmodes[0].rect.height == 0 ||
        info->pmodes[0].rect.width > kernel::fb::FB_MAX_WIDTH ||
        info->pmodes[0].rect.height > kernel::fb::FB_MAX_HEIGHT) {
        Logger::error("virtio-gpu: display mode rejected");
        return false;
    }
    uint32_t width = info->pmodes[0].rect.width;
    uint32_t height = info->pmodes[0].rect.height;
    uint64_t span = static_cast<uint64_t>(width) * height * 4;
    if (span == 0 || span > kernel::fb::FB_MAX_BYTES) {
        Logger::error("virtio-gpu: span out of bounds");
        return false;
    }
    uint32_t pages = static_cast<uint32_t>((span + 4095) / 4096);
    if (pages == 0 || pages > kernel::fb::FB_MAX_PAGES) {
        Logger::error("virtio-gpu: page count out of bounds");
        return false;
    }
    // Backing store: PMM-contiguous at boot (pre-baseline, zero
    // ResourceTracker delta). Static BSS cannot be used — a 4 MiB backing
    // trips the riscv64 boot.S fail-stop (kernel_phys_end < 0x80C00000).
    if (!kernel::fb::backing_init(width, height)) {
        Logger::error("virtio-gpu: backing alloc failed");
        return false;
    }

    // RESOURCE_CREATE_2D (id 1, XRGB8888).
    GpuCreate2d create{};
    create.hdr.type = GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = GPU_RESOURCE_ID;
    create.format = GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (!submit(&create, sizeof(create), sizeof(GpuCtrlHdr),
                GPU_RESP_OK_NODATA)) {
        GpuUnref unref{};
        unref.hdr.type = GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = GPU_RESOURCE_ID;
        submit(&unref, sizeof(unref), sizeof(GpuCtrlHdr),
               GPU_RESP_OK_NODATA);
        kernel::fb::backing_release();
        Logger::error("virtio-gpu: CREATE_2D failed");
        return false;
    }

    // RESOURCE_ATTACH_BACKING (id 1, one entry per backing page).
    uint32_t attach_len =
        static_cast<uint32_t>(sizeof(GpuAttachBacking) +
                              pages * sizeof(GpuMemEntry));
    if (attach_len > sizeof(g_cmd_buf)) {
        kernel::fb::backing_release();
        Logger::error("virtio-gpu: attach overflows cmd buffer");
        return false;
    }
    auto *attach = reinterpret_cast<GpuAttachBacking *>(g_cmd_buf);
    attach->hdr.type = GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->hdr.flags = 0;
    attach->hdr.fence_id = 0;
    attach->hdr.ctx_id = 0;
    attach->hdr.padding = 0;
    attach->resource_id = GPU_RESOURCE_ID;
    attach->nr_entries = pages;
    auto *entries = reinterpret_cast<GpuMemEntry *>(g_cmd_buf + sizeof(*attach));
    for (uint32_t i = 0; i < pages; ++i) {
        uint64_t p = 0;
        if (!kernel::fb::backing_phys(i, p)) {
            kernel::fb::backing_release();
            Logger::error("virtio-gpu: backing page unmapped");
            return false;
        }
        entries[i].addr = p;
        entries[i].length = 4096;
        entries[i].padding = 0;
    }
    if (!submit_staged(attach_len, sizeof(GpuCtrlHdr),
                       GPU_RESP_OK_NODATA)) {
        GpuUnref unref{};
        unref.hdr.type = GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = GPU_RESOURCE_ID;
        submit(&unref, sizeof(unref), sizeof(GpuCtrlHdr),
               GPU_RESP_OK_NODATA);
        kernel::fb::backing_release();
        Logger::error("virtio-gpu: ATTACH_BACKING failed");
        return false;
    }

    // SET_SCANOUT (output 0, full rect) + initial full present.
    GpuSetScanout scan{};
    scan.hdr.type = GPU_CMD_SET_SCANOUT;
    scan.rect.x = 0;
    scan.rect.y = 0;
    scan.rect.width = width;
    scan.rect.height = height;
    scan.scanout_id = 0;
    scan.resource_id = GPU_RESOURCE_ID;
    if (!submit(&scan, sizeof(scan), sizeof(GpuCtrlHdr),
                GPU_RESP_OK_NODATA)) {
        GpuUnref unref{};
        unref.hdr.type = GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = GPU_RESOURCE_ID;
        submit(&unref, sizeof(unref), sizeof(GpuCtrlHdr),
               GPU_RESP_OK_NODATA);
        kernel::fb::backing_release();
        Logger::error("virtio-gpu: SET_SCANOUT failed");
        return false;
    }

    g_width = width;
    g_height = height;
    g_ready = true;
    w = width;
    h = height;
    return true;
}

bool virtio_gpu_present(uint32_t y0, uint32_t y1) {
    if (!g_ready)
        return false;
    if (y0 >= y1 || y0 >= g_height)
        return y0 == y1; // empty band: nothing to do
    if (y1 > g_height)
        y1 = g_height;
    SpinLockGuard<sync::SpinLock> guard(g_gpu_lock);

    uint32_t pitch = g_width * 4;
    GpuTransfer xfer{};
    xfer.hdr.type = GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.rect.x = 0;
    xfer.rect.y = y0;
    xfer.rect.width = g_width;
    xfer.rect.height = y1 - y0;
    xfer.offset = static_cast<uint64_t>(y0) * pitch;
    xfer.resource_id = GPU_RESOURCE_ID;
    if (!submit(&xfer, sizeof(xfer), sizeof(GpuCtrlHdr),
                GPU_RESP_OK_NODATA)) {
        return false;
    }
    GpuFlush flush{};
    flush.hdr.type = GPU_CMD_RESOURCE_FLUSH;
    flush.rect = xfer.rect;
    flush.resource_id = GPU_RESOURCE_ID;
    return submit(&flush, sizeof(flush), sizeof(GpuCtrlHdr),
                  GPU_RESP_OK_NODATA);
}

bool virtio_gpu_ready() {
    return g_ready;
}

} // namespace kernel::gpu

#endif // !CONFIG_ARCH_X86_64
