# block-srv — Block Storage Server PD Contract (issue #47, strangler step a)

Userspace block server behind `CONFIG_BLOCK_SRV` (default 0). Step-a
scope: PD task + SDF row (captured PCI/virtio topology + optional DMA
domain) + Kconfig OFF shim + loopback/parity + fault drill. No flip:
kernel probing stays (gated off only when the PD owns the device).

## Wire

48 B `BlockRpc` (`kernel/driver/block_srv.hpp`, mirrors `TimerRpc`):
op/flags/rsvd/epoch/owner id+gen/lba/arg/handle/status-op/status/aux.
Ops: BIND, GEOMETRY, READ_BLOCK, WRITE_BLOCK, FLUSH, DMA_MAP,
DMA_UNMAP, LOOPBACK, INJECT_MID_TRANSFER, CAP_GET (9).

Step-b1 order: CAP_GET rounds first (kernel→PD carries index +
encoded handle + phys + size per row cap; the PD maps and echoes),
BIND last (flags = row cap count; the PD probes with every cap
present and answers backend + geometry: status_op = backend,
arg = sector size, aux = sector count, or the probe stage 1..6
when backend NONE). The phys is the EXACT window base (virtio
slots stride 0x200 inside a page): the cap spans whole pages from
the aligned base and the PD probes at va + (phys & 0xFFF).
READ/WRITE carry the
request sequence in arg; the single in-flight claim fails fast
(-EAGAIN), a restart race reports -EAGAIN once, mismatches -EIO.
Data moves through the 4-page shared window at `kBlockDataOffset`
(virtio queue area in page 0, header at `kBlockHdrOffset`, status
at `kBlockStatusOffset`); kernel release/acquire fences pair with
the PD's reply barrier. LOOPBACK keeps the fixed 512 B pattern;
DMA_MAP with a zero handle fails closed (`-EACCES` shape);
FLUSH is a no-op success (writes are direct, no write-back cache).

## Lifecycle

PID cell + epoch + tombstone ring (same shape as timer-srv):
`on_restart` stores the PID and bumps the epoch; the server purges
all state on epoch change. Teardown appends (owner id, gen) to a
bounded (16) lock-free ring; the next RPC piggybacks CANCEL_OWNERS.
Replies validate (server, epoch, owner id+gen, op echo); mismatch is
dropped with a bounded re-wait, never consumed.

## DMA discipline

No `IoMmuDmaCap` ⇒ typed deny on any DMA op; domains disjoint per
SDF row; domain destroy cascades (unmap + fence in-flight, gen-tagged
handles). Legacy ATA PIO stays a non-DMA path — never a bypass.
The SDF row carries at most 6 page-aligned CapSpecs + one DMA domain
grant; capture runs pre-IrqGuard via `capture_block_topology()`.

## Step-b1 backends (issue #47, live)

x86_64: ATA PIO over I/O-port caps (primary 0x1F0 + secondary 0x170,
IDENTIFY across master/slave, LBA28 READ/WRITE + FLUSH CACHE).
aarch64/riscv64: virtio-blk over MMIO, legacy v1 interface on QEMU
`virt` (VERSION reads 1 — accepted like the kernel finder; modern
v2 also accepted). Legacy queue is one contiguous chunk in shared
pages 2–3 (desc + avail + 4 KiB-aligned used ring, PFN path, no
Ready bit); pages 0–1 stay the data/header/status window at the
fixed `kBlock*Offset` contract. Bounded used-ring poll, no IRQ.

## Pre-flip status

AHCI/ATA + virtio-blk probing stays in-kernel (gated off only when the
PD owns the device). The flip (real block I/O through the PD,
fd-table handoff design, revoke cascade) is a later step with the
§7 gate. OFF (default) keeps in-kernel probing: `drivers` 107/107 +
`storage` 167/167 + `ahci_live` oracles bit-identical.
