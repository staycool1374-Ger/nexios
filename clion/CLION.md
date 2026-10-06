# CLion setup — Jarvis RTOS (x86_64)

`CMakeLists.txt` in this directory is **indexing only**. Builds, QEMU and
GDB all run through the repo Makefile — this folder adds IDE run
configurations (`.idea/runConfigurations/`, git-ignored by design) so the
whole loop works from inside CLion.

## 1. Open the project

File → Open → select the `clion/` directory (not the repo root).
Let CLion load the CMake project (indexing may take a few minutes).

## 2. PATH (macOS GUI apps miss Homebrew)

All run configurations already export
`PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin`
so `x86_64-elf-g++`, `qemu-system-x86_64` and `xorriso` resolve.
If you launch CLion from a terminal instead, it inherits your shell PATH
and the override is harmless.

No extra toolchain setup is needed for the make-based configs below —
they use the Makefile's own triplet. Only the native debugger needs the
cross GDB (see §4).

## 3. Build from the IDE

Run configurations (green arrow dropdown):

| Config           | Command       | Produces                        |
|------------------|---------------|---------------------------------|
| `make build`     | `make build`  | check-style + `debug/nexios-rtos.iso` |
| `make debug ISO` | `make debug`  | `debug/nexios-rtos.iso` only    |

Working directory for both is the repo root (`$PROJECT_DIR$/..`).
Build problems match the Makefile/g++ output 1:1.

## 4. GDB remote stub from the IDE (two steps)

**Step A — freeze the guest.** Run `QEMU frozen (GDB stub :1234)`
(`clion/qemu-frozen.sh`: interactive flags from the Makefile plus
`-s -S`). The guest halts at boot; serial stays in that run tool window
(`mon:stdio`, Ctrl+A then X exits). The script refuses to start if
another nexios QEMU holds `build/fat32.img`.

**Step B — attach the IDE debugger.** Run `GDB remote :1234` in **Debug**
mode. It uses `/opt/homebrew/bin/x86_64-elf-gdb` (not the bundled GDB),
symbols `build/kernel-debug.elf`, and sources `tools/gdb/init.gdb`
(backtrace hook + `tools/gdb/kernel.py` printers). If CLion reports an
unknown configuration type, recreate it manually: Run | Edit
Configurations | + | GDB Remote Debug with those same three values.

Then: `continue`, IDE breakpoints/watchpoints, evaluate, and
`source tools/gdb/<script>.gdb` in the GDB console
(`hang-capture.gdb`, `trace-*.gdb`, `watch_*.gdb` all work).

Limits you should know:

- QEMU hbreak watchpoints are capped at **4** (x86 DR registers).
- TCG speed on a loaded Mac makes wall-clock timing lie; deadline-miss
  spam under host load is usually the host, not the kernel (#280).
- Serial input goes to the QEMU run window, not the debugger console.
- Batch (non-interactive) GDB stays a terminal job:
  `make debug-test x86_64 debug <class> tools/gdb/<script>.gdb`.

## 5. Hunting #280-class stalls from here

1. `make debug ISO`, Step A, Step B, `continue`.
2. Reproduce (boot the test class or idle the shell until wedged).
3. On a wedge: pause in the IDE and read `current_task()` state,
   `scheduler_lock_`, `save_rsp_to`, runqueue membership, tick counter —
   or `source tools/gdb/hang-capture.gdb` for the scripted snapshot.
4. The reopen criterion for the load-driven verdict is a stall on an
   **idle** host: quit builders, LSP sidecars and browsers first
   (a runaway helper at ~100% CPU once faked a kernel freeze).

## Files here

- `CMakeLists.txt` — indexer project (never builds product code).
- `qemu-frozen.sh` — frozen-guest launcher used by the QEMU config.
- `CLION.md` — this file.
- `.idea/runConfigurations/` — the four configs above (ignored by git;
  re-created from this doc if lost).
