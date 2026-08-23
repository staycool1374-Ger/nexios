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

Browse the backlog: https://github.com/staycool1374-Ger/nexios/issues?q=is%3Aopen+label%3Afeature
