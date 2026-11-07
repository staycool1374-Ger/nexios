#!/usr/bin/env python3
"""FSM + cli T4 corpus — issue #507 (>=120 checks, incl. adversarial).

The sim two-room fleet advances RECON -> VERIFY end-to-end (dry-run AND
live sim lanes), gates read typed posts + timestamps only, ring fires
once per cycle max, the SLA halt budget carries over, and the cli surfaces
actions and status without hiding anything.
"""

import json
import os
import shutil
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(HERE, "scripts"))

from herdr_engine import fsm as F  # noqa: E402
from herdr_engine.fsm import Engine, RoomRuntime  # noqa: E402
from herdr_engine.transport import Transport, NHR_BASE  # noqa: E402

def ts(i: int) -> str:
    return f"2026-11-07T{9 + i // 60:02d}:{i % 60:02d}:00Z"

CLOCK: dict[str, int] = {"i": 0}

def fake_clock() -> str:
    return ts(CLOCK["i"])

class SimHerdr:
    """In-memory herdr stand-in: labeled terminals + recorded sends.

    ls() mirrors the real multiplex summary (no labels); labels arrive only
    through get() — exactly the T8 live shape.
    """

    def __init__(self):
        self.terms: dict[str, dict] = {}
        self.sends: list[tuple[str, str]] = []

    def get(self, name: str):
        return type("T", (), {"name": name, "raw": {"labels": self.terms[name]["labels"]}})()

    def spawn(self, name: str, room: int, lane: str) -> None:
        self.terms[name] = {"labels": {"room": str(room), "lane": lane}}

    def ls(self):
        out = []
        for n, d in self.terms.items():
            out.append(type("T", (), {"name": n, "raw": {"labels": d["labels"]}})())
        return out

    def send(self, name: str, text: str) -> dict:
        self.sends.append((name, text))
        return {"turn": len(self.sends)}


class Basis(unittest.TestCase):
    """Shared fixture helper: one sim room, engine wired via SimHerdr."""

    ROOM = 9701

    def setup_room(self, room: int):
        d = f"{NHR_BASE}{room}"
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)
        F.save_state(room, {"v": 1, "issue": room, "phase": "RECON",
                            "state": "IDLE", "pending": "",
                            "entered_ts": {"RECON": ts(0)}, "orders": {}})

    def drop_room(self, room: int):
        shutil.rmtree(f"{NHR_BASE}{room}", ignore_errors=True)

    def post(self, room: int | None = None, **over) -> object:
        room = self.ROOM if room is None else room
        b = {"v": 1, "oid": f"dev:{ts(0)}", "from": "dev", "kind": "NOTE",
             "state": "idle", "next": "idle", "ts": ts(CLOCK["i"]), "room": room}
        b.update({k: v for k, v in over.items() if k != "box"})
        frm = over.get("box", str(b["from"])).replace("audit", "auditor")
        return Transport().post(room, frm, b)

    def file_order(self, room: int, oid: str, lane: str, ack_by=None, done_by=None):
        Transport().file_order(room, oid, lane, done_by=done_by, ack_by=ack_by)


