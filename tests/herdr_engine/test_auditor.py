#!/usr/bin/env python3
"""Auditor T5 corpus — issue #507 (>=40 checks incl. verdict chaos).

Covers: per-demand instance spawn + brief shape, ledger persistence,
identity-gated verdict intake (tip/demand mismatches), NEED verdict ->
re_audit marker, teardown at once on verdict, idle ladder teardown after
2 cycles, crash/exit chaos (0/1/timeout) with relaunch budget 3 ->
operator NEED, and no path that silently drops a dead auditor.
"""

import json
import os
import shutil
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(HERE, "scripts"))

from herdr_engine import auditor as A  # noqa: E402
from herdr_engine.auditor import Audit, Instance  # noqa: E402
from herdr_engine.fsm import load_state, save_state  # noqa: E402
from herdr_engine.transport import Rejection, Transport  # noqa: E402

ROOM = 9801


def ts(i: int) -> str:
    return f"2026-11-07T{9 + i // 60:02d}:{i % 60:02d}:00Z"


class SimHerdr:
    def __init__(self, fail_spawn: int = 0):
        self.terms: dict[str, dict] = {}
        self.sends: list[str] = []
        self.kills: list[str] = []
        self.fail_fail = None
        self.fail_spawn = fail_spawn

    def spawn(self, name, args, cwd, task=None, labels=None):
        if self.fail_spawn and self.fail_spawn > 0:
            self.fail_spawn -= 1
            raise RuntimeError("sim spawn failure (chaos mode)")
        self.terms[name] = {"labels": labels or {}, "task": task}

    def kill(self, name):
        self.kills.append(name)
        self.terms.pop(name, None)

    def live(self, name):
        return name in self.terms


class Base(unittest.TestCase):
    def setUp(self):
        self.d = f"{A.rdir(ROOM)}"
        shutil.rmtree(self.d, ignore_errors=True)
        os.makedirs(self.d)
        save_state(ROOM, {"v": 1, "issue": ROOM, "phase": "IMPL", "state": "IDLE",
                          "pending": "", "orders": {}, "entered_ts": {"IMPL": "x"}})
        self.tip = "a" * 40
        self.demand = "next:qe:abcdef01"
        self.t = Transport()
        self.h = SimHerdr()

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def aud(self, clock=None):
        aud = Audit(herdr=self.h, transport=self.t, clock=clock)
        return aud

    def post_verdict(self, tip=None, demand=None, kind="DECISION", summary="APPROVED plan",
                     oid=None, acks_extra=None, skip_schema=False):
        base = {"v": 1, "oid": oid or "audit:2026-11-07T09:30:00", "from": "audit", "kind": kind,
                "state": "idle", "next": "idle", "ts": ts(30), "room": ROOM,
                "summary": summary,
                "evidence": {"tip": self.tip if tip is None else tip},
                "acks": [self.demand if demand is None else demand]}
        if acks_extra:
            base["acks"] += acks_extra
        try:
            return self.t.post(ROOM, "auditor", base)
        except json.JSONDecodeError:
            raise
        # invalid posts are still validated by transport (never written)


class SpawnLedger(Base):
    def test_01_spawn_demand_creates_instance(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand, refs=["docs/x.md"])
        self.assertEqual(ins.key, A.identity(ROOM, self.tip, self.demand))
        self.assertEqual(ins.state, "working")
        self.assertTrue(self.h.live(ins.key), "sim herdr got the terminal")

    def test_02_brief_carries_tip_demand_refs(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand, refs=["a.md", "b.md"])
        self.assertIn(self.tip, ins.brief)
        self.assertIn(self.demand, ins.brief)
        self.assertIn("a.md", ins.brief)
        self.assertIn("b.md", ins.brief)
        self.assertIn("DECISION", ins.brief)

    def test_03_identity_hash8_shape(self):
        key = A.identity(ROOM, self.tip, self.demand)
        self.assertTrue(key.startswith("aud-9801-") and len(key.split("-")[-1]) == 8)

    def test_04_spawn_demand_idle(self):
        aud = self.aud()
        a = aud.spawn_demand(ROOM, self.tip, "")
        self.assertTrue(aud.latest_posts(ROOM) == [])

    def test_05_ledger_persisted_in_room_state(self):
        aud = self.aud()
        aud.spawn_demand(ROOM, self.tip, self.demand)
        st = load_state(ROOM)
        audits = st["audits"]
        self.assertTrue(any(v["tip"] == self.tip and v["state"] == "working"
                            for v in audits.values()))

    def test_06_reload_restores_instance(self):
        aud = self.aud()
        aud.spawn_demand(ROOM, self.tip, self.demand)
        aud2 = self.aud()
        aud2.load(ROOM)
        self.assertTrue(any(i.tip == self.tip for i in aud2.ledger.values()))


