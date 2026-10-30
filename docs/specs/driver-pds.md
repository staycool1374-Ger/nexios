# Driver PDs — Static SDF + Supervision Contract (issue #48)

Strangler step a (no flip): userspace driver PDs behind per-role
`CONFIG_DRIVER_PD_*` (default 0). Kernel keeps routing slots only.

## Roles and caps (SDF rows in `task/boot_sdf.cpp`)

| Role | PD ELF | Caps (exact) | Notes |
|---|---|---|---|
| serial | serial-pd.c.elf | Mmio COM1 (RX path) | RX-only; kernel keeps TX + panic |
| kbd | kbd-pd.c.elf | IRQ 33 + IOPORT 0x60–0x64 (x86) | arch stub off-x86 (decoder stays UART) |
| rtc | rtc-pd.c.elf | CMOS ports / PL031 / Goldfish (per arch) | no IRQ; feeds timer-srv (#241) |
| fb | fb-pd.c.elf | device caps only (mode-set/present) | zero IRQ (polled); emulation = #51 |
| block | block-pd.c.elf | MSI/MSI-X + DMA domain + SHM | virtio/AHCI via SDF dispatch |
| net | net-pd.c.elf | MSI-X/INTID + DMA domain + SHM | RX rings + TX, per-domain DMA |

Rules: no ambient authority (fail-closed at claim/arm/map); one owner per
device (compile-time gate); restart re-derives via `sdf_rebind` (never
inherits domain/vector/mapping); in-flight I/O error-replies (never hangs);
kill/restart ×3 keeps ResourceTracker delta-0; serial-log fault artifacts.

## Wire

48 B `DriverRpc` (`userspace/driver_pd.h`, mirrors `TimerRpc`): op/flags/
epoch/owner id+gen/args/status. Control via `send_sync` (+FAST ≤48B);
bulk via SPSC SHM. Bounded wait (`send_sync_bounded`, `kRpcGuardTicks`);
epoch+gen-validated replies; tombstone drain for dead owners.

## Boundaries (no double ownership)

- #48 owns device caps + PD isolation; #51 owns emulation policy.
- #192 owns irq-mux policy; this lane uses route-only delivery.
- #241/#194 timer-srv consumes RTC reads; #193 supervises restarts.
- #168 SMMU keeps ARM DMA honest; legacy ATA PIO stays non-DMA.

## VFS/block rows (#47, issue #47 PDs share this SDF)

- `VFS_SRV` (vfs-srv PD): zero-cap row by design (no ambient authority).
  Composes empty; the PD talks to clients over its endpoint only.
- `BLOCK_SRV` (block-srv PD): captured PCI/virtio MMIO CapSpecs (page
  aligned, ≤6) + optional DMA domain (only when an IOMMU was probed at
  capture). Degraded (no controller/IOMMU) composes empty with a logged
  warning — allowed, never silent. See `docs/specs/block-srv.md`.
- Single device owner: `CONFIG_DRIVER_PD_BLOCK` and `CONFIG_BLOCK_SRV`
  are mutually exclusive (hard build error); kernel probing is gated
  off only by exactly one of them; #47 owns this table's block row
  (S4/S5/S6 wire the fb/block/net PDs here).
