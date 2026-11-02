#!/usr/bin/env python3
"""Deterministic multi-issue state machine & concurrency engine for NexIOS.

Replaces the former coordinator role outright (the engine is the code-driven
coordinator; the consultant is a publishing advisor, never an orchestrator):
all orchestration, worker execution orders, workflow transitions, auditor
lifecycle, and modal mitigation run here, driven by watchdog ticks.
Deterministic: identical room
inputs always produce identical decisions; every decision is logged and every
incoming worker post is ACKed with a recorded transition. Worker posts may
carry an optional `ACK: <oid>` 4th trailer line; the engine clears exactly
that ledger oid (explicit acknowledgment of an engine order).

Reads:  room runtimes (/tmp/nexios-<issue>/), tmux panes (probes), audits/.
Writes: repo-bound logs only (<repo>/logs/state-machine/), /tmp room briefs,
        room state.json (fcntl-guarded), tmux doorbells, auditor launches.
Never:  kernel code, test files, worker checkouts, fence sources.

Usage:
  state_machine.py tick [--issues 51,52 | --all] [--interval S] [--once]
                         [--dry-run] [--log-dir DIR] [--repo DIR]
  state_machine.py status [--issues ...]   # read-only serial table, no mutation
  state_machine.py brief --issues 52,53 [--dev-branch main]  # RECON briefs only
"""
import argparse
import collections
import datetime
import signal
import tempfile
import fcntl
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import time

VERSION = "state-machine v3 2026-11-02 (IMPL promotion + intake NOTE)"

ROOMS_GLOB = "/tmp/nexios-*"
BOXES = ("dev", "qe", "auditor")
SESS = {"dev": "nexios-dev", "qe": "nexios-qe", "audit": "nexios-audit",
        "consultant": "nexios-consultant"}
LANES = ("dev", "qe")
PEER = {"dev": "qe", "qe": "dev"}

# Modal signature copied from scripts/nexios-bin/worker-state.sh v3 MODAL_RE.
MODAL_RE = (r"allow once|allow always|permission required|needs approval|"
            r"always allow|enter confirm|reject permission|"
            r"tell opencode what to do differently|needs your decision|"
            r"awaiting approval")

POST_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}T\S*) FROM=(dev|qe|audit|auditor) "
    r"(READY|NEED|NOTE|DECISION) :: (.*)$")
RESUME_RE = (r"(hold|waiting|awaiting|parked)[^.]*re-?ring|on re-?ring")
STATE_RE = re.compile(r"^STATE:\s*(.*)$")
NEXT_RE = re.compile(r"^NEXT:\s*(.*)$")
ACK_RE = re.compile(r"^ACK:\s*(\S+)\s*$")
# Order-id grammar — keep byte-identical with _OID_RE in scripts/nexios-bin/mcp-worker-state.py.
_TS19 = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}"
ACK_OID_RE = re.compile(r"^(?:(dev|qe):" + _TS19 + r"|next:(dev|qe):[0-9a-f]{8}|peer:(dev|qe):" + _TS19 + r"|first:(dev|qe))$")
# WAKE lines live in inbox/coord.md (worker->engine doorbell record). The tick
# sweep never read them (G1): load_wakes() surfaces them as wake-<role> events.
WAKE_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})Z FROM=(dev|qe|auditor|audit|coord) WAKE :: (.*)$")


def ref_lane(ref):
    """Lane addressed by an order id, or None. fullmatch is mandatory."""
    m = ACK_OID_RE.fullmatch(ref or "")
    if not m or m.lastindex is None:
        return None
    return m.group(m.lastindex)

VALID_ROOM_STATES = ("IN_PROGRESS", "AUDITED", "VERIFIED", "PLANNED",
                     "QUEUED", "CLOSED", "HOLD")


RING_LIMIT = 3  # doctrine: ring an order max 3x; then NEED-recreate, never a 4th ring
RING_ESCALATED = RING_LIMIT + 1  # ledger sentinel: order escalated as ACK-MISSING, ring no more
CTX_WARN = 70  # log watch-only
CTX_CRIT = 85  # checkpoint NOTE first, then /compact; never die at 100%% with unposted work
AUDIT_TIMEOUT_S = 1800  # live auditor silent this long -> NEED stuck


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
        self.allow_recreate = bool(getattr(args, "allow_recreate", False))
        self.dev_dir = os.path.abspath(getattr(args, "dev_dir", None) or self.repo)
        qd = getattr(args, "qe_dir", None)
        if not qd:
            sib = os.path.join(os.path.dirname(self.repo), "jarvis_test")
            qd = sib if os.path.isdir(sib) else None
        self.qe_dir = os.path.abspath(qd) if qd else None
        self.notes_file = getattr(args, "notes_file", None)
        self._gh_dead = False
        self._gh_need = False
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

    def run(self, argv, timeout=120, cwd=None):
        try:
            p = subprocess.run(argv, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout,
                               text=True, cwd=cwd)
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


CTX_LINE_RE = re.compile(r"^CTX:\s*(\d{1,3})\s*%")

def last_state_next(tail_lines):
    st = nx = None
    cx = None
    for l in tail_lines:
        m = STATE_RE.match(l.strip())
        if m:
            st = m.group(1)[:120]
        m = NEXT_RE.match(l.strip())
        if m:
            nx = m.group(1)[:200]
        m = CTX_LINE_RE.match(l.strip())
        if m:
            try:
                cx = int(m.group(1))
            except ValueError:
                pass
    return st, nx, cx


def wire_gaps(full_lines, post):
    """Missing STATE:/NEXT: trailer lines after a fresh post (wire format).

    The i+1:i+4 window intentionally also covers the optional ACK: 4th
    trailer line, so an ACK never trips a gap.
    """
    try:
        i = full_lines.index(post)
    except ValueError:
        return ["unlocated"]
    nxt = [l.strip() for l in full_lines[i + 1:i + 4]]
    gaps = []
    if not any(l.startswith("STATE:") for l in nxt):
        gaps.append("STATE")
    if not any(l.startswith("NEXT:") for l in nxt):
        gaps.append("NEXT")
    return gaps


def post_ack(full_lines, post):
    """Order id ACKed by a post, or None. Pure.

    Accepts the ACK only on the fixed 3rd trailer line (header/STATE/NEXT/ACK),
    so a body line starting with ACK: can never clear a ledger oid.
    """
    try:
        i = full_lines.index(post)
    except ValueError:
        return None
    if len(full_lines) < i + 4:
        return None
    if not full_lines[i + 1].strip().startswith("STATE:"):
        return None
    if not full_lines[i + 2].strip().startswith("NEXT:"):
        return None
    m = ACK_RE.match(full_lines[i + 3].strip())
    return m.group(1) if m else None


def order_outstanding(role, coord_last, posts, rings, ack_since):
    """Whether a coord order still awaits its ACK. Pure.

    Grandfathered (pre-arm or older-than-arm) orders keep the legacy
    timestamp rule; armed orders stay outstanding until an explicit ACK.
    """
    if not coord_last:
        return False
    oid = "%s:%s" % (role, coord_last[:19])
    if rings.get(oid) == -1:
        return False
    if not ack_since or coord_last[:19] < ack_since[:19]:
        return bool(not posts or coord_last[:19] > posts[-1][:19])
    return True


def order_ring_action(n, ack_gated):
    """Ring/escalate/await decision for an outstanding order count. Pure."""
    if n < RING_LIMIT:
        return "ring"
    if n == RING_LIMIT:
        return "escalate" if ack_gated else "recreate"
    return "awaiting"


def reset_resume_rings(rings):
    """Delete every next: ledger entry. Pure. Returns the deleted count."""
    doomed = [oid for oid in rings if oid.startswith("next:")]
    for oid in doomed:
        del rings[oid]
    return len(doomed)


def ack_scan_posts(role, boxes, handled, fresh):
    """Posts to scan for ACK trailers. Pure.

    Fresh posts normally; but on first sight of a lane (no handled cursor
    yet) the full history is scanned, so a virgin lane's first post —
    which may carry the order's ACK — is actionable instead of being
    consumed silently while the cursor jumps past it.
    """
    if fresh:
        return list(fresh)
    if (handled or {}).get(role) is None:
        return list((boxes.get(role) or {}).get("all", []))
    return []


def match_ack(ref, role, boxes, rings, resume_now):
    """Classify a fresh ACK trailer. Pure. Returns (verdict, oid).

    verdict is "ack" (clear exactly this oid), "ignore-malformed" (bad
    shape or wrong lane), "ignore-stale" (not outstanding), or (None, None)
    when there is no ref to classify.
    """
    if not ref:
        return None, None
    if not ACK_OID_RE.fullmatch(ref) or ref_lane(ref) != role:
        return "ignore-malformed", ref
    boxes = boxes or {}
    rings = rings or {}
    resume_now = resume_now or {}
    cur = None
    if (boxes.get(role) or {}).get("coord_last"):
        cur = "%s:%s" % (role, boxes[role]["coord_last"][:19])
    if ref == cur or ref in rings or ref in resume_now.values():
        return "ack", ref
    return "ignore-stale", ref


CTX_RE = re.compile(r"(\d+(?:\.\d+)?K)\s*\((\d+)%\)")

def pane_ctx_pct(pane_text):
    """Context % from a worker pane footer (e.g. '116.1K (11%)'); None if absent."""
    m = CTX_RE.search(pane_text or "")
    if not m:
        return None
    try:
        return int(m.group(2))
    except ValueError:
        return None


def invalid_params(pane_text):
    """Undelivered-doorbell signature: compact first, re-send after."""
    return bool(re.search(r"invalid[-_]parameters", pane_text or "", re.I))


def append_box_note(room_dir, box, text):
    """Single-line atomic append to a worker box (coordination traffic)."""
    p = os.path.join(room_dir, "inbox", box + ".md")
    try:
        with open(p, "a") as f:
            f.write(text.rstrip("\n") + "\n")
        return True
    except OSError:
        return False


def checkpoint_brief(issue, role, pct):
    return ("CHECKPOINT ORDER (#%d %s at %d%% ctx) -- pre-compaction protocol\n"
            "issue: %d\n"
            "Steps: 1. post CHECKPOINT NOTE to inbox/%s.md (branch + tip + files touched + next step) + RING. "
            "2. run /compact in this session. 3. after compact, post COMPACT-DONE NOTE + RING. "
"Every post carries STATE/NEXT lines plus a CTX: <n>%% line (pane footer). "
            "If the order that triggered this carries [oid <id>], pass ref=<id> on the CHECKPOINT NOTE and the COMPACT-DONE NOTE; ref-less posts never clear the order. "
            "If compaction delivery fails, hold for recreate (never work past 100%% with unposted work).\n"
            "scripts/agent_up.sh wake %d %s nexios-engine \"inbox: %s READY \u2014 read inbox/%s.md\"\n"
            % (issue, role, pct, issue, role, issue, role, role, role))


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


def discover_rooms(only=None, skip_debris=False):
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
        if skip_debris and re.match(r"^\s*provision workers\s*$", st.get("pending") or ""):
            _b = load_boxes(rdir)
            if sum(len(_b[x]["all"]) for x in BOXES) == 0:
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
        st, nx, cx = last_state_next(tail12)
        # Full-box scan: a filed order must not fall out of the tail window
        # before the next tick's unacked loop sees it (G1 follow-through).
        coord = [l for l in all_lines if " FROM=coord NOTE" in l]
        boxes[box] = {"all": posts, "full": all_lines, "tail": tail12, "state": st, "ctx": cx,
                      "next": nx, "coord_last": coord[-1][:160] if coord else None}
    return boxes


def new_posts(boxes, handled, limit=5):
    """Posts newer than handled cursors (ts compare; clocks skew, so max-ts)."""
    fresh = {}
    for box in BOXES:
        since = (handled or {}).get(box)
        if since is None:
            fresh[box] = []  # first sight: init cursor, report nothing
        else:
            posts = [l for l in boxes[box]["all"] if l[:20] > since[:20]]
            fresh[box] = posts[-limit:] if limit else posts
    return fresh


def parse_wakes(lines, since=None):
    """WAKE lines from inbox/coord.md. Pure (G1).

    Returns (wakes, newest): wakes are entries strictly newer than since,
    newest is the max ts over ALL parsed lines (so the wake_seen cursor
    advances past already-consumed lines too). ts is post[:19] shaped."""
    parsed = []
    for line in lines or []:
        m = WAKE_RE.match(line)
        if not m:
            continue
        parsed.append({"ts": m.group(1), "role": m.group(2),
                       "text": m.group(3)})
    parsed.sort(key=lambda w: w["ts"])
    newest = parsed[-1]["ts"] if parsed else None
    if since is None:
        return parsed, newest
    return [w for w in parsed if w["ts"] > since], newest


def load_wakes(room_dir, since=None):
    """(wakes, newest) from a room's inbox/coord.md. Read-only (G1)."""
    return parse_wakes(read_lines(os.path.join(room_dir, "inbox", "coord.md")),
                       since)


def resurface_posts(posts, wake_ts):
    """Last post at-or-before a WAKE, bounded to 1 (G1 one-shot re-drive).

    A WAKE whose post is already at/below the handled cursor would
    otherwise be logged and dropped; resurfacing re-evaluates it exactly
    once, keyed by the additive wake_seen cursor."""
    if not wake_ts:
        return []
    prior = [p for p in (posts or []) if p[:19] <= wake_ts]
    return prior[-1:] if prior else []


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

    The auditor runs headless with --auto (asks auto-approved, denies hold),
    so an auditor modal means a stale pre-auto session: Escape still
    applies, but the lane needs a relaunch, not another tick.
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


def auditor_approved(box_all):
    """Strict APPROVED verdict: the newest auditor verdict line says
    APPROVED. Pure. Only the auditor's own posts count (sender audit or
    auditor) and only verdict-shaped ones (kind DECISION, or a DECISION:
    marker in the body as the READY/NEED verdicts carry) — a dev NOTE
    merely mentioning DECISION can neither spoof nor shadow (Gap A gate)."""
    for line in reversed(box_all or []):
        m = POST_RE.match(line)
        if not m or m.group(2) not in ("audit", "auditor"):
            continue
        if m.group(3) != "DECISION" and "DECISION:" not in m.group(4):
            continue
        return bool(re.search(r"\bAPPROVED\b", m.group(4)))
    return False


