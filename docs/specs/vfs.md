# VFS Subsystem Specification

**Semantics:** binding contract for the virtual-filesystem layer: the vnode
model, mount model, the vfsd authorization protocol, path resolution with
TOCTOU closure, the fd-table, and the per-filesystem backends.  This is the
normative VFS spec (previously a documented gap).  All symbol names verified
against the current tree.

```
 Ring-3 / kernel task
        │  syscall (OPEN=9, READ=10, ..., RMDIR=43)
        ▼
  Syscall::handle
        │
        ├─ fd-ops (read/write/close/fstat/dup/dup2)
        ├─ path-ops (open/stat/chdir/mkdir/unlink/rmdir)
        └─ resolve_then_authorize / resolve_parent_then_authorize
              │  resolve → capture ino → vfsd_authorize (IPC) → re-resolve
              │  ── compare pointer + ino (TOCTOU closure, VULN-C4)
              ▼
        VFS CORE (vfs.cpp): resolve(), resolve_parent(), FdTable, vnode_ref_*
              │  IPC::send_sync to vfsd (blocks: BLOCKED + dequeue + reschedule)
              ▼
        vfsd daemon (SS prio 20): vfsd_dispatch() — OPEN/CLOSE/STAT/FSTAT
              (READ/WRITE are kernel-side after authorization)
              ▼
        VnodeOps dispatch (per-fs ops tables)
   ┌───────┬───────┬───────┬─────────┬────────┬─────────┐
   ▼       ▼       ▼       ▼         ▼        ▼         ▼
 initrd  devfs   procfs  tmpfs    fat32    pipe      blocking (tty/kbd)
 "/"     "/dev"  "/proc"  "/tmp"   (lazy)  (anon)    BLOCKED+resched
```

## 1. Vnode Model

### 1.1 `struct Vnode` (vfs.hpp)
```cpp
struct Vnode {
    const VnodeOps *ops;     // per-filesystem dispatch table (may be nullptr entries)
    uint64_t ino;            // per-fs namespace, NOT globally unique
    uint64_t size;
    uint16_t mode;           // S_IFREG | S_IFDIR | S_IFCHR
    void *private_data;      // fs-specific (cluster / page phys / PipeBuffer / PidDirVnode)
    uint64_t refcount;       // atomic (see §6)
    Vnode *parent;           // for `..`
};
```

### 1.2 `VnodeOps` dispatch table
`read` / `write` / `open` / `close` / `lseek` / `fstat` / `ioctl` / `readdir` /
`lookup` / `mkdir` / `unlink` / `create`.  Absent methods are `nullptr`; callers
must null-check before dispatch.  There is **no** `getattr`/`rename`/`truncate`.

### 1.3 Vnode storage & `close` ownership
| Type | Storage | `close` behavior |
|---|---|---|
| file (initrd/fat32/tmpfs) | MemPool | frees `private_data` + vnode |
| dir (fat32 child) | MemPool | frees + `MemPool::free` |
| fat32 root / tmpfs root / initrd root | static | resets `refcount=1`, never freed |
| devfs / procfs static | static globals | no-op |
| procfs pid-dir | MemPool | frees `PidDirVnode` |
| pipe read/write | MemPool | frees `PipeBuffer` when shared refcount→0 + vnode |

**Two-part free model:** the vnode struct (MemPool or static) and `private_data`
are freed by the fs-specific `close` — `close` is the owner-only free path.

### 1.4 Lookup ownership (issue #268)
- `lookup()` / `resolve()` / `resolve_parent()` results are **OWNED** — the
  caller releases exactly once via `vfs::release()` when done (fd attach,
  cwd adoption, and `resolve()` traversal intermediates excepted — see below).
- `release()` is zero-safe: refcount-0 vnodes (tmpfs cache entries, static
  roots, devfs statics) are borrowed and never freed by it; never call
  `ops->close` directly (except `FdTable::free`, which owns the fd ref).
