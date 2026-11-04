# Userspace C Runtime Hardening (issue #455)

Five-item hardening of the userspace C runtime (in-tree `src/libc` +
hosted picolibc). Vendored picolibc stays pristine (no-network rule):
fixes land in in-tree code, glue, linker script, or crt0 — never in
`third_party/`.

## 1. `snprintf(NULL, 0, ...)` — in-tree root-caused in #444

In-tree `vsnprintf` wrote the NUL through NULL and never counted the
would-be length; #444 fixes both. Hosted engine needs no fix: live
proof `LIBC_VERIFY: snprintf0 5` (would-be length, zero bytes written
through NULL).

## 2. Thread-safe errno — done, verification only

In-tree: TP-cell errno (`errno.c` `__errno_location`, crt0 Step 3
`TLS_SET`, #338). Hosted: `__errno` TP offset-0 + static fallback
(`nexios_glue.c`), sysroot built `-Derrno-function=__errno`,
`-Dthread-local-storage=false`. Two-thread isolation rides #53
`pthread_verify`; the hosted open-failure mapping gap is tracked there.

## 3. C++ static init / `.init_array` — implemented here

`lds/user_task.ld` provides `.init_array` (+ `.fini_array` for
symmetry, unwalked) with `PROVIDE __init_array_start/end` + `KEEP`.
`src/libc/crt0.S` walks the array between launch parse and `main`
(Step 4b, all three arches): bounded (64 constructors), NULL-entry
skip, no syscalls/alloc, entry regs stashed on the stack (ctors
clobber caller-saved regs), post-TLS_SET (ctors may use errno) —
weak refs so images without `.init_array` skip at zero cost.
Verified: `userspace/initarray-probe.c` (two C constructors +
errno-in-ctor) prints `INITARRAY_VERIFY: PASS` live; `proc_elf`
134/134 green (walk is a no-op for all pre-existing images —
neither libc archive contains `.init_array`).

## 4. `malloc(0)` — deterministic, probed

In-tree: `malloc(0)` → 1-byte `sbrk` block (writable), `free` no-op,
`calloc(0)` → NULL. Hosted: live proof `LIBC_VERIFY: malloc0w 1`
(unique writable block). `realloc` always-NULL stays out of scope.

## 5. I/O bindings + stream buffering — proven + pinned

Bindings complete (glue + `_`-stubs, #303 item 1). Buffering, from
vendored source (read-only): picolibc `posix-console` sets stdout to
`__BLBF` (line-buffered, newline flush + exit destructor) and stderr
to `__BLBF`. stdout line-buffering is kept. stderr is forced
unbuffered by a constructor in `userspace/picolibc/nexios_stdio_init.c`
(sysroot-only TU — the in-tree `stdarg.h` stub would otherwise shadow
the compiler builtin and break picolibc `stdio.h`; linked into every
hosted program per the recipe in `packaging/meson.build`): a crash
mid-line must not lose the partial line. In-tree stdio is unbuffered
direct-write by construction. Constructor ordering note: glue/app
constructors share the step-4b walk; order between them is unspecified
and relied upon by nothing.
