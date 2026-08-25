# Hardware Driver Layer & Interrupt Architecture Specification

**Semantics:** binding contracts for the block-device abstraction, the AHCI /
ATA-PIO / virtio-blk drivers, the DMA engine, and the x86_64 interrupt
dispatch.  Includes the confirmed audit flaws (`audits/hardware_ahci.md`) that
a spec MUST state as open hardening requirements.  All symbols verified against
the current tree.

## 1. Block-Device Abstraction

```cpp
class kernel::block::BlockDevice {          // pure virtual, BLOCK_SIZE=512
    virtual bool read_sector(uint64_t lba, uint8_t *buffer);   // false on any failure
    virtual bool write_sector(uint64_t lba, const uint8_t *buffer);
    virtual uint64_t sector_count() const;
    virtual uint64_t sector_size() const;   // 512
    virtual bool is_read_only() const;
};
```
`AhciDriver`, `AtaPioDriver`, `VirtioBlkDriver` implement it — synchronous,
blocking, single-command-at-a-time from the caller's perspective.  There is no
multi-request queue at the BlockDevice layer.  Drivers use static `probe()`
factories (`MemPool::alloc` + placement-new), tracked by ResourceTracker.

## 2. DMA Contract

### 2.1 Buffer & scatter-gather
- `DmaBuffer {phys_addr, virt_addr, size, owned}` — `alloc_buffer()` =
  PMM contiguous + VMM map at HHDM + zero; `free_buffer()` = unmap + free.
- `SgList` (≤256 entries), `PrdTable` (≤256 `PrdEntry`: ATA bus-master format,
  `byte_count = count-1`, bit15 = EOT).  `DmaChannel` = `init/start/is_busy/
  handle_irq/abort`; `BmDmaChannel` via BMDMA PCI I/O ports.

### 2.2 DmaEngine state machine
```
start_transfer(prd, dir, cb, ctx):  if active_ → false; channel_.start; active_=true;
                                    callback_=cb; callback_ctx_=ctx        // NOT atomic
handle_irq() (ISR ctx):             if !active_ → false; success = channel_.handle_irq();
                                    active_=false; if cb → cb(ctx, success)  // cb IN IRQ
is_busy():  active_ && channel_.is_busy() (clears active_ if HW idle)
abort():    channel_.abort(); active_=false; callback_=nullptr
```
**FLAW-01 (confirmed, OPEN):** `active_`/`callback_`/`callback_ctx_` are
mutated by `handle_irq()` (ISR) and by `start_transfer()`/`abort()`/`is_busy()`
(task) with **zero mutual exclusion** — a data race.  **Required:** a
`sync::SpinLock` member acquired via `IrqSpinLockGuard` (cli + lock — same core
runs the ISR, so a plain SpinLock self-deadlocks); in `handle_irq()` capture
`callback_`/`callback_ctx_` into stack locals while holding the lock, release,
then invoke the callback **outside** the critical section.

**FLAW-02 (confirmed, OPEN):** `PingPongDma` shares `prepare_idx_`/`xfer_idx_`/
`active_`/`completed_`/`chain_cb_`/`chain_ctx_` across task/IRQ with no lock —
an in-flight DMA target can be handed to the producer.  Same fix pattern;
`prepare_buf`/`xfer_buf` return the resolved pointer while holding the lock.

## 3. AHCI (`ahci.cpp`)

- **Init:** PCI find (class 01/06) → ABAR = BAR5 (validate `bar_count>5`,
  `address!=0`) → map ABAR MMIO page-by-page → set bus master → read
  `HBA_CAP`/`HBA_PI` → HBA reset (GHC_HR poll ≤ 10000) → enable
  `GHC_AE|GHC_IE`.  ⚠️ **FLAW-04:** `GHC_IE` is asserted with **no ISR
  registered**; only the polling `wait_cmd` acknowledges PORT_IS/HBA_IS.
- **Port init:** SSTS DET==3 (online); stop DMA (clear CMD_ST/FRE, wait
  CMD_CR/FR); clear SERR/IS; allocate CL (1 page), RFIS (1 page), CT[32]
  (2 pages each), data buffers[32]; program PORT_CLB/FB; start FRE|ST.