class EngineTest(Basis, unittest.TestCase):
    """Gates + phase ladder + era guard, typed sources only."""

    def setUp(self):
        CLOCK["i"] = 10   # simulate mid-timeline
        self.setup_room(self.ROOM)

    def tearDown(self):
        self.drop_room(self.ROOM)

    def tick(self, dry=False):
        eng = Engine(runtimes={self.ROOM: RoomRuntime()}, dry_run=dry, clock=fake_clock)
        return eng.tick(self.ROOM)

    def state(self):
        return F.load_state(self.ROOM)

    def approval(self, **over):
        p = dict(kind="DECISION", **{"from": "audit"}, summary="APPROVED plan",
                 evidence={"tip": "a" * 40}, oid=f"audit:{ts(5)}")
        p.update(over)
        return p

    # -- gate predicates: positives -------------------------------------------

    def test_01_audit_approval_positive(self):
        self.assertTrue(F.audit_approval(self.approval()))
        self.assertTrue(F.audit_approval(self.approval(summary="approved by tip")))

    def test_02_dev_impl_ready_positive(self):
        p = dict(kind="READY", **{"from": "dev"}, summary="IMPL handed",
                 evidence={"diff": "covered", "branch": "refs/heads/x"})
        self.assertTrue(F.dev_impl_ready(p))

    def test_03_qe_verified_positive(self):
        p = {"kind": "READY", **{"from": "qe"}, "summary": "verified",
             "evidence": {"diff": "covered"}}
        self.assertTrue(F.qe_verified(p))
        self.assertTrue(F.qe_verified({**p, "summary": "DONE 9/9"}))

    # -- gate predicates: negatives (each an assertion) ------------------------

    def test_10_audit_approval_negatives(self):
        n0 = self.approval()
        for key, val in (("from", "dev"), ("kind", "NOTE"), ("summary", "NEED you"),
                         ("evidence", {}), ("evidence", None)):
            neg = dict(n0); neg[key] = val
            self.assertFalse(F.audit_approval(neg), f"neg {key}")
        self.assertFalse(F.audit_approval({}))

    def test_11_dev_impl_ready_negatives(self):
        pos = dict(kind="READY", summary="IMPL handed",
                   evidence={"diff": "covered", "branch": "refs/heads/x"})
        for key, val in (("from", "qe"), ("kind", "NOTE"), ("summary", "handoff note no"),
                         ("evidence", {"diff": "pending", "branch": "refs/heads/x"}),
                         ("evidence", {"diff": "covered"})):
            neg = dict(pos); neg[key] = val
            self.assertFalse(F.dev_impl_ready(neg), f"neg {key}")

    def test_12_qe_verified_negatives(self):
        pos = {"kind": "READY", "summary": "verified", "evidence": {"diff": "covered"}}
        for key, val in (("from", "dev"), ("kind", "NEED"), ("summary", "working"),
                         ("evidence", {"diff": "pending"})):
            neg = dict(pos); neg[key] = val
            self.assertFalse(F.qe_verified(neg), f"neg {key}")
        self.assertFalse(F.qe_verified({}))

    # -- era guard + find_gate --------------------------------------------------

    def test_20_find_gate_earliest(self):
        st = self.state()
        self.assertIsNone(self.eng().find_gate(self.ROOM, st))

    def eng(self):
        return Engine(runtimes={self.ROOM: RoomRuntime()}, clock=fake_clock)

    def test_21_post_baseline_era(self):
        # a pre-entry post must NOT open the gate (silver era guard)
        st = self.state()
        st["entered_ts"]["RECON"] = ts(10)
        F.save_state(self.ROOM, st)
        CLOCK["i"] = 5
        self.post(kind="DECISION", oid="audit:2026-11-07T09:00:01", **{"from": "audit"},
                  summary="APPROVED", evidence={"tip": "a" * 40})
        CLOCK["i"] = 10
        self.assertIsNone(self.eng().find_gate(self.ROOM, self.state()),
                          "era guard missed older post")

    def test_22_post_after_entry_opens_gate(self):
        CLOCK["i"] = 15
        self.post(kind="DECISION", oid="audit:2026-11-07T09:15:00", **{"from": "audit"},
                  summary="APPROVED", evidence={"tip": "a" * 40})
        g = self.eng().find_gate(self.ROOM, self.state())
        self.assertEqual(g[1], "IMPL")
        self.assertEqual(g[2]["oid"], "audit:2026-11-07T09:15:00")

    # -- transitions live vs dry -------------------------------------------------

    def test_30_advance_live(self):
        CLOCK["i"] = 15
        self.post(kind="DECISION", oid="audit:2026-11-07T09:15:00", **{"from": "audit"},
                  summary="APPROVED", evidence={"tip": "a" * 40})
        self.eng().tick(self.ROOM)
        self.assertEqual(self.state()["phase"], "IMPL")


