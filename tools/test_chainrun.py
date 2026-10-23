#!/usr/bin/env python3
"""Host-only oracle for tools/chainrun.py (issue #407 item 1).

Stub-first unittest suite: no QEMU, no third-party deps. Each case builds
a chain dict, runs `python3 tools/chainrun.py` with --log-dir inside a temp
root, and asserts exit codes, banners, logs, and marker effects.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
RUNNER = REPO_ROOT / "tools" / "chainrun.py"


def run_chain(chain, *extra, cwd=None):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        chain_path = tmp / "chain.json"
        chain_path.write_text(json.dumps(chain), encoding="utf-8")
        logdir = tmp / "logs"
        cmd = [sys.executable, str(RUNNER), "--log-dir", str(logdir),
               *extra, str(chain_path)]
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              cwd=str(cwd or tmp), timeout=120)
        return proc, tmp


def sh_step(name, body, **kw):
    step = {"name": name, "type": "shell-command",
            "command": ["sh", "-c", body], "timeout": 10}
    step.update(kw)
    return step


class TestChainrun(unittest.TestCase):
    def test_linear_success(self):
        chain = {"name": "t", "steps": [
            sh_step("a", "echo hi"),
            sh_step("b", "echo there", next="stop")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            logdir = tmp / "logs"
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(logdir),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("CHAIN OK", p.stdout)
            self.assertTrue((logdir).exists())
            logs = sorted(logdir.rglob("*.log"))
            self.assertEqual(len(logs), 2)

    def test_named_next_skips(self):
        chain = {"name": "t", "steps": [
            sh_step("a", "echo go", next="c"),
            sh_step("b", "touch MARK_B"),
            sh_step("c", "echo end", next="stop")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertFalse((tmp / "MARK_B").exists())

    def test_stop_sentinel(self):
        chain = {"name": "t", "steps": [
            sh_step("a", "echo go", next="stop"),
            sh_step("b", "touch MARK_B")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertFalse((tmp / "MARK_B").exists())

    def test_on_error_target_runs_then_stop(self):
        chain = {"name": "t", "steps": [
            sh_step("a", "exit 3", on_error="fix"),
            sh_step("fix", "touch FIXED", next="stop")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertTrue((tmp / "FIXED").exists())
            self.assertIn("CHAIN OK", p.stdout)

    def test_on_error_default_fails_and_names_step(self):
        chain = {"name": "t", "steps": [sh_step("boom", "exit 7")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 1)
            self.assertIn("step=boom", p.stderr)

    def test_retry_recovers_second_attempt(self):
        body = ("n=$(cat cnt 2>/dev/null || echo 0); echo $((n+1)) > cnt; "
                "test $n -ge 1")
        chain = {"name": "t", "steps": [sh_step("flaky", body, retries=1)]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            logs = sorted((tmp / "l").rglob("*.a2.log"))
            self.assertEqual(len(logs), 1)

    def test_retry_exhausted_three_attempts(self):
        chain = {"name": "t", "steps": [sh_step("bad", "exit 1", retries=2)]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 1)
            logs = sorted((tmp / "l").rglob("00-bad.a*.log"))
            self.assertEqual(len(logs), 3)

    def test_loop_runs_body_count(self):
        chain = {"name": "t", "steps": [
            {"name": "start", "type": "loop-start", "id": "L", "count": 2},
            sh_step("body", "echo x >> lines"),
            {"name": "end", "type": "loop-end", "id": "L"}]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual((tmp / "lines").read_text().splitlines(),
                             ["x", "x"])

    def test_timeout_kills_fast(self):
        chain = {"name": "t", "steps": [sh_step("slow", "sleep 30",
                                                timeout=0.5)]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            start = time.monotonic()
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            elapsed = time.monotonic() - start
            self.assertEqual(p.returncode, 1)
            self.assertLess(elapsed, 5.0)

    def test_prev_target(self):
        s1 = ("n=$(cat cnt 2>/dev/null || echo 0); echo $((n+1)) > cnt; "
              "test $n -ge 1")
        chain = {"name": "t", "steps": [
            sh_step("init", "echo init"),
            sh_step("gate", s1, on_error="prev")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual((tmp / "cnt").read_text().strip(), "2")

    def _expect_config_error(self, chain):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 2, p.stderr + p.stdout)

    def test_validation_unknown_target_exit2(self):
        self._expect_config_error(
            {"name": "t", "steps": [sh_step("a", "true", next="ghost")]})
        self._expect_config_error(
            {"name": "t", "steps": [sh_step("a", "true", on_error="ghost")]})

    def test_validation_missing_timeout_exit2(self):
        step = {"name": "a", "type": "shell-command", "command": ["true"]}
        self._expect_config_error({"name": "t", "steps": [step]})

    def test_validation_unbalanced_loop_exit2(self):
        self._expect_config_error(
            {"name": "t", "steps": [
                {"name": "s", "type": "loop-start", "id": "L", "count": 1},
                sh_step("a", "true")]})
        self._expect_config_error(
            {"name": "t", "steps": [
                {"name": "e", "type": "loop-end", "id": "L"},
                sh_step("a", "true")]})

    def test_validation_jump_into_loop_exit2(self):
        self._expect_config_error(
            {"name": "t", "steps": [
                sh_step("a", "true", next="body"),
                {"name": "s", "type": "loop-start", "id": "L", "count": 1},
                sh_step("body", "true"),
                {"name": "e", "type": "loop-end", "id": "L"}]})

    def test_validation_unknown_key_exit2(self):
        step = sh_step("a", "true")
        step["bogus"] = 1
        self._expect_config_error({"name": "t", "steps": [step]})

    def test_dry_run_executes_nothing(self):
        chain = {"name": "t", "steps": [sh_step("a", "touch MARK")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--dry-run",
                 "--log-dir", str(tmp / "l"), str(cp)],
                capture_output=True, text=True, cwd=str(tmp), timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertFalse((tmp / "MARK").exists())
            self.assertFalse((tmp / "l").exists())

    def test_list_shows_resolved_policy(self):
        chain = {"name": "t", "steps": [
            {"name": "a", "type": "shell-command", "command": ["true"],
             "timeout_policy": {"class": "core_sched", "arch": "x86_64",
                                "key": "outer_timeout"}},
            {"name": "b", "type": "shell-command", "command": ["true"],
             "timeout_policy": {"class": "core_sched", "arch": "x86_64",
                                "key": "expect_timeout"}}]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--list",
                 "--log-dir", str(tmp / "l"), str(cp)],
                capture_output=True, text=True, cwd=str(tmp), timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("600", p.stdout)
            self.assertIn("250", p.stdout)

    def test_policy_unknown_key_exit2(self):
        chain = {"name": "t", "steps": [
            {"name": "a", "type": "shell-command", "command": ["true"],
             "timeout_policy": {"class": "x", "arch": "x86_64",
                                "key": "bogus"}}]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            self.assertEqual(p.returncode, 2)

    def test_missing_gdb_binary_exit3(self):
        chain = {"name": "t", "steps": [
            {"name": "g", "type": "gdb-batch", "script": "tools/gdb/init.gdb",
             "timeout": 5}]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--gdb", "/nonexistent-gdb",
                 "--log-dir", str(tmp / "l"), str(cp)],
                capture_output=True, text=True, cwd=str(tmp), timeout=60)
            self.assertEqual(p.returncode, 3, p.stderr + p.stdout)

    def test_step_budget_cycle_exit1(self):
        chain = {"name": "t", "steps": [
            sh_step("a", "true", next="b"),
            sh_step("b", "true", next="a")]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--max-steps", "10",
                 "--log-dir", str(tmp / "l"), str(cp)],
                capture_output=True, text=True, cwd=str(tmp), timeout=60)
            self.assertEqual(p.returncode, 1)
            self.assertIn("budget", p.stderr)

    def test_background_reaped_at_exit(self):
        # Fire-and-forget must not block the chain; the unique sleep proves
        # the process really spawned, and pgrep must find no survivor after
        # the chain-end reap (reap is synchronous: TERM, 2 s grace, KILL,
        # wait — no polling needed, but allow init a moment on loaded hosts).
        chain = {"name": "t", "steps": [
            {"name": "bg", "type": "shell-command",
             "command": ["sh", "-c", "exec sleep 98765"], "background": True,
             "timeout": 5, "next": "stop"}]}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            cp = tmp / "chain.json"
            cp.write_text(json.dumps(chain), encoding="utf-8")
            start = time.monotonic()
            p = subprocess.run(
                [sys.executable, str(RUNNER), "--log-dir", str(tmp / "l"),
                 str(cp)], capture_output=True, text=True, cwd=str(tmp),
                timeout=60)
            elapsed = time.monotonic() - start
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertLess(elapsed, 10.0)
            deadline = time.monotonic() + 5
            while True:
                q = subprocess.run(["pgrep", "-f", "sleep 98765"],
                                   capture_output=True, timeout=10)
                if q.returncode != 0:
                    break
                if time.monotonic() > deadline:
                    self.fail("background process survived reaping")
                time.sleep(0.2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