def pending_age_s(stored):
    try:
        dt = datetime.datetime.strptime(stored.get("since", ""),
                                        "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
        return (datetime.datetime.now(datetime.timezone.utc) - dt).total_seconds()
    except (ValueError, TypeError, AttributeError):
        return None


def auditor_brief_text(issue, tip, scope):
    return ("AUDIT EXECUTOR (#%d) -- bounded read-only audit\n"
            "issue: %d\ntip: %s (audit this tip only; anything else is out of scope)\n"
            "scope: %s\n"
            "rules: read-only (no branches, no code, no test runs, no launches); retrieval first (graphify+vault); "
            "post DECISION (APPROVED | NEED with S1/S2/S3 findings) to inbox/auditor.md + wake.\n"
            "Auditor verdicts use the DECISION schema and never carry ref/ACK (the bridge rejects ref on box=auditor).\n"
            "scripts/agent_up.sh wake %d auditor nexios-engine \"inbox: auditor READY \u2014 read inbox/auditor.md\"\n"
            % (issue, issue, tip, scope, issue))


def auditor_lifecycle(ctx, issue, room_dir, st, boxes, fresh):
    """Request-driven audit cycle with an invisible auditor session.

    Dev/qe post NEED-audit (request); the engine generates the tip-pinned
    brief, starts the auditor via `agent_up.sh audit` (headless tmux session
    with --auto: asks auto-approved, denies hold; never windowed, and a modal
    there means a stale pre-auto session that must be relaunched),
    awaits the verdict, and hands findings back to the requester (NOTE + ring).
    Legacy pending/phase heuristic stays as fallback. Returns
    (acted, need_verdict, ap_store) with ap_store None (leave), {} (clear),
    or the audit_pending dict to persist.
    """
    acted = []
    pending = (st.get("pending") or "")
    stored = st.get("audit_pending") or {}
    store = None
    for post in fresh.get("auditor", []):
        if "DECISION" not in post:
            continue
        target = stored.get("requester", "dev")
        note, oid = coord_note(target, "AUDIT %s (#%d): detail inbox/auditor.md"
                               % ("APPROVED" if "APPROVED" in post else "NEED", issue))
        rep = latest_audit_report(ctx, issue)
        if rep:
            note += "; report %s" % os.path.basename(rep)
        if ctx.dry:
            acted.append("WOULD hand audit verdict to %s + ring-if-idle" % target)
        else:
            if append_box_note(room_dir, target, note):
                acted.append("audit verdict handed to %s" % target)
                if target in SESS and fast_probe(ctx, SESS[target]).startswith("IDLE"):
                    acted.append(ring(ctx, SESS[target], target, "audit verdict ready", oid=oid,
                                      issue=issue))
            else:
                acted.append("HAND-BACK-FAILED %s" % target)
    if any("audit verdict handed to" in a or "WOULD hand audit verdict" in a for a in acted):
        store = {}
    reqs = [(box, q) for box in ("dev", "qe") for q in fresh.get(box, [])
            if (lambda m: m and m.group(3) == "NEED" and "audit" in q.lower())(POST_RE.match(q))]
    demand = None
    if reqs:
        demand = {"requester": reqs[-1][0], "text": reqs[-1][1][-200:]}
    elif re.search(r"audit", pending, re.I) or st.get("phase") == "AUDIT":
        demand = {"requester": "dev", "text": pending[:200]}
    dec = auditor_decision(boxes["auditor"]["all"])
    if dec and not reqs:
        if stored and store is None:
            acted.append("verdict on file; stale audit_pending cleared")
            store = {}
        return acted, False, store
    if not demand and not stored:
        return acted, False, store
    requester = (demand or {}).get("requester") or stored.get("requester", "dev")
    brief = os.path.join(room_dir, "prompts", "auditor.txt")
    tip = stored.get("tip")
    if not os.path.exists(brief):
        info = git_info(ctx.dev_dir)
        if not info:
            acted.append("NEED audit brief ungeneratable: dev checkout tip unreadable")
            return acted, True, store
        tip = info[1]
        scope_txt = (demand or {}).get("text") or pending[:200] or "lane audit"
        notes = sorted(glob.glob(os.path.join(room_dir, "notes", "*.md")))
        if notes:
            scope_txt += "; room notes: %s" % os.path.basename(notes[-1])
        bw = write_brief(ctx, room_dir, "auditor.txt",
                         auditor_brief_text(issue, tip, scope_txt[:220]))
        acted.append(bw)
        if bw.startswith("BRIEF-FAILED"):
            return acted, True, store
    if audit_session_live(ctx):
        age = pending_age_s(stored)
        if age is not None and age > AUDIT_TIMEOUT_S:
            acted.append("NEED auditor stuck: session live, no verdict for %ds (tip %s)"
                         % (int(age), tip or stored.get("tip", "?")))
        else:
            acted.append("auditor session live; awaiting verdict")
        return acted, True, {"requester": requester,
                             "since": stored.get("since") or utcnow(), "tip": tip}
    if ctx.dry:
        acted.append("WOULD launch auditor: agent_up.sh audit %s" % issue)
        return acted, True, {"requester": requester, "since": utcnow(), "tip": tip}
    rc, out = ctx.run([ctx.agent_up, "audit", str(issue)], timeout=180)
    acted.append("auditor launched rc=%d %s" % (rc, (out.strip().splitlines() or ["?"])[-1][:100]))
    if rc != 0:
        return acted, True, store
    return acted, True, {"requester": requester,
                         "since": stored.get("since") or utcnow(), "tip": tip}


def dev_blocked_on_qe(dev_next):
    return bool(dev_next) and bool(
        re.search(r"waiting\b.*\bqe\b|waiting on.*qe|blocked.*qe", dev_next, re.I))


def pick_backlog(rooms_here, current_issue, exclude=None):
    """Lowest other non-halted, non-closed issue as backlog target.

    exclude holds issues already assigned or busy this cycle: never hand
    out two targets in one cycle, and never target a room whose dev lane
    is busy (G5)."""
    excl = exclude or set()
    for iss, _rdir, st in rooms_here:
        if iss == current_issue or iss in excl:
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


def github_repo_slug(ctx):
    if getattr(ctx, "_slug", None) is not None:
        return ctx._slug
    slug = None
    rc, out = ctx.run(["git", "-C", ctx.repo, "remote", "get-url", "origin"],
                      timeout=15)
    if rc == 0:
        m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?\s*$", out.strip())
        if m:
            slug = m.group(1)
    ctx._slug = slug
    return slug


def github_issue_state(ctx, issue):
    """CLOSED/OPEN via gh API; None on any failure (fail closed)."""
    slug = github_repo_slug(ctx)
    if not slug:
        return None
    rc, out = ctx.run(["gh", "api", "repos/%s/issues/%d" % (slug, issue),
                       "--jq", ".state"], timeout=30)
    if rc != 0:
        return None
    s = out.strip().upper()
    return s if s in ("OPEN", "CLOSED") else None


def pointer_path(ctx, issue):
    return os.path.join(ctx.repo, ".nexios-runtime", str(issue))


def retire_room(ctx, issue, room_dir, st, boxes=None):
    """Gated auto-retire. Destructive steps run only if ALL gates pass:
      G1 room phase == CLOSED (checked by caller)
      G2 GitHub issue state == closed
      G3 `clean <issue> --dry-run` rc == 0
    Live: `clean <issue>`, `watch <issue> disarm`, unlink pointer symlink.
    Anything short -> NEED lines, no mutation. Dry-run never shells clean.
    """
    acts = []
    gh_state = github_issue_state(ctx, issue)
    if gh_state != "CLOSED":
        acts.append("NEED retire #%d blocked: github=%s (need closed)"
                    % (issue, gh_state))
        return acts, False
    if ctx.dry:
        acts.append("WOULD retire #%d: clean --dry-run probe, learnings draft, "
                    "live clean + watch disarm + unlink pointer" % issue)
        return acts, False
    rc, out = ctx.run([ctx.agent_up, "clean", str(issue), "--dry-run"],
                      timeout=120)
    if rc != 0:
        last = (out.strip().splitlines() or ["?"])[-1][:120]
        acts.append("NEED retire #%d blocked: clean --dry-run failed: %s"
                    % (issue, last))
        return acts, False
    if boxes is None:
        boxes = load_boxes(room_dir)
    dp = os.path.join(room_dir, "notes", "learnings-draft-%d.md" % issue)
    try:
        with open(dp, "w") as f:
            f.write(learnings_draft_text(issue, st, boxes))
        acts.append("learnings draft -> %s (curate by hand)" % dp)
    except OSError as e:
        acts.append("LEARNINGS-DRAFT-FAILED: %s" % e)
        return acts, False
    rc, out = ctx.run([ctx.agent_up, "clean", str(issue)], timeout=180)
    last = (out.strip().splitlines() or ["?"])[-1][:100]
    acts.append("clean #%d rc=%d %s" % (issue, rc, last))
    if rc != 0:
        return acts, False
    rc2, _o2 = ctx.run([ctx.agent_up, "watch", str(issue), "disarm"],
                       timeout=30)
    acts.append("ticker disarm #%d rc=%d" % (issue, rc2))
    ptr = pointer_path(ctx, issue)
    try:
        if os.path.islink(ptr):
            os.unlink(ptr)
            acts.append("pointer unlinked %s" % ptr)
        else:
            acts.append("pointer absent (nothing to unlink) %s" % ptr)
    except OSError as e:
        acts.append("POINTER-FAILED %s: %s" % (ptr, e))
        return acts, False
    return acts, True


def ring_line(box, text, oid=None, issue=None):
    """Doorbell pointer text. Pure.

    issue=None keeps the legacy bare-box format byte-identical; with an
    issue the pointer is room-scoped so shared dev/qe sessions cannot
    misdeliver it (G2)."""
    suffix = (" [oid %s]" % oid) if oid else ""
    if issue is None:
        return "inbox: %s%s -- read inbox/%s.md" % (text, suffix, box)
    return "inbox [#%s/%s]: %s%s -- read inbox/%s.md" % (issue, box, text, suffix, box)


def ring(ctx, sess, box, text, channel="inbox", oid=None, issue=None):
    """Doorbell. Idle sessions only (caller checks). inbox = box pointer,
    notes = consultant channel (logs/consultant-notes.md). oid, when given,
    is the ledger order id the worker must ACK (post_update ref=<oid>)."""
    if channel == "notes":
        line = "notes: %s -- read logs/consultant-notes.md" % text
    else:
        line = ring_line(box, text, oid=oid, issue=issue)
    if ctx.dry:
        return "WOULD ring %s: %s" % (sess, line)
    rc, out = ctx.run([ctx.agent_up, "msg", sess, line], timeout=60)
    if rc == 0:
        return "rang %s" % sess
    return "RING-FAILED %s: %s" % (sess, out.strip()[:120])


def coord_note(role, text):
    """Engine order line for a lane box + its ledger oid. Pure.

    The oid embeds the post timestamp exactly as has_unacked derives it,
    so the written id and the ledger key always agree.
    """
    ts = utcnow()
    oid = "%s:%s" % (role, ts[:19])
    return ("%s FROM=coord NOTE :: %s [oid %s]" % (ts, text, oid)), oid


def write_brief(ctx, room_dir, name, content):
    p = os.path.join(room_dir, "prompts", name)
    if ctx.dry:
        return "WOULD write brief %s (%dB)" % (p, len(content))
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(p), prefix=".brief.")
        try:
            with os.fdopen(fd, "w") as f:
                f.write(content)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, p)
        except BaseException:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            raise
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


def need_idle_missing(role, has_unacked, probe, fresh_posts, declared_next, rang):
    """True when a NEED-idle note is warranted for one lane.

    A fresh wire-light NOTE suppresses the false NEED for exactly the
    tick that consumes it (wire-gap NEED-back at tick time remains the
    format signal; next tick with no fresh post and no NEXT still NEEDs).
    """
    if role in has_unacked:
        return False
    if not str(probe).startswith("IDLE"):
        return False
    if fresh_posts:
        return False
    if rang:
        return False
    return not declared_next or declared_next.startswith("idle")


def dev_impl_ready_ts(posts):
    """Newest dev IMPL READY post ts (post[:20]), or None. Pure.

    Mirrors the qe-merge detector (READY + IMPL in body) with the sender
    check the inline lambda lacks: only a dev post counts. Uncapped input
    expected (fresh_all): the capped fresh window can drop a READY."""
    found = None
    for post in posts or []:
        m = POST_RE.match(post)
        if not m or m.group(2) != "dev" or m.group(3) != "READY":
            continue
        if "IMPL" not in post.upper():
            continue
        found = post[:20]
    return found


def backlog_idle_due(role, probe, deaf_lanes, unacked_lanes, fresh_posts,
                     declared_next, rang, recreating, pending):
    """Generic WORKING->WAITING queue check for an idle lane (G5).

    True only when the lane is idle, contactable, order-free, fresh-post
    free, unrung, not being recreated — and the current room's pending
    does not still name dev (never swap away from a room that still owes
    dev work)."""
    if role in deaf_lanes or role in unacked_lanes:
        return False
    if not str(probe).startswith("IDLE"):
        return False
    if fresh_posts:
        return False
    if rang or recreating:
        return False
    if declared_next and not declared_next.startswith("idle"):
        return False
    if re.search(r"\bdev\b", pending or "", re.I):
        return False
    return True


def qe_order_due(qe_order_for, impl_ts):
    """Whether the explicit qe own-box order (G4) must be filed now."""
    return bool(impl_ts) and qe_order_for != impl_ts


def dev_busy(rings, declared_next):
    """Whether a room's dev lane is busy: non-idle NEXT, or a live dev
    order (filed or rung; acked and escalated do not count). Pure."""
    if declared_next and not str(declared_next).startswith("idle"):
        return True
    for oid, val in (rings or {}).items():
        if oid.startswith("dev:") and val not in (-1, RING_ESCALATED):
            return True
    return False


def session_rung(actions, sess, cycle_rung):
    """True iff sess already rang this tick: this room's actions, or an
    earlier room's in the shared cycle (dev/qe sessions ring once per
    cycle, G2). Pure."""
    return rang_session(actions, sess) or sess in (cycle_rung or set())


def impl_promotion_due(dev_next, contacted, fresh_dev, approved, rings,
                       has_unacked, stored):
    """Post-RECON IMPL promotion gate (Gap A). Pure.

    True when dev explicitly waits for an IMPL order, the lane was
    contacted before (virgin lanes go through intake), nothing is in
    flight, audit APPROVED is on file, no live dev order exists, and no
    order was already filed for this exact NEXT text. Takes the LIVE
    rings dict — never the state.json snapshot (in-tick ACKs/filings)."""
    if not (re.search(r"waiting|await|park", dev_next or "", re.I)
            and "IMPL" in (dev_next or "").upper()):
        return False
    if not contacted or fresh_dev:
        return False
    if not approved or "dev" in (has_unacked or set()):
        return False
    for oid, val in (rings or {}).items():
        if oid.startswith("dev:") and val not in (-1, RING_ESCALATED):
            return False
    if (stored or {}).get("next") == (dev_next or ""):
        return False
    return True


def impl_stalled(dev_next, contacted, fresh_dev, approved, rings,
                 has_unacked, stored):
    """ACKed-but-stale-NEXT detector (Gap A follow-through). Pure.

    True when promotion already fired for this exact NEXT text and its
    order resolved (ACKed or vanished) while the NEXT never advanced:
    refiling would spam, staying silent would stall — the caller must
    emit a bounded NEED instead. Never true while the stored order is
    live or escalated."""
    if not (re.search(r"waiting|await|park", dev_next or "", re.I)
            and "IMPL" in (dev_next or "").upper()):
        return False
    if not contacted or fresh_dev:
        return False
    if not approved or "dev" in (has_unacked or set()):
        return False
    stored = stored or {}
    if stored.get("next") != (dev_next or "") or not stored.get("oid"):
        return False
    return stored["oid"] not in (rings or {}) or rings.get(stored["oid"]) == -1


# Lane STATE vocabulary (#457): IDLE (pane idle, waits for new work),
# WORKING (#n) (pane active with a task — #n is the issue the lane is
# ACTUALLY engaged in across rooms, which may differ from the row's
# room), BLOCKED (#n) (pane active, nothing outstanding anywhere, and
# this room's NEXT waits on input). Auditor: WORKING/- (no session when
# its work is done).
LANE_WAITS_RE = re.compile(r"waiting|parked|blocked|hold|await|resume", re.I)
LANE_PEER_RE = re.compile(
    r"\b(auditor|audit|qe|dev|verdict|review|approv|order|ring|ready|input|feedback)\b", re.I)


def room_lane_facts(room_dir, rst, role):
    """(order, post, active, waits) engagement facts for one lane. Pure
    apart from box reads: order = unacked coord order here; post = posts
    newer than the handled cursor (first sight reports no fresh, mirroring
    new_posts); active = non-idle NEXT that is not an input-wait; waits =
    NEXT names an input-wait (audit/qe/dev verdicts, orders, rings)."""
    boxes = load_boxes(room_dir)
    box = boxes.get(role) or {}
    st = rst or {}
    handled = st.get("handled") or {}
    coord_last = box.get("coord_last")
    order = bool(coord_last and order_outstanding(
        role, coord_last, box.get("all", []),
        st.get("rings") or {}, st.get("ack_since")))
    since = handled.get(role)
    post = bool(since is not None and any(
        p[:20] > since[:20] for p in box.get("all", [])))
    nxt = box.get("next") or ""
    waits = bool(LANE_WAITS_RE.search(nxt) and LANE_PEER_RE.search(nxt))
    active = bool(nxt) and not nxt.startswith("idle") and not waits
    return {"order": order, "post": post, "active": active, "waits": waits}


def lane_facts_all(rooms_all, role):
    """{issue: fact-dict} for one lane across rooms. Read-only."""
    out = {}
    for iss, rdir, rst in (rooms_all or []):
        out[iss] = room_lane_facts(rdir, rst, role)
    return out


def lane_state(probe, here, facts, issue):
    """Display STATE for a worker lane cell. Pure.

    here: this room's fact-dict; facts: {issue: fact-dict} across rooms;
    issue: this row's room. The (#n) suffix is the issue the lane is
    ACTUALLY engaged in: unacked order here first, then the lowest
    engaged issue across rooms (order > fresh post > active NEXT), then
    this room's input-wait as BLOCKED. ABSENT/BLOCKED-modal pass through
    (session-level truth); IDLE never carries a suffix."""
    probe = probe or ""
    if probe == "ABSENT" or probe.startswith("BLOCKED-modal"):
        return probe
    if probe.startswith("IDLE"):
        return "IDLE"
    if (here or {}).get("order"):
        return "WORKING (#%d)" % issue
    for kind in ("order", "post", "active"):
        cands = [iss for iss, f in (facts or {}).items()
                 if (f or {}).get(kind)]
        if cands:
            return "WORKING (#%d)" % min(cands)
    if (here or {}).get("waits"):
        return "BLOCKED (#%d)" % issue
    return "WORKING"


def audit_state(live):
    """Auditor lane cell: WORKING while the session exists, else '-'.
    Pure. The auditor has no session when its work is done."""
    return "WORKING" if live else "-"


def peer_mention(text, peer):
    """Whole-word, case-insensitive peer mention in a post body. Pure."""
    return bool(re.search(r"\b%s\b" % re.escape(peer), text or "", re.I))


def peer_ready_offers(boxes, peer):
    """Unanswered READY offers from PEER[peer] addressed to peer, newest first.

    Each offer: {"sender", "ts", "oid", "post"} with ts=post[:19] and
    oid="peer:<peer>:<ts>". A post qualifies iff POST_RE matches,
    group(2) is the opposite lane, group(3) is READY, group(4) mentions
    the peer, and its ts is newer than the peer's latest post ts.
    Scans committed boxes (not the one-tick fresh window). Pure.
    """
    sender = PEER[peer]
    peer_posts = (boxes.get(peer) or {}).get("all", [])
    peer_ts = peer_posts[-1][:19] if peer_posts else ""
    offers = []
    for post in (boxes.get(sender) or {}).get("all", []):
        m = POST_RE.match(post)
        if not m or m.group(2) != sender or m.group(3) != "READY":
            continue
        if not peer_mention(m.group(4), peer):
            continue
        ts = post[:19]
        if peer_ts and ts <= peer_ts:
            continue
        offers.append({"sender": sender, "ts": ts,
                       "oid": "peer:%s:%s" % (peer, ts), "post": post})
    offers.sort(key=lambda o: o["ts"], reverse=True)
    return offers


def peer_handoff_pick(peer, offers, rings, peer_ts):
    """One outstanding handoff for peer, or None. Pure.

    An unresponded rung oid (value 1, peer_ts older than its ts) wins over
    newer offers; otherwise the newest offer. rung=True when the ledger
    already holds 1 for the picked oid.
    """
    for oid in sorted(rings):
        if not oid.startswith("peer:%s:" % peer) or not rings.get(oid):
            continue
        if rings.get(oid) == -1:
            continue
        ots = oid[len("peer:%s:" % peer):]
        if not peer_ts or peer_ts < ots:
            for o in offers:
                if o["oid"] == oid:
                    return dict(o, rung=True)
            return {"sender": PEER[peer], "ts": ots, "oid": oid,
                    "post": "", "rung": True}
    live = [o for o in offers if rings.get(o["oid"]) != -1]
    if not live:
        return None
    pick = dict(live[0])
    pick["rung"] = rings.get(pick["oid"]) == 1
    return pick


def peer_handoff_done_oids(boxes, peer, rings):
    """Ledger oids to flip to -1: peer:<peer>:<ts> with value 1 answered by
    a peer post at/after ts. Pure; other namespaces untouched."""
    peer_posts = (boxes.get(peer) or {}).get("all", [])
    if not peer_posts:
        return []
    peer_ts = peer_posts[-1][:19]
    prefix = "peer:%s:" % peer
    return [oid for oid in rings
            if oid.startswith(prefix)
            and rings.get(oid) == 1 and oid[len(prefix):] <= peer_ts]


def peer_handoff_action(pick, probe, deaf, already_rang, recreating):
    """Pure gate: 'none' (no pick), 'skip' (rung/deaf/rang/recreating),
    'hold' (peer not IDLE), else 'ring'."""
    if pick is None:
        return "none"
    if pick.get("rung") or deaf or already_rang or recreating:
        return "skip"
    if not str(probe).startswith("IDLE"):
        return "hold"
    return "ring"


def rang_session(actions, sess):
    """True iff actions already ring sess (live or dry-run text). Pure."""
    return any(a.startswith("rang " + sess) or
               a.startswith("WOULD ring " + sess) for a in actions)


def recreating_peers(actions):
    """Lanes whose session is being relaunched this tick. Pure."""
    out = set()
    for role in LANES:
        if any(a.startswith("WOULD kill " + SESS[role]) or
               a.startswith("relaunch " + SESS[role]) for a in actions):
            out.add(role)
    return out


