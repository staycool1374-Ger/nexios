# NexIOS RTOS — Development Roadmap

**Build:** v0.4.3-dev | **Last Release:** v0.4.2 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.4.2)

> **This file is no longer the work tracker.** Open bugs and features live as
> GitHub Issues (repo `staycool1374-Ger/nexios`); progress is kept consistent
> there by the developer agent (see AGENTS.md "GitHub Issue Tracking").
> This file keeps only the guardrails, release pointers, and phase structure.

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.4.3

v0.4.2 RELEASED (2026-09-03) — User-Space Infrastructure milestone:
Untyped child-split/sub-range carve, IRQ caps + user-space IRQ delivery
(WAIT/NOTIFY), MMIO caps + fine-grained I/O delegation (IOPB), IOMMU DMA
protection (VT-d tables + live enablement), MSI-X vector infrastructure,
aarch64 PAN/PXN + bring-up. Details in `ROADMAP_done.md`.

**Current work items — tracked as GitHub Issues in [Milestone v0.4.3](https://github.com/staycool1374-Ger/nexios/milestone/2):**
| Issue | Item |
|---|---|
| [#92](https://github.com/staycool1374-Ger/nexios/issues/92) | Syscall fastpath — static asm jump table (design: `docs/specs/syscall-fastpath.md`) |
| [#93](https://github.com/staycool1374-Ger/nexios/issues/93) | FPU/SIMD context — fixed-offset save areas, lazy FPU (design: `docs/specs/fpu-context.md`) |
| [#85](https://github.com/staycool1374-Ger/nexios/issues/85) | Test coverage: v0.4.x (18 modules) — Phase 5 SMP test-class checklist |

## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.4.2 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak, Fine-Grained Lock & Safety-Guardrail Enforcement, CSpace capability security, User-Space Infrastructure caps/IOMMU/MSI-X).

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
