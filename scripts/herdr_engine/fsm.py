#!/usr/bin/env python3
"""Lane FSM (T4) — phases/gates/rings/SLA in fleet form (issue #507 §5.3).

Gates are decided ONLY from (a) transport posts (schema v1) with their
timestamps and (b) git-evidence shapes carried in evidence fields. Zero
prose parsing. Orders are filed(oid, ack-by, done-by), ringed via hc once
per cycle max, and escalate through the T7 hook when SLAs blow; the halt
budget carries over. CLOSED is GitHub-derived — the engine never writes it.
"""

from __future__ import annotations

import datetime
import json
import os
import threading
from dataclasses import dataclass, field
from typing import Any, Callable

from herdr_engine.transport import NHR_BASE, oid_lane, room_lock

PHASES = ("RECON", "IMPL", "VERIFY", "CLOSE-OUT", "CLOSED")
GATE_NAMES = {
    ("RECON", "IMPL"): "audit_approval",
    ("IMPL", "VERIFY"): "dev_impl_ready",
    ("VERIFY", "CLOSE-OUT"): "qe_verified",
    ("CLOSE-OUT", "CLOSED"): "github_derived",
}
HALT_BUDGET_DEFAULT = 3


def now_str(clock: Callable[[], str] | None = None) -> str:
    if clock is not None:
        return clock()
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def room_dir(room: int | str) -> str:
    return f"{NHR_BASE}{int(room)}"


def load_state(room) -> dict[str, Any]:
    try:
        return json.load(open(os.path.join(room_dir(room), "state.json")))
    except (OSError, ValueError):
        return {}


def save_state(room, st: dict[str, Any]) -> None:
    p = os.path.join(room_dir(room), "state.json")
    tmp = p + f".tmp.{os.getpid()}"  # unique per writer (T8 live finding: engine tick and lane order-filing race on one tmp)
    with open(tmp, "w") as fh:
        json.dump(st, fh, indent=1)
        fh.write("\n")
    os.replace(tmp, p)


def posts(room) -> list[dict[str, Any]]:
    # validated read: only schema-clean posts ever reach gates or views
    from herdr_engine.transport import read_stream
    valid, _ = read_stream(room_dir(room))
    return valid


def posts_since(room, ts: str) -> list[dict[str, Any]]:
    return [p for p in posts(room) if (p.get("ts") or "") > (ts or "")]


# --- gate predicates (typed sources only) -----------------------------------

def audit_approval(p: dict[str, Any]) -> bool:
    """DECISION from audit with a tip in evidence and APPROVED in summary."""
    return (
        p.get("from") == "audit"
        and p.get("kind") == "DECISION"
        and "APPROVED" in str(p.get("summary", "")).upper()
        and bool((p.get("evidence") or {}).get("tip"))
    )


def dev_impl_ready(p: dict[str, Any]) -> bool:
    """READY from dev carrying IMPL handoff with covered diff evidence."""
    ev = p.get("evidence") or {}
    return (
        p.get("from") == "dev"
        and p.get("kind") == "READY"
        and "IMPL" in str(p.get("summary", "")).upper()
        and ev.get("diff") == "covered"
        and bool(ev.get("branch"))
    )


def qe_verified(p: dict[str, Any]) -> bool:
    """READY from qe stating verified/done with covered evidence."""
    ev = p.get("evidence") or {}
    return (
        p.get("from") == "qe"
        and p.get("kind") == "READY"
        and any(w in str(p.get("summary", "")).upper() for w in ("DONE", "VERIFIED", "VERIFY"))
        and ev.get("diff") == "covered"
    )


GATE_PREDICATES = {
    "audit_approval": audit_approval,
    "dev_impl_ready": dev_impl_ready,
    "qe_verified": qe_verified,
}


# --- actions ----------------------------------------------------------------

@dataclass
class Action:
    room: int
    verb: str
    detail: dict[str, Any] = field(default_factory=dict)

    def text(self) -> str:
        return f"room {self.room} {self.verb} {json.dumps(self.detail, sort_keys=True)}"


@dataclass
class RoomRuntime:
    """Fleet wiring for one room: herdr + hooks (transport is file-backed)."""
    herdr: Any = None
    on_overdue: Callable[[str, dict[str, Any]], None] | None = None
    halt_budget: int = HALT_BUDGET_DEFAULT