def first_contact_due(role, has_posts, brief_present, probe, deaf, rings):
    """One-shot first ring for a virgin lane. Pure.

    Fires only when the lane never posted, a staged brief exists, the lane
    probes IDLE and reachable, and no prior first-contact ring is ledgered.
    """
    if has_posts or not brief_present:
        return False, None
    oid = "first:%s" % role
    if rings.get(oid):
        return False, None
    if deaf or not str(probe).startswith("IDLE"):
        return False, None
    return True, oid


def ring_peer_handoff(ctx, peer, sender, ts, oid, issue=None):
    """Doorbell peer, pointing at the SENDER box holding the READY."""
    return ring(ctx, SESS[peer], sender,
                "peer handoff from %s READY %s" % (sender, ts), oid=oid,
                issue=issue)


def tick_room(ctx, issue, room_dir, st, rooms_all, cycle=None):
    """One deterministic tick for one room. Returns graph node + table rows.

    cycle is the shared per-sweep dict {"rung": set(), "busy": set()}:
    rung holds sessions already doorbelled by earlier rooms (shared
    dev/qe sessions ring once per cycle), busy holds issues already
    assigned as backlog targets this cycle."""
    node = collections.OrderedDict()
    node["issue"] = issue
    node["tick"] = ctx.tick_seq
    actions = []
    events = []
    acked = []
    if cycle is None:
        cycle = {"rung": set(), "busy": set()}

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
    # G1: WAKE lines are durable demand, not tick-exhaust. Surface each as
    # an event, and resurface the post it answers so a WAKE whose post is
    # already at/below the handled cursor is re-evaluated exactly once.
    _ROLE_BOX = {"dev": "dev", "qe": "qe", "auditor": "auditor",
                 "audit": "auditor"}
    wakes, wake_newest = load_wakes(room_dir, st.get("wake_seen"))
    for wake in wakes:
        events.append("wake-%s" % wake["role"])
        actions.append("WAKE %s %s %s" % (wake["role"], wake["ts"],
                                          wake["text"][:100]))
    if wakes:
        _latest_wake = {}
        for wake in wakes:
            _wbox = _ROLE_BOX.get(wake["role"])
            if _wbox:
                _latest_wake[_wbox] = wake["ts"]
        for _wbox, _wts in _latest_wake.items():
            for _post in resurface_posts(boxes[_wbox]["all"], _wts):
                if _post not in fresh[_wbox]:
                    fresh[_wbox].append(_post)
    for box in BOXES:
        for post in fresh[box]:
            m = POST_RE.match(post)
            kind = m.group(3) if m else "NOTE"
            acked.append({"box": box, "kind": kind,
                          "ts": post[:20], "summary": post[-160:]})
            events.append("%s %s" % (box, kind))
            for gap in wire_gaps(boxes[box]["full"], post):
                if box == "auditor":
                    actions.append("wire-advisory auditor post %s lacks %s (verdict schema differs)"
                                   % (post[:20], gap))
                else:
                    actions.append("NEED-back %s: post %s lacks %s (inference banned)"
                                   % (box, post[:20], gap))
    node["new_posts"] = acked
    retired = False
    if (st.get("phase") or "") == "CLOSED":
        r_acts, retired = retire_room(ctx, issue, room_dir, st, boxes)
        actions.extend(r_acts)
        events.append("retire-check")
    if retired:
        node["retired"] = True
        node["merge_ready"] = False
        if ctx.dry:
            actions.append("WOULD advance handled cursors")
        else:
            actions.append("cursors moot: room retired by clean")
        g_acts, gh_posted = gh_transitions(ctx, issue, st, node, actions, False, fresh)
        actions.extend(g_acts)
        if isinstance(gh_posted, dict) and gh_posted:
            def mutate_gh(s, _p=gh_posted):
                posted = s.get("gh_posted") or {}
                posted.update(_p)
                s["gh_posted"] = posted
            if not write_state_locked(room_dir, mutate_gh):
                actions.append("STATE-WRITE-FAILED #%d (gh posted map)" % issue)
        node["actions"] = actions
        node["acked"] = len(acked)
        ctx.graph[str(issue)] = node
        rows = [(role, "RETIRED", "-", "-") for role in ("dev", "qe", "audit")]
        return node, rows

    probes = {}
    for role in ("dev", "qe"):
        probes[role] = fast_probe(ctx, SESS[role])
    guard = modal_guard(ctx, [SESS["dev"], SESS["qe"], SESS["audit"],
                           SESS["consultant"]])
    for sess, acted, detail in guard:
        if acted or "WOULD" in detail:
            actions.append("modal-guard %s: %s" % (sess, detail))
            if sess == SESS["audit"] and acted:
                actions.append("NEED auditor modal under --auto: stale pre-auto session; kill nexios-audit + relaunch audit")
    ctxp = {}
    deaf = set()
    for role in ("dev", "qe"):
        pane = pane_capture(ctx, SESS[role])
        pct = pane_ctx_pct(pane)
        ctxp[role] = pct
        if invalid_params(pane):
            deaf.add(role)
            actions.append("NEED compact-first %s: invalid-parameters in pane; rings suppressed, order /compact"
                           % SESS[role])
            events.append("deaf-%s" % role)
        elif pct is not None and pct >= CTX_CRIT:
            brief = checkpoint_brief(issue, role, pct)
            actions.append(write_brief(ctx, room_dir,
                                       "%s-%d-checkpoint.txt" % (role, issue), brief))
            if ctx.dry:
                actions.append("WOULD post checkpoint NOTE + ring-if-idle (%s @%d%%)"
                               % (role, pct))
            else:
                note, oid = coord_note(role, "CHECKPOINT pre-compaction (@%d%% ctx): "
                                             "post CHECKPOINT NOTE, run /compact, RING" % pct)
                if append_box_note(room_dir, role, note):
                    actions.append("checkpoint NOTE filed for %s @%d%%" % (role, pct))
                    if role not in deaf and probes[role].startswith("IDLE"):
                        actions.append(ring(ctx, SESS[role], role,
                                            "checkpoint brief @%d%%" % pct, oid=oid))
                    else:
                        actions.append("%s WORKING: checkpoint NOTE filed, ring deferred to boundary"
                                       % role)
                else:
                    actions.append("NOTE-FAILED %s checkpoint (inbox append failed)" % role)
            events.append("ctx-crit-%s" % role)
        elif pct is not None and pct >= CTX_WARN:
            actions.append("CTX-WARN %s at %d%% (watch)" % (role, pct))
    node["probes"] = probes
    node["context"] = ctxp
    role_map = pane_role_sessions(ctx)
    dups = {r: s for r, s in role_map.items() if len(s) > 1}
    if "UNLABELED" in role_map:
        actions.append("NOTE unlabeled agent session(s) %s (opencode without NEXIOS_AGENT_ROLE; review by hand)"
                       % ",".join(role_map["UNLABELED"]))
    for r, s in sorted(dups.items()):
        actions.append("NEED stale-duplicate %s: sessions %s (never auto-kill another starter's; close by hand)"
                       % (r, ",".join(s)))
    node["dup_sessions"] = dups
    posted = {}
    for role in ("dev", "qe"):
        posted[role] = boxes[role].get("ctx")
        pc = ctxp.get(role)
        if posted[role] is not None and pc is not None and abs(posted[role] - pc) > 15:
            actions.append("CTX-MISMATCH %s: posted %d%% vs pane %d%% (trust pane)"
                           % (role, posted[role], pc))
    node["context_posted"] = posted

    a_acts, need_verdict, ap_store = auditor_lifecycle(ctx, issue, room_dir, st, boxes, fresh)
    actions.extend(a_acts)
    r_acts, rel_store = release_tick(ctx, issue, boxes, st)
    actions.extend(r_acts)

    dev_next = boxes["dev"]["next"]

    rings = st.get("rings") or {}
    ack_since = st.get("ack_since") or None
    ack_failures = int(st.get("ack_failures", 0) or 0)
    resume_now = {}
    for role in ("dev", "qe"):
        nx = (boxes[role]["next"] or "")
        if re.search(RESUME_RE, nx, re.I):
            resume_now[role] = "next:%s:%s" % (role, hashlib.md5(nx.encode()).hexdigest()[:8])
    fresh_all = new_posts(boxes, handled, limit=0)  # 0 = no cap: no ACK lost
    if wakes:
        _latest_wake_all = {}
        for wake in wakes:
            _wbox = _ROLE_BOX.get(wake["role"])
            if _wbox:
                _latest_wake_all[_wbox] = wake["ts"]
        for _wbox, _wts in _latest_wake_all.items():
            for _post in resurface_posts(boxes[_wbox]["all"], _wts):
                if _post not in fresh_all[_wbox]:
                    fresh_all[_wbox].append(_post)
    for role in LANES:
        for post in ack_scan_posts(role, boxes, handled, fresh_all.get(role, [])):
            ack_verdict, ack_ref = match_ack(post_ack(boxes[role]["full"], post),
                                            role, boxes, rings, resume_now)
            if ack_verdict is None:
                continue
            if ack_verdict == "ignore-malformed":
                actions.append("ack-ignored %s from %s (malformed or wrong lane)" % (ack_ref, role))
            elif ack_verdict == "ignore-stale":
                actions.append("ack-ignored %s from %s (not outstanding)" % (ack_ref, role))
            else:
                rings[ack_ref] = -1
                actions.append("ack %s from %s" % (ack_ref, role))
                events.append("%s ACK" % role)
                if not ack_since:
                    ack_since = utcnow()[:19]
                    n = reset_resume_rings(rings)
                    actions.append("ack-discipline armed at %s by %s (legacy resume counters reset: %d)"
                                   % (ack_since, role, n))
    has_unacked = set()
    for role in ("dev", "qe"):
        b = boxes[role]
        if not b["coord_last"]:
            continue
        oid = "%s:%s" % (role, b["coord_last"][:19])
        ack_gated = bool(ack_since) and b["coord_last"][:19] >= ack_since[:19]
        if not order_outstanding(role, b["coord_last"], b["all"], rings, ack_since):
            continue
        has_unacked.add(role)
        n = rings.get(oid, 0)
        act = order_ring_action(n, ack_gated)
        if role in deaf or not probes[role].startswith("IDLE"):
            actions.append("%s WORKING reads at boundary; no ring" % role)
        elif act == "ring":
            disp = re.sub(r"\s*\[oid [^]]+\]\s*$", "", b["coord_last"])[:80]
            actions.append(ring(ctx, SESS[role], role,
                                "unacked order -- %s" % disp, oid=oid))
            rings[oid] = n + 1
            events.append("ring-%s" % role)
        elif act == "recreate":
            r_acts, _rel = recreate_worker(ctx, issue, room_dir, role,
                "order %s unacked after %d rings" % (b["coord_last"][:19], n),
                b["state"], b["next"])
            actions.extend(r_acts)
            rings[oid] = -1 if _rel else n + 1
        elif act == "escalate":
            actions.append("ACK-MISSING %s order %s after %d rings (no ACK post)"
                           % (role, oid, n))
            rings[oid] = RING_ESCALATED
            ack_failures += 1
            events.append("ack-missing-%s" % role)
        else:
            if ack_gated:
                actions.append("ack-awaiting %s: order %s (no repeat rings; awaiting ACK)"
                               % (SESS[role], oid))
            else:
                actions.append("recreate-awaiting %s: order %s (no repeat orders)"
                               % (SESS[role], b["coord_last"][:19]))
    for oid in list(rings):
        if oid.startswith("next:"):
            _p, _r, _h = oid.split(":")
            if oid not in resume_now.values():
                del rings[oid]
                continue
            if not ack_since and fresh.get(_r):
                del rings[oid]
            continue
        if oid.startswith("peer:"):
            continue  # retired only by peer_handoff_done_oids below
        if oid.startswith("first:"):
            continue  # one-shot; virginity ends it
        orole, _, ots = oid.partition(":")
        cur = (boxes.get(orole) or {}).get("coord_last")
        if cur and ots < cur[:19]:
            del rings[oid]  # superseded by a newer order
            continue
        if not ack_since or ots < ack_since[:19]:
            posts = boxes.get(orole, {}).get("all", [])
            if posts and posts[-1][:19] > ots:
                del rings[oid]
    for role, oid in resume_now.items():
        if role in has_unacked or role in deaf:
            continue
        if not probes[role].startswith("IDLE"):
            continue
        n = rings.get(oid, 0)
        act = order_ring_action(n, bool(ack_since))
        if act == "ring":
            actions.append(ring(ctx, SESS[role], role, "resume per declared NEXT", oid=oid))
            rings[oid] = n + 1
            events.append("ring-%s-resume" % role)
        elif act == "recreate":
            r_acts, _rel = recreate_worker(ctx, issue, room_dir, role,
                "parked-awaiting ring unacked after %d rings" % n,
                boxes[role]["state"], boxes[role]["next"])
            actions.extend(r_acts)
            rings[oid] = -1 if _rel else n + 1
        elif act == "escalate":
            actions.append("ACK-MISSING %s resume %s after %d rings (no ACK post)"
                           % (role, oid, n))
            rings[oid] = RING_ESCALATED
            ack_failures += 1
            events.append("ack-missing-%s" % role)
        elif ack_since:
            actions.append("ack-awaiting %s: resume %s (no repeat rings; awaiting ACK)"
                           % (SESS[role], oid))
    if ack_failures == 3:
        actions.append("NEED ack-discipline: 3 orders reached RING_LIMIT without ACK; refresh briefs/wake lanes")

    # Uncapped IMPL READY detector (G4): the capped fresh window can drop
    # a READY, and the qe own-box order must fire on the newest one.
    # The peer-pointer skip below covers exactly the filed-or-due set (S3):
    # a NOTE-FAILED retry stays due (qo_store persists only on success).
    impl_ts = dev_impl_ready_ts(fresh_all.get("dev", []))
    qe_order_for = (st.get("qe_order") or {}).get("for")
    qe_will_file = bool(impl_ts and qe_order_due(qe_order_for, impl_ts))
    for peer in LANES:
        if not ack_since:
            for oid in peer_handoff_done_oids(boxes, peer, rings):
                rings[oid] = -1
                events.append("peer-handoff-done-%s" % peer)
        if peer == "qe" and impl_ts and (qe_will_file or qe_order_for == impl_ts):
            # The explicit qe own-box order (below) supersedes the peer
            # pointer this tick: one doorbell to nexios-qe, room-scoped.
            continue
        offers = peer_ready_offers(boxes, peer)
        peer_posts = boxes[peer]["all"]
        peer_ts = peer_posts[-1][:19] if peer_posts else ""
        pick = peer_handoff_pick(peer, offers, rings, peer_ts)
        if pick is None:
            continue
        act = peer_handoff_action(pick, probes[peer], peer in deaf,
                                  session_rung(actions, SESS[peer], cycle["rung"]),
                                  peer in recreating_peers(actions))
        if act == "ring":
            res = ring_peer_handoff(ctx, peer, pick["sender"], pick["ts"],
                                    pick["oid"], issue=issue)
            actions.append(res)
            if res.startswith(("rang ", "WOULD ring ")):
                rings[pick["oid"]] = 1
            events.append("peer-handoff-%s" % peer)
        elif act == "hold":
            actions.append("peer-handoff %s held: %s READY %s unanswered "
                           "(probe %s; no ring)"
                           % (peer, pick["sender"], pick["ts"], probes[peer]))
            events.append("peer-handoff-hold-%s" % peer)

    for role in ("dev", "qe"):
        brief_p = os.path.join(room_dir, "prompts", "%s.txt" % role)
        try:
            brief_present = os.path.getsize(brief_p) > 0
        except OSError:
            brief_present = False
        due, first_oid = first_contact_due(role, bool(boxes[role]["all"]),
                                           brief_present, probes[role],
                                           role in deaf, rings)
        if not due:
            continue
        # Gap B: intake is a full acked order (coord NOTE + room-scoped
        # ring with the standard dev:<ts>/qe:<ts> oid), not a ring-only
        # pointer. first:<role> stays only the one-shot guard flag: set
        # -1 when filed (never rung, never ACKed; never write 0 — falsy).
        note, ioid = coord_note(role, "INTAKE #%d %s first brief -- read prompts/%s.txt + room pending; ACK it with post_update ref=<id> before starting" % (issue, role, role))
        if ioid in rings:
            actions.append("order deferred %s intake (oid collision)" % role)
            continue
        if ctx.dry:
            actions.append("WOULD post %s intake order (oid %s)" % (role, ioid))
            rings[ioid] = 0
            rings[first_oid] = -1
            has_unacked.add(role)
            if role == "dev":
                cycle["dev_order"] = issue
        else:
            if append_box_note(room_dir, role, note):
                rings[ioid] = 0
                rings[first_oid] = -1
                has_unacked.add(role)
                actions.append("order filed %s intake (oid %s)" % (role, ioid))
                if role == "dev":
                    cycle["dev_order"] = issue
                if (role not in deaf and probes[role].startswith("IDLE")
                        and not session_rung(actions, SESS[role], cycle["rung"])):
                    res = ring(ctx, SESS[role], role,
                               "first brief -- read prompts/%s.txt" % role,
                               oid=ioid, issue=issue)
                    actions.append(res)
                    if res.startswith(("rang ", "WOULD ring ")):
                        rings[ioid] = 1
            else:
                actions.append("NOTE-FAILED %s intake #%d" % (role, issue))
        events.append("first-contact-%s" % role)

    # G3 + G5: backlog with ack. Both the blocked-on-qe swap and the
    # generic WORKING->IDLE queue check file an acked order (coord NOTE in
    # dev.md + room-scoped ring with oid); the backlog brief carries the
    # order id from here on.
    def _busy_rooms():
        # Cached per cycle: other rooms' dev-busy state is static for the
        # sweep; recomputing per room is O(n^2) box re-reads (S3).
        if "busy_rooms" not in cycle:
            out = set()
            for _iss, _rdir, _rst in (rooms_all or []):
                if _iss == issue:
                    continue
                if dev_busy((_rst or {}).get("rings"),
                            load_boxes(_rdir)["dev"].get("next")):
                    out.add(_iss)
            cycle["busy_rooms"] = out
        return cycle["busy_rooms"]

    backlog_target = None
    if dev_blocked_on_qe(dev_next) and not probes["qe"].startswith("IDLE"):
        backlog_target = pick_backlog(rooms_all, issue,
                                      exclude=cycle["busy"] | _busy_rooms())
        if backlog_target is None:
            actions.append("NEED backlog empty: dev waits on qe, nothing to swap to")
    elif not cycle.get("dev_order") and backlog_idle_due("dev", probes["dev"], deaf, has_unacked,
                          fresh.get("dev"),
                          boxes["dev"]["next"],
                          session_rung(actions, SESS["dev"], cycle["rung"]),
                          "dev" in recreating_peers(actions),
                          st.get("pending")):
        backlog_target = pick_backlog(rooms_all, issue,
                                      exclude=cycle["busy"] | _busy_rooms())
        if backlog_target is None:
            actions.append("NEED backlog empty: dev idle, no other room to brief")
    if backlog_target is not None:
        target = backlog_target
        brief = ("BACKLOG SWAP (#%d) -- dev blocked on qe output in #%d\n"
                 "issue: %d\nbranch: none yet (read-only until recalled)\n"
                 "Task: read-only recon of #%d per its room pending/next; "
                 "no branch, no code. Every post ends with STATE/NEXT lines plus a CTX: <n>%% line (pane footer). "
"Post NOTE to inbox/dev.md + RING. If a ring or NOTE carries [oid <id>], ACK it with post_update ref=<id>; the backlog assignment carries [oid <id>] — ACK it the same way.\n"
                 "scripts/agent_up.sh wake %d dev nexios-engine \"inbox: dev READY — read inbox/dev.md\"\n"
                 % (target, issue, target, target, target))
        actions.append(write_brief(ctx, room_dir,
                                   "dev-%d-backlog.txt" % target, brief))
        note, boid = coord_note("dev", "BACKLOG assignment #%d (this room #%d): read prompts/dev-%d-backlog.txt; ACK it with post_update ref=<id> before starting" % (target, issue, target))
        if ctx.dry:
            actions.append("WOULD post backlog order #%d (oid %s)" % (target, boid))
        else:
            if append_box_note(room_dir, "dev", note):
                rings[boid] = 0
                actions.append("order filed dev backlog #%d (oid %s)" % (target, boid))
                if ("dev" not in deaf and probes["dev"].startswith("IDLE")
                        and not session_rung(actions, SESS["dev"], cycle["rung"])):
                    res = ring(ctx, SESS["dev"], "dev",
                               "backlog #%d ready" % target, oid=boid,
                               issue=issue)
                    actions.append(res)
                    if res.startswith(("rang ", "WOULD ring ")):
                        rings[boid] = 1
            else:
                actions.append("NOTE-FAILED dev backlog order #%d" % target)
        cycle["busy"].update((issue, target))
        events.append("backlog-swap #%d" % target)

    # Gap A: post-RECON IMPL promotion. A plan-approved dev that explicitly
    # waits for an IMPL order gets an acked order (NOTE + room-scoped ring),
    # instead of waiting forever while pending names dev.
    stored_impl = st.get("dev_impl") or {}
    di_store = None
    if stored_impl and stored_impl.get("next") != (boxes["dev"]["next"] or ""):
        di_store = {}
    _dev_next = boxes["dev"]["next"] or ""
    _approved = auditor_approved(boxes["auditor"]["all"])
    if impl_promotion_due(_dev_next, bool(boxes["dev"]["all"]),
                          fresh.get("dev"), _approved, rings, has_unacked,
                          stored_impl):
        note, di_oid = coord_note("dev", "IMPL promotion #%d: plan approved (auditor APPROVED on file); start IMPL per room briefs/notes; ACK it with post_update ref=<id> before starting" % issue)
        if di_oid in rings:
            actions.append("order deferred dev IMPL (oid collision)")
        elif ctx.dry:
            actions.append("WOULD post dev IMPL order (oid %s)" % di_oid)
            rings[di_oid] = 0
            has_unacked.add("dev")
            di_store = {"next": _dev_next, "oid": di_oid}
            cycle["dev_order"] = issue
        elif append_box_note(room_dir, "dev", note):
            rings[di_oid] = 0
            has_unacked.add("dev")
            actions.append("order filed dev IMPL (oid %s)" % di_oid)
            di_store = {"next": _dev_next, "oid": di_oid}
            cycle["dev_order"] = issue
            if ("dev" not in deaf and probes["dev"].startswith("IDLE")
                    and not session_rung(actions, SESS["dev"], cycle["rung"])):
                res = ring(ctx, SESS["dev"], "dev",
                           "IMPL order -- proceed to step-a", oid=di_oid,
                           issue=issue)
                actions.append(res)
                if res.startswith(("rang ", "WOULD ring ")):
                    rings[di_oid] = 1
        else:
            actions.append("NOTE-FAILED dev IMPL #%d" % issue)
        events.append("dev-impl-order-filed")
    elif impl_stalled(_dev_next, bool(boxes["dev"]["all"]), fresh.get("dev"),
                      _approved, rings, has_unacked, stored_impl):
        actions.append("NEED dev #%d: IMPL order %s filed+resolved but NEXT unchanged (stale NEXT; no new order)"
                       % (issue, stored_impl.get("oid")))

    for role in ("dev", "qe"):
        rang = session_rung(actions, SESS[role], cycle["rung"])
        if need_idle_missing(role, has_unacked, probes[role],
                             fresh.get(role), boxes[role]["next"], rang):
            actions.append("NEED %s idle with no briefed work (no invention)" % role)

    contacted = bool(boxes["dev"]["all"] or boxes["qe"]["all"])
    if verdict.get("escalate") and contacted:
        slow = {r: slow_probe(ctx, SESS[r]) for r in ("dev", "qe")}
        node["slow_probes"] = slow
        actions.append("ESCALATE #%d UNCHANGED x%d: slow=%s" %
                       (issue, verdict["streak"], slow))
        for r in ("dev", "qe"):
            if slow.get(r) == "STUCK":
                r_acts, _rel = recreate_worker(ctx, issue, room_dir, r,
                    "UNCHANGED x%d STUCK" % verdict["streak"],
                    boxes[r]["state"], boxes[r]["next"])
                actions.extend(r_acts)
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
                                    "armed trigger [%s]" % arm.get("on", ""),
                                    issue=issue))
            else:
                actions.append("NEED malformed armed do: %s" % do)
        else:
            actions.append("ARMED [%s] fired: %s" % (arm.get("on", ""), do[:120]))
        events.append("armed-fired")

    # G4: dev IMPL READY -> explicit qe own-box order (coord NOTE in qe.md
    # + room-scoped ring with oid), not just the peer pointer. The qe-merge
    # staging below keeps its own dedupe; both share impl_ts.
    # qo_store persists ONLY on a filed note: a NOTE-FAILED retry needs
    # qe_order_due true next tick, and the peer-pointer skip below stays
    # exactly the filed-or-due set (S3).
    qo_store = None
    if qe_will_file:
        note, qoid = coord_note("qe", "IMPL READY #%d dev %s — start verification per prompts/qe.txt + room pending; ACK it with post_update ref=<id> before starting" % (issue, impl_ts))
        if ctx.dry:
            actions.append("WOULD post qe order IMPL READY (oid %s)" % qoid)
        else:
            if append_box_note(room_dir, "qe", note):
                rings[qoid] = 0
                actions.append("order filed qe IMPL READY (oid %s)" % qoid)
                qo_store = {"for": impl_ts}
                if ("qe" not in deaf and probes["qe"].startswith("IDLE")
                        and not session_rung(actions, SESS["qe"], cycle["rung"])):
                    res = ring(ctx, SESS["qe"], "qe", "IMPL READY verification",
                               oid=qoid, issue=issue)
                    actions.append(res)
                    if res.startswith(("rang ", "WOULD ring ")):
                        rings[qoid] = 1
            else:
                actions.append("NOTE-FAILED qe IMPL READY #%d" % issue)
        events.append("qe-order-filed")

    dec = auditor_decision(boxes["auditor"]["all"])
    qe_ready = any("READY" in l for l in boxes["qe"]["all"][-3:])
    node["merge_ready"] = bool(dec and "APPROVED" in dec and qe_ready)
    qm_store = None
    if not ctx.dry:
        if impl_ts:
            ts = impl_ts
            if (st.get("qe_merge") or {}).get("for") != ts:
                qd = ctx.qe_dir
                if not qd or not os.path.isdir(qd):
                    actions.append("NEED qe-merge #%d: testbed checkout unknown" % issue)
                else:
                    info_q = git_info(qd)
                    dirty_q = ([l for l in info_q[2]
                                if not l.split()[-1].startswith(QE_EXEMPT)]
                               if info_q else None)
                    if info_q is None or dirty_q:
                        actions.append("NEED qe-merge #%d: testbed not clean; clean first" % issue)
                    else:
                        rc_q, out_q = ctx.run([ctx.agent_up, "qe-merge", str(issue)],
                                              timeout=300, cwd=qd)
                        last_q = (out_q.strip().splitlines() or ["?"])[-1][:100]
                        if rc_q == 0:
                            actions.append("testbed staged for #%d (merge --no-commit held)" % issue)
                            qm_store = {"for": ts}
                        else:
                            actions.append("NEED qe-merge #%d failed: %s" % (issue, last_q))
    else:
        if impl_ts:
            actions.append("WOULD stage testbed via qe-merge on fresh IMPL READY")

    failed = any(a.startswith(("RING-FAILED", "STATE-FAILED", "BRIEF-FAILED",
                               "POINTER-FAILED", "STATE-WRITE-FAILED"))
                 for a in actions)
    escalated = bool(verdict.get("escalate")) and contacted
    retries = int(st.get("retries", 0) or 0)
    budget = int(st.get("retry_budget", 3) or 0)
    if escalated or failed:
        retries += 1
    elif acked:
        retries = 0
    halt_now = bool(budget) and retries >= budget
    if halt_now:
        actions.append("HALT #%d: retry budget exhausted (%d/%d) -> MANUAL_INTERVENTION"
                       % (issue, retries, budget))
    g_acts, gh_posted = gh_transitions(ctx, issue, st, node, actions, halt_now, fresh)
    actions.extend(g_acts)
    def mutate(s):
        h = s.get("handled") or {}
        for box in BOXES:
            if boxes[box]["all"]:
                h[box] = max(x[:20] for x in boxes[box]["all"])
        s["handled"] = h
        s["rings"] = rings
        if ack_since:
            s["ack_since"] = ack_since
        s["ack_failures"] = ack_failures
        s["retries"] = retries
        if isinstance(ap_store, dict):
            if ap_store:
                s["audit_pending"] = ap_store
            else:
                s.pop("audit_pending", None)
        if isinstance(rel_store, dict):
            s["release"] = rel_store
        elif rel_store == "closed":
            s.pop("release", None)
            s["release_armed"] = False
        if isinstance(gh_posted, dict) and gh_posted:
            posted = s.get("gh_posted") or {}
            posted.update(gh_posted)
            s["gh_posted"] = posted
        if isinstance(qm_store, dict) and qm_store:
            s["qe_merge"] = qm_store
        if isinstance(qo_store, dict) and qo_store:
            s["qe_order"] = qo_store
        if isinstance(di_store, dict):
            if di_store:
                s["dev_impl"] = di_store
            else:
                s.pop("dev_impl", None)
        if wake_newest:
            s["wake_seen"] = wake_newest
        if halt_now:
            s["halt"] = True
            s["pending"] = ("MANUAL_INTERVENTION: retry budget exhausted "
                            "(%d/%d)" % (retries, budget))
        s["updated"] = utcnow()
    if ctx.dry:
        actions.append("WOULD advance handled cursors")
        if halt_now:
            actions.append("WOULD HALT #%d: budget %d/%d" % (issue, retries, budget))
    else:
        if not write_state_locked(room_dir, mutate):
            actions.append("STATE-WRITE-FAILED #%d" % issue)

    node["actions"] = actions
    node["acked"] = len(acked)
    ctx.graph[str(issue)] = node

    rows = []
    # Lane engagement across rooms (#457): the (#n) suffix is the issue
    # the lane is ACTUALLY working, which may differ from this row.
    # Recomputed per room tick (small files; always current, no cache).
    lane_facts = {"dev": lane_facts_all(rooms_all, "dev"),
                  "qe": lane_facts_all(rooms_all, "qe")}
    for role, sess in (("dev", SESS["dev"]), ("qe", SESS["qe"]),
                       ("audit", SESS["audit"])):
        if role == "audit":
            rep = latest_audit_report(ctx, issue)
            activity = ("report %s" % os.path.basename(rep)) if rep else (
                auditor_decision(boxes["auditor"]["all"]) or "no verdict")
            planned = "collect verdict" if need_verdict else "-"
            state = audit_state(audit_session_live(ctx))
        else:
            activity = boxes[role]["state"] or boxes[role]["next"] or "-"
            planned = next((a for a in actions
                            if role in a or SESS[role] in a), "-")
            base = lane_state(probes[role],
                              lane_facts[role].get(issue, {}),
                              lane_facts[role], issue)
            state = base + (" %d%%" % ctxp[role] if ctxp.get(role) is not None else "")
        rows.append((role, state, activity, planned))
    return node, rows


