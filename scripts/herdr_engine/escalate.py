#!/usr/bin/env python3
"""Escalation ladder (T7) — SLAs terminate in typed operator NEEDs.

Ladder per violation count: nudge → re-ring → re-create → operator NEED
(owner + reason + evidence) as the terminal element. When a rung cannot
run (no herdr / no terminal), it does NOT end silently: the next rung
fires. The T4 SLA count is the ladder's driving counter and is carried
over; once a NEED is on file the order state becomes blocked_on_operator
and the engine takes no further actions until an operator decides.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

from herdr_engine.fsm import Engine as BaseEngine, RoomRuntime, load_state, now_str, save_state
from herdr_engine.transport import oid_lane

LADDER = ("nudge", "re-ring", "re-create", "operator_need")
SLA = {"ack_by": 900, "done_by": 1800}


def lane_terminal(rt: RoomRuntime, room: int, lane: str | None) -> str | None:
    """Terminal lookup by get() — mirrors fsm.lane_terminal.

    T8 live finding: `hc ls` payloads are a multiplex summary and carry NO
    labels; only `hc get` does.
    """
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


@dataclass
class Rungs:
    """Concrete rungs; each returns (fired, terminal-name|None)."""

    herdr: Any = None
    dry: bool = False
    clock: Callable[[], str] | None = None

    def nudge(self, room, oid, rt: RoomRuntime, lane: str) -> tuple[bool, str | None]:
        name = lane_terminal(rt, room, lane)
        if name and self.herdr is not None:
            self.herdr.send(name, f"nudge {oid}: please ACK the engine ring")
            return True, name
        return False, None

    def re_ring(self, room, oid, rt: RoomRuntime, lane: str) -> tuple[bool, str | None]:
        name = lane_terminal(rt, room, lane)
        if name and self.herdr is not None:
            self.herdr.send(name, f"re-ring {oid} — SLA {oid} breached")
            return True, name
        return False, None

    def re_create(self, room, oid, rt: RoomRuntime, lane: str) -> tuple[bool, str | None]:
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

    def operator_need(self, room, oid, bound: str, count: int) -> dict[str, Any]:
        return {
            "owner": "operator",
            "reason": f"order {oid} breached {bound} {count}x with the ladder exhausted",
            "evidence": {"room": room, "oid": oid, "bound": bound, "count": count,
                         "ts": now_str(self.clock)},
        }


@dataclass
class Escalation:
    room: int
    oid: str
    bound: str
    count: int
    ts: str
    action: str
    step: int
    terminal: bool = False
    payload: dict[str, Any] = field(default_factory=dict)


@dataclass
class Escalator:
    """Ladder driver; attach() wires it as the engine's on_overdue hook."""

    herdr: Any = None
    dry: bool = False
    clock: Callable[[], str] | None = None
    need_actions: list[dict[str, Any]] = field(default_factory=list)
    fired: list[Escalation] = field(default_factory=list)

    # -- rung selection ------------------------------------------------------

    @staticmethod
    def rung_for(count: int) -> tuple[str, int]:
        idx = min(max(count - 1, 0), len(LADDER) - 1)
        return LADDER[idx], idx

    def fire(self, room: int, oid: str, bound: str, count: int,
             rt: RoomRuntime) -> Escalation:
        idx = self.rung_for(count)[1]
        rung = Rungs(herdr=self.herdr, dry=self.dry, clock=self.clock)
        lane = oid_lane(oid) or "dev"
        while True:
            act = LADDER[idx]
            if act == "operator_need":
                need = rung.operator_need(room, oid, bound, count)
                esc = Escalation(room=room, oid=oid, bound=bound, count=count,
                                 ts=now_str(self.clock), action=act, step=idx,
                                 terminal=True, payload=need)
                break
            ok, name = getattr(rung, act.replace("-", "_"))(room, oid, rt, lane)
            if ok:
                esc = Escalation(room=room, oid=oid, bound=bound, count=count,
                                 ts=now_str(self.clock), action=act, step=idx,
                                 payload={"sent": True, "lane": lane})
                break
            idx += 1   # not silent: the broken rung escalates itself
        self.fired.append(esc)
        if esc.terminal:
            self.need_actions.append({"room": room, **esc.payload})
            st = load_state(room)
            if st:
                o = st.setdefault("orders", {}).setdefault(oid, {})
                o["operator"] = esc.payload
                o["state"] = "blocked_on_operator"
                if not self.dry:
                    save_state(room, st)
        return esc

    # -- engine wiring ---------------------------------------------------------

    def attach(self, engine: BaseEngine) -> None:
        def hook(oid: str, d: dict[str, Any]) -> None:
            room = d["room"]
            rt = engine.runtimes.get(room) or RoomRuntime()
            self.fire(room, oid, d["bound"], d["count"], rt)
        for rt in engine.runtimes.values():
            rt.on_overdue = hook
