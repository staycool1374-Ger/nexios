#!/usr/bin/env python3
"""herdr.py shim corpus — issue #507 T2 (>=1 test per command mapping).

Sim rules (T2 done-criteria): the session lives/parks/dies per state
machine semantics — lives = fenced spawn state live; parks = after a
landed task the terminal keeps existing; dies = kill -> finished(killed).
Shells only, no model needed. Terminals are tagged hpy* and swept.
"""

import json
import os
import sys
import threading
import time
import unittest
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))

from herdr_engine.herdr import HCError, Herdr, Term  # noqa: E402

PYTHON = "/opt/homebrew/bin/python3"  # unsigned -> ps eww environ visible (T0)
TAG = "hpy" + uuid.uuid4().hex[:8]
SANDBOX = f"/tmp/nherdr-herdrpy-{os.getpid()}"


class ShimBase(unittest.TestCase):
    hc: Herdr
    cwd: str
    cfg: str
    _terminals: list[str]

    @classmethod
    def setUpClass(cls):
        cls.hc = Herdr(timeout=20.0)
        cls.cwd = os.path.join(SANDBOX, "cwd")
        cls.cfg = os.path.join(SANDBOX, "opencode.dummy.json")
        os.makedirs(cls.cwd, exist_ok=True)
        with open(cls.cfg, "w") as fh:
            json.dump({"$schema": "https://opencode.ai/config.json"}, fh)

    def setUp(self):
        self._terminals = []

    def new_term(self, name: str) -> str:
        full = f"{TAG}-{name}"
        self._terminals.append(full)
        return full

    def tearDown(self):
        for t in self._terminals:
            try:
                self.hc.kill(t)
            except (HCError, OSError):
                pass
            try:
                self.hc.rm(t)
            except (HCError, OSError):
                pass
        self._terminals.clear()

    @classmethod
    def tearDownClass(cls):
        # final sweep: anything tagged TAG* that a traceback orphaned
        try:
            names = [t.name for t in cls.hc.ls(include_finished=False)]
        except HCError:
            names = []
        for name in names:
            if name.startswith(TAG):
                try:
                    cls.hc.kill(name)
                except HCError:
                    pass
                try:
                    cls.hc.rm(name)
                except HCError:
                    pass


class TestFenceRender(ShimBase):
    def test_01_fence_cmd_is_s4_line(self):
        cmd = Herdr.fence_cmd(
            role="dev", cwd="/w", cfg_path="/c.json", fenced_bin="/bin/oc", name="t1"
        )
        self.assertEqual(cmd[:4], ["env", "TMPDIR=/tmp/agentup-tmp-t1", "OPENCODE_CONFIG=/c.json", "NEXIOS_AGENT_ROLE=dev"])
        self.assertEqual(cmd[4:], ["/bin/oc"])
        with_task = Herdr.fence_cmd("dev", "/w", "/c", "/bin/oc", "t1", task="do it")
        self.assertEqual(with_task[-1], "do it")

    def test_02_fence_tmpdir_shape(self):
        self.assertEqual(Herdr.fence_tmpdir("t7"), "/tmp/agentup-tmp-t7")


class TestSessionSemantics(ShimBase):
    """lives / parks / dies (T2 done-criteria, mapped through the shim)."""

    def spawn_shell(self, name: str) -> str:
        full = self.new_term(name)
        term = self.hc.spawn(full, ["zsh"], self.cwd)
        self.assertEqual(term.state, "live")
        self.assertEqual(self.hc.state(full), "live")     # lives
        return full

    def test_10_spawn_live(self):
        full = self.spawn_shell("s1")
        term = self.hc.get(full)
        self.assertEqual(term.name, full)                 # get mapping
        self.assertIn(full, self.hc.registry)             # registry

    def test_11_send_then_wait_parks(self):
        full = self.spawn_shell("s2")
        # engine recipe: the waiter starts first — --output only sees text
        # printed after it attaches (T0 finding 2)
        result = {}

        def waiter() -> None:
            result["r"] = self.hc.wait(full, timeout=20, output="parked-here")

        wt = threading.Thread(target=waiter, daemon=True)
        wt.start()
        time.sleep(1.0)
        out = self.hc.send(full, "echo parked-here")      # send mapping
        self.assertTrue("turn" in out or "started" in out, f"send payload odd: {out}")
        wt.join(timeout=25)
        r = result.get("r") or {}
        self.assertEqual(r.get("ended"), True)            # wait mapping
        # parked: alive and addressable after the turn landed
        self.assertEqual(self.hc.state(full), "live")

    def test_12_kill_dies(self):
        full = self.spawn_shell("s3")
        self.hc.kill(full)                                # kill mapping
        term = self.hc.get(full)
        self.assertEqual(term.state, "finished")
        self.assertEqual(term.reason, "killed")           # dies

    def test_13_rm_absent_get_raises(self):
        full = self.spawn_shell("s4")
        self.hc.kill(full)
        self.hc.rm(full)                                  # rm mapping
        with self.assertRaises(HCError):
            self.hc.get(full)
        self.assertEqual(self.hc.state(full), "absent")
        self.assertNotIn(full, self.hc.registry)

    def test_14_ls_listing_contains_term(self):
        full = self.spawn_shell("s5")
        names = [t.name for t in self.hc.ls()]            # ls mapping
        self.assertIn(full, names)

    def test_15_interrupt_no_crash(self):
        full = self.spawn_shell("s6")
        try:                                              # interrupt mapping
            self.hc.interrupt(full)
        except HCError as exc:
            self.fail(f"interrupt raised: {exc.msg}")


