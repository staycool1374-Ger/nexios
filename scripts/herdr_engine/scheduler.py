#!/usr/bin/env python3
"""Scheduler (T6) — slot table + admission queue (issue #507 §5.3).

Slot table: n_dev worktrees + n_testbed worktrees (start n=2 each).
Rooms acquire a slot per kind (impl/test/audit), leases are explicit,
release frees the slot, and the next queued room is admitted FIFO on the
next tick. No cross-room pending-text scans ever happen — an IDLE-denied
scam across rooms is exactly the class this engine is banned from (§2):
the scheduler only knows its own slot book and the explicit join calls.

status() exposes slots/free/busy/queue for `herdr_up.sh status`-style
horizontal surfaces (T8 wires them).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

from herdr_engine.fsm import now_str

KINDS = ("impl", "test", "audit")


@dataclass
class Slot:
    name: str
    kind: str           # impl -> dev worktree, test/audit -> testbed worktree
    path: str
    lease: str | None = None    # str(room) or None
    since: str | None = None


@dataclass
class Scheduler:
    clock: Callable[[], str] | None = None
    slots: dict[str, Slot] = field(default_factory=dict)
    queue: list[tuple[str, str]] = field(default_factory=list)  # (room, kind)

    def __post_init__(self) -> None:
        """Slot table: n=2 per group; kind decides access (impl=dev, test/audit=testbed)."""
        group_of = {"dev": ["impl"], "testbed": ["test", "audit"]}
        n = {"dev": 2, "testbed": 2}
        bases = {"dev": "~/jarvis-herdr-dev-wt{i}",
                 "testbed": "~/jarvis-herdr-testbed-wt{i}"}
        for g in group_of:
            for i in range(1, n[g] + 1):
                for kind in group_of[g]:
                    sname = f"{kind}-{i}"
                    if sname not in self.slots:
                        self.slots[sname] = Slot(name=sname, kind=kind,
                                                 path=bases[g].format(i=i))

    # -- admission ------------------------------------------------------------

    def group_for(self, kind: str) -> str:
        return {"impl": "dev", "test": "testbed", "audit": "testbed"}[kind]

    def join(self, room: int, kind: str) -> bool:
        """Admission queue join; idempotent; no scan of foreign rooms."""
        if kind not in KINDS:
            raise SystemExit(f"join: kind must be {'|'.join(KINDS)}, got {kind!r}")
        entry = (str(room), kind)
        if any(k == kind and r == entry[0] for r, k in self.queue):
            return False
        self.queue.append(entry)
        return True

    def _free_slot(self, kind: str) -> Slot | None:
        for slot in self.slots.values():
            if slot.kind == kind and slot.lease is None:
                return slot
        return None

    def acquire(self, room: int, kind: str) -> Slot | None:
        """"First try direct lease; else enqueue; tick() admits."""
        slot = self._free_slot(kind)
        if slot is None:
            return self.join(room, kind) and None
        self.lease(slot, room)
        return slot

    def lease(self, slot: Slot, room: int) -> Slot:
        slot.lease = str(room)
        slot.since = now_str(self.clock)
        return slot

    def release(self, room: int) -> list[str]:
        out = [n for n, s in self.slots.items() if s.lease == str(room)]
        for n in out:
            self.slots[n].lease = None
            self.slots[n].since = None
        self.queue = [(r, k) for (r, k) in self.queue if r != str(room)]
        return out

    def tick(self) -> list[tuple[str, Slot]]:
        """Admit queued rooms into free slots (FIFO)."""
        admitted = []
        still = []
        for room, kind in self.queue:
            slot = self._free_slot(kind)
            if slot is not None:
                self.lease(slot, room)
                admitted.append((room, slot))
            else:
                still.append((room, kind))
        self.queue = still
        return admitted

    def busy(self, room: int) -> list[Slot]:
        return [s for s in self.slots.values() if s.lease == str(room)]

    # -- observability ---------------------------------------------------------

    def status(self) -> str:
        free = [s.name for s in self.slots.values() if s.lease is None]
        busy = [f"{s.name}:{s.lease}" for s in self.slots.values() if s.lease is not None]
        q = [f"{r}/{k}" for r, k in self.queue]
        return "slots free=[" + " ".join(free) + "] busy=[" + " ".join(busy) + "] queue=[" + " ".join(q) + "]"
