# vfs-srv — VFS Server PD Contract (issue #47, strangler step a)

Userspace VFS server behind `CONFIG_VFS_SRV` (default 0). Step-a scope:
PD task + SDF row + Kconfig OFF shim + loopback/parity + fault drill.
No flip: authorize stays kernel-side (`syscall_handlers_fs.cpp`,
`vfs.cpp`, `vfsd.c` untouched).

## Wire

48 B `VfsRpc` (`kernel/vfs/vfs_srv.hpp`, mirrors `TimerRpc`): op/flags/
epoch/owner id+gen/ino/offset/size/status-op/status. Ops: BIND,
INITRD/FAT32/TMPFS/DEVFS/PIPE/RAMDISK/PROCFS_RENDER (probe only),
INJECT_CORRUPT (one-shot drill). Server answers BIND; every other op
returns a typed error pre-flip (`-ENOSYS`, corrupt probe `-EIO`).

## Lifecycle

PID cell + epoch + tombstone ring (same shape as timer-srv):
`on_restart` stores the PID and bumps the epoch; the server purges
all state on epoch change. Teardown appends (owner id, gen) to a
bounded (16) lock-free ring; the next RPC piggybacks CANCEL_OWNERS.
Replies validate (server, epoch, owner id+gen, op echo); mismatch is
dropped with a bounded re-wait, never consumed.

## Bounded wait

`IPC::send_sync_bounded` with `kRpcGuardTicks` (5000). Expiry returns
`-EAGAIN`; never a silent success, never a held block. No IRQ window
widening vs `send_sync`.

## Pre-flip status

80-83-style VFS RPC numbers do not exist yet; all POSIX VFS traffic
still runs the in-kernel path. The flip (real backend serving,
fd-table handoff design, revoke cascade) is a later step with the
§7 gate (audit + test-full debug AND release + delta-0). OFF
(default) keeps the in-kernel registry: `storage` 167/167 + `vfs_*`
+ `proc_elf` + shell E2E oracles bit-identical.
