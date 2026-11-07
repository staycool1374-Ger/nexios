#!/usr/bin/env python3
"""Transport T3 corpus — issue #507 (schema v1, rejections, ACK ledger, views).

Target ≥60 checks incl. adversarial posts (missing / dup / mis-typed fields,
fake ACKs, stale CTX) and a round-trip through a sim room under /tmp/nherdr-*.
Stdlib unittest, shells-free, deterministic.
"""

import json
import os
import shutil
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))

from herdr_engine.transport import (  # noqa: E402
    NHR_BASE, OID_POST_RE, OID_RE, Posted, Rejection, Transport, is_demand_shaped,
    oid_lane, validate_post,
)

ROOM = 9002


# canonical: wire schema v1 keys 'from' / 'next'
def base(**over):
    d = {"v": 1, "oid": "dev:2026-11-07T10:00:00", "from": "dev",
         "kind": "NOTE", "state": "idle", "next": "idle",
         "ts": "2026-11-07T10:00:01Z", "room": ROOM}
    d.update(over)
    return d


class V(unittest.TestCase):
    """validate_post — each check is one assertion point."""

    def wv(self, body):
        r = validate_post(body)
        self.assertIsInstance(r, Rejection, f"expected rejection, got {r!r}")
        return r

    def wp(self, body):
        r = validate_post(body)
        if isinstance(r, Rejection):
            self.fail(f"unexpected rejection: {r.render()}")
        return r

    def f(self, rj, fld):
        self.assertTrue(any(v.field.startswith(fld) for v in rj.violations),
                        f"no violation on '{fld}': {rj.render()}")

    # -- happy path ---------------------------------------------------------
    def test_00_valid_minimal(self):
        post = self.wp(base())
        self.assertEqual(post["kind"], "NOTE")

    def test_01_valid_full_evidence(self):
        post = self.wp(base(evidence={
            "branch": "refs/heads/fix/461x", "tip": "a" * 40, "base": "main",
            "diff": "covered"}, ctx_pct=65,
            acks=["dev:2026-11-07T09:00:00"]))
        self.assertEqual(post["ctx_pct"], 65)

    def test_02_valid_all_oid_shapes(self):
        for o in ("dev:2026-11-07T10:00:00", "next:qe:abcdef01",
                  "peer:dev:2026-11-07T09:59:59", "first:qe"):
            self.wp(base(oid=o))

    # -- root / required ------------------------------------------------------
    def test_10_root_type(self):
        self.f(self.wv([1, 2]), "<root>")

    def test_11_missing_required_each(self):
        for k in ("v", "oid", "from", "kind", "state", "next"):
            b = base(); b.pop(k)
            self.f(self.wv(b), k)

    def test_12_implicit_required_empty(self):
        b = base(kind=None)
        self.f(self.wv(b), "kind")

    # -- mis-typed per field ---------------------------------------------------
    def test_20_v_wrong(self):
        self.f(self.wv(base(v=2)), "v")

    def test_21_oid_grammar(self):
        for o in ("banana", "dev:2026-11-02", "dev:2026-11-07T10:00:00Z",
                  "next:foo:abcdef01", "peer:dev:2026-11-07T09:59", "first:foo",
                  "dev:2026-11-07T10:00:00\nX", 12345):
            self.f(self.wv(base(oid=o)), "oid")

    def test_22_from_grammar(self):
        for f_ in ("", "boss", "auditor", 9):
            self.f(self.wv(base(**{"from": f_})), "from")

    def test_23_kind_grammar(self):
        for k in ("", "note", "TODO"):
            self.f(self.wv(base(kind=k)), "kind")

    def test_24_state_grammar(self):
        for s in ("", "WORKING", "blocking"):
            self.f(self.wv(base(state=s)), "state")

    def test_25_next_grammar(self):
        for n in ("", "IDLE", "working", "blocked  ", 7):
            self.f(self.wv(base(next=n)), "next")

    def test_26_next_prefixed_valid(self):
        for n in ("idle", "working impl", "waiting on qe", "blocked on engine"):
            self.wp(base(next=n))

    def test_27_ts_shape(self):
        for t in ("", "2026-11-07T10:00:01", "2026-11-07 10:00:01Z", "now"):
            self.f(self.wv(base(ts=t)), "ts")

    def test_28_room(self):
        for rm in ("507", None):
            self.f(self.wv(base(room=rm)), "room")

    def test_29_ctx_pct(self):
        for c in ("65", 65.5, -1, 101, True):
            self.f(self.wv(base(ctx_pct=c)), "ctx_pct")

    def test_2a_ctx_pct_valid_bounds(self):
        for c in (0, 65, 100):
            self.wp(base(ctx_pct=c))

    # -- evidence ---------------------------------------------------------------
    def test_30_evidence_type(self):
        self.f(self.wv(base(evidence=42)), "evidence")

    def test_31_branch_shape(self):
        self.f(self.wv(base(evidence={"branch": "fix/477"})), "evidence.branch")

    def test_32_tip_shape(self):
        self.f(self.wv(base(evidence={"tip": "GHIJKLMN"})), "evidence.tip")

    def test_33_diff_enum(self):
        self.f(self.wv(base(evidence={"diff": "covered-ish"})), "evidence.diff")

    def test_34_diff_valid(self):
        self.wp(base(evidence={"diff": "pending"}))

    # -- acks-------------------------------------------------------------------
    def test_40_acks_type(self):
        self.f(self.wv(base(acks="dev:2026-11-07T09:00:00")), "acks")

    def test_41_acks_elements(self):
        self.f(self.wv(base(acks=["banana"])), "acks[0]")

    def test_42_acks_target_lane(self):
        self.f(self.wv(base(acks=["first:audit"])), "acks[0]")

    def test_43_audit_token_tip_and_demand_ack_ok(self):
        b = base(**{"from": "audit", "oid": "audit:2026-11-07T10:00:00",
                    "evidence": {"tip": "pending-7b8b59a6", "branch": "refs/heads/engine-herdr"},
                    "acks": ["audit:2026-11-07T09:00:00"]})
        r = validate_post(b, role="audit")
        self.assertNotIsInstance(r, Rejection, getattr(r, "render", lambda: "")())

    def test_44_dev_token_tip_stays_rejected(self):
        r = validate_post(base(evidence={"tip": "pending-7b8b59a6"}), role="dev")
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("hex sha" in v.error for v in r.violations))

    def test_45_audit_garbage_tip_rejected(self):
        b = base(**{"from": "audit", "oid": "audit:2026-11-07T10:00:00",
                    "evidence": {"tip": "  x"}})
        self.assertIsInstance(validate_post(b, role="audit"), Rejection)

    def test_46_dev_cannot_ack_audit_demand(self):
        self.assertIsInstance(validate_post(base(acks=["audit:2026-11-07T09:00:00"]), role="dev"),
                              Rejection)

    # -- unknown fields ------------------------------------------------------------
    def test_50_unknown_field_rejected(self):
        self.f(self.wv(base(approved_by="engine")), "approved_by")

    def test_51_summary_type_checked(self):
        self.f(self.wv(base(summary=42)), "summary")

    # -- oid lake helpers --------------------------------------------------------
    def test_60_oid_lane(self):
        self.assertEqual(oid_lane("peer:qe:2026-11-07T09:00:00"), "qe")
        self.assertIsNone(oid_lane("nope"))

    def test_61_oid_regex_fullmatch_no_newline(self):
        self.assertIsNone(OID_RE.fullmatch("dev:2026-11-07T10:00:00\n"))

    def test_62_demand_shaped(self):
        self.assertTrue(is_demand_shaped("NEED", "anything", ""))
        self.assertTrue(is_demand_shaped("NOTE", "operator DECISION taken", ""))
        self.assertTrue(is_demand_shaped("READY", "impl handoff", ""))
        self.assertFalse(is_demand_shaped("READY", "status", ""))
        self.assertFalse(is_demand_shaped("NOTE", "checkpoint", "ctx 10%"))


