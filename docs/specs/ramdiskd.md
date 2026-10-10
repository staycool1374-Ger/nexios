# Ramdisk Service (ramdiskd) — issue #275

Userspace `SPORADIC_SERVER` daemon (`userspace/ramdiskd.c`, `bin/ramdiskd.c.elf`)
serving fixed 512 B blocks over chunked IPC. Kernel side:
`src/kernel/ramdisk/ramdiskd.{hpp,cpp}`.

## Opcodes (400-series)

| Op | Value | Direction |
|---|---|---|
| `RAMDISK_READ_BLOCK` | 400 | client → daemon |
| `RAMDISK_WRITE_BLOCK` | 401 | client → daemon |
| `RAMDISK_STAT` | 402 | client → daemon |
| `RAMDISK_GRANT` | 403 | kernel → daemon (one-way, no reply) |

## Wire format (64 B IPC payload)

`sys_receive` copies ONLY `msg.data[]` into the userspace buffer and
returns `msg.type` (`sender_id` is never delivered — same contract as
debugd). The client therefore packs its own pid first: request data[]
= `sender_pid:u64` + `block_no:u64` + `chunk_idx:u64` (24 B) + payload
(32 B) = 56 B; reply data[] = `result:i64` (8 B) + payload (32 B) =
40 B. Replies go to the data-packed pid with type 0. Kernel `send_sync`
sender-matches, so no manual drain is needed (defense in depth only).

One 512 B block = 16 chunks × 32 B (`RAMDISK_CHUNKS_PER_BLOCK`,
`RAMDISK_CHUNK_DATA`). Requests/replies carry exactly one chunk; clients
loop 0–15 per block. Chunk data rides in the request (write) or the reply
(read); abort fail-closed on first error (torn blocks visible — see
non-goals).

Error codes: `OK=0`, `RANGE=-1`, `NOGRANT=-2`, `NOMEM=-3`, `UNKNOWN_OP=-4`.
All bounds (`block < 32768`, `chunk < 16`) fail closed; reachable IPC
input never panics.

## Capabilities

Backing store: 8 × 2 MiB segments (one `FrameCap` each — a 16 MiB cap
cannot map: `FrameUserMap` regions are 2 MiB). Block B lives in segment
`B / 4096` at `(B % 4096) * 512 + chunk * 32`.

Lifecycle: kernel carves early post-PMM/pre-probe (`boot_allocate`),
exact-retypes each segment into the daemon CSpace (`READ|WRITE`) at grant
(`grant_storage`), and delivers 8 handles via `RAMDISK_GRANT`. The daemon
maps each handle with `SYS_FRAME_MAP` into its 2 MiB windows. Daemon death
frees frames via normal slot disposal; re-grant re-carves + re-mints on
both restart paths. No new cap type, no new syscall, no new lock.
**Carve scrub (issue #318):** `boot_allocate` zeroes each freshly carved
segment (HHDM memset) so a never-staged block reads exactly zero — the
pristine-zero contract for `ramdisk_fs`. Scrub happens at *carve*, not at
grant (a grant-time scrub would wipe the store on every regrant).

## Boot / degraded / reboot

`DaemonWatch` stays vfsd/iocd/watchdogd only — boot never blocks on
ramdiskd. The grant runs post-rendezvous via the PID cell; any failure
logs + dmesg (`DAEMON+10`) and continues degraded (daemon answers
`NOGRANT`). `reboot_from_table` picks the taskdefs row up automatically.

## Non-goals (follow-ups)

- loadelf/runelf from ramdisk (needs VFS integration + checksums or
  quiescence — torn 512 B writes are visible to concurrent readers).
- XMODEM transport (sub-issue #276). Multi-block atomicity, resize,
  zero-copy ring (#106 Part B) replacing chunked IPC.