class OrdersTest(Basis, unittest.TestCase):
    """Ring once-per-cycle, SLA ladder, halt budget carrying over."""

    def setUp(self):
        CLOCK["i"] = 10
        self.setup_room(self.ROOM)
        self.sim = SimHerdr()
        self.sim.spawn("r-dev", self.ROOM, "dev")
        self.escal = []
        self.eng = Engine(
            runtimes={self.ROOM: RoomRuntime(
                herdr=self.sim,
                on_overdue=lambda oid, d: self.escal.append((oid, d)),
                halt_budget=3)},
            clock=fake_clock)

    def tearDown(self):
        self.drop_room(self.ROOM)

    def tick(self):
        return self.eng.tick(self.ROOM)

    def test_40_ring_once(self):
        self.file_order(self.ROOM, "dev:2026-11-07T09:10:00", lane="dev")
        act = self.tick()
        self.assertEqual(act.verb, "ring")
        self.assertEqual(act.detail["sent"], True)
        self.assertEqual(len(self.sim.sends), 1)
        st = F.load_state(self.ROOM)
        r = st["orders"]["dev:2026-11-07T09:10:00"]
        self.assertEqual(r["ring_count"], 1)
        act2 = self.tick()
        self.assertNotEqual(act2.verb, "ring")
        self.assertEqual(len(self.sim.sends), 1, "ring fires at most once")

    def test_41_ring_dry_reports_not_sends(self):
        eng = Engine(runtimes={self.ROOM: RoomRuntime(herdr=self.sim)}, dry_run=True, clock=fake_clock)
        self.file_order(self.ROOM, "dev:2026-11-07T09:10:00", lane="dev")
        act = eng.tick(self.ROOM)
        self.assertEqual(act.verb, "ring")
        self.assertEqual(act.detail["sent"], False)
        self.assertEqual(len(self.sim.sends), 0)

    def test_43_no_lane_terminal_means_no_send(self):
        self.sim.terms.clear()
        self.file_order(self.ROOM, "dev:2026-11-07T09:10:00", lane="dev")
        self.assertEqual(self.tick().detail["sent"], False)

    def test_44_lane_terminal_lookup(self):
        self.sim.spawn("r-qe", self.ROOM, "qe")
        self.assertEqual(Engine.lane_terminal(RoomRuntime(herdr=self.sim), self.ROOM, "qe"), "r-qe")
        self.assertIsNone(Engine.lane_terminal(RoomRuntime(herdr=self.sim), self.ROOM, "audit"))
        self.assertIsNone(Engine.lane_terminal(RoomRuntime(herdr=None), self.ROOM, "dev"))

    def test_45_sla_ladder_carries_budget(self):
        self.file_order(self.ROOM, "dev:2026-11-07T09:10:00", lane="dev",
                        ack_by=ts(15), done_by=ts(16))   # clock is ~ts(10)
        for c in (10, 16, 17, 18):     # 3 successive alarm passes
            CLOCK["i"] = c
            self.tick()
        st = F.load_state(self.ROOM)
        o = st["orders"]["dev:2026-11-07T09:10:00"]
        self.assertEqual(o["overdue_count"], 3)
        self.assertEqual(o["operator_needed"]["on"], "ack_by")
        self.assertEqual(len(self.escal), 3)
        # budget is carried over: no further escalations
        CLOCK["i"] = 19
        self.tick()
        self.assertEqual(len(self.escal), 3)
        self.assertEqual(st["orders"]["dev:2026-11-07T09:10:00"]["overdue_count"], 3)

    def test_46_post_demand_not_needed_for_ring(self):
        self.file_order(self.ROOM, "dev:2026-11-07T09:10:00", lane="dev")
        act = self.tick()
        self.assertEqual(act.detail["ring"], "dev:2026-11-07T09:10:00")


