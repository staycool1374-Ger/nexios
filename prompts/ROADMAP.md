# NexIOS RTOS — Development Roadmap

**Build:** v0.4.2-dev | **Last Release:** v0.4.1 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.4.1)

> **This file is no longer the work tracker.** Open bugs and features live as
> GitHub Issues (repo `staycool1374-Ger/nexios`); progress is kept consistent
> there by the developer agent (see AGENTS.md "GitHub Issue Tracking").
> This file keeps only the guardrails, release pointers, and phase structure.

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.4.2

v0.4.1 RELEASED (2026-08-16) — CSpace milestone: Capability-Based Access
Control iteration-1 (CNode/CSlot, grant/copy/mint/revoke, SYS_CAP_* 51–54,
Endpoint/FrameCap, cap-gated IPC + frame mapping) + Untyped memory allocator
(`cap::retype`). Details in `ROADMAP_done.md`.

**Current work items — tracked as GitHub Issues in [Milestone v0.4.2](https://github.com/staycool1374-Ger/nexios/milestone/1):**
| Issue | Item |
|---|---|
| [#1](https://github.com/staycool1374-Ger/nexios/issues/1) | Untyped child-split + sub-range carve (`cap::retype`, design: `docs/specs/cspace.md` §2.8) |
| [#2](https://github.com/staycool1374-Ger/nexios/issues/2) | IRQ caps + user-space IRQ delivery (IrqCap) |
| [#3](https://github.com/staycool1374-Ger/nexios/issues/3) | MMIO caps + fine-grained I/O delegation |
| [#4](https://github.com/staycool1374-Ger/nexios/issues/4) | IOMMU DMA protection (VT-d / AMD-Vi / SMMU) |
| [#5](https://github.com/staycool1374-Ger/nexios/issues/5) | MP-4.4 — aarch64 PAN/PXN enablement |
| [#6](https://github.com/staycool1374-Ger/nexios/issues/6) | Implement all pending audits/refactorings under `audits/` |

## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.12 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak, Fine-Grained Lock & Safety-Guardrail Enforcement).

---

## Future Roadmap (Aspirational)

All aspirational items are filed as GitHub Issues with the `feature` label,
titled `[<phase>] <item>` (search: `is:open label:feature`). Phase overview:

- **Phase 4.6 (0.4.x):** User-Space Driver Infrastructure & Hardware Isolation
- **Phase 4.7 (0.4.x):** Time, Deterministic Scheduling & Bounded I/O; Interrupt-Driven I/O & Bounded Blocking
- **Phase 5 (0.4.x):** SMP + Multicore
- **Phase 6 (0.5.x):** System Integration / Userspace ABI (syscall ABI, picolibc, POSIX time, runelf, multi-arch production boot, Raspberry Pi 4)
- **Phase 7 (0.6.x):** Safety Systems
- Later phases through Phase 10 (v1.0.0 release gate)

### Design Papers (docs/specs/)

Detailed, NexIOS-specific design documents derived from the Cyjon codebase
study (2026-08); each targets a v0.4.x release and references the verified
current-state anchors in `src/kernel/`:

| Paper | Topic | Target |
|---|---|---|
| [`docs/specs/syscall-fastpath.md`](../docs/specs/syscall-fastpath.md) | Syscall dispatch via static asm jump table — deterministic per-call latency | v0.4.3 (Phase 4.7) |
| [`docs/specs/fpu-context.md`](../docs/specs/fpu-context.md) | FPU/SIMD state: fixed-offset save areas + reentrancy rules for lazy FPU | v0.4.3 (Phase 4.7) |
| [`docs/specs/exception-table-audit.md`](../docs/specs/exception-table-audit.md) | Exception vector audit incl. concrete fix: #VE/#HV error-code classification (**bug [#91](https://github.com/staycool1374-Ger/nexios/issues/91)**, S1) | v0.4.3 (Phase 4.7) |
| [`docs/specs/per-cpu-smp.md`](../docs/specs/per-cpu-smp.md) | Per-CPU foundation (LAPIC-id indexed) + SMP bring-up skeleton ([#94](https://github.com/staycool1374-Ger/nexios/issues/94)) | v0.4.4 (Phase 5) |
| [`docs/specs/elf-shared-libs.md`](../docs/specs/elf-shared-libs.md) | DT_NEEDED shared-object resolution for user ELF images ([#95](https://github.com/staycool1374-Ger/nexios/issues/95)) | v0.4.4 (Phase 6 prep) |
| [`docs/specs/kernel-half-merge.md`](../docs/specs/kernel-half-merge.md) | Kernel-half page-table merge as fork/exec fast path, post-MP-7 ([#96](https://github.com/staycool1374-Ger/nexios/issues/96)) | v0.4.5 (Phase 6 prep) |

Each paper has a tracking issue (`[0.4.x] Implement design paper: …`,
`feature` label) plus GitHub milestones **v0.4.3** (#92–93 + #91),
**v0.4.4**, **v0.4.5**; the bug in the exception audit is tracked
separately as `bug`/S1.

Browse the backlog: https://github.com/staycool1374-Ger/nexios/issues?q=is%3Aopen+label%3Afeature