- **Command slot:** `alloc_slot()` = first clear bit in `PORT_CI|PORT_SACT`
  (no per-slot lock — ⚠️ FLAW-04).  `start_cmd()`: zero CT+CH, build CmdFIS
  (type 0x27, PM port 0x80|(tag<<3) for NCQ, `device=0xE0`), 48-bit LBA,
  PRD[0] `(512-1)|IOC`, CmdHeader `cfl=5`, `atomic_fence()`, issue
  `PORT_CI = 1<<slot`.  Commands: READ_DMA_EXT 0x25 / WRITE_DMA_EXT 0x35 /
  READ/WRITE_FPDMA_QUEUED 0x60/0x61 / IDENTIFY 0xEC.
- **`wait_cmd` error paths:** poll PORT_CI clear (≤ 5,000,000 × io_wait);
  PORT_IS TFES → ack+false; TFD_ERR → clear+false; timeout → clear+false.
  ⚠️ **FLAW-05 (OPEN):** an up-to-5-second busy spin that blocks the core and
  starves equal/lower-priority tasks.  **Required:** per-slot completion
  records + a real ISR + scheduler wake; `wait_cmd` becomes a bounded
  blocked-wait.  ⚠️ **FLAW-04 teardown:** `~AhciDriver()` frees CL/RFIS/CT/
  buffers with GHC_IE still enabled → in-flight completion ISR UAF.

## 4. ATA-PIO (`ata_pio.cpp`)

- Register map (base + N): `+0` data, `+2` sector count, `+3..+5` LBA,
  `+6` drive/head (master 0xE0 / slave 0xF0), `+7` status/command.
- `identify()`: select, zero regs, `ATA_CMD_IDENTIFY` (0xEC), reject status
  0/0xFF, `poll_status` BSY-clear, reject ERR, `wait_for_drq`, read 256 words,
  parse `sector_count` (words 60/61 or 100-103).
- `read_sector`/`write_sector`: poll BSY → program LBA → `0x20`/`0x30` →
  `wait_for_drq` → 256× inw/outw → poll status.
- **Pure polling, no IRQ/DMA;** `ATA_POLL_TIMEOUT=100000` io_wait loops.
  Legacy fallback only (not the primary boot IO path in the current tree).

## 5. Virtio-blk (`virtio_blk.cpp`)

- `probe()`: `virtio_find_device(0x1042)` → `init()`: status
  RESET→ACK→DRIVER → negotiate `VIRTIO_F_VERSION_1` → queue_size=16 → 4 PMM
  pages (desc/avail/used/dma_buf) → `virtio_setup_queue` → DRIVER_OK →
  `sector_count` from device_cfg.
- **Descriptor chain** (idx = avail_idx % 16):
  `[idx]  hdr(16, F_NEXT) → [(idx+1)%16] data(512, F_NEXT [+F_WRITE for read])
  → [(idx+2)%16] status(1, F_WRITE)`.
  Ordering: write header/data, `avail->ring[idx]=idx`, fence, `avail->idx++`,
  fence, `virtio_notify` kick.  Completion: busy-poll `used->idx` (≤ 1M),
  status == `VIRTIO_BLK_S_OK`, memcpy out.
  **FLAW-06 (BOUNDED — M-2 ledger update, 2026-08-25):** the poll is capped at
  1M iterations and now snapshots `used->idx` BEFORE `virtio_notify` (FLAW-03
  pattern), and the whole submit chain is serialized under a device mutex
  (H-3).  A fully IRQ-driven blocked wait (ISR walking the used_ ring + wait
  primitive) remains Phase 4.7 roadmap scope.

## 6. Interrupt Layer (x86_64)