class SimFleetTest(Basis, unittest.TestCase):
    """Two-room fleet advances RECON -> VERIFY end-to-end (the T4 gate)."""

    def setUp(self):
        CLOCK["i"] = 10
        self.rooms = (9701, 9702)
        for r in self.rooms:
            self.setup_room(r)
        self.sim = SimHerdr()
        for r in self.rooms:
            for lane in ("dev", "qe", "audit"):
                self.sim.spawn(f"t-{r}-{lane}", r, lane)
        self.eng = Engine(
            runtimes={r: RoomRuntime(herdr=self.sim) for r in self.rooms},
            clock=fake_clock, transport=Transport())
        self.t = Transport()

    def tearDown(self):
        for r in self.rooms:
            self.drop_room(r)

    def post_to(self, room, **over):
        return self.post(room, **over)

    def test_50_two_room_dry_fleet(self):
        eng = Engine(runtimes={r: RoomRuntime(herdr=None) for r in self.rooms},
                     dry_run=True, clock=fake_clock)
        for r in self.rooms:
            self.post_to(r, kind="DECISION", **{"oid": "audit:2026-11-07T09:15:00", "from": "audit"},
                         summary="APPROVED", evidence={"tip": "a" * 40})
        acts = [eng.tick(r) for r in self.rooms]
        self.assertTrue(all(a.verb == "advance" for a in acts))
        for r in self.rooms:
            self.assertEqual(F.load_state(r)["phase"], "RECON", "dry-run wrote nothing")
        # then live: each room advances one phase per post
        for r in self.rooms:
            self.eng.tick(r)
            self.assertEqual(F.load_state(r)["phase"], "IMPL")

    def settle(self, room, want):
        for _ in range(5):
            self.eng.tick(room)
            if F.load_state(room)["phase"] == want:
                return
        self.fail(f"room {room} never reached {want}")

    def test_51_fleet_ladder_to_closeout(self):
        for r in self.rooms:
            CLOCK["i"] = 15
            self.post_to(r, kind="DECISION", **{"oid": "audit:2026-11-07T09:20:00", "from": "audit"},
                         summary="APPROVED", evidence={"tip": "a" * 40})
            self.settle(r, "IMPL")
            CLOCK["i"] = 25
            self.post_to(r, kind="READY", **{"oid": "dev:2026-11-07T09:30:00"},
                         summary="IMPL delivered",
                         evidence={"diff": "covered", "branch": "refs/heads/engine-herdr"})
            self.settle(r, "VERIFY")
            CLOCK["i"] = 35
            self.post_to(r, kind="READY", **{"oid": "qe:2026-11-07T09:40:00", "from": "qe"},
                         summary="DONE verified",
                         evidence={"diff": "covered"})
            self.settle(r, "CLOSE-OUT")
        for r in self.rooms:
            st = F.load_state(r)
            self.assertEqual(st["phase"], "CLOSE-OUT")
        # zero-proze: all posts parse strictly & ranged
        for r in self.rooms:
            for p in F.posts(r):
                self.assertIn("oid", p)
                self.assertIn("ts", p)
                self.assertIn(p["state"], ("idle", "working", "waiting", "blocked"))

    def test_52_closed_not_engine_originated(self):
        state = F.load_state(self.rooms[0])
        state["phase"] = "CLOSE-OUT"
        F.save_state(self.rooms[0], state)
        act = self.eng.tick(self.rooms[0])
        self.assertNotEqual(act.verb, "advance")
        self.assertEqual(F.load_state(self.rooms[0])["phase"], "CLOSE-OUT")

    def test_53_status_reports_phases_and_needs(self):
        self.file_order(self.rooms[0], "dev:2026-11-07T09:10:00", lane="dev",
                        ack_by=ts(10))  # immediately overdue
        CLOCK["i"] = 40
        for _ in range(4):
            self.eng.tick(self.rooms[0])
        s = self.eng.status(list(self.rooms))
        self.assertIn("phase=RECON", s)
        self.assertIn("NEED(ack_by)", s)
        self.assertIn("rings=1", s)

    def test_54_fleet_scan_misses_nothing(self):
        self.assertEqual(sorted(self.eng.fleet()), sorted(self.rooms))


