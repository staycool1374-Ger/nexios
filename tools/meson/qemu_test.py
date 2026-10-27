#!/usr/bin/env python3
"""Single-class QEMU runner for the Meson build (issue #170, Phase 4).

Mirrors `make execute-test` / `_do_execute_test` / `_run_test_qemu`
(Makefile:1123/1136/1446) and the run_all_classes.sh verdict contract
exactly, so existing history/test tooling keeps working unchanged:

  python3 tools/meson/qemu_test.py --arch ARCH --build debug|release
      --class CLASS [--builddir DIR] [--dump-counts]

Steps: resolve policy via tools/test-policy.awk (single source of truth),
map selftest, write test-config, `meson configure -Dtest_class` +
`meson compile`, assert the baked config, boot the built image
(builddir ISO for x86_64, ELF/BIN paths otherwise), run expect +
watchdog with stall-retry, print the Make-compatible verdict block.
Exit 0 on PASS, 1 otherwise. `none` is refused (use the run_* targets).
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
POLICY_TSV = REPO / "tools" / "test-policy.tsv"
POLICY_AWK = REPO / "tools" / "test-policy.awk"
RUN_TEST_EXP = REPO / "tools" / "run-test.exp"
QEMU_FILE_LOG = REPO / "tools" / "qemu-file-log.sh"
GEN_TEST_CONFIG = REPO / "tools" / "meson" / "gen_test_config.py"
TEST_HISTORY = REPO / "tools" / "meson" / "test_history.py"

SELFTEST_CLASSES = {
    "x86_64": ["safe"],
    "aarch64": ["arch_aarch64", "configuration_build", "hal",
                "hal_timer", "irq_early_std", "memory_page_tables",
                "memory_vmm", "timing_core"],
    "riscv64": ["configuration_build", "hal_timer", "irq_early_std",
                "memory_page_tables", "memory_vmm", "timing_core",
                "scheduler_zombie"],
}

QEMU_SYSTEM = {
    "x86_64": "qemu-system-x86_64",
    "aarch64": "qemu-system-aarch64",
    "riscv64": "qemu-system-riscv64",
}


def policy(arch, cls, key):
    """Resolve one policy value via awk (fail loud on empty)."""
    out = subprocess.run(
        ["awk", "-f", str(POLICY_AWK), "-v", "arch=%s" % arch,
         "-v", "class=%s" % cls, "-v", "key=%s" % key,
         str(POLICY_TSV)],
        capture_output=True, text=True, check=False,
    )
    value = out.stdout.strip()
    if out.returncode != 0 or not value:
        raise SystemExit("ERROR: test-policy: no %s for class=%s ARCH=%s"
                         % (key, cls, arch))
    return value


def checkout_tag():
    """Per-checkout tag (mirrors Make CHECKOUT_TAG + issue #372 rules)."""
    name = REPO.name
    tag = re.sub(r"[^A-Za-z0-9_]", "_", name) or "checkout"
    return tag


def find_ovmf():
    """Locate OVMF firmware (mirrors the Makefile/OVMF_CODE search)."""
    candidates = [
        "/opt/homebrew/Cellar/qemu",
        "/usr/share/qemu",
        "/usr/local/share/qemu",
    ]
    for base in candidates:
        for path in sorted(Path(base).rglob("edk2-x86_64-code.fd")):
            return str(path)
    # Ubuntu ovmf package layout has no edk2-*.fd names: probe the real
    # CODE-image paths the distro package installs (mirrors the CI
    # "Locate OVMF firmware" step; observed on ubuntu-24.04 runners:
    # /usr/share/ovmf/OVMF.fd present, run 40526115948). Exact edk2
    # name above stays preferred; these are same-shape CODE fallbacks.
    # No _secboot variants: an enforced-secure-boot image would refuse
    # the unsigned test ISO.
    for path in (
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
        "/usr/share/ovmf/OVMF.fd",
        "/usr/share/qemu/OVMF.fd",
        "/usr/share/OVMF/OVMF.fd",
    ):
        if Path(path).is_file():
            return path
    return None
    return ""


def _fb_devices():
    """virtio-gpu + ramfb unless disabled via environment (Make parity).

    QEMU_FB_GPU / QEMU_FB_RAMFB unset (or set to the default device spec)
    keep the backend; setting either to the empty string disables it.
    """
    args = []
    for var, default in (("QEMU_FB_GPU", "-device virtio-gpu-device"),
                         ("QEMU_FB_RAMFB", "-device ramfb")):
        spec = os.environ.get(var, default)
        if spec.strip():
            args += spec.split()
    return args


def qemu_argv(arch, build, cls, paths, tag):
    """Assemble the QEMU command (mirrors per-arch QEMU_FLAGS blocks)."""
    serial_log = paths["serial"]
    com1_log = paths["com1"]
    args = [QEMU_SYSTEM[arch]]
    if arch == "x86_64":
        ovmf = find_ovmf()
        # NOTE: the file backend cannot multiplex: unlike the stdio mux,
        # one chardev cannot serve both serial and debugcon. Mirror the
        # Make FILE transform exactly — serial goes direct to its file,
        # debugcon keeps the file chardev.
        if not ovmf and cls in ("ahci_live", "iommu_live"):
            raise SystemExit(
                "ERROR: %s requires OVMF edk2-x86_64-code.fd, none found"
                % cls)
        if cls == "iommu_live":
            # Live VT-d variant: q35 + intel-iommu, UEFI CD, no
            # ahci/fat32/net (disk-free test class).
            args += ["-cdrom", paths["iso"], "-m", "256M",
                     "-chardev", "file,path=%s,id=dbg" % serial_log,
                     "-serial", "file:%s" % paths["com1"],
                     "-device", "isa-debugcon,chardev=dbg",
                     "-monitor", "none",
                     "-machine", "q35", "-cpu", "max",
                     "-drive", "if=pflash,format=raw,readonly=on,file=%s" % ovmf,
                     "-device", "intel-iommu,intremap=off,dma-translation=on"]
        elif cls == "ahci_live":
            # Real AHCI variant: q35 + ICH9 stack, scratch disk, no FAT32.
            scratch = paths["scratch"]
            if not Path(scratch).exists():
                subprocess.run(
                    ["dd", "if=/dev/zero", "of=%s" % scratch,
                     "bs=1M", "count=64", "status=none"], check=True)
            args += ["-m", "256M",
                    "-chardev", "file,path=%s,id=dbg" % serial_log,
                    "-serial", "file:%s" % paths["com1"],
                    "-device", "isa-debugcon,chardev=dbg",
                    "-monitor", "none",
                    "-machine", "q35,sata=off", "-cpu", "max",
                     "-drive", "if=pflash,format=raw,readonly=on,file=%s" % ovmf,
                     "-device", "ich9-ahci,id=ahci0",
                     "-drive", "file=%s,if=none,id=ahci_cd,media=cdrom,readonly=on" % paths["iso"],
                     "-device", "ide-cd,drive=ahci_cd,bus=ahci0.0,unit=0",
                     "-drive", "file=%s,if=none,id=ahci_scratch,format=raw" % scratch,
                     "-device", "ide-hd,drive=ahci_scratch,bus=ahci0.1,unit=0"]
        else:
            # Base: default pc machine (SeaBIOS reads the ISO; q35 is
            # ahci/iommu-only) + OVMF pflash + FAT32 IDE drive.
            if not ovmf:
                raise SystemExit("ERROR: x86_64 runs require OVMF firmware")
            args += ["-cdrom", paths["iso"], "-m", "256M",
                     "-chardev", "file,path=%s,id=dbg" % serial_log,
                     "-serial", "file:%s" % paths["com1"],
                     "-device", "isa-debugcon,chardev=dbg",
                     "-monitor", "none",
                     "-netdev", "user,id=net0",
                     "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
                     "-boot", "order=d",
                     "-drive", "if=pflash,format=raw,readonly=on,file=%s" % ovmf,
                     "-cpu", "max",
                     "-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % paths["fat32"],
                     "-device", "virtio-rng-pci,disable-legacy=on",
                     "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        if cls in ("iommu_live", "ahci_live"):
            # The variant blocks carry no debug-exit device; the test
            # runner appends it unconditionally (mirrors _run_test_qemu).
            args += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    elif arch == "aarch64":
        args += ["-kernel", paths["kernel_elf"], "-m", "256M",
                 "-serial", "mon:stdio",
                 "-netdev", "user,id=net0",
                 "-device", "virtio-net-device,netdev=net0"]
        if Path(paths["fat32"]).exists():
            args += ["-drive", "file=%s,if=none,id=vblk0,format=raw" % paths["fat32"],
                     "-device", "virtio-blk-device,drive=vblk0"]
        # Framebuffer backend (mirrors QEMU_FB_GPU/QEMU_FB_RAMFB, empty
        # disables): without a display the Terminal never initialises and
        # capture stays dead.
        args += _fb_devices()
        # Single machine block (mirrors Makefile:431-435): -machine/-cpu/
        # -display/-no-reboot/-dtb/-semihosting each exactly once.
        args += ["-device", "virtio-rng-pci,disable-legacy=on",
                 "-machine", "virt", "-cpu", "cortex-a72",
                 "-display", "none", "-no-reboot",
                 "-dtb", paths["dtb"],
                 "-semihosting-config", "enable=on,target=native"]
    else:
        args += ["-kernel", paths["kernel_bin"], "-m", "256M",
                 "-serial", "mon:stdio",
                 "-netdev", "user,id=net0",
                 "-device", "virtio-net-device,netdev=net0"]
        if Path(paths["fat32"]).exists():
            args += ["-drive", "file=%s,if=none,id=vblk0,format=raw" % paths["fat32"],
                     "-device", "virtio-blk-device,drive=vblk0"]
        args += _fb_devices()
        args += ["-device", "virtio-rng-pci,disable-legacy=on",
                 "-machine", "virt", "-bios", "default",
                 "-display", "none", "-no-reboot"]
    if cls in ("smp_bringup", "smp_sched", "smp_multicpu"):
        args += ["-smp", "2"]
    if os.environ.get("HRT_ICOUNT"):
        args += ["-icount", "shift=0,sleep=off"]
    if os.environ.get("HRT_TRACE"):
        args += ["-trace", "events=tools/qemu/trace_events.txt"]
    args += ["-name", "jarvis-%s" % tag]
    return args


class Watchdog(threading.Thread):
    """Kill a stalled QEMU (mirrors the Make monitor subshell)."""

    def __init__(self, serial_log, stall_limit, tag, qemu_bin, stop):
        super().__init__(daemon=True)
        self.serial_log = serial_log
        self.stall_limit = stall_limit
        self.pattern = re.compile(
            re.escape(qemu_bin) + r".*[ -]name[ =]jarvis-"
            + re.escape(tag) + r"([^A-Za-z0-9_]|$).*nexios-rtos|"
            r"nexios-rtos.*[ -]name[ =]jarvis-"
            + re.escape(tag) + r"([^A-Za-z0-9_]|$)")
        self.stop = stop
        self.tripped = False

    def _size(self):
        try:
            return Path(self.serial_log).stat().st_size
        except OSError:
            return 0

    def _pkill_scoped(self):
        try:
            out = subprocess.run(["pgrep", "-f", "qemu-system"],
                                 capture_output=True, text=True).stdout
        except OSError:
            return
        for line in out.splitlines():
            pid = line.strip()
            if not pid.isdigit():
                continue
            try:
                cmd = open("/proc/%s/cmdline" % pid, "rb").read().replace(
                    b"\0", b" ").decode("utf-8", "ignore")
            except OSError:
                try:
                    cmd = subprocess.run(
                        ["ps", "-p", pid, "-o", "args="],
                        capture_output=True, text=True).stdout
                except OSError:
                    continue
            if self.pattern.search(cmd):
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except OSError:
                    pass

    def run(self):
        last = -1
        stall = 0
        while not self.stop.is_set():
            time.sleep(1)
            if self.stop.is_set():
                break
            current = self._size()
            if current == last:
                stall += 1
                if stall >= self.stall_limit and not self.tripped:
                    self.tripped = True
                    line = ("[HOST-WATCHDOG] Tests interrupted — no serial "
                            "output for %d s\n" % self.stall_limit)
                    for path in (self.serial_log,
                                 self.serial_log.replace("serial", "verdict")):
                        try:
                            with open(path, "a",
                                       encoding="utf-8") as handle:
                                handle.write(line)
                        except OSError:
                            pass
                    self._pkill_scoped()
                    return
            else:
                stall = 0
                last = current


def ensure_fresh_builddir(builddir, arch, build):
    """Wipe on LTO/buildtype mismatch (A2 canary: never mix objects).

    Compares build/.lto-mode-<arch> against the configured b_lto; on
    mismatch the builddir is wiped and reconfigured from the committed
    cross file. A missing marker on an unbuilt tree is normal (postlink
    writes it) — no wipe.
    """
    builddir = Path(builddir)
    if not (builddir / "build.ninja").exists():
        subprocess.run(
            ["meson", "setup", str(builddir), "--cross-file",
             "cross/%s-elf.txt" % arch, "-Darch=%s" % arch],
            cwd=str(REPO), check=True)
    out = subprocess.run(
        ["meson", "introspect", str(builddir), "--buildoptions"],
        capture_output=True, text=True, check=True)
    live_lto = None
    for option in json.loads(out.stdout):
        if option.get("name") == "b_lto":
            live_lto = "on" if option.get("value") else "off"
    marker = builddir / (".lto-mode-%s" % arch)
    if marker.exists() and live_lto is not None:
        want = "lto=%s buildtype=%s" % (live_lto, build)
        if marker.read_text(encoding="utf-8").strip() != want:
            print("[HOST] LTO/mode marker mismatch (want %r) — wiping %s"
                  % (want, builddir))
            shutil.rmtree(builddir, ignore_errors=True)
            subprocess.run(
                ["meson", "setup", str(builddir), "--cross-file",
                 "cross/%s-elf.txt" % arch, "-Darch=%s" % arch],
                cwd=str(REPO), check=True)


def pump_output(proc, log_path, stop):
    """Stream a child pipe to the per-class log and our own stdout.

    Binary-safe (serial carries arbitrary bytes); terminal gets
    errors-replaced text. Host-side only — the guest serial path stays
    file-backed (issue #345).
    """
    with open(log_path, "ab", buffering=0) as log_fh:
        out = getattr(sys.stdout, "buffer", sys.stdout)
        while True:
            chunk = proc.stdout.read1(65536) if hasattr(proc.stdout, "read1") else proc.stdout.read(65536)
            if not chunk:
                break
            try:
                log_fh.write(chunk)
            except OSError:
                pass
            try:
                out.write(chunk)
                out.flush()
            except (OSError, ValueError):
                pass
    stop.set()


def run_attempt(arch, build, cls, builddir, paths, tag, logdir):
    """One QEMU attempt: configure, build, boot, watch, verdict."""
    requested = resolve_classes(arch, build, cls)
    ensure_fresh_builddir(builddir, arch, build)
    configure_cmd = ["meson", "configure", "-Dtest_class=%s" % ",".join(requested)]
    if build == "release":
        # Mirrors Make: release links without LTO (no -flto in release
        # LDFLAGS; avoids the LTO-plugin/static-init hazards — e.g. an
        # inline-only memset with no out-of-line definition, latent
        # follow-up if LTO releases are ever wanted).
        configure_cmd += ["-Db_lto=false"]
    configure_cmd.append(str(builddir))
    subprocess.run(configure_cmd, check=True)
    subprocess.run(["meson", "compile", "-C", str(builddir)], check=True)
    assert_baked(arch, builddir, requested)
    if arch == "x86_64":
        iso = builddir / "src" / "nexios-rtos.iso"
        if not iso.exists():
            raise SystemExit("ERROR: missing built ISO %s" % iso)
        paths = dict(paths, iso=str(iso))

    expect_timeout = int(policy(arch, cls, "expect_timeout"))
    watchdog_stall = int(policy(arch, cls, "watchdog_stall"))
    max_attempts = int(policy(arch, cls, "max_attempts"))
    stall_sigs = [s for s in policy(arch, cls, "stall_signatures").split(",")
                  if s]

    qemu_bin = QEMU_SYSTEM[arch]
    if shutil.which(qemu_bin) is None:
        print("QEMU missing: %s" % qemu_bin)
        return 2, "no-qemu"
    if shutil.which("expect") is None:
        print("expect missing.")
        return 2, "no-expect"

    print("  %-7s %s…" % ("TEST", "Running %s class=%s" % (build, cls)))
    verdict = ""
    stall_seen = False
    stall_status = ""
    attempt = 0
    start_all = time.time()
    while attempt < max_attempts:
        attempt += 1
        for key in ("serial", "com1", "verdict", "start_ts", "end_ts"):
            try:
                Path(paths[key]).unlink()
            except OSError:
                pass
        Path(paths["start_ts"]).write_text("%d" % int(time.time()),
                                           encoding="utf-8")
        stop = threading.Event()
        monitor = Watchdog(paths["serial"], watchdog_stall, tag,
                           qemu_bin, stop)
        monitor.start()
        cmd = qemu_argv(arch, build, cls, paths, tag)
        expect_cmd = (["expect", str(RUN_TEST_EXP), str(expect_timeout),
                       paths["verdict"], paths["serial"], "sh",
                       str(QEMU_FILE_LOG), paths["serial"], paths["com1"]]
                      + cmd)
        class_log = os.path.join(logdir, "%s.log" % cls)
        if attempt == 1:
            try:
                os.unlink(class_log)
            except OSError:
                pass
        pump_stop = threading.Event()
        proc = subprocess.Popen(
            expect_cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        pump = threading.Thread(target=pump_output,
                                args=(proc, class_log, pump_stop),
                                daemon=True)
        pump.start()
        proc.wait()
        pump_stop.wait(timeout=10)
        pump.join(timeout=10)
        Path(paths["end_ts"]).write_text("%d" % int(time.time()),
                                         encoding="utf-8")
        stop.set()
        monitor.join(timeout=5)
        scoped_pkill(qemu_bin, tag)
        try:
            verdict = Path(paths["verdict"]).read_text(
                encoding="utf-8").strip()
        except OSError:
            verdict = "RESULT: UNKNOWN (no verdict file — expect aborted)"
        if verdict.startswith("RESULT: PASS"):
            if stall_seen:
                stall_status = "STALL-RECOVERED"
            break
        if any(verdict.startswith("RESULT: %s" % sig)
               for sig in stall_sigs):
            if attempt < max_attempts:
                print("[HOST] STALL attempt %d/%d: %s — retrying "
                      "identical binary" % (attempt, max_attempts, verdict))
                stall_seen = True
                continue
            stall_status = "STALL-UNRECOVERED"
            verdict = ("RESULT: FAIL (stall unrecovered after %d "
                       "attempts: %ds, zero-S-line TIMEOUT/QEMU_EXIT)"
                       % (max_attempts, expect_timeout))
            break
        break
    elapsed = int(time.time() - start_all)
    print(verdict)
    print("[HOST] start=%ds end=%ds elapsed=%ds"
          % (start_all, start_all + elapsed, elapsed))
    if stall_status:
        print("[HOST] %s" % stall_status)
    if verdict.startswith("RESULT: PASS"):
        print("[HOST] TESTS PASSED")
        return 0, verdict
    print("[HOST] TESTS FAILED — see %s" % paths["serial"])
    print("VERDICT: %s" % verdict)
    return 1, verdict


def scoped_pkill(qemu_bin, tag):
    """Kill only this checkout's QEMU (issue #372 trailing-delimiter)."""
    pattern = re.compile(
        re.escape(qemu_bin) + r".*[ -]name[ =]jarvis-"
        + re.escape(tag) + r"([^A-Za-z0-9_]|$).*nexios-rtos|"
        r"nexios-rtos.*[ -]name[ =]jarvis-"
        + re.escape(tag) + r"([^A-Za-z0-9_]|$)")
    try:
        out = subprocess.run(["pgrep", "-af", "qemu-system"],
                             capture_output=True, text=True).stdout
    except OSError:
        return
    for line in out.splitlines():
        parts = line.split(None, 1)
        if not parts or not parts[0].isdigit():
            continue
        if pattern.search(line):
            try:
                os.kill(int(parts[0]), signal.SIGKILL)
            except OSError:
                pass


def resolve_classes(arch, build, cls):
    """Map the requested class to baked test-config lines (mirror Make)."""
    if cls != "selftest":
        return [cls]
    if arch == "x86_64":
        return ["safe"]
    if build == "release":
        raise SystemExit(
            "ERROR: no measured release selftest subset for ARCH=%s "
            "(all selftest classes PLANNED:0 in release)" % arch)
    return list(SELFTEST_CLASSES[arch])


def assert_baked(arch, builddir, requested):
    """Fail loud on a stale baked config (A3 pre-QEMU gate)."""
    baked = builddir / (".baked-test-config-%s" % arch)
    try:
        have = baked.read_text(encoding="utf-8").split()
    except OSError:
        raise SystemExit("ERROR: missing baked test-config %s — rebuild"
                         % baked)
    if sorted(have) != sorted(requested):
        raise SystemExit(
            "ERROR: baked test-config %s != requested %s — stale artifact, "
            "rebuild via qemu_test" % (have, requested))


def legacy_iso_path(builddir, build):
    """Source-tree ISO staged by the build (mirrors Make DEBUG_/RELEASE_ISO)."""
    if build == "release":
        return REPO / "release" / "nexios-rtos.iso"
    return REPO / "debug" / "nexios-rtos.iso"


def dump_counts(arch, build, builddir, paths, tag):
    """Boot once, capture TCOUNT lines, exit clean (mirror _run_dump_counts)."""
    requested = ["dump-counts"]
    configure_cmd = ["meson", "configure", "-Dtest_class=dump-counts"]
    if build == "release":
        configure_cmd += ["-Db_lto=false"]
    configure_cmd.append(str(builddir))
    subprocess.run(configure_cmd, check=True)
    subprocess.run(["meson", "compile", "-C", str(builddir)], check=True)
    if arch == "x86_64":
        iso = builddir / "src" / "nexios-rtos.iso"
        if not iso.exists():
            print("ERROR: no built ISO at %s" % iso)
            return 1
        paths = dict(paths, iso=str(iso))
    dump_log = Path("/tmp/jarvis-dump-counts-%s.log" % tag)
    try:
        dump_log.unlink()
    except OSError:
        pass
    cmd = qemu_argv(arch, build, "none", dict(paths), tag)
    serial_idx = None
    for i, token in enumerate(cmd):
        if token == "-serial" and i + 1 < len(cmd):
            serial_idx = i + 1
    if serial_idx is None:
        print("ERROR: QEMU flags have no -serial to redirect for dump-counts")
        return 1
    cmd[serial_idx] = "file:%s" % dump_log
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass
    finally:
        try:
            proc.kill()
        except OSError:
            pass
        proc.wait()
    print("=== Per-class test registration counts ===")
    found = False
    try:
        text = dump_log.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        text = ""
    for line in text.splitlines():
        if "[TCOUNT]" in line:
            print(line.strip())
            found = True
    if not found:
        print("(no TCOUNT lines found)")
    try:
        dump_log.unlink()
    except OSError:
        pass
    return 0


def classify_verdict(text, cls):
    """Map captured output to (rc, passed, failed, elapsed_ms, status).

    Single source of truth for the verdict contract (mirrors the
    run_all_classes.sh parsing). test_verdict_parser.py exercises it.
    """
    passed = failed = elapsed_ms = None
    match = re.search(r"^\s*PASSED:\s*(\d+)", text, re.M)
    if match:
        passed = match.group(1)
    match = re.search(r"^\s*FAILED:\s*(\d+)", text, re.M)
    if match:
        failed = match.group(1)
    match = re.search(r"^\s*TIME_ELAPSED_MS:\s*(\d+)", text, re.M)
    if match:
        elapsed_ms = match.group(1) + "ms"
    status = ""
    if passed is None or failed is None:
        passed, failed, elapsed_ms = "0", "0", "0ms"
        if re.search(r"RESULT: PASS \(expected panic", text):
            status = " STATUS: EXPECTED_PANIC_PASS"
        elif "No tests registered" in text and "task_fpu" in cls:
            status = " STATUS: NO_TESTS_REGISTERED"
        elif "STALL-UNRECOVERED" in text:
            status = " STATUS: STALL"
        elif re.search(r"kernel panic|page fault|triple fault|#PF|PANIC:",
                       text, re.IGNORECASE):
            status = " STATUS: PANIC"
        elif re.search(r"RESULT: TIMEOUT|Terminated: 15|\[STALL\]|watchdog",
                       text, re.IGNORECASE):
            status = " STATUS: TIMEOUT"
        else:
            status = " STATUS: NO_SUMMARY"
        if "EXPECTED_PANIC_PASS" in status or "NO_TESTS_REGISTERED" in status:
            return 0, passed, failed, elapsed_ms, status
        return 1, passed, failed, elapsed_ms, status
    if not elapsed_ms:
        elapsed_ms = "0ms"
    if "STALL-RECOVERED" in text and failed == "0":
        status = " STATUS: STALL-RECOVERED"
    rc = 0 if failed == "0" else 1
    return rc, passed, failed, elapsed_ms, status


def main():
    """CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Run one test class under QEMU (Meson flow).")
    parser.add_argument("--arch", required=True,
                        choices=["x86_64", "aarch64", "riscv64"])
    parser.add_argument("--build", required=True, choices=["debug", "release"])
    parser.add_argument("--class", dest="cls", default="none")
    parser.add_argument("--builddir", default=None)
    parser.add_argument("--dump-counts", action="store_true")
    parser.add_argument("--history", default="test-history.txt")
    parser.add_argument("--no-history", action="store_true")
    args = parser.parse_args()
    if args.cls == "none" and not args.dump_counts:
        print("qemu_test runs automated classes; 'none' is interactive "
              "(use the run_* targets).")
        return 2
    if args.build == "release" and args.arch != "x86_64":
        print("ERROR: release builds only supported on x86_64")
        return 2
    builddir = Path(args.builddir or
                    ("build/meson-%s-release" % args.arch
                     if args.build == "release"
                     else "build/meson-%s" % args.arch)).resolve()
    if not (builddir / "build.ninja").exists():
        print("ERROR: builddir %s not configured "
              "(meson setup --cross-file cross/%s-elf.txt -Darch=%s%s)"
              % (builddir, args.arch, args.arch,
                 " -Dbuildtype=release -Db_lto=false"
                 if args.build == "release" else ""))
        return 2
    tag = checkout_tag()
    logdir = Path("/tmp/jarvis-meson-%s" % tag)
    logdir.mkdir(parents=True, exist_ok=True)
    paths = {
        "serial": str(logdir / "serial.log"),
        "com1": str(logdir / "com1.log"),
        "verdict": str(logdir / "verdict.log"),
        "start_ts": str(logdir / "start.timestamp"),
        "end_ts": str(logdir / "end.timestamp"),
        "scratch": str(logdir / "ahci_scratch.img"),
        "fat32": str(builddir / "packaging" / "fat32.img"),
        "kernel_elf": str(builddir / "src" / "kernel-debug.elf"),
        "kernel_bin": str(builddir / "src" / "kernel-riscv64.bin"),
        "dtb": str(builddir / "virt.dtb"),
    }
    if args.dump_counts:
        return dump_counts(args.arch, args.build, builddir, paths, tag)
    log_path = logdir / ("%s.log" % args.cls)
    rc, _verdict = run_attempt(args.arch, args.build, args.cls,
                               builddir, paths, tag, logdir)
    with open(log_path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    rc, passed, failed, elapsed_ms, status = classify_verdict(text, args.cls)
    import datetime
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    row = "%s %s PASSED: %s FAILED: %s TIME: %s%s" % (
        stamp, args.cls, passed, failed, elapsed_ms, status)
    if not args.no_history:
        subprocess.run([sys.executable, str(REPO / "tools" / "meson" / "test_history.py"),
                        args.history, row], check=False)
    print(row)
    return rc


if __name__ == "__main__":
    sys.exit(main())
