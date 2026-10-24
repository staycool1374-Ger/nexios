#!/usr/bin/env python3
"""Host automation chain runner (issue #407 item 1, TACT-inspired).

Runs declarative debug-flow chains: named steps with success/error targets,
per-step retry/timeout, loops, delays. Step types: shell-command, gdb-batch,
expect-script, wait-port, delay, loop-start/loop-end. Host-only: owns no QEMU,
never touches the kernel; steps either wrap `gdb_run.py debug-test/debug-shell`
(which own the QEMU lifecycle) or attach to an externally started stub.

Usage:
  python3 tools/chainrun.py [--list | --dry-run] [--log-dir DIR]
      [--gdb PATH] [--max-steps N] [--tail N] <chain.json>
Exit codes: 0 stop/end, 1 step failure or budget exhaustion, 2 usage or
chain-definition/policy error, 3 missing host tool or repo precondition,
130/143 interrupted. Logs: one run dir per invocation, one file per attempt.
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2
EXIT_ENV = 3

REPO_ROOT = Path(__file__).resolve().parents[1]
POLICY_AWK = REPO_ROOT / "tools" / "test-policy.awk"
POLICY_TSV = REPO_ROOT / "tools" / "test-policy.tsv"

KNOWN_TYPES = ("shell-command", "gdb-batch", "expect-script", "wait-port",
               "delay", "loop-start", "loop-end")
KNOWN_TOP_KEYS = ("name", "description", "gdb", "max_steps", "defaults",
                  "steps")
KNOWN_STEP_KEYS = ("name", "type", "command", "cwd", "env", "background",
                   "script", "argv", "elf", "host", "port", "seconds", "id",
                   "count", "timeout", "timeout_policy", "retries",
                   "retry_delay", "backoff", "accept_exit", "expect_stdout",
                   "next", "on_error")
POLICY_KEYS = ("expect_timeout", "outer_timeout")

_interrupted = 0
_live_groups = set()


def _sig_handler(signo, _frame):
    global _interrupted
    _interrupted = signo
    for pgid in list(_live_groups):
        try:
            os.killpg(pgid, signal.SIGTERM)
        except OSError:
            pass


for _sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(_sig, _sig_handler)


def die_config(msg):
    print(f"chainrun: config error: {msg}", file=sys.stderr)
    return EXIT_CONFIG


class ChainError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def checkout_tag():
    return re.sub(r"[^A-Za-z0-9_]", "_", REPO_ROOT.name)


def slug(name):
    return re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_") or "step"


def resolve_policy(arch, cls, key):
    if key not in POLICY_KEYS:
        raise ChainError(EXIT_CONFIG, f"unknown policy key '{key}'")
    try:
        out = subprocess.run(
            ["awk", "-f", str(POLICY_AWK), f"-varch={arch}",
             f"-vclass={cls}", f"-vkey={key}", str(POLICY_TSV)],
            capture_output=True, text=True, timeout=30)
    except OSError as e:
        raise ChainError(EXIT_ENV, f"cannot run policy awk: {e}")
    val = out.stdout.strip()
    if out.returncode != 0 or not val:
        raise ChainError(EXIT_CONFIG,
                         f"policy lookup failed arch={arch} class={cls} "
                         f"key={key}: {out.stderr.strip()}")
    try:
        return float(val)
    except ValueError:
        raise ChainError(EXIT_CONFIG, f"policy value not numeric: '{val}'")


def load_chain(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise ChainError(EXIT_CONFIG, f"cannot load chain '{path}': {e}")
    if not isinstance(data, dict):
        raise ChainError(EXIT_CONFIG, "chain root must be an object")
    for k in data:
        if k not in KNOWN_TOP_KEYS:
            raise ChainError(EXIT_CONFIG, f"unknown top-level key '{k}'")
    if not data.get("name"):
        raise ChainError(EXIT_CONFIG, "chain needs a non-empty name")
    steps = data.get("steps")
    if not isinstance(steps, list) or not steps:
        raise ChainError(EXIT_CONFIG, "chain needs a non-empty steps list")
    return data


def check_step(raw, idx):
    if not isinstance(raw, dict):
        raise ChainError(EXIT_CONFIG, f"step {idx} must be an object")
    for k in raw:
        if k not in KNOWN_STEP_KEYS:
            raise ChainError(EXIT_CONFIG,
                             f"step {idx} has unknown key '{k}'")
    name = raw.get("name")
    if not name or not isinstance(name, str):
        raise ChainError(EXIT_CONFIG, f"step {idx} needs a name")
    typ = raw.get("type")
    if typ not in KNOWN_TYPES:
        raise ChainError(EXIT_CONFIG,
                         f"step '{name}': unknown type '{typ}'")
    nxt = raw.get("next", "next")
    err = raw.get("on_error", "fail")
    for label, tgt in (("next", nxt), ("on_error", err)):
        if not isinstance(tgt, str) or not tgt:
            raise ChainError(EXIT_CONFIG,
                             f"step '{name}': bad {label} target")
    # Type-required fields.
    if typ == "shell-command" and "command" not in raw:
        raise ChainError(EXIT_CONFIG,
                         f"step '{name}': shell-command needs command")
    if typ in ("gdb-batch", "expect-script") and "script" not in raw:
        raise ChainError(EXIT_CONFIG, f"step '{name}': {typ} needs script")
    if typ == "wait-port" and ("host" not in raw or "port" not in raw):
        raise ChainError(EXIT_CONFIG,
                         f"step '{name}': wait-port needs host and port")
    if typ == "delay" and "seconds" not in raw:
        raise ChainError(EXIT_CONFIG, f"step '{name}': delay needs seconds")
    if typ in ("loop-start", "loop-end") and "id" not in raw:
        raise ChainError(EXIT_CONFIG, f"step '{name}': {typ} needs id")
    if typ == "shell-command" and raw.get("background"):
        for k in ("accept_exit", "expect_stdout"):
            if k in raw:
                raise ChainError(EXIT_CONFIG,
                                 f"step '{name}': {k} not allowed on "
                                 "background steps")
    # Numeric bounds.
    secs = raw.get("seconds")
    if secs is not None and not (isinstance(secs, (int, float))
                                 and 0 <= secs <= 300):
        raise ChainError(EXIT_CONFIG,
                         f"step '{name}': seconds must be in [0..300]")
    cnt = raw.get("count")
    if typ == "loop-start" and (not isinstance(cnt, int) or
                               not 0 <= cnt <= 10000):
        raise ChainError(EXIT_CONFIG,
                         f"step '{name}': count must be int in [0..10000]")
    for k in ("retries",):
        v = raw.get(k, 0)
        if not isinstance(v, int) or not 0 <= v <= 20:
            raise ChainError(EXIT_CONFIG,
                             f"step '{name}': {k} must be int in [0..20]")
    for k in ("retry_delay", "backoff"):
        v = raw.get(k, 1 if k == "backoff" else 0)
        if not isinstance(v, (int, float)) or v < (1 if k == "backoff"
                                                   else 0):
            raise ChainError(EXIT_CONFIG, f"step '{name}': bad {k}")
    to = raw.get("timeout")
    if to is not None and (not isinstance(to, (int, float)) or to <= 0):
        raise ChainError(EXIT_CONFIG, f"step '{name}': bad timeout")
    if typ in ("shell-command", "gdb-batch", "expect-script"):
        if to is None and "timeout_policy" not in raw:
            raise ChainError(EXIT_CONFIG,
                             f"step '{name}': needs timeout or "
                             "timeout_policy")
    acc = raw.get("accept_exit", [0])
    if (not isinstance(acc, list) or not acc
            or any(not isinstance(x, int) for x in acc)):
        raise ChainError(EXIT_CONFIG, f"step '{name}': bad accept_exit")
    pat = raw.get("expect_stdout")
    if pat is not None:
        try:
            re.compile(pat)
        except re.error as e:
            raise ChainError(EXIT_CONFIG,
                             f"step '{name}': bad expect_stdout: {e}")
    tp = raw.get("timeout_policy")
    if tp is not None:
        if not isinstance(tp, dict):
            raise ChainError(EXIT_CONFIG,
                             f"step '{name}': bad timeout_policy")
        for k in ("class", "arch", "key"):
            if k not in tp:
                raise ChainError(EXIT_CONFIG,
                                 f"step '{name}': timeout_policy needs "
                                 f"'{k}'")


def validate_and_compile(data, check_files):
    steps = data["steps"]
    for i, raw in enumerate(steps):
        check_step(raw, i)
    names = [s["name"] for s in steps]
    if len(set(names)) != len(names):
        raise ChainError(EXIT_CONFIG, "step names must be unique")
    index = {n: i for i, n in enumerate(names)}
    # Static loop pairing (proper nesting enforced).
    pair_end = {}
    stack = []
    for i, raw in enumerate(steps):
        if raw["type"] == "loop-start":
            stack.append((raw["id"], i))
        elif raw["type"] == "loop-end":
            if not stack or stack[-1][0] != raw["id"]:
                raise ChainError(EXIT_CONFIG,
                                 f"unbalanced loop-end '{raw['name']}'")
            _, start = stack.pop()
            pair_end[start] = i
    if stack:
        raise ChainError(EXIT_CONFIG,
                         f"unclosed loop-start id '{stack[-1][0]}'")
    # Enclosing loops per index (body = (start, end]); the source side
    # additionally counts its own loop-start/end (entering your own loop
    # at its start pushes a frame, so fall-through start->body is legal).
    enclosing = []
    enclosing_self = []
    for i in range(len(steps)):
        env = set()
        env_self = set()
        for s, e in pair_end.items():
            lid = steps[s]["id"]
            if s < i <= e:
                env.add(lid)
            if s <= i <= e:
                env_self.add(lid)
        enclosing.append(env)
        enclosing_self.append(env_self)

    def resolve_target(src, tgt):
        if tgt == "next":
            return src + 1
        if tgt == "prev":
            if src == 0:
                raise ChainError(EXIT_CONFIG, "'prev' from first step")
            return src - 1
        if tgt == "stop":
            return -1
        if tgt == "fail":
            return -2
        if tgt not in index:
            raise ChainError(EXIT_CONFIG,
                             f"unknown target '{tgt}'")
        return index[tgt]

    compiled = []
    for i, raw in enumerate(steps):
        nxt = resolve_target(i, raw.get("next", "next"))
        err = resolve_target(i, raw.get("on_error", "fail"))
        # No jumping into a loop body from outside it.
        for tgt, label in ((nxt, "next"), (err, "on_error")):
            if tgt < 0 or tgt >= len(steps):
                continue  # stop/fail sentinels and implicit end-of-chain
            if not (enclosing[tgt] <= enclosing_self[i]):
                raise ChainError(EXIT_CONFIG,
                                 f"step '{raw['name']}': {label} enters "
                                 "a loop body from outside")
        to = raw.get("timeout")
        if to is None and "timeout_policy" in raw:
            tp = raw["timeout_policy"]
            to = resolve_policy(tp["arch"], tp["class"], tp["key"])
        compiled.append({"raw": raw, "next": nxt, "on_error": err,
                         "timeout": to})
    if check_files:
        for c in compiled:
            raw = c["raw"]
            if raw["type"] in ("gdb-batch", "expect-script"):
                p = REPO_ROOT / raw["script"]
                if not p.is_file():
                    raise ChainError(EXIT_CONFIG,
                                     f"missing script '{raw['script']}'")
    return compiled, pair_end


def tcp_ready(host, port, timeout):
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection((host, int(port)),
                                          timeout=min(2.0, max(0.1,
                                                               timeout))):
                return True
        except OSError:
            pass
        if time.monotonic() >= deadline:
            return False
        if _interrupted:
            return False
        time.sleep(0.2)


def spawn_fg(argv, cwd, env, timeout, log_path):
    """Run to completion; kill the process GROUP on timeout. Returns rc
    (124 on timeout like GNU timeout) with combined output in the log."""
    start = time.monotonic()
    proc = subprocess.Popen(
        argv, cwd=cwd, env=env, stdout=open(log_path, "ab"),
        stderr=subprocess.STDOUT, start_new_session=True)
    _live_groups.add(proc.pid)
    try:
        try:
            rc = proc.wait(timeout=timeout)
            return rc, False
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except OSError:
                pass
            try:
                rc = proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    pass
                rc = proc.wait()
            with open(log_path, "ab") as f:
                f.write(f"\n[TIMEOUT after {timeout:g}s]\n".encode())
            return 124, True
    finally:
        _live_groups.discard(proc.pid)
        _ = time.monotonic() - start


class Runner:
    def __init__(self, chain_path, chain, compiled, pair_end, args):
        self.chain_path = chain_path
        self.chain = chain
        self.compiled = compiled
        self.pair_end = pair_end
        self.args = args
        self.tag = checkout_tag()
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        base = args.log_dir or (Path("/tmp") / f"jarvis-chain-{self.tag}")
        self.run_dir = Path(base) / f"{chain['name']}-{stamp}-{os.getpid()}"
        self.step_count = 0
        self.attempt_count = 0
        self.error_count = 0
        self.background = []
        self.failed = None  # (step_name, rc, log_path)

    def log_path(self, idx, attempt, bg=False):
        name = self.compiled[idx]["raw"]["name"]
        suffix = ".bg.log" if bg else f".a{attempt}.log"
        return self.run_dir / f"{idx:02d}-{slug(name)}{suffix}"

    def base_env(self, idx, attempt):
        env = dict(os.environ)
        env.update({
            "JARVIS_CHAIN_NAME": self.chain["name"],
            "JARVIS_CHAIN_RUN_DIR": str(self.run_dir),
            "JARVIS_CHAIN_STEP": self.compiled[idx]["raw"]["name"],
            "JARVIS_CHAIN_ATTEMPT": str(attempt),
            "JARVIS_CHAIN_CHECKOUT_TAG": self.tag,
        })
        if self.failed is not None:
            env["JARVIS_CHAIN_FAILED_STEP"] = self.failed[0]
            env["JARVIS_CHAIN_FAILED_RC"] = str(self.failed[1])
            env["JARVIS_CHAIN_FAILED_LOG"] = str(self.failed[2])
        return env

    def reap_background(self):
        for _name, proc in self.background:
            try:
                proc.terminate()
            except OSError:
                pass
        deadline = time.monotonic() + 2
        for _name, proc in self.background:
            try:
                proc.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                try:
                    proc.kill()
                except OSError:
                    pass
                proc.wait()
        self.background = []

    def run_background(self, idx, argv, cwd, env):
        log = self.log_path(idx, 0, bg=True)
        proc = subprocess.Popen(
            argv, cwd=cwd, env=env, stdout=open(log, "ab"),
            stderr=subprocess.STDOUT, start_new_session=True)
        self.background.append((self.compiled[idx]["raw"]["name"], proc))

    def exec_step(self, idx, attempt):
        c = self.compiled[idx]
        raw = c["raw"]
        typ = raw["type"]
        env = self.base_env(idx, attempt)
        if "cwd" in raw:
            cwd = str(REPO_ROOT / raw["cwd"])
        else:
            cwd = os.getcwd()
        if isinstance(raw.get("env"), dict):
            env.update({k: str(v) for k, v in raw["env"].items()})
        if typ == "delay":
            total = float(raw["seconds"])
            end = time.monotonic() + total
            while time.monotonic() < end:
                if _interrupted:
                    return 130, False, ""
                time.sleep(min(0.2, end - time.monotonic()))
            return 0, False, ""
        if typ == "wait-port":
            ok = tcp_ready(raw["host"], raw["port"], float(c["timeout"] or 30))
            return (0, False, "") if ok else (1, False, "")
        if typ == "shell-command":
            cmd = raw["command"]
            argv = cmd if isinstance(cmd, list) else ["/bin/sh", "-lc", cmd]
            if raw.get("background"):
                self.run_background(idx, argv, cwd, env)
                return 0, False, ""
            log = self.log_path(idx, attempt)
            rc, _to = spawn_fg(argv, cwd, env, float(c["timeout"]), log)
            out = log.read_bytes().decode("utf-8", "replace") if log.exists() else ""
            return rc, False, out
        if typ == "gdb-batch":
            gdb = (self.args.gdb or self.chain.get("gdb")
                   or os.environ.get("GDB") or "x86_64-elf-gdb")
            import shutil
            if shutil.which(gdb) is None and not Path(gdb).is_file():
                raise ChainError(EXIT_ENV, f"gdb binary missing: {gdb}")
            host = raw.get("host", "127.0.0.1")
            port = int(raw.get("port", 1234))
            if not tcp_ready(host, port, min(float(c["timeout"]), 10.0)):
                return 1, False, ""
            elf = raw.get("elf", "build/kernel-debug.elf")
            argv = [gdb, elf, "-batch", "-x", raw["script"]]
            log = self.log_path(idx, attempt)
            rc, _to = spawn_fg(argv, cwd, env, float(c["timeout"]), log)
            out = log.read_bytes().decode("utf-8", "replace") if log.exists() else ""
            return rc, False, out
        if typ == "expect-script":
            import shutil
            if shutil.which("expect") is None:
                raise ChainError(EXIT_ENV, "expect binary missing")
            argv = ["expect", raw["script"]] + list(raw.get("argv", []))
            log = self.log_path(idx, attempt)
            rc, _to = spawn_fg(argv, cwd, env, float(c["timeout"]), log)
            out = log.read_bytes().decode("utf-8", "replace") if log.exists() else ""
            return rc, False, out
        raise ChainError(EXIT_CONFIG, f"bad type '{typ}'")

    def attempt_ok(self, idx, rc, out):
        raw = self.compiled[idx]["raw"]
        if rc not in raw.get("accept_exit", [0]):
            return False
        pat = raw.get("expect_stdout")
        if pat is not None and re.search(pat, out) is None:
            return False
        return True

    def run(self):
        self.run_dir.mkdir(parents=True, exist_ok=True)
        steps = self.compiled
        budget = self.args.max_steps
        pc = 0
        loop_stack = []
        start_t = time.monotonic()
        try:
            while 0 <= pc < len(steps):
                if _interrupted:
                    return 130 if _interrupted == signal.SIGINT else 143
                if self.step_count >= budget:
                    print(f"CHAIN FAILED chain={self.chain['name']}: "
                          f"step budget {budget} exhausted",
                          file=sys.stderr)
                    return EXIT_FAIL
                raw = steps[pc]["raw"]
                typ = raw["type"]
                if typ == "loop-start":
                    lid = raw["id"]
                    if raw["count"] == 0:
                        pc = self.pair_end[pc] + 1
                        continue
                    loop_stack.append({"id": lid, "remaining": raw["count"]})
                    pc += 1
                    continue
                if typ == "loop-end":
                    if not loop_stack or loop_stack[-1]["id"] != raw["id"]:
                        print(f"chainrun: loop desync at '{raw['name']}'",
                              file=sys.stderr)
                        return EXIT_CONFIG
                    frame = loop_stack[-1]
                    frame["remaining"] -= 1
                    if frame["remaining"] > 0:
                        # back to the body (skip the loop-start itself)
                        start = next(s for s, e in self.pair_end.items()
                                     if e == pc)
                        pc = start + 1
                    else:
                        loop_stack.pop()
                        pc += 1
                    continue
                ok, log = self.run_step(pc)
                if ok:
                    pc = self.goto(pc, steps[pc]["next"])
                else:
                    self.error_count += 1
                    tgt = steps[pc]["on_error"]
                    if tgt == -2:
                        self.report_failure(pc)
                        return EXIT_FAIL
                    pc = tgt
            elapsed = time.monotonic() - start_t
            print(f"CHAIN OK name={self.chain['name']} "
                  f"steps={self.step_count} attempts={self.attempt_count} "
                  f"errors={self.error_count} elapsed={elapsed:.1f}s "
                  f"logdir={self.run_dir}")
            return EXIT_OK
        finally:
            self.reap_background()

    def goto(self, pc, tgt):
        if tgt == -1:
            return len(self.compiled)
        return tgt

    def run_step(self, idx):
        c = self.compiled[idx]
        raw = c["raw"]
        retries = int(raw.get("retries", 0))
        delay = float(raw.get("retry_delay", 0))
        backoff = float(raw.get("backoff", 1))
        last_rc, last_log, last_ok = 1, None, False
        for attempt in range(1, retries + 2):
            if _interrupted:
                return False, None
            self.step_count += 1
            self.attempt_count += 1
            rc, _to, out = self.exec_step(idx, attempt)
            last_rc = rc
            last_log = self.log_path(idx, attempt)
            last_ok = self.attempt_ok(idx, rc, out)
            if last_ok:
                return True, last_log
            if attempt <= retries:
                wait = min(delay * (backoff ** (attempt - 1)), 60.0)
                if wait > 0:
                    time.sleep(wait)
        self.failed = (raw["name"], last_rc,
                       last_log or self.log_path(idx, 1))
        return False, self.failed[2]

    def report_failure(self, idx):
        if self.failed is None:
            print("chainrun: internal error: no failure recorded",
                  file=sys.stderr)
            return
        raw = self.compiled[idx]["raw"]
        _name, rc, log = self.failed
        print(f"CHAIN FAILED chain={self.chain['name']} step={raw['name']} "
              f"type={raw['type']} rc={rc} log={log}", file=sys.stderr)
        try:
            tail = log.read_bytes().decode("utf-8", "replace").splitlines()
            for line in tail[-self.args.tail:]:
                print(f"  | {line}", file=sys.stderr)
        except OSError:
            pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="Host automation chain runner")
    ap.add_argument("chain", help="chain JSON file")
    ap.add_argument("--list", action="store_true",
                    help="validate and list resolved timeouts")
    ap.add_argument("--dry-run", action="store_true",
                    help="validate fully without executing")
    ap.add_argument("--log-dir", default=None)
    ap.add_argument("--gdb", default=None)
    ap.add_argument("--max-steps", type=int, default=10000)
    ap.add_argument("--tail", type=int, default=20)
    args = ap.parse_args(argv)
    if not (REPO_ROOT / "meson.build").is_file() or not POLICY_AWK.is_file():
        print("chainrun: must run from the NexIOS checkout", file=sys.stderr)
        return EXIT_ENV
    try:
        data = load_chain(args.chain)
        compiled, pair_end = validate_and_compile(data, check_files=True)
    except ChainError as e:
        print(f"chainrun: {e}", file=sys.stderr)
        return e.code
    if args.list or args.dry_run:
        for i, c in enumerate(compiled):
            raw = c["raw"]
            print(f"{i:02d} {raw['name']} [{raw['type']}] "
                  f"timeout={c['timeout']} next={raw.get('next', 'next')} "
                  f"on_error={raw.get('on_error', 'fail')}")
        return EXIT_OK
    runner = Runner(args.chain, data, compiled, pair_end, args)
    try:
        return runner.run()
    except ChainError as e:
        print(f"chainrun: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