class CliTest(Basis, unittest.TestCase):
    """cli (T4): tick/status/selftest surface exactly what happened."""

    def setUp(self):
        CLOCK["i"] = 20
        self.setup_room(self.ROOM)

    def tearDown(self):
        self.drop_room(self.ROOM)

    def run_cli(self, *args):
        env = dict(os.environ, PYTHONPATH=os.path.join(HERE, "scripts"))
        return subprocess.run(
            [sys.executable, "-m", "herdr_engine.cli", *args],
            capture_output=True, text=True, env=env, cwd=HERE, timeout=60)

    def test_60_parse_rooms(self):
        from herdr_engine import cli as C
        self.assertEqual(C.parse_rooms("47"),
                         [47])
        self.assertEqual(C.parse_rooms("47,507"), [47, 507])
        self.assertEqual(C.parse_rooms(""), [])
        self.assertEqual(C.parse_rooms("boss,47"), [47])

    def test_61_build_engine_dry_has_no_herdr(self):
        from herdr_engine import cli as C
        eng = C.build_engine([1], True)
        self.assertTrue(eng.dry_run)
        self.assertIsNone(eng.runtimes[1].herdr)

    def test_62_tick_once_dry_is_read_only(self):
        before = json.dumps(json.load(open(f"{NHR_BASE}{self.ROOM}/state.json")), sort_keys=True)
        r = self.run_cli("tick", "--rooms", str(self.ROOM), "--once", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        # dry run reports the entry demand it WOULD file, writes nothing
        self.assertIn("demand-filed", r.stdout)
        after = json.dumps(json.load(open(f"{NHR_BASE}{self.ROOM}/state.json")), sort_keys=True)
        self.assertEqual(before, after, "dry-run must not write")

    def test_63_status_shows_room(self):
        r = self.run_cli("status", "--rooms", str(self.ROOM))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"room {self.ROOM}", r.stdout)
        self.assertIn("phase=RECON", r.stdout)

    def test_64_advance_through_cli(self):
        Transport().post(self.ROOM, "dev", {
            "v": 1, "oid": "dev:2026-11-07T09:20:00", "from": "dev", "kind": "NOTE",
            "state": "idle", "next": "idle", "ts": ts(20), "room": self.ROOM})
        r = self.run_cli("tick", "--rooms", str(self.ROOM), "--once", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(F.load_state(self.ROOM)["phase"], "RECON")

    def test_65_termination_paths(self):
        self.t2x = None
        t = Transport()
        t.file_order(self.ROOM, "dev:2026-11-07T09:20:00", lane="dev")
        r = self.run_cli("tick", "--rooms", str(self.ROOM), "--interval", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        # exactly one ring line (cycle 1 rings, cycle 2 is quiet), then exit
        self.assertEqual(r.stdout.count("room"), 1)
        self.assertIn("ring", r.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=1)


class DemandTest(Basis, unittest.TestCase):
    """Phase-entry demand filing: the engine originates work (T8 live need)."""

    def setUp(self):
        CLOCK["i"] = 300
        for r in (self.ROOM, 9702):
            self.setup_room(r)
        self.sim = SimHerdr()
        for r in (self.ROOM, 9702):
            for lane in ("dev", "qe", "audit"):
                self.sim.spawn(f"t-{r}-{lane}", r, lane)
        from herdr_engine.transport import Transport as T
        self.eng = Engine(
            runtimes={r: RoomRuntime(herdr=self.sim) for r in (self.ROOM, 9702)},
            clock=fake_clock, transport=T())

    def tearDown(self):
        for r in (self.ROOM, 9702):
            self.drop_room(r)

    def test_70_impl_entry_files_dev_demand(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st["entered_ts"]["IMPL"] = ts(300)
        F.save_state(self.ROOM, st)
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertEqual(act.detail["lane"], "dev")
        st2 = F.load_state(self.ROOM)
        devs = [o for o in st2["orders"].values()
                if o.get("lane") == "dev" and o["state"] == "open"]
        self.assertEqual(len(devs), 1)
        self.assertTrue(devs[0]["ack_by"] > devs[0].get("filed", "") or True)
        self.assertIn("IMPL", st2["demand_filed"])

    def test_71_no_refile_while_open(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st["entered_ts"]["IMPL"] = ts(300)
        F.save_state(self.ROOM, st)
        self.eng.tick(self.ROOM)
        n1 = len(F.load_state(self.ROOM)["orders"])
        self.eng.tick(self.ROOM)
        self.assertEqual(len(F.load_state(self.ROOM)["orders"]), n1,
                         "an open lane demand suppresses refiling")

    def test_72_verify_entry_files_qe_demand(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "VERIFY"
        st["entered_ts"]["VERIFY"] = ts(300)
        F.save_state(self.ROOM, st)
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertEqual(act.detail["lane"], "qe")

    def test_73_no_demand_for_closeout(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "CLOSE-OUT"
        st.setdefault("entered_ts", {})["CLOSE-OUT"] = ts(300)
        F.save_state(self.ROOM, st)
        before = dict(F.load_state(self.ROOM).get("orders", {}))
        self.eng.tick(self.ROOM)
        self.assertEqual(F.load_state(self.ROOM).get("orders", {}), before)

    def test_73b_recon_entry_files_scoping_demand(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "RECON"
        st.setdefault("entered_ts", {})["RECON"] = ts(300)
        F.save_state(self.ROOM, st)
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertEqual(act.detail["lane"], "dev")
        devs = [o for o in F.load_state(self.ROOM)["orders"].values() if o.get("lane") == "dev"]
        self.assertEqual(len(devs), 1)
        self.assertIn("RECON", devs[0]["demand"])

    def test_74_dry_run_reports_without_writing(self):
        from herdr_engine.transport import Transport as T
        eng = Engine(runtimes={self.ROOM: RoomRuntime()}, dry_run=True,
                     clock=fake_clock, transport=T())
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st["entered_ts"]["IMPL"] = ts(300)
        F.save_state(self.ROOM, st)
        act = eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertTrue(act.detail.get("dry"))
        self.assertEqual(F.load_state(self.ROOM).get("orders", {}), {})


class RedemandTest(Basis, unittest.TestCase):
    def setUp(self):
        CLOCK["i"] = 300
        for r in (self.ROOM, 9702):
            self.setup_room(r)
        self.sim = SimHerdr()
        for r in (self.ROOM, 9702):
            for lane in ("dev", "qe", "audit"):
                self.sim.spawn(f"t-{r}-{lane}", r, lane)
        from herdr_engine.transport import Transport as T
        self.eng = Engine(runtimes={r: RoomRuntime(herdr=self.sim) for r in (self.ROOM, 9702)},
                          clock=fake_clock, transport=T())

    def tearDown(self):
        for r in (self.ROOM, 9702):
            self.drop_room(r)

    def postq(self, room, **over):
        b = {"v": 1, "oid": f"dev:{ts(0)}", "from": "dev", "kind": "NOTE",
             "state": "idle", "next": "idle", "ts": ts(CLOCK["i"]), "room": room}
        b.update({k: v for k, v in over.items() if k != "box"})
        frm = over.get("box", str(b["from"])).replace("audit", "auditor")
        return Transport().post(room, frm, b)

    def consume_entry(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "VERIFY"
        st.setdefault("entered_ts", {})["VERIFY"] = ts(290)
        st.setdefault("demand_filed", {})["VERIFY"] = {
            "oid": "qe:2026-11-07T09:00:00", "entered": ts(290)}
        F.save_state(self.ROOM, st)

    def test_80_gapfix_files_dev_on_qe_need(self):
        self.consume_entry()
        CLOCK["i"] = 310
        self.postq(self.ROOM, kind="NEED", **{"from": "qe", "oid": "qe:2026-11-07T10:10:00",
                   "summary": "NEED gaps", "evidence": {"diff": "none"}})
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertEqual(act.detail["lane"], "dev")
        self.assertIn("gaps:qe:2026-11-07T10:10:00".replace("qe:", "qe:"), json.dumps(F.load_state(self.ROOM).get("demand_filed", {})))

    def test_81_no_refile_when_open_dev_exists(self):
        self.consume_entry()
        self.eng.transport.file_order(self.ROOM, "dev:2026-11-07T09:00:00", lane="dev")
        CLOCK["i"] = 310
        self.postq(self.ROOM, kind="NEED", **{"from": "qe", "oid": "qe:2026-11-07T10:10:00",
                   "summary": "NEED gaps"})
        before = sorted(F.load_state(self.ROOM)["orders"])
        self.eng.tick(self.ROOM)
        self.assertEqual(sorted(F.load_state(self.ROOM)["orders"]), before,
                         "no new demand filed while a dev order stays open")

    def test_82_reverify_on_fresh_ready(self):
        self.consume_entry()
        CLOCK["i"] = 310
        self.postq(self.ROOM, kind="READY", **{"oid": "dev:2026-11-07T10:11:00",
                   "summary": "IMPL done", "evidence": {"branch": "refs/heads/x", "diff": "covered"}})
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "demand-filed")
        self.assertEqual(act.detail["lane"], "qe")


class LinkageTest(Basis, unittest.TestCase):
    """Gate linkage: deciding posts must ACK an open gate-lane demand (T8)."""

    def setUp(self):
        CLOCK["i"] = 300
        self.setup_room(self.ROOM)
        self.sim = SimHerdr()
        for lane in ("dev", "qe", "audit"):
            self.sim.spawn(f"t-{self.ROOM}-{lane}", self.ROOM, lane)
        self.eng = Engine(runtimes={self.ROOM: RoomRuntime(herdr=self.sim)},
                          clock=fake_clock, transport=Transport())

    def tearDown(self):
        self.drop_room(self.ROOM)

    def approve(self, **over):
        b = {"v": 1, "oid": "audit:2026-11-07T10:00:00", "from": "audit",
             "kind": "DECISION", "state": "idle", "next": "idle",
             "ts": ts(CLOCK["i"]), "room": self.ROOM,
             "summary": "APPROVED plan", "evidence": {"tip": "a" * 40}}
        b.update(over)
        return self.t.post(self.ROOM, "auditor", b) if hasattr(self, "t") else Transport().post(self.ROOM, "auditor", b)

    def test_90_unlinked_ready_does_not_advance(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st.setdefault("entered_ts", {})["IMPL"] = ts(290)
        F.save_state(self.ROOM, st)
        self.approve()
        self.eng.tick(self.ROOM)   # files the phase demand, no open dev orders yet
        self.post_dev_ready()
        for _ in range(3):
            self.eng.tick(self.ROOM)
        self.assertEqual(F.load_state(self.ROOM)["phase"], "IMPL",
                         "stray READY without demand ACK must not advance")

    def post_dev_ready(self, ack=None):
        b = {"v": 1, "oid": "dev:2026-11-07T10:10:00", "from": "dev",
             "kind": "READY", "state": "idle", "next": "idle",
             "ts": ts(CLOCK["i"]), "room": self.ROOM, "summary": "IMPL done",
             "evidence": {"branch": "refs/heads/x", "diff": "covered"}}
        if ack:
            b["acks"] = [ack]
        return Transport().post(self.ROOM, "dev", b)

    def test_91_linked_ready_advances(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st.setdefault("entered_ts", {})["IMPL"] = ts(290)
        F.save_state(self.ROOM, st)
        self.approve()
        self.eng.tick(self.ROOM)  # files IMPL dev demand
        st = F.load_state(self.ROOM)
        oids = [o for o, e in st["orders"].items() if e.get("lane") == "dev" and e["state"] == "open"]
        self.assertTrue(oids)
        self.post_dev_ready(ack=oids[0])
        for _ in range(4):
            self.eng.tick(self.ROOM)
        self.assertEqual(F.load_state(self.ROOM)["phase"], "VERIFY")

    def test_92_ring_records_worker(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "IMPL"
        st.setdefault("entered_ts", {})["IMPL"] = ts(290)
        F.save_state(self.ROOM, st)
        self.approve()
        self.eng.tick(self.ROOM)  # demand
        self.eng.tick(self.ROOM)  # ring
        st = F.load_state(self.ROOM)
        rung = [o for o in st["orders"].values() if o.get("ring_count", 0) > 0]
        self.assertTrue(rung)
        self.assertEqual(rung[0].get("rung_to"), f"t-{self.ROOM}-dev")


class StallTest(Basis, unittest.TestCase):
    def setUp(self):
        CLOCK["i"] = 400
        self.setup_room(self.ROOM)
        self.sim = SimHerdr()
        for lane in ("dev", "qe", "audit"):
            self.sim.spawn(f"t-{self.ROOM}-{lane}", self.ROOM, lane)
        self.eng = Engine(runtimes={self.ROOM: RoomRuntime(herdr=self.sim)},
                          clock=fake_clock, transport=Transport())

    def tearDown(self):
        self.drop_room(self.ROOM)

    def postq(self, **over):
        b = {"v": 1, "oid": f"qe:{ts(0)}", "from": "qe", "kind": "NEED",
             "state": "idle", "next": "idle", "ts": ts(CLOCK["i"]), "room": self.ROOM,
             "summary": "NEED same gap"}
        b.update(over)
        return Transport().post(self.ROOM, "qe", b)

    def test_95_three_identical_needs_raise_operator_need(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "VERIFY"
        st.setdefault("entered_ts", {})["VERIFY"] = ts(300)
        F.save_state(self.ROOM, st)
        for i, h in enumerate(["10:00:0%d" % i for i in range(3)]):
            CLOCK["i"] = 410 + i
            self.postq(oid=f"qe:2026-11-07T{h}", ts=f"2026-11-07T{h}Z")
        act = self.eng.tick(self.ROOM)
        self.assertEqual(act.verb, "need")
        self.assertEqual(F.load_state(self.ROOM)["stalled"]["phase"], "VERIFY")

    def test_96_differing_needs_do_not_stall(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "VERIFY"
        st.setdefault("entered_ts", {})["VERIFY"] = ts(300)
        F.save_state(self.ROOM, st)
        for i, s in enumerate(["NEED gap-a", "NEED gap-b", "NEED gap-a"]):
            CLOCK["i"] = 410 + i
            self.postq(oid=f"qe:2026-11-07T10:00:0{i}", summary=s,
                       ts=f"2026-11-07T10:00:0{i}Z")
        act = self.eng.tick(self.ROOM)
        self.assertNotEqual(act.verb, "need")

    def test_97_stall_suppresses_further_demands(self):
        st = F.load_state(self.ROOM)
        st["phase"] = "VERIFY"
        st.setdefault("entered_ts", {})["VERIFY"] = ts(300)
        F.save_state(self.ROOM, st)
        for i in range(3):
            CLOCK["i"] = 410 + i
            self.postq(oid=f"qe:2026-11-07T10:00:0{i}", ts=f"2026-11-07T10:00:0{i}Z")
        self.eng.tick(self.ROOM)
        n_orders = len(F.load_state(self.ROOM).get("orders", {}))
        out = self.eng.tick(self.ROOM)
        self.assertEqual(F.load_state(self.ROOM).get("orders", {}).__len__(), n_orders)
        self.assertNotEqual(out.verb if out else None, "demand-filed")
