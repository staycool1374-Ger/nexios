#!/usr/bin/env python3
"""nexios-worker-state MCP server (stdio, stdlib only).
Tools: worker_state(session) -> WORKING|IDLE|STUCK verdict; worker_states() -> both.
post_update accepts an optional ref (engine order id) and appends an
ACK: <ref> trailer line after NEXT. --selftest runs the built-in matrix.
Backed by worker-state.sh next to this file.

#507 T3 extension (herdr pilot): `post_update_v1` validates and posts a
wire-schema-v1 JSON body to /tmp/nherdr-<room> (typed rejections on
malformed input — never a write, never silence); `herdr_inbox_view`
renders JSON-lines views. Legacy tools keep operating on /tmp/nexios-<room>.
--selftest runs the legacy matrix; --selftest-v1 runs the herdr v1 matrix.
"""
import json, subprocess, sys, os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SH = os.path.join(HERE, "worker-state.sh")
SESSIONS = ["nexios-dev", "nexios-qe"]
sys.path.insert(0, os.path.join(HERE, os.pardir))
from herdr_engine.transport import NHR_BASE, Rejection, Transport as HerdrTransport  # noqa: E402


import glob as _glob

def _room_dir(room):
    if not str(room).isdigit():
        return None
    d = f"/tmp/nexios-{int(room)}"
    return d if os.path.isdir(d) else None

def _tool_inbox_read(a):
    d = _room_dir(a.get("room", ""))
    if not d:
        return True, f"no room /tmp/nexios-{a.get('room')}"
    box = a.get("box", "")
    if box not in ("dev", "qe", "auditor", "coord"):
        return True, "box must be dev|qe|auditor|coord"
    f = os.path.join(d, "inbox", box + ".md")
    if not os.path.isfile(f):
        return True, f"no inbox {f}"
    n = int(a.get("lines", 5) or 5)
    with open(f, errors="replace") as fh:
        lines = fh.read().splitlines()
    tail = lines[-n:] if len(lines) > n else lines
    return False, "\n".join(tail) or "(empty)"

def _tool_rooms_list(a):
    out = []
    for st in sorted(_glob.glob("/tmp/nexios-*/state.json")):
        try:
            d = json.load(open(st))
            out.append(f"{d.get('issue')}: phase={d.get('phase')} state={d.get('state')} pending={str(d.get('pending'))[:100]}")
        except Exception as e:
            out.append(f"{st}: unreadable ({e})")
    return False, "\n".join(out) or "(no rooms)"

def _tool_room_state(a):
    d = _room_dir(a.get("room", ""))
    if not d:
        return True, f"no room /tmp/nexios-{a.get('room')}"
    try:
        return False, json.dumps(json.load(open(os.path.join(d, "state.json"))), indent=1)
    except Exception as e:
        return True, f"unreadable: {e}"

def _tool_watch_status(a):
    d = _room_dir(a.get("room", ""))
    if not d:
        return True, f"no room /tmp/nexios-{a.get('room')}"
    p = subprocess.run(["bash", "/Users/arnold/jarvis/scripts/watchdog.sh", "status", str(int(a["room"]))],
                       capture_output=True, text=True, timeout=15)
    return False, (p.stdout.strip() or p.stderr.strip() or "(no output)")


import datetime as _dt
_VALID_KIND = ("READY", "NEED", "NOTE")
_VALID_BOX = ("dev", "qe", "auditor")
_VALID_STATE = ("idle", "working", "waiting", "blocked")
_TS19 = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}"
# Order-id grammar — keep byte-identical with ACK_OID_RE in scripts/state_machine.py.
_OID_RE = re.compile(r"^(?:(dev|qe):" + _TS19 + r"|next:(dev|qe):[0-9a-f]{8}|peer:(dev|qe):" + _TS19 + r"|first:(dev|qe))$")


def _ref_lane(ref):
    """Lane addressed by an order id, or None. fullmatch is mandatory ($ matches
    before a trailing newline; a ref with \\n must never inject a post line)."""
    m = _OID_RE.fullmatch(ref or "")
    if not m or m.lastindex is None:
        return None
    return m.group(m.lastindex)

