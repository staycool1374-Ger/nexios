# CLion Setup + Live GDB Debugging

How to open the NexIOS project in CLion (native Meson support since
2023.3 — open `meson.build`, no Make plugin, no CMake indexing shim)
and debug the running kernel live through QEMU's GDB stub. CLI-only flows
(batch surveillance, panic capture, custom commands) stay in
`docs/debugging.md` — read that first if you haven't. The live-debug flow
(QEMU `-s -S` + Remote Debug) is UNCHANGED by the build-system switch —
only the commands that produce the binaries changed.

## 1. Prerequisites

Install once. The table below is what the build actually invokes;
`cross/<arch>-elf.txt` pins the same triplets for Meson.

| Tool | macOS (Homebrew) | Linux (Debian/Ubuntu) | Used for |
|------|------------------|------------------------|----------|
| QEMU | `qemu` | `qemu-system-x86 qemu-system-arm qemu-system-misc` | all targets |
| ISO tools | `xorriso grub` | `xorriso grub-pc` | `grub-mkrescue` ISO build |
| x86_64 toolchain | `x86_64-elf-gcc nasm` | `gcc-x86-64-linux-gnu` + `nasm` | kernel + bootloader |
| aarch64 toolchain | `aarch64-elf-gcc` | `gcc-aarch64-linux-gnu` | off-x86 builds |
| riscv64 toolchain | `riscv64-elf-gcc` | `gcc-riscv64-linux-gnu` | off-x86 builds |
| GDB per arch | `x86_64-elf-gdb aarch64-elf-gdb riscv64-elf-gdb` | `gdb-multiarch` | stub clients |
| Helpers | `coreutils expect llvm` | `coreutils expect` | scripted runs, `clang-tidy` |
| Meson + ninja | `pip install meson==1.12.0 ninja` | `pip install meson==1.12.0 ninja` (never apt: apt's 1.3.2 fails the freestanding sanity check) | build system |

```bash
# macOS, one line:
brew install qemu xorriso grub x86_64-elf-gcc aarch64-elf-gcc riscv64-elf-gcc \
  x86_64-elf-gdb aarch64-elf-gdb riscv64-elf-gdb nasm coreutils expect llvm
pip install 'meson==1.12.0' 'ninja==1.13.2'
```

Verify the three binaries your debug sessions will use:

```bash
which qemu-system-x86_64 x86_64-elf-gdb nc expect
```

## 2. First build

```bash
git clone <repo> nexios && cd nexios
meson setup build/meson-x86_64 --cross-file cross/x86_64-elf.txt -Darch=x86_64
tools/meson/build.sh build/meson-x86_64   # style gate + compile + ISO
```

This produces `build/meson-x86_64/src/nexios-rtos.iso` and
`build/meson-x86_64/src/kernel-debug.elf`.
`build.sh` exiting 0 with `Errors: 0` is the definition of "it works" —
see `prompts/AGENTS-KERNEL-BRIEFING.md` §6 for every build/test target
and `tools/meson/build.sh --help` for the Meson command map.

Repeat per arch with `cross/aarch64-elf.txt` (`build/meson-aarch64`)
and `cross/riscv64-elf.txt` (`build/meson-riscv64`). One builddir per
arch — never reconfigure a builddir to another arch in place.

## 3. Open the project in CLion

1. **File → Open** → select `meson.build` (or the project folder with
   the Meson plugin enabled). **Do not create a fresh native configure:**
   CLion must open the *same configured builddir* you set up on the CLI
   (Settings → Meson → existing build directory). A fresh IDE-side
   configure without the cross file produces a native build that fails
   confusingly — the CLI `meson setup --cross-file …` line above is the
   source of truth, the IDE mirrors it.
2. **Toolchain:** the default (Xcode clang on macOS) is fine for editing
   and navigation. The real build runs through `meson compile`
   (terminal or the Meson tool window) — cross-compilation happens there,
   not through the IDE toolchain.
3. **Formatting:** the repo root `.clang-format` (80 cols, 4-space,
   Allman braces) is picked up by CLion automatically. Reformat before
   committing.
4. **Red squiggles ≠ build errors.** CLion evaluates headers in one arch
   config at a time, so arch-gated code for the *other* arches shows
   false errors. Open the matching `compile_commands.json`
   (`build/meson-<arch>/compile_commands.json`) to silence the wrong-arch
   noise — one of the concrete Meson wins over the old setup.
5. **Running targets from the IDE** (optional): the Meson tool window
   lists `check-style`, `debug-test`, `debug-shell` and the rest as
   run targets. The full output and test-history discipline live in the
   terminal workflows, so prefer the terminal for anything beyond a
   quick build.

## 4. Live debugging through the GDB stub

Principle: QEMU's `-s` flag exposes a gdbserver-compatible stub on
TCP `:1234`; `-S` freezes the CPU at startup so you can breakpoint
*before* anything runs. CLion's **Remote Debug** configuration attaches
to that stub — QEMU is the "remote", no `gdbserver` binary involved.

### 4.1 Build with debug info, no LTO

```bash
meson setup build/meson-x86_64-nolto --cross-file cross/x86_64-elf.txt \
  -Darch=x86_64 -Db_lto=false
meson compile -C build/meson-x86_64-nolto
printf 'none\n' > initrd/tests/test-config.txt   # boot to shell, not a test
```

or the one-command equivalent:

```bash
python3 tools/meson/gdb_run.py debug-test --arch x86_64 --build debug \
  --class none --gdb-script tools/gdb/test-batch.gdb     # batch, exits on panic
python3 tools/meson/gdb_run.py debug-shell --arch x86_64 --build debug \
  --gdb-script tools/gdb/init.gdb --cmds cmds.txt        # scripted interactive
```

`NO_LTO` is what the GDB flows use internally — LTO inlines and
reorders enough to make single-stepping misleading. Always debug
NO_LTO builds.

### 4.2 Launch frozen QEMU (terminal 1)

Same flags as interactive mode (`QEMU_FLAGS_INTERACTIVE` equivalent),
plus stub, freeze, and a serial you can type into:

```bash
qemu-system-x86_64 -cdrom build/meson-x86_64-nolto/src/nexios-rtos.iso -m 256M \
  -boot order=d \
  -drive if=pflash,format=raw,readonly=on,file=/opt/homebrew/share/qemu/edk2-x86_64-code.fd \
  -cpu max \
  -drive file=build/meson-x86_64-nolto/packaging/fat32.img,format=raw,if=ide,index=1,media=disk \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,disable-legacy=on \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -serial mon:stdio -display none -no-reboot -s -S
```

The terminal now shows the QEMU monitor/serial mux and waits — the CPU
is frozen. Confirm the stub is up:

```bash
nc -z localhost 1234 && echo STUB-UP
```

Notes:

- There is **no host watchdog** on a manually launched QEMU (the stall
  monitor only wraps harness-driven runs), so you can sit on a
  breakpoint as long as you like.
- `Ctrl-A` is the QEMU escape prefix on `mon:stdio` (`Ctrl-A x` quits).
- To stop QEMU afterwards, kill it scoped to this checkout —
  never bare `pkill qemu` on a shared host:
  `pkill -f "qemu-system-x86_64.*nexios-rtos"`.

### 4.3 Create the CLion Remote Debug configuration

Use **Remote Debug** (works with any build system, symbol file + manual
launch — this is the QEMU-stub case). *Not* "Remote GDB Server", which
wants to upload-and-launch a userspace binary itself.

1. **Run → Edit Configurations → + → Remote Debug.** Name it
   `nexios-x86_64-stub`.
2. **Debugger:** pick `x86_64-elf-gdb` (add under Settings → Build,
   Execution, Deployment → Toolchains as a custom GDB if missing).
   CLion's bundled multiarch GDB also works, but the cross GDB matches
   the debug flow exactly.
3. **`'target remote' args:** `127.0.0.1:1234`
4. **Symbol file:** `$ProjectFileDir$/build/meson-x86_64-nolto/src/kernel-debug.elf`
   (absolute path works too). Rebuild → **reconnect**: this config
   does not sync files; a stale ELF means breakpoints land nowhere.
5. **Run → Debug** (or Shift+F9). CLion connects; the guest is still
   frozen — nothing runs until you press Resume.

### 4.4 Debug like a local program

- Breakpoints: gutter-click by file:line, or symbol
  (`kernel::Scheduler::reschedule`, `panic`, `handle_interrupt_c` —
  full table in `docs/debugging.md`).
- In the **Debugger → Console** tab, load the kernel helpers (once per
  session — do *not* source `tools/gdb/init.gdb`, it issues its own
  `target` command and fights the IDE connection):

  ```gdb
  source tools/gdb/kernel.py
  tasks
  regs
  panicinfo
  ```

  This gives you the `tasks` / `task <n>` / `regs` / `pml4` /
  `panicinfo` / `step-into|over|out` / `trace-syscall` commands from
  `docs/debugging.md`.
- **Serial stays live:** type shell commands in terminal 1 while
  stopped/running in CLion — breakpoints hit, serial keeps flowing.
- **Variables/pretty-printers:** `kernel::TaskControlBlock` and friends
  auto-format (see `docs/debugging.md`).
- **Watchpoints:** software breakpoints are safe; *hardware* watchpoints
  stall QEMU TCG (the guest goes silent with no output) — prefer
  conditional breakpoints, or `lldb`+file-backed flows for stray-write
  hunts (see briefing gotcha on lldb watchpoints).
- **Detaching** leaves QEMU running (frozen or live). Kill it with the
  scoped `pkill` above; a re-run needs a fresh QEMU *and* a CLion
  reconnect.

## 5. Other architectures

Same recipe, different launcher + GDB + symbol file. One CLion
configuration per arch (only the Debugger, symbol path, and port stay
constant — the stub is always `:1234`):

| Arch | QEMU launch (frozen, serial on stdio) | CLion Debugger | Symbol file |
|------|----------------------------------------|----------------|-------------|
| aarch64 | `qemu-system-aarch64 -machine virt -cpu cortex-a72 -kernel build/meson-aarch64/src/kernel-debug.elf -m 256M -serial mon:stdio -display none -s -S` | `aarch64-elf-gdb` (`brew install aarch64-elf-gdb`) | `build/meson-aarch64/src/kernel-debug.elf` |
| riscv64 | `qemu-system-riscv64 -machine virt -bios default -kernel build/meson-riscv64/src/kernel-riscv64.bin -m 256M -serial mon:stdio -display none -s -S` | `riscv64-elf-gdb` | `build/meson-riscv64/src/kernel-debug.elf` (symbols always come from the ELF, even though QEMU loads the raw binary) |

Build each arch first (`tools/meson/build.sh build/meson-aarch64`, …),
bake `test_class=none` as in §4.1. No batch-mode hardware
watchpoints on TCG for either arch — same stall rule as x86_64.

## 6. Troubleshooting

| Symptom | Cause → fix |
|---------|-------------|
| `nc -z localhost 1234` fails | QEMU died at startup — check the ISO exists (`ls build/meson-x86_64-nolto/src/*.iso`), the edk2 path (§4.2), and free port 1234 (`lsof -i :1234`; two checkouts fight over one stub). |
| CLion connects but breakpoints never hit | Stale symbol file (rebuild → reconnect), wrong-arch ELF, wrong builddir (main vs `-nolto`), or code compiled with LTO — rebuild the NO_LTO twin. |
| Guest runs but serial is silent | `test-config.txt` still names a test class, or a previous crashed run left the UART wedged — reset the file to `none`, restart QEMU. |
| `source tools/gdb/kernel.py` errors in CLion console | Run it from the project root (relative path) or use the absolute path; needs GDB with Python (cross GDBs ship it). |
| QEMU eats keystrokes / `Ctrl-C` kills the wrong thing | `mon:stdio` owns the terminal: `Ctrl-A` is the escape prefix, `Ctrl-A x` quits QEMU cleanly. |
| `gdb` / `test-gdb` shortcuts from old notes fail | Those Make targets are gone post-cutover — use `python3 tools/meson/gdb_run.py debug-test …` (batch) and `… debug-shell …` (scripted interactive). |

## 7. Further reading

- `docs/debugging.md` — CLI flows, panic/page-fault recipes, breakpoint table.
- `tools/gdb/test-batch.gdb` — what the automated surveillance asserts.
- `tools/gdb/init.gdb` — CLI equivalent of §4 (auto-connect + helpers).
- `tools/chains/*.json` — panic-capture, ring-dump, shell-smoke chains for `tools/chainrun.py`.
- `prompts/AGENTS-KERNEL-BRIEFING.md` §6 — build/test target reference.