```
 CPU interrupt gate (IDT[i], type 0x8E; IST1 for #DF; SYSCALL 0x80 trap 0xEE)
   │ vector i (+ error code for ISR_ERR)
   ▼
 isr_common (isr_stubs.asm)
   │ inc [isr_nesting_depth]          ← depth ≤ 2 contract
   │ rdtsc → irq_entry_tsc; push GPRs; rdi=vec rsi=err rdx=rip rcx=rsp
   ▼
 handle_interrupt_c(vec, err, rip, regs, tsc)        [kernel.cpp]
   ├─ v==7        lazy FPU/SSE (fpu_owner, fxsave/fxrstor)
   ├─ user-recover g_user_access_recover_ip redirect
   ├─ v<32        user: deliver_signal / kernel: guard-page check → panic
   ├─ v==0x80     syscall_handler → signals → reschedule
   ├─ THREADED_IRQS: IrqThread::for_vector(v) → isr_entry (ack+Notify) → task
   ├─ else        IDT::handle_interrupt → handlers_[v]
   │               ├─ v==64 timer → Timer::handle_irq → Scheduler::on_tick → re-arm
   │               └─ v==33 kbd   → Keyboard::handle_irq (byte-atomic mods_, SPSC ring)
   ▼
 EOI (APIC for all; PIC for 32–47) + latency histogram
   ▼
 isr_common epilogue:
   cli; deferred-switch? (generation check, depth ≤ 2, stack-bounds check) → switch
   pop GPRs; add rsp,16; dec [isr_nesting_depth]; iretq
```

### 6.1 IRQ allocation
- Static vector map: IRQ0→32, IRQ1→33, IRQ2-15→32+i (masked); APIC timer=64;
  keyboard=33.  No runtime allocator in the IRQ path; `irq_alloc` is enforced
  as a test class (no allocations in timer/keyboard/syscall ISRs).

### 6.2 APIC timer
- TSC-deadline mode when supported (`LVT_TIMER_TSCDEADLINE` + `wrmsr(
  MSR_TSC_DEADLINE)`); else periodic bus-clock (calibrate, INITCNT).  Masks
  I/O APIC IRQ0 (PIT).  Registered handler: `Timer::handle_irq`
  (atomic `ticks_++`) + `Scheduler::on_tick()` + re-arm.

### 6.3 Nesting depth (≤ 2)
- Depth 1 = normal IRQ; 2 = SYSCALL+timer nesting; ≥ 3 = detected bug — the
  context switch in the epilogue is skipped.  `on_tick` checks it to skip
  re-entrant scheduler ops.  Tests reset it to 0.

### 6.4 Keyboard
- Vector 33; threaded mode = `IrqThread::create(33, prio 50, ...)`.
- `handle_irq` reads STATUS/DATA, updates `mods_` byte-atomic (valid lock-free
  SPSC producer pattern — audit V-5 dismissed), translates scancode → ASCII,
  `push_ring`.  ⚠️ **FLAW-10 (OPEN):** `init()` has an **unbounded** PS/2
  output-buffer drain loop — must be capped like the second bounded drain.

## 7. Binding Invariants

1. **No dynamic allocation in the IRQ path** (MemPool/PMM/heap) — completion
   state is statically embedded; enforced by the `irq_alloc` test class.
2. **Bounded blocking everywhere.** AHCI `wait_cmd` (5M spins, FLAW-05),
   virtio `submit_request` (1M spins, FLAW-06), serial (FLAW-08), keyboard
   drain (FLAW-10) must become bounded loops or scheduler-blocked waits.
   Timeout values are the *blocked-wait bound*, not a spin bound.
3. **Spinlock-in-IRQ rules.** Shared ISR/task state (DmaEngine, PingPongDma,
   AHCI port command state) needs a `SpinLock` via `IrqSpinLockGuard`;
   callbacks invoked only after release, from stack-captured locals; no
   allocation/blocking while holding a lock in IRQ context.
4. **`GHC_IE` ordering.** AHCI global interrupt-enable must not be set until a
   real ISR is wired (FLAW-04); teardown clears GHC_IE/PORT_IE and takes port
   locks before freeing CL/RFIS/CT/data memory.
5. **UART FIFO drain.** 16550 FIFO = 16 bytes; `Serial::putchar` polls THRE per
   char and must drain between bursts to avoid overflow (perturbs timing).
6. **Memory ordering for DMA.** `kernel::atomic_fence()` precedes issuing
   commands (AHCI `PORT_CI` write, virtio avail idx increment + kick) so
   descriptor writes are visible before the doorbell.
7. **Nesting depth ≤ 2**; deeper = corruption (switch skipped).

## 8. Open Flaw Ledger

