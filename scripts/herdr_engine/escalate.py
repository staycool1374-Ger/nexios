#!/usr/bin/env python3
"""Worker poke toolkit (T7, repurposed post-durable-queue) — issue #507.

Mechanical rungs an OPERATOR may invoke against a live worker: nudge →
re-ring → re-create (kill + rm the dead terminal). The ladder has NO
terminal rung anymore: waiting is a valid task state, not a violation,
so the engine never fires these automatically. `fire()` tries the rungs
top-down and returns a pending `wait` (never terminal, never mutating
room state) when no terminal can receive the poke.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

from herdr_engine.fsm import RoomRuntime, load_state, now_str
from herdr_engine.transport import oid_lane

LADDER = ("nudge", "re-ring", "re-create")


def lane_terminal(rt: RoomRuntime, room: int, lane: str | None,
                  exclude: set | frozenset = frozenset()) -> str | None:
    """Terminal lookup by get() — mirrors fsm.lane_terminal."""
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


@dataclass
class Rungs:
    """Concrete rungs; each returns (fired, terminal-name|None)."""

    herdr: Any = None
    dry: bool = False
    clock: Callable[[], str] | None = None

    def nudge(self, room, oid, rt: RoomRuntime, lane: str, note: str = "") -> tuple[bool, str | None]:
        name = lane_terminal(rt, room, lane)
        if name and self.herdr is not None:
            self.herdr.send(name, f"nudge {oid}: please ACK the engine ring" + (f" — {note}" if note else ""))
            return True, name
        return False, None

    def re_ring(self, room, oid, rt: RoomRuntime, lane: str, note: str = "") -> tuple[bool, str | None]:
        name = lane_terminal(rt, room, lane)
        if name and self.herdr is not None:
            self.herdr.send(name, f"re-ring {oid}" + (f" — {note}" if note else ""))
            return True, name
        return False, None

    def re_create(self, room, oid, rt: RoomRuntime, lane: str, note: str = "") -> tuple[bool, str | None]:
        name = lane_terminal(rt, room, lane)
        if name and self.herdr is not None:
            self.herdr.kill(name)
            rm = getattr(self.herdr, "rm", None)
            if rm is not None:
                try:
                    rm(name)
                except Exception:
                    pass
            return True, name
        return False, None


@dataclass
class Escalation:
    room: int
    oid: str
    action: str
    step: int
    ts: str
    terminal: bool = False
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class Escalator:
    """Manual poke driver. The engine never calls this on its own: there is
    no automatic path here from any tick. An operator invokes fire() to poke
    a live worker; when no terminal can receive it, the answer is an
    explicit pending wait — not silence, not a NEED, not a state change."""

    herdr: Any = None
    dry: bool = False
    clock: Callable[[], str] | None = None
    fired: list[Escalation] = field(default_factory=list)

    # -- rung selection ------------------------------------------------------

    @staticmethod
    def rung_for(count: int) -> tuple[str, int]:
        idx = min(max(count - 1, 0), len(LADDER) - 1)
        return LADDER[idx], idx

    def _diagnose(self, room: int, lane: str | None) -> str:
        """What post would open the room's gate, plus the lane's latest post."""
        try:
            from herdr_engine.fsm import Engine as _E, load_state, posts as _posts
            st = load_state(room)
            req = _E.gate_requirement(str(st.get("phase")))
            last = ""
            lane_posts = [p for p in _posts(room) if p.get("from") == lane]
            if lane_posts:
                lp = lane_posts[-1]
                last = f" | your last post {lp.get('oid')} ({lp.get('ts')}): {(lp.get('summary') or '')[:100]}"
            return f"to advance {st.get('phase')}: {req}{last}"
        except Exception:
            return ""

    def fire(self, room: int, oid: str, rt: RoomRuntime) -> Escalation:
        rung = Rungs(herdr=self.herdr, dry=self.dry, clock=self.clock)
        lane = oid_lane(oid) or "dev"
        note = self._diagnose(room, lane)
        for idx, act in enumerate(LADDER):
            ok, name = getattr(rung, act.replace("-", "_"))(room, oid, rt, lane, note)
            if ok:
                esc = Escalation(room=room, oid=oid, action=act, step=idx,
                                 ts=now_str(self.clock),
                                 payload={"sent": True, "lane": lane, "note": note})
                break
        else:
            # No terminal can receive the poke — explicit pending wait.
            # The task stays exactly where it is (WAITING); nothing about
            # the room changes, and no human is paged.
            esc = Escalation(room=room, oid=oid, action="wait", step=len(LADDER),
                             ts=now_str(self.clock),
                             payload={"sent": False, "lane": lane, "note": note})
        self.fired.append(esc)
        return esc