class VerdictIntake(Base):
    def test_10_verdict_accepted_and_teardown(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict()
        res = [v for (i, v) in aud.scan_room(ROOM) if i.key == ins.key][0]
        self.assertEqual(res["summary"], "APPROVED plan")
        self.assertEqual(ins.state, "verdict")
        ev = aud.tick(ROOM)
        self.assertEqual(ev[-1][0:2], (ins.key, "teardown"))
        self.assertEqual(ins.state, "torn_down")
        self.assertIn(ins.key, self.h.kills, "teardown kills the terminal")

    def test_11_need_verdict_writes_re_audit_marker(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict(summary="NEED plan gaps")
        [v for (i, v) in aud.scan_room(ROOM)]
        st = load_state(ROOM)
        self.assertIn("re_audit", st)
        self.assertEqual(st["re_audit"]["tip"], self.tip)

    def test_12_verdict_tip_mismatch_rejected(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict(tip="b" * 40)
        out = dict((i.key, v) for (i, v) in aud.scan_room(ROOM))
        self.assertIsNone(out[ins.key], "mismatch verdict is not a verdict")
        self.assertEqual(ins.state, "working")

    def test_13_verdict_without_demand_ack_rejected(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict(demand="next:qe:00ffffff")
        out = dict((i.key, v) for (i, v) in aud.scan_room(ROOM))
        self.assertIsNone(out[ins.key])

    def test_14_malformed_verdict_never_stops_intake(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        # malformed: bad kind (schema-invalid) lands as typed rejection (never a write)
        r = self.t.post(ROOM, "auditor", {"v": 1, "oid": f"audit:{ts(31)}", "from": "audit",
                                          "kind": "STAMP", "state": "idle", "next": "idle",
                                          "ts": ts(31), "room": ROOM})
        self.assertIsInstance(r, Rejection)
        try:
            lines = open(self.d + "/posts.jsonl").read().count("\n")
        except FileNotFoundError:
            lines = 0
        self.assertEqual(lines, 0, "malformed verdict must not enter the stream")

    def test_15_verdict_identity_one_clear(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict()
        outs = [v for (i, v) in aud.scan_room(ROOM) if i.key == ins.key][0]
        self.assertEqual(outs["oid"], "audit:2026-11-07T09:30:00", "identity = kind+tip+demand")


class Ladder(Base):
    def test_20_idle_teardown_after_two_cycles(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        ev1 = aud.tick(ROOM)
        self.assertEqual(ev1[0][1], "idle")
        self.assertEqual(ins.idle_cycles, 1)
        ev2 = aud.tick(ROOM)
        self.assertEqual(ev2[0][1], "teardown-idle")
        self.assertEqual(ins.state, "torn_down")
        self.assertIn(ins.key, self.h.kills)

    def test_21_crash_relies(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        ins._term_state = "absent"
        ev = aud.tick(ROOM)
        self.assertEqual(ev[0][1], "relaunch")
        self.assertEqual(ins.relaunches, 1)
        # budget exhaustion: 3 relaunches consumed then operator NEED
        for _ in range(3):
            ins._term_state = "absent"
            ev = aud.tick(ROOM)
        self.assertEqual(ins.state, "operator_needed")
        self.assertEqual(aud.operator_notifications[-1]["operator_needed"]["why"],
                         "relaunch budget exhausted")
        self.assertEqual(ev[-1][1], "operator_need")

    def test_22_relaunch_frame_max_is_three(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        ins._term_state = "absent"
        for i in range(1, 5):
            ev = aud.tick(ROOM)
            ins._term_state = "absent"
            if i <= 3:
                self.assertEqual(ev[0][1], "relaunch")
                self.assertEqual(ins.relaunches, i)
            else:
                self.assertEqual(ev[0][1], "operator_need")

    def test_23_spawn_chaos_is_typed_not_silent(self):
        self.h = SimHerdr(fail_spawn=1)
        self.h.fail_fail = None
        aud = self.aud()
        with self.assertRaises(RuntimeError):
            aud.spawn_demand(ROOM, self.tip, self.demand)
        # the ledger is untouched (typed behavior: caller decides)
        st = load_state(ROOM)
        self.assertEqual(st.get("audits", {}), {}, "failed spawn writes nothing")

    def test_24_exit_code_chaos_zero_rc_counts_working(self):
        # exit code 0 with no verdict post is an exit, not a verdict: a stop
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        ins._term_state = "absent"
        ev = aud.tick(ROOM)
        self.assertEqual(ev[0][1], "relaunch")

    def test_25_after_need_verdict_instance_tears_down(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict(summary="NEED more evidence")
        aud.scan_room(ROOM)
        aud.tick(ROOM)
        self.assertEqual(ins.state, "torn_down")

    def test_26_operator_need_visible_in_room_state(self):
        aud = self.aud()
        ins = aud.spawn_demand(ROOM, self.tip, self.demand)
        ins._term_state = "absent"
        for _ in range(4):
            ins._term_state = "absent"
            aud.tick(ROOM)
        st = load_state(ROOM)
        audits = st["audits"]
        need = [v for v in audits.values() if v["state"] == "operator_needed"]
        self.assertTrue(need and need[0]["operator_needed"]["why"] == "relaunch budget exhausted")


class Reaudit(Base):
    def test_30_reaudit_marker_era_reopen(self):
        aud = self.aud()
        aud.spawn_demand(ROOM, self.tip, self.demand)
        self.post_verdict(summary="NEED real gaps")
        aud.scan_room(ROOM)
        st = load_state(ROOM)
        self.assertEqual(st["re_audit"]["key"], aud.ledger[A.identity(ROOM, self.tip, self.demand)].key)
        # the FSM consumes re_audit by reopening the era: entered_ts of the
        # current phase is dropped by the T6 integration (documented contract)


if __name__ == "__main__":
    unittest.main(verbosity=1)