_DEMAND_WORD_RE = re.compile(r"\b(DONE|DECISION|VERIFIED|NEED)\b")

def _post_is_demand_shaped(kind, summary, body):
    """True when a post should wake the engine on arrival. Pure.

    Mirror of the agent_up.sh mode_wake fast-path filter, enforced at the
    choke point every post flows through: NEED always fires; DECISION /
    DONE / VERIFIED / NEED in the one-line summary fire; READY fires only
    with IMPL content (delivery handoff). Pure ACK / status NOTEs stay
    silent for the 180 s heartbeat. A false positive costs one idempotent
    lock-skipped tick; a false negative costs a full interval."""
    upper_sum = (summary or "").upper()
    if kind == "NEED":
        return True
    if "DECISION" in upper_sum or _DEMAND_WORD_RE.search(upper_sum):
        return True
    if kind == "READY" and "IMPL" in (upper_sum + "\n" + (body or "").upper()):
        return True
    return False

def _fire_fast_tick(room, box, kind, summary, body, ts, room_dir):
    """Durable WAKE + detached one-shot tick for a demand-shaped post.

    Best-effort arrival trigger (message-driven engine): any failure here
    must never break the post itself, so everything is guarded and the
    caller ignores the return. The sweep lock serializes with the
    interval loop (skip-if-held), so overlap is safe by construction."""
    try:
        if not _post_is_demand_shaped(kind, summary, body):
            return False
        coord = os.path.join(room_dir, "inbox", "coord.md")
        if os.path.isfile(coord):
            with open(coord, "a") as fh:
                fh.write("%s FROM=%s WAKE :: inbox: %s %s -- read inbox/%s.md\n"
                         % (ts, box, box, kind, box))
        root = os.path.dirname(os.path.dirname(HERE))
        sm = os.path.join(root, "scripts", "state_machine.py")
        logd = os.path.join(root, "logs", "state-machine")
        if not (os.path.isfile(sm) and os.path.isdir(room_dir)):
            return False
        os.makedirs(logd, exist_ok=True)
        logf = open(os.path.join(logd, "engine-console.log"), "ab")
        try:
            subprocess.Popen(
                [sys.executable, sm, "tick", "--issues", str(int(room))],
                cwd=root, stdin=subprocess.DEVNULL,
                stdout=logf, stderr=subprocess.STDOUT,
                start_new_session=True)
        finally:
            logf.close()
        return True
    except Exception:
        return False
def _tool_post_update(a):
    errs = []
    room, box = str(a.get("room", "")), a.get("box", "")
    kind, summary = a.get("kind", ""), (a.get("summary", "") or "").strip()
    state, detail = a.get("state", ""), (a.get("state_detail", "") or "").strip()
    nxt, body = (a.get("next", "") or "").strip(), (a.get("body", "") or "").rstrip()
    ref = (a.get("ref") or "").strip()
    role = {"dev": "dev", "qe": "qe", "auditor": "audit"}.get(box, "")
    d = _room_dir(room)
    if not d:
        errs.append("no room /tmp/nexios-" + room)
    if box not in _VALID_BOX:
        errs.append("box must be dev|qe|auditor, got '" + box + "'")
    if kind not in _VALID_KIND:
        errs.append("kind must be READY|NEED|NOTE, got '" + kind + "'")
    if state not in _VALID_STATE:
        errs.append("state must be idle|working|waiting|blocked, got '" + state + "'")
    if not detail:
        errs.append("state_detail is required (what is true right now)")
    ok_next = nxt == "idle" or any(nxt == p or nxt.startswith(p + " ") for p in ("working", "waiting", "blocked"))
    if not ok_next:
        errs.append("next must be idle or working|waiting|blocked <what>")
    if not summary:
        errs.append("summary is required (one line)")
    if ref and box == "auditor":
        errs.append("ref is not allowed on box=auditor (verdict schema has no ACK trailer; omit ref)")
    elif ref and not _OID_RE.fullmatch(ref):
        errs.append("ref '" + ref + "' is not a valid order id; expected <dev|qe>:<YYYY-MM-DDTHH:MM:SS> | next:<lane>:<hash8> | peer:<lane>:<YYYY-MM-DDTHH:MM:SS> | first:<lane> (omit ref for an FYI post)")
    elif ref and _ref_lane(ref) != role:
        errs.append("ref '" + ref + "' targets lane '" + str(_ref_lane(ref)) + "' but box is '" + role + "'; ACK only orders addressed to your lane")
    if errs:
        return True, "REJECTED - fix and resend:\n- " + "\n".join(errs)
    ts = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    ack = ("ACK: " + ref + "\n") if ref else ""
    post = ts + " FROM=" + role + " " + kind + " :: " + summary + "\nSTATE: " + state + " :: " + detail + "\nNEXT: " + nxt + "\n" + ack + body + "\n"
    with open(os.path.join(d, "inbox", box + ".md"), "a") as fh:
        fh.write(post)
    trig = _fire_fast_tick(room, box, kind, summary, body, ts, d)
    tail = " [fast-path tick fired]" if trig else ""
    return False, "posted to " + d + "/inbox/" + box + ".md:\n" + post + tail

