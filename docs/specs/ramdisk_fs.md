# Ramdisk Filesystem (ramdisk_fs) — issue #314

Userspace `SPORADIC_SERVER` block store (#275) mounted into the VFS as a
real filesystem at `/mnt/ramdisk` (`src/kernel/vfs/ramdisk_fs.{hpp,cpp}`).

## 1. Why a flat store (not FAT)

Owner-directed v0.5.3 scope decision (see #314): reusing the FAT32 backend
pulls allocation-table/FAT-chain machinery that dwarfs the need (upload a
few ELFs, run them). A flat file store — fixed directory region + extent
list over ramdisk blocks — is sufficient, auditable, and leaves a clean
seam where the FAT backend replaces it later (same `VnodeOps` surface,
same mount point, same `/mnt/ramdisk` path).

## 2. On-disk layout (all named constants in ramdisk_fs.cpp)

| Region | Blocks | Content |
|---|---|---|
| Superblock | 0 | magic `RDS1FLAT`, version 1, file_count, next_data_block |
| Directory | 1–9 | 50 `RdDirEntry` (88 B each, serialized contiguously) |
| Data | 10+ | file extents (bump-allocated, 512 B units) |

`RdDirEntry`: name[64] (empty name = free slot), size, start_block,
block_count. Per-file cap 1 MiB. No free list (unlinked blocks leak until
reformat — documented non-goal), no journaling, no resize.

## 3. Concurrency (§11) and caching

- The FS mutex is NEVER held across block IPC (`IPC::send_sync`
  reschedules; the daemon never takes this lock, so no wait cycle exists).
- Mutations snapshot under the lock, run IPC unlocked, then re-validate
  an epoch counter (bounded 3 retries) before committing.
- The directory region is cached in memory (epoch-guarded); the cache is
  keyed on the daemon pid, so a daemon restart transparently invalidates
  it (no snapshot-restore hook needed — backing blocks persist like
  daemon CSpaces, and MemPool-owned vnodes/extents rewind safely).
- Sizing/superblock updates accumulate in cache and flush on close
  (publish-on-close); concurrent same-file writers are NOT supported
  (single-writer uploader discipline).

## 4. Block layer

`read_block_full` / `write_block_full` loop the normative 16×32 B chunk
IPC (`sender_pid` + `block_no` + `chunk_idx` header, `RAMDISK_OK` checked
per chunk — same wire as `ramdisk_live_roundtrip`), with bounded retries
(cold lazy segment maps can fail transiently under TCG). Partial edge
blocks use read-modify-write. Reachable failures return `VFS_INVALID`
(fail-closed); `pid == 0` (ungranted) fails before any IPC.

## 5. Torn-image safety (no rename syscall exists)

There is no rename/link syscall in the ABI, so temp-name + atomic rename
is unimplementable without an ABI change (out of scope). The safety
chain is instead:
1. The uploader (`rxfile` in userspace/xmodem.c) writes the declared
   YMODEM size, closes, re-reads and byte-verifies, and prints
   `done file=<name> bytes=<N> verified` ONLY on match; any failure
   unlinks the partial file (fail-closed, nothing torn left behind).
2. The ELF loader refuses truncated images independently
   (`validate_header` + VULN-H2 segment-bounds checks against file size).
3. Single-writer uploader discipline (no concurrent mutation window).

## 6. Mount / degraded behavior

- `ramdisk_fs_try_mount()` mounts at `/mnt/ramdisk` (best-effort,
  fail-closed when ungranted; idempotent). Called after the ramdisk
  grant in `init_task_main` and from `reset_and_remount()` (never blocks
  boot or tests).
- Formatting is lazy: an unformatted store mounts an empty view and
  formats on first create (keeps boot-time mount free of block IPC).
- `loadelf`/`runelf` need zero loader changes (path-agnostic `resolve`).

## 7. Uploader bridge (deviation note)

The plan placed a file-ness bridge in the shell; implemented instead in
`xmodem.c` (`rxfile` command): only xmodem holds the byte stream, the
YMODEM block-0 name/size, and the size to verify against, and userspace
`open`/`write`/`read`/`close`/`unlink` syscalls already exist — no kernel
or shell changes needed. Raw `rx` block staging is untouched (the
`verify-276.py` gate stays green).

## 8. Verification

- Kernel: `test_ramdisk_fs` (6 tests in the `storage` aggregate):
  mount presence, create/write/read/delete round-trip, bounds rejection,
  partial-overwrite behavior, readdir listing, loadelf-from-ramdisk
  acceptance (`request_load` → OK + `request_cancel` → OK, no execution).
- Live: shell file ops (`touch`/`echo >`/`cat`/`ls`/`rm`) + YMODEM
  `rxfile` upload + `loadelf`/`runelf` from `/mnt/ramdisk`.
- Open follow-ups: #317 (send_sync reply contract), #318 (carve
  content), #319 (fast wall clock under q35+TCG).
