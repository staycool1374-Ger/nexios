#!/usr/bin/env python3
"""Transport (T3) — wire schema v1, typed rejections, ACK ledger, inbox views.

Feeds the herdr engine (issue #507 §5.1/§5.2): workers call the MCP
`post_update_v1` tool with the validated JSON body; the transport keeps the
raw stream in `/tmp/nherdr-<room>/posts.jsonl`, renders JSON-lines inbox
*views* on demand (never parsed back), and maintains the ACK ledger inside
the room's `state.json`. Malformed input never writes — it returns a typed
`Rejection` (field, error, hint) for the lane.

Invariants preserved from the legacy bridge (scripts/nexios-bin/
mcp-worker-state.py): byte-identical order-id grammar with state_machine.py,
closed vocabularies, lane-targeted ACKs, and the pure demand-wake filter.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from typing import Any, Callable

def room_lock(room_dir: str):
    """Per-room exclusive lock shared by ALL engine writers (T8 live finding).

    Every read-modify-write on a room namespace (posts, orders, phases,
    auditor ledgers) must hold this from load to save; otherwise the async
    engine tick and a lane MCP post interleave and clobber state.json.

    Process-reentrant: nested acquisition by the same process (e.g. post()
    calling _save_state()) reuses the held file description instead of
    self-deadlocking on a second LOCK_EX.
    """
    import fcntl
    import threading

    if not hasattr(room_lock, "_depth"):
        room_lock._depth = {}  # type: ignore[attr-defined]
        room_lock._guard = threading.Lock()  # type: ignore[attr-defined]
        room_lock._handles = {}  # type: ignore[attr-defined]

    class _L:
        def __enter__(self_inner):
            with room_lock._guard:  # type: ignore[attr-defined]
                n = room_lock._depth.get(room_dir, 0)  # type: ignore[attr-defined]
                if n == 0:
                    lf = open(os.path.join(room_dir, "state.lock"), "w")
                    fcntl.flock(lf, fcntl.LOCK_EX)
                    room_lock._handles[room_dir] = lf  # type: ignore[attr-defined]
                room_lock._depth[room_dir] = n + 1  # type: ignore[attr-defined]
            return self_inner

        def __exit__(self_inner, *a):
            with room_lock._guard:  # type: ignore[attr-defined]
                n = room_lock._depth.get(room_dir, 1) - 1  # type: ignore[attr-defined]
                if n <= 0:
                    try:
                        lf = room_lock._handles.pop(room_dir)  # type: ignore[attr-defined]
                        fcntl.flock(lf, fcntl.LOCK_UN)
                        lf.close()
                    finally:
                        room_lock._depth.pop(room_dir, None)  # type: ignore[attr-defined]
                else:
                    room_lock._depth[room_dir] = n  # type: ignore[attr-defined]
    return _L()


TS19 = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}"
# Post-identity grammar: audit issues its own lane-scoped identity (verdict
# posts carry kind+tip; audit never owns orders), ACK targets stay dev|qe.
OID_POST_RE = re.compile(
    r"^(?:(dev|qe|audit):" + TS19 +
    r"|next:(dev|qe):[0-9a-f]{8}|peer:(dev|qe):" + TS19 + r"|first:(dev|qe))$"
)
# Order-id grammar — byte-identical with the legacy bridge / state_machine.py.
OID_RE = re.compile(
    r"^(?:(dev|qe):" + TS19 + r"|next:(dev|qe):[0-9a-f]{8}|peer:(dev|qe):" + TS19 +
    r"|first:(dev|qe))$"
)
KINDS = ("READY", "NEED", "NOTE", "DECISION")
STATES = ("idle", "working", "waiting", "blocked")
FROMS = ("dev", "qe", "audit")
BOX_ROLE = {"dev": "dev", "qe": "qe", "auditor": "audit"}
DIFFS = ("covered", "pending", "none")
TS_RE = re.compile(r"^" + TS19 + r"Z$")
DEMAND_WORD_RE = re.compile(r"\b(DONE|DECISION|VERIFIED|NEED)\b")
REQUIRED = ("v", "oid", "from", "kind", "state", "next")
NHR_BASE = os.environ.get("NHR_BASE", "/tmp/nherdr-")

_MISSING = object()


def read_stream(room_path: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Validate-on-read: every stream line re-checked against schema v1.

    T8 live finding: lanes can append to posts.jsonl by any path (MCP or
    shell), so write-time validation alone cannot guarantee the wire.
    Invalid lines are quarantined to posts.rejected.jsonl with their errors
    and NEVER returned to gates, verdict intake, or views.
    """
    valid: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []
    try:
        fh = open(os.path.join(room_path, "posts.jsonl"), errors="replace")
    except OSError:
        return valid, rejected
    with fh:
        for ln in fh:
            ln = ln.strip()
            if not ln:
                continue
            try:
                body = json.loads(ln)
            except ValueError as exc:
                rejected.append({"line": ln[:200], "errors": [f"not JSON: {exc}"]})
                continue
            role = body.get("from") if isinstance(body, dict) and body.get("from") in FROMS else None
            checked = validate_post(body, role=role)
            if isinstance(checked, Rejection):
                rejected.append({"line": ln[:200],
                                 "errors": [v.render() for v in checked.violations]})
                continue
            valid.append(checked)
    if rejected:
        with open(os.path.join(room_path, "posts.rejected.jsonl"), "a") as fh:
            for r in rejected:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    return valid, rejected


