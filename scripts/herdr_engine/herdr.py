#!/usr/bin/env python3
"""Typed subprocess shim over hc (herdr-core 0.1.18) — issue #507 T2.

Piped `hc` output is JSON for every command except run/exec/read/file-read
(hc manual), so the shim pipes and parses JSON everywhere and surfaces typed
errors. The fence spawn helper renders the exact issue-#507 §4 line:

    hc run --bg --name <t> --cwd <dir> env OPENCODE_CONFIG=<cfg>
      TMPDIR=/tmp/agentup-tmp-<t> NEXIOS_AGENT_ROLE=<role> <OPENCODE_BIN> ...

Identity is verified on the agent process environment (docs/engine-herdr/
spawn-proof.md: on macOS only unsigned programs expose `ps eww` environ —
zsh/sh do not; the seam therefore targets the agent process, same as legacy
check_fenced) and a mismatch is killed on sight.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
from dataclasses import dataclass, field
from typing import Any, Callable

HC_BIN_ENV = "HERDR_HC_BIN"
FOUR_VALUES = ("PWD", "OPENCODE_CONFIG", "NEXIOS_AGENT_ROLE", "TMPDIR")


@dataclass
class Term:
    """Observed state of one hc terminal (subset of `hc get --json`)."""

    name: str
    address: str | None = None
    state: str | None = None
    pid: int | None = None
    exit_code: int | None = None
    reason: str | None = None
    title: str | None = None
    cwd: str | None = None
    turn: int = 0
    raw: dict[str, Any] = field(default_factory=dict)


class HCError(Exception):
    """Typed failure of one hc invocation."""

    def __init__(self, cmd: list[str], rc: int, msg: str):
        super().__init__(f"hc {' '.join(cmd[:6])} ... -> rc={rc}: {msg[:300]}")
        self.cmd = cmd
        self.rc = rc
        self.msg = msg


class Herdr:
    """Typed wrapper over the `hc` CLI; one instance per engine process."""

    def __init__(self, hc_bin: str | None = None, timeout: float = 30.0):
        self.hc_bin = hc_bin or os.environ.get(HC_BIN_ENV, os.path.expanduser("~/.local/bin/hc"))
        self.timeout = timeout
        self.registry: dict[str, Term] = {}
        self.expect_identity: dict[str, dict[str, str]] = {}
        self._events_proc: subprocess.Popen | None = None

    # -- plumbing -----------------------------------------------------------

    @staticmethod
    def fence_tmpdir(name: str) -> str:
        return f"/tmp/agentup-tmp-{name}"

    @classmethod
    def fence_cmd(
        cls,
        role: str,
        cwd: str,
        cfg_path: str,
        fenced_bin: str,
        name: str,
        task: str | None = None,
        extra_bin_args: list[str] | None = None,
    ) -> list[str]:
        """The §4 spawn command (without the leading `hc run` wrapper)."""
        cmd = [
            "env",
            f"TMPDIR={cls.fence_tmpdir(name)}",
            f"OPENCODE_CONFIG={cfg_path}",
            f"NEXIOS_AGENT_ROLE={role}",
            fenced_bin,
            *(extra_bin_args or []),
        ]
        if task:
            cmd.append(task)
        return cmd

    def _run(self, args: list[str], expect_json: bool = True):
        try:
            proc = subprocess.run(
                [self.hc_bin, *args], capture_output=True, text=True, timeout=self.timeout
            )
        except (subprocess.TimeoutExpired, OSError) as exc:
            raise HCError(args, -1, f"{type(exc).__name__}: {exc}") from exc
        out = proc.stdout
        if proc.returncode != 0:
            raise HCError(args, proc.returncode, (proc.stderr or out or "").strip())
        if expect_json:
            try:
                return json.loads(out) if out.strip() else {}
            except json.JSONDecodeError as exc:
                raise HCError(args, proc.returncode, f"bad JSON: {out[:200]!r}") from exc
        return out

    # -- lifecycle ----------------------------------------------------------

    def _touch(self, payload: dict[str, Any], key: str | None = None) -> Term:
        if key is None:
            key = payload.get("name") or payload.get("address") or ""
        term = self.registry.setdefault(key, Term(name=key))
        term.address = payload.get("address") or term.address
        term.state = payload.get("state") or term.state
        fg = payload.get("foreground") or {}
        term.pid = payload.get("pid") or fg.get("pid") or term.pid
        term.exit_code = payload.get("exit_code", term.exit_code)
        term.reason = payload.get("reason") or term.reason
        term.title = payload.get("title") or term.title
        term.cwd = payload.get("cwd") or term.cwd
        term.turn = payload.get("turn", term.turn)
        term.raw = payload
        if payload.get("name"):
            term.name = payload["name"]
        return term

    def spawn(
        self,
        name: str,
        program_args: list[str],
        cwd: str,
        task: str | None = None,
        labels: dict[str, str] | None = None,
    ) -> Term:
        """`hc run --bg --json --name <n> --cwd <cwd> [labels] <program...>`"""
        args = ["run", "--bg", "--json", "--name", name, "--cwd", cwd]
        for k, v in (labels or {}).items():
            args += ["--label", f"{k}={v}"]
        args += program_args
        if task:
            args += [task]
        payload = self._run(args)
        return self._touch(payload)

    def spawn_fenced(
        self,
        name: str,
        role: str,
        cwd: str,
        cfg_path: str,
        fenced_bin: str,
        task: str | None = None,
        extra_bin_args: list[str] | None = None,
    ) -> dict[str, str]:
        """Spawn per the fence contract and record the approved identity.

        Returns the wanted (variable, value) pairs so the engine can verify
        at prompt time and kill on mismatch (nobody opens 2b unverifiable).
        """
        cmd = self.fence_cmd(role, cwd, cfg_path, fenced_bin, name, task, extra_bin_args)
        self._run(["run", "--bg", "--json", "--name", name, "--cwd", cwd, *cmd])
        wanted = {
            "PWD": cwd,
            "OPENCODE_CONFIG": cfg_path,
            "NEXIOS_AGENT_ROLE": role,
            "TMPDIR": self.fence_tmpdir(name),
        }
        self.registry.setdefault(name, Term(name=name))
        self.expect_identity[name] = wanted
        return wanted

    def verify_fenced(self, name: str, want: dict[str, str] | None = None) -> bool:
        """Four fence values on the terminal's foreground program env."""
        want = want or self.expect_identity.get(name) or {}
        if not want or set(want) != set(FOUR_VALUES):
            return False
        term = self.get(name)
        if not term.pid:
            return False
        env = subprocess.run(
            ["ps", "eww", "-p", str(term.pid)], capture_output=True, text=True
        ).stdout
        return all(f"{k}={v}" in env for k, v in want.items())

    def kill_on_mismatch(self, name: str) -> bool:
        """Identity gate: True = verified; False = killed (self+registry)."""
        if self.verify_fenced(name):
            return True
        try:
            self.kill(name)
        except HCError:
            pass
        self.registry.pop(name, None)
        self.expect_identity.pop(name, None)
        return False

    # -- observation --------------------------------------------------------

    def get(self, name: str) -> Term:
        """`hc get <name> --json`; raises HCError when the terminal is gone."""
        payload = self._run(["get", name, "--json"])
        return self._touch(payload)

    def ls(self, include_finished: bool = False) -> list[Term]:
        args = ["ls", "--json"] + (["--all"] if include_finished else [])
        payload = self._run(args)
        return [self._touch(t, key=t.get("name") or t.get("address")) for t in payload.get("terminals", [])]

    def state(self, name: str) -> str:
        """Terminal state or 'absent' (get-not-found is an empty state)."""
        try:
            return self.get(name).state or ""
        except HCError:
            return "absent"

    # -- interaction --------------------------------------------------------

    def send(self, name: str, text: str) -> dict[str, Any]:
        return self._run(["send", name, text])

    def wait(
        self,
        name: str,
        timeout: float = 180.0,
        output: str | None = None,
    ) -> dict[str, Any]:
        args = ["wait", name, "--json", "--timeout", str(int(timeout))]
        if output:
            args += ["--output", output]
        # subprocess deadline exceeds hc's own check-in timeout, else the
        # two race and the shim dies before hc can return its check-in line
        try:
            proc = subprocess.run(
                [self.hc_bin, *args], capture_output=True, text=True, timeout=timeout + 10.0
            )
        except subprocess.TimeoutExpired as exc:
            raise HCError(args, -1, f"TimeoutExpired: {exc}") from exc
        if proc.returncode != 0:
            raise HCError(args, proc.returncode, (proc.stderr or proc.stdout or "").strip())
        return json.loads(proc.stdout)

    def wait_children(self, mode: str = "all", timeout: float = 180.0) -> dict[str, Any]:
        if mode not in ("all", "any"):
            raise HCError(["wait", "--children"], -1, f"mode must be all|any, got {mode!r}")
        try:
            proc = subprocess.run(
                [self.hc_bin, "wait", "--children", mode, "--json", "--timeout", str(int(timeout))],
                capture_output=True, text=True, timeout=timeout + 10.0,
            )
        except subprocess.TimeoutExpired as exc:
            raise HCError(["wait", "--children"], -1, f"TimeoutExpired: {exc}") from exc
        if proc.returncode != 0:
            raise HCError(["wait", "--children"], proc.returncode, (proc.stderr or proc.stdout or "").strip())
        return json.loads(proc.stdout)

    # -- teardown / config --------------------------------------------------

    def kill(self, name: str) -> None:
        self._run(["kill", name])

    def rm(self, name: str) -> None:
        try:
            self._run(["rm", name])
        except HCError:
            pass
        self.registry.pop(name, None)
        self.expect_identity.pop(name, None)

    def label(self, name: str, **kv: str) -> dict[str, Any] | None:
        if not kv:
            return None
        return self._run(["label", name, *[f"{k}={v}" for k, v in kv.items()]])

    def keep(self, name: str, on: bool = True) -> dict[str, Any] | None:
        args = ["keep", name] + ([] if on else ["--off"])
        return self._run(args)

    def interrupt(self, name: str) -> dict[str, Any] | None:
        return self._run(["interrupt", name])

    def exec(self, cmd: list[str], machine: str | None = None) -> tuple[int, str]:
        args = ["exec"] + (["--on", machine] if machine else []) + cmd
        try:
            return 0, self._run(args, expect_json=False).strip()
        except HCError as exc:
            return exc.rc, exc.msg

    # -- events bus ---------------------------------------------------------

    @staticmethod
    def normalize_event(raw: dict[str, Any]) -> dict[str, Any]:
        """Canonical event shape for the callback bus.

        `hc events` emits (a) one full-state snapshot line first
        ({"seq", "terminals"}) and (b) change events wrapped as
        {"jsonrpc":..., "method":"event", "params":{type,data,terminal,seq}}.
        """
        params = raw.get("params")
        if isinstance(params, dict):
            return {
                "type": params.get("type"),
                "data": params.get("data"),
                "terminal": params.get("terminal"),
                "seq": params.get("seq"),
                "raw": raw,
            }
        return {"type": "snapshot", "data": raw.get("terminals"),
                "terminal": None, "seq": raw.get("seq"), "raw": raw}

    def events(self, handler: Callable[[dict[str, Any]], None], **kwargs) -> threading.Thread:
        """Follow `hc events --json` in a daemon thread; dispatch each line.

        kwargs: terminal=, children_of=, labels={k: v}.
        Handler exceptions are swallowed to keep the bus alive (typed state
        machine handles recovery, not crash-on-listener-bug).
        """
        args = ["events", "--json"]
        if "terminal" in kwargs:
            args += ["--terminal", kwargs["terminal"]]
        if "children_of" in kwargs:
            args += ["--children-of", kwargs["children_of"]]
        for k, v in (kwargs.get("labels") or {}).items():
            args += ["--label", f"{k}={v}"]

        def loop() -> None:
            try:
                proc = subprocess.Popen(
                    [self.hc_bin, *args], stdout=subprocess.PIPE, text=True, bufsize=1
                )
                self._events_proc = proc
                assert proc.stdout is not None
                for line in iter(proc.stdout.readline, ""):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        handler(self.normalize_event(json.loads(line)))
                    except json.JSONDecodeError:
                        continue
                    except Exception:
                        continue
            except Exception:
                return

        thread = threading.Thread(target=loop, daemon=True)
        thread.start()
        return thread

    def stop_events(self) -> None:
        proc, self._events_proc = self._events_proc, None
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
