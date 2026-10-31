#!/usr/bin/env python3
"""Deterministic multi-issue state machine & concurrency engine for NexIOS.

Replaces the coordinator role (now a read-only Consultant): all orchestration,
worker execution orders, workflow transitions, auditor lifecycle, and modal
mitigation run here, driven by watchdog ticks. Deterministic: identical room
inputs always produce identical decisions; every decision is logged and every
incoming worker post is ACKed with a recorded transition.

Reads:  room runtimes (/tmp/nexios-<issue>/), tmux panes (probes), audits/.
Writes: repo-bound logs only (<repo>/logs/state-machine/), /tmp room briefs,
        room state.json (fcntl-guarded), tmux doorbells, auditor launches.
Never:  kernel code, test files, worker checkouts, fence sources.

Usage:
  state_machine.py tick [--issues 51,52 | --all] [--interval S] [--once]
                        [--dry-run] [--log-dir DIR] [--repo DIR]
  state_machine.py status [--issues ...]   # read-only serial table, no mutation
"""
import argparse
import collections
import datetime
import fcntl
import glob
import json
import os
import re
import subprocess
import sys
import time

VERSION = "state-machine v1 2026-10-31 (consultant cutover engine)"

ROOMS_GLOB = "/tmp/nexios-*"
BOXES = ("dev", "qe", "auditor")
SESS = {"dev": "nexios-dev", "qe": "nexios-qe", "audit": "nexios-audit"}

# Modal signature copied from scripts/nexios-bin/worker-state.sh v3 MODAL_RE.
MODAL_RE = (r"allow once|allow always|permission required|needs approval|"
            r"always allow|enter confirm|reject permission|"
            r"tell opencode what to do differently|needs your decision|"
            r"awaiting approval")

POST_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}T\S*) FROM=(dev|qe|audit|auditor) "
    r"(READY|NEED|NOTE) :: (.*)$")
STATE_RE = re.compile(r"^STATE:\s*(.*)$")
NEXT_RE = re.compile(r"^NEXT:\s*(.*)$")

VALID_ROOM_STATES = ("IN_PROGRESS", "AUDITED", "VERIFIED", "PLANNED",
                     "QUEUED", "CLOSED", "HOLD")


def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def default_repo():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


class Ctx:
    """Run context: paths, flags, handles. No room state lives here."""

    def __init__(self, args):
        self.repo = os.path.abspath(args.repo or default_repo())
        self.agent_up = args.agent_up or os.path.join(
            self.repo, "scripts", "agent_up.sh")
        self.worker_state = os.path.join(
            self.repo, "scripts", "nexios-bin", "worker-state.sh")
        self.audits_dir = os.path.join(self.repo, "audits")
        self.log_dir = os.path.abspath(
            args.log_dir or os.path.join(self.repo, "logs", "state-machine"))
        self.dry = bool(args.dry_run)
        self.tick_seq = 0
        self.graph = collections.OrderedDict()  # issue -> room graph node
        self.log_fh = None

    def log_path(self):
        return os.path.join(self.log_dir, "state-machine.log")

    def graph_path(self):
        return os.path.join(self.log_dir, "state-graph.json")

    def open_log(self):
        if not self.dry:
            os.makedirs(self.log_dir, exist_ok=True)
        else:
            os.makedirs(self.log_dir, exist_ok=True)
        self.log_fh = open(self.log_path(), "a")

    def log(self, line):
        stamp = utcnow()
        tag = "DRY " if self.dry else ""
        self.log_fh.write("%s %s%s\n" % (stamp, tag, line))
        self.log_fh.flush()

    def close_log(self):
        if self.log_fh:
            self.log_fh.close()
            self.log_fh = None

    def run(self, argv, timeout=120):
        try:
            p = subprocess.run(argv, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout,
                               text=True)
            return p.returncode, p.stdout
        except FileNotFoundError:
            return 127, "missing binary: %s" % argv[0]
        except subprocess.TimeoutExpired:
            return 124, "timeout after %ss: %s" % (timeout, " ".join(argv))


def tmux(ctx, *args, timeout=15):
    return ctx.run(["tmux"] + list(args), timeout=timeout)


def read_lines(path, limit=None):
    try:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    return lines if limit is None else lines[-limit:]


