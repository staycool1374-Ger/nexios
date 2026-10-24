# Rust Userspace (issue #273)

Statically-linked Rust (`no_std`, `panic = "abort"`, core-only) user ELFs
on NexIOS x86_64. Proven by `user/rust-hello` (prints via WRITE, exits 0).

## 1. Non-goals

No `std`, no dynamic linking, no Linux syscall compat, no Rust TLS
runtime, no kernel changes. `panic = "abort"` always (no unwinding).

## 2. Toolchain

- Host stable (bootstrap only): `rustc/cargo 1.99.0 (Homebrew)`.
  Cannot `-Z build-std` — nightly required for the custom target.
- Pinned nightly: `nightly-2026-10-12`
  (`rustc 1.101.0-nightly (080cc2714 2026-10-11)`), pin lives in
  `user/rust-hello/rust-toolchain.toml` (+ `rust-src` component).
- Install is project-local: `RUSTUP_HOME/CARGO_HOME` under `build/`
  (see `user-rust-hello` rule); never touches `~/.rustup`, never depends
  on the dev machine's toolchain. First build after a `clean` needs
  network (toolchain + core build-deps fetch, pinned by
  `user/rust-hello/Cargo.lock` via `--locked`); later builds are served
  from the cargo cache.
- One-time bootstrap (the `user-rust-hello` rule does NOT perform it;
  without it `build/cargo/bin/cargo` is absent and the rule fails):
  ```bash
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    RUSTUP_HOME=$PWD/build/rustup CARGO_HOME=$PWD/build/cargo sh -s -- \
    -y --no-modify-path --no-update-default-toolchain
  RUSTUP_HOME=$PWD/build/rustup CARGO_HOME=$PWD/build/cargo \
    PATH=$PWD/build/cargo/bin:$PATH \
    rustup toolchain install nightly-2026-10-12 --component rust-src
  RUSTUP_HOME=$PWD/build/rustup CARGO_HOME=$PWD/build/cargo \
    PATH=$PWD/build/cargo/bin:$PATH \
    rustup default nightly-2026-10-12
  ```
  The explicit `default` matters: the recipe invokes cargo from the repo
  root via `--manifest-path`, and rustup discovers `rust-toolchain.toml`
  from the working directory — not the manifest path — so the crate pin
  alone does not select the nightly.
- `meson compile user-rust-hello -C build/meson-aarch64` (or riscv64) prints a skip and
  succeeds — Rust is x86_64-only. WARNING: switching builddirs never wipes
  its siblings (one builddir per arch), but the Rust toolchain lives in the
  shared `build/` tree — budget a re-download after a full `build/` purge.

## 3. Target spec (`rust-targets/x86_64-nexios.json`)

Derived field-for-field from `x86_64-unknown-none`; deltas: non-PIE
static `ET_EXEC` (`position-independent-executables false`,
`relocation-model static`, `code-model small`, `dynamic-linking false`),
`exe-suffix .elf`, `panic-strategy abort`. (`os`/`env`/`vendor` are
intentionally omitted: nightly-2026-10-12 accepts the file without them
and the E2E build proves it; do not re-add without re-running the build.)
Keeps `rust-lld`, soft-float/no-SSE features, `disable-redzone`,
`singlethread`. Two nightly-JSON gotchas baked in: `target-pointer-width`
is a NUMBER (string rejected), and `eh-frame`/`stack-protector` are not
valid fields (removed).

## 4. Crate (`user/rust-hello`)

Single binary crate, zero dependencies, `panic = "abort"` in dev and
release, release `opt-level = "z"`, `strip = true`, `lto = true`
(904-byte binary, fits the 64 KiB `/tmp` cap with headroom).

`src/main.rs`: `#![no_std] #![no_main]`, `global_asm!` `_start` (align
stack → `call rust_main` → `EXIT(rax)` → `hlt`, mirrors `crt0.S`),
`rust_main` (WRITE hello, return 0), `panic_handler` → `EXIT(1)`.

### ABI shims

Single source of truth: `src/libc/syscall.h` (`int $0x80`, `rax` = nr,
`rbx`/`rcx`/`rdx`/`rsi` = args). Rust `asm!` FORBIDS `rbx`
(LLVM-reserved), so the trap is a `global_asm!` trampoline (`sys_trap4`:
SysV `rdi/rsi/rdx/rcx` → NexIOS `rax/rbx/rcx/rdx`, manual push/pop rbx,
stack stays 16-aligned).

- `WRITE = 13` (fd, buf, len) — the hello-print vehicle.
- `EXIT = 6` (code).
- CORRECTION to the issue text: `KLOG (48)` is a dmesg READ/clear
  (`sys_klog`), NOT console print. `PRINT (4)` is a stub returning 0.
  Console output for user tasks is `WRITE` on fd 1 (loader-wired console).

## 5. Layout proof

```
$ x86_64-elf-readelf -lW build/user/rust-hello.elf
Elf file type is EXEC (Executable file)
Entry point 0x20114c            (= _start, verified by disassembly)
LOAD  0x200000  R      (headers)
LOAD  0x20113c  R E    (text; max end 0x201183)
PHDR / GNU_STACK only extras — no INTERP, no DYNAMIC.
```

Loader windows (`src/lib/constants.hpp`, `elf.cpp:292-321`): every LOAD
end `< HEAP_VADDR (0x60000000)` with a red-zone page to spare; heap
`0x60000000+1MiB`, stack at `0x70000000`. The link pins
`-z max-page-size=0x1000` (lld default is already 4K; explicit per #104).

## 6. Build, stage, run

```bash
meson compile user-rust-hello -C build/meson-x86_64   # → build/user/rust-hello.elf (904 B)
```

Staging for the E2E used ephemeral initrd `/bin` residence (a temporary
`cp` line in the CPIO rule, reverted afterwards — the binary is NOT a
permanent initrd resident; no test pins initrd contents for it). `/tmp`
tmpfs also fits (64 KiB cap) via the XMODEM upload path if preferred.

```
$ loadelf /bin/rust-hello.elf
[INFO]  loading /bin/rust-hello.elf completed successfully in 0.008s
$ runelf
Started task #10
Hello from Rust on NexIOS
$ dmesg
[DMESG ...]: INFO ELF 8001  ELF load started: loading /bin/rust-hello.elf ...
[DMESG ...]: INFO ELF 8002  ELF load completed: ... completed successfully ...
[DMESG ...]: INFO USERSPACE 9004 Task started: rust-hello.elf ...
[DMESG ...]: INFO USERSPACE 9001 Task exited: Task #10 'rust-hello.elf' exited (code 0)
```

Acceptance (manual E2E, no kernel tests): KLOG... WRITE-printed hello on
console, exit code 0, `ELF 8002` + `USERSPACE 9001` in dmesg. dmesg
subsystem renders as `USERSPACE` (not `USER`).

## 7. Limits & follow-ups

core-only (`alloc`/`std` unsupported — no heap runtime wired);
`panic = "abort"` (a panic exits 1, no message); x86_64-only;
TLS/shared-lib support follows #74/#95. Syscall-number drift is guarded
kernel-side by `syscall_abi_surface_mirror_and_version`
(`test_syscall.cpp`, pins KLOG=48); the crate cites `syscall.h` line
numbers as its source of truth (a Rust-side mirror test is follow-up
work, not this issue).