def git_info(workdir):
    """(branch, tip, dirty_list) read-only; None on any failure (no blind relaunch)."""
    def run(args):
        try:
            pr = subprocess.run(["git", "-C", workdir] + args, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=30, text=True)
            return (pr.returncode, pr.stdout) if pr.returncode == 0 else (pr.returncode, None)
        except (OSError, subprocess.TimeoutExpired):
            return (1, None)
    rc, branch = run(["branch", "--show-current"])
    rc2, tip = run(["rev-parse", "--short", "HEAD"])
    rc3, dirty = run(["status", "--porcelain"])
    if rc != 0 or rc2 != 0 or rc3 != 0 or not (branch or "").strip():
        return None
    lines = (tip or "?").strip().splitlines()
    return (branch.strip(), lines[0] if lines else "?",
            [l for l in (dirty or "").splitlines() if l.strip()][:15])


def resume_brief_text(issue, role, branch, tip, dirty, state, nxt):
    dirty_lines = "".join("  %s\n" % l for l in dirty) or "  (clean tree)\n"
    return ("RESUME (#%d %s) -- auto-recreate: prior session stuck/unresponsive\n"
            "issue: %d\nbranch: %s (stay; do not switch before checkpoint)\n"
            "tip at relaunch: %s\ntree carried over:\n%s"
            "last STATE: %s\nlast NEXT: %s\n"
            "Steps: 1. verify inventory (branch/tip/files above match your tree). "
            "2. post CHECKPOINT NOTE to inbox/%s.md (branch + tip + files touched + next step) + RING. "
            "3. continue the next step above; normal wire discipline (STATE/NEXT + CTX %% line, doorbell every post); "
            "engine orders carry [oid <id>] and must be ACKed via post_update ref=<id>.\n"
            "scripts/agent_up.sh wake %d %s nexios-engine \"inbox: %s READY \u2014 read inbox/%s.md\"\n"
            % (issue, role, issue, branch, tip, dirty_lines,
               state or "-", nxt or "-", role,
               issue, role, role, role))


def proc_tree(ctx):
    """Full (pid, ppid) table in one ps call (pgrep -P misses some parents)."""
    rc, out = ctx.run(["ps", "-e", "-o", "pid=,ppid="], timeout=15)
    if rc != 0:
        return {}
    kids = collections.defaultdict(list)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].lstrip("-").isdigit():
            kids[parts[1]].append(parts[0])
    return kids


def pane_role_sessions(ctx):
    """Live tmux sessions grouped by NEXIOS_AGENT_ROLE (ps-walk, read-only)."""
    found = collections.defaultdict(list)
    rc, out = tmux(ctx, "ls", "-F", "#{session_name}")
    if rc != 0:
        return {}
    kids = proc_tree(ctx)
    for sess in [l.strip() for l in out.splitlines() if l.strip()]:
        rc2, pids = tmux(ctx, "list-panes", "-t", sess, "-F", "#{pane_pid}")
        if rc2 != 0:
            continue
        stack = [q.strip() for q in pids.splitlines() if q.strip().isdigit()]
        seen = set(stack)
        i = 0
        while i < len(stack) and len(stack) < 500:
            for k in kids.get(stack[i], []):
                if k not in seen:
                    seen.add(k)
                    stack.append(k)
            i += 1
        if not stack:
            continue
        rc3, env = ctx.run(["ps", "eww", "-p", ",".join(stack[:200])], timeout=15)
        if rc3 != 0:
            continue
        role_hit = None
        saw_agent = False
        for line in env.splitlines():
            toks = line.split(None, 1)
            if not toks or not toks[0].isdigit():
                continue
            rest = toks[1] if len(toks) > 1 else ""
            m = re.search(r"NEXIOS_AGENT_ROLE=([A-Za-z]+)", rest)
            if m:
                role_hit = m.group(1)
                break
            if re.search(r"bin/opencode\b", rest):
                saw_agent = True
        if role_hit:
            if sess not in found[role_hit]:
                found[role_hit].append(sess)
        elif saw_agent and sess not in found["UNLABELED"]:
            found["UNLABELED"].append(sess)
    return dict(found)


def recreate_worker(ctx, issue, room_dir, role, reason, state, nxt):
    """Kill + relaunch a stuck worker with a resume brief. Gated, step by step.

    Preconditions (any miss -> NEED, no mutation beyond the brief file):
    dev|qe only; checkout dir known; branch+tip readable; brief written;
    --allow-recreate; kill confirmed; run rc 0. Duplicates are NEVER auto-killed.
    """
    sess = SESS[role]
    acts = []
    if role not in ("dev", "qe"):
        return ["NEED recreate refused: role %s not worker-relocatable" % role], False
    d = ctx.dev_dir if role == "dev" else ctx.qe_dir
    if not d or not os.path.isdir(d):
        return ["NEED recreate %s: checkout dir unknown" % sess], False
    info = git_info(d)
    if not info:
        return ["NEED recreate %s: cannot read branch/tip (refuse blind relaunch)" % sess], False
    branch, tip, dirty = info
    name = "%s-%d-resume-auto.txt" % (role, issue)
    w = write_brief(ctx, room_dir, name,
                    resume_brief_text(issue, role, branch, tip, dirty, state, nxt))
    acts.append(w)
    if w.startswith("BRIEF-FAILED"):
        return acts, False
    if ctx.dry:
        acts.append("WOULD kill %s + run %s on %s@%s (allow-recreate)" % (sess, sess, branch, tip))
        return acts, False
    if not ctx.allow_recreate:
        acts.append("NEED recreate %s (%s): resume brief ready at prompts/%s; kill+run needs --allow-recreate"
                    % (sess, reason, name))
        return acts, False
    rc, _o = tmux(ctx, "kill-session", "-t", sess)
    if rc != 0:
        acts.append("KILL-FAILED %s (not relaunched)" % sess)
        return acts, False
    rc2, _o2 = tmux(ctx, "has-session", "-t", sess)
    if rc2 == 0:
        acts.append("KILL-UNCONFIRMED %s still present (not relaunched)" % sess)
        return acts, False
    args = [ctx.agent_up, "run", sess, d, role,
            os.path.join(room_dir, "prompts", name)]
    if branch and branch != "HEAD":
        args.append(branch)
    rc3, out3 = ctx.run(args, timeout=180)
    last = (out3.strip().splitlines() or ["?"])[-1][:100]
    acts.append("relaunch %s rc=%d %s" % (sess, rc3, last))
    if rc3 != 0:
        acts.append("NEED MANUAL %s is DOWN after failed relaunch; rerun: %s"
                    % (sess, " ".join(args)))
        return acts, False
    return acts, True


