# NexIOS Audit Report — Drivers / VFS / Net / Boot / Arch

**Scope:** `src/kernel/driver/*`, `src/kernel/vfs/*`, `src/kernel/net/*`, `src/kernel/arch/*`, `src/kernel/boot/*`
**Date:** 2026-08-22 · **Baseline:** v0.4.2-dev (commit `624b9e5f`) · **Method:** static review against the FLAW history (BUGS.md, docs/specs/drivers.md §8 ledger)
**Focus:** DMA races, unbounded polling, IRQ-handler safety, locking on shared state

---

## Verification of Documented Fixes

The fixes recorded in the drivers ledger are real and present in the code:

- **FLAW-03 (virtio-net ring races): RESOLVED — confirmed.** `IrqSpinLockGuard` on `dev.lock_` in send/poll/add_rx_buf; `tx_inflight_` flag; used-ring snapshot taken **before** notify (`virtio_net.cpp:283–296`); consume/recycle/advance atomic under the lock (`virtio_net.cpp:330+`).
- **FLAW-08 (serial unbounded polling): RESOLVED — confirmed.** TX/RX polls bounded via `SERIAL_TX_WAIT_ITERS` / `SERIAL_RX_WAIT_ITERS` (`arch/x86_64/serial.cpp:57,66,87`).
- **FLAW-10 (keyboard drain): RESOLVED — confirmed.** Drain capped at 16 iterations (`arch/x86_64/keyboard.cpp:76`); ISR is short, lock-free, atomics with release ordering.
- **FLAW-01/02 (DmaEngine / PingPongDma): clean.** `IrqSpinLockGuard` throughout; callback is released from the lock before firing in IRQ context (`dma.cpp:258–270`); documented PingPong→Engine lock order (`dma.cpp:357`).
- **ata_pio:** `poll_status` bounded (`ATA_POLL_TIMEOUT`, ata_pio.cpp:97).

## Findings — HIGH

### H-1. AHCI: no locks on shared port state
`alloc_slot()` (ahci.cpp:278) reads CI/SACT without a lock. Two concurrent `read_sector`/`write_sector` calls can acquire the same slot and the same `data_bufs_` DMA buffer → data corruption. `start_cmd`/`wait_cmd` are likewise not serialized.

### H-2. FLAW-04 still open: `GHC_IE` enabled without ISR
ahci.cpp:520 enables interrupts globally at the HBA while no handler is registered. The teardown use-after-free risk documented in the spec remains unchanged.

### H-3. virtio_blk: entire submit path unlocked
`submit_request` shares a single DMA buffer and the avail/used rings without any spinlock (virtio_blk.cpp:130–195). Additionally `used_idx = used_->idx` is read only **after** `virtio_notify()` (line 169) — exactly the race pattern that FLAW-03 fixed in the net driver. With concurrent callers or fast completions, completion checks can be lost or corrupted.

## Findings — MEDIUM

### M-1. FLAW-05 half-fixed
`wait_cmd` is now bounded/parametrized, but defaults to 5,000,000 `io_wait` iterations of busy-spin with unclear IRQ state (ahci.cpp:367–394; callers pass `5000000` at :419,:452,:536). This blocks the core for up to seconds instead of being a scheduler-blocked wait as the spec requires. Ledger entry "OPEN" is correct.

### M-2. Ledger inconsistency FLAW-06
Code now has `int timeout = 1000000; while (... && --timeout > 0)` (virtio_blk.cpp:171) — i.e. bounded — but the ledger docs/specs/drivers.md:189 still says OPEN. Either backport the fix into the ledger or implement the required scheduler-blocked bounded wait.

### M-3. net.cpp: globals without lock
`g_arp_cache`, `g_ip_ident`, `g_icmp_reply` (net.cpp:33–37) are read/written concurrently from the RX path (poll/IRQ context) and from `net_send_udp`/`net_arp_resolve` (task context) with no synchronization → torn ARP-cache updates, duplicate IP ident values.

### M-4. virtio_net_poll: missing `desc_idx` validation (virtio_net.cpp:347ff.)
`rx_used->ring[slot].id` comes from the device and is used unchecked as an index into `rx_bufs[desc_idx]`. A faulty/malicious device can cause OOB read/write. Bounds check against `queue_size` is missing.

### M-5. virtio_net_send_frame UAF window (virtio_net.cpp:300–310)
The completion poll runs outside the lock on `dev.tx_used`. If `virtio_net_destroy()` runs concurrently, `dev` dangles afterwards (the local reference does not survive the `g_virtio_net_dev = nullptr` check).

## Findings — LOW

### L-1. `net_arp_resolve` busy-wait
Busy-waits up to ~150 ticks (3×50) with a `net_poll` spin (net.cpp:183–197). Bounded, but blocks the core; cooperative waiting would be more RTOS-conformant.

### L-2. `pipe_write` missed wakeup on partial write
Posts `data_avail` only after fully writing (vfs/pipe.cpp:106–117). The early-return path (buffer full, total < count) skips the post — potentially lost wakeup. `read_closed`/`write_closed` checks are unsynchronized.

### L-3. Boot-time calibration polls
APIC calibration 1M-iteration poll (apic.cpp:320) and TSC calibration up to 12×50k-TSC-cycle spins (timer.cpp:107–115). Boot context, bounded, acceptable — noted for completeness.

### L-4. procfs TCB pointer lifetime (procfs.cpp:291,439,502)
Reads `TaskControlBlock*` without lifetime guarantees — TOCTOU if the task exits between pointer acquisition and read (likely mitigated by cooperative scheduling).

## Clean Areas

boot/bootinfo.hpp (`add_region` bounds-checked), boot.asm (Multiboot registers saved, double-fault IST in idt.cpp:73), keyboard/timer ISRs short and lock-free, IDT dispatch trivial (idt.cpp:106ff.).

## Assessment

The net driver has been carefully hardened, but **AHCI and virtio_blk lag behind**: the same race classes fixed there have not been applied consistently. For a hard RTOS with SIL-3 ambitions, findings H-1 through H-3 are blockers.
