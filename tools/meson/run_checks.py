#!/usr/bin/env python3
"""Non-test run_target implementations (issue #170 Phase 6).

Usage:
    python3 tools/meson/run_checks.py <name>

Each entry mirrors the corresponding Make target with absolute paths
(run_targets execute with cwd=builddir, so no relative paths allowed).
Environment overrides where Make has them; see --help text.
"""

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent


def run(cmd, **kwargs):
    """Run a command from the repo root, echoing it first."""
    print("  %-7s %s" % ("RUN", " ".join(str(c) for c in cmd)))
    kwargs.setdefault("cwd", str(REPO))
    subprocess.run(cmd, check=True, **kwargs)


def check_config(_arch):
    """Mirror `make check-config` (validates nexios_config.h)."""
    run([sys.executable, str(REPO / "tools" / "check-config.py")],
        env={**os.environ, "ARCH": "x86_64"})


def test_policy_check(_arch):
    """Mirror `make test-policy-check` (awk validate + oracle tests)."""
    run(["awk", "-f", str(REPO / "tools" / "test-policy.awk"),
         "-v", "validate=1", str(REPO / "tools" / "test-policy.tsv")],
        cwd=str(REPO))
    run(["bash", str(REPO / "tools" / "test-policy-test.sh")],
        cwd=str(REPO))


def audit(_arch):
    """Mirror `make audit HPP=.. CPP=..` (needs MESON_AUDIT_HPP/CPP)."""
    hpp = os.environ.get("MESON_AUDIT_HPP", "")
    cpp = os.environ.get("MESON_AUDIT_CPP", "")
    if not hpp or not cpp:
        print("audit needs MESON_AUDIT_HPP and MESON_AUDIT_CPP "
              "(mirrors make audit HPP=.. CPP=..)")
        return 2
    run([sys.executable, str(REPO / "tools" / "audit_module.py"), hpp, cpp])
    return 0


def picolibc(_arch):
    """Mirror the `picolibc` target (sysroot build, pinned version)."""
    version = "1.8.12"
    lines = (REPO / "meson.options").read_text().splitlines()
    for i, line in enumerate(lines):
        if "picolibc_version" not in line:
            continue
        # The pin lives on the `value : 'x.y.z',` line after the option;
        # the option line itself carries no version token.
        for probe in lines[i:i + 2]:
            for token in probe.replace("'", " ").replace('"', " ").split():
                if token and token[0].isdigit() and "." in token:
                    version = token.rstrip(",")
    env = dict(os.environ, PICOLIBC_VERSION=version)
    run(["sh", str(REPO / "tools" / "build-picolibc.sh")], env=env,
        cwd=str(REPO))
    return 0


def user_rust_hello(_arch):
    """Mirror `make user-rust-hello` (x86_64 only, needs bootstrapped toolchain)."""
    if _arch != "x86_64":
        print("user-rust-hello: x86_64 only, skipping")
        return 0
    cargo_home = REPO / "build" / "cargo"
    cargo = cargo_home / "bin" / "cargo"
    if not cargo.exists():
        print("RUST toolchain not bootstrapped: run the one-time rustup-init "
              "command in docs/specs/rust-userspace.md section 2, then retry.")
        return 1
    env = dict(os.environ,
               RUSTUP_HOME=str(REPO / "build" / "rustup"),
               CARGO_HOME=str(cargo_home),
               PATH=str(cargo_home / "bin") + os.pathsep + os.environ.get("PATH", ""),
               RUSTFLAGS="-C link-arg=-z -C link-arg=max-page-size=0x1000")
    run([str(cargo), "build",
         "--manifest-path", str(REPO / "user" / "rust-hello" / "Cargo.toml"),
         "--target", str(REPO / "rust-targets" / "x86_64-nexios.json"),
         "-Z", "build-std=core", "-Z", "json-target-spec", "--release",
         "--target-dir", str(REPO / "build" / "rust-target"),
         "--locked"], env=env, cwd=str(REPO))
    out = REPO / "build" / "user" / "rust-hello.elf"
    out.parent.mkdir(parents=True, exist_ok=True)
    import shutil
    shutil.copy2(REPO / "build" / "rust-target" / "x86_64-nexios" /
                 "release" / "rust-hello.elf", out)
    return 0


ACTIONS = {
    "check-config": check_config,
    "test-policy-check": test_policy_check,
    "audit": audit,
    "picolibc": picolibc,
    "user-rust-hello": user_rust_hello,
}


def main():
    """Dispatch one named check."""
    if len(sys.argv) not in (2, 3) or sys.argv[1] not in ACTIONS:
        print("usage: run_checks.py {%s} [arch]" % "|".join(sorted(ACTIONS)))
        return 2
    arch = sys.argv[2] if len(sys.argv) == 3 else "x86_64"
    return ACTIONS[sys.argv[1]](arch)


if __name__ == "__main__":
    sys.exit(main())