class SimRoom(unittest.TestCase):
    """Round-trip through a sim room + ledger + views."""

    d: str
    t: Transport

    @classmethod
    def setUpClass(cls):
        cls.d = f"{NHR_BASE}{ROOM}"
        shutil.rmtree(cls.d, ignore_errors=True)
        os.makedirs(os.path.join(cls.d, "prompts"))
        os.makedirs(os.path.join(cls.d, "reports"))
        json.dump({"v": 1, "issue": ROOM, "phase": "RECON", "state": "IDLE",
                   "pending": "", "orders": {}}, open(os.path.join(cls.d, "state.json"), "w"))
        cls.t = Transport()
        cls.t.file_order(ROOM, "dev:2026-11-07T09:00:00", lane="dev", demand="plan")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.d, ignore_errors=True)

    def test_70_no_room_rejection(self):
        r = self.t.post(404404, "dev", base(room=404404))
        self.assertIsInstance(r, Rejection)
        self.assertEqual(r.kind, "no-room")

    def test_71_box_must_be_lane(self):
        r = self.t.post(ROOM, "supervisor", base())
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any(v.field == "box" for v in r.violations))

    def test_72_from_box_mismatch(self):
        r = self.t.post(ROOM, "qe", base())
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any(v.field == "from" for v in r.violations))

    def test_73_room_arg_mismatch(self):
        r = self.t.post(ROOM, "dev", base(room=ROOM + 1))
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any(v.field == "room" for v in r.violations))

    def test_74_post_write_and_view(self):
        p = self.t.post(ROOM, "dev", base(state="working", next="working plan"))
        self.assertIsInstance(p, Posted)
        self.assertEqual(p.acked, 0)
        raw = open(self.d + "/posts.jsonl").read()
        self.assertIn("2026-11-07T10:00:01Z", raw)
        view = self.t.inbox_view(ROOM, "dev", lines=5)
        self.assertIn('{"facing": true', view)
        self.assertIn("dev:2026-11-07T10:00:00", view)
        other = json.loads(view.splitlines()[-1])["body"]
        self.assertEqual(other["state"], "working")

    def test_75_stale_ts_rejected(self):
        r = self.t.post(ROOM, "dev", base(ts="2026-11-07T10:00:00Z"))
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("stale" in v.error for v in r.violations))

    def test_76_dup_oid_rejected(self):
        r = self.t.post(ROOM, "dev", base(ts="2026-11-07T10:00:02Z"))
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("duplicate" in v.error for v in r.violations))

    def test_77_fake_ack_rejected(self):
        r = self.t.post(ROOM, "dev", base(**{"oid": "dev:2026-11-07T10:00:03", "ts": "2026-11-07T10:00:03Z", "acks": ["dev:2026-11-07T00:00:00"]}))
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("unknown order id" in v.error for v in r.violations))
        st = json.load(open(self.d + "/state.json"))
        self.assertNotIn("acked", st["orders"].get("dev:2026-11-07T00:00:00", {"state": "none"}))

    def test_78_wrong_lane_ack_rejected(self):
        r = self.t.post(ROOM, "qe", base(**{"oid": "qe:2026-11-07T10:00:04", "from": "qe",
                                            "ts": "2026-11-07T10:00:04Z",
                                            "acks": ["dev:2026-11-07T09:00:00"]}))
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("targets lane" in v.error for v in r.violations))

    def test_79_ack_and_prune(self):
        b = base(**{"oid": "dev:2026-11-07T10:00:05", "ts": "2026-11-07T10:00:05Z",
                    "acks": ["dev:2026-11-07T09:00:00"]})
        p = self.t.post(ROOM, "dev", b)
        self.assertIsInstance(p, Posted)
        self.assertEqual(p.acked, 1)
        st = json.load(open(self.d + "/state.json"))
        self.assertEqual(st["orders"]["dev:2026-11-07T09:00:00"]["state"], "acked")
        self.assertEqual(st["orders"]["dev:2026-11-07T09:00:00"]["acked_by"]["lane"], "dev")
        self.assertTrue(any(a["oid"] == "dev:2026-11-07T09:00:00" for a in st["acks_log"]))
        # clearing twice → typed rejection
        r2 = self.t.post(ROOM, "dev", base(**{"oid": "dev:2026-11-07T10:00:06", "ts": "2026-11-07T10:00:06Z",
                                              "acks": ["dev:2026-11-07T09:00:00"]}))
        self.assertIsInstance(r2, Rejection)
        self.assertTrue(any("already acknowledged" in v.error for v in r2.violations))

    def test_7d_audit_demand_ack_clears(self):
        self.t.file_order(ROOM, "audit:2026-11-07T09:00:00", lane="audit")
        p = self.t.post(ROOM, "auditor", base(**{
            "from": "audit", "oid": "audit:2026-11-07T10:00:07",
            "ts": "2026-11-07T10:00:07Z", "kind": "DECISION", "summary": "APPROVED",
            "evidence": {"tip": "pending-abc123ef", "branch": "refs/heads/engine-herdr"},
            "acks": ["audit:2026-11-07T09:00:00"]}))
        self.assertIsInstance(p, Posted)
        st = json.load(open(self.d + "/state.json"))
        self.assertEqual(st["orders"]["audit:2026-11-07T09:00:00"]["state"], "acked")

    def test_7a_view_facing_acks(self):
        dev_view = self.t.inbox_view(ROOM, "dev", lines=5)
        dev_bodies = [json.loads(x) for x in dev_view.splitlines() if x.strip()]
        self.assertTrue(dev_bodies)
        self.assertTrue(all(b["facing"] for b in dev_bodies), "own-lane posts always face")
        qe_view = self.t.inbox_view(ROOM, "qe", lines=5)
        qe_bodies = [json.loads(x) for x in qe_view.splitlines() if x.strip()]
        self.assertTrue(any(not b["facing"] for b in qe_bodies) or not qe_bodies)

    def test_7b_wake_filter(self):
        fires = []
        t2 = Transport(waker=lambda room, box, kind, summary: fires.append((kind, summary)))
        p = t2.post(ROOM, "dev", base(**{"oid": "dev:2026-11-07T11:00:00",
                                         "ts": "2026-11-07T11:00:00Z",
                                         "kind": "NEED", "state": "blocked",
                                         "next": "blocked on engine"}))
        self.assertIsInstance(p, Posted)
        self.assertEqual(p.woke, True)
        self.assertEqual(fires[0][0], "NEED")
        p2 = t2.post(ROOM, "dev", base(**{"oid": "dev:2026-11-07T11:01:00",
                                          "ts": "2026-11-07T11:01:00Z",
                                          "kind": "NOTE", "summary": "checkpoint"}))
        self.assertIsInstance(p2, Posted)
        self.assertEqual(p2.woke, False)
        self.assertEqual(len(fires), 1)

    def test_7c_rejections_absolutely_do_not_touch_the_stream(self):
        before = open(self.d + "/posts.jsonl").read().count("\n")
        self.t.post(ROOM, "dev", base(kind="garbage"))
        after = open(self.d + "/posts.jsonl").read().count("\n")
        self.assertEqual(before, after)


