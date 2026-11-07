#!/usr/bin/env python3
"""Lane FSM (T4) — phases/gates/rings/handover in fleet form (issue #507 §5.3).

Gates are decided ONLY from (a) transport posts (schema v1) with their
timestamps and (b) git-evidence shapes carried in evidence fields. Zero
prose parsing.

Orders live on a DURABLE queue: `open` (WAITING for a worker — no
deadline, no escalation, waits indefinitely) → `acked` (claimed/working;
a lane ACKed the order on the transport ledger) → `blocked` (a NEED from
the holding lane handed it back with a note; the worker is freed the
moment the claim clears and a different worker may re-ring it) →
`retired` (gate transition). There is no other terminal state: a task is
never parked, closed, or timed out. Rings fire at most once per cycle;
a missing worker means quiet waiting, not an event. CLOSED is
GitHub-derived — the engine never writes it.
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
# What each gate needs, in lane-readable words. Escalation rungs quote this
# so a stall tells the lane exactly what post would open the gate (T8 live:
# volume-only nudges never unstick a shape-mismatched lane).
GATE_REQUIREMENTS = {
    ("RECON", "IMPL"): "audit DECISION with summary APPROVED, evidence.tip matching your demand, ACKing a filed audit demand",
    ("IMPL", "VERIFY"): "dev READY stating IMPL, evidence {branch: refs/heads/<branch>, diff: covered}, ACKing an open dev demand",
    ("VERIFY", "CLOSE-OUT"): "qe READY stating DONE/verified, evidence {diff: covered}, ACKing an open qe demand",
    ("CLOSE-OUT", "CLOSED"): "GitHub-derived closure only (the engine never originates CLOSED)",
}
# Order states (durable queue — a task is never parked, closed, or timed
# out; waiting is a valid state, not a violation).
#   open    = WAITING for a worker (unclaimed; ring-eligible indefinitely)
#   acked   = claimed/working (lane ACKed the order on the ledger)
#   blocked = handed back by the holding lane's NEED (worker freed the moment
#             the claim cleared; carries the handover note; re-ringable by a
#             DIFFERENT worker)
#   retired = done (gate transition)
ACTIVE = ("open", "acked", "blocked")   # every non-terminal order state
WAITING = ("open", "blocked")           # ring-eligible: no live claim


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
    """Fleet wiring for one room: herdr handle (transport is file-backed)."""
    herdr: Any = None


class Engine:
    """Circle-of-rooms FSM driver. One tick = one action per room max."""

    # phase -> (working lane, demand text template). Filing happens once per
    # phase entry; it is the engine-side answer to "who files the first order".
    DEMANDS = {
        "RECON": ("dev", "RECON: read the room GitHub issue; scope the work; post a RECON NOTE with findings (evidence {branch, diff: pending})"),
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

    @staticmethod
    def gate_requirement(ph: str) -> str:
        to_ph = Engine.next_phase(ph)
        if to_ph is None:
            return "no onward gate"
        return GATE_REQUIREMENTS.get((ph, to_ph), "no onward gate")

    def find_gate(self, room, st: dict[str, Any]) -> tuple[str, str, dict[str, Any] | None] | None:
        """(from_phase, to_phase, deciding post) for the room's open gate.

        Output-in-boundary (T8): when the gate lane has open orders, the
        deciding post must ACK one of them — stray READYs never advance.
        With no open lane orders, the predicate alone decides (as before).

        Operator discharge (§13): a recorded operator discharge for exactly
        this (from → to) gate satisfies it with no post (deciding post is
        None). The discharge is recorded only through the explicit
        discharge API — nothing in the tick ever creates one.
        """
        ph = st.get("phase")
        to_ph = self.next_phase(ph)
        if ph not in PHASES[:-2] or to_ph is None:
            return None
        dis = (st.get("discharges") or {}).get(f"{ph}->{to_ph}")
        if dis and dis.get("by") == "operator":
            return ph, to_ph, None
        name = GATE_NAMES[(ph, to_ph)]
        entered = (st.get("entered_ts") or {}).get(ph, "")
        pred = GATE_PREDICATES[name]
        gate_lane = {"audit_approval": "audit", "dev_impl_ready": "dev",
                     "qe_verified": "qe"}.get(name)
        open_oids = {oid for oid, o in (st.get("orders") or {}).items()
                     if o.get("state") in WAITING and o.get("lane") == gate_lane}
        for p in reversed(posts_since(room, entered)):
            if pred(p):
                if open_oids and not (set(p.get("acks") or []) & open_oids):
                    continue  # unlinked READY: shape right, demand wrong
                return ph, to_ph, p
        return None

    # -- tick ----------------------------------------------------------------

    def tick(self, room: int) -> Action | None:
        if not os.path.exists(os.path.join(room_dir(room), "state.json")):
            return None  # room not set up yet; herdr_up.sh setup creates it
        with room_lock(room_dir(room)):
            return self._tick_locked(room)
    def _tick_locked(self, room: int) -> Action | None:
        st = load_state(room)
        if not st:
            return None
        rt = self.runtimes.get(room) or RoomRuntime()
        if st.get("phase") == "CLOSED":
            return self._record(Action(room, "ok", {"phase": "CLOSED"}))

        # 1) ring at most one waiting order this cycle (dead claims are
        # voided first; no worker means quiet waiting — never an event)
        ring = self._ring_tick(room, st, rt)
        if ring is not None:
            return self._record(ring)

        # 2) gates (a ready transition always wins over filing new work;
        # an operator discharge advances with no post, by recorded reason)
        gate = self.find_gate(room, st)
        if gate:
            frm, to, post = gate
            if post is None:
                dis = (st.get("discharges") or {}).get(f"{frm}->{to}", {})
                act = self._apply_transition(room, st, "", to,
                                             by=f"operator-discharge:{dis.get('reason', '')[:120]}")
            else:
                act = self._apply_transition(room, st, post.get("oid", ""), to)
            return self._record(act)

        # 3) handover: a lane NEED hands its claimed orders back (BLOCKED,
        # worker freed); waiting orders are never touched
        hand = self._handover_tick(room, st, rt)
        if hand is not None:
            return self._record(hand)

        # 4) stall watch: identical NEEDs x3 with no movement ends the
        # loop with a typed operator NEED instead of another lap
        stall = self._stall_tick(room, st)
        if stall is not None:
            return self._record(stall)

        # 5) phase-entry demand: file the working lane's first order so a
        # phase never waits on a human to originate work
        filed = self._demand_tick(room, st, rt)
        if filed is not None:
            return self._record(filed)

        # 6) follow-up demands: re-verify on fresh IMPL evidence, gap-fix on
        # qe NEEDs. Ledger + timestamps only, never prose (T8 live need).
        # Skipped entirely once the room is flagged stalled.
        if st.get("stalled"):
            return self._record(Action(room, "ok", {
                "phase": st.get("phase"),
                "open_orders": sorted(
                    oid for oid, o in (st.get("orders") or {}).items() if o.get("state") == "open"),
            }))
        red = self._redemand_tick(room, st, rt)
        if red is not None:
            return self._record(red)

        return self._record(Action(room, "ok", {
            "phase": st.get("phase"),
            "open_orders": sorted(
                oid for oid, o in (st.get("orders") or {}).items() if o.get("state") == "open"),
        }))

    def _record(self, act: Action | None):
        if act is not None:
            self.actions.append(act)
        return act

    def _apply_transition(self, room, st, oid, to, by=None):
        detail = {"to": to, "by": by if by is not None else oid}
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
        if st.get("stalled") or st.get("operator_needed"):
            return None  # stalled rooms file nothing; the operator owns next move
        ph = st.get("phase")
        if ph not in self.DEMANDS:
            return None
        lane, text = self.DEMANDS[ph]
        entered = (st.get("entered_ts") or {}).get(ph, "")
        for o in (st.get("orders") or {}).values():
            if o.get("state") in ACTIVE and o.get("lane") == lane:
                return None  # working lane already holds a live task
        filed = (st.get("demand_filed") or {}).get(ph)
        if filed and filed.get("entered") == entered:
            return None  # already filed for this phase entry; waiting on evidence
        ts = now_str(self.clock)
        oid = f"{lane}:{ts[:-1]}"
        detail = {"demand": oid, "lane": lane, "phase": ph}
        if self.dry_run or self.transport is None:
            detail["dry"] = True
            return Action(room, "demand-filed", detail)
        # No deadlines: a filed task waits on the queue until a worker
        # claims it, however long that takes.
        self.transport.file_order(room, oid, lane=lane, demand=text)
        # the filing above re-read and saved fresh state; our tick snapshot
        # is now stale, so refresh before marking (else we would clobber it)
        st.clear()
        st.update(load_state(room))
        st.setdefault("demand_filed", {})[ph] = {"oid": oid, "entered": entered}
        save_state(room, st)
        return Action(room, "demand-filed", detail)

    def _newest_post(self, room: int, frm: str | None = None,
                        kind: str | None = None) -> dict[str, Any] | None:
        cands = [p for p in posts(room)
                 if (frm is None or p.get("from") == frm)
                 and (kind is None or p.get("kind") == kind)]
        return max(cands, key=lambda p: p.get("ts", "")) if cands else None

    def _newest_demand_ts(self, st: dict[str, Any], lane: str) -> str:
        best = ""
        for oid in (st.get("orders") or {}):
            if oid_lane(oid) == lane:
                ts = oid.split(":", 1)[1] if ":" in oid else ""
                best = max(best, ts)
        for key, rec in (st.get("demand_filed") or {}).items():
            if isinstance(rec, dict) and str(rec.get("oid", "")).startswith(lane + ":"):
                best = max(best, str(rec.get("oid", ""))[len(lane) + 1:])
        return best

    def _file_followup(self, room, st, lane: str, text: str, key: str) -> Action:
        ts = now_str(self.clock)
        oid = f"{lane}:{ts[:-1]}"
        detail = {"demand": oid, "lane": lane, "phase": st.get("phase"), "key": key}
        if self.dry_run or self.transport is None:
            detail["dry"] = True
            return Action(room, "demand-filed", detail)
        self.transport.file_order(room, oid, lane=lane, demand=text)
        st.clear()
        st.update(load_state(room))
        st.setdefault("demand_filed", {})[key] = {"oid": oid, "entered": ts}
        save_state(room, st)
        return Action(room, "demand-filed", detail)

    def _redemand_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> Action | None:
        # Follow-up loop dev <-> audit: an audit NEED sends the room back to
        # dev for rework (plan rework in RECON, implementation rework later);
        # a fresh dev READY re-opens verification. The loop repeats until
        # audit APPROVED, then qe owns the gate. Timestamps + demand keys
        # only, never prose.
        if st.get("phase") not in ("RECON", "IMPL", "VERIFY"):
            return None
        filed = st.get("demand_filed", {})
        # Rule A — re-verify: fresh dev READY newer than the newest qe demand
        if st.get("phase") == "VERIFY":
            ready = self._newest_post(room, frm="dev", kind="READY")
            if ready and ready.get("ts", "") > self._newest_demand_ts(st, "qe"):
                if not any(o.get("state") == "open" and o.get("lane") == "qe"
                           for o in (st.get("orders") or {}).values()):
                    key = f"reverify:{ready.get('oid')}"
                    if key not in filed:
                        return self._file_followup(
                            room, st, "qe",
                            f"re-verify IMPL evidence {ready.get('oid')}; post READY DONE/verified or NEED with gaps",
                            key)
        # Rule B — gap-fix: latest post is a NEED (kind NEED, or a DECISION
        # whose summary starts with NEED per the verdict convention) and no
        # open dev demand. The NEED's own oid is the work order reference.
        latest = self._newest_post(room)
        if (latest and self._is_need(latest) and latest.get("from") in ("qe", "audit")
                and not any(o.get("state") == "open" and o.get("lane") == "dev"
                            for o in (st.get("orders") or {}).values())):
            key = f"gaps:{latest.get('oid')}"
            if key not in filed:
                return self._file_followup(
                    room, st, "dev",
                    f"address NEED {latest.get('oid')}; see stream; post READY with diff covered",
                    key)
        return None

    @staticmethod
    def _is_need(p: dict[str, Any]) -> bool:
        """A NEED is kind NEED, or a DECISION whose summary starts with NEED.

        Audit verdicts share kind=DECISION with approvals; the brief mandates
        the summary lead with the verdict word, so this is a wire-convention
        check, not prose parsing.
        """
        if p.get("kind") == "NEED":
            return True
        if p.get("kind") == "DECISION" and str(p.get("summary", "")).strip().upper().startswith("NEED"):
            return True
        return False

    @staticmethod
    def _canon(summary: str) -> str:
        """Structural normalization for stall comparison (not prose parsing):
        hex shas, timestamps, and oid-like tokens become placeholders, so a
        verdict that only re-stamps ids still matches itself."""
        import re
        s = re.sub(r"[0-9a-f]{7,64}", "#H", summary)
        s = re.sub(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?Z?", "#T", s)
        s = re.sub(r"\b(dev|qe|audit|next|peer|first):[^\s,;]+", "#O", s)
        s = re.sub(r"\s+", " ", s).strip()
        return s

    def _stall_tick(self, room, st: dict[str, Any]) -> Action | None:
        """Three NEEDs in a row, same lane, canonically identical summary,
        no phase movement since — the loop stopped converging."""
        if st.get("stalled") or st.get("operator_needed"):
            return None  # already surfaced; stay quiet, do not re-file
        tail = posts(room)[-3:]
        if len(tail) < 3:
            return None
        if not all(p.get("kind") == "NEED" for p in tail):
            return None
        if len({p.get("from") for p in tail}) != 1:
            return None
        if len({self._canon(p.get("summary") or "") for p in tail}) != 1:
            return None
        ev = {"room": room, "phase": st.get("phase"), "repeated": tail[-1].get("summary", "")[:160],
              "oids": [p.get("oid") for p in tail], "ts": now_str(self.clock)}
        if not self.dry_run:
            st["stalled"] = ev
            save_state(room, st)
        return Action(room, "need", {"operator": "operator", "why": "stall-loop",
                                     "evidence": ev, "dry": self.dry_run})

    # -- handover --------------------------------------------------------------

    def _handover_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> Action | None:
        """BLOCKED handover: a NEED from a lane hands that lane's claimed
        (acked) orders back to the queue with a note. The worker is freed
        the moment the claim clears — a freed worker may take other tasks,
        and the handed-back task waits for a DIFFERENT worker. Waiting
        (open) orders are never touched: only work a worker actually held
        can be handed back, and unclaimed work never blocks."""
        moved: list[str] = []
        for lane in ("dev", "qe", "audit"):
            needs = [p for p in posts(room)
                     if p.get("from") == lane and self._is_need(p)]
            if not needs:
                continue
            need = max(needs, key=lambda p: p.get("ts", ""))
            for oid, o in (st.get("orders") or {}).items():
                if o.get("lane") != lane or o.get("state") != "acked":
                    continue
                acked_ts = (o.get("acked_by") or {}).get("ts", "")
                if acked_ts and acked_ts >= (need.get("ts") or ""):
                    continue  # claimed after (or by) this NEED: not its handover
                o["state"] = "blocked"
                o["blocked_from"] = o.pop("rung_to", None)
                o["blocked_need"] = {"oid": need.get("oid"),
                                     "summary": (need.get("summary") or "")[:160],
                                     "ts": need.get("ts")}
                o["handover_count"] = o.get("handover_count", 0) + 1
                moved.append(oid)
        if not moved:
            return None
        detail: dict[str, Any] = {"handover": sorted(moved), "state": "blocked"}
        if self.dry_run:
            detail["dry"] = True
            return Action(room, "handover", detail)
        save_state(room, st)
        return Action(room, "handover", detail)

    # -- ring ----------------------------------------------------------------

    def _terminal_alive(self, rt: RoomRuntime, name: str | None) -> bool:
        if rt.herdr is None or not name:
            return False
        try:
            rt.herdr.get(name)
            return True
        except Exception:
            return False

    def _ring_tick(self, room, st: dict[str, Any], rt: RoomRuntime) -> Action | None:
        """Ring at most one waiting order per cycle. Ring-eligible = WAITING
        with no live claim. A dead claim (bound terminal gone) is voided —
        never counted, never escalated; the task simply waits again. A lane
        with no live terminal means quiet waiting: no action, no noise.
        Blocked orders exclude the terminal that handed them back."""
        orders = st.get("orders") or {}
        if not self.dry_run:
            voided = False
            for o in orders.values():
                if (o.get("state") in ("open", "blocked", "acked")
                        and o.get("rung_to")
                        and not self._terminal_alive(rt, o["rung_to"])):
                    o["rung_to"] = None
                    voided = True
            if voided:
                save_state(room, st)
        for oid, o in orders.items():
            if o.get("state") not in WAITING or o.get("rung_to"):
                continue  # claimed, already-rung, or done: leave it alone
            if self.dry_run:
                return Action(room, "ring", {"ring": oid, "to": o.get("lane"),
                                             "sent": False, "dry": True})
            excl = {o["blocked_from"]} if o.get("blocked_from") else set()
            name = self.lane_terminal(rt, room, o.get("lane"), exclude=excl)
            if name is None:
                continue  # no worker for this lane right now: wait quietly
            o["rung_to"] = name
            o["ring_count"] = o.get("ring_count", 0) + 1
            o["ringed_at"] = now_str(self.clock)
            save_state(room, st)
            sent = False
            if rt.herdr is not None:
                rt.herdr.send(name, f"ring {oid} {o.get('demand', '')}".strip())
                sent = True
            return Action(room, "ring", {"ring": oid, "to": o.get("lane"),
                                        "sent": sent, "dry": self.dry_run})
        return None

    @staticmethod
    def lane_terminal(rt: RoomRuntime, room: int, lane: str | None,
                      exclude: set | frozenset = frozenset()) -> str | None:
        # T8 live finding: `hc ls` payloads are a multiplex summary and carry
        # NO labels — only `hc get` does. Look the terminals up by name.
        if rt.herdr is None or not lane:
            return None
        try:
            names = [t.name for t in rt.herdr.ls()]
        except Exception:
            return None
        for name in names:
            if name in exclude:
                continue
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
            for gk, gv in sorted((st.get("discharges") or {}).items()):
                if isinstance(gv, dict) and gv.get("by") == "operator":
                    parts.append(f"discharged[{gk}]")
            try:
                from herdr_engine.transport import rejected_count
                nrej = rejected_count(room_dir(room))
                if nrej:
                    parts.append(f"rejected={nrej}")
            except Exception:
                pass
            for oid, o in sorted((st.get("orders") or {}).items()):
                line = f"{oid}: {o.get('state')} rings={o.get('ring_count', 0)}"
                if o.get("rung_to"):
                    line += f" rung_to={o['rung_to']}"
                if o.get("state") == "blocked":
                    need = o.get("blocked_need") or {}
                    line += (f" blocked_from={o.get('blocked_from')}"
                             f" need={need.get('oid')}")
                if o.get("operator_needed") or o.get("operator"):
                    # legacy flags (pre-durable-queue rooms); no new order
                    # ever carries these — waiting is not a violation
                    line += " LEGACY-NEED"
                parts.append(line)
            out.append(" ".join(parts))
        return "\n".join(out) or "no rooms in the herdr namespace"
