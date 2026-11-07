#!/usr/bin/env python3
"""Escalation T7 corpus (repurposed post-durable-queue) — issue #507.

The ladder is a MANUAL worker-poke toolkit now (nudge/re-ring/re-create);
it has no terminal rung and the engine never fires it automatically —
waiting is a valid task state, not a violation. fire() tries the rungs
top-down and returns an explicit pending `wait` when no terminal can
receive the poke: never terminal, never mutating room state, never paging
a human.
"""

import json
import os
import shutil
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(HERE, "scripts"))

from herdr_engine import escalate as E  # noqa: E402
from herdr_engine.fsm import Engine, RoomRuntime, load_state, save_state  # noqa: E402
from herdr_engine.scheduler import Scheduler  # noqa: E402

ROOM = 9901
OID = "dev:2026-11-07T10:00:00"


def ts(i):
    return f"2026-11-07T{9 + i // 60:02d}:{i % 60:02d}:00Z"


class SimHerdr:
    def __init__(self):
        self.terms = {}
        self.sends = []
        self.kills = []
        self.removes = []

    def spawn(self, name, room, lane):
        self.terms[name] = {"labels": {"room": str(room), "lane": lane}}

    def ls(self):
        # multiplex summary: no labels (T8 live shape)
        return [type("T", (), {"name": n, "raw": {}})()
                for n, d in self.terms.items()]

    def get(self, name):
        # labels arrive only through get()
        return type("T", (), {"name": name, "raw": {"labels": self.terms[name]["labels"]}})()

    def send(self, name, text):
        if name not in self.terms:
            raise RuntimeError("send to dead terminal")
        self.sends.append((name, text))

    def kill(self, name):
        self.kills.append(name)
        self.terms.pop(name, None)

    def rm(self, name):
        self.removes.append(name)


class Base(unittest.TestCase):
    def setUp(self):
        self.d = f"/tmp/nherdr-{ROOM}"
        shutil.rmtree(self.d, ignore_errors=True)
        os.makedirs(self.d)
        save_state(ROOM, {"v": 1, "issue": ROOM, "phase": "IMPL", "state": "IDLE",
                          "pending": "", "orders": {OID: {
                              "state": "open", "lane": "dev"}},
                          "entered_ts": {"IMPL": ts(1)}})
        self.h = SimHerdr()
        self.h.spawn("free-dev", ROOM, "dev")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def rt(self, herdr=True):
        return RoomRuntime(herdr=self.h if herdr else None)

    def esc(self, herdr=True, clock=None):
        return E.Escalator(herdr=self.h if herdr else None, clock=clock)


class RungMapping(Base):
    def test_01_count_to_rung(self):
        self.assertEqual(E.Escalator.rung_for(1), ("nudge", 0))
        self.assertEqual(E.Escalator.rung_for(2), ("re-ring", 1))
        self.assertEqual(E.Escalator.rung_for(3), ("re-create", 2))
        self.assertEqual(E.Escalator.rung_for(9), ("re-create", 2),
                         "counts clamp to the last mechanical rung — never terminal")

    def test_02_nudge_sends(self):
        esc = self.esc().fire(ROOM, OID, self.rt())
        self.assertEqual(esc.action, "nudge")
        self.assertFalse(esc.terminal)
        self.assertEqual(esc.payload.get("sent"), True)
        self.assertEqual(len(self.h.sends), 1)
        self.assertIn("nudge", self.h.sends[0][1])

    def test_04_recreate_kills_exactly_one_terminal(self):
        h = SimHerdr()
        h.spawn("x-dev", ROOM, "dev")
        # drive straight to the re-create rung via the Rungs unit
        rung = E.Rungs(herdr=h)
        ok, name = rung.re_create(ROOM, OID, RoomRuntime(herdr=h), "dev")
        self.assertTrue(ok)
        self.assertEqual(name, "x-dev")
        self.assertEqual(h.kills, ["x-dev"])
        self.assertEqual(h.removes, ["x-dev"])

    def test_05_no_terminal_means_pending_wait(self):
        self.h.terms.pop("free-dev")
        esc = self.esc(herdr=True).fire(ROOM, OID, self.rt())
        self.assertEqual(esc.action, "wait", "nothing to poke: explicit wait, not silence")
        self.assertFalse(esc.terminal)
        st = load_state(ROOM)
        self.assertEqual(st["orders"][OID]["state"], "open", "wait mutates nothing")
        self.assertNotIn("operator", st["orders"][OID])
        self.assertNotIn("operator_needed", st["orders"][OID])

    def test_06_wait_is_not_terminal_even_repeatedly(self):
        self.h.terms.pop("free-dev")
        for _ in range(5):
            esc = self.esc().fire(ROOM, OID, self.rt())
            self.assertEqual(esc.action, "wait")
            self.assertFalse(esc.terminal)
        st = load_state(ROOM)
        self.assertEqual(st["orders"][OID]["state"], "open")

    def test_07_dry_run_writes_nothing(self):
        esc = E.Escalator(dry=True).fire(ROOM, OID, self.rt())
        self.assertFalse(esc.terminal)
        st = load_state(ROOM)
        self.assertEqual(st["orders"][OID]["state"], "open")

    def test_08_status_shows_plain_open(self):
        self.esc().fire(ROOM, OID, self.rt())
        eng = Engine(runtimes={ROOM: RoomRuntime()}, clock=lambda: ts(9))
        s = eng.status([ROOM])
        self.assertIn("open", s)
        self.assertNotIn("blocked_on_operator", s)
        self.assertNotIn("NEED(", s)


class SlotIntegration(unittest.TestCase):
    def test_30_scheduler_status_surface_supported(self):
        self.s = Scheduler()
        self.s.acquire(9701, "impl")
        self.assertIn("impl-1:9701", self.s.status())
        self.assertIn("queue=[", self.s.status())


class DiagnosticRungs(unittest.TestCase):
    def setUp(self):
        self.d = "/tmp/nherdr-9921"
        shutil.rmtree(self.d, ignore_errors=True)
        os.makedirs(self.d)
        json.dump({"v": 1, "issue": 9921, "phase": "IMPL", "state": "IDLE",
                   "pending": "", "orders": {}, "entered_ts": {"IMPL": "2026-11-07T09:00:00Z"}},
                  open(self.d + "/state.json", "w"))
        self.h = SimHerdr()
        self.h.spawn("d1", 9921, "dev")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_40_nudge_carries_gate_requirement(self):
        from herdr_engine.escalate import Escalator
        from herdr_engine.fsm import RoomRuntime
        esc = Escalator(herdr=self.h).fire(9921, "dev:2026-11-07T10:00:00",
                                           RoomRuntime(herdr=self.h))
        self.assertEqual(esc.action, "nudge")
        sent = self.h.sends[-1][1]
        self.assertIn("to advance IMPL", sent)
        self.assertIn("diff: covered", sent)

    def test_41_no_terminal_never_silent_never_terminal(self):
        from herdr_engine.escalate import Escalator
        from herdr_engine.fsm import RoomRuntime
        self.h.terms.clear()
        esc = Escalator(herdr=self.h).fire(9921, "dev:2026-11-07T10:00:00",
                                           RoomRuntime(herdr=self.h))
        self.assertEqual(esc.action, "wait")
        self.assertFalse(esc.terminal)
        st = load_state(9921)
        self.assertEqual(st.get("orders", {}), {})


if __name__ == "__main__":
    unittest.main(verbosity=1)
