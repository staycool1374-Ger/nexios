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

/// @file virtio_gpu.hpp
/// @brief virtio-gpu control-queue driver over legacy-v1 virtio-MMIO
/// (issue #394 Slice A).  Guest-RAM 2D scanout: no BAR, no VMM remap —
/// the device reads pixels from the static fb_backing buffer (phys via
/// VMM::virt_to_phys_err).  Device IDs: 0x1050 modern / 0x1010 legacy
/// (MMIO DeviceID 16).  Command wire formats follow Virtio 1.2 §5.7.

#include <types.hpp>

#if !defined(CONFIG_ARCH_X86_64)

namespace kernel::gpu {

// --- Wire protocol (Virtio 1.2 §5.7.6) ---

constexpr uint32_t GPU_CMD_GET_DISPLAY_INFO = 0x0100;
constexpr uint32_t GPU_CMD_RESOURCE_CREATE_2D = 0x0101;
constexpr uint32_t GPU_CMD_RESOURCE_UNREF = 0x0102;
constexpr uint32_t GPU_CMD_SET_SCANOUT = 0x0103;
constexpr uint32_t GPU_CMD_RESOURCE_FLUSH = 0x0104;
constexpr uint32_t GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105;
constexpr uint32_t GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
constexpr uint32_t GPU_RESP_OK_NODATA = 0x1100;
constexpr uint32_t GPU_RESP_OK_DISPLAY_INFO = 0x1101;
constexpr uint32_t GPU_RESP_ERR_UNSPEC = 0x1200;

constexpr uint32_t GPU_FORMAT_B8G8R8X8_UNORM = 2;

constexpr uint16_t GPU_QUEUE_SIZE = 8;
constexpr uint32_t GPU_RESOURCE_ID = 1;
constexpr int GPU_CMD_TIMEOUT_ITERS = 1000000;

/// Backend selected by the framebuffer init chain (GPU preferred).
enum class FbBackend : uint8_t {
    None = 0,
    VirtioGpu = 1,
    Ramfb = 2,
};

/// @brief Pure backend-selection policy (QE-testable, no hardware).
/// @return VirtioGpu when the GPU probed (ramfb ignored), Ramfb when only
/// ramfb probed, None when neither did.
constexpr FbBackend select_backend(bool gpu_ok, bool ramfb_ok) {
    if (gpu_ok)
        return FbBackend::VirtioGpu;
    if (ramfb_ok)
        return FbBackend::Ramfb;
    return FbBackend::None;
}

// --- Wire structs (packed; static_asserted sizes guard the layout) ---

struct GpuCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));
static_assert(sizeof(GpuCtrlHdr) == 24, "gpu ctrl hdr is 24 B");

struct GpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));
static_assert(sizeof(GpuRect) == 16, "gpu rect is 16 B");

struct GpuDisplayOne {
    GpuRect rect;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));
static_assert(sizeof(GpuDisplayOne) == 24, "gpu display-one is 24 B");

struct GpuDisplayInfo {
    GpuCtrlHdr hdr;
    GpuDisplayOne pmodes[16];
} __attribute__((packed));
static_assert(sizeof(GpuDisplayInfo) == 24 + 16 * 24,
              "gpu display-info fits the 512 B response");

struct GpuCreate2d {
    GpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));
static_assert(sizeof(GpuCreate2d) == 40, "gpu create-2d is 40 B");

struct GpuAttachBacking {
    GpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));
static_assert(sizeof(GpuAttachBacking) == 32, "gpu attach is 32 B");

struct GpuMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));
static_assert(sizeof(GpuMemEntry) == 16, "gpu mem entry is 16 B");

struct GpuSetScanout {
    GpuCtrlHdr hdr;
    GpuRect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));
static_assert(sizeof(GpuSetScanout) == 48, "gpu set-scanout is 48 B");

struct GpuTransfer {
    GpuCtrlHdr hdr;
    GpuRect rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));
static_assert(sizeof(GpuTransfer) == 56, "gpu transfer is 56 B");

struct GpuFlush {
    GpuCtrlHdr hdr;
    GpuRect rect;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));
static_assert(sizeof(GpuFlush) == 48, "gpu flush is 48 B");

struct GpuUnref {
    GpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));
static_assert(sizeof(GpuUnref) == 32, "gpu unref is 32 B");

// --- Driver API (boot-time, single-hart, no ISR registered) ---

/// @brief Probe the virtio-gpu device, create the 2D resource + scanout.
/// @param w Receives the display width on success.
/// @param h Receives the display height on success.
/// @return true when the GPU path is live (idempotent latch inside).
bool virtio_gpu_probe(uint32_t &w, uint32_t &h);

/// @brief Flush the dirty row band [y0, y1) to the host scanout.
/// @return true when the transfer+flush completed (false = keep dirty).
bool virtio_gpu_present(uint32_t y0, uint32_t y1);

/// @brief Response staging buffer (valid until the next submit).
const void *virtio_gpu_resp();

/// @brief True when the GPU path completed probe successfully.
bool virtio_gpu_ready();

} // namespace kernel::gpu

#endif // !CONFIG_ARCH_X86_64