_HERDR_T = None

def _herdr_transport():
    global _HERDR_T
    if _HERDR_T is None:
        _HERDR_T = HerdrTransport()
    return _HERDR_T

_hd_boxes = ("dev", "qe", "auditor")
_hd_known = ("v", "oid", "from", "kind", "state", "next", "ts", "room",
             "ctx_pct", "acks", "evidence", "summary")

def _tool_post_update_v1(a):
    """Validated wire-schema-v1 post into /tmp/nherdr-<room> (never prose)."""
    body = a.get("post")
    if body is None:  # flat form: pull the v1 fields out of the args
        body = {k: a[k] for k in _hd_known if a.get(k) is not None or k in ("v",)}
        for k in ("v", "room"):
            if k in body and isinstance(body[k], str) and body[k].isdigit():
                body[k] = int(body[k])
    room = a.get("room", body.get("room") if isinstance(body, dict) else None)
    box = a.get("box", "dev")
    if isinstance(room, dict):
        return True, "REJECTED v1: room must be the room id"
    if isinstance(room, str):
        if not room.isdigit():
            return True, "REJECTED v1: room must be a numeric issue id"
        room = int(room)
    r = _herdr_transport().post(room, box, body)
    if isinstance(r, (Rejection,)):
        return True, r.render()
    return False, json.dumps({
        "posted": r.body, "acked": r.acked, "woke": r.woke, "stream": r.stream_path,
    })

def _tool_herdr_inbox_view(a):
    room = a.get("room", "")
    box = a.get("box", "dev")
    lines = int(a.get("lines", 5) or 5)
    if isinstance(room, int):
        room = str(room)
    return False, _herdr_transport().inbox_view(room, box, lines)

def run_tool(name, args):
    if name == "post_update_v1":
        return _tool_post_update_v1(args)
    if name == "herdr_inbox_view":
        return _tool_herdr_inbox_view(args)
    if name == "worker_state":
        sess = args.get("session", "")
        if sess not in SESSIONS + ["nexios-coord"]:
            return True, f"unknown session '{sess}' (want one of {SESSIONS})"
        p = subprocess.run(["bash", SH, sess], capture_output=True, text=True, timeout=30)
        return False, (p.stdout.strip() or p.stderr.strip() or "(no output)")
    if name == "inbox_read":
        return _tool_inbox_read(args)
    if name == "rooms_list":
        return _tool_rooms_list(args)
    if name == "room_state":
        return _tool_room_state(args)
    if name == "post_update":
        return _tool_post_update(args)
    if name == "watch_status":
        return _tool_watch_status(args)
    if name == "worker_states":
        outs = []
        for s in SESSIONS:
            p = subprocess.run(["bash", SH, s], capture_output=True, text=True, timeout=30)
            outs.append(p.stdout.strip() or f"{s} (no output)")
        return False, "\n".join(outs)
    return True, f"unknown tool '{name}'"