| Flaw | Location | Status |
|---|---|---|
| FLAW-01 DmaEngine ISR/task race | dma.cpp | **RESOLVED (2026-08-16, `bf40f351`)** — IrqSpinLockGuard; callback-after-unlock from stack locals |
| FLAW-02 PingPongDma index race | dma.cpp | **RESOLVED (2026-08-16, `bf40f351`)** — lock; start_next resolves directly; shutdown clears chain cb under lock |
| FLAW-03 virtio-net ring races | virtio_net.cpp | **RESOLVED (2026-08-16, `a8fe7bd9`)** — lock + tx_inflight_; poll consume/recycle/advance atomic; used-snapshot-before-notify |
| FLAW-04 AHCI GHC_IE w/o ISR + teardown UAF | ahci.cpp | OPEN (§3) |
| FLAW-05 AHCI 5s busy-poll | ahci.cpp wait_cmd | OPEN (§3) |
| FLAW-06 virtio-blk 1M spin | virtio_blk.cpp | BOUNDED (§5) — 1M cap + used-idx pre-notify snapshot + device mutex; IRQ-driven wait = Phase 4.7 |
| FLAW-08 serial unbounded polling | serial.cpp | **RESOLVED (2026-08-16, `357c62a1`)** — bounded TX/RX polls (1M iters + pause); drop/'\0' failure semantics |
| FLAW-10 keyboard unbounded drain | keyboard.cpp | **RESOLVED (2026-08-16, `357c62a1`)** — first drain capped at 16 (i8042 depth) |

# §9 FLAW Fix Plan (v0.4.1)

**Scope:** FLAW-01 (DmaEngine), FLAW-02 (PingPongDma), FLAW-03 (virtio-net ring races), FLAW-08 (serial unbounded polling, x86_64), FLAW-10 (keyboard unbounded drain). FLAW-04/05/06 (AHCI/virtio-blk) are **out of scope** for this plan.

## 9.1 Current State Assessment

### 9.1.1 FLAW-01 — DmaEngine ISR/task race

Shared fields (no lock, mutated from both IRQ and task context):
- `active_` — `src/kernel/driver/dma.hpp:215`, `callback_` — `dma.hpp:216`, `callback_ctx_` — `dma.hpp:217`.

Mutators:
- `start_transfer()` — `src/kernel/driver/dma.cpp:227-237` (task): reads `active_`, writes `active_`/`callback_`/`callback_ctx_`.
- `is_busy()` — `dma.cpp:239-247` (task): reads and clears `active_`.
- `handle_irq()` — `dma.cpp:249-258` (ISR): reads/clears `active_`, reads `callback_`/`callback_ctx_`, **invokes the callback at dma.cpp:254-256 while still in IRQ context**.
- `abort()` — `dma.cpp:260-265` (task): clears `active_`/`callback_`/`callback_ctx_`.

**Race mechanism:** task-side `start_transfer()`/`abort()`/`is_busy()` can interleave with the ISR's `handle_irq()` — torn `active_`/`callback_`/`callback_ctx_` reads, a callback fired with a stale context, a transfer started after the ISR already cleared state, or an `abort()` racing the callback. Because the same core runs the ISR, a plain `SpinLock` (cli-free) would self-deadlock; the lock must be taken with IRQs disabled.

### 9.1.2 FLAW-02 — PingPongDma index race

Shared fields: `prepare_idx_` (dma.hpp:277), `xfer_idx_` (dma.hpp:278), `active_` (dma.hpp:279), `completed_` (dma.hpp:280), `chain_cb_` (dma.hpp:281), `chain_ctx_` (dma.hpp:282).

Accessors/mutators:
- `prepare_buf()` — dma.cpp:292-294, `xfer_buf()` — dma.cpp:296-298 (task).
- `start_next()` — dma.cpp:306-341 (task): swaps indices at 321-323/331-333; calls `engine_.start_transfer()` at 328.
- `on_completion()` — dma.cpp:343-351 (IRQ, via `pingpong_completion_cb` at dma.cpp:300-304): clears `active_`, increments `completed_`, reads `chain_cb_`/`chain_ctx_`/`xfer_idx_`, **invokes the chain callback at dma.cpp:346-348 inside the IRQ path**.
- `busy()` — dma.hpp:264-266 (inline, task), `completed_count()` — dma.hpp:269-271 (inline, task), `shutdown()` — dma.cpp:353-361 (task).