def rejected_count(room_path: str) -> int:
    try:
        with open(os.path.join(room_path, "posts.rejected.jsonl")) as fh:
            return sum(1 for _ in fh)
    except OSError:
        return 0


def oid_lane(oid: str) -> str | None:
    """Lane named by a post oid (audit included); ACK order ids stay dev|qe."""
    m = OID_POST_RE.fullmatch(oid or "")
    if not m:
        return None
    return m.group(m.lastindex)


def is_demand_shaped(kind: str, summary: str, body: str) -> bool:
    """Pure wake filter, verbatim-port of the legacy fast-path.

    NEED always; DEMAND words in summary; READY only with IMPL content.
    A false positive costs one idempotent tick; a false negative an interval.
    """
    upper_sum = (summary or "").upper()
    if kind == "NEED":
        return True
    if "DECISION" in upper_sum or DEMAND_WORD_RE.search(upper_sum):
        return True
    if kind == "READY" and "IMPL" in (upper_sum + "\n" + (body or "").upper()):
        return True
    return False


@dataclass
class Violation:
    field: str
    error: str
    hint: str = ""

    def render(self) -> str:
        return f"{self.field}: {self.error}" + (f" — hint: {self.hint}" if self.hint else "")


@dataclass
class Rejection:
    """Typed rejection event (schema class) — never silent, never speech."""

    kind: str = "schema"
    violations: list[Violation] = field(default_factory=list)

    def add(self, fld: str, error: str, hint: str = "") -> None:
        self.violations.append(Violation(fld, error, hint))

    def render(self) -> str:
        head = f"REJECTED [{self.kind}] — fix and resend:"
        return head + "\n- " + "\n- ".join(v.render() for v in self.violations)

    @property
    def is_rejection(self) -> bool:
        return True


@dataclass
class Posted:
    """Confirmed post: stream line, ledger effects, wake decision."""

    body: dict[str, Any]
    box: str
    stream_path: str
    acked: int = 0
    woke: bool = False

    @property
    def is_rejection(self) -> bool:
        return False


TIP_SHA = r"[0-9a-f]{8,64}"
# Audit verdict tips are demand-scoped tokens ("pending-<hash8>"), not git
# shas (T8 live finding: the hex-only rule made audit demands un-ACKable).
TIP_TOKEN = r"[A-Za-z][A-Za-z0-9._-]{0,63}"