TOOLS = [
    {"name": "worker_state",
     "description": "WORKING | IDLE | STUCK verdict for a tmux worker session (pane turnover over 5s + run markers + children). Sessions: nexios-dev, nexios-qe.",
     "inputSchema": {"type": "object", "properties": {"session": {"type": "string", "description": "tmux session name"}}, "required": ["session"]}},
    {"name": "worker_states",
     "description": "worker_state for all worker sessions (nexios-dev, nexios-qe).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "inbox_read",
     "description": "Read the tail of a room inbox box (worker returns). room: numeric issue id. box: dev|qe|auditor|coord. lines: how many (default 5). Returns raw lines.",
     "inputSchema": {"type": "object", "properties": {"room": {"type": "string"}, "box": {"type": "string", "description": "dev, qe, auditor or coord"}, "lines": {"type": "integer"}}, "required": ["room", "box"]}},
    {"name": "rooms_list",
     "description": "List nexios rooms with issue, phase, state, pending (from each room state.json).",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "room_state",
     "description": "Read a room state.json (issue, phase, state, pending).",
     "inputSchema": {"type": "object", "properties": {"room": {"type": "string"}}, "required": ["room"]}},
    {"name": "watch_status",
     "description": "Watchdog arm state + unchanged streak for a room (armed pid / disarmed + streak).",
     "inputSchema": {"type": "object", "properties": {"room": {"type": "string"}}, "required": ["room"]}},
    {"name": "post_update",
     "description": "Post a worker status update to a room inbox with ENFORCED wire format. Validates kind/state/next against the closed vocabulary and appends the 3-line post. When ref is given, appends an ACK: <ref> trailer after NEXT (order ids: <dev|qe>:<ts19>, next:<lane>:<hash8>, peer:<lane>:<ts19>, first:<lane>). Omit ref for FYI posts. Rejects anything else with the exact fix.",
     "inputSchema": {"type": "object", "properties": {
       "room": {"type": "string", "description": "numeric issue id"},
       "box": {"type": "string", "description": "dev, qe or auditor"},
       "kind": {"type": "string", "description": "READY, NEED or NOTE"},
       "summary": {"type": "string", "description": "one-line summary"},
       "state": {"type": "string", "description": "idle | working | waiting | blocked"},
       "state_detail": {"type": "string", "description": "what is true right now"},
       "next": {"type": "string", "description": "idle | working <what> | waiting <on-what> | blocked <on-what>"},
       "ref": {"type": "string", "description": "optional order id this post acknowledges (copy it from the engine ring/NOTE [oid ...]); omit for FYI"},
       "body": {"type": "string", "description": "evidence, numbers, file:lines (free text)"}},
       "required": ["room", "box", "kind", "summary", "state", "state_detail", "next"]}}
]

TOOLS_V1 = [
    {"name": "post_update_v1",
     "description": "Post a wire-schema-v1 JSON body to a herdr pilot room (/tmp/nherdr-<room>): validates v/oid/from/kind/state/next/ts/room/ctx_pct/acks/evidence/summary against the closed schema; malformed posts get a typed rejection (field, error, hint) and never touch the stream. ACK semantics identical to the legacy bridge (one ACK clears one dev|qe order on the room ledger).",
     "inputSchema": {"type": "object", "properties": {
       "room": {"type": "integer", "description": "numeric issue id (herdr room)"},
       "box": {"type": "string", "description": "dev, qe or auditor"},
       "post": {"type": "object", "description": "the v1 JSON body; see docs/engine-herdr/spec.md 3"}},
       "required": ["room", "box"]}},
    {"name": "herdr_inbox_view",
     "description": "Render the tail of a herdr room's posts.jsonl as JSON lines, each {facing, body}; facing=true when the post is from or acks the requested lane box. Views are rendered by the transport, never parsed back.",
     "inputSchema": {"type": "object", "properties": {
       "room": {"type": "string", "description": "numeric issue id"},
       "box": {"type": "string", "description": "dev, qe or auditor"},
       "lines": {"type": "integer", "description": "how many (default 5)"}},
       "required": ["room", "box"]}},
]

