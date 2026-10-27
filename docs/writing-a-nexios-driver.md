# Writing a NexIOS Driver (Ring-3)

Scope: **userspace** drivers — ordinary `userspace/*.c` programs that are
created, linked, and loaded with `runelf`. In-kernel hardware drivers
(AHCI/ATA-PIO/virtio-blk, DMA, IRQ dispatch) are a different layer,
specified in `docs/specs/drivers.md` and out of scope here. The
worked example is `userspace/ref-driver.c` (issue #171 P6).

## 1. Entry contract (binding)

Every Ring-3 image enters through the P1 `crt0` 6-step sequence
(`src/libc/crt0.S`, header comment): raw `ABI_VERSION` query → SDK-major
check vs the embedded constant → `TLS_SET` to the picolibc block →
launch parse → `main` → clean `SYS_EXIT` with `main`'s return code.
`tls_base_ == 0` means unset; the SDK major or CRT0-contract mismatch
takes the fail path: a best-effort diagnostic on fd 2 plus `SYS_EXIT`
with `0xC0DE` (see §4 for where that diagnostic actually lands).

`main` forms: `int main(void)` (cf. `hey.c`), `argc/argv`
(cf. `cat.c`, `ls.c`), and the 3-arg + envp form the loader drives
(covered by `loader_runelf_3arg_main_envp`). Return an exit code per
check and let `crt0` exit with it — the exit code is the
test-observable signal; serial text is diagnostic-only. The reference
driver returns 0 on all-green, 1..5 naming the failed check.

Pin your SDK at compile time with `src/libc/sdk_version.h`
(`NEXIOS_SDK_MAJOR/MINOR`, `NEXIOS_CAP_API_MAJOR/MINOR`,
`NEXIOS_CRT0_CONTRACT`; added in P0): a `_Static_assert` on the major
and contract turns a lane bump into a loud build break instead of a
silent behavior change. `ref-driver.c` pins SDK major 0, contract 1.

## 2. Bootstrapping: you own exactly what you were given (binding)

The P4 rule, as code comments at `finalize_loaded_task`
(`src/kernel/elf/elf.cpp`) and `ensure_cspace`
(`src/kernel/task/task.cpp`): **the loader seeds nothing**. A new
driver arrives with a null CSpace (lazy, null-until-first-use) and
receives only `argc/argv/envp` — no caps, no versioned launch struct
(reserved-absent), no magic. Everything in its CSpace arrived through
an explicit grant/copy/mint its **creator** performed before first
dispatch (caller-relative handles), and `exec` inherits the CSpace
(`exec_into_current` preserves it while resetting mappings/FDs/TLS).

Consequences for driver authors: never assume ambient authority —
open the FDs, map the frames, and hold the endpoint caps your creator
gave you; request the rest through the IPC patterns below. Any future
behavior change here needs a plan amendment and re-audit first (P4
scope boundary).

## 3. IPC patterns

Raw API: `src/libc/ipc.h`. `ipc_send_sync(dest, type, data, size,
reply_buf)` is the workhorse — contract (issue #317 option b): the
reply **payload** lands in `reply_buf` (a real destination, at least
`size` bytes; `NULL` for no payload), and the return value is the
reply message **type** (negative on failure), not a result code. Async
`ipc_send(..., flags)` with `IPC_NONBLOCK` never waits; `ipc_notify /
ipc_notify_wait` and `ipc_event_set / ipc_event_wait` cover
fire-and-forget wakeups.

Patterns, in increasing order of coupling:

1. **Fail-closed probe (no peer).** Send `IPC_NONBLOCK` to a PID that
   cannot exist and assert a negative return. This exercises the
   boundary validation with zero hang risk. `ref-driver.c` does this
   against PID 9999 (PIDs are small dense indices; 9999 is out of
   range on every boot).
2. **Self-notify post (no peer).** `ipc_notify(getpid(), value)`
   queues into your own queue; assert non-negative. No wait is done
   here on purpose — see §4 on why waits need a responder.
3. **Responder round-trip (needs a peer).** The `ipcsync-probe.c`
   pattern: the test harness (PID 1) acts as responder, so
   `ipc_send_sync(1, ...)` returns a real reply type + payload.
   Under a bare `runelf` shell there is no responder — a _sync wait
   with nobody to answer hangs forever. Draft the responder side
   first, or keep _sync calls out of the standalone path.
4. **Post-merge cap-API (P3 headers).** `grant / copy / revoke /
   mint-with-badge`, `MMIO_MAP`, `IRQ_REGISTER/WAIT`,
   `IOMMU_MAP/UNMAP`, endpoint send/recv, `FRAME_*` — allow-list only
   (`src/libc/cap/` on the P3 lane). `dst` binds to your own or an
   endpoint-delegated CSpace only; every op fails closed with a code,
   never `ENSURE`s. Raw constructors (`create(phys/vector)`,
   `CNode::install/remove`, revoke cascades, `IOPORT_GRANT`) are
   deliberately unreachable from Ring-3 headers.

## 4. Serial debugging: read the right log

Two channels exist and they are not the same (P1/F1 calibration):

- `com1.log` — the **guest-serial** target (`-serial file:`): this is
  where the `.Lfail` contract-mismatch diagnostic (`crt0: ABI
  contract mismatch, exiting`, 7x in calibration) and per-task
  `0xC0DE` exit reports land. Assert on this channel.
- `serial.log` — the **debug chardev** (`isa-debugcon`): 0 hits for
  the fail-path diagnostic by design (the write goes through the
  loader-wired `/dev/tty` path, not the debug channel).

Methodology: exit code first (it is the binding signal), serial text
second (diagnostic-only; the fd-2 write result is ignored by design).
`printf` works through the `WRITE` path once the image is dispatched.
For hard faults, the audit tripwires (`ud2`/`udf`/`unimp`, P1) and the
GDB driver (`tools/meson/gdb_run.py debug-test`, `tools/gdb/`
scripts) give you the death point; the debug context-switch ring in
each TCB (`debug_switch_ring[4]`) records the last switches.

## 5. Building: in-tree today, staged SDK tomorrow

In-tree (works now): `packaging/meson.build` auto-discovers every
`userspace/*.c` outside `picolibc/` and links `<name>.c.elf` from
`crt0.o + <name>.c + libc.a` with `-I src/libc` and
`-Wl,-z,max-page-size=0x1000`. Non-root names land in `/bin`
(`load /bin/ref-driver.c.elf`, then bare `runelf`); only
`hey/nullderef/prime` are staged at root (issue #274). Adding the
`.c` is the whole integration — no build-file edit.

Staged SDK (P5, `/opt/nexios-sdk`): install rules populate
`include/` (12 headers + `sys/` x3), `lds/user_task.ld`, `lib/crt0.o`
and `lib/libc.a` from a clean build; two `meson install --destdir`
runs diff clean (determinism proof). `lds/user_task.ld` **augments**
the toolchain script (`INSERT AFTER .rodata`, `KEEP(.nexios_sdk)`,
per-arch load origins untouched) and is consumed with
`-T user_task.ld`. The P6 end-state builds the reference driver
against the staging instead of the source tree — that cross-file
proof runs once the P2 (stub archive) + P3 (cap headers) lanes merge;
until then this guide's in-tree build is the executable claim, and
the cross-file E2E stays explicitly deferred, not silently skipped.
