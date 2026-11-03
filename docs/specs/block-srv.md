# block-srv — Block Storage Server PD Contract (issue #47, strangler step a)

Userspace block server behind `CONFIG_BLOCK_SRV` (default 0). Step-a
scope: PD task + SDF row (captured PCI/virtio topology + optional DMA
domain) + Kconfig OFF shim + loopback/parity + fault drill. No flip:
kernel probing stays (gated off only when the PD owns the device).

## Wire

48 B `BlockRpc` (`kernel/driver/block_srv.hpp`, mirrors `TimerRpc`):
op/flags/epoch/owner id+gen/lba/arg/handle/status-op/status. Ops:
BIND, GEOMETRY, READ_BLOCK, WRITE_BLOCK, FLUSH, DMA_MAP, DMA_UNMAP,
LOOPBACK, INJECT_MID_TRANSFER. Server answers BIND + LOOPBACK
(fixed 512 B pattern); DMA_MAP with a zero handle fails closed
(`-EACCES` shape); everything else returns a typed `-ENOSYS` pre-flip.

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
The SDF row carries at most 6 page-aligned MMIO CapSpecs + one DMA
domain; capture runs pre-IrqGuard via `capture_block_topology()`.

## Pre-flip status

AHCI/ATA + virtio-blk probing stays in-kernel (gated off only when the
PD owns the device). The flip (real block I/O through the PD,
fd-table handoff design, revoke cascade) is a later step with the
§7 gate. OFF (default) keeps in-kernel probing: `drivers` 107/107 +
`storage` 167/167 + `ahci_live` oracles bit-identical.
