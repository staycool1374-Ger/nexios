# Driver PDs — Static SDF + Supervision Contract (issue #48)

Strangler step a (no flip): userspace driver PDs behind per-role
`CONFIG_DRIVER_PD_*` (default 0). Kernel keeps routing slots only.

## Roles and caps (SDF rows in `task/boot_sdf.cpp`)

| Role | PD ELF | Caps (exact) | Notes |
|---|---|---|---|
| serial | serial-pd.c.elf | Mmio COM1 (io_port) | TX+RX owned (#51 step-c); kernel keeps panic raw_write fallback only |
| kbd | kbd-pd.c.elf | IRQ 33 + IOPORT 0x60–0x64 (x86) | arch stub off-x86 (decoder stays UART) |
| rtc | rtc-pd.c.elf | CMOS ports / PL031 / Goldfish (per arch) | no IRQ; feeds timer-srv (#241) |
| fb | fb-pd.c.elf | device caps only (mode-set/present) | zero IRQ (polled); emulation = #51 |
| block | block-pd.c.elf | MSI/MSI-X + DMA domain + SHM | virtio/AHCI via SDF dispatch |
| net | net-pd.c.elf | MSI-X/INTID + DMA domain + SHM | RX rings + TX, per-domain DMA |

Rules: no ambient authority (fail-closed at claim/arm/map); one owner per
device (compile-time gate); restart re-derives via `sdf_rebind` (never
inherits domain/vector/mapping); in-flight I/O error-replies (never hangs);
kill/restart ×3 keeps ResourceTracker delta-0; serial-log fault artifacts.

## Serial TX flip (issue #51 step-c)

Kernel→PD BIND conveys the PD's composed cap handles (arg0 = encoded
MmioCap handle, arg1 = phys base, arg2 = size, flags = cap count;
single-cap roles only — multi-cap BIND is a second-round follow-up).
serial-pd maps once at BIND (x86 `SYS_IOPORT_GRANT`, aa64/rv64
`SYS_MMIO_MAP`) and serves polled `DRV_OP_WRITE` (arg0 = byte, bounded
LSR poll, fail-closed `-EAGAIN` on stall), replying to `req.owner_id`
(the send_sync requester queue; reply-to-1 is a dead letter, #441).
Kernel TX attempts the RPC only when safe to block (task context,
ISR-nesting 0, PD alive); every other context — and any RPC failure —
takes the raw path. `Logger::raw_write` bypasses Terminal entirely, so
panic never reaches the PD. IRQ4 stays kernel-side until the step-d
input flip (no dead caps in step-c).

## Wire

48 B `DriverRpc` (`userspace/driver_pd.h`, mirrors `TimerRpc`): op/flags/
epoch/owner id+gen/args/status. Control via `send_sync` (+FAST ≤48B);
bulk via SPSC SHM. Bounded wait (`send_sync_bounded`, `kRpcGuardTicks`);
epoch+gen-validated replies; tombstone drain for dead owners.

### Serial TX ring (issue #51 step-c-prime, option A)

Sync per-char `DRV_OP_WRITE` does not survive boot flood (1 IPC/char at
SS(1,10) PD budget + unmutexed PD/raw UART sharing garbles; convicted
live, 51c HELD UNMERGED). The TX path is a kernel SPSC ring drained by
the PD instead:

- SDF row: SERIAL carries the UART cap (index 0) + one SHM CapSpec
  (index 1, `kSerialTxRingPages` pages, `CONFIG_SERIAL_TX_SHM`-gated).
  `mint_one` allocates fresh user frames per compose/rebind (never
  inherited), pins them in a `FrameCap`, installs it in the PD CSpace,
  initialises the header via the HHDM alias, and records (phys, pages)
  for the producer. Rollback/compose-failure clears the record —
  fail-closed to the raw path.
- Conveyance: BIND (index 0, UART) then MAP_SHM (index 1: arg0 =
  encoded FrameCap handle, arg1 = pages, arg2 = 8 B elements). The PD
  `SYS_FRAME_MAP`s once, validates the header (magic/version/pow2
  capacity/element size — torn/hostile headers unmap + refuse), and
  re-maps on a changed handle (unmap-then-map, never double-map).
- Producer (kernel `serial_tx_write`): packs 7 bytes per 8 B element,
  drops + counts past a full ring (the print path never blocks),
  NOTIFYs the PD on empty→nonempty. Returns live-channel-true even on
  drops: counted loss is the documented backpressure, a raw-path
  fallback would duplicate bytes (the 51c garble).
- Consumer (serial-pd drain loop): pops all → `uart_putc` per byte
  (reused bounded poll; stops fail-closed on stall, remainder next
  round — no duplication, order preserved). 1-tick recv poll when the
  ring is mapped (bounded drain latency for RPCs and TX), block-forever
  before MAP (zero idle overhead). The NOTIFY is a latency
  optimization; the poll is the liveness backstop.
- Epochs: PD restart bumps `pd_epoch`; the producer re-resolves phys
  per epoch, BIND+MAP re-run once per epoch. Panic/ISR keep the raw
  path (`Logger::raw_write` bypasses Terminal entirely).
- Reversibility: `CONFIG_SERIAL_TX_SHM` (default 0). OFF = 51c
  behavior (per-char WRITE RPC when `CONFIG_DRIVER_PD_SERIAL`, else
  raw). Zero new syscalls (NOTIFY + `SYS_FRAME_MAP` exist).

## Boundaries (no double ownership)

- #48 owns device caps + PD isolation; #51 owns emulation policy.
- #192 owns irq-mux policy; this lane uses route-only delivery.
- #241/#194 timer-srv consumes RTC reads; #193 supervises restarts.
- #168 SMMU keeps ARM DMA honest; legacy ATA PIO stays non-DMA.

## VFS/block rows (#47, issue #47 PDs share this SDF)

- `VFS_SRV` (vfs-srv PD): zero-cap row by design (no ambient authority).
  Composes empty; the PD talks to clients over its endpoint only.
- `BLOCK_SRV` (block-srv PD, issue #47 step-b1): static arch-switched
  row + one 4-page shared window (`CapKind::DMA_REGION`, raw sector/
  queue area, kernel HHDM alias). x86_64: two IO-port caps
  (0x1F0/0x170 × 8 ports, exact ranges — never page-widened, the
  grant ledger stays least-privilege). aarch64/riscv64: first captured
  virtio transport window + 4-page window. Window phys is conveyed
  EXACT (virtio slots stride 0x200 inside a page): the cap covers
  whole pages from the aligned base and the PD applies the
  intra-page offset itself (mapping an aligned truncation made the
  PD probe the wrong slot — device 0 instead of blk). No IRQ cap in
  b1 (polled completion; IRQ delegation is step-b2).
  Degraded off-x86 (transport window without a frame is never
  composed) starts capless with a logged warning — allowed, never
  silent. Slot/frame persistence covers all roles (`kPdRoleCount`).
  See `docs/specs/block-srv.md`.
- Single device owner: `CONFIG_DRIVER_PD_BLOCK` and `CONFIG_BLOCK_SRV`
  are mutually exclusive (hard build error); kernel probing is gated
  off only by exactly one of them; #47 owns this table's block row
  (S4/S5/S6 wire the fb/block/net PDs here).

## Term/log PDs (issue #51, share this table)

TERM_SRV (term-srv.c.elf) + LOG_SRV (log-srv.c.elf) are policy-only
rows: `sdf_policy_only()` is true, `sdf_compose` rejects any non-empty
variant, and `pd_owns_device` is unconditionally false for both. The
#48 SDF boundary test asserts this: no device cap may name a term/log
role. See `docs/specs/term-srv.md` for the wire/lifecycle contract.