class ReadStream(unittest.TestCase):
    """Validate-on-read: gates only ever see schema-clean posts (T8 live)."""

    d = f"{NHR_BASE}9004"

    def setUp(self):
        shutil.rmtree(self.d, ignore_errors=True)
        os.makedirs(self.d)
        json.dump({"v": 1, "issue": 9004, "phase": "RECON", "state": "IDLE",
                   "pending": "", "orders": {}}, open(os.path.join(self.d, "state.json"), "w"))

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def write(self, lines):
        with open(os.path.join(self.d, "posts.jsonl"), "w") as fh:
            for ln in lines:
                fh.write(ln + "\n")

    def good_dev(self):
        import copy
        b = base(room=9004)
        b["oid"] = "dev:2026-11-07T10:00:00"
        return json.dumps(b)

    def test_80_malformed_and_invalid_quarantined(self):
        from herdr_engine.transport import read_stream, rejected_count
        self.write([
            self.good_dev(),
            "{not json",
            json.dumps(dict(json.loads(self.good_dev()), kind="STAMP")),
            json.dumps({"v": 1, "oid": "audit:2026-11-07T10:00:01", "from": "audit",
                        "kind": "DECISION", "state": "idle", "next": "idle",
                        "ts": "2026-11-07T10:00:01Z", "room": 9004,
                        "evidence": {"tip": "pending-abc123ef"}}),
        ])
        valid, rejected = read_stream(self.d)
        self.assertEqual(len(valid), 2, "dev post + audit verdict survive")
        self.assertEqual(len(rejected), 2, "garbage + bad-kind quarantined")
        self.assertEqual(rejected_count(self.d), 2)
        self.assertTrue(all("oid" in v for v in valid))

    def test_81_fsm_posts_only_valid(self):
        from herdr_engine import fsm as F
        self.write(["{oops", self.good_dev()])
        got = F.posts(9004)
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0]["oid"], "dev:2026-11-07T10:00:00")


