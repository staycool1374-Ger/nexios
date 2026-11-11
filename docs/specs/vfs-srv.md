# vfs-srv — VFS Server PD Contract (issue #47, strangler step-b3)

Userspace VFS server behind `CONFIG_VFS_SRV` (default 0). Step-a scope
(PD task + SDF row + Kconfig OFF shim + loopback/parity + fault drill)
is superseded by step-b3: live backend serving for all seven backends
(INITRD/FAT32/TMPFS/DEVFS/PIPE/RAMDISK/PROCFS). No flip: authorize
stays kernel-side (`syscall_handlers_fs.cpp`, `vfs.cpp`, `vfsd.c`
untouched); the kernel VFS path stays authoritative, the PD shadows it.

## Wire

48 B `VfsRpc` (`kernel/vfs/vfs_srv.hpp`, mirrors `TimerRpc`): op/flags/
rsvd/epoch/owner id+gen/ino/offset/size/status-op/status. Ops 0..8 are
the frozen step-a ABI (BIND, backend probes, INJECT_CORRUPT drill).
Step-b3 adds 9..19: CAP_GET, OPEN, CLOSE, READ, WRITE, STAT, READDIR,
CREATE, UNLINK, BACKEND_STATUS, INJECT_BACKEND_FAULT.

CAP_GET/BIND mirror the block-srv shape with a 48 B packing difference:
kind rides `flags`, the encoded row handle rides `size`, phys/size go
in `ino`/`offset`, the row index in `status_op`. BIND goes last
(`offset` = row cap count); the PD answers the backend mask in `size`
(one bit per backend, INITRD=0 .. PROCFS=6).

OPEN returns a PD-owned handle packed as `backend << 56 | base`. For
image-backed backends the low 56 bits are `(locator24 << 32 | size32)`
— the raw image locator (cpio byte offset / FAT start cluster /
ramdisk start block / proc file index) plus the file size, which the
kernel uses to clamp reads (EOF shape). Locators stay under 2^24
(16 MiB images; b3 test images are ~2 MiB — larger images fail closed
at OPEN with `-EINVAL`). TMPFS/PIPE handles are table slots into the
PD-owned stores (generation-tagged; stale handles fail typed, never
silently succeed).

## Split of duties

The kernel stages RAW backend bytes into the shared 4-page window
(`kVfsRegionPages`, data at `kVfsDataOffset`, name slot at
`kVfsNameOffset`, stats at `kVfsStatsOffset`); the PD parses and owns
namespace, handles and policy:
- INITRD: kernel builds a compact cpio index (name/data_off/size/mode,
  ≤128 entries) once per epoch into .bss and stages it per op; the PD
  resolves paths and packs image offsets.
- FAT32: kernel stages BPB + root-dir cluster; the PD parses geometry +
  dirents and packs (start_cluster, size); data reads are kernel-walked
  chains the PD length-checks.
- RAMDISK: kernel stages super + full dir region; the PD parses extents
  and packs (start_block, size); data reads are kernel-staged spans.
- TMPFS/PIPE: PD-owned bounded stores (8×4 KiB files, 4×1 KiB rings);
  no staging, behavioral parity only.
- DEVFS: static metadata table; data ops are `-ENOSYS` (device authority
  stays kernel-side — D3).
- PROCFS: kernel renders via its own VnodeOps into the window; the PD
  validates format + digest (D6 validator, never renders).

Per-op staging (not bind-time): the 2.3 MiB initrd image cannot fit a
16 KiB window. The cpio index is cached kernel-side per epoch; the
PD keeps no cross-op image state except its open-handle table (cleared
on epoch bump). Single RPC per op; the kernel clamps reads to
`min(size, file_size - offset)` so short final chunks terminate the
digest loop deterministically.

## Lifecycle

PID cell + epoch + tombstone ring (same shape as timer-srv):
`on_restart` stores the PID and bumps the epoch; the server purges
its open-handle table on epoch change. Replies validate (server,
epoch, owner id+gen); mismatch is dropped, never consumed. A dead PD
is restarted by the supervisor; the next op rebinds (CAP_GET re-maps
the fresh window — the PD always unmaps + remaps on convey, never
trusts a stale VA; a null window fails closed with `-ENOSYS`, never
faults). `sdf_policy_only` still holds: the VFS_SRV row carries only a
memory DMA_REGION window (no MMIO/IRQ/DMA-device caps); the compose
gate rejects device kinds for policy roles.

## Bounded wait

`IPC::send_sync_bounded` with `kRpcGuardTicks` (5000). Expiry returns
`-EAGAIN`; never a silent success, never a held block. Single in-flight
claim per proxy (fail-fast `-EAGAIN`, no spin across reschedule).

## Pre-flip status

`src/kernel/vfs/` stays compiled (no removal); `vfs_srv_live`
14/14/14 on x86_64/aarch64/riscv64 ON builds
(`vfs_srv_bind_backends`, `vfs_srv_cap_rounds`, initrd stat/read,
tmpfs roundtrip/guards, devfs nodes, pipe roundtrip/bounded, fat32
geometry/read, ramdisk extent, procfs digest, fault/epoch/restart),
host FNV-1a digests match per backend. OFF (default) keeps the
in-kernel paths: `storage` 167/167 ×3 arches. `test_vfs_srv.cpp` and
its registry wiring land via the testbed lane (D8a); only the kernel
proxy + PD + docs ship on main.
