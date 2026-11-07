#!/usr/bin/env python3
"""Auditor lifecycle (T5) — instance per demand, identity-gated verdicts.

Per demand (tip + demand-oid + refs) ONE audited instance is spawned under
the fence (auditor role; `--agent`/`--auto` flags live in the role fence).
The verdict is only ever typed: an audit DECISION post with
evidence.tip == demand tip and acks carrying the demand oid — identity =
kind+tip+demand. Every verdict flows through the T3 transport; malformed or
mismatched verdicts become rejections, never prose.

Lifecycle ladder: spawn → work → verdict (teardown at once) |
idle×2 (teardown) | crash-exit (relaunch ≤3 → operator NEED | stop).
NEED verdict ⇒ the `re_audit` marker in room state (era re-open, §5.3);
the FSM consumes it at T6.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, field
from typing import Any, Callable

from herdr_engine.fsm import now_str, room_dir as rdir, load_state, save_state
from herdr_engine.transport import Rejection

BRIEF_TEMPLATE = """You are the audited examiner for room {room}.

Identity: examine tip {tip} against demand {demand}.
Refs to consult: {refs}.
Report ONLY through post_update_v1 (kind DECISION, evidence.tip={tip},
ACK the demand oid {demand}; summary APPROVED or NEED ...).
A malformed post returns a typed rejection; fix the field it names.
"""


def identity(room: int, tip: str, demand: str) -> str:
    """Instance key verdict identity: kind+tip+demand (hash8 like oids)."""
    h = hashlib.sha256(f"{room}|{tip}|{demand}".encode()).hexdigest()[:8]
    return f"aud-{room}-{h}"


@dataclass
class Instance:
    key: str
    room: int
    tip: str
    demand: str
    term: str | None = None
    state: str = "new"          # new|working|verdict|torn_down|operator_needed
    idle_cycles: int = 0
    relaunches: int = 0
    verdict: dict[str, Any] | None = None
    operator_needed: dict[str, Any] | None = None
    brief: str = ""

    def dump(self) -> dict[str, Any]:
        return {k: getattr(self, k) for k in
                ("key", "room", "tip", "demand", "term", "state", "idle_cycles",
                 "relaunches", "verdict", "operator_needed", "brief")}


@dataclass
class Audit:
    herdr: Any = None                    # hc shim or a sim facade
    transport: Any = None                # T3 transport (files the demand order)
    cwd: str = "."
    idle_limit: int = 2
    relaunch_budget: int = 3
    clock: Callable[[], str] | None = None
    operator_notifications: list[dict[str, Any]] = field(default_factory=list)

    # -- ledger persistence --------------------------------------------------

    def _instances_for(self, room: int) -> list[Instance]:
        return [i for i in self.ledger.values() if i.room == room]

    ledger: dict[str, Instance] = field(default_factory=dict)

    def save(self, room: int) -> None:
        st = load_state(room)
        if not st:
            return
        audits = st.setdefault("audits", {})
        for key, ins in self.ledger.items():
            if ins.room == room:
                audits[key] = ins.dump()
        save_state(room, st)

    def load(self, room: int) -> None:
        st = load_state(room)
        for key, raw in (st.get("audits") or {}).items():
            ins = self.ledger.get(key)
            if ins is None:
                ins = self.ledger[key] = Instance(key=key, room=raw["room"],
                                                  tip=raw["tip"], demand=raw["demand"])
            for k, v in raw.items():
                if hasattr(ins, k) and k != "key":
                    setattr(ins, k, v)

    # -- spawning --------------------------------------------------------------

    def spawn_demand(
        self,
        room: int,
        tip: str,
        demand: str = "",
        refs: list[str] | None = None,
        cwd: str | None = None,
        ack_by: str | None = None,
        done_by: str | None = None,
    ) -> Instance:
        key = identity(room, tip, demand)
        brief = BRIEF_TEMPLATE.format(
            room=room, tip=tip, demand=demand or "(FYI)",
            refs=", ".join(refs or []) or "(none)")
        ins = self.ledger.get(key) or self.ledger.setdefault(
            key, Instance(key=key, room=room, tip=tip, demand=demand))
        ins.brief = brief
        if ins.state in ("torn_down", "new") and ins.term is None:
            ins.term = key
            if self.herdr is not None:
                self.herdr.spawn(key, ["zsh"], cwd or self.cwd,
                                 task=brief.replace("\n", " ")[:180],
                                 labels={"room": str(room), "lane": "audit"})
            ins.state = "working"
            if self.transport is not None and demand:
                self.transport.file_order(room, demand, lane="audit",
                                          ack_by=ack_by, done_by=done_by,
                                          demand=f"audit insight {key}")
        self.save(room)
        return ins

    # -- verdict intake ---------------------------------------------------------

    def latest_posts(self, room: int) -> list[dict[str, Any]]:
        try:
            fh = open(os.path.join(rdir(room), "posts.jsonl"), errors="replace")
        except OSError:
            return []
        with fh:
            out = []
            for ln in fh:
                try:
                    out.append(json.loads(ln))
                except ValueError:
                    continue
        return out

    def accept_verdict(self, ins: Instance, post: dict[str, Any]) -> dict[str, Any] | Rejection:
        """Verdict identity: kind+tip+demand — reject anything else typed."""
        rj = Rejection()
        tip = (post.get("evidence") or {}).get("tip")
        if tip != ins.tip:
            rj.add("evidence.tip", f"verdict tip '{tip}' != demanded '{ins.tip}'",
                   "identity mismatch")
            return rj
        if ins.demand and ins.demand not in (post.get("acks") or []):
            rj.add("acks", f"verdict must ACK demand {ins.demand}",
                   "identity = kind+tip+demand")
            return rj
        ins.verdict = {"oid": post.get("oid"), "kind": post["kind"],
                       "summary": post.get("summary", ""), "ts": post.get("ts")}
        ins.state = "verdict"
        ins.idle_cycles = 0
        if "NEED" in str(post.get("summary", "")).upper():
            st = load_state(ins.room)
            st["re_audit"] = {"key": ins.key, "ts": post.get("ts"), "tip": ins.tip}
            save_state(ins.room, st)
        self.save(ins.room)
        return ins

    def scan_room(self, room: int) -> list[tuple[Instance, dict[str, Any] | None]]:
        """Newest audit DECISION per live instance; None = no fresh verdict."""
        posts = self.latest_posts(room)
        out = []
        for ins in self._instances_for(room):
            if ins.state in ("torn_down", "operator_needed"):
                continue
            mine = [p for p in posts
                    if str((p.get("evidence") or {}).get("tip")) == ins.tip
                    and p.get("kind") == "DECISION"]
            if not mine:
                out.append((ins, None))
                continue
            res = self.accept_verdict(ins, mine[-1])
            if isinstance(res, Rejection):
                out.append((ins, None))       # identity/shape rejected: keep waiting
                continue
            out.append((ins, res.verdict))
        return out

    # -- lifecycle loop ----------------------------------------------------------

    def tick(self, room: int) -> list[tuple[str, str, Any]]:
        """One lifecycle pass. Returns [(key, event, payload), ...]."""
        events: list[tuple[str, str, Any]] = []
        self.load(room)
        now = now_str(self.clock)
        for key, ins in list(self.ledger.items()):
            if ins.room != room or ins.state in ("torn_down", "operator_needed"):
                continue
            verdicts = {i.key: v for (i, v) in self.scan_room(room)}
            served = verdicts.get(key)
            if served is not None:
                events.append((key, "teardown", served))
                ins.state = "torn_down"
                if self.herdr is not None and ins.term:
                    self.herdr.kill(ins.term)
                self.save(room)
                continue

            # crash detection: terminal reporter flags the instance as absent
            if getattr(ins, "_term_state", "live") == "absent":
                ins.relaunches += 1
                if ins.relaunches > self.relaunch_budget:
                    ins.state = "operator_needed"
                    ins.operator_needed = {"why": "relaunch budget exhausted",
                                           "ts": now}
                    self.operator_notifications.append(ins.dump())
                    events.append((key, "operator_need", ins.operator_needed))
                    self.save(room)
                    continue
                events.append((key, "relaunch", ins.relaunches))
                if self.herdr is not None:
                    self.herdr.spawn(key, ["zsh"], self.cwd,
                                     task=ins.brief.replace("\n", " ")[:180],
                                     labels={"room": str(ins.room), "lane": "audit"})
                ins.state = "working"
                ins.idle_cycles = 0
                self.save(room)
                continue

            # idle ladder: nothing new this cycle
            ins.idle_cycles += 1
            if ins.idle_cycles >= self.idle_limit:
                ins.state = "torn_down"
                events.append((key, "teardown-idle", ins.idle_cycles))
                if self.herdr is not None and ins.term:
                    self.herdr.kill(ins.term)
                self.save(room)
                continue
            events.append((key, "idle", ins.idle_cycles))
            self.save(room)
        return events