def validate_post(body: Any, role: str | None = None) -> dict[str, Any] | Rejection:
    """Schema v1 — returns a canonical post dict or a typed Rejection.

    role (dev|qe|audit): lanes post audit verdicts with demand-scoped tip
    tokens and audit demand ACKs; every other lane keeps git-sha evidence.
    """
    rj = Rejection()

    def bad(fld: str, err: str, hint: str = "") -> None:
        rj.violations.append(Violation(fld, err, hint))

    if not isinstance(body, dict):
        bad("<root>", "post must be a JSON object", "see docs/engine-herdr/spec.md §3")
        return rj
    for req in REQUIRED:
        if req not in body or body[req] in (None, ""):
            bad(req, "missing required field")
    if rj.violations:
        return rj

    post: dict[str, Any] = {k: body[k] for k in body}

    if post["v"] != 1:
        bad("v", "must be 1", "wire schema v1")
    if not isinstance(post["oid"], str) or not OID_POST_RE.fullmatch(post["oid"]):
        lane = "<role>"
        bad("oid", "not a valid order id",
            f"{lane}:<YYYY-MM-DDTHH:MM:SS> | next:{lane}:<hash8> | peer:{lane}:<ts19> | first:{lane}")
    if not isinstance(post["from"], str) or post["from"] not in FROMS:
        bad("from", f"must be one of {'|'.join(FROMS)}")
    if post["kind"] not in KINDS:
        bad("kind", f"must be one of {'|'.join(KINDS)}")
    if post["state"] not in STATES:
        bad("state", f"must be one of {'|'.join(STATES)}")
    nxt = post["next"]
    ok_next = nxt == "idle" or any(
        isinstance(nxt, str) and re.fullmatch(p + r" \S.*", nxt) for p in ("working", "waiting", "blocked")
    )
    if not ok_next:
        bad("next", "must be idle or working|waiting|blocked <what>",
            'e.g. "idle" or "blocked on qe re-baseline"')

    ts = post.get("ts", _MISSING)
    if ts is _MISSING or not isinstance(ts, str) or not TS_RE.fullmatch(ts):
        bad("ts", "missing or not <YYYY-MM-DDTHH:MM:SS>Z")

    room = post.get("room", _MISSING)
    if room is _MISSING or not isinstance(room, int):
        bad("room", "missing or not an integer")
    elif not 0 < int(room) < 10 ** 6:
        bad("room", "out of plausibility range")

    ctx = post.get("ctx_pct", _MISSING)
    if ctx is not _MISSING:
        if not isinstance(ctx, int) or isinstance(ctx, bool) or not (0 <= ctx <= 100):
            bad("ctx_pct", "must be an integer 0..100")

    s = post.get("summary", _MISSING)
    if s is not _MISSING and not isinstance(s, str):
        bad("summary", "must be a string (one-liner; the wake filter reads it)")

    acks = post.get("acks", [])
    if not isinstance(acks, list):
        bad("acks", "must be a list of order ids")
    else:
        ack_re = OID_POST_RE if role == "audit" else OID_RE
        for i, a in enumerate(acks):
            if not isinstance(a, str) or not ack_re.fullmatch(a):
                bad(f"acks[{i}]", "not a valid order id")
            elif role != "audit" and oid_lane(a) not in ("dev", "qe"):
                bad(f"acks[{i}]", "order ids target dev|qe lanes only")

    ev = post.get("evidence", _MISSING)
    if ev is not _MISSING:
        if not isinstance(ev, dict):
            bad("evidence", "must be an object")
        elif ev:
            branch = ev.get("branch", _MISSING)
            if branch is not _MISSING and (
                not isinstance(branch, str) or not branch.startswith("refs/heads/") or " " in branch
            ):
                bad("evidence.branch", "must start with refs/heads/ and contain no spaces")
            tip = ev.get("tip", _MISSING)
            if tip is not _MISSING and not isinstance(tip, str):
                bad("evidence.tip", "must be a string")
            elif tip is not _MISSING and not re.fullmatch(TIP_SHA, tip):
                if role == "audit" and re.fullmatch(TIP_TOKEN, tip):
                    pass  # demand-scoped token identity (kind+tip+demand)
                else:
                    lane_tip = "hex sha (8..64)" + (" or demand token (audit only)" if role == "audit" else "")
                    bad("evidence.tip", f"must be a {lane_tip}")
            diff = ev.get("diff", _MISSING)
            if diff is not _MISSING and diff not in DIFFS:
                bad("evidence.diff", f"must be one of {'|'.join(DIFFS)}")

    known = set(REQUIRED) | {"ts", "room", "ctx_pct", "acks", "evidence", "summary"}
    for k in body:
        if k not in known:
            bad(k, "unknown field", f"wire schema v1 knows {sorted(known)}")
    if rj.violations:
        return rj

    return post