def worker_posts(lines):
    return [l for l in lines if POST_RE.match(l)]


def last_state_next(tail_lines):
    st = nx = None
    for l in tail_lines:
        m = STATE_RE.match(l.strip())
        if m:
            st = m.group(1)[:120]
        m = NEXT_RE.match(l.strip())
        if m:
            nx = m.group(1)[:200]
    return st, nx


def load_state_locked(room_dir):
    """Read state.json under shared lock. Returns (state_dict_or_None)."""
    path = os.path.join(room_dir, "state.json")
    lockp = os.path.join(room_dir, "state.lock")
    try:
        lf = open(lockp, "a+")
        fcntl.flock(lf, fcntl.LOCK_SH)
        try:
            with open(path) as f:
                st = json.load(f)
        finally:
            fcntl.flock(lf, fcntl.LOCK_UN)
            lf.close()
        return st
    except (OSError, ValueError):
        return None


def write_state_locked(room_dir, mutate):
    """Apply mutate(state_dict) under exclusive lock + atomic replace."""
    path = os.path.join(room_dir, "state.json")
    lockp = os.path.join(room_dir, "state.lock")
    lf = open(lockp, "a+")
    fcntl.flock(lf, fcntl.LOCK_EX)
    try:
        try:
            with open(path) as f:
                st = json.load(f)
        except (OSError, ValueError):
            st = {}
        mutate(st)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(st, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        return True
    except OSError:
        return False
    finally:
        fcntl.flock(lf, fcntl.LOCK_UN)
        lf.close()


def discover_rooms(only=None):
    """Sorted [(issue:int, room_dir)] for rooms with a parseable state.json."""
    out = []
    for rdir in sorted(glob.glob(ROOMS_GLOB)):
        st = load_state_locked(rdir)
        if not st:
            continue
        try:
            issue = int(st.get("issue"))
        except (TypeError, ValueError):
            continue
        if only is not None and issue not in only:
            continue
        out.append((issue, rdir, st))
    return out


def load_boxes(room_dir):
    """Per-box tails: posts, last STATE/NEXT, full worker-authored list."""
    boxes = {}
    for box in BOXES:
        p = os.path.join(room_dir, "inbox", box + ".md")
        all_lines = read_lines(p)
        posts = worker_posts(all_lines)
        tail12 = all_lines[-12:]
        st, nx = last_state_next(tail12)
        coord = [l for l in tail12 if " FROM=coord NOTE" in l]
        boxes[box] = {"all": posts, "tail": tail12, "state": st,
                      "next": nx, "coord_last": coord[-1][:160] if coord else None}
    return boxes


def new_posts(boxes, handled):
    """Posts newer than handled cursors (ts compare; clocks skew, so max-ts)."""
    fresh = {}
    for box in BOXES:
        since = (handled or {}).get(box)
        if since is None:
            fresh[box] = []  # first sight: init cursor, report nothing
        else:
            fresh[box] = [l for l in boxes[box]["all"] if l[:20] > since[:20]][-5:]
    return fresh


def pane_capture(ctx, sess):
    rc, out = tmux(ctx, "capture-pane", "-p", "-t", sess)
    if rc != 0:
        return None
    return out


def modal_present(pane_text):
    return bool(pane_text) and bool(re.search(MODAL_RE, pane_text, re.I))


def fast_probe(ctx, sess):
    """Single-snapshot verdict: MODAL? | markers? | static? (heuristic)."""
    rc, _ = tmux(ctx, "has-session", "-t", sess)
    if rc != 0:
        return "ABSENT"
    pane = pane_capture(ctx, sess)
    if pane is None:
        return "ABSENT"
    if modal_present(pane):
        return "BLOCKED-modal"
    if "esc interrupt" in pane:
        return "WORKING-markers"
    return "IDLE-static"


def slow_probe(ctx, sess):
    """Authoritative verdict via scripts/nexios-bin/worker-state.sh (~20s)."""
    if not os.path.exists(ctx.worker_state):
        return "UNKNOWN-no-probe-script"
    rc, out = ctx.run(["bash", ctx.worker_state, sess], timeout=60)
    first = (out.strip().splitlines() or ["UNKNOWN-empty"])[0]
    for verdict in ("WORKING", "IDLE", "STUCK", "BLOCKED", "ABSENT"):
        if verdict in first:
            return verdict
    return "UNKNOWN-" + first[:60]


def modal_guard(ctx, sessions):
    """Deny-default: send Escape to any session showing a permission modal.

    Returns [(sess, acted_bool, detail)]. Dry-run only reports.
    """
    res = []
    for sess in sessions:
        pane = pane_capture(ctx, sess)
        if pane is None:
            res.append((sess, False, "absent"))
            continue
        if not modal_present(pane):
            res.append((sess, False, "no-modal"))
            continue
        if ctx.dry:
            res.append((sess, False, "WOULD-Escape(deny-default)"))
            continue
        rc, out = tmux(ctx, "send-keys", "-t", sess, "Escape")
        res.append((sess, rc == 0, "Escape-sent" if rc == 0 else "send-failed"))
    return res


def latest_audit_report(ctx, issue):
    """Newest audits/report-*.md mentioning the issue, or None."""
    cands = []
    try:
        names = sorted(os.listdir(ctx.audits_dir))
    except OSError:
        return None
    for n in names:
        if n.startswith("report-") and n.endswith(".md") and str(issue) in n:
            p = os.path.join(ctx.audits_dir, n)
            try:
                cands.append((os.path.getmtime(p), p))
            except OSError:
                pass
    return max(cands)[1] if cands else None


def audit_session_live(ctx):
    rc, _ = tmux(ctx, "has-session", "-t", SESS["audit"])
    return rc == 0


def auditor_decision(box_all):
    """Newest DECISION line in auditor box, or None."""
    for l in reversed(box_all):
        if "DECISION" in l:
            return l[:220]
    return None


def auditor_lifecycle(ctx, issue, room_dir, st, boxes):
    """Ensure an audit verdict exists when the lane waits on one.

    Returns (acted: [strings], need_verdict: bool). Never invents briefs:
    without prompts/auditor.txt it records NEED and stops.
    """
    acted = []
    pending = (st.get("pending") or "")
    wants = bool(re.search(r"audit", pending, re.I)) or st.get("phase") == "AUDIT"
    if not wants:
        return acted, False
    dec = auditor_decision(boxes["auditor"]["all"])
    if dec:
        return acted, False  # verdict on file; transition rows handle it
    brief = os.path.join(room_dir, "prompts", "auditor.txt")
    if not os.path.exists(brief):
        acted.append("NEED auditor brief missing: %s" % brief)
        return acted, True
    if audit_session_live(ctx):
        acted.append("auditor session live; awaiting verdict")
        return acted, True
    if ctx.dry:
        acted.append("WOULD launch auditor: agent_up.sh audit %s" % issue)
        return acted, True
    rc, out = ctx.run([ctx.agent_up, "audit", str(issue)], timeout=180)
    acted.append("auditor launched rc=%d %s" % (rc, out.strip().splitlines()[-1:] or ""))
    return acted, True


def dev_blocked_on_qe(dev_next):
    return bool(dev_next) and bool(
        re.search(r"waiting\b.*\bqe\b|waiting on.*qe|blocked.*qe", dev_next, re.I))


def pick_backlog(rooms_here, current_issue):
    """Lowest other non-halted, non-closed issue as backlog target."""
    for iss, _rdir, st in rooms_here:
        if iss == current_issue:
            continue
        if st.get("halt"):
            continue
        if (st.get("state") or "") in ("CLOSED", "HOLD"):
            continue
        return iss
    return None


def eval_armed(armed, events):
    """Fire armed triggers whose 'on' substring matches this tick's events.

    events: list of short strings e.g. 'dev READY', 'qe READY', 'UNCHANGED x3'.
    Returns [(arm, fired_bool)].
    """
    out = []
    blob = "\n".join(events)
    for arm in armed or []:
        on = arm.get("on", "")
        out.append((arm, bool(on) and on in blob))
    return out


def ring(ctx, sess, box, text):
    """Single-line inbox-pointer doorbell. Idle sessions only (caller checks)."""
    line = "inbox: %s -- read inbox/%s.md" % (text, box)
    if ctx.dry:
        return "WOULD ring %s: %s" % (sess, line)
    rc, out = ctx.run([ctx.agent_up, "msg", sess, line], timeout=60)
    if rc == 0:
        return "rang %s" % sess
    return "RING-FAILED %s: %s" % (sess, out.strip()[:120])


def write_brief(ctx, room_dir, name, content):
    p = os.path.join(room_dir, "prompts", name)
    if ctx.dry:
        return "WOULD write brief %s (%dB)" % (p, len(content))
    try:
        with open(p, "w") as f:
            f.write(content)
        return "wrote brief %s" % p
    except OSError as e:
        return "BRIEF-FAILED %s: %s" % (p, e)


def set_state_key(ctx, issue, key, value):
    if ctx.dry:
        return "WOULD state %s %s=%s" % (issue, key, value)
    rc, out = ctx.run([ctx.agent_up, "state", str(issue), "%s=%s" % (key, value)],
                      timeout=30)
    if rc == 0:
        return "state %s %s set" % (issue, key)
    return "STATE-FAILED %s %s: %s" % (issue, key, out.strip()[:120])


def parse_verdict(line):
    line = (line or "").strip()
    m = re.match(r"^(BASELINE|UNCHANGED|CHANGED)\s+(\S+)(.*)$", line)
    if not m:
        return {"kind": "UNKNOWN", "raw": line[:120]}
    kind, issue, rest = m.group(1), m.group(2), m.group(3)
    streak = 0
    ms = re.search(r"x(\d+)", rest)
    if ms:
        streak = int(ms.group(1))
    return {"kind": kind, "issue": issue, "rest": rest.strip(),
            "streak": streak, "escalate": "ESCALATE" in rest}


def tick_room(ctx, issue, room_dir, st, rooms_all):
    """One deterministic tick for one room. Returns graph node + table rows."""
    node = collections.OrderedDict()
    node["issue"] = issue
    node["tick"] = ctx.tick_seq
    actions = []
    events = []
    acked = []

    if st.get("halt"):
        node["halted"] = True
        node["actions"] = ["HALTED: no actions"]
        return node, []

    if ctx.dry:
        last = [l for l in read_lines(os.path.join(room_dir, "watch.log"))
                if "TICK" in l]
        _m = re.search(r"(BASELINE|UNCHANGED|CHANGED)\s+\S+.*", last[-1]) if last else None
        tailbit = _m.group(0) if _m else ""
        verdict = parse_verdict(tailbit)
        verdict["dry_no_tick_run"] = True
    else:
        rc, out = ctx.run([ctx.agent_up, "watch", str(issue), "tick"], timeout=60)
        verdict = parse_verdict((out.strip().splitlines() or [""])[0])
    node["verdict"] = verdict
    if verdict["kind"] == "UNCHANGED":
        events.append("UNCHANGED x%d" % verdict["streak"])
    elif verdict["kind"] == "CHANGED":
        events.append("CHANGED")

    boxes = load_boxes(room_dir)
    handled = st.get("handled") or {}
    fresh = new_posts(boxes, handled)
    for box in BOXES:
        for post in fresh[box]:
            m = POST_RE.match(post)
            kind = m.group(3) if m else "NOTE"
            acked.append({"box": box, "kind": kind,
                          "ts": post[:20], "summary": post[-160:]})
            events.append("%s %s" % (box, kind))
    node["new_posts"] = acked

    probes = {}
    for role in ("dev", "qe"):
        probes[role] = fast_probe(ctx, SESS[role])
    guard = modal_guard(ctx, [SESS["dev"], SESS["qe"], SESS["audit"]])
    for sess, acted, detail in guard:
        if acted or "WOULD" in detail:
            actions.append("modal-guard %s: %s" % (sess, detail))
    node["probes"] = probes

    a_acts, need_verdict = auditor_lifecycle(ctx, issue, room_dir, st, boxes)
    actions.extend(a_acts)

    dev_next = boxes["dev"]["next"]

    for role in ("dev", "qe"):
        b = boxes[role]
        if b["coord_last"] and (not b["all"] or
                                b["coord_last"][:19] > b["all"][-1][:19]):
            if probes[role].startswith("IDLE"):
                actions.append(ring(ctx, SESS[role], role,
                                    "unacked order -- %s" % b["coord_last"][:80]))
                events.append("ring-%s" % role)
            else:
                actions.append("%s WORKING reads at boundary; no ring" % role)

    if dev_blocked_on_qe(dev_next) and not probes["qe"].startswith("IDLE"):
        target = pick_backlog(rooms_all, issue)
        if target is None:
            actions.append("NEED backlog empty: dev waits on qe, nothing to swap to")
        else:
            brief = ("BACKLOG SWAP (#%d) -- dev blocked on qe output in #%d\n"
                     "issue: %d\nbranch: none yet (read-only until recalled)\n"
                     "Task: read-only recon of #%d per its room pending/next; "
                     "no branch, no code. Post NOTE + RING.\n"
                     % (target, issue, target, target))
            actions.append(write_brief(ctx, room_dir,
                                       "dev-%d-backlog.txt" % target, brief))
            if probes["dev"].startswith("IDLE"):
                actions.append(ring(ctx, SESS["dev"], "dev",
                                    "backlog #%d ready" % target))
            events.append("backlog-swap #%d" % target)

    for role in ("dev", "qe"):
        if probes[role].startswith("IDLE") and not any(
                a.startswith("rang " + SESS[role]) or
                a.startswith("WOULD ring " + SESS[role]) for a in actions):
            if not boxes[role]["next"] or boxes[role]["next"].startswith("idle"):
                actions.append("NEED %s idle with no briefed work (no invention)" % role)

    if verdict.get("escalate"):
        slow = {r: slow_probe(ctx, SESS[r]) for r in ("dev", "qe")}
        node["slow_probes"] = slow
        actions.append("ESCALATE #%d UNCHANGED x%d: slow=%s" %
                       (issue, verdict["streak"], slow))
        actions.append("NEED remedy: checkpoint NOTE, then /compact, "
                       "then recreate with resume brief (operator/harness)")

    for arm, fired in eval_armed(st.get("armed"), events):
        if not fired:
            continue
        do = arm.get("do", "")
        if do.startswith("ring "):
            parts = do.split()
            if len(parts) >= 3:
                actions.append(ring(ctx, parts[1], parts[2],
                                    "armed trigger [%s]" % arm.get("on", "")))
            else:
                actions.append("NEED malformed armed do: %s" % do)
        else:
            actions.append("ARMED [%s] fired: %s" % (arm.get("on", ""), do[:120]))
        events.append("armed-fired")

    dec = auditor_decision(boxes["auditor"]["all"])
    qe_ready = any("READY" in l for l in boxes["qe"]["all"][-3:])
    node["merge_ready"] = bool(dec and "APPROVED" in dec and qe_ready)

    def mutate(s):
        h = s.get("handled") or {}
        for box in BOXES:
            if boxes[box]["all"]:
                h[box] = max(x[:20] for x in boxes[box]["all"])
        s["handled"] = h
        s["updated"] = utcnow()
    if ctx.dry:
        actions.append("WOULD advance handled cursors")
    else:
        if not write_state_locked(room_dir, mutate):
            actions.append("STATE-WRITE-FAILED #%d" % issue)

    node["actions"] = actions
    node["acked"] = len(acked)
    ctx.graph[str(issue)] = node

    rows = []
    for role, sess in (("dev", SESS["dev"]), ("qe", SESS["qe"]),
                       ("audit", SESS["audit"])):
        if role == "audit":
            rep = latest_audit_report(ctx, issue)
            activity = ("report %s" % os.path.basename(rep)) if rep else (
                auditor_decision(boxes["auditor"]["all"]) or "no verdict")
            planned = "collect verdict" if need_verdict else "-"
            state = "LIVE" if audit_session_live(ctx) else "ABSENT"
        else:
            activity = boxes[role]["state"] or boxes[role]["next"] or "-"
            planned = next((a for a in actions
                            if role in a or SESS[role] in a), "-")
            state = probes[role]
        rows.append((role, state, activity, planned))
    return node, rows


def fmt_table(issue, verdict, rows):
    W = (8, 16, 42, 42)
    def cell(s, w):
        s = str(s).replace("\n", " ")
        return (s[:w - 1] + "\u2026") if len(s) > w else s.ljust(w)
    head = "#%s verdict=%s %s" % (issue, verdict.get("kind", "?"),
                                  verdict.get("rest", ""))
    bar = "+%s+%s+%s+%s+" % tuple("-" * (w + 2) for w in W)
    out = [head, bar,
           "| %s | %s | %s | %s |" % (
               cell("WORKER", W[0]), cell("STATE", W[1]),
               cell("ACTIVITY", W[2]), cell("PLANNED", W[3])), bar]
    for r in rows:
        out.append("| %s | %s | %s | %s |" % tuple(cell(c, w) for c, w in zip(r, W)))
    out.append(bar)
    return "\n".join(out)


def write_graph(ctx):
    if ctx.dry:
        return "DRY: graph not written"
    try:
        os.makedirs(ctx.log_dir, exist_ok=True)
        tmp = ctx.graph_path() + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"version": VERSION, "tick": ctx.tick_seq,
                       "rooms": ctx.graph}, f, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, ctx.graph_path())
        return "graph -> %s" % ctx.graph_path()
    except OSError as e:
        return "GRAPH-FAILED: %s" % e


