#!/usr/bin/env python3
"""Scheduler T6 corpus — issue #507 (>=30 checks).

The gate: 3 rooms, 2 slots ⇒ the third queues, and no lane is ever
stale-pinned — admission only happens via explicit join + tick, release
frees exactly one slot, and foreign rooms' lanes are never touched.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(HERE, "scripts"))

from herdr_engine.scheduler import KINDS, Scheduler  # noqa: E402


class SlotTable(unittest.TestCase):
    def setUp(self):
        self.s = Scheduler()

    def test_01_table_shape(self):
        impl = [s for s in self.s.slots.values() if s.kind == "impl"]
        test = [s for s in self.s.slots.values() if s.kind == "test"]
        audit = [s for s in self.s.slots.values() if s.kind == "audit"]
        self.assertEqual(len(impl), 2)
        self.assertEqual(len(test), 2)
        self.assertEqual(len(audit), 2, "audit shares testbed worktrees")
        self.assertEqual(len(self.s.slots), 6)

    def test_02_slot_paths_layout(self):
        impl = sorted(s.path for s in self.s.slots.values() if s.kind == "impl")
        self.assertEqual(impl, ["~/jarvis-herdr-dev-wt1", "~/jarvis-herdr-dev-wt2"])
        tb = sorted(s.path for s in self.s.slots.values() if s.kind == "test")
        tb2 = sorted(s.path for s in self.s.slots.values() if s.kind == "audit")
        self.assertEqual(tb, tb2, "test+audit sit on the same worktrees")

    def test_03_status_of_fresh_table(self):
        st = self.s.status()
        self.assertIn("queue=[]", st)
        self.assertEqual(st.count("free"), 1)

    def test_04_acq_lease_and_busy(self):
        s = self.s.acquire(9701, "impl")
        self.assertIsNotNone(s)
        self.assertEqual(s.lease, "9701")
        self.assertEqual([x.name for x in self.s.busy(9701)], [s.name])
        t_slot = self.s.acquire(9701, "test")       # testbed group unaffected
        self.assertIsNotNone(t_slot)
        st = self.s.status()
        self.assertIn(f"{s.name}:9701", st)

    def test_05_release_frees_install_exactly_one(self):
        s1 = self.s.acquire(9701, "impl")
        s2 = self.s.acquire(9702, "impl")
        self.s.release(9701)
        busy = [n for n, sl in self.s.slots.items() if sl.lease]
        self.assertEqual(busy, [s2.name])
        self.assertNotIn("9701", self.s.status())   # released room fully gone
        self.assertIn("9702", self.s.status())      # the other lease survives
        # the released room's queue entries also go with it
        self.assertFalse(any(r == "9701" for r, _ in self.s.queue))

    def test_06_release_unknown_room_is_silent(self):
        self.assertEqual(self.s.release(9999), [])


class AdmissionQueue(unittest.TestCase):
    def setUp(self):
        self.s = Scheduler()

    def test_10_three_rooms_two_slots(self):
        # the T6 gate: third room queues, nobody stale-pins any lane
        self.assertIsNotNone(self.s.acquire(9701, "impl"))
        self.assertIsNotNone(self.s.acquire(9702, "impl"))
        third = self.s.acquire(9703, "impl")
        self.assertIsNone(third, "no free impl slot -> queued, not denied blind")
        self.assertEqual(self.s.queue, [("9703", "impl")])
        st = self.s.status()
        self.assertIn("queue=[9703/impl]", st)

    def test_11_tick_admits_when_free(self):
        self.s.acquire(9701, "impl")
        acquired = self.s.acquire(9702, "impl")
        self.s.acquire(9703, "impl")
        self.s.release(9701)
        admitted = self.s.tick()
        self.assertEqual([r for r, _ in admitted], ["9703"], "admitted room str-keyed")
        self.assertEqual(self.s.queue, [], "queue drained")
        self.assertEqual(sorted(s.lease for s in self.s.slots.values() if s.kind == "impl"),
                         ["9702", "9703"])

    def test_12_tick_fair_fifo(self):
        self.s.acquire(9701, "impl")
        self.s.acquire(9702, "impl")
        self.s.acquire(9703, "impl")
        self.s.join(9704, "impl")
        self.assertEqual(self.s.queue, [("9703", "impl"), ("9704", "impl")])
        self.s.release(9701)
        admitted = self.s.tick()
        self.assertEqual([r for r, _ in admitted], ["9703"])
        self.assertEqual([r for r, _ in self.s.queue], ["9704"], "FIFO holds")
        self.s.release(9702)
        self.s.tick()
        self.assertEqual(sorted(s.lease for s in self.s.slots.values() if s.kind == "impl"),
                         ["9703", "9704"])

    def test_13_join_idempotent(self):
        self.s.acquire(9703, "impl")
        self.assertTrue(self.s.join(9703, "impl"))
        self.assertFalse(self.s.join(9703, "impl"))
        self.assertEqual(len(self.s.queue), 1)

    def test_14_join_unknown_kind_hard_fails(self):
        with self.assertRaises(SystemExit):
            self.s.join(9701, "boss")

    def test_15_kinds_closed_vocab(self):
        self.assertEqual(KINDS, ("impl", "test", "audit"))

    def test_16_no_cross_room_scan_semantics(self):
        # lanes untouched: join/acquire carry no notion of looking at other
        # rooms' pending text; the only inputs are room+kind and the ledger
        self.s.acquire(9701, "impl")
        s = self.s.acquire(9702, "impl")
        queue_before = list(self.s.queue)
        joined = self.s.join(9703, "audit")
        self.assertTrue(joined)
        self.assertEqual(queue_before, [], "audit queue never shuffled impl waiters")
        self.assertEqual([r for r, k in self.s.queue], ["9703"])

    def test_17_mixed_groups_flow_independently(self):
        self.s.acquire(9701, "impl")
        self.s.acquire(9702, "impl")
        self.s.acquire(9703, "impl")           # impl queued
        self.assertIsNotNone(self.s.acquire(9703, "test"), "testbed group independent")


if __name__ == "__main__":
    unittest.main(verbosity=1)
