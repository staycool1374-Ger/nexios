# pthread Userspace Library (issue #53)

POSIX-thread API for multi-threaded user servers, on in-tree libc +
`SYS_THREAD_CREATE` (#52). Picolibc stays pristine.

## Phasing (audit-bound)

- **Phase (i) — now, no kernel dependency:** `userspace/pthread/`
  headers + single-threaded passthrough stubs + link recipe +
  `userspace/pthread_verify.c` skeleton. Birth/join/cond fail closed
  with `ENOSYS`; mutex/once/keys are functional single-threaded.
- **Phase (ii) — gated on #52:** `THREAD_EXIT`/`THREAD_JOIN` syscalls
  (joint ABI numbering with #52 `THREAD_CREATE`, single `MAX` bump, no
  independent bump), birth wiring (`SYS_THREAD_CREATE`), mutex slow path
  on `EVENT_WAIT`, verification switch-on (`thread_pthread` class +
  `pthread_verify` matrix). No `WAITPID` extension, no futex.

## API surface (`userspace/pthread/pthread.h`)

`create/join/detach/exit/self/equal`, `mutex_init/lock/trylock/unlock/
destroy`, `cond_init/wait/signal/broadcast/destroy`, `once`,
`key_create/setspecific/getspecific/delete`. Exact `int`-returning
prototypes (issue #303 item 1: no `long` decls). Static-only, like the
sysroot (`build-picolibc.sh` static assertions are the precedent).

## Contracts

- **Stack ownership (audit S2-1):** the kernel owns thread stack VA+PMM
  (#52 window); pthread passes only the page count. The TP/errno block
  lives inside the returned VA. No `sbrk` carve, no BRK/stack/FRAME/MMIO/
  PAGER overlap, overflow-checked math (#19).
- **Mutex (audit S2-2):** fast path is userspace atomics; slow path reuses
  `EVENT_WAIT` (phase ii). Non-PI — must not guard RT sections. Wakes
  address the waiting THREAD, never the process leader.
- **Join (audit S1(a)):** the handle is a capability, never a raw tid;
  self-join and join cycles fail closed.
- **Exit (audit S1(b)):** self-only, never returns; status lives in the
  kernel TCB until joined or detached-reaped; rendezvous is
  lost-wakeup-free in both orders.
- **TLS/errno:** the child trampoline calls `TLS_SET` first, before any
  libc call; the TP/errno block is mapped writable in the shared root
  (audit S1(c)). In-tree errno is the TP cell (issue #338); hosted
  programs use the `__errno` override (#303 item 2).
- **TP/trampoline (audit S2-4):** 3-arch guarded (`rdfsbase`/`tpidr_el0`/
  `tp`, mirroring `nexios_glue.c`), no hardcoded addresses (#60).
- **Link hygiene (auditor S3 + #303):** stubs-only link (never the whole
  in-tree archive — `errno`/`_exit` collisions), `int`-exact prototypes.

## Tests

Phase (i): `pthread_verify` builds, links, and stages to `/bin`; marker
lines (`PTHREAD_VERIFY: ...`) anticipate the phase (ii) `thread_pthread`
(TF_KERNEL) class: create/join/exit round-trip, errno-cell isolation
exact-match, mutex exclusion counter, cond wake, both rendezvous orders,
double-join/self-join fail-closed, detached-exit `ResourceTracker`
delta 0. `selftest` unaffected.