RELEASE_OWNERS = {1: "consultant", 2: "consultant", 3: "consultant",
                  4: "consultant", 5: "consultant", 6: "consultant", 7: "engine"}
RELEASE_TEXTS = {
    1: "Verify merge: lane branch merged into main; main==origin/main; tree clean.",
    2: "Confirm release version with operator (repo convention + milestone); record as STEP 2 DONE :: vX.Y.Z.",
    3: "Create annotated tag + push for vX.Y.Z.",
    4: "Draft release notes (README highlights + milestone + lane evidence) to logs/release-<issue>-notes.md.",
    5: "Create GitHub Release from draft; the Nextcloud mirror remains OPERATOR (coordinate in chat).",
    6: "Verify release exists + mirror present; record evidence.",
    7: "ENGINE: API-verify + close case.",
}
DONE_RE = re.compile(r"RELEASE\s+(\d+)\s+STEP\s+(\d+)\s+DONE\b")
VERSION_RE = re.compile(r"\bv(\d+\.\d+\.\d+[\w.-]*)")


def release_steps():
    return [{"n": n, "owner": RELEASE_OWNERS[n],
             "text": RELEASE_TEXTS[n], "done": False,
             "evidence": ""} for n in range(1, 8)]


def release_task_text(issue, steps):
    lines = ["%s RELEASE CASE #%d opened -- consultant executes, engine tracks" % (utcnow(), issue),
             "Reply by appending `RELEASE %d STEP <n> DONE :: <evidence>` lines to this file." % issue,
             "Stall (no new DONE in 3 ticks) re-rings you (max 3), then NEEDs the operator."]
    for s in steps:
        lines.append("STEP %d [%s]: %s"
                       % (s["n"], s["owner"], s["text"].replace("<issue>", str(issue))))
    return "\n".join(lines) + "\n"


def notes_path(ctx):
    override = getattr(ctx, "notes_file", None) or os.environ.get("STATE_MACHINE_NOTES_FILE")
    if override:
        return os.path.abspath(override)
    return os.path.join(ctx.repo, "logs", "consultant-notes.md")


def notes_append(ctx, text):
    if ctx.dry:
        return "WOULD append consultant note (%dB)" % len(text)
    try:
        p = notes_path(ctx)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "a") as f:
            f.write(text.rstrip("\n") + "\n")
        return "consultant note appended (%s)" % p
    except OSError as e:
        return "NOTES-FAILED: %s" % e


def notes_progress(ctx, issue):
    done = {}
    try:
        lines = open(notes_path(ctx), errors="replace").read().splitlines()
    except OSError:
        return done
    for l in lines:
        m = DONE_RE.search(l)
        if m and int(m.group(1)) == issue:
            done[int(m.group(2))] = l[:220]
    return done


def gh_release_ok(ctx, version):
    slug = github_repo_slug(ctx)
    if not slug or not version:
        return False
    rc, _o = ctx.run(["gh", "api", "repos/%s/releases/tags/%s" % (slug, version)],
                     timeout=30)
    return rc == 0


def release_tick(ctx, issue, boxes, st):
    """Release ticket: open/task/track/remind/close. Returns (acts, rel_store).

    rel_store: None (leave) | dict (persist) | "closed" (pop + disarm).
    Full release can never be engine-alone: publish/mirror/tag stay
    operator-owned; the consultant works its steps; the engine tracks.
    """
    acts = []
    rel = st.get("release")
    armed = bool(st.get("release_armed"))
    store = None
    if armed and not rel:
        steps = release_steps()
        body = release_task_text(issue, steps)
        if ctx.dry:
            acts.append("WOULD open release case #%d + task consultant" % issue)
        else:
            acts.append(notes_append(ctx, body))
            probe = fast_probe(ctx, SESS["consultant"])
            if probe == "ABSENT":
                acts.append("NEED consultant session missing for release tasking")
            elif probe.startswith("IDLE"):
                acts.append(ring(ctx, SESS["consultant"], "", "release case #%d opened" % issue,
                                 channel="notes"))
            else:
                acts.append("consultant WORKING: release task filed, ring deferred")
        rel = {"stage": "open", "steps": steps, "reminds": 0,
               "last_progress": ctx.tick_seq}
        store = rel
    if not rel:
        return acts, store
    done = notes_progress(ctx, issue)
    changed = False
    for s in rel["steps"]:
        if not s["done"] and s["n"] in done:
            s["done"] = True
            s["evidence"] = done[s["n"]]
            changed = True
    if changed:
        rel["last_progress"] = ctx.tick_seq
        store = rel
        acts.append("release #%d progress: steps %s DONE"
                    % (issue, sorted(n for n in done if n in range(1, 8))))
    if all(s["done"] for s in rel["steps"]):
        ev = next((s["evidence"] for s in rel["steps"] if s["n"] == 2), "")
        m = VERSION_RE.search(ev or "")
        ver = ("v" + m.group(1)) if m else None
        if ctx.dry:
            acts.append("WOULD verify release %s via API + close case" % (ver or "?"))
        elif ver and gh_release_ok(ctx, ver):
            acts.append("release #%d verified %s; case closed" % (issue, ver))
            store = "closed"
        else:
            acts.append("NEED release #%d all steps DONE but API verify failed (version %s)"
                        % (issue, ver))
        return acts, store
    if ctx.tick_seq - rel.get("last_progress", ctx.tick_seq) >= 3:
        if rel.get("reminds", 0) < 3:
            if ctx.dry:
                acts.append("WOULD re-ring consultant (release stall)")
            else:
                probe = fast_probe(ctx, SESS["consultant"])
                if probe.startswith("IDLE"):
                    acts.append(ring(ctx, SESS["consultant"], "",
                                     "release #%d stalled at steps %s" % (
                                         issue, [s["n"] for s in rel["steps"] if not s["done"]]),
                                     channel="notes"))
                else:
                    acts.append("consultant not idle: stall ring deferred")
            rel["reminds"] = rel.get("reminds", 0) + 1
            store = rel
        else:
            acts.append("NEED operator: release #%d stalled (3 reminds sent)" % issue)
    return acts, store


def gh_bin():
    cand = os.environ.get("NEXIOS_GH_BIN")
    if cand:
        return cand
    if os.path.exists("/opt/homebrew/bin/gh"):
        return "/opt/homebrew/bin/gh"
    return "gh"


def gh_post_comment(ctx, issue, body):
    slug = github_repo_slug(ctx)
    if not slug:
        return False
    rc, _o = ctx.run([gh_bin(), "api",
                      "repos/%s/issues/%d/comments" % (slug, issue),
                      "-f", "body=" + body[:1800]], timeout=60)
    return rc == 0


def gh_transitions(ctx, issue, st, node, actions, halt_now, fresh=None):
    """Post-on-transition progress notes to the room's own issue (opt-in).

    Armed per room via `state <issue> gh_notes=true` (default off: a robot
    posting as the operator must be explicitly armed). One comment per event,
    templated bodies, deduped via state.json gh_posted. #410 stays human.
    Returns (acts, posted_dict_or_None).
    """
    acts = []
    if not st.get("gh_notes"):
        return acts, None
    posted = st.get("gh_posted") or {}
    wanted = {}
    phase = st.get("phase") or "?"
    if ("phase:" + phase) not in posted:
        wanted["phase:" + phase] = "phase -> %s" % phase
    if node.get("merge_ready") and not posted.get("merge"):
        wanted["merge"] = "MERGE-READY: audit APPROVED + QE READY on file"
    if halt_now and not posted.get("halt"):
        wanted["halt"] = "HALT: %s" % (st.get("pending") or "?")[:160]
    if node.get("retired") and not posted.get("retired"):
        wanted["retired"] = "lane retired and cleaned"
    if any("audit verdict handed to" in a for a in actions):
        dec_ts = [q[:20] for q in (fresh or {}).get("auditor", []) if "DECISION" in q]
        key = "verdict:%s" % (max(dec_ts) if dec_ts else (st.get("handled") or {}).get("auditor", "?"))
        if key not in posted:
            wanted[key] = "audit verdict handed to lane"
    if any("case closed" in a for a in actions) and not posted.get("release-closed"):
        wanted["release-closed"] = "release case verified + closed"
    if not wanted:
        return acts, None
    if ctx.dry:
        return (["WOULD post %d gh notes (%s)" % (len(wanted), ",".join(sorted(wanted)))], None)
    if getattr(ctx, "_gh_dead", False):
        return acts, None
    new_posts = {}
    for k in sorted(wanted):
        body = "state-machine tick #%d: #%d %s" % (ctx.tick_seq, issue, wanted[k])
        if gh_post_comment(ctx, issue, body):
            acts.append("gh note posted [%s]" % k)
            new_posts[k] = utcnow()
        else:
            if not getattr(ctx, "_gh_need", False):
                acts.append("NEED gh comments failing (auth?); progress notes paused this run")
                ctx._gh_need = True
            ctx._gh_dead = True
            break
    return acts, (new_posts or None)


QE_EXEMPT = ("test-history.txt", "graphify-out/")


def learnings_draft_text(issue, st, boxes):
    ndev = len(boxes["dev"]["all"])
    nqe = len(boxes["qe"]["all"])
    naud = len(boxes["auditor"]["all"])
    dec = auditor_decision(boxes["auditor"]["all"]) or "none on file"
    return ("# LEARNINGS DRAFT -- lane #%d retired %s (for curation; NOT the curated file)\n\n"
            "- closeout: room phase CLOSED, issue closed, `clean` rc 0.\n"
            "- effort: %s retries recorded; halt=%s.\n"
            "- audit: %s.\n"
            "- inbox volume: dev %d / qe %d / auditor %d posts (see room inbox/*.md).\n"
            "- graph: repo logs/state-machine/state-graph.json (node archived at retire tick).\n\n"
            "Promote durable traps/invariants to prompts/LEARNINGS.md as `### #%d` by hand; "
            "then delete this draft.\n"
            % (issue, utcnow(), st.get("retries", 0), st.get("halt", False),
               dec[:160], ndev, nqe, naud, issue))


PRODUCTIVE_PREFIXES = ("rang ", "wrote ", "auditor launched", "relaunch",
    "testbed staged", "state ", "clean #", "pointer ", "consultant note",
    "audit verdict handed", "checkpoint NOTE filed", "gh note posted",
    "release #", "HALT ", "learnings draft", "verdict on file", "WOULD ",
    "order filed", "WAKE ")


def cycle_productive(all_actions):
    """True if the cycle did (or, dry, would do) anything beyond passive notes."""
    return any(a.startswith(PRODUCTIVE_PREFIXES) for a in all_actions)


def cycle_progress(all_actions, room_nodes, dry=False):
    """True if the cycle shows real work (engine acts OR worker activity).

    Coordinator/coord.md lines are excluded by construction: BOXES covers
    only dev/qe/auditor and the watchdog fingerprint counts only
    FROM=(dev|qe|audit). ESCALATE/NEED alone are intentionally not progress.
    Dry mode preserves legacy exit semantics (no watchdog run, no cursor
    advance, so frozen signals must not pin quiet at 0).
    """
    if cycle_productive(all_actions):
        return True
    if dry:
        return False
    for node in room_nodes or []:
        if (node.get("verdict") or {}).get("kind") == "CHANGED":
            return True
        try:
            if int(node.get("acked") or 0) > 0:
                return True
        except (TypeError, ValueError):
            continue
    return False


def load_saved_seq(ctx):
    try:
        n = int(json.load(open(ctx.graph_path())).get("tick", 0))
        return n if n >= 0 else 0
    except (OSError, ValueError, TypeError):
        return 0


_STOP = {"flag": None}


def _request_stop(signum, frame):
    _STOP["flag"] = signum


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


def clip_text(value, width=48):
    """Short one-line render; '-' for missing, ellipsis when truncated. Pure."""
    if value is None or value == "":
        return "-"
    s = str(value).replace("\n", " ")
    return (s[:width - 1] + "\u2026") if len(s) > width else s


def summary_snapshot(issue, st, node):
    """Per-issue end-of-run row as plain data. Pure, no I/O."""
    st = st or {}
    node = node or {}
    verdict = node.get("verdict") or {}
    handled = st.get("handled") or {}
    try:
        streak = int(verdict.get("streak") or 0)
    except (TypeError, ValueError):
        streak = 0
    rings = sum(1 for v in (st.get("rings") or {}).values()
                if isinstance(v, int) and v >= 1)
    return collections.OrderedDict((
        ("issue", issue),
        ("phase", st.get("phase") or "-"),
        ("pending", st.get("pending") or "-"),
        ("verdict_kind", verdict.get("kind") or "-"),
        ("verdict_streak", streak),
        ("handled_dev", handled.get("dev") or "-"),
        ("handled_qe", handled.get("qe") or "-"),
        ("rings", rings),
        ("merge_ready", node.get("merge_ready")),
        ("halt", bool(st.get("halt"))),
    ))


class RunTracker:
    """Which issues this run is responsible for, and which retired in-run.

    done evidence is GitHub CLOSED only: an issue enters `retired` solely via
    tick_room's node["retired"], which retire_room grants only on G2
    github CLOSED. No branch/merge inspection anywhere; no I/O here.
    """

    def __init__(self, issues=None, rooms=None):
        self.fixed = set(issues) if issues else None
        self.initial_present = {iss for iss, _r, _s in (rooms or [])}
        self.seen = set()
        self.retired = set()
        self.states = collections.OrderedDict()

    def observe(self, issue, st, node):
        self.seen.add(issue)
        self.states[issue] = dict(st or {})
        if (node or {}).get("retired"):
            self.retired.add(issue)

    def tracked_issues(self):
        if self.fixed is not None:
            return set(self.fixed)
        return set(self.seen) | set(self.retired) | set(self.states)

    def summary_states(self):
        return self.states


def all_done(tracker, rooms):
    """(done_bool, detail_str): every tracked issue retired-and-gone (or
    never present), and nothing present. Pure."""
    present = {iss for iss, _rdir, _st in rooms}
    if tracker.fixed is not None:
        def ok(i):
            return ((i in tracker.retired and i not in present) or
                    (i not in tracker.initial_present))
        pending = sorted(i for i in tracker.fixed if not ok(i))
        detail = ("tracked=%d retired=%d absent-at-start=%d present=%d pending=%s"
                  % (len(tracker.fixed), len(tracker.retired),
                     len(tracker.fixed - tracker.initial_present),
                     len(present), ",".join("#%d" % i for i in pending) or "-"))
        return (not pending, detail)
    if not tracker.seen:
        return True, "no issues observed (empty tracked set)"
    if present:
        pending = sorted(tracker.seen - tracker.retired)
        return (False, "rooms present=%d pending=%s"
                % (len(present), ",".join("#%d" % i for i in pending) or "-"))
    pending = sorted(tracker.seen - tracker.retired)
    detail = ("rooms gone; seen=%d retired=%d pending=%s"
              % (len(tracker.seen), len(tracker.retired),
                 ",".join("#%d" % i for i in pending) or "-"))
    return (not pending, detail)


def build_summary(room_states, nodes, reason, tracked=None, retired=None, dry=False):
    """End-of-run summary text. Pure: dicts in, string out."""
    retired = set(retired or ())
    norm_nodes = {}
    for k, v in (nodes or {}).items():
        try:
            norm_nodes[int(k)] = v
        except (TypeError, ValueError):
            continue
    if tracked is None:
        issues = sorted(set(room_states) | set(norm_nodes) | retired)
    else:
        issues = sorted(tracked)
    n_retired = 0
    n_active = 0
    lines = []
    for issue in issues:
        if issue in retired:
            status = "retired"
            n_retired += 1
        elif issue in room_states:
            row_state = room_states[issue] or {}
            if row_state.get("halt"):
                status = "halted"
            else:
                status = "active"
                n_active += 1
        else:
            status = "absent"
        if issue in room_states:
            row = summary_snapshot(issue, room_states[issue],
                                   norm_nodes.get(issue))
            streak_n = row["verdict_streak"] or 0
            streak = (" x%d" % streak_n) if streak_n > 0 else ""
            line = ("#%d status=%s phase=%s pending=\"%s\" verdict=%s%s "
                    "handled dev=%s qe=%s rings=%s merge_ready=%s"
                    % (issue, status, row["phase"],
                       clip_text(row["pending"]),
                       row["verdict_kind"], streak,
                       row["handled_dev"], row["handled_qe"],
                       row["rings"] if status != "absent" else "-",
                       row["merge_ready"] if row["merge_ready"] is not None else "-"))
        else:
            line = ("#%d status=%s phase=- pending=\"-\" verdict=- "
                    "handled dev=- qe=- rings=- merge_ready=-"
                    % (issue, status))
        lines.append(line)
    n_absent = sum(1 for issue in issues
                   if issue not in retired and issue not in room_states)
    head = ("summary%s: %d tracked, %d active, %d retired, %d absent"
            % (" (DRY)" if dry else "", len(issues), n_active, n_retired, n_absent))
    pend = sorted(issue for issue in issues
                  if issue in room_states
                  and not (room_states[issue] or {}).get("halt"))
    foot = "stop: %s | pending=%s" % (
        reason, ",".join("#%d" % i for i in pend) or "-")
    return "\n".join([head] + lines + [foot])