**Race mechanism:** an in-flight DMA target can be handed to the producer: `prepare_buf()` can return `&bufs_[xfer_idx_]` (the buffer the DMA engine is still writing) if the ISR's `on_completion()` hasn't cleared `active_` yet. `xfer_buf()` inside `start_next()` (dma.cpp:311) is a nested call into the same object — if the fix introduces a non-recursive SpinLock, this self-reentrancy becomes a self-deadlock.

### 9.1.3 FLAW-03 — virtio-net ring races

Shared ring state in `VirtioNetDevice`: `rx_avail->idx`, `tx_avail->idx`, `rx_used->idx`, `tx_used->idx` (virtio_net.hpp:58-68), `rx_avail_idx`/`tx_avail_idx`/`rx_last_seen_used` (virtio_net.hpp:79-81), `rx_bufs[]`/`rx_bufs_phys[]` (virtio_net.hpp:71-72, recycled buffers), single shared `tx_buf`/`tx_buf_phys` (virtio_net.hpp:75-76).

Racy functions:
- `add_rx_buf()` — virtio_net.cpp:104-114 (probe, poll-recycle).
- `virtio_net_send_frame()` — virtio_net.cpp:244-279: copies into shared `tx_buf` (253-255), programs `tx_desc[idx]` (257-261), publishes avail (263-265), notifies (268), busy-polls `tx_used->idx` (271-275). Concurrent sends corrupt the single TX buffer and race `tx_avail_idx`.
- `virtio_net_poll()` — virtio_net.cpp:281-308: reads `rx_used->idx` (287), **recycles the buffer via `add_rx_buf()` (305), then advances `rx_last_seen_used` (306)** — non-atomic; a concurrent poll can double-hand the same buffer or double-consume the same used slot.

**Race mechanism:** producer/consumer races on the shared avail/used indices and on buffer ownership. `atomic_fence()` (112/264/266) orders device-visible memory but provides no mutual exclusion between concurrent software producers.

### 9.1.4 FLAW-08 — Serial unbounded polling (x86_64)

- `putchar()` CR THRE poll — `serial.cpp:46-48`: `while ((inb(COM1+5) & 0x20) == 0);` — **unbounded**.
- `putchar()` char THRE poll — `serial.cpp:50-51`: **unbounded**.
- `getchar()` LSR poll — `serial.cpp:58-59`: **unbounded**.

**Mechanism:** a dead/hung UART spins the core forever with no `arch::pause()`. No in-tree caller of `Serial::getchar` exists (only the definition) — timeout semantics change is low-risk. AArch64 (`aarch64/serial.cpp:59-64, 71-74`) is a documented follow-up, not part of this plan's gates.

### 9.1.5 FLAW-10 — Keyboard unbounded PS/2 drain

- First drain loop — `keyboard.cpp:74-75`: `while ((inb(STATUS_PORT) & 0x01) != 0) inb(DATA_PORT);` — **unbounded**. Model for the fix is the *second* bounded drain at keyboard.cpp:80-84 (`for (int i = 0; i < 4; ++i)` with break-on-empty).

**Mechanism:** a stuck i8042 output-buffer-full bit hangs `Keyboard::init()` forever (boot-time hang).

## 9.2 Design per FLAW

### 9.2.1 FLAW-01 — DmaEngine locking protocol

1. Add member `mutable sync::SpinLock lock_;` to `DmaEngine` (dma.hpp:213-218).
2. **Guard choice:** `IrqSpinLockGuard` **exclusively** (cli + lock) — same core runs the ISR; a plain `SpinLockGuard` held by a task would be re-entered by the ISR and spin forever. With cli held by the task guard, the ISR never preempts the critical section.
3. `start_transfer()`: take guard at top; keep order (`if (active_) return false;` → `channel_.start(...)` → set `active_`/`callback_`/`callback_ctx_`). `channel_.start()` is bounded port I/O (dma.cpp:181-188) — legal under cli+lock.
4. `handle_irq()`: under the lock: `if (!active_) return false;` `success = channel_.handle_irq()`; **capture `cb`/`ctx` into stack locals**; clear `active_`/`callback_`/`callback_ctx_`; release; **then** `if (cb) cb(ctx, success);` — callback strictly **outside** the critical section.
5. `is_busy()`: under the lock: read `active_`, `channel_.is_busy()`, clear `active_` if HW idle.
6. `abort()`: under the lock: `channel_.abort()` first (stops HW → no completion IRQ), then clear `active_`/`callback_`/`callback_ctx_`.
7. Lock-order: `DmaEngine::lock_` is a leaf lock — `handle_irq` invokes callbacks after release, never nests into `PingPongDma::lock_`.