class Transport:
    """Room-scoped JSON-lines transport with an ACK ledger in state.json."""

    def __init__(self, waker: Callable[[str, str, str, str], None] | None = None):
        self.waker = waker

    # -- paths --------------------------------------------------------------

    @staticmethod
    def room_dir(room: int | str) -> str:
        return f"{NHR_BASE}{int(room)}"

    @classmethod
    def _have_room(cls, room: int | str) -> str | None:
        d = cls.room_dir(room)
        return d if os.path.isdir(d) else None

    # -- aggregate lazy in-memory state -------------------------------------

    @staticmethod
    def _load_state(d: str) -> dict[str, Any]:
        p = os.path.join(d, "state.json")
        if os.path.exists(p):
            try:
                return json.load(open(p, errors="replace"))
            except (OSError, ValueError):
                return {}
        return {}

    _locked = staticmethod(lambda d: room_lock(d))

    @staticmethod
    def _save_state(d: str, st: dict[str, Any]) -> None:
        p = os.path.join(d, "state.json")
        with Transport._locked(d):
            tmp = p + f".tmp.{os.getpid()}"
            with open(tmp, "w") as fh:
                json.dump(st, fh, indent=1)
                fh.write("\n")
            os.replace(tmp, p)

    @classmethod
    def rfile_order_state(cls, in_room, oid, lane=None, done_by=None,
                          ack_by=None, demand=""):
        """Locked order filing: the only room-state writer beside tick()."""
        d = cls._have_room(in_room)
        if not d:
            raise ValueError(f"no room {NHR_BASE}{in_room}")
        with cls._locked(d):
            st = cls._load_state(d)
            if "phase" not in st:
                # never seal a torn/empty read into the room ledger (T8 live finding)
                raise ValueError(f"refusing order filing on unreadable state {d}/state.json")
            orders = st.setdefault("orders", {})
            if oid in orders:
                return orders[oid]
            orders[oid] = {"lane": oid_lane(oid) or "audit", "state": "open",
                           "filed": "now", "ack_by": ack_by, "done_by": done_by,
                           "demand": demand}
            cls._save_state(d, st)
            return orders[oid]

    # -- posts --------------------------------------------------------------

    def post(self, room: int | str, box: str, body: Any) -> Posted | Rejection:
        d = self._have_room(room)
        rj = Rejection()
        if not d:
            rj.kind = "no-room"
            rj.violations.append(Violation("room", f"no room {NHR_BASE}{room}",
                                           "run scripts/herdr_up.sh setup <issue>"))
            return rj
        role = BOX_ROLE.get(box or "")
        if role is None:
            rj.violations.append(Violation("box", "must be dev|qe|auditor", "closed vocabulary"))
            return rj
        post = validate_post(body, role=role)
        if isinstance(post, Rejection):
            return post
        if post["from"] != role:
            rj.violations.append(Violation(
                "from", f"post from '{post['from']}' cannot ride box '{box}'",
                "each lane posts through its own box"))
            return rj
        if isinstance(post.get("room"), int) and post["room"] != int(room):
            rj.violations.append(Violation("room", f"body room {post['room']} != room arg {int(room)}"))
            return rj

        with self._locked(d):
            st = self._load_state(d)
            lanes = st.setdefault("lanes", {})
            lane = lanes.setdefault(role, {})
            # stale-ts adjudication: per-lane monotonic ts (t, not prose)
            new_ts = post["ts"]
            prev_ts = lane.get("last_ts")
            if prev_ts and new_ts <= prev_ts:
                rj.violations.append(Violation(
                    "ts", f"stale/out-of-order (lane last {prev_ts}, got {new_ts})",
                    "bump ts; timestamps only, no prose adjudication"))
                return rj
            # duplicate oid
            seen_oids = st.setdefault("seen_oids", [])
            if post["oid"] in seen_oids:
                rj.violations.append(Violation("oid", f"duplicate order id {post['oid']}"))
                return rj
    
            # ACK ledger: match/prune (fake ACKs get rejected, ledger untouched)
            acked = 0
            for a in post.get("acks") or []:
                orders = st.setdefault("orders", {})
                entry = orders.get(a)
                if entry is None:
                    rj.violations.append(Violation(f"acks[oid={a}]", "unknown order id on this room's ledger",
                                                   "copy the oid from the engine ring/NOTE"))
                    return rj
                if oid_lane(a) != role and role != "audit":
                    rj.violations.append(
                        Violation(f"acks[oid={a}]", f"targets lane '{oid_lane(a)}' but poster lane is '{role}'",
                                  "ACK only orders addressed to your lane"))
                    return rj
                if entry.get("state") == "acked":
                    rj.violations.append(Violation(f"acks[oid={a}]", "already acknowledged", "one ACK clears one oid"))
                    return rj
                entry["state"] = "acked"
                entry["acked_by"] = {"lane": role, "post_oid": post["oid"], "ts": new_ts}
                acked += 1
                st.setdefault("acks_log", []).append({"oid": a, "cleared_by": post["oid"], "ts": new_ts})
    
            stream = os.path.join(d, "posts.jsonl")
            line = json.dumps(post, ensure_ascii=False)
            with open(stream, "a") as fh:
                fh.write(line + "\n")
    
            lane["last_ts"] = new_ts
            lane["last_state"] = {"state": post["state"], "next": post["next"]}
            if post["oid"] not in seen_oids:
                seen_oids.append(post["oid"])
            self._save_state(d, st)

        woke = False
        if self.waker and is_demand_shaped(post["kind"], str(post.get("summary", "")), str(post.get("body", ""))):
            self.waker(str(room), box, post["kind"], str(post.get("summary", "")))
            woke = True
        return Posted(body=post, box=box, stream_path=stream, acked=acked, woke=woke)

    def file_order(self, room: int | str, oid: str, lane: str | None = None, done_by: str | None = None,
                   ack_by: str | None = None, demand: str = "") -> dict[str, Any]:
        """Engine-side: file an order on the ledger (ring source of truth)."""
        return self.rfile_order_state(int(room), oid, lane, done_by, ack_by, demand)

    # -- views --------------------------------------------------------------

    def inbox_view(self, room: int | str, box: str, lines: int = 5) -> str:
        """JSON-lines view for a lane box — rendered, never parsed back."""
        d = self._have_room(room)
        if not d:
            return json.dumps({"view": [], "error": f"no room {NHR_BASE}{room}"}) + "\n"
        role = BOX_ROLE.get(box or "", "")
        out = []
        try:
            raw = open(os.path.join(d, "posts.jsonl"), errors="replace").read().splitlines()
        except FileNotFoundError:
            raw = []
        for line in raw:
            try:
                post = json.loads(line)
            except ValueError:
                continue
            facing = (BOX_ROLE.get(post.get("from")) == role) or any(
                isinstance(a, str) and oid_lane(a) == role for a in post.get("acks", [])
            )
            out.append(json.dumps({"facing": facing, "body": post}, ensure_ascii=False))
        return "\n".join(out[-lines:] if len(out) > lines else out) + "\n"