def run_selftest_v1():
    """herdr v1 bridge matrix — posts + rejections + ledger + view via run_tool."""
    import shutil
    passed = 0
    total = 0

    def check(name, cond):
        nonlocal passed, total
        total += 1
        passed += 1 if cond else 0
        print("%s %s" % ("PASS" if cond else "FAIL", name), flush=True)

    room = 9991
    d = NHR_BASE + str(room)
    if os.path.exists(d):
        print("FAIL v1 selftest room exists (refusing to touch live room)", flush=True)
        return 1
    os.makedirs(os.path.join(d, "prompts"))
    os.makedirs(os.path.join(d, "reports"))
    json.dump({"v": 1, "issue": room, "phase": "RECON", "state": "IDLE",
               "pending": "", "orders": {}}, open(os.path.join(d, "state.json"), "w"))
    try:
        entry = _herdr_transport().file_order(room, "dev:2026-11-07T09:00:00",
                                              lane="dev", demand="plan")
        check("v1-order-filed", entry["state"] == "open")
        ok, out = run_tool("post_update_v1", {"room": room, "box": "dev", "post": {
            "v": 1, "oid": "dev:2026-11-07T10:00:00", "from": "dev", "kind": "NOTE",
            "state": "working", "next": "working plan",
            "ts": "2026-11-07T10:00:00Z", "room": room, "ctx_pct": 65,
            "evidence": {"branch": "refs/heads/engine-herdr", "tip": "a" * 40, "diff": "covered"},
            "summary": "checkpoint"}})
        check("v1-valid-post", not ok and json.loads(out)["posted"]["ctx_pct"] == 65)
        ok, out = run_tool("post_update_v1", {"room": room, "box": "dev", "post": {
            "v": 2, "oid": "dev:2026-11-07T10:00:01", "from": "dev", "kind": "NOTE",
            "state": "idle", "next": "idle", "ts": "2026-11-07T10:00:01Z", "room": room}})
        check("v1-reject-schema-version", ok and "v:" in out and "must be 1" in out)
        ok, out = run_tool("post_update_v1", {"room": room, "box": "dev", "post": {
            "v": 1, "oid": "banana", "from": "dev", "kind": "NOTE",
            "state": "idle", "next": "idle", "ts": "2026-11-07T10:00:02Z", "room": room}})
        check("v1-reject-oid", ok and "not a valid order id" in out)
        ok, out = run_tool("post_update_v1", {"room": room, "box": "qe", "post": {
            "v": 1, "oid": "qe:2026-11-07T10:00:03", "from": "qe", "kind": "READY",
            "state": "idle", "next": "idle", "ts": "2026-11-07T10:00:03Z", "room": room,
            "acks": ["dev:2026-11-07T09:00:00"]}})
        check("v1-reject-wrong-lane-ack", ok and "targets lane" in out)
        ok, out = run_tool("post_update_v1", {"room": room, "box": "dev", "post": {
            "v": 1, "oid": "dev:2026-11-07T10:00:04", "from": "dev", "kind": "NOTE",
            "state": "idle", "next": "idle", "ts": "2026-11-07T10:00:04Z", "room": room,
            "acks": ["dev:2026-11-07T09:00:00"]}})
        check("v1-ack-clears-own-lane", not ok and json.loads(out)["acked"] == 1)
        st = json.load(open(os.path.join(d, "state.json")))
        check("v1-ledger-acked", st["orders"]["dev:2026-11-07T09:00:00"]["state"] == "acked")
        check("v1-acks-log", bool(st.get("acks_log")))
        ok, out = run_tool("post_update_v1", {"room": room, "box": "dev", "post": {
            "v": 1, "oid": "dev:2026-11-07T10:00:05", "from": "dev", "kind": "NOTE",
            "state": "idle", "next": "idle", "ts": "2026-11-07T10:00:05Z", "room": room,
            "acks": ["dev:2026-11-07T09:00:00"]}})
        check("v1-reject-dup-ack", ok and "already acknowledged" in out)
        ok, out = run_tool("herdr_inbox_view", {"room": str(room), "box": "qe", "lines": 5})
        lines = [x for x in out.splitlines() if x.strip().startswith("{")]
        check("v1-view-non-empty", not ok and bool(lines))
        facing = [json.loads(x) for x in lines]
        check("v1-view-faces-others", all(not b["facing"] for b in facing))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("herdr v1 bridge self-test: %d/%d PASS" % (passed, total), flush=True)
    return 0 if passed == total else 1