### 9.2.2 FLAW-02 — PingPongDma locking protocol

1. Add member `mutable sync::SpinLock lock_;` to `PingPongDma` (dma.hpp:273-283).
2. `prepare_buf()`/`xfer_buf()`: take the guard, resolve and return the pointer **while holding the lock**.
3. `start_next()`: take the guard at top. **Do not call `xfer_buf()`/`prepare_buf()` inside** (non-recursive lock); resolve directly. Swap indices; set `chain_cb_`/`chain_ctx_`; call `engine_.start_transfer(...)` **nested** (fixed order `PingPongDma::lock_` → `DmaEngine::lock_`; never reversed). On failure swap back.
4. `on_completion()`: under the lock: `active_ = false; ++completed_;` **capture `chain_cb_`/`chain_ctx_`/`&bufs_[xfer_idx_]`** into stack locals; release; **then** invoke chain callback outside.
5. `busy()`/`completed_count()` (inline, dma.hpp): wrap in `IrqSpinLockGuard`; `lock_` is `mutable`. Include `<kernel/sync/irq_spinlock_guard.hpp>` in dma.hpp.
6. `init()`: allocate buffers **outside** the lock, then reset state under the lock.
7. `shutdown()`: under the lock: `engine_.abort()`, clear `active_`/`completed_`/`chain_cb_`/`chain_ctx_`; release; **then** free buffers outside.

### 9.2.3 FLAW-03 — virtio-net locking protocol

1. Add members to `VirtioNetDevice`: `sync::SpinLock lock_;` and `bool tx_inflight_ = false;`.
2. Extract lock-free helper `rx_ring_fill_locked(dev, qi)` (descriptor + avail entry + fence + idx++); `add_rx_buf()` = guarded wrapper.
3. `virtio_net_poll()`: one `IrqSpinLockGuard` covering read-used → copy-out → recycle (`rx_ring_fill_locked`) → `rx_last_seen_used++` — consumption+recycle+advance atomic.
4. `virtio_net_send_frame()`: under guard — `if (tx_inflight_) return false;` copy, program desc, publish avail, fence, notify, `tx_avail_idx++`, `tx_inflight_ = true`; exit. Bounded completion poll **outside** the lock; re-acquire to clear `tx_inflight_`.
5. `virtio_net_probe()`: init `tx_inflight_ = false`.
6. New teardown API `void virtio_net_destroy();` — clears `g_virtio_net_dev`, destructs + MemPool::free.
7. **Memory ordering:** `atomic_fence()` placements (112/264/266) are mandatory and preserved inside the guarded regions.
8. Test cleanliness: probe tests end with `virtio_net_destroy()` + `arch::virtio_write_status(transport, arch::VIRTIO_STATUS_RESET)`.

### 9.2.4 FLAW-08 — Serial bounded polling (x86_64)

1. Constants: `SERIAL_TX_WAIT_ITERS = 1000000`, `SERIAL_RX_WAIT_ITERS = 1000000`.
2. `putchar()` CR path: bounded `for` with `arch::pause()`; on timeout skip the `'\r'` write.
3. `putchar()` char path: bounded; on timeout drop and return.
4. `getchar()`: bounded; on timeout return `'\0'` (documented sentinel; no kernel callers).
5. Failure = drop/sentinel/return-false, never a hang. Follow-up (out of gates): aarch64/serial.cpp.

### 9.2.5 FLAW-10 — Keyboard bounded drain

