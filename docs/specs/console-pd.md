# Console PD — emulation policy (issue #51 items 2/4)

`userspace/fb-pd.c` (+ `font8x16.h`, `driver_pd.h` console section) is
the framebuffer **emulation** PD: #48 owns the device cap, #51 owns
what the pixels mean. Zero IRQ by design (1-tick recv poll when the
ring is mapped, block-forever before MAP).

## Wire (`DriverRpc`, same 48 B budget as the serial PD)

| Op | Payload | Effect |
|---|---|---|
| `BIND` (0) | arg0 = scanout MmioCap handle, arg1 = phys, arg2 = bytes, flags = chunk index | `SYS_MMIO_MAP` once per chunk (idempotent re-BIND, changed handle fails closed) |
| `BIND_FRAME` (9) | same shape, frame handle | `SYS_FRAME_MAP` once per chunk (RAM-backed scanouts) |
| `CONSOLE_MODE` (8) | arg0 = pitch, arg1 = width, arg2 = height, flags = bpp | geometry + clamped text grid + clear; re-publishes on MAP_STATUS too (bind order is a non-contract) |
| `MAP_SHM` (6) | arg0 = ring FrameCap handle, arg1 = pages, arg2 = 8 (element size) | `SYS_FRAME_MAP` + v2 header validation (magic/version/pow2/esz), idempotent re-MAP |
| `MAP_STATUS` (7) | arg0 = status FrameCap handle, arg1 = pages | `SYS_FRAME_MAP`, zeroed (no stale counters), then geometry + map flags published |
| `PRESENT` (5) | — | bounded ack (host flush stays kernel-side) |

Replies mirror the serial PD (`op`/`epoch`/`owner` echoed, `arg0 = 1`
on success, typed `-E*` status on failure, sent to `req.owner_id`).

## Render loop

`console_drain` runs before every recv: pop while `cells <
CONSOLE_CELLS_PER_PASS` (320), `put_char` per byte (`\n`/`\r`/`\b`
handled, circular text grid — scroll repaints only the recycled
band, never a full-screen copy), then publish `rendered_cells` /
`pass_max_cells` / `passes` / cursor. The bound is enforced by
**peeking**: `ring_peek_len` reads the pending length without
consuming; an element that would cross the budget stays queued for
the next pass (pop-first would silently drop the element tail —
convicted by `fb_bounded_render` at 896/900 before the fix). Quiesce
(`drain_hold` nonzero) skips the drain entirely so producer drop
accounting stays exact.

Chunk math: `off / CONSOLE_MMIO_CHUNK` (== `CONSOLE_FRAME_CHUNK` ==
2 MiB; both mirror their kernel window constants with static_asserts
in `boot_sdf.hpp`) selects the chunk, `off % CHUNK` the intra-chunk
offset — per-byte resolution, so no pixel ever straddles a boundary
wrong. Out-of-range bytes are dropped (fail-closed render).

## Status page (`struct console_status`, 15 × `u64`)

`rendered_cells`, `pass_max_cells`, `passes`, `rejected`,
`dropped_full`, `mode_w/h`, `pitch`, `bpp`, `cols`, `rows`,
`cursor_row`, `scanout_mapped`, `status_mapped`, `ring_mapped`.
Header-free (zeroed at MAP); the kernel reads it through the HHDM
alias with acquire loads. `pass_max_cells` is the exact
bounded-operation evidence (`fb_bounded_render` asserts it never
exceeds `CONSOLE_CELLS_PER_PASS`).

## Non-ownership

FRAME pins (`FrameCap::create_pinned`) wrap boot-lifetime backing:
dispose revokes the dead PD's mappings but never frees the frames
(the device stays programmed to them; the next epoch re-wraps). The
mint fails closed on any non-user page in the span — wrapping kernel
RAM would hand the PD supervisor memory.
