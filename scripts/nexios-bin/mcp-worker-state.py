#!/usr/bin/env python3
"""nexios-worker-state MCP server (stdio, stdlib only).
Tools: worker_state(session) -> WORKING|IDLE|STUCK verdict; worker_states() -> both.
Backed by worker-state.sh next to this file.
"""
import json, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SH = os.path.join(HERE, "worker-state.sh")
SESSIONS = ["nexios-dev", "nexios-qe"]


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
def _tool_post_update(a):
    errs = []
    room, box = str(a.get("room", "")), a.get("box", "")
    kind, summary = a.get("kind", ""), (a.get("summary", "") or "").strip()
    state, detail = a.get("state", ""), (a.get("state_detail", "") or "").strip()
    nxt, body = (a.get("next", "") or "").strip(), (a.get("body", "") or "").rstrip()
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
    if errs:
        return True, "REJECTED - fix and resend:\n- " + "\n- ".join(errs)
    role = {"dev": "dev", "qe": "qe", "auditor": "audit"}[box]
    ts = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    post = ts + " FROM=" + role + " " + kind + " :: " + summary + "\nSTATE: " + state + " :: " + detail + "\nNEXT: " + nxt + "\n" + body + "\n"
    with open(os.path.join(d, "inbox", box + ".md"), "a") as fh:
        fh.write(post)
    return False, "posted to " + d + "/inbox/" + box + ".md:\n" + post

def run_tool(name, args):
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
     "description": "Post a worker status update to a room inbox with ENFORCED wire format. Validates kind/state/next against the closed vocabulary and appends the 3-line post. Rejects anything else with the exact fix.",
     "inputSchema": {"type": "object", "properties": {
       "room": {"type": "string", "description": "numeric issue id"},
       "box": {"type": "string", "description": "dev, qe or auditor"},
       "kind": {"type": "string", "description": "READY, NEED or NOTE"},
       "summary": {"type": "string", "description": "one-line summary"},
       "state": {"type": "string", "description": "idle | working | waiting | blocked"},
       "state_detail": {"type": "string", "description": "what is true right now"},
       "next": {"type": "string", "description": "idle | working <what> | waiting <on-what> | blocked <on-what>"},
       "body": {"type": "string", "description": "evidence, numbers, file:lines (free text)"}},
       "required": ["room", "box", "kind", "summary", "state", "state_detail", "next"]}}
]

def respond(rid, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": rid}
    msg["result" if error is None else "error"] = result if error is None else {"code": -32603, "message": error}
    sys.stdout.write(json.dumps(msg) + "\n"); sys.stdout.flush()

def main():
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
            respond(rid, {"tools": TOOLS})
        elif method == "tools/call":
            is_err, text = run_tool(params.get("name", ""), params.get("arguments", {}) or {})
            respond(rid, {"content": [{"type": "text", "text": text}], "isError": is_err})
        else:
            respond(rid, error=f"unknown method '{method}'")

if __name__ == "__main__":
    main()