- `resolve()` frees owned intermediates internally; mount roots / cwd /
  the global root are borrowed during traversal. Every `resolve()` return
  is owned (borrowed finals are inc'd on exit), including `"/"` and `"."`.
  `resolve_parent()` bare-name case inc's cwd for the same guarantee.
- Transfers (no extra ref): fd attach consumes the resolve ref
  (`syscall_task_open` takes it; one `vnode_ref_inc` per ADDITIONAL fd
  sharing the vnode, e.g. stdio triple); cwd adoption consumes it
  (alias-safe: same-dir `cd` drops only the fresh ref).
- Correction to §1.3: tmpfs file `close` is a no-op — cached vnodes free
  only at `unlink` (open-but-unlinked lifetime is a known follow-up, not
  covered here). tmpfs `lookup` returns the borrowed cache entry.
- UP assumption: no VFS refcount traffic in IRQ context (documented at
  `vfs::release`); SMP will need a locked/RCU reclamation pass.

## 2. Mount Model

```cpp
struct Filesystem { const char *name; Vnode *(*get_root)(); };
struct Mount { const char *mount_point; Filesystem *fs; Vnode *root_vnode; bool used; };
static Mount mount_table[MAX_MOUNTS /*32*/];   // no unmount API
```
- Registration = the static `Filesystem` globals (`initrd`, `devfs`, `procfs`,
  `tmpfs`, `fat32`); `find_fs(name)` scans the mount table (unmounted = unfindable).
- First mount (`mount_count==1`) sets `root_vnode_global`.  Boot order
  (`reset_and_remount`): `/` initrd, `/dev` devfs, `/proc` procfs, `/tmp` tmpfs.
  `fat32` is mounted by the shell/init via `mount_fat32()`.
- `mount_point` is stored **by pointer** — callers must pass string-literal /
  static storage.
- **fs-instance identity:** a mounted instance is identified by its `Mount`
  entry + `root_vnode` pointer (no explicit instance field on `Vnode`).

## 3. vfsd Message Protocol

- Ops: `VFS_OPEN=100..VFS_RMDIR=112`; `struct Msg` is exactly
  `IPC_MAX_MSG_SIZE` (64 bytes): `{sender_id, type, arg0, arg1=ino, path[32]}`.
  `struct Reply {int64_t result; uint64_t data0..data3;}`.
- **Authorization transport** (`vfsd_authorize`): bypass for kernel tasks and
  the vfsd itself; **`vfsd_pid==0` ⇒ fail CLOSED** (all VFS syscalls return -1
  until `daemon_mgr` restarts vfsd).  Request must carry the caller pid; reply
  accepted iff `data_size >= sizeof(Reply)` and `result >= 0`.
- Daemon side: OPEN/CLOSE perform a real open+close (authorization-by-doing);
  STAT/FSTAT return `st_size`/`st_mode`; **READ/WRITE are authorization-only
  stubs** — the kernel performs the real I/O.  Boot handshake:
  `MSG_DAEMON_READY` → PID 1; init waits ≤ 500 ticks for vfsd+iocd.
- vfsd TaskDef: SS prio 20, budget 2 / period 10 (~20% WCET), granularity 1.

## 4. Path Resolution (TOCTOU closure)

```
vn  = resolve(path);                // 1. resolve FIRST
ino = vn->ino;
vfsd_authorize(op, pid, path, ino); // 2. blocking IPC (CPU yields to vfsd)
vn2 = resolve(path);                // 3. re-resolve
return vn2 && vn2 == vn && vn2->ino == ino;   // 4. identity = pointer + ino
```
- Longest-prefix mount selection for absolute paths; relative paths start at
  `cwd_vnode`; `..` at a mount root climbs by re-resolving the mount point's
  parent.  Mount-point shadowing: a path component equal to a mount point's
  last component jumps to that mount root.
- **Resolve returns UNHELD vnodes** — no ref during traversal; only fd/cwd
  holders keep a vnode alive.
- **GAP (open):** `MAX_PATH_DEPTH=16` is declared but **not enforced** — no
  runtime depth cap in `resolve()`.

## 5. FdTable Contract

```cpp
struct FileDescription { Vnode *vnode; uint64_t offset; uint64_t flags; bool used; };
struct FdTable { FileDescription fds[MAX_FDS /*32*/]; };   // task.hpp
```
- `alloc()`: lowest free slot, `VFS_INVALID`/`VFS_ERR_FD_TABLE_FULL` when full.
- `free()`: bounds-check → `vnode_ref_dec`; on 1→0 → `ops->close`.
- `dup`/`dup2`: struct-copy the whole `FileDescription` (**dup shares the
  offset**, POSIX-compliant) + `vnode_ref_inc`.  `dup2` frees the target first.
- `cwd`: `cwd_vnode` + `cwd[256]` guarded by `cwd_lock_` (spinlock, VULN-C5/C6).
- **Invariant:** `syscall_task_open` does **not** `vnode_ref_inc` — the fd slot
  is the single implicit reference, balanced by `FdTable::free`'s dec→close.

## 6. Refcount Rules (binding)

```cpp
vnode_ref_inc(vn):  __atomic_fetch_add(&vn->refcount, 1, RELAXED)
vnode_ref_dec(vn):  return __atomic_fetch_sub(&vn->refcount, 1, ACQ_REL) == 1;  // 1→0 ⇒ owner must close
```
- Zero-check uses the **returned previous value** of `fetch_sub` — never a re-read.
- `close()` runs exactly once, by the owner observing the 1→0 transition.
- **Seeding asymmetry:** dynamic vnodes start `refcount=1`; static vnodes start
  `0` (they are never `MemPool::free`d).  Any future vnode cache must preserve
  this.

## 7. Blocking / WCET

- vfsd authorize blocks the caller via `IPC::send_sync` (BLOCKED + `dequeue_ready`
  **before** state=BLOCKED; then `reschedule()` + `hlt` loop).  No spinning.
- `tty_read`/`kbd_read` (VULN-W2): poll → `O_NONBLOCK` check → else
  `state=BLOCKED; reschedule()` — no `UINT64_MAX` spin.
- `pipe_read` blocks on `data_avail.wait()`; pipe write returns partial count.
- vfsd SS envelope: budget 2/period 10 ⇒ replies within ~2 ticks or drops to
  background priority; kernel authorize waits add ≤ ~1 server period latency.
- Bounded data: tmpfs files cap at 64 KiB; write bounce buffer caps at 1 MiB.

## 8. Filesystem Backends (one-line semantics + invariants)

| FS | Semantics | Key invariants |
|---|---|---|
| initrd | read-only boot image at `/` | read-only ops; `ino`=0 root, 1 files; vnode lifetime == fd/cwd |
| devfs | static char devices at `/dev` (tty/null/console/kbd/random) | never freed; `open` stores flags in `private_data` |
| procfs | dynamic pseudo-fs at `/proc` (meminfo/self/pci/PID-dirs) | pid-dirs allocated on lookup, freed by `pid_dir_close`; static nodes reused |
| tmpfs | in-memory tree at `/tmp`, files = 16 contiguous PMM pages (64 KiB) | `tmpfs_lock` global; `unlink` refuses non-empty dirs; lazy page alloc on first write |
| fat32 | on-disk via BlockDevice, 8.3 names, cluster chain | read-only file data; root vnode static, re-init on partition change; EOF ≥ 0x0FFFFFF8 |
| pipe | anon FIFO: 4 KiB ring, shared `PipeBuffer` (refcount 2) | buffer freed at shared refcount→0; read blocks on `data_avail` |

## 9. Test-Isolation Contract

- VFS-mutating entry points call `mark_vfs_touched()`; `snapshot_restore`
  restores `vfsd_pid` and re-mounts via `reset_and_remount()` + `tmpfs_reset_root()`.
- ResourceTracker counts `track_vnode_*` / `track_fd_*` / `track_pipe_buffer_*`
  — new kernel resource types must add counters.

## 10. Gaps

- **No runtime path-depth cap** (`MAX_PATH_DEPTH` unused).
- **tmpfs `next_ino` is a module-global** — not snapshot-restored (content is
  rewound, numbering is not).
- **`sys_receive` timeout (VULN-W3)** — verification status unconfirmed
  (see `specs/boundary.md`).
- **Boundary audit VULN-H2/H4/U2** (ELF size guard, argv/envp cap, stack
  reserve) are IMPLEMENTED — see `specs/boundary.md` ledger.