def emit_summary(ctx, tracker, nodes, reason):
    """Print + log the end-of-run summary (dual pattern, no mutation)."""
    if tracker.fixed is not None:
        tracked = tracker.tracked_issues()
    else:
        tracked = None
    text = build_summary(tracker.summary_states(), nodes, reason,
                         tracked=tracked, retired=tracker.retired,
                         dry=ctx.dry)
    print(text, flush=True)
    for line in text.splitlines():
        ctx.log(line)


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
    rooms = discover_rooms(issues, skip_debris=True)
    tracker = RunTracker(issues, rooms)
    if not rooms:
        emit_summary(ctx, tracker, {},
                     "no rooms%s" % (" for issues %s" % sorted(issues) if issues else ""))
        return 2
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        try:
            signal.signal(sig, _request_stop)
        except (OSError, ValueError):
            pass
    ctx.tick_seq = load_saved_seq(ctx)
    if ctx.tick_seq:
        ctx.log("resumed seq at #%d" % ctx.tick_seq)
    stop_path = os.path.join(ctx.log_dir, "STOP")
    quiet = 0
    rc = 0
    while True:
        if _STOP["flag"] is not None:
            msg = "signal %s: stashing + exiting cleanly" % _STOP["flag"]
            print(msg, flush=True)
            ctx.log(msg)
            emit_summary(ctx, tracker, ctx.graph, msg)
            break
        if os.path.exists(stop_path):
            msg = "STOP sentinel present: stashing + exiting cleanly"
            print(msg, flush=True)
            ctx.log(msg)
            try:
                os.unlink(stop_path)
            except OSError:
                pass
            emit_summary(ctx, tracker, ctx.graph, msg)
            break
        ctx.tick_seq += 1
        ctx.log("tick #%d rooms=%s" % (ctx.tick_seq, [i for i, _, _ in rooms]))
        # G6 + S1: one sweep-wide cycle (shared rung/busy sets) and a
        # per-cycle engine lock, so a fast-path `tick --issues N --once`
        # is safe while the 180 s loop sleeps and a no-op mid-cycle.
        # The lock is never held across sleep(interval) below.
        cycle = {"rung": set(), "busy": set()}
        _lf = None
        if not ctx.dry:
            # Live sweep only: dry runs never write state, so they take no
            # lock (S3); the lock is never held across sleep(interval).
            try:
                _lf = open(os.path.join(ctx.log_dir, "engine.lock"), "a+")
                fcntl.flock(_lf, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                _lf = None
            if _lf is None:
                _msg = "cycle skipped: another engine tick holds the lock"
                print(_msg, flush=True)
                ctx.log(_msg)
                if not interval:
                    return 0
                time.sleep(interval)
                rooms = discover_rooms(issues, skip_debris=True)
                continue
        try:
            cycle_actions = []
            cycle_nodes = []
            for issue, rdir, st in rooms:
                node, rows = tick_room(ctx, issue, rdir, st, rooms, cycle)
                tracker.observe(issue, load_state_locked(rdir) or st, node)
                table = fmt_table(issue, node.get("verdict", {}), rows)
                print(table, flush=True)
                ctx.log("issue #%d verdict=%s acked=%d merge_ready=%s" % (
                    issue, node.get("verdict", {}).get("kind"),
                    node.get("acked", 0), node.get("merge_ready")))
                for a in node.get("actions", []):
                    ctx.log("#%d :: %s" % (issue, a))
                    cycle_actions.append(a)
                cycle_nodes.append(node)
                # Shared sessions ring once per cycle: collect this room's
                # doorbells NOW so the next room's session_rung sees them
                # intra-sweep (S3), not just post-sweep.
                for _a in node.get("actions", []):
                    for _sess in (SESS["dev"], SESS["qe"]):
                        if _a.startswith("rang " + _sess) or \
                                _a.startswith("WOULD ring " + _sess):
                            cycle["rung"].add(_sess)
            ctx.log(write_graph(ctx))
        finally:
            if _lf is not None:
                try:
                    fcntl.flock(_lf, fcntl.LOCK_UN)
                    _lf.close()
                except OSError:
                    pass
        if not interval:
            emit_summary(ctx, tracker, ctx.graph, "once (single cycle, interval 0)")
            break
        if cycle_progress(cycle_actions, cycle_nodes, dry=ctx.dry):
            if quiet > 0:
                ctx.log("quiet reset after %d quiet cycle(s)" % quiet)
            quiet = 0
        else:
            quiet += 1
            ctx.log("quiet %d/3 (no progress)" % quiet)
            if quiet >= 3:
                msg = "all quiet for 3 cycles: no work pending, ending cleanly"
                print(msg, flush=True)
                ctx.log(msg)
                emit_summary(ctx, tracker, ctx.graph, msg)
                break
        time.sleep(interval)
        rooms = discover_rooms(issues, skip_debris=True)
        done, detail = all_done(tracker, rooms)
        if done:
            emit_summary(ctx, tracker, ctx.graph, "all-done (%s)" % detail)
            break
    return rc


def status_planned_lane(role, box, st, probe):
    """Next-step read for one worker lane. Pure, read-only (no rings)."""
    if (st or {}).get("halt"):
        return "HALTED: no actions"
    box = box or {}
    posts = box.get("all", [])
    coord_last = box.get("coord_last")
    if coord_last and order_outstanding(
            role, coord_last, posts, (st or {}).get("rings") or {},
            (st or {}).get("ack_since")):
        return "ring: unacked order"
    nxt = box.get("next") or ""
    if re.search(RESUME_RE, nxt, re.I):
        return "ring: resume per NEXT"
    if nxt:
        return "awaits: %s" % clip_text(nxt, 42)
    pending = (st or {}).get("pending") or ""
    if pending:
        return "pending: %s" % clip_text(pending, 42)
    if str(probe).startswith("IDLE"):
        return "NEED idle: no briefed work"
    return "-"


def status_planned_audit(ctx, issue, boxes):
    """Next-step read for the audit lane. Read-only (existing readers)."""
    if auditor_decision((boxes.get("auditor") or {}).get("all", [])):
        return "verdict on file"
    if audit_session_live(ctx):
        return "awaiting verdict"
    if latest_audit_report(ctx, issue):
        return "report on file"
    return "collect verdict"


def cmd_status(ctx, issues):
    rooms = discover_rooms(issues)
    if not rooms:
        print("no rooms")
        return 2
    facts = {"dev": lane_facts_all(rooms, "dev"),
             "qe": lane_facts_all(rooms, "qe")}
    for issue, rdir, st in rooms:
        boxes = load_boxes(rdir)
        rows = []
        for role, sess in (("dev", SESS["dev"]), ("qe", SESS["qe"]),
                           ("audit", SESS["audit"])):
            if role == "audit":
                rep = latest_audit_report(ctx, issue)
                activity = ("report %s" % os.path.basename(rep)) if rep else (
                    auditor_decision(boxes["auditor"]["all"]) or "no verdict")
                state = audit_state(audit_session_live(ctx))
                rows.append((role, state, activity,
                             status_planned_audit(ctx, issue, boxes)))
            else:
                probe = fast_probe(ctx, sess)
                activity = boxes[role]["state"] or boxes[role]["next"] or "-"
                rows.append((role, lane_state(probe, facts[role].get(issue, {}),
                                             facts[role], issue), activity,
                             status_planned_lane(role, boxes[role], st, probe)))
        print(fmt_table(issue, {"kind": "STATUS", "rest": (st.get("pending") or "")[:60]},
                        rows), flush=True)
    return 0


def canon_issue(raw):
    """Canonical issue int like runtime_dir: numeric, no leading zeros, 1..7 digits."""
    s = str(raw).strip()
    if not s.isdigit():
        return None
    n = s.lstrip("0") or "0"
    if n == "0" or len(n) > 7:
        return None
    return int(n)


def github_issue_text(ctx, issue):
    """(title, body) for an issue; (None, None) on any failure (never invent)."""
    seam = os.environ.get("STATE_MACHINE_GH_JSON")
    if seam:
        try:
            d = json.load(open(seam))
            return d.get("title", ""), d.get("body", "")
        except (OSError, ValueError):
            return None, None
    slug = github_repo_slug(ctx)
    if not slug:
        return None, None
    rc, out = ctx.run(["gh", "api", "repos/%s/issues/%d" % (slug, issue)],
                      timeout=30)
    if rc != 0:
        return None, None
    try:
        d = json.loads(out)
        return d.get("title", ""), d.get("body", "")
    except ValueError:
        return None, None


def issue_briefs_text(issue, dev_branch, title, body):
    """RECON brief pair for one issue. Pure: no I/O, {repo} stays literal."""
    seed = "%s\n\n%s" % (title, (body or "").strip())
    dev_brief = ("%s\n\nIssue: %d\nRole prompt: {repo}/prompts/PROMPT-dev.md\n"
                 "Branch: %s (recon first; feature branch at impl)\n"
                 "Task: RECON phase (read-only, no branch, no code): read the issue "
                 "text above + relevant code/tests; draft the implementation plan; "
                 "post PLAN NOTE to inbox/dev.md + RING (every post ends with STATE/NEXT plus a CTX: <n>%% line). "
                 "Every engine order carries its id ([oid <id>] in the ring or coord NOTE); ACK it in your next post via post_update ref=<id> (a post without ref is FYI only and never clears an order). "
                 "No branches, no code, "
                 "no QEMU until the plan is audit-approved.\n"
                 "scripts/agent_up.sh wake %d dev nexios-engine \"inbox: dev READY \u2014 read inbox/dev.md\"\n"
                 % (seed, issue, dev_branch, issue))
    qe_brief = ("%s\n\nIssue: %d\nRole prompt: {repo}/prompts/PROMPT-testdev.md\n"
                "Branch: none (read-only; testbed stays clean)\n"
                "Task: parallel RECON (read-only, no branch, no code): draft the "
                "verification approach for this issue; post NOTE to inbox/qe.md + "
                "RING (every post ends with STATE/NEXT plus a CTX: <n>%% line). "
                "Every engine order carries its id ([oid <id>] in the ring or coord NOTE); ACK it in your next post via post_update ref=<id> (a post without ref is FYI only and never clears an order). "
                "RING. No test runs until dev posts IMPL READY.\n"
                "scripts/agent_up.sh wake %d qe nexios-engine \"inbox: qe READY \u2014 read inbox/qe.md\"\n"
                % (seed, issue, issue))
    return dev_brief, qe_brief


def cmd_provision(ctx, issues, a):
    """Cold start per issue: setup + issue-text briefs + sessions + state.

    Never invents: no issue text -> NEED, stop. setup is the idempotent
    creator. `workers` (not `up`/`coord`: no GUI windows, no attach).
    """
    if not issues:
        print("provision needs --issues (cold start from the issue list)")
        return 2
    rc_all = 0
    for issue in sorted(issues):
        tag = "#%d" % issue
        rdir = "/tmp/nexios-%d" % issue
        if ctx.dry:
            ctx.log("WOULD setup %s" % tag)
        else:
            rc, out = ctx.run([ctx.agent_up, "setup", str(issue)], timeout=120)
            tail = (out.strip().splitlines() or ["?"])[-1][:120]
            ctx.log("%s setup rc=%d %s" % (tag, rc, tail))
            if rc != 0:
                ctx.log("%s NEED setup failed; stop" % tag)
                rc_all = 1
                continue
        title, body = github_issue_text(ctx, issue)
        if title is None:
            ctx.log("%s NEED issue text unavailable (no invention); stop" % tag)
            rc_all = 1
            continue
        dev_brief, qe_brief = issue_briefs_text(issue, a.dev_branch, title, body)
        if ctx.dry:
            ctx.log("%s WOULD write prompts/dev.txt (%dB) + prompts/qe.txt (%dB)"
                    % (tag, len(dev_brief), len(qe_brief)))
        else:
            ctx.log("%s %s" % (tag, write_brief(ctx, rdir, "dev.txt", dev_brief)))
            ctx.log("%s %s" % (tag, write_brief(ctx, rdir, "qe.txt", qe_brief)))
        if a.no_launch:
            ctx.log("%s --no-launch: sessions untouched" % tag)
        elif ctx.dry:
            ctx.log("%s WOULD run: workers %d (normalize may auto-stash)" % (tag, issue))
        else:
            rc, out = ctx.run([ctx.agent_up, "workers", str(issue)], timeout=300)
            tail = (out.strip().splitlines() or ["?"])[-2:]
            ctx.log("%s workers rc=%d %s" % (tag, rc, " // ".join(tail)))
            if rc != 0:
                ctx.log("%s NEED workers launch failed; stop" % tag)
                rc_all = 1
                continue
        ctx.log("%s %s" % (tag, set_state_key(
            ctx, issue, "pending",
            "provisioned from issue text; await dev PLAN + qe approach NOTEs")))
    if rc_all == 0 and not a.no_launch:
        if ctx.dry:
            ctx.log("WOULD ensure consultant session + open consultant/dev/qe windows")
        else:
            rc, out = ctx.run([ctx.agent_up, "consult"], timeout=180)
            tail = (out.strip().splitlines() or ["?"])[-1][:100]
            ctx.log("consult rc=%d %s" % (rc, tail))
            if rc == 0:
                rc2, out2 = ctx.run([ctx.agent_up, "windows"], timeout=60)
                last2 = (out2.strip().splitlines() or ["?"])[-1][:100]
                ctx.log("windows rc=%d %s" % (rc2, last2))
    return rc_all


def cmd_brief(ctx, issues, a):
    """Write per-room RECON briefs from GitHub issue text. All-or-nothing:
    every text fetched before any write; any miss aborts with rc 1."""
    if ctx.dry:
        for issue in issues:
            title, _body = github_issue_text(ctx, issue)
            if title is None:
                print("#%d NEED issue text unavailable (no invention); stop" % issue)
                return 1
            dev_brief, qe_brief = issue_briefs_text(
                issue, a.dev_branch, title, _body)
            print("WOULD write #%d prompts/dev.txt (%dB) + prompts/qe.txt (%dB)"
                  % (issue, len(dev_brief), len(qe_brief)))
        return 0
    for issue in issues:
        if not os.path.isdir("/tmp/nexios-%d/prompts" % issue):
            sys.stderr.write("#%d NEED room runtime missing (run setup first); stop\n" % issue)
            return 1
    texts = {}
    for issue in issues:
        title, body = github_issue_text(ctx, issue)
        if title is None:
            sys.stderr.write("#%d NEED issue text unavailable (no invention); stop\n" % issue)
            return 1
        texts[issue] = issue_briefs_text(issue, a.dev_branch, title, body)
    rc = 0
    for issue in issues:
        dev_brief, qe_brief = texts[issue]
        rdir = "/tmp/nexios-%d" % issue
        for name, content in (("dev.txt", dev_brief), ("qe.txt", qe_brief)):
            res = write_brief(ctx, rdir, name, content)
            print("#%d %s" % (issue, res))
            if res.startswith("BRIEF-FAILED"):
                sys.stderr.write("#%d NEED brief write failed: %s\n" % (issue, res))
                rc = 1
    return rc


def mode_self_test():
    """Deterministic regression: #442 quiet-progress + NEED-idle, #443 peer handoff,
    #456 room-scoped rings, WAKE ingestion, acked backlog/qe orders,
    #457 lane STATE with actual issue + auditor WORKING/-."""
    passed = 0
    total = 0

    def check(name, got, want=True):
        nonlocal passed, total
        total += 1
        ok = bool(got) is bool(want)
        print("%s %s" % ("PASS" if ok else "FAIL", name), flush=True)
        if ok:
            passed += 1
        return ok

    mk = lambda kind, acked: {"verdict": {"kind": kind}, "acked": acked}
    check("progress-empty-false", cycle_progress([], [], dry=False), False)
    check("progress-unchanged-0-false",
          cycle_progress(["NOTE idle"], [mk("UNCHANGED", 0)], dry=False), False)
    check("progress-rang-true",
          cycle_progress(["rang nexios-dev"], [mk("UNCHANGED", 0)], dry=False), True)
    check("progress-changed-true",
          cycle_progress(["NOTE idle"], [mk("CHANGED", 0)], dry=False), True)
    check("progress-acked-true",
          cycle_progress(["NEED qe idle with no briefed work (no invention)"],
                         [mk("UNCHANGED", 2)], dry=False), True)
    check("progress-dry-signals-false",
          cycle_progress([], [mk("CHANGED", 1)], dry=True), False)
    check("progress-dry-rang-true",
          cycle_progress(["WOULD ring nexios-dev"], [mk("UNCHANGED", 0)], dry=True), True)
    check("progress-order-filed-productive",
          cycle_progress(["order filed qe IMPL READY (oid qe:2026-11-02T08:00:00)"],
                         [mk("UNCHANGED", 0)], dry=False), True)
    check("progress-wake-productive",
          cycle_progress(["WAKE dev 2026-11-02T19:21:27 inbox: dev READY"],
                         [mk("UNCHANGED", 0)], dry=False), True)
    check("need-idle-true",
          need_idle_missing("dev", set(), "IDLE-static", [], None, False), True)
    check("need-idle-fresh-false",
          need_idle_missing("dev", set(), "IDLE-static", ["2026-11-02T07:44:32Z FROM=qe NOTE :: x"],
                            None, False), False)
    check("need-idle-working-false",
          need_idle_missing("dev", set(), "WORKING-markers", [], None, False), False)
    check("need-idle-next-false",
          need_idle_missing("dev", set(), "IDLE-static", [], "working :: parked", False), False)
    check("need-idle-unacked-false",
          need_idle_missing("dev", {"dev"}, "IDLE-static", [], None, False), False)
    check("need-idle-rang-false",
          need_idle_missing("dev", set(), "IDLE-static", [], None, True), False)
    check("parse-changed", parse_verdict("CHANGED 51 (8 lines differ)")["kind"] == "CHANGED", True)
    check("wire-gaps-state",
          wire_gaps(["2026-11-02T07:44:32Z FROM=qe NOTE :: x", "body"], "2026-11-02T07:44:32Z FROM=qe NOTE :: x") == ["STATE", "NEXT"], True)
    fresh_first = new_posts({"dev": {"all": ["2026-11-02T07:44:32Z FROM=dev NOTE :: x"]}}, {})
    check("new-posts-first-sight-empty", fresh_first["dev"] == [], True)
    dev_ready = "2026-11-02T08:03:09Z FROM=dev READY :: step-b IMPL READY QE: execute this"
    qe_note = "2026-11-02T07:44:32Z FROM=qe NOTE :: parked on dev IMPL READY"
    from types import SimpleNamespace
    check("peer-mention-true", peer_mention("QE: execute verification", "qe"), True)
    check("peer-mention-case-insensitive", peer_mention("Qe: x", "qe"), True)
    check("peer-mention-substring-false", peer_mention("sequencer", "qe"), False)
    check("peer-mention-dev-substring-false", peer_mention("device driver", "dev"), False)
    b_empty_qe = {"dev": {"all": [dev_ready]}, "qe": {"all": []}}
    off = peer_ready_offers(b_empty_qe, "qe")
    check("peer-offer-ready-detected", len(off) == 1, True)
    check("peer-offer-oid", off and off[0]["oid"] == "peer:qe:2026-11-02T08:03:09", True)
    for kind in ("NOTE", "NEED", "DECISION"):
        b_kind = {"dev": {"all": [dev_ready.replace("READY", kind)]}, "qe": {"all": []}}
        check("peer-offer-%s-ignored" % kind.lower(), peer_ready_offers(b_kind, "qe") == [], True)
    b_self = {"dev": {"all": [dev_ready.replace("QE:", "dev:")]}, "qe": {"all": []}}
    check("peer-offer-self-sender-ignored", peer_ready_offers(b_self, "qe") == [], True)
    b_answered = {"dev": {"all": [dev_ready]},
                  "qe": {"all": ["2026-11-02T08:39:31Z FROM=qe NEED :: looking"]}}
    check("peer-offer-answered-empty", peer_ready_offers(b_answered, "qe") == [], True)
    b_unanswered = {"dev": {"all": [dev_ready]}, "qe": {"all": [qe_note]}}
    check("peer-offer-unanswered-kept", len(peer_ready_offers(b_unanswered, "qe")) == 1, True)
    dev_ready2 = "2026-11-02T08:20:00Z FROM=dev READY :: update QE: recheck"
    b_two = {"dev": {"all": [dev_ready, dev_ready2]}, "qe": {"all": []}}
    off2 = peer_ready_offers(b_two, "qe")
    check("peer-offer-newest-first", off2 and off2[0]["ts"] == "2026-11-02T08:20:00", True)
    check("peer-pick-none", peer_handoff_pick("qe", [], {}, "") is None, True)
    pk = peer_handoff_pick("qe", off, {}, "")
    check("peer-pick-new", pk and pk["oid"] == off[0]["oid"] and pk["rung"] is False, True)
    rings_pend = {"peer:qe:2026-11-02T08:03:09": 1}
    pk2 = peer_handoff_pick("qe", off2, rings_pend, "")
    check("peer-pick-pending-wins",
          pk2 and pk2["oid"] == "peer:qe:2026-11-02T08:03:09" and pk2["rung"] is True, True)
    pk3 = peer_handoff_pick("qe", off2, rings_pend, "2026-11-02T08:10:00")
    check("peer-pick-answered-pending-skipped",
          pk3 and pk3["oid"] == "peer:qe:2026-11-02T08:20:00" and pk3["rung"] is False, True)
    check("peer-pick-superseded-sender-resolved", pk2 and pk2["sender"] == "dev", True)
    rings_done = {"peer:qe:2026-11-02T08:03:09": 1}
    b_resp = {"dev": {"all": [dev_ready]},
              "qe": {"all": ["2026-11-02T08:39:31Z FROM=qe NEED :: looking"]}}
    check("peer-done-oid",
          peer_handoff_done_oids(b_resp, "qe", rings_done) == ["peer:qe:2026-11-02T08:03:09"], True)
    b_old = {"dev": {"all": [dev_ready]}, "qe": {"all": [qe_note]}}
    check("peer-done-not-when-peer-older", peer_handoff_done_oids(b_old, "qe", rings_done) == [], True)
    check("peer-done-namespace-isolated",
          peer_handoff_done_oids(b_resp, "qe", {"qe:x": 1, "next:qe:abc": 1}) == [], True)
    check("peer-gate-ring-idle",
          peer_handoff_action(pk, "IDLE-static", False, False, False) == "ring", True)
    check("peer-gate-hold-working",
          peer_handoff_action(pk, "WORKING-markers", False, False, False) == "hold", True)
    check("peer-gate-hold-nonidle",
          peer_handoff_action(pk, "BLOCKED-modal", False, False, False) == "hold", True)
    check("peer-gate-skip-deaf",
          peer_handoff_action(pk, "IDLE-static", True, False, False) == "skip", True)
    pk_rung = {"sender": "dev", "ts": "2026-11-02T08:03:09",
               "oid": "peer:qe:2026-11-02T08:03:09", "post": dev_ready, "rung": True}
    check("peer-gate-skip-rung",
          peer_handoff_action(pk_rung, "IDLE-static", False, False, False) == "skip", True)
    check("peer-gate-skip-already-rang",
          peer_handoff_action(pk, "IDLE-static", False, True, False) == "skip", True)
    check("peer-gate-skip-recreating",
          peer_handoff_action(pk, "IDLE-static", False, False, True) == "skip", True)
    check("peer-gate-none", peer_handoff_action(None, "IDLE-static", False, False, False) == "none", True)
    check("rang-session-live", rang_session(["rang nexios-qe"], "nexios-qe"), True)
    check("rang-session-dry", rang_session(["WOULD ring nexios-qe: x"], "nexios-qe"), True)
    check("rang-session-false", rang_session(["NOTE idle"], "nexios-qe"), False)
    dry_ctx = SimpleNamespace(dry=True)
    rp = ring_peer_handoff(dry_ctx, "qe", "dev", "2026-11-02T08:03:09",
                            "peer:qe:2026-11-02T08:03:09")
    check("peer-ring-points-at-sender-box",
          "WOULD ring nexios-qe" in rp and rp.rstrip().endswith("read inbox/dev.md"), True)
    check("recreating-peers",
          recreating_peers(["WOULD kill nexios-qe x"]) == {"qe"}, True)
    check("recreating-peers-negative",
          recreating_peers(["NEED recreate nexios-qe x"]) == set(), True)
    st51 = {"phase": "IN_PROGRESS", "pending": "dev step-b",
            "handled": {"dev": "2026-11-02T08:03:09Z", "qe": "2026-11-02T08:39:31Z"},
            "rings": {"peer:dev:2026-11-02T08:39:31": 1, "qe:x": 0, "z": 2}}
    nd51 = {"verdict": {"kind": "UNCHANGED", "streak": 2}, "merge_ready": False}
    snap = summary_snapshot(51, st51, nd51)
    check("summary-row-active",
          snap["phase"] == "IN_PROGRESS" and snap["verdict_kind"] == "UNCHANGED"
          and snap["verdict_streak"] == 2 and snap["handled_dev"] == "2026-11-02T08:03:09Z"
          and snap["handled_qe"] == "2026-11-02T08:39:31Z" and snap["rings"] == 2
          and snap["merge_ready"] is False, True)
    check("summary-rings-outstanding",
          summary_snapshot(1, {"rings": {"a": -1, "b": 0, "c": 2}}, {})["rings"] == 1, True)
    check("summary-rings-none",
          summary_snapshot(1, {"rings": {"a": -1, "b": 0}}, {})["rings"] == 0, True)
    check("summary-clip", clip_text("x" * 80) == "x" * 47 + "\u2026", True)
    check("summary-clip-short", clip_text("abc") == "abc", True)
    check("summary-clip-missing", clip_text(None) == "-" and clip_text("") == "-", True)
    txt = build_summary({51: st51}, {"51": nd51}, "quiet exit",
                        tracked={51}, retired=set(), dry=False)
    check("summary-row-format",
          '#51 status=active phase=IN_PROGRESS pending="dev step-b" verdict=UNCHANGED x2' in txt
          and "handled dev=2026-11-02T08:03:09Z qe=2026-11-02T08:39:31Z rings=2" in txt
          and "merge_ready=False" in txt, True)
    check("summary-header", txt.splitlines()[0].startswith("summary: 1 tracked"), True)
    check("summary-stop-pending", txt.splitlines()[-1] == "stop: quiet exit | pending=#51", True)
    txt0 = build_summary({51: st51}, {"51": {"verdict": {"kind": "UNCHANGED"}}}, "x",
                         tracked={51}, retired=set(), dry=False)
    check("summary-verdict-streak-zero", "verdict=UNCHANGED " in txt0 and " x0" not in txt0, True)
    txt1 = build_summary({51: st51}, {}, "x", tracked={51}, retired=set(), dry=False)
    check("summary-verdict-unknown", "verdict=- " in txt1, True)
    txt2 = build_summary({51: dict(st51, pending="y" * 80)}, {"51": nd51}, "x",
                         tracked={51}, retired=set(), dry=False)
    check("summary-pending-truncated", "\u2026" in txt2, True)
    txt3 = build_summary({51: st51}, {"51": nd51}, "x", tracked={51}, retired={51}, dry=False)
    check("summary-retired-row", "#51 status=retired" in txt3, True)
    txt4 = build_summary({51: st51}, {"51": nd51}, "x", tracked={51, 52}, retired=set(), dry=False)
    check("summary-absent-row",
          '#52 status=absent phase=- pending="-" verdict=- handled dev=- qe=- rings=- merge_ready=-' in txt4,
          True)
    txt5 = build_summary({51: dict(st51, halt=True)}, {"51": nd51}, "x",
                         tracked={51}, retired=set(), dry=False)
    check("summary-halted-row", "#51 status=halted" in txt5
          and txt5.splitlines()[-1] == "stop: x | pending=-", True)
    txt6 = build_summary({51: st51}, {"51": {}}, "x", tracked={51}, retired=set(), dry=False)
    check("summary-merge-unknown", "merge_ready=-" in txt6, True)
    check("summary-line-count", len(txt.splitlines()) == 3, True)
    txtd = build_summary({51: st51}, {"51": nd51}, "x", tracked={51}, retired=set(), dry=True)
    check("summary-dry-marked",
          txtd.splitlines()[0].startswith("summary (DRY):")
          and txtd.splitlines()[1:] == txt.splitlines()[1:][:-1] + [txtd.splitlines()[-1]], True)
    txte = build_summary({}, {}, "x", tracked=set(), retired=set(), dry=False)
    check("summary-empty", txte.splitlines()[0] == "summary: 0 tracked, 0 active, 0 retired, 0 absent", True)
    tr = RunTracker({51, 52}, [(51, "/r51", {}), (52, "/r52", {})])
    tr.observe(51, {"phase": "CLOSED"}, {"retired": True})
    tr.observe(52, {"phase": "CLOSED"}, {"retired": True})
    check("done-fixed-all-retired", all_done(tr, [])[0] is True, True)
    tr2 = RunTracker({51, 52}, [(51, "/r51", {})])
    tr2.observe(51, {"phase": "CLOSED"}, {"retired": True})
    check("done-fixed-absent-at-start", all_done(tr2, [])[0] is True, True)
    tr3 = RunTracker({51}, [(51, "/r51", {})])
    tr3.observe(51, {"phase": "IN_PROGRESS"}, {})
    check("done-fixed-vanished-unretired", all_done(tr3, [])[0] is False, True)
    tr4 = RunTracker({51}, [(51, "/r51", {})])
    tr4.observe(51, {"phase": "IN_PROGRESS"}, {})
    check("done-fixed-present-blocks", all_done(tr4, [(51, "/r51", {})])[0] is False, True)
    tr5 = RunTracker({51}, [(51, "/r51", {})])
    tr5.observe(51, {"phase": "CLOSED"}, {"retired": True})
    check("done-fixed-retired-but-present", all_done(tr5, [(51, "/r51", {})])[0] is False, True)
    tr6 = RunTracker(None, [])
    check("done-all-empty", all_done(tr6, [])[0] is True, True)
    tr7 = RunTracker(None, [])
    tr7.observe(51, {"phase": "CLOSED"}, {"retired": True})
    tr7.observe(52, {"phase": "CLOSED"}, {"retired": True})
    check("done-all-all-retired", all_done(tr7, [])[0] is True, True)
    tr8 = RunTracker(None, [])
    tr8.observe(51, {"phase": "CLOSED"}, {"retired": True})
    check("done-all-new-room-midrun", all_done(tr8, [(52, "/r52", {})])[0] is False, True)
    tr9 = RunTracker(None, [])
    tr9.observe(51, {"phase": "CLOSED"}, {"retired": True})
    tr9.observe(52, {"phase": "IN_PROGRESS"}, {})
    check("done-all-unretired-gone", all_done(tr9, [])[0] is False, True)
    trA = RunTracker({51}, [(51, "/r51", {})])
    trA.observe(51, {"phase": "CLOSED"}, {"retired": True})
    check("tracker-observe-retired",
          51 in trA.retired and trA.states[51] == {"phase": "CLOSED"}, True)
    check("tracker-tracked-fixed", RunTracker({51}, []).tracked_issues() == {51}, True)
    b_idle = {"all": [], "coord_last": None, "state": None,
              "next": None, "ctx": None}
    check("status-plan-unacked",
          status_planned_lane("qe", {"all": ["2026-11-02T07:44:32Z FROM=qe NOTE :: x"],
                                     "coord_last": "2026-11-02T08:00:00Z FROM=coord NOTE :: order",
                                     "state": None, "next": None, "ctx": None},
                              {"pending": "p"}, "IDLE-static") == "ring: unacked order", True)
    check("status-plan-acked-order",
          status_planned_lane("qe", {"all": ["2026-11-02T08:05:00Z FROM=qe NOTE :: x"],
                                     "coord_last": "2026-11-02T08:00:00Z FROM=coord NOTE :: order",
                                     "state": None, "next": None, "ctx": None},
                              {"pending": "p"}, "IDLE-static") == "pending: p", True)
    check("status-plan-resume",
          status_planned_lane("dev", dict(b_idle, next="parked; on re-ring continue"),
                              {"pending": "p"}, "IDLE-static") == "ring: resume per NEXT", True)
    check("status-plan-resume-shared-regex",
          status_planned_lane("dev", dict(b_idle, next="hold for re-ring"),
                              {}, "IDLE-static") == "ring: resume per NEXT"
          and re.search(RESUME_RE, "awaiting parked, re-ring now", re.I) is not None, True)
    check("status-plan-awaits-next",
          status_planned_lane("dev", dict(b_idle, next="working :: parked"),
                              {"pending": "p"}, "IDLE-static") == "awaits: working :: parked", True)
    check("status-plan-pending",
          status_planned_lane("dev", b_idle, {"pending": "provisioned from issue text"},
                              "WORKING-markers") == "pending: provisioned from issue text", True)
    check("status-plan-need-idle",
          status_planned_lane("dev", b_idle, {}, "IDLE-static") == "NEED idle: no briefed work", True)
    check("status-plan-working-bare",
          status_planned_lane("dev", b_idle, {}, "WORKING-markers") == "-", True)
    check("status-plan-halted",
          status_planned_lane("dev", dict(b_idle, next="x"), {"halt": True},
                              "IDLE-static") == "HALTED: no actions", True)
    check("first-due-virgin-idle",
          first_contact_due("dev", False, True, "IDLE-static", False, {}) == (True, "first:dev"), True)
    check("first-due-contacted",
          first_contact_due("dev", True, True, "IDLE-static", False, {}) == (False, None), True)
    check("first-due-no-brief",
          first_contact_due("dev", False, False, "IDLE-static", False, {}) == (False, None), True)
    check("first-due-working",
          first_contact_due("dev", False, True, "WORKING-markers", False, {}) == (False, None), True)
    check("first-due-deaf",
          first_contact_due("dev", False, True, "IDLE-static", True, {}) == (False, None), True)
    check("first-due-rung-once",
          first_contact_due("dev", False, True, "IDLE-static", False,
                            {"first:dev": 1}) == (False, None), True)
    check("first-due-namespace-isolated",
          first_contact_due("qe", False, True, "IDLE-static", False,
                            {"first:dev": 1}) == (True, "first:qe"), True)
    check("ring-line-legacy",
          ring_line("dev", "first brief") == "inbox: first brief -- read inbox/dev.md", True)
    check("ring-line-oid",
          ring_line("dev", "x", oid="dev:2026-11-02T08:00:00") == "inbox: x [oid dev:2026-11-02T08:00:00] -- read inbox/dev.md", True)
    check("ring-line-scoped",
          ring_line("dev", "x", issue=240) == "inbox [#240/dev]: x -- read inbox/dev.md", True)
    check("ring-line-scoped-oid",
          ring_line("qe", "y", oid="qe:2026-11-02T08:00:00", issue=54) == "inbox [#54/qe]: y [oid qe:2026-11-02T08:00:00] -- read inbox/qe.md", True)
    check("ring-line-notes-unchanged",
          ring(SimpleNamespace(dry=True), "s", "", "t", channel="notes") == "WOULD ring s: notes: t -- read logs/consultant-notes.md", True)
    _w1 = "2026-11-02T18:46:59Z FROM=dev WAKE :: inbox: dev READY"
    _w2 = "2026-11-02T19:21:27Z FROM=qe WAKE :: inbox: qe READY"
    check("parse-wakes-header-ignored",
          parse_wakes(["# inbox/coord.md append-only"]) == ([], None), True)
    _pw, _pn = parse_wakes([_w1])
    check("parse-wakes-fields",
          len(_pw) == 1 and _pw[0]["ts"] == "2026-11-02T18:46:59"
          and _pw[0]["role"] == "dev" and _pn == "2026-11-02T18:46:59", True)
    check("parse-wakes-since-strict",
          parse_wakes([_w1], "2026-11-02T18:46:59") == ([], "2026-11-02T18:46:59"), True)
    _pw2, _pn2 = parse_wakes([_w1, _w2], "2026-11-02T18:46:59")
    check("parse-wakes-newest-advances",
          len(_pw2) == 1 and _pw2[0]["role"] == "qe" and _pn2 == "2026-11-02T19:21:27", True)
    check("parse-wakes-malformed-ignored",
          parse_wakes(["FROM=dev WAKE :: x", "2026-11-02 GARBLE"]) == ([], None), True)
    _pa = "2026-11-02T08:00:00Z FROM=dev NOTE :: a"
    _pb = "2026-11-02T09:00:00Z FROM=dev NOTE :: b"
    check("resurface-last-at-or-before",
          resurface_posts([_pa, _pb], "2026-11-02T08:30:00") == [_pa], True)
    check("resurface-none-without-wake",
          resurface_posts([_pa, _pb], None) == [], True)
    check("resurface-picks-last-only",
          resurface_posts([_pa, _pb], "2026-11-02T09:00:00") == [_pb], True)
    _rdy = "2026-11-02T08:03:09Z FROM=dev READY :: step-b IMPL READY QE: x"
    check("impl-ready-detected",
          dev_impl_ready_ts([_rdy]) == "2026-11-02T08:03:09Z", True)
    check("impl-ready-case-insensitive",
          dev_impl_ready_ts(["2026-11-02T08:03:09Z FROM=dev READY :: impl ready"]) == "2026-11-02T08:03:09Z", True)
    check("impl-ready-note-ignored",
          dev_impl_ready_ts(["2026-11-02T08:03:09Z FROM=dev NOTE :: IMPL plan"]) is None, True)
    check("impl-ready-without-impl",
          dev_impl_ready_ts(["2026-11-02T08:03:09Z FROM=dev READY :: parked"]) is None, True)
    check("impl-ready-newest",
          dev_impl_ready_ts([_rdy, "2026-11-02T08:20:00Z FROM=dev READY :: IMPL done"]) == "2026-11-02T08:20:00Z", True)
    check("impl-ready-wrong-sender-ignored",
          dev_impl_ready_ts(["2026-11-02T08:03:09Z FROM=qe READY :: IMPL done"]) is None, True)
    _idle = dict(role="dev", probe="IDLE-static", deaf_lanes=set(), unacked_lanes=set(),
                 fresh_posts=[], declared_next=None, rang=False, recreating=False,
                 pending="provisioned from issue text")
    check("backlog-idle-due-true", backlog_idle_due(**_idle), True)
    check("backlog-idle-due-working",
          backlog_idle_due(**dict(_idle, probe="WORKING-markers")) is False, True)
    check("backlog-idle-due-deaf",
          backlog_idle_due(**dict(_idle, deaf_lanes={"dev"})) is False, True)
    check("backlog-idle-due-unacked",
          backlog_idle_due(**dict(_idle, unacked_lanes={"dev"})) is False, True)
    check("backlog-idle-due-fresh",
          backlog_idle_due(**dict(_idle, fresh_posts=["x"])) is False, True)
    check("backlog-idle-due-next",
          backlog_idle_due(**dict(_idle, declared_next="working :: parked")) is False, True)
    check("backlog-idle-due-rang",
          backlog_idle_due(**dict(_idle, rang=True)) is False, True)
    check("backlog-idle-due-recreating",
          backlog_idle_due(**dict(_idle, recreating=True)) is False, True)
    check("backlog-idle-due-pending-names-dev",
          backlog_idle_due(**dict(_idle, pending="await dev PLAN")) is False, True)
    check("qe-order-due-true", qe_order_due(None, "2026-11-02T08:03:09Z"), True)
    check("qe-order-due-false",
          qe_order_due("2026-11-02T08:03:09Z", "2026-11-02T08:03:09Z") is False, True)
    check("qe-order-due-superseded",
          qe_order_due("2026-11-02T08:00:00Z", "2026-11-02T08:03:09Z"), True)
    check("qe-order-due-none",
          qe_order_due("2026-11-02T08:00:00Z", None) is False, True)
    check("dev-busy-next",
          dev_busy({}, "working :: blocked on qe"), True)
    check("dev-busy-live-ring",
          dev_busy({"dev:2026-11-02T08:00:00": 1}, None), True)
    check("dev-busy-acked-ring-false",
          dev_busy({"dev:2026-11-02T08:00:00": -1}, "idle :: waiting") is False, True)
    check("dev-busy-empty-false", dev_busy({}, None) is False, True)
    _open = {"halt": False, "state": "IN_PROGRESS"}
    _rooms = [(51, "/r51", dict(_open)), (52, "/r52", dict(_open))]
    check("pick-backlog-exclude",
          pick_backlog(_rooms, 51, exclude={52}) is None, True)
    check("pick-backlog-no-exclude",
          pick_backlog(_rooms, 51) == 52, True)
    _none = {"order": False, "post": False, "active": False, "waits": False}
    _wait = dict(_none, waits=True)
    _ord = dict(_none, order=True)
    _act = dict(_none, active=True)
    check("lane-absent", lane_state("ABSENT", _none, {240: _none}, 240) == "ABSENT", True)
    check("lane-modal", lane_state("BLOCKED-modal", _none, {240: _none}, 240) == "BLOCKED-modal", True)
    check("lane-idle", lane_state("IDLE-static", _wait, {240: _wait}, 240) == "IDLE", True)
    check("lane-working-order-here",
          lane_state("WORKING-markers", _ord, {240: _ord}, 240) == "WORKING (#240)", True)
    check("lane-working-actual-elsewhere",
          lane_state("WORKING-markers", _wait, {240: _wait, 54: _ord}, 240) == "WORKING (#54)", True)
    check("lane-working-active-here",
          lane_state("WORKING-markers", _act, {240: _act}, 240) == "WORKING (#240)", True)
    check("lane-blocked-waits",
          lane_state("WORKING-markers", _wait, {240: _wait, 54: _none}, 240) == "BLOCKED (#240)", True)
    check("lane-working-bare",
          lane_state("WORKING-markers", _none, {240: _none}, 240) == "WORKING", True)
    check("lane-waits-order-word",
          bool(LANE_WAITS_RE.search("waiting IMPL order (plan-approved)")), True)
    check("lane-peer-auditor-word",
          bool(LANE_PEER_RE.search("waiting auditor plan-review")), True)
    check("lane-peer-no-false-positive",
          not LANE_PEER_RE.search("device driver working"), True)
    check("audit-state-live", audit_state(True) == "WORKING", True)
    check("audit-state-absent", audit_state(False) == "-", True)
    _appr = "2026-11-02T08:00:00Z FROM=audit DECISION :: APPROVED — step-a may proceed"
    _need = "2026-11-02T09:00:00Z FROM=audit DECISION :: NEED rework"
    check("auditor-approved-decision", auditor_approved([_appr]), True)
    check("auditor-approved-ready-kind",
          auditor_approved(["2026-11-02T17:48:44Z FROM=audit READY :: DECISION: APPROVED — x"]), True)
    check("auditor-approved-newest-need", auditor_approved([_appr, _need]) is False, True)
    check("auditor-approved-note-spoof",
          auditor_approved(["2026-11-02T08:00:00Z FROM=dev NOTE :: mentions DECISION APPROVED"]) is False, True)
    check("auditor-approved-none", auditor_approved([]) is False, True)
    _iw = "waiting IMPL order (plan-approved)"
    check("impl-due-true",
          impl_promotion_due(_iw, True, [], True, {}, set(), {}), True)
    check("impl-due-idle-next",
          impl_promotion_due("idle :: waiting", True, [], True, {}, set(), {}) is False, True)
    check("impl-due-working-next",
          impl_promotion_due("working :: step-a impl", True, [], True, {}, set(), {}) is False, True)
    check("impl-due-virgin-no-posts",
          impl_promotion_due(_iw, False, [], True, {}, set(), {}) is False, True)
    check("impl-due-fresh-post",
          impl_promotion_due(_iw, True, ["x"], True, {}, set(), {}) is False, True)
    check("impl-due-no-approval",
          impl_promotion_due(_iw, True, [], False, {}, set(), {}) is False, True)
    check("impl-due-unacked-lane",
          impl_promotion_due(_iw, True, [], True, {}, {"dev"}, {}) is False, True)
    check("impl-due-case-insensitive",
          impl_promotion_due("AWAITING IMPL ORDER", True, [], True, {}, set(), {})
          and impl_promotion_due("parked until IMPL order", True, [], True, {}, set(), {}), True)
    check("impl-due-live-ring-0",
          impl_promotion_due(_iw, True, [], True, {"dev:2026-11-02T08:00:00": 0}, set(), {}) is False, True)
    check("impl-due-live-ring-1",
          impl_promotion_due(_iw, True, [], True, {"dev:2026-11-02T08:00:00": 1}, set(), {}) is False, True)
    check("impl-due-ring-acked",
          impl_promotion_due(_iw, True, [], True, {"dev:2026-11-02T08:00:00": -1}, set(), {}), True)
    check("impl-due-ring-escalated",
          impl_promotion_due(_iw, True, [], True, {"dev:2026-11-02T08:00:00": 4}, set(), {}), True)
    check("impl-due-non-dev-ring-ignored",
          impl_promotion_due(_iw, True, [], True, {"qe:x": 1, "first:dev": -1}, set(), {}), True)
    check("impl-due-dedupe-same-next",
          impl_promotion_due(_iw, True, [], True, {}, set(), {"next": _iw, "oid": "dev:2026-11-02T08:00:00"}) is False, True)
    check("impl-due-stored-old-next",
          impl_promotion_due(_iw, True, [], True, {}, set(), {"next": "waiting IMPL order old", "oid": "dev:2026-11-02T07:00:00"}), True)
    _st = {"next": _iw, "oid": "dev:2026-11-02T08:00:00"}
    check("impl-stalled-acked-same-next",
          impl_stalled(_iw, True, [], True, {"dev:2026-11-02T08:00:00": -1}, set(), _st), True)
    check("impl-stalled-escalated",
          impl_stalled(_iw, True, [], True, {"dev:2026-11-02T08:00:00": 4}, set(), _st) is False, True)
    check("impl-stalled-fresh-post",
          impl_stalled(_iw, True, ["x"], True, {"dev:2026-11-02T08:00:00": -1}, set(), _st) is False, True)
    check("first-due-guard-minus-one",
          first_contact_due("dev", False, True, "IDLE-static", False, {"first:dev": -1}) == (False, None), True)
    check("first-due-guard-zero-not-guard",
          first_contact_due("dev", False, True, "IDLE-static", False, {"first:dev": 0}) == (True, "first:dev"), True)
    _inote, _ioid = coord_note("dev", "INTAKE #99 dev first brief")
    check("intake-oid-shape",
          ACK_OID_RE.fullmatch(_ioid) is not None and ref_lane(_ioid) == "dev", True)
    check("intake-order-outstanding",
          order_outstanding("dev", _inote, [], {_ioid: 1}, "2026-11-02T07:00:00"), True)
    check("intake-order-acked",
          order_outstanding("dev", _inote, [], {_ioid: -1}, "2026-11-02T07:00:00") is False, True)
    check("ack-regex-order",
          ACK_OID_RE.fullmatch("dev:2026-11-02T08:00:00") is not None
          and ref_lane("dev:2026-11-02T08:00:00") == "dev", True)
    check("ack-regex-next",
          ACK_OID_RE.fullmatch("next:qe:abcdef01") is not None
          and ref_lane("next:qe:abcdef01") == "qe", True)
    check("ack-regex-peer",
          ACK_OID_RE.fullmatch("peer:qe:2026-11-02T08:00:00") is not None
          and ref_lane("peer:qe:2026-11-02T08:00:00") == "qe", True)
    check("ack-regex-first",
          ACK_OID_RE.fullmatch("first:dev") is not None
          and ref_lane("first:dev") == "dev", True)
    check("ack-regex-reject-garbage", ACK_OID_RE.fullmatch("banana") is None, True)
    check("ack-regex-reject-short-ts", ACK_OID_RE.fullmatch("dev:2026-11-02") is None, True)
    check("ack-regex-reject-ts20", ACK_OID_RE.fullmatch("dev:2026-11-02T08:00:00Z") is None, True)
    check("ack-regex-reject-upper-hash", ACK_OID_RE.fullmatch("next:qe:ABCDEF01") is None, True)
    check("ack-regex-wrong-lane", ref_lane("nope") is None, True)
    hp = ["2026-11-02T08:00:00Z FROM=dev READY :: s", "STATE: idle :: d",
          "NEXT: waiting x", "ACK: dev:2026-11-02T07:59:00", "body"]
    check("post-ack-fourth-line", post_ack(hp, hp[0]) == "dev:2026-11-02T07:59:00", True)
    check("post-ack-none", post_ack(hp[:3], hp[0]) is None, True)
    hp2 = ["2026-11-02T08:00:00Z FROM=dev NOTE :: s", "ACK: dev:2026-11-02T07:59:00"]
    check("post-ack-body-collision-guard", post_ack(hp2, hp2[0]) is None, True)
    check("post-ack-no-state", post_ack(["2026-11-02T08:00:00Z FROM=dev NOTE :: s", "x",
                                         "NEXT: idle", "ACK: dev:2026-11-02T07:59:00"],
                                        "2026-11-02T08:00:00Z FROM=dev NOTE :: s") is None, True)
    check("wire-gaps-tolerates-ack",
          wire_gaps(hp + ["tail"], hp[0]) == [], True)
    check("order-outstanding-armed-chatter",
          order_outstanding("qe", "2026-11-02T08:00:00Z FROM=x", ["2026-11-02T07:00:00Z FROM=qe NOTE :: y"],
                            {}, "2026-11-02T07:30:00") is True, True)
    check("order-outstanding-acked",
          order_outstanding("qe", "2026-11-02T08:00:00Z FROM=x", ["2026-11-02T09:00:00Z FROM=qe NOTE :: y"],
                            {"qe:2026-11-02T08:00:00": -1}, "2026-11-02T07:30:00") is False, True)
    check("order-outstanding-grandfathered",
          order_outstanding("qe", "2026-11-02T06:00:00Z FROM=x", ["2026-11-02T07:00:00Z FROM=qe NOTE :: y"],
                            {}, "2026-11-02T07:30:00") is False, True)
    check("order-outstanding-legacy-current",
          order_outstanding("qe", "2026-11-02T08:00:00Z FROM=x", ["2026-11-02T07:00:00Z FROM=qe NOTE :: y"],
                            {}, None) is True, True)
    check("order-outstanding-legacy-answered",
          order_outstanding("qe", "2026-11-02T08:00:00Z FROM=x", ["2026-11-02T09:00:00Z FROM=qe NOTE :: y"],
                            {}, None) is False, True)
    check("order-outstanding-no-coord",
          order_outstanding("qe", None, ["2026-11-02T09:00:00Z FROM=qe NOTE :: y"], {}, None) is False, True)
    check("order-ring-action",
          [order_ring_action(n, True) for n in (0, 2, 3, 4)] == ["ring", "ring", "escalate", "awaiting"]
          and [order_ring_action(n, False) for n in (0, 2, 3, 4)] == ["ring", "ring", "recreate", "awaiting"],
          True)
    check("reset-resume-rings",
          reset_resume_rings({"next:qe:a": 1, "dev:x": 2, "next:dev:b": 3}) == 2, True)
    mb = {"dev": {"all": ["2026-11-02T08:00:00Z FROM=dev READY :: s"],
                   "coord_last": "2026-11-02T07:59:00Z FROM=coord NOTE :: o"},
          "qe": {"all": []}}
    check("match-ack-current-order",
          match_ack("dev:2026-11-02T07:59:00", "dev", mb, {}, {}) == ("ack", "dev:2026-11-02T07:59:00"), True)
    check("match-ack-in-rings",
          match_ack("first:dev", "dev", {"dev": {"all": []}},
                    {"first:dev": 1}, {}) == ("ack", "first:dev"), True)
    check("match-ack-in-resume",
          match_ack("next:dev:abcdef01", "dev", {"dev": {"all": []}}, {},
                    {"dev": "next:dev:abcdef01"}) == ("ack", "next:dev:abcdef01"), True)
    check("match-ack-unmatched",
          match_ack("dev:2026-11-02T07:59:00", "dev",
                    {"dev": {"all": [], "coord_last": None}}, {}, {}) == ("ignore-stale", "dev:2026-11-02T07:59:00"),
          True)
    check("match-ack-wrong-lane",
          match_ack("qe:2026-11-02T07:59:00", "dev", mb, {}, {})[0] == "ignore-malformed", True)
    check("match-ack-garbage",
          match_ack("banana", "dev", mb, {}, {})[0] == "ignore-malformed", True)
    check("match-ack-none", match_ack(None, "dev", mb, {}, {}) == (None, None), True)
    check("match-ack-auditor-excluded",
          match_ack("dev:2026-11-02T07:59:00", "auditor", {"auditor": {"all": []}}, {}, {})[0] == "ignore-malformed",
          True)
    check("match-ack-clears-peer",
          match_ack("peer:qe:2026-11-02T07:59:00", "qe", {"qe": {"all": []}},
                    {"peer:qe:2026-11-02T07:59:00": 1}, {}) == ("ack", "peer:qe:2026-11-02T07:59:00"), True)
    stt = {"pending": "provisioned from issue text"}
    check("status-plan-armed-unacked",
          status_planned_lane("qe", {"all": [], "coord_last": "2026-11-02T08:00:00Z FROM=coord NOTE :: o",
                                     "state": None, "next": None, "ctx": None},
                              dict(stt, rings={"qe:2026-11-02T08:00:00": 1}, ack_since="2026-11-02T07:30:00"),
                              "IDLE-static") == "ring: unacked order", True)
    check("ack-scan-fresh-wins",
          ack_scan_posts("dev", {"dev": {"all": ["a", "b"]}}, {"dev": "t"},
                         ["b"]) == ["b"], True)
    check("ack-scan-first-sight-full",
          ack_scan_posts("dev", {"dev": {"all": ["a", "b"]}}, {}, []) == ["a", "b"], True)
    check("ack-scan-handled-empty",
          ack_scan_posts("dev", {"dev": {"all": ["a"]}}, {"dev": "t"}, []) == [], True)
    check("status-plan-armed-acked",
          status_planned_lane("qe", {"all": ["2026-11-02T09:00:00Z FROM=qe NOTE :: y"],
                                     "coord_last": "2026-11-02T08:00:00Z FROM=coord NOTE :: o",
                                     "state": None, "next": None, "ctx": None},
                              dict(stt, rings={"qe:2026-11-02T08:00:00": -1}, ack_since="2026-11-02T07:30:00"),
                              "IDLE-static") == "pending: provisioned from issue text", True)
    print("state-machine self-test: %d/%d PASS" % (passed, total), flush=True)
    return 0 if passed == total else 1


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
    t.add_argument("--allow-recreate", action="store_true")
    t.add_argument("--dev-dir", default=None)
    t.add_argument("--qe-dir", default=None)
    t.add_argument("--notes-file", default=None)
    s = sub.add_parser("status")
    s.add_argument("--issues", default="")
    s.add_argument("--all", action="store_true")
    pv = sub.add_parser("provision")
    pv.add_argument("--issues", default="")
    pv.add_argument("--dev-branch", default="main")
    pv.add_argument("--qe-branch", default="testbed")
    pv.add_argument("--no-launch", action="store_true")
    b = sub.add_parser("brief")
    b.add_argument("--issues", default="")
    b.add_argument("--dev-branch", default="main")
    sub.add_parser("selftest")
    for _p in (t, s, pv, b):
        _p.add_argument("--repo", default=None)
        _p.add_argument("--agent-up", default=None)
        _p.add_argument("--log-dir", default=None)
        _p.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "selftest":
        return mode_self_test()
    issues = None
    if a.cmd in ("tick", "status") and not getattr(a, "all", False):
        issues = set(int(x) for x in a.issues.split(",") if x.strip()) or None
        if issues is None and not getattr(a, "all", False):
            ap.error("pass --issues 51,52 or --all")
    ctx = Ctx(a)
    ctx.open_log()
    try:
        if a.cmd == "tick":
            return cmd_tick(ctx, issues, a.interval)
        if a.cmd == "provision":
            canon = set()
            for x in (a.issues.split(",") if a.issues else []):
                n = canon_issue(x)
                if n is None:
                    ap.error("bad issue number: %r" % x)
                canon.add(n)
            if not canon:
                ap.error("provision needs --issues (cold start from the issue list)")
            return cmd_provision(ctx, canon, a)
        if a.cmd == "brief":
            ordered = []
            for x in (a.issues.split(",") if a.issues else []):
                n = canon_issue(x)
                if n is None:
                    ap.error("bad issue number: %r" % x)
                if n not in ordered:
                    ordered.append(n)
            if not ordered:
                ap.error("brief needs --issues")
            return cmd_brief(ctx, ordered, a)
        return cmd_status(ctx, issues)
    finally:
        ctx.close_log()


if __name__ == "__main__":
    sys.exit(main())