def run_selftest():
    import shutil
    passed = 0
    total = 0

    def check(name, cond):
        nonlocal passed, total
        total += 1
        print("%s %s" % ("PASS" if cond else "FAIL", name), flush=True)
        if cond:
            passed += 1

    room = "9990"
    d = "/tmp/nexios-" + room
    if os.path.exists(d):
        print("FAIL selftest room exists (refusing to touch live room)", flush=True)
        return 1
    os.makedirs(os.path.join(d, "inbox"))
    open(os.path.join(d, "inbox", "dev.md"), "w").write("# inbox/dev.md\n")
    open(os.path.join(d, "inbox", "qe.md"), "w").write("# inbox/qe.md\n")
    try:
        base = {"room": room, "box": "dev", "kind": "NOTE",
                "summary": "s", "state": "idle", "state_detail": "d",
                "next": "idle"}
        err, _out = _tool_post_update(dict(base))
        check("ref-absent-legacy",
              not err and "ACK:" not in open(os.path.join(d, "inbox", "dev.md")).read().split("# inbox", 1)[-1])
        err, out = _tool_post_update(dict(base, ref="dev:2026-11-02T08:00:00"))
        check("ref-order",
              not err and "\nACK: dev:2026-11-02T08:00:00\n" in out)
        err, out = _tool_post_update(dict(base, box="qe", ref="next:qe:abcdef01"))
        check("ref-next", not err and "\nACK: next:qe:abcdef01\n" in out)
        err, out = _tool_post_update(dict(base, box="qe", ref="peer:qe:2026-11-02T08:00:00"))
        check("ref-peer", not err and "ACK: peer:qe:" in out)
        err, out = _tool_post_update(dict(base, ref="first:dev"))
        check("ref-first", not err and "\nACK: first:dev\n" in out)
        err, _out = _tool_post_update(dict(base, ref="banana"))
        check("ref-reject-garbage", err and "not a valid order id" in _out)
        err, _out = _tool_post_update(dict(base, ref="dev:2026-11-02"))
        check("ref-reject-short-ts", err and "not a valid order id" in _out)
        err, _out = _tool_post_update(dict(base, ref="dev:2026-11-02T08:00:00\nX"))
        check("ref-reject-newline", err and "not a valid order id" in _out)
        err, _out = _tool_post_update(dict(base, box="qe", ref="dev:2026-11-02T08:00:00"))
        check("ref-reject-lane-mismatch", err and "targets lane" in _out)
        err, _out = _tool_post_update(dict(base, box="auditor", ref="dev:2026-11-02T08:00:00"))
        check("ref-reject-auditor", err and "not allowed on box=auditor" in _out)
        err, _out = _tool_post_update(dict(base, ref="dev:2026-11-02T08:00:00Z"))
        check("ref-reject-ts20", err and "not a valid order id" in _out)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("bridge self-test: %d/%d PASS" % (passed, total), flush=True)
    return 0 if passed == total else 1


def respond(rid, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": rid}
    msg["result" if error is None else "error"] = result if error is None else {"code": -32603, "message": error}
    sys.stdout.write(json.dumps(msg) + "\n"); sys.stdout.flush()

def main():
    if "--selftest" in sys.argv:
        return run_selftest()
    if "--selftest-v1" in sys.argv:
        return run_selftest_v1()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            respond(None, error=f"parse: {e}"); continue
        rid, method, params = req.get("id"), req.get("method"), req.get("params", {}) or {}
        if method == "initialize":
            respond(rid, {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                          "serverInfo": {"name": "nexios-worker-state", "version": "1.0.0"}})
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            tools = list(TOOLS) + list(TOOLS_V1)
            respond(rid, {"tools": tools})
        elif method == "tools/call":
            is_err, text = run_tool(params.get("name", ""), params.get("arguments", {}) or {})
            respond(rid, {"content": [{"type": "text", "text": text}], "isError": is_err})
        else:
            respond(rid, error=f"unknown method '{method}'")

if __name__ == "__main__":
    sys.exit(main())