class Engine:
    """Circle-of-rooms FSM driver. One tick = one action per room max."""

    # phase -> (working lane, demand text template). Filing happens once per
    # phase entry; it is the engine-side answer to "who files the first order".
    DEMANDS = {
        "IMPL": ("dev", "IMPL: execute the approved plan; post READY with diff covered"),
        "VERIFY": ("qe", "VERIFY: check the IMPL evidence read-only; post READY DONE/verified or NEED with gaps"),
    }

    def __init__(
        self,
        runtimes: dict[int, RoomRuntime] | None = None,
        dry_run: bool = False,
        clock: Callable[[], str] | None = None,
        transport: Any = None,
    ):
        self.runtimes = runtimes or {}
        self.dry_run = dry_run
        self.clock = clock
        self.transport = transport
        self.wake = threading.Event()  # set by external arrival triggers
        self.actions: list[Action] = []

    # -- phase/gate logic ----------------------------------------------------

    @staticmethod
    def next_phase(ph: str) -> str | None:
        i = PHASES.index(ph) if ph in PHASES else None
        return PHASES[i + 1] if i is not None and i + 1 < len(PHASES) else None

    def find_gate(self, room, st: dict[str, Any]) -> tuple[str, str, dict[str, Any]] | None:
        """(from_phase, to_phase, deciding post) for the room's open gate."""
        ph = st.get("phase")
        to_ph = self.next_phase(ph)
        if ph not in PHASES[:-2] or to_ph is None:
            return None
        name = GATE_NAMES[(ph, to_ph)]
        entered = (st.get("entered_ts") or {}).get(ph, "")
        pred = GATE_PREDICATES[name]
        for p in reversed(posts_since(room, entered)):
            if pred(p):
                return ph, to_ph, p
        return None

    # -- tick ----------------------------------------------------------------

    def tick(self, room: int) -> Action | None:
        with room_lock(room_dir(room)):
            return self._tick_locked(room)
    def _tick_locked(self, room: int) -> Action | None:
        st = load_state(room)
        if not st:
            return None
        rt = self.runtimes.get(room) or RoomRuntime()
        if st.get("phase") == "CLOSED":
            return self._record(Action(room, "ok", {"phase": "CLOSED"}))

        # 1) SLA ladder (never ends silently; budget carries over)
        self._sla_tick(room, st, rt)

        # 1b) phase-entry demand: file the working lane's first order so a
        # phase never waits on a human to originate work
        filed = self._demand_tick(room, st, rt)
        if filed is not None:
            return self._record(filed)

        # 2) ring at most one open order this cycle
        ring = self._ring_tick(room, st, rt)
        if ring is not None:
            return self._record(ring)

        # 3) gates
        gate = self.find_gate(room, st)
        if gate:
            frm, to, post = gate
            act = self._apply_transition(room, st, post.get("oid", ""), to)
            return self._record(act)

        return self._record(Action(room, "ok", {
            "phase": st.get("phase"),
            "open_orders": sorted(
                oid for oid, o in (st.get("orders") or {}).items() if o.get("state") == "open"),
        }))

    def _record(self, act: Action | None):
        if act is not None:
            self.actions.append(act)
        return act

    def _apply_transition(self, room, st, oid, to):
        detail = {"to": to, "by": oid}
        if not self.dry_run:
            st["phase"] = to
            st.setdefault("entered_ts", {})[to] = now_str(self.clock)
            if oid:
                order = (st.get("orders") or {}).get(oid)
                if order is not None:
                    order["state"] = "retired"
                    st.setdefault("orders", {})[oid] = order
            save_state(room, st)
        else:
            detail["dry"] = True
        return Action(room, "advance", detail)

    # -- phase-entry demand ------------------------------------------------------

    def _demand_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> Action | None:
        ph = st.get("phase")
        if ph not in self.DEMANDS:
            return None
        lane, text = self.DEMANDS[ph]
        entered = (st.get("entered_ts") or {}).get(ph, "")
        for o in (st.get("orders") or {}).values():
            if o.get("state") == "open" and o.get("lane") == lane:
                return None  # working lane already has an open demand
        filed = (st.get("demand_filed") or {}).get(ph)
        if filed and filed.get("entered") == entered:
            return None  # already filed for this phase entry; waiting on evidence
        import datetime
        ts = now_str(self.clock)
        oid = f"{lane}:{ts[:-1]}"
        base = datetime.datetime.strptime(ts, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=datetime.timezone.utc)
        ack_by = (base + datetime.timedelta(seconds=900)).strftime("%Y-%m-%dT%H:%M:%SZ")
        done_by = (base + datetime.timedelta(seconds=1800)).strftime("%Y-%m-%dT%H:%M:%SZ")
        detail = {"demand": oid, "lane": lane, "phase": ph}
        if self.dry_run or self.transport is None:
            detail["dry"] = True
            return Action(room, "demand-filed", detail)
        self.transport.file_order(room, oid, lane=lane,
                                  ack_by=ack_by, done_by=done_by, demand=text)
        # the filing above re-read and saved fresh state; our tick snapshot
        # is now stale, so refresh before marking (else we would clobber it)
        st.clear()
        st.update(load_state(room))
        st.setdefault("demand_filed", {})[ph] = {"oid": oid, "entered": entered}
        save_state(room, st)
        return Action(room, "demand-filed", detail)

    # -- SLA -----------------------------------------------------------------

    def _sla_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> list[str]:
        now = now_str(self.clock)
        changed = False
        overdue = []
        for oid, o in (st.get("orders") or {}).items():
            # halt budget carries over: at budget, the ladder is terminal and
            # the T7 operator NEED owns the order, so stop climbing silently
            if o.get("state") != "open" or o.get("overdue_count", 0) >= rt.halt_budget:
                continue
            for bound in ("ack_by", "done_by"):
                limit = o.get(bound)
                if limit and now > limit:
                    o["overdue_count"] = o.get("overdue_count", 0) + 1
                    if o["overdue_count"] >= rt.halt_budget and not o.get("operator_needed"):
                        o["operator_needed"] = {"on": bound, "ts": now}
                    if rt.on_overdue:
                        rt.on_overdue(oid, {"room": room, "bound": bound,
                                            "count": o["overdue_count"]})
                    overdue.append(oid)
                    changed = True
                    break
        if changed:
            save_state(room, st)
        return overdue

    # -- ring ----------------------------------------------------------------

    def _ring_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> Action | None:
        for oid, o in (st.get("orders") or {}).items():
            if o.get("state") != "open" or o.get("ring_count", 0) >= 1:
                continue
            now = now_str(self.clock)
            o["ring_count"] = o.get("ring_count", 0) + 1
            o["ringed_at"] = now
            sent = False
            if not self.dry_run:
                save_state(room, st)
                name = self.lane_terminal(rt, room, o.get("lane"))
                if name and rt.herdr is not None:
                    rt.herdr.send(name, f"ring {oid} {o.get('demand', '')}".strip())
                    sent = True
            return Action(room, "ring", {"ring": oid, "to": o.get("lane"),
                                         "sent": sent, "dry": self.dry_run})
        return None

    @staticmethod
    def lane_terminal(rt: RoomRuntime, room: int, lane: str | None) -> str | None:
        # T8 live finding: `hc ls` payloads are a multiplex summary and carry
        # NO labels — only `hc get` does. Look the terminals up by name.
        if rt.herdr is None or not lane:
            return None
        try:
            names = [t.name for t in rt.herdr.ls()]
        except Exception:
            return None
        for name in names:
            try:
                full = rt.herdr.get(name)
            except Exception:
                continue
            labels = (full.raw or {}).get("labels") or {}
            if str(labels.get("room")) == str(room) and labels.get("lane") == lane:
                return name
        return None

    # -- fleet loop ----------------------------------------------------------

    def fleet(self) -> list[int]:
        rooms = []
        for d in sorted(os.listdir("/tmp")):
            if d.startswith("nherdr-") and d[len("nherdr-"):].isdigit():
                r = int(d[len("nherdr-"):])
                if r in self.runtimes:
                    rooms.append(r)
        return rooms

    def run_cycles(self, n: int = 1, wake_timeout: float = 0.05) -> list[Action]:
        all_acts: list[Action] = []
        for _ in range(n):
            self.wake.clear()
            for room in self.fleet():
                act = self.tick(room)
                if act is not None:
                    all_acts.append(act)
            if n > 1 and not self.wake.is_set():
                self.wake.wait(timeout=wake_timeout)
        return all_acts

    # -- status ----------------------------------------------------------------

    def status(self, rooms: list[int] | None = None) -> str:
        out = []
        for room in (rooms if rooms is not None else self.fleet()):
            st = load_state(room)
            if not st:
                out.append(f"room {room} (absent/unreadable)")
                continue
            parts = [f"room {room} phase={st.get('phase')}"]
            try:
                from herdr_engine.transport import rejected_count
                nrej = rejected_count(room_dir(room))
                if nrej:
                    parts.append(f"rejected={nrej}")
            except Exception:
                pass
            for oid, o in sorted((st.get("orders") or {}).items()):
                line = f"{oid}: {o.get('state')} rings={o.get('ring_count', 0)}"
                if o.get("operator_needed"):
                    line += f" NEED({o['operator_needed']['on']})"
                parts.append(line)
            out.append(" ".join(parts))
        return "\n".join(out) or "no rooms in the herdr namespace"
