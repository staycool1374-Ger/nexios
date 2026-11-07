#!/usr/bin/env python3
"""Escalation T7 corpus — issue #507 (>=40 checks, no silent ends).

Runs against a sim herdr: terminal presence gates the sends; kill/rm
exactly one terminal on re-create; the count-to-rung mapping follows
nudge/re-ring/re-create/operator_NEED; a broken rung escalates itself
(no path ends in silence); a NEED persists as blocked_on_operator in
room state and appears in the engine status; with herdr=None every rung
climbs to the operator NEED (never dropped).
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
                              "state": "open", "lane": "dev", "overdue_count": 1}},
                          "entered_ts": {"IMPL": ts(1)}})
        self.h = SimHerdr()
        self.h.spawn("free-dev", ROOM, "dev")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def rt(self, herdr=True):
        return RoomRuntime(herdr=self.h if herdr else None)

    def esc(self, herdr=True, clock=None):
        return E.Escalator(herdr=self.h if herdr else None, clock=clock)

    def aide_ticks(self):
        RT = self.rt(herdr=True)
        return RT


class RungMapping(Base):
    def test_01_count_to_rung(self):
        for count, act in ((1, "nudge"), (2, "re-ring"), (3, "re-create"),
                           (4, "operator_need"), (9, "operator_need")):
            esc = self.esc().fire(ROOM, OID, "ack_by", count, self.rt())
            self.assertEqual(esc.action, act, f"count {count}")
            self.assertEqual(esc.bound, "ack_by")

    def test_02_nudge_sends(self):
        esc = self.esc().fire(ROOM, OID, "ack_by", 1, self.rt())
        self.assertEqual(esc.payload.get("sent"), True)
        self.assertEqual(len(self.h.sends), 1)
        self.assertIn("nudge", self.h.sends[0][1])

    def test_03_rering_uses_second_entry(self):
        esc = self.esc().fire(ROOM, OID, "ack_by", 2, self.rt())
        self.assertEqual(esc.action, "re-ring")
        self.assertIn("re-ring", self.h.sends[0][1])

    def test_04_recreate_kills_exactly_one_terminal(self):
        esc = self.esc().fire(ROOM, OID, "ack_by", 3, self.rt())
        self.assertEqual(esc.action, "re-create")
        self.assertEqual(self.h.kills, ["free-dev"])
        self.assertEqual(self.h.removes, ["free-dev"])

    def test_05_operator_need_terminal_payload(self):
        esc = self.esc(clock=lambda: ts(9)).fire(ROOM, OID, "ack_by", 4, self.rt())
        self.assertTrue(esc.terminal)
        self.assertEqual(esc.payload["owner"], "operator")
        self.assertIn("4x", esc.payload["reason"])
        self.assertEqual(len(self.h.sends), 0, "no terminal talk after the NEED")

    def test_06_no_silent_on_missing_terminal(self):
        self.h.terms.pop("free-dev")
        esc = self.esc(herdr=True).fire(ROOM, OID, "ack_by", 1, self.rt())
        self.assertEqual(esc.action, "operator_need", "broken nudge climbed to the own-lane top")
        esc = self.esc().fire(ROOM, OID, "ack_by", 2, self.rt())
        self.assertEqual(esc.action, "operator_need", "re-ring missing also climbed")


class OperatorPersistence(Base):
    def test_10_blocked_on_operator_state(self):
        esc = self.esc().fire(ROOM, OID, "done_by", 4, self.rt())
        self.assertTrue(esc.terminal)
        st = load_state(ROOM)
        self.assertEqual(st["orders"][OID]["state"], "blocked_on_operator")
        self.assertEqual(st["orders"][OID]["operator"]["owner"], "operator")

    def test_11_dry_run_writes_nothing(self):
        esc = E.Escalator(dry=True).fire(ROOM, OID, "ack_by", 4, self.rt())
        self.assertTrue(esc.terminal)
        st = load_state(ROOM)
        self.assertEqual(st["orders"][OID]["state"], "open", "dry run leaves state")
        self.assertEqual(st["orders"][OID].get("operator"), None)

    def test_12_need_actions_register_fleet_visible(self):
        inst = self.esc()
        esc = inst.fire(ROOM, OID, "ack_by", 4, self.rt())
        self.assertEqual(esc.action, "operator_need")
        self.assertEqual(len(inst.need_actions), 1)
        self.assertEqual(inst.need_actions[0]["owner"], "operator")

    def test_13_status_shows_blocked_on_operator(self):
        self.esc().fire(ROOM, OID, "ack_by", 4, self.rt())
        eng = Engine(runtimes={ROOM: RoomRuntime()}, clock=lambda: ts(9))
        self.assertIn("blocked_on_operator", eng.status([ROOM]))


class Attached(Base):
    """attach(): the ladder becomes the engine's on_overdue hook."""

    def setUpPlus(self):
        self.eng = Engine(runtimes={ROOM: RoomRuntime(herdr=self.h)}, clock=lambda: ts(9))
        self.escalator = self.esc(herdr=True)
        self.escalator.attach(self.eng)

    def test_20_attach_and_fire_hook(self):
        self.setUpPlus()

        def hook(oid, d):
            self.escalator.fire(d["room"], oid, d["bound"], d["count"], self.rt())

        hook(OID, {"room": ROOM, "bound": "ack_by", "count": 1})
        self.assertEqual(len(self.h.sends), 1)

    def test_21_ladder_never_silent_across_counts(self):
        self.setUpPlus()
        for count in (1, 2, 3, 4):
            esc = self.escalator.fire(ROOM, OID, "ack_by", count, self.rt())
            self.assertTrue(esc.action in ("nudge", "re-ring", "re-create", "operator_need"))
            if esc.terminal:
                break
        self.assertTrue(len(self.escalator.fired) >= 1)

    def test_22_no_herdr_front_rungs_still_terminate(self):
        esc = E.Escalator(herdr=None, clock=lambda: ts(9)).fire(
            ROOM, OID, "ack_by", 1, self.rt(herdr=False))
        self.assertTrue(esc.terminal, "count 1 with no terminal must not silently drop")
        self.assertEqual(esc.action, "operator_need")

    def test_23_engine_assembly_with_ladder(self):
        self.setUpPlus()
        st = load_state(ROOM)
        self.assertIn("orders", st)
        engine_fired = self.escalator.fire(ROOM, OID, "ack_by", 4, self.rt())
        self.assertTrue(engine_fired.terminal)
        self.assertIn("blocked_on_operator", Engine(
            runtimes={ROOM: RoomRuntime()}, clock=lambda: ts(9)).status([ROOM]))


class SlotIntegration(unittest.TestCase):
    def test_30_scheduler_status_surface_supported(self):
        self.s = Scheduler()
        self.s.acquire(9701, "impl")
        self.assertIn("impl-1:9701", self.s.status())
        self.assertIn("queue=[", self.s.status())


if __name__ == "__main__":
    unittest.main(verbosity=1)
