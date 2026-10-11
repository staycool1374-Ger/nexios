# XMODEM/YMODEM Receiver (xmodem) — issue #276

Userspace program (`userspace/xmodem.c`, `bin/xmodem.c.elf`) receiving
files over the serial stream into ramdiskd. Single-process (no fork —
#312 denies `fork()` from runelf'd tasks). Zero kernel changes.

## Protocol

- Receiver-initiated `C` handshake (up to 10× every 3 s, CRC-only —
  no checksum/`NAK` fallback); sender-initiated streams accepted if a
  valid `SOH`/`STX` arrives in-window.
- Packets: `SOH` 128 B / `STX` 1024 B + seq + complement + data +
  CRC-16/CCITT-FALSE big-endian (`poly 0x1021`, `init 0x0000`;
  `"123456789"` → `0x31C3`, shared header `userspace/xmodem_crc.h`,
  verified by `lib_crc16_*` kernel tests).
- Per-packet: seq/complement + CRC checked; mismatch → `NAK`, 10
  retries then `CAN CAN` abort; duplicate of last ACK → re-`ACK`;
  wrong seq → abort; `CAN CAN` → silent abort; `EOT` → `ACK`, done.
- YMODEM block 0 (`seq 0` first): `filename\0size_ascii\0`; size bounds
  the stream (bytes beyond size dropped); no block 0 → pure XMODEM
  until `EOT`. Final partial block zero-padded (`0x1A` stripped only
  via YMODEM size).
- Timeouts: 1 s alarm slices per byte (poll + `nanosleep`), 10 s per
  packet, 600 s overall cap. Every wait bounded; fail-closed abort
  (`CAN CAN`, nothing further written).

## Framings (interactive `mode`)

- `uart`: raw binary. `base64`: quartets (whitespace skipped, `=`
  end-padding only, bad alphabet aborts; decoded output clamped).
  `hex`: pairs (whitespace skipped, odd tail aborts).

## Ramdisk sink

Stream bytes fill 512 B staging blocks from `start block`; each full
block goes out as 16×32 B chunks (opcode 401, client pid packed first
per the ramdiskd wire); `EOT`/size-end flushes the partial. First
non-OK (incl. `NOGRANT`) aborts. `dump` reads blocks back (opcode 400)
and emits raw bytes (hex per byte in `hex` mode) for host compare.

## Use

`loadelf /bin/xmodem.c.elf` + `runelf`, answer `mode/cmd/pid/block`
(`cmd` loops; `quit` exits). E2E: `tools/verify-276.py` (TCP serial,
golden byte-compare all framings + NAK-recovery + NOGRANT).

## Non-goals

YMODEM-G, sender role, checksum fallback, multi-file batch, `loadelf`
from ramdisk (needs VFS + quiescence), atomicity/resize/zero-copy.

## Operator upload (`tools/upload-ramdisk.py`, issue #313)

One command wrapping the proven `tools/verify-276.py` driver:

```
tools/upload-ramdisk.py FILE --start-block 100 [--pid N] [--port 4465]
                             [--packet soh|stx] [--framing auto|raw|base64|hex]
```

- Attaches to a running target (boots nothing itself); discovers the
  ramdiskd pid from `tasks` unless `--pid` is given; drives the receiver
  via `source /tests/xmodem276.txt` + the `mode:/cmd:/pid:/block:`
  answers (`rx` path only).
- Framing `auto` (default): file bytes are scanned once — any `0x0A`
  or `0x09` selects `base64`, else `raw`. Framing is transport-only:
  the receiver decodes it before staging, so the stored bytes are
  always the raw file bytes and `done bytes=` equals the file size
  (`--framing` overrides; `hex` also accepted).
- `--start-block` is required (no silent default: a wrong block
  overwrites live ramdisk data). Pick a region known free (e.g. 100
  in the verify-276 layout); capacity beyond the 16 MiB store is
  refused before dialling. Empty files are refused (the receiver
  NAKs zero-size YMODEM).
- Progress, both ends: host prints one updating `\r` line
  (`[up sent/total pct% rate ETA naks spill]`) plus a final
  `upload done bytes=.. blocks=.. framing=.. naks=.. spill=..` line
  verified against the receiver's `done bytes=`; the receiver prints
  `[rx n=<done> total=<size>]` every 16 blocks (`total=0` for
  pure-XMODEM senders without block-0). Good output shows steady
  `\r` updates and zero unexpected resends; a `CAN-abort` with no
  `done` line means the receiver refused (grant/pid/block — check
  the serial log; the receiver writes nothing further after abort).
- Exit codes: `0` ok, `1` dialogue/handshake failure, `2`
  usage/byte-count mismatch, `3` receiver CAN-abort, `4`
  timeout/retry-exhausted. `--selftest` runs the host-side checks
  without a target.
