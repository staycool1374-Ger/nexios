# Cache Coloring Allocator

**Doc ID:** NEX-SPEC-2026-09-15-001
**Status:** IMPLEMENTED (issue #62)
**Milestone target:** v0.4.5 (Per-CPU Scheduling & Cache)
**Related:** `docs/specs/memory.md` §5 (owner-bit contract),
`docs/specs/per-cpu-smp.md` §3.4.9, `src/kernel/memory/cache_color.hpp`,
`src/kernel/memory/pmm.hpp` (colored path).

## 1. Contract

A page's color is the cache-set index bits above the page offset:

```
color_of(phys) = (phys >> COLOR_SHIFT) & COLOR_MASK
```

with `NUM_COLORS = 16`, `SHIFT = 12`, `MASK = 15` (bits 15..12).
The geometry is a QEMU-contracted convention — no observable silicon
cache exists under virtualization — retunable via the three constexprs
in `cache_color.hpp`. No silicon truth is claimed; no perf claims are
made beyond relative scaling (colored spread vs uncolored clumping).

## 2. API

| Entry | Ownership | Failure |
|---|---|---|
| `PMM::color_of(phys)` | — (pure) | n/a (any u64) |
| `PMM::alloc_page_colored(color)` | KERNEL | 0 + OOM path |
| `PMM::alloc_user_page_colored(color)` | USER | 0 + OOM path |
| `*_colored_err(color, out&)` | as above | `PMM_ERR_OOM` / `PMM_ERR_USER_OOM` |
| `PMM::reset_color_cursor()` | — (test hook) | n/a |

Colored alloc is single-page-only: colored-contiguous (same color AND
physically contiguous) is over-constrained and rarely satisfiable;
multi-page callers keep using `alloc_contiguous`. Out-of-range colors
fail closed (0 / OOM code, never panic on reachable input).

## 3. Implementation (PMM extension, not a pool)

Colored allocation lives inside PMM under `pmm_lock_` (no second lock,
no separate pool — a wrapper pool would desync bitmap vs free-list vs
ResourceTracker vs snapshot rewind, the S1 the design exists to avoid):

- `try_alloc_colored_kernel/user(color)`: color-strided bitmap scan
  `[window_base_page_, window_end_page_)` from the per-color cursor
  (page index `i` has color `i & MASK` because SHIFT == page shift),
  wrapping once for completeness; `bitmap_set` + `owner_set_*` +
  `--free_pages_`, cursor advanced past the hit, then
  `rebuild_free_list()` re-syncs the free list the scan bypassed.
- Public wrappers mirror `alloc_page`/`alloc_user_page` exactly:
  `CONFIG_STATIC_POOLS_ONLY` gate, `CONFIG_MEMORY_BUDGET`
  accounting, `ResourceTracker::track_pmm_alloc`, and the
  unlock→`oom_handler_`→relock retry protocol.
- `free_page` is shared (no color logic); owner bits keep the
  `free_user_pages` contract intact.
- Cursors are placement-only state (never correctness): tests reset
  them at entry via `reset_color_cursor()`; no snapshot capture.

The colored path is a creation/test-time path: the strided scan plus
rebuild is not RT-budgeted. Tick paths (`on_tick`, `scan_deadlines`,
`balancer_tick`) allocate nothing.

## 4. Tests (stub→real, names kept)

`test_cache_coloring.cpp` (was: 4 PENDING stubs from #85 module 10):
`spread` (cycling requests return exact colors, adjacent differ,
net-zero), `collisions_bounded` (uniform spread keeps the stub's
`collisions*COLORS <= M*(M-1)/2 + M` formula green, per-color exact),
`arbitrary_sizes` (one page per size class, range + stability),
`bit_extraction` (fixed phys vector incl. HHDM, OOR via `_err`).

## 5. WCET impact

None on hot paths: the allocator never runs on tick/scan/balancer
paths, and the balancer moves queued TCBs without allocating. The
v0.4.5 re-audit (`docs/specs/wcet-reaudit-v0.4.5.md`) bounds
`balancer_tick` explicitly instead.