if __name__ == "__main__":
    unittest.main(verbosity=1)


class AckGuard(unittest.TestCase):
    def setUp(self):
        self.d = f"{NHR_BASE}9005"
        import shutil, os, json
        shutil.rmtree(self.d, ignore_errors=True)
        os.makedirs(self.d)
        json.dump({"v": 1, "issue": 9005, "phase": "IMPL", "state": "IDLE",
                   "pending": "", "orders": {}, "entered_ts": {"IMPL": "2026-11-07T09:00:00Z"}},
                  open(self.d + "/state.json", "w"))
        self.t = Transport()

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def postq(self, **over):
        b = base(room=9005)
        b.update(over)
        return self.t.post(9005, "dev", b)

    def test_90_ack_withdrawn_rejected(self):
        self.t.file_order(9005, "dev:2026-11-07T10:00:00", lane="dev")
        st = json.load(open(self.d + "/state.json"))
        st["orders"]["dev:2026-11-07T10:00:00"]["state"] = "withdrawn"
        json.dump(st, open(self.d + "/state.json", "w"))
        r = self.postq(oid="dev:2026-11-07T10:00:01", ts="2026-11-07T10:00:01Z",
                       acks=["dev:2026-11-07T10:00:00"])
        self.assertIsInstance(r, Rejection)
        self.assertTrue(any("not open" in v.error for v in r.violations))