1. First drain capped at **16** (i8042 output-buffer depth): `for (int i = 0; i < 16; ++i) { if ((inb(STATUS_PORT) & 0x01) == 0) break; inb(DATA_PORT); }`.
2. Failure behavior: stale bytes left; the keyboard ISR consumes/discards them — no hang.
3. Optional: `arch::pause()` inside both drain loops.

## 9.3 Phased Execution Plan

### Phase 1 — FLAW-01 DmaEngine (extend `drivers_dma`)
- Files: `src/kernel/driver/dma.hpp`, `dma.cpp`, `src/kernel/test/test_dma.cpp`.
- Tests (+4): `dma_engine_reject_when_busy`, `dma_engine_no_double_callback`, `dma_engine_abort_suppresses_callback`, `dma_engine_irqguard_roundtrip`.
- Verify: `make execute-test x86_64 debug drivers_dma` → **16/16** (12→16).

### Phase 2 — FLAW-02 PingPongDma (extend `drivers_dma`)
- Files: `dma.hpp`, `dma.cpp`, `test_dma.cpp`.
- Tests (+3): `pingpong_xfer_buf_while_busy`, `pingpong_reject_when_busy`, `pingpong_shutdown_clears_state`.
- Verify: `drivers_dma` → **19/19** (16→19).

### Phase 3 — FLAW-03 virtio-net (extend `drivers_virtio`)
- Files: `virtio_net.hpp`, `virtio_net.cpp`, `test_virtio.cpp`.
- Tests (+2): `virtio_net_probe_poll_and_teardown`, `virtio_net_send_frame_completes`.
- Verify: `drivers_virtio` → **11/11** (9→11).

### Phase 4 — FLAW-08 serial + FLAW-10 keyboard (extend `drivers_core`)
- Files: `serial.cpp`, `keyboard.cpp`, `test_driver.cpp`.
- Tests (+2): `serial_putchar_bounded_ok`, `keyboard_init_bounded_drain`.
- Verify: `drivers_core` → **8/8** (6→8).

### Phase 5 — Counts, docs, full gates
- Counts (test_expected_counts.hpp): `drivers_dma` 12→**19**, `drivers_virtio` 9→**11**, `drivers_core` 6→**8**, `all` → **931**. Reconcile against observed baseline; `validate_all_consistency()` must not warn.
- Verify: `make build` (0 errors); debug `all` → **931/931** (trace ON); release `all` → **84/84** (trace OFF); selftest → **132/132**.
- Update §8 ledger: FLAW-01/02/03/08/10 → RESOLVED (FLAW-04/05/06 remain OPEN). SIL 3 audit via diff-patch protocol.

## 9.4 SIL 3 Considerations

1. IRQ-context: no alloc/block/callback under a lock in IRQ context; critical sections contain only flag/index I/O, bounded port I/O, bounded memcpy, virt_to_phys walk.
2. `IrqSpinLockGuard` (cli + lock) mandatory for every new lock; plain `SpinLockGuard` self-deadlocks under same-core ISR preemption.
3. Callback-after-unlock from stack-captured locals (FLAW-01 handle_irq, FLAW-02 on_completion).
4. Nesting depth ≤ 2 preserved.
5. cli/lock orders CPU-visible accesses only; device-DMA ordering requires the virtio `atomic_fence()` placements — preserved.
6. `SpinLock` is non-copyable — drivers must never be memcpy'd; `virtio_net_destroy()` clears the global before free; `shutdown` clears chain cb under lock before freeing buffers.
7. Bounded-wait semantics: timeouts are bounds, not spin targets; failure = drop/sentinel/return-false, never a hang.

## 9.5 Gate / Definition of Done

| Phase | Scope | Class count | Gate |
|---|---|---|---|
| 1 | FLAW-01 | `drivers_dma` 12→16 | 16/16 |
| 2 | FLAW-02 | `drivers_dma` 16→19 | 19/19 |
| 3 | FLAW-03 | `drivers_virtio` 9→11 | 11/11 |
| 4 | FLAW-08/10 | `drivers_core` 6→8 | 8/8 |
| 5 | Gates | `all` → **931** | debug all 931/931, release all 84/84, selftest 132/132 |

Final: `make build` clean, SIL 3 auditor sign-off, zero ResourceTracker leak deltas, §8 ledger updated.