class TestFenceIdentity(ShimBase):
    def test_20_spawn_fenced_returns_wanted(self):
        name = self.new_term("foe")
        want = self.hc.spawn_fenced(name, role="dev", cwd=self.cwd,
                                    cfg_path=self.cfg, fenced_bin=PYTHON)
        self.assertEqual(set(want), {"PWD", "OPENCODE_CONFIG", "NEXIOS_AGENT_ROLE", "TMPDIR"})
        self.assertEqual(want["NEXIOS_AGENT_ROLE"], "dev")

    def test_21_verify_fenced_ok(self):
        name = self.new_term("fov")
        self.hc.spawn_fenced(name, role="dev", cwd=self.cwd, cfg_path=self.cfg, fenced_bin=PYTHON)
        time.sleep(1.5)
        self.assertTrue(self.hc.verify_fenced(name))      # identity seam pass

    def test_22_mismatch_detected(self):
        name = self.new_term("fom")
        self.hc.spawn_fenced(name, role="dev", cwd=self.cwd, cfg_path=self.cfg, fenced_bin=PYTHON)
        time.sleep(1.5)
        want = self.hc.expect_identity[name].copy()
        want["NEXIOS_AGENT_ROLE"] = "qe"                  # mutated expectations
        self.assertFalse(self.hc.verify_fenced(name, want))

    def test_23_kill_on_mismatch(self):
        name = self.new_term("fok")
        self.hc.spawn_fenced(name, role="dev", cwd=self.cwd, cfg_path=self.cfg, fenced_bin=PYTHON)
        time.sleep(1.5)
        self.hc.expect_identity[name]["PWD"] = "/no/such/dir"
        self.assertFalse(self.hc.kill_on_mismatch(name))  # killed on sight
        self.assertNotIn(name, self.hc.registry)


class TestConfigurationEffects(ShimBase):
    def test_30_label_sets_and_lists(self):
        name = self.new_term("l1")
        self.hc.spawn(name, ["zsh"], self.cwd, labels={"lane": "qe"})
        payload = self.hc.label(name, lane="dev")         # label mapping
        self.assertTrue(isinstance(payload, dict) and "lane" in json.dumps(payload),
                        f"label payload odd: {payload}")
        # `hc get` mirrors labels; `hc ls` shows only multiplex summary
        got = self.hc.get(name)
        self.assertEqual(got.raw.get("labels", {}).get("lane"), "dev")

    def test_31_keep_toggle(self):
        name = self.new_term("k1")
        self.hc.spawn(name, ["zsh"], self.cwd)
        self.assertEqual(self.hc.state(name), "live")
        self.hc.keep(name, on=True)                       # keep mapping
        self.hc.keep(name, on=False)
        self.assertEqual(self.hc.state(name), "live")

    def test_40_exec_mapping(self):
        rc, out = self.hc.exec(["echo", "hi"])            # exec mapping
        self.assertEqual((rc, out), (0, "hi"))
        rc, _out = self.hc.exec(["false"])
        self.assertNotEqual(rc, 0)

    def test_41_events_registration(self):
        name = self.new_term("e1")
        seen = []
        ready = threading.Event()

        def handler(ev):
            seen.append(ev)
            if ev.get("type") == "created" and (ev.get("data") or {}).get("name") == name:
                ready.set()

        thread = self.hc.events(handler)                  # events mapping
        self.hc.spawn(name, ["zsh"], self.cwd)
        # deadline accommodates hc's initial full-state snapshot replay
        self.assertTrue(ready.wait(timeout=25),
                        f"no created event seen; got {len(seen)} lines")
        self.hc.stop_events()
        self.assertTrue(thread.daemon)


class TestTypedErrors(ShimBase):
    def test_50_wait_children_mode_validation(self):
        with self.assertRaises(HCError):
            self.hc.wait_children(mode="surprise")

    def test_51_get_unknown_is_typed(self):
        with self.assertRaises(HCError):
            self.hc.get(f"{TAG}-does-not-exist")

    def test_52_term_defaults(self):
        t = Term(name="x")
        self.assertEqual((t.state, t.pid, t.turn), (None, None, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
