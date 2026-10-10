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
| Superblock | 0 | magic `RDS1FLAT`, version 2, file_count, next_data_block, dir_crc32 |
| Directory | 1–10 | 50 `RdDirEntry` (96 B each, serialized contiguously; CRC32-protected) |
| Data | 11+ | file extents (bump-allocated, 512 B units) |

`RdDirEntry`: name[64] (empty name = free slot), size, start_block,
block_count, crc32 (finalized CRC32 over the file's live bytes),
flags (`RD_ENTRY_VALID`). Per-file cap 1 MiB. No free list (unlinked blocks
leak until reformat — documented non-goal), no journaling, no resize.

### 2.1 Integrity — metadata CRC32 + carve scrub (issue #318)

- **Metadata region CRC.** The superblock's `dir_crc32` is the CRC32 over the
  serialized directory region; `rd_load_all` recomputes and rejects the whole
  image (`-3`) on mismatch — a corrupt directory is discarded fail-closed.
- **Per-file data CRC.** `crc32` covers the file's live byte range only (block
  padding excluded), computed at close (`rd_flush_entry`) via
  `src/lib/crc32.hpp`, persisted with the entry. Any content write clears
  `RD_ENTRY_VALID` (in-flight — a torn write is never exposed); close
  recomputes and re-sets it.
- **Verification.** After a fresh cache fill, `rd_verify_cached_entries()`
  re-reads each `VALID` entry and clears `VALID` (fail-closed) on mismatch —
  this is the boot-time integrity check. `rd_lookup`/`rd_readdir` expose only
  `VALID` entries; `ramdisk_fs_verify_all()` is the test seam (returns the
  mismatch count and fails the entry closed). Lazy (per cache fill, not a full
  16 MiB mount scan) so boot stays free of hundreds of thousands of chunk
  round-trips; corruption after a fill is caught by `ramdisk_fs_verify_all()`
  or the next fill (daemon restart / remount).
- **Pristine-zero contract.** The carve scrubs every segment to zero
  (`ramdiskd::boot_allocate`, HHDM memset at carve — not at grant, else a
  regrant would wipe the store). A never-staged block therefore reads exactly
  zero deterministically (no stale-code disclosure). This also makes a
  foreign-frame aliasing fault detectable: the content would not match the
  stored CRC / the zero baseline.
- **Legacy/version mismatch.** A non-v2 image is discarded fail-closed
  (no free list ⇒ not resumable); the version bump is the migration story.
- CRC is corruption/early-detection only — **not** security (no adversary, no
  crypto).

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

`rd_block_op` / `rd_block_op_retry` loop the normative 16×32 B chunk
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

- Kernel: `test_ramdisk_fs` (7 tests in the `storage` aggregate):
  mount presence, create/write/read/delete round-trip, bounds rejection,
  partial-overwrite behavior, readdir listing, loadelf-from-ramdisk
  acceptance (`request_load` → OK + `request_cancel` → OK, no execution),
  and CRC-detects-corruption (single-byte corruption → verify mismatch →
  fail-closed resolve). `test_ramdisk` also adds `ramdisk_pristine_reads_zero`
  (carve-scrub contract, issue #318).
- Live: shell file ops (`touch`/`echo >`/`cat`/`ls`/`rm`) + YMODEM
  `rxfile` upload + `loadelf`/`runelf` from `/mnt/ramdisk`.
- Open follow-ups: #317 (send_sync reply contract), #318 (carve
  content), #319 (fast wall clock under q35+TCG).