def cmd_tick(ctx, issues, interval):
    rooms = discover_rooms(issues)
    if not rooms:
        print("no rooms%s" % (" for issues %s" % sorted(issues) if issues else ""))
        return 2
    rc = 0
    while True:
        ctx.tick_seq += 1
        ctx.log("tick #%d rooms=%s" % (ctx.tick_seq, [i for i, _, _ in rooms]))
        for issue, rdir, st in rooms:
            node, rows = tick_room(ctx, issue, rdir, st, rooms)
            table = fmt_table(issue, node.get("verdict", {}), rows)
            print(table, flush=True)
            ctx.log("issue #%d verdict=%s acked=%d merge_ready=%s" % (
                issue, node.get("verdict", {}).get("kind"),
                node.get("acked", 0), node.get("merge_ready")))
            for a in node.get("actions", []):
                ctx.log("#%d :: %s" % (issue, a))
        ctx.log(write_graph(ctx))
        if not interval:
            break
        time.sleep(interval)
        rooms = discover_rooms(issues)
    return rc


def cmd_status(ctx, issues):
    rooms = discover_rooms(issues)
    if not rooms:
        print("no rooms")
        return 2
    for issue, rdir, st in rooms:
        boxes = load_boxes(rdir)
        rows = []
        for role, sess in (("dev", SESS["dev"]), ("qe", SESS["qe"]),
                           ("audit", SESS["audit"])):
            if role == "audit":
                rep = latest_audit_report(ctx, issue)
                activity = ("report %s" % os.path.basename(rep)) if rep else (
                    auditor_decision(boxes["auditor"]["all"]) or "no verdict")
                state = "LIVE" if audit_session_live(ctx) else "ABSENT"
                rows.append((role, state, activity, "-"))
            else:
                probe = fast_probe(ctx, sess)
                activity = boxes[role]["state"] or boxes[role]["next"] or "-"
                rows.append((role, probe, activity, "-"))
        print(fmt_table(issue, {"kind": "STATUS", "rest": (st.get("pending") or "")[:60]},
                        rows), flush=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="NexIOS deterministic state machine")
    ap.add_argument("--repo", default=None)
    ap.add_argument("--agent-up", default=None)
    ap.add_argument("--log-dir", default=None)
    ap.add_argument("--dry-run", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tick")
    t.add_argument("--issues", default="")
    t.add_argument("--all", action="store_true")
    t.add_argument("--interval", type=int, default=0)
    s = sub.add_parser("status")
    s.add_argument("--issues", default="")
    s.add_argument("--all", action="store_true")
    for _p in (t, s):
        _p.add_argument("--repo", default=None)
        _p.add_argument("--agent-up", default=None)
        _p.add_argument("--log-dir", default=None)
        _p.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    issues = None
    if not a.all:
        issues = set(int(x) for x in a.issues.split(",") if x.strip()) or None
        if issues is None and not a.all:
            ap.error("pass --issues 51,52 or --all")
    ctx = Ctx(a)
    ctx.open_log()
    try:
        if a.cmd == "tick":
            return cmd_tick(ctx, issues, a.interval)
        return cmd_status(ctx, issues)
    finally:
        ctx.close_log()


if __name__ == "__main__":
    sys.exit(main())
