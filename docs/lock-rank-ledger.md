# Lock-Rank Ledger (issue #546-B)

Authoritative per-lock rank record. Every production `sync::SpinLock` has
exactly one row: ranked, or Unranked with a reason token from the closed
set {ISR-reachable, allocator-IF0, sched-nested, M8-deferred}. Contract
§2's table is a summary and must match this file row for row.

## Ranked locks

| Lock | Rank | Declared at |
|---|---|---|
| `scheduler_lock_` | Sched(1) | task/scheduler.cpp:189 (ctor arg; depth authority unchanged, invisible to the order stack) |
| `sync::Queue::lock_` | Queue(2) | sync/queue.hpp:51, init sync/queue.cpp:36 |
| `MessageQueue::lock_` | Queue(2) | ipc/ipc.cpp:101 |
| `DeathNotify::s_lock_` | Leaf(3) | ipc/death_notify.cpp:37 |
| `g_iopb_lock` | Leaf(3) | arch/x86_64/hal/iopb.cpp:53 |
| `g_derivation_lock` | Leaf(3) | cap/derivation.cpp:39 |
| `g_hold_lock` | Leaf(3) | proc/proc_tcb_caps.hpp:79 |

## Unranked locks (M8 owns ranking; INV-LOCK-4)

| Lock | Declared at | Reason | m8_status |
|---|---|---|---|
| urc `s_lock` | console/urc.cpp:44 | ISR-reachable | deferred |
| `g_stop_lock` | debug/debug_stop.cpp:92 | sched-nested (documented order Sched→g_stop→DeathNotify) | deferred |
| `g_bp_lock` | debug/debug_stop.cpp:110 | M8-deferred | deferred |
| iommu `g_lock_` | iommu/iommu.cpp:54 | M8-deferred | deferred |
| `brk_lock` | syscall/syscall_handlers_misc.hpp | M8-deferred (function-local static) | deferred |
| `g_bind_lock` | syscall/syscall_handlers_debug.cpp:92 | ISR-reachable (drain path) | deferred |
| `mempool_lock_` | memory/mempool.cpp:30 | allocator-IF0 (must never park: park path allocates) | not-applicable |
| `pmm_lock_` | memory/pmm.cpp | allocator-IF0 | not-applicable |
| `ElfLoader::lock_` | elf/elf_loader.hpp:224 | M8-deferred | deferred |
| `PagerRegistry::s_lock_` | ipc/pager_registry.hpp:165 | M8-deferred | deferred |
| `ForeignFrameMap::s_lock_` | cap/frame_foreign_map.hpp:99 | M8-deferred | deferred |
| `CapabilityMap` family (`cap.hpp:72`, `vspace_map.hpp:168`, `frame_map.hpp:89`, `carve_binding.hpp:157`, `mmio.hpp:127`, `endpoint.hpp:45`) | see Declared at | M8-deferred | deferred |
| `TaskControlBlock::lock_`, `cwd_lock_` | task/task.hpp:102, task/task.hpp:244 | M8-deferred | deferred |
| `Scheduler::zombie_lock_` | task/scheduler.hpp:1765 | M8-deferred | deferred |
| `TimerWheel::lock_` | time/timer_wheel.hpp:122 | M8-deferred | deferred |
| `PosixTime::lock_` | time/posix_time.hpp:219 | ISR-reachable | deferred |
| `IrqDelivery::lock_` | irq_delivery.hpp:115 | ISR-reachable | deferred |
| `DmaChannel::lock_` | driver/dma.hpp:255,327 | ISR-reachable | deferred |
| `VirtioNetDev::lock_` | driver/virtio_net.hpp:89 | ISR-reachable | deferred |
| `ElfShared::lock_` | elf/elf_shared.hpp:184 | M8-deferred | deferred |

Reason-token semantics: **ISR-reachable** — the lock is taken from ISR
context, so parking is structurally impossible (IsrContext deny); ranking
waits on M8's ISR-order audit. **allocator-IF0** — the lock guards the
page/pool allocator; parking from it would re-enter the allocator via
the park path (sleep-wheel arm), so these locks use `lock_irq_atomic()`
and never reach the hook. **sched-nested** — documented nesting under
`scheduler_lock_`; no enum slot exists between Sched and Leaf, so the
row stays Unranked with the documented order until M8 rules. **M8-deferred**
— not yet audited for nesting; M8 either ranks or renews the deferral.

## QE gate

`rank_table_complete`: the ledger row count equals the production
`sync::SpinLock` definition count (grep-verified); exactly one Sched row;
every ranked row matches the tree rank at its declaration site; every
Unranked row carries a token + `m8_status`. `rank_order_sched_first`:
Sched→Queue inversion adds 0 to `spinlock_violation_count(Sched,Queue)`;
a Queue→Sched pair adds exactly 1 and never panics.
