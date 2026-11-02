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
                         [--no-audit-teardown]
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
                     "QUEUED", "CLOSED", "HOLD", "CLOSING")


RING_LIMIT = 3  # doctrine: ring an order max 3x; then NEED-recreate, never a 4th ring
RING_ESCALATED = RING_LIMIT + 1  # ledger sentinel: order escalated as ACK-MISSING, ring no more
CTX_WARN = 70  # log watch-only
CTX_CRIT = 85  # checkpoint NOTE first, then /compact; never die at 100%% with unposted work
AUDIT_TIMEOUT_S = 1800  # live auditor silent this long -> NEED stuck
AUDIT_SLA_RERING_S = 900  # outstanding audit demand this old -> one extra ring (#461 B)
AUDIT_SLA_ESCALATE_S = 1800  # outstanding audit demand this old -> NEED escalation (#461 B)
ACK_SLA_S = 900  # worker orders carry ack-by <filed+900s>; overdue -> one nudge (#461 F)
AUDIT_OID_PREFIX = "audit:"  # ledger namespace for auditor-lane orders
AUDIT_RETASK_S = 300  # re-ring a lost/idle auditor order after ~2 ticks
AUDIT_TEARDOWN_TICKS = 2  # idle ticks after verdict handoff before teardown
AUDIT_REASK_MAX = RING_LIMIT  # same 3-ring doctrine for audit orders


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
        self.audit_sess = os.environ.get("STATE_MACHINE_AUDIT_SESS") or SESS["audit"]
        self.allow_audit_teardown = not bool(getattr(args, "no_audit_teardown", False))
        self.audit_claimed = (0, None)
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


def room_rings(st):
    """The room's ledger dict, shared by reference.

    Every writer in a tick must mutate THIS object: `st.get(...) or {}`
    hands each caller a private throwaway when the stored value is empty,
    silently dropping cross-function writes (auditor rings lost while the
    ack/session state persisted). Mutates st only to install the entry.
    """
    rings = st.get("rings")
    if not isinstance(rings, dict):
        rings = {}
        st["rings"] = rings
    return rings


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


def engine_loop_live():
    """True when a state-machine tick loop process exists. Read-only."""
    try:
        pr = subprocess.run(["pgrep", "-f", "state_machine.py tick"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            timeout=15)
        return pr.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


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


def kill_audit_session(ctx, reason):
    """Best-effort teardown of the auditor session. Never raises, never fails
    the tick: every outcome is a plain string. Dry-run never touches tmux."""
    if ctx.dry:
        return "WOULD kill %s (%s)" % (ctx.audit_sess, reason)
    rc, out = tmux(ctx, "kill-session", "-t", ctx.audit_sess)
    if rc != 0:
        return "TEARDOWN-FAILED %s (%s): %s" % (ctx.audit_sess, reason, out.strip()[:100])
    rc2, _out2 = tmux(ctx, "has-session", "-t", ctx.audit_sess)
    if rc2 == 0:
        return "KILL-UNCONFIRMED %s (%s): session still present" % (ctx.audit_sess, reason)
    return "auditor teardown %s (%s); confirmed ABSENT" % (ctx.audit_sess, reason)


def audit_session_live(ctx):
    rc, _ = tmux(ctx, "has-session", "-t", ctx.audit_sess)
    return rc == 0


def auditor_decision(box_all, full=None):
    """Newest DECISION line in auditor box, or None. Trailer-aware (#447)."""
    best_ts = None
    best_txt = None
    for l in reversed(box_all or []):
        if "DECISION" in l:
            best_ts, best_txt = l[:19], l[:220]
            break
    tts, ttext = audit_trailer_verdict(full or [])
    if tts and (best_ts is None or tts > best_ts):
        return "%s %s" % (tts, ttext)
    return best_txt


def auditor_approved(box_all, full=None):
    """Strict APPROVED verdict: the newest auditor verdict line says
    APPROVED. Pure. Only the auditor's own posts count (sender audit or
    auditor) and only verdict-shaped ones (kind DECISION, or a DECISION:
    marker in the body as the READY/NEED verdicts carry) — a dev NOTE
    merely mentioning DECISION can neither spoof nor shadow (Gap A gate).
    Trailer-aware (#447): a DECISION: trailer after an audit READY/NEED
    post counts with the parent post's sender/kind."""
    for line in reversed(box_all or []):
        m = POST_RE.match(line)
        if not m or m.group(2) not in ("audit", "auditor"):
            continue
        if m.group(3) != "DECISION" and "DECISION:" not in m.group(4):
            continue
        post_ts, post_ok = line[:19], bool(re.search(r"\bAPPROVED\b", m.group(4)))
        tts, ttext = audit_trailer_verdict(full or [])
        if tts and tts > post_ts:
            return bool(re.search(r"\bAPPROVED\b", ttext or ""))
        return post_ok
    tts, ttext = audit_trailer_verdict(full or [])
    if tts:
        return bool(re.search(r"\bAPPROVED\b", ttext or ""))
    return False


def pending_age_s(stored):
    try:
        dt = datetime.datetime.strptime(stored.get("since", ""),
                                        "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
        return (datetime.datetime.now(datetime.timezone.utc) - dt).total_seconds()
    except (ValueError, TypeError, AttributeError):
        return None


def auditor_brief_text(issue, tip, scope, room_dir=None):
    return ("AUDIT EXECUTOR (#%d) -- bounded read-only audit\n"
            "issue: %d\ntip: %s (audit this tip only; anything else is out of scope)\n"
            "room: %s\n"
            "scope: %s\n"
            "rules: read-only (no branches, no code, no test runs, no launches); retrieval first (graphify+vault); "
            "post DECISION (APPROVED | NEED with S1/S2/S3 findings) to inbox/auditor.md + wake.\n"
            "Auditor verdicts use the DECISION schema and never carry ref/ACK (the bridge rejects ref on box=auditor).\n"
            "scripts/agent_up.sh wake %d auditor nexios-engine \"inbox: auditor READY \u2014 read %s/inbox/auditor.md\"\n"
            % (issue, issue, tip, room_dir or "room inbox", scope, issue,
               room_dir or "the room"))


def audit_demand_key(requester, text):
    """Stable id for one audit request (mirrors resume_now hashing). Pure."""
    return "%s:%s" % (requester, hashlib.md5(text.encode()).hexdigest()[:8])


def audit_brief_tip(path):
    """Tip pinned in an auditor brief, or None. Read-only file read."""
    try:
        with open(path, errors="replace") as f:
            for line in f.read().splitlines():
                m = re.match(r"^tip:\s*(\S+)", line.strip())
                if m:
                    return m.group(1)
    except OSError:
        pass
    return None


def audit_verdict_ts(box_all):
    """Newest DECISION post ts (19 chars, no trailing Z), or None.
    Read-only; display stays in auditor_decision."""
    best = None
    for line in box_all or []:
        m = POST_RE.match(line)
        if m and m.group(3) == "DECISION" and (best is None or line[:19] > best):
            best = line[:19]
    return best


def audit_trailer_verdict(full_lines):
    """Newest DECISION: trailer verdict with parent POST ts. Pure (#447).

    The auditor wire schema puts the verdict on trailer lines
    (DECISION: APPROVED ...), not on the POST line (FROM=audit READY).
    Scans full box lines for ^DECISION:, attributing each to the nearest
    preceding POST_RE line's ts. Returns (ts19, text) or (None, None)."""
    best_ts = None
    best_text = None
    last_post_ts = None
    for line in full_lines or []:
        m = POST_RE.match(line)
        if m:
            last_post_ts = line[:19]
            continue
        s = (line or "").strip()
        if s.startswith("DECISION:"):
            if last_post_ts and (best_ts is None or last_post_ts > best_ts):
                best_ts = last_post_ts
                best_text = s[:220]
    return best_ts, best_text


def audit_verdict_ts_full(box_all, full_lines=None):
    """Newest verdict ts from POST DECISION or DECISION: trailer. Pure."""
    best = audit_verdict_ts(box_all)
    tts, _ = audit_trailer_verdict(full_lines or [])
    if tts and (best is None or tts > best):
        best = tts
    return best


def audit_unhandled_verdict(box_all, full_lines, handled_ts):
    """Newest unhandled verdict (POST or trailer) newer than handled. Pure.

    Fixes first-sight loss: new_posts() reports [] when the handled
    cursor is unset, while the tick cursor still advances past the
    verdict. Scanning unhandled history makes the first sight actionable."""
    since = (handled_ts or "")[:19]
    best_ts = None
    best_text = None
    for line in box_all or []:
        m = POST_RE.match(line)
        if m and m.group(3) == "DECISION" and line[:19] > since:
            if best_ts is None or line[:19] > best_ts:
                best_ts = line[:19]
                best_text = line[-200:]
    tts, ttext = audit_trailer_verdict(full_lines or [])
    if tts and tts > since and (best_ts is None or tts > best_ts):
        best_ts, best_text = tts, ttext
    return (best_ts, best_text) if best_ts else (None, None)


def audit_state_demand(st, boxes=None):
    """Fallback demand from room pending/phase, plus dev/qe NEXT waiting audit.

    Covers #240-style stalls: no NEED-audit post and pending None, but a
    lane NEXT says `waiting auditor plan-review` / `waiting ... audit`.
    Pure."""
    pending = (st.get("pending") or "")
    if re.search(r"audit", pending, re.I) or st.get("phase") == "AUDIT":
        return {"requester": "dev", "text": pending[:200]}
    for box in ("dev", "qe"):
        nxt = ((boxes or {}).get(box) or {}).get("next") or ""
        if re.search(r"waiting.*audit|plan-review", nxt, re.I):
            return {"requester": box, "text": nxt[:200]}
    return None


def audit_fresh_need(fresh):
    """Newest fresh dev/qe NEED-audit post, or None. Pure."""
    cands = [(box, q) for box in ("dev", "qe") for q in (fresh or {}).get(box, [])
             if (lambda m: m and m.group(3) == "NEED" and "audit" in q.lower())(POST_RE.match(q))]
    if not cands:
        return None
    box, q = cands[-1]
    return {"requester": box, "text": q[-200:]}


def audit_demand(boxes, st, fresh=None):
    """Outstanding audit demand, fresh posts first, then unhandled history,
    then the room pending/phase/NEXT fallback. Pure."""
    hit = audit_fresh_need(fresh or {})
    if hit:
        return hit
    if fresh is not None:
        handled = st.get("handled") or {}
        for box in ("dev", "qe"):
            since = handled.get(box) or ""
            for line in (boxes.get(box) or {}).get("all", []):
                if line[:20] > since[:20]:
                    m = POST_RE.match(line)
                    if m and m.group(3) == "NEED" and "audit" in line.lower():
                        return {"requester": box, "text": line[-200:]}
    return audit_state_demand(st, boxes)


def audit_queue_head(issue, demand, rooms_all):
    """Oldest outstanding audit demand (self included). Pure; FIFO, no starvation."""
    if demand is None:
        return None
    best = (utcnow(), issue)
    for iss, _rdir, ost in rooms_all or []:
        if iss == issue:
            continue
        ost = ost or {}
        if ost.get("audit_pending") and not (ost.get("halt") or (ost.get("state") or "") in ("CLOSED", "HOLD")):
            since = (ost["audit_pending"].get("since") or "9" * 20)
            if (since, iss) < best:
                best = (since, iss)
    if best[1] != issue and best < (utcnow(), issue):
        return best[1]
    return issue


def audit_ring_action(n):
    """Ring/escalate/await for an audit order count. Pure (mirrors order_ring_action)."""
    if n < RING_LIMIT:
        return "ring"
    if n == RING_LIMIT:
        return "escalate"
    return "awaiting"


def audit_retask_gate(demand, fresh_need, probe, live, brief_present, brief_tip, cur_tip,
                      verdict_on_file, demand_key, handled_demand, oid, rings, claimed_by, age_s):
    """One-word gate for the audit lane. Pure. Vocabulary:
    skip-no-demand | launch | defer-working | defer-busy | skip-done |
    brief-then-ring | ring | escalate | awaiting | ring-again | hold-stale."""
    if demand is None:
        return "skip-no-demand"
    if not live:
        return "launch"
    if not str(probe).startswith("IDLE"):
        return "defer-working"
    if claimed_by not in (None,):
        return "defer-busy"
    if verdict_on_file and not fresh_need and demand_key == handled_demand:
        return "skip-done"
    if not brief_present or (cur_tip and brief_tip and brief_tip != cur_tip):
        return "brief-then-ring"
    n = (rings or {}).get(oid, 0) if oid else 0
    if isinstance(n, int) and n < 0:
        return "skip-done"
    if n <= 0:
        return "ring"
    if n == RING_LIMIT:
        return "escalate"
    if n > RING_LIMIT:
        return "awaiting"
    if (age_s or 0) >= AUDIT_RETASK_S:
        return "ring-again"
    return "awaiting"


def audit_sla_action(age_s, live, probe, oid, n, rerings,
                     re_ring_after_s=AUDIT_SLA_RERING_S,
                     escalate_after_s=AUDIT_SLA_ESCALATE_S,
                     handled=False):
    """ok | re-ring | escalate-absent | escalate-stale | escalate-missing. Pure.

    Age-based SLA over an outstanding audit demand, orthogonal to the
    ring-count doctrine: the one re-ring never mints an oid and never
    touches rings[oid] (3-ring escalation untouched). Absent/stale/missing
    escalate to NEEDs. There is no kill path (teardown stays verdict-gated,
    so a stale-live session is re-tasked, never destroyed). An answered
    demand (handled) is always ok: the log must never overstate the flag."""
    if handled or age_s is None:
        return "ok"
    idle = str(probe).startswith("IDLE")
    if not live:
        return "escalate-absent" if age_s >= escalate_after_s else "ok"
    if not idle:
        return "escalate-stale" if age_s >= escalate_after_s else "ok"
    if oid and (n or 0) > 0:
        if age_s >= re_ring_after_s and (rerings or 0) < 1:
            return "re-ring"
        return "ok" if age_s < escalate_after_s else "escalate-missing"
    return "escalate-missing" if age_s >= escalate_after_s else "ok"


def _ts_age_s(since_ts, now_ts):
    """Seconds between two ts19/Z stamps, or None. Pure (clock-skew safe)."""
    try:
        base = datetime.datetime.strptime((since_ts or "")[:19],
                                          "%Y-%m-%dT%H:%M:%S")
        now = datetime.datetime.strptime((now_ts or "")[:19],
                                         "%Y-%m-%dT%H:%M:%S")
        return max(0.0, (now - base).total_seconds())
    except (ValueError, TypeError):
        return None


def audit_sla_age_s(stored, sla, demand_key, now_ts):
    """Oldest known age (seconds) of the current audit demand. Pure.

    max() of the sticky audit_pending since and the SLA first-seen stamp,
    so FIFO-queued demand (no store yet) still ages; None while neither
    exists (demand just formed)."""
    cands = []
    if isinstance(stored, dict) and stored.get("since"):
        cands.append(_ts_age_s(stored["since"], now_ts))
    if isinstance(sla, dict) and sla.get("key") == demand_key and sla.get("first"):
        cands.append(_ts_age_s(sla["first"], now_ts))
    cands = [c for c in cands if c is not None]
    return max(cands) if cands else None


def audit_sla_next(asess, demand_key, action, now_ts):
    """Updated sla store for the audit session record. Pure.

    Keyed on demand_key: a new demand resets first-seen and the re-ring
    budget, so each demand gets exactly one re-ring. No demand clears it."""
    if not demand_key:
        return {}
    sla = dict((asess or {}).get("sla") or {})
    if sla.get("key") != demand_key:
        sla = {"key": demand_key, "first": now_ts, "rerings": 0}
    if action == "re-ring":
        sla["rerings"] = int(sla.get("rerings") or 0) + 1
    return sla


def audit_teardown_gate(live, probe, verdict_on_file, handled_past_verdict, demand, fresh_need,
                        other_demand, dup_audit, idle_ticks, n_ticks, teardown_enabled, dry):
    """(new_idle_ticks, action) for auditor teardown. Pure except dry flag use.
    Actions: absent | disabled | hold-dup | hold-demand | hold-verdict |
    hold-working | countdown | kill | would-kill."""
    if not live:
        return 0, "absent"
    if not teardown_enabled:
        return 0, "disabled"
    if dup_audit:
        return 0, "hold-dup"
    if fresh_need or demand or other_demand:
        return 0, "hold-demand"
    if not verdict_on_file or not handled_past_verdict:
        return 0, "hold-verdict"
    if not str(probe).startswith("IDLE"):
        return 0, "hold-working"
    k = (idle_ticks or 0) + 1
    if k < (n_ticks or 2):
        return k, "countdown"
    return k, ("would-kill" if dry else "kill")


def audit_planned_text(verdict_on_file, report_on_file, live, stored_pending, demand,
                       queue_head, issue):
    """Next-step text for the audit lane. Pure; never returns bare 'collect verdict'
    when nothing asks for one."""
    if verdict_on_file:
        return "verdict on file"
    if report_on_file:
        return "report on file"
    if stored_pending:
        return "awaiting verdict"
    if demand is not None and queue_head is not None and queue_head != issue:
        return "queued behind #%d" % queue_head
    if demand is not None:
        return "ring audit lane" if live else "queued: launch audit"
    if live:
        return "idle: no audit requested"
    return "no audit requested"


def auditor_lifecycle(ctx, issue, room_dir, st, boxes, fresh, rooms_all=None):
    """Request-driven audit cycle with a single shared auditor session.

    Dev/qe post NEED-audit (request); the engine generates the tip-pinned
    brief, starts the auditor via `agent_up.sh audit` (headless tmux session
    with --auto: asks auto-approved, denies hold; never windowed, and a modal
    there means a stale pre-auto session that must be relaunched),
    re-tasks the live session with a fresh brief + ring when another room
    needs it (FIFO across rooms, one claim per tick), awaits the verdict,
    hands findings back to the requester (NOTE + ring), and tears the
    session down after AUDIT_TEARDOWN_TICKS quiet post-verdict ticks.
    Legacy pending/phase heuristic stays as fallback. Returns (acted,
    need_verdict, ap_store, as_store, audit_note): ap_store as before ({} or
    the audit_pending dict); as_store is the audit_session record to persist
    (None = leave untouched); audit_note feeds the tick/status tables.
    """
    acted = []
    pending = (st.get("pending") or "")
    stored = st.get("audit_pending") or {}
    store = None
    asess = dict(st.get("audit_session") or {})
    rings = room_rings(st)
    fresh_need = audit_fresh_need(fresh)
    verdict_ts = audit_verdict_ts_full(boxes["auditor"]["all"], boxes["auditor"].get("full"))
    handled_past = bool(verdict_ts) and (st.get("handled") or {}).get("auditor", "")[:19] >= verdict_ts
    live = audit_session_live(ctx)
    aprobe = fast_probe(ctx, ctx.audit_sess) if live else "ABSENT"
    rep = latest_audit_report(ctx, issue)
    seen_verdict = False
    for post in fresh.get("auditor", []):
        if "DECISION" not in post:
            continue
        if seen_verdict:
            acted.append("duplicate audit verdict suppressed (same tick)")
            continue
        seen_verdict = True
        target = ((fresh_need or {}).get("requester") if isinstance(fresh_need, dict) else None) or stored.get("requester", "dev")
        note, oid = coord_note(target, "AUDIT %s (#%d): detail inbox/auditor.md"
                               % ("APPROVED" if "APPROVED" in post else "NEED", issue))
        if rep:
            note += "; report %s" % os.path.basename(rep)
        if ctx.dry:
            acted.append("WOULD hand audit verdict to %s + ring-if-idle" % target)
        else:
            if append_box_note(room_dir, target, note):
                acted.append("audit verdict handed to %s" % target)
                asess["handled_demand"] = asess.get("demand_key")
                if target in SESS and fast_probe(ctx, SESS[target]).startswith("IDLE"):
                    acted.append(ring(ctx, SESS[target], target, "audit verdict ready", oid=oid,
                                      issue=issue))
            else:
                acted.append("HAND-BACK-FAILED %s" % target)
    if not seen_verdict:
        uhts, uhtext = audit_unhandled_verdict(
            boxes["auditor"]["all"], boxes["auditor"].get("full"),
            (st.get("handled") or {}).get("auditor", ""))
        if uhts and uhts > (st.get("handled") or {}).get("auditor", "")[:19]:
            seen_verdict = True
            target = ((fresh_need or {}).get("requester") if isinstance(fresh_need, dict) else None) or stored.get("requester", "dev")
            note, oid = coord_note(target, "AUDIT %s (#%d): detail inbox/auditor.md"
                                   % ("APPROVED" if "APPROVED" in (uhtext or "") else "NEED", issue))
            if rep:
                note += "; report %s" % os.path.basename(rep)
            if ctx.dry:
                acted.append("WOULD hand audit verdict to %s + ring-if-idle" % target)
            else:
                if append_box_note(room_dir, target, note):
                    acted.append("audit verdict handed to %s (unhandled scan)" % target)
                    asess["handled_demand"] = asess.get("demand_key")
                    if target in SESS and fast_probe(ctx, SESS[target]).startswith("IDLE"):
                        acted.append(ring(ctx, SESS[target], target, "audit verdict ready", oid=oid,
                                          issue=issue))
                else:
                    acted.append("HAND-BACK-FAILED %s" % target)
    if any("audit verdict handed to" in a or "WOULD hand audit verdict" in a for a in acted):
        store = {}
    demand = audit_demand(boxes, st, fresh)
    demand_key = audit_demand_key(demand["requester"], demand["text"]) if demand else None
    qhead = audit_queue_head(issue, demand, rooms_all)
    other_demand = False
    for iss, _rdir, ost in rooms_all or []:
        if iss != issue and (ost or {}).get("audit_pending"):
            other_demand = True
            break
    cur_idle = asess.get("idle_ticks", 0) if isinstance(asess.get("idle_ticks"), int) else 0
    new_idle, t_act = audit_teardown_gate(
        live, aprobe, bool(verdict_ts), handled_past, demand is not None,
        fresh_need is not None, other_demand,
        (len(pane_role_sessions(ctx).get("audit", [])) > 1) if live and not (fresh_need or demand or other_demand) and verdict_ts and handled_past else False,
        cur_idle, AUDIT_TEARDOWN_TICKS, ctx.allow_audit_teardown, ctx.dry)
    if t_act in ("kill", "would-kill"):
        res = kill_audit_session(ctx, "verdict %s handled, idle %d/%d, no demand"
                                 % (verdict_ts or "-", new_idle, AUDIT_TEARDOWN_TICKS))
        acted.append(res)
        if not ctx.dry and res.startswith("auditor teardown"):
            for o in list(rings):
                if o.startswith(AUDIT_OID_PREFIX) and isinstance(rings.get(o), int) and rings[o] > 0:
                    rings[o] = -1
            asess = {"state": "parked", "idle_ticks": 0,
                     "handled_demand": asess.get("handled_demand"),
                     "verdict_ts": verdict_ts}
            store = {}
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), False,
                                        bool(stored), demand, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if t_act == "disabled":
        acted.append("auditor teardown disabled (--no-audit-teardown); session held live")
        if not ctx.dry:
            asess["idle_ticks"] = 0
            if asess.get("state") != "live":
                asess["state"] = "live"
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), True,
                                        bool(stored), demand, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if t_act == "countdown":
        asess["state"] = "live"
        asess["idle_ticks"] = new_idle
        if new_idle == 1:
            acted.append("auditor idle %d/%d since verdict; teardown when quiet" % (new_idle, AUDIT_TEARDOWN_TICKS))
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), True,
                                        bool(stored), demand, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if t_act == "absent" and asess.get("state") == "live":
        asess = {"state": "absent", "idle_ticks": 0,
                 "handled_demand": asess.get("handled_demand"),
                 "verdict_ts": verdict_ts}
    dec = auditor_decision(boxes["auditor"]["all"], boxes["auditor"].get("full"))
    if dec and not fresh_need and not demand:
        if stored and store is None:
            acted.append("verdict on file; stale audit_pending cleared")
            store = {}
        audit_note = audit_planned_text(True, bool(rep), live, bool(stored), None, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if demand is None and not stored:
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, False, None, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    requester = (demand or {}).get("requester") or stored.get("requester", "dev")
    brief = os.path.join(room_dir, "prompts", "auditor.txt")
    brief_present = os.path.exists(brief)
    brief_tip = audit_brief_tip(brief) if brief_present else None
    tip = stored.get("tip")
    cur_tip = None
    if not brief_present or brief_tip is None:
        info = git_info(ctx.dev_dir)
        if not info:
            acted.append("NEED audit brief ungeneratable: dev checkout tip unreadable")
            audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
            return acted, True, store, (asess or None), audit_note
        tip = info[1]
        cur_tip = tip
    else:
        info = git_info(ctx.dev_dir)
        cur_tip = info[1] if info else tip
        tip = cur_tip or tip
    oid = None
    for o in list(rings):
        if o.startswith(AUDIT_OID_PREFIX) and isinstance(rings.get(o), int) and rings[o] > 0:
            oid = o
            break
    if demand and (oid is None or asess.get("demand_key") != demand_key):
        for o in list(rings):
            if o.startswith(AUDIT_OID_PREFIX) and isinstance(rings.get(o), int) and rings[o] > 0:
                rings[o] = -1
        oid = None
    claimed = ctx.audit_claimed[1] if (ctx.audit_claimed[0] == ctx.tick_seq
                                       and ctx.audit_claimed[1] not in (None, issue)) else None
    age_s = pending_age_s(stored)
    act = audit_retask_gate(demand, fresh_need, aprobe, live, brief_present, brief_tip, cur_tip,
                            bool(verdict_ts), demand_key, asess.get("handled_demand"), oid, rings,
                            claimed, age_s)
    # Item B (#461): age-based auditor SLA, orthogonal to the ring-count
    # doctrine. Evaluated only while a demand is outstanding (orphaned
    # stores age silently). The re-ring reuses the live oid and never
    # touches rings[oid]; stale forces a re-brief + ring attempt (never a
    # kill — teardown stays verdict-gated).
    _sla_action = "ok"
    _sage = None
    _now19 = utcnow()
    _sla0 = (asess or {}).get("sla") or {}
    if demand_key:
        _sage = audit_sla_age_s(stored, _sla0, demand_key, _now19)
        _sla_action = audit_sla_action(
            _sage, live, aprobe, oid, rings.get(oid, 0) if oid else 0,
            _sla0.get("rerings", 0) if _sla0.get("key") == demand_key else 0,
            handled=(asess.get("handled_demand") == demand_key))
        if (_sla_action == "re-ring" and verdict_ts
                and _sla0.get("key") == demand_key and _sla0.get("first")
                and verdict_ts >= _sla0["first"][:19]):
            _sla_action = "ok"  # verdict already covers this demand era
    if demand_key or _sla0:
        asess["sla"] = audit_sla_next(asess, demand_key, _sla_action, _now19)
    if _sla_action == "re-ring":
        if qhead is not None and qhead != issue:
            acted.append("AUDIT-SLA re-ring deferred #%d: queued behind #%d (FIFO)" % (issue, qhead))
        elif ctx.dry:
            acted.append("WOULD re-ring audit lane (oid %s, no new oid)" % oid)
        else:
            acted.append(ring(ctx, ctx.audit_sess, "auditor", "AUDIT-SLA re-ring #%d" % issue,
                              oid=oid, pointer=os.path.join(room_dir, "prompts", "auditor.txt"), tag_oid=False))
            ctx.audit_claimed = (ctx.tick_seq, issue)
    elif _sla_action == "escalate-absent":
        acted.append("AUDIT-SLA absent #%d: demand aging %ds with no session; launch path owns it"
                     % (issue, int(_sage or 0)))
    elif _sla_action == "escalate-stale":
        acted.append("NEED audit #%d: live session stale (demand aging %ds, probe %s); re-task or teardown needed"
                     % (issue, int(_sage or 0), aprobe))
        if act not in ("skip-no-demand", "skip-done"):
            act = "brief-then-ring"
    elif _sla_action == "escalate-missing":
        acted.append("NEED audit #%d: demand aging %ds with no ring progress (oid %s); re-drive needed"
                     % (issue, int(_sage or 0), oid))
    if act == "skip-no-demand":
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, bool(stored), None, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if act == "skip-done":
        audit_note = audit_planned_text(True, bool(rep), live, bool(stored), demand, qhead, issue)
        return acted, False, store, (asess or None), audit_note
    if act == "defer-working":
        acted.append("NEED audit #%d deferred: auditor busy (probe %s); re-task when idle" % (issue, aprobe))
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, store, (asess or None), audit_note
    if act == "defer-busy":
        acted.append("audit #%d queued behind #%s (lane busy this tick)" % (issue, claimed))
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, store, (asess or None), audit_note
    if qhead is not None and qhead != issue and act in ("launch", "ring", "ring-again", "brief-then-ring"):
        acted.append("audit #%d queued behind #%d (FIFO); re-task when head clears" % (issue, qhead))
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, store, (asess or None), audit_note
    if act == "launch":
        if not brief_present:
            scope_txt = (demand or {}).get("text") or pending[:200] or "lane audit"
            notes = sorted(glob.glob(os.path.join(room_dir, "notes", "*.md")))
            if notes:
                scope_txt += "; room notes: %s" % os.path.basename(notes[-1])
            if ctx.dry:
                acted.append("WOULD write brief %s (lane audit)" % brief)
            else:
                bw = write_brief(ctx, room_dir, "auditor.txt",
                                 auditor_brief_text(issue, tip or cur_tip or "unknown", scope_txt[:220], room_dir))
                acted.append(bw)
                if bw.startswith("BRIEF-FAILED"):
                    audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
                    return acted, True, store, (asess or None), audit_note
        if ctx.dry:
            acted.append("WOULD launch auditor: agent_up.sh audit %s" % issue)
            audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
            return acted, True, {"requester": requester, "since": utcnow(), "tip": tip, "demand_key": demand_key}, (asess or None), audit_note
        rc, out = ctx.run([ctx.agent_up, "audit", str(issue)], timeout=180)
        acted.append("auditor launched rc=%d %s" % (rc, (out.strip().splitlines() or ["?"])[-1][:100]))
        if rc != 0:
            audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
            return acted, True, store, (asess or None), audit_note
        asess["state"] = "live"
        asess["idle_ticks"] = 0
        asess["demand_key"] = demand_key
        ctx.audit_claimed = (ctx.tick_seq, issue)
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), True, True, demand, qhead, issue)
        return acted, True, {"requester": requester,
                             "since": stored.get("since") or utcnow(), "tip": tip,
                             "demand_key": demand_key}, asess, audit_note
    if act in ("ring", "ring-again"):
        if not oid:
            oid = "%s%s" % (AUDIT_OID_PREFIX, utcnow()[:19])
        new_store = {"requester": requester, "since": stored.get("since") or utcnow(),
                     "tip": tip or cur_tip, "demand_key": demand_key}
        if ctx.dry:
            acted.append("WOULD ring audit lane (oid %s)" % oid)
            audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
            return acted, True, new_store, (asess or None), audit_note
        else:
            if aprobe.startswith("IDLE"):
                acted.append(ring(ctx, ctx.audit_sess, "auditor", "AUDIT #%d" % issue,
                                  oid=oid, pointer=os.path.join(room_dir, "prompts", "auditor.txt"), tag_oid=False))
                n = rings.get(oid, 0)
                rings[oid] = (n + 1) if isinstance(n, int) and n >= 0 else 1
                asess["demand_key"] = demand_key
                asess["oid"] = oid
                asess["state"] = "live"
                asess["idle_ticks"] = 0
                ctx.audit_claimed = (ctx.tick_seq, issue)
            else:
                acted.append("audit #%d ring deferred: lane busy at ring time" % issue)
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, new_store, (asess or None), audit_note
    if act == "brief-then-ring":
        scope_txt = (demand or {}).get("text") or pending[:200] or "lane audit"
        notes = sorted(glob.glob(os.path.join(room_dir, "notes", "*.md")))
        if notes:
            scope_txt += "; room notes: %s" % os.path.basename(notes[-1])
        new_store = {"requester": requester, "since": stored.get("since") or utcnow(),
                     "tip": tip or cur_tip, "demand_key": demand_key}
        if ctx.dry:
            acted.append("WOULD write audit brief + ring audit lane")
            audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
            return acted, True, new_store, (asess or None), audit_note
        else:
            bw = write_brief(ctx, room_dir, "auditor.txt",
                             auditor_brief_text(issue, tip or cur_tip or "unknown", scope_txt[:220], room_dir))
            acted.append(bw)
            if bw.startswith("BRIEF-FAILED"):
                audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
                return acted, True, store, (asess or None), audit_note
            oid = "%s:%s" % (AUDIT_OID_PREFIX, utcnow()[:19])
            rings[oid] = 1
            asess["demand_key"] = demand_key
            asess["oid"] = oid
            asess["tip"] = tip or cur_tip
            asess["state"] = "live"
            asess["idle_ticks"] = 0
            if aprobe.startswith("IDLE"):
                acted.append(ring(ctx, ctx.audit_sess, "auditor", "AUDIT #%d" % issue,
                                  oid=oid, pointer=os.path.join(room_dir, "prompts", "auditor.txt"), tag_oid=False))
                ctx.audit_claimed = (ctx.tick_seq, issue)
            else:
                acted.append("audit #%d brief rewritten; ring deferred (probe %s)" % (issue, aprobe))
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, new_store, (asess or None), audit_note
    if act == "escalate":
        acted.append("ACK-MISSING audit #%d order %s: rung %d times, no verdict; re-brief next tick" % (issue, oid, AUDIT_REASK_MAX))
        if oid:
            rings[oid] = RING_ESCALATED
        audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, True, demand, qhead, issue)
        return acted, True, store, (asess or None), audit_note
    audit_note = audit_planned_text(bool(verdict_ts), bool(rep), live, bool(stored), demand, qhead, issue)
    return acted, True, store, (asess or None), audit_note


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


def ring_line(box, text, oid=None, issue=None, ack_by=None):
    """Doorbell pointer text. Pure.

    issue=None keeps the legacy bare-box format byte-identical; with an
    issue the pointer is room-scoped so shared dev/qe sessions cannot
    misdeliver it (G2). ack_by (a deadline stamp) slots BEFORE the oid so
    [oid ...] stays the trailing token (#461 F)."""
    suffix = (" ack-by %s" % ack_by if ack_by else "") + ((" [oid %s]" % oid) if oid else "")
    if issue is None:
        return "inbox: %s%s -- read inbox/%s.md" % (text, suffix, box)
    return "inbox [#%s/%s]: %s%s -- read inbox/%s.md" % (issue, box, text, suffix, box)


def ring(ctx, sess, box, text, channel="inbox", oid=None, issue=None, pointer=None, tag_oid=True,
         ack_by=None):
    """Doorbell. Idle sessions only (caller checks). inbox = box pointer,
    notes = consultant channel (logs/consultant-notes.md). oid, when given,
    is the ledger order id the worker must ACK (post_update ref=<oid>).
    pointer overrides the box path hint (for cross-room pointers);
    tag_oid=False suppresses the [oid ...] suffix (auditor lanes are never
    asked to ACK). ack_by slots before the oid suffix (#461 F)."""
    if channel == "notes":
        line = "notes: %s -- read logs/consultant-notes.md" % text
    else:
        segs = (" ack-by %s" % ack_by if ack_by else "") + ((" [oid %s]" % oid) if (oid and tag_oid) else "")
        if pointer:
            line = "inbox: %s%s -- read %s" % (text, segs, pointer)
        else:
            line = ring_line(box, text, oid=(oid if tag_oid else None), issue=issue, ack_by=ack_by)
    if ctx.dry:
        return "WOULD ring %s: %s" % (sess, line)
    rc, out = ctx.run([ctx.agent_up, "msg", sess, line], timeout=60)
    if rc == 0:
        return "rang %s" % sess
    return "RING-FAILED %s: %s" % (sess, out.strip()[:120])


def coord_note(role, text, ack_by=None):
    """Engine order line for a lane box + its ledger oid. Pure.

    The oid embeds the post timestamp exactly as has_unacked derives it,
    so the written id and the ledger key always agree. ack_by=True stamps
    ack-by <filed+ACK_SLA_S> before the oid so [oid ...] stays trailing;
    the caller records the deadline via file_order_deadline (#461 F).
    """
    ts = utcnow()
    oid = "%s:%s" % (role, ts[:19])
    body = text + (" ack-by %s" % ack_by_ts(ts[:19]) if ack_by else "")
    return ("%s FROM=coord NOTE :: %s [oid %s]" % (ts, body, oid)), oid


def ack_by_ts(order_ts19, sla_s=ACK_SLA_S):
    """ack-by deadline stamp for an order ts, or None. Pure."""
    try:
        base = datetime.datetime.strptime((order_ts19 or "")[:19], "%Y-%m-%dT%H:%M:%S")
    except (ValueError, TypeError):
        return None
    return (base + datetime.timedelta(seconds=sla_s)).strftime("%Y-%m-%dT%H:%M:%SZ")


def ack_overdue(by_ts, now_ts):
    """True when now is past the ack-by deadline. Pure.

    No deadline (legacy/grandfathered orders) never nudges."""
    try:
        now = datetime.datetime.strptime((now_ts or "")[:19], "%Y-%m-%dT%H:%M:%S")
        by = datetime.datetime.strptime((by_ts or "")[:19], "%Y-%m-%dT%H:%M:%S")
    except (ValueError, TypeError):
        return False
    return now > by


def ack_nudge_due(overdue, nudged):
    """True on the first overdue sighting only. Pure."""
    return bool(overdue) and not nudged


def ack_overdue_ids(deadlines, rings, now_ts):
    """Outstanding oids past their ack-by deadline, oldest first. Pure.

    deadlines is st["ack_deadline"] ({oid: {"by": ts, "nudged": 0/1}});
    only live ledger oids qualify (acked/escalated/removed never nudge).
    Ledger oids embed ts19, so oid sort is chronological."""
    out = []
    for oid, info in (deadlines or {}).items():
        val = (rings or {}).get(oid)
        if val is None or val in (-1, RING_ESCALATED):
            continue
        if ack_overdue((info or {}).get("by"), now_ts):
            out.append(oid)
    return sorted(out)


def file_order_deadline(ad_store, oid):
    """Record the ack-by deadline for a freshly filed order. Pure.

    Mutates and returns the passed store ({oid: {"by": ts, "nudged": 0}});
    orders without a parseable ts are not tracked (never nudge)."""
    ts = oid.split(":", 1)[1][:19] if ":" in (oid or "") else ""
    by = ack_by_ts(ts)
    if by:
        ad_store[oid] = {"by": by, "nudged": 0}
    return ad_store


def prune_deadlines(deadlines, rings):
    """Drop acked/escalated/removed orders from the deadline store. Pure."""
    return {o: i for o, i in (deadlines or {}).items()
            if (rings or {}).get(o) not in (None, -1, RING_ESCALATED)}


def write_ack_overdue(room_dir, pairs):
    """Live-only ack.overdue visibility file (one 'oid by' per line). I/O.

    Absent/empty list removes the file; returns a short status for logs."""
    p = os.path.join(room_dir, "ack.overdue")
    try:
        if not pairs:
            try:
                os.unlink(p)
            except OSError:
                pass
            return "ack-overdue-clear"
        with open(p, "w") as f:
            for oid, by in pairs:
                f.write("%s %s\n" % (oid, by))
        return "ack-overdue-%d" % len(pairs)
    except OSError:
        return "ack-overdue-FAILED"


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


READY_TIP_RE = re.compile(r"^[0-9a-f]{7,40}$", re.I)
READY_SHA_RE = re.compile(r"^[0-9a-f]{64}$", re.I)
READY_STAT_RE = re.compile(
    r"(\d+)\s+files?(?:\s+changed)?,?\s*(\d+)\s+insertions?|(\d+)\s*/\s*(\d+)", re.I)
READY_FIELD_RE = re.compile(
    r"^\s*(branch|patch|tip|stat|symbols|sha256)\s*:\s*(.+?)\s*$", re.I)
QE_DIFF_RE = re.compile(r"diff\s+present\s*:\s*(y|n|yes|no)\b", re.I)
DONE_WORD_RE = re.compile(r"\bDONE\b")
VERIFIED_WORD_RE = re.compile(r"\bVERIFIED\b")
# Worktree fallback reads at most this many bytes per tick (S3: a large
# in-repo patch path must not impose unbounded per-tick I/O/CPU).
READY_WORKTREE_MAX_BYTES = 1 << 20


def _worktree_bytes(ctx, patch, limit=READY_WORKTREE_MAX_BYTES):
    """Bounded worktree read for the scratch fallback, or None. I/O."""
    try:
        path = os.path.join(ctx.dev_dir, patch)
        if os.path.getsize(path) > limit:
            return None
        with open(path, "rb") as f:
            return f.read(limit + 1)
    except OSError:
        return None


def post_body_lines(full_lines, post, max_lines=40):
    """Body lines of one post: lines after its header until the next POST header. Pure.

    Worker posts are multi-line (header + STATE:/NEXT:/free-text body);
    boxes[box]["all"] holds headers only, ["full"] holds every line.
    Last occurrence wins: two byte-identical headers (same-second re-post)
    evaluate the newest post against the newest body."""
    seq = list(full_lines or [])
    try:
        idx = len(seq) - 1 - seq[::-1].index(post)
    except ValueError:
        return []
    out = []
    for line in list(full_lines)[idx + 1:idx + 1 + max_lines]:
        if POST_RE.match(line):
            break
        out.append(line)
    return out


def newest_impl_ready_post(posts):
    """Newest dev POST header that is READY and names IMPL, or None. Pure."""
    best = None
    for post in posts or []:
        m = POST_RE.match(post)
        if not m or m.group(2) != "dev" or m.group(3) != "READY":
            continue
        if "IMPL" not in post.upper():
            continue
        best = post
    return best


def parse_ready_artifacts(lines):
    """Artifact fields claimed by an IMPL READY post block. Pure.

    Grammar (case-insensitive keys, one field per line, first wins):
      branch: <no-space>  patch: <repo-relative>  tip: <7-40 hex>
      stat: <n> files?, <n> insertions? | <n>/<n>
      symbols: <up to 8 space/comma separated>  sha256: <64 hex, optional>
    Returns None when the block carries no artifact field at all (pure
    free-text claim); otherwise raw values (validation is gaps' job)."""
    fields = {}
    for line in lines or []:
        m = READY_FIELD_RE.match(line)
        if not m:
            continue
        key = m.group(1).lower()
        if key not in fields:
            fields[key] = m.group(2)
    return fields or None


def ready_artifact_gaps(fields, probes):
    """Unverifiable-claim tokens for an IMPL READY field set. Pure.

    probes is None (shape-only) or the ready_probe dict. Tokens feed the
    re-post NEED verbatim; an empty list means the claim verifies."""
    if not fields:
        return ["no-artifacts"]
    gaps = []
    branch = fields.get("branch") or ""
    if not branch:
        gaps.append("missing-branch")
    elif re.search(r"\s", branch) or len(branch) > 120:
        gaps.append("bad-branch")
    patch = fields.get("patch") or ""
    if not patch:
        gaps.append("missing-patch")
    elif (patch.startswith("/") or patch.startswith("\\")
          or ".." in patch.split("/") or "\\" in patch
          or re.search(r"\s", patch) or len(patch) > 120):
        gaps.append("bad-patch")
    tip = fields.get("tip") or ""
    if not tip:
        gaps.append("missing-tip")
    elif not READY_TIP_RE.match(tip):
        gaps.append("bad-tip")
    stat = fields.get("stat") or ""
    if not stat:
        gaps.append("missing-stat")
    elif not READY_STAT_RE.search(stat):
        gaps.append("bad-stat")
    syms = re.findall(r"[^\s,]+", fields.get("symbols") or "")
    if not (fields.get("symbols") or "").strip():
        gaps.append("missing-symbols")
    elif len(syms) > 8 or any(len(s) > 120 for s in syms):
        gaps.append("bad-symbols")
    if "sha256" in fields and not READY_SHA_RE.match(fields["sha256"] or ""):
        gaps.append("bad-sha256")
    if probes is None:
        return gaps
    tip_shape_ok = "missing-tip" not in gaps and "bad-tip" not in gaps
    patch_shape_ok = "missing-patch" not in gaps and "bad-patch" not in gaps
    # Scratch fallback (#410): a worktree file matching the claimed digest
    # substitutes for tip resolution (uncommitted work is verifiable).
    worktree_ok = bool(patch_shape_ok and probes.get("patch_worktree_sha_ok"))
    if tip_shape_ok and not probes.get("tip_resolves") and not worktree_ok:
        gaps.append("tip-unresolvable")
    if (tip_shape_ok and patch_shape_ok and probes.get("tip_resolves")
            and not probes.get("patch_at_tip") and not worktree_ok):
        if fields.get("sha256"):
            gaps.append("patch-sha-mismatch")
        else:
            gaps.append("patch-absent-at-tip")
    found = probes.get("symbols_found") or {}
    for sym in syms[:8]:
        if not found.get(sym):
            gaps.append("symbols-missing:%s" % sym[:60])
    return gaps


def ready_gate_verdict(fields, probes, terminal_closeout):
    """ok | grandfathered | reject for an IMPL READY claim. Pure.

    Grandfather covers only the terminal case (dev DONE + audit APPROVED):
    an old artifact-less READY stands because the work is demonstrably
    finished, not because the claim verified."""
    if not ready_artifact_gaps(fields, probes):
        return "ok"
    if terminal_closeout:
        return "grandfathered"
    return "reject"


def ready_probe(ctx, fields):
    """Read-only git verification of a READY claim in the dev checkout. I/O.

    Only rev-parse / cat-file -e / grep run (never status/checkout/fetch);
    worktree reads stay inside the checkout (relative patch, no ..);
    every failure degrades to False (caller NEEDs, never crashes)."""
    probes = {"tip_resolves": False, "patch_at_tip": False,
              "patch_worktree_sha_ok": False, "symbols_found": {}}
    if not fields:
        return probes

    def _grun(argv):
        try:
            return ctx.run(argv, timeout=30, cwd=ctx.dev_dir)
        except OSError:
            return 1, ""
    tip = fields.get("tip") or ""
    patch = fields.get("patch") or ""
    safe_patch = bool(patch) and not patch.startswith(("/", "\\")) \
        and ".." not in patch.split("/") and "\\" not in patch
    syms = re.findall(r"[^\s,]+", fields.get("symbols") or "")[:8]
    if tip and READY_TIP_RE.match(tip):
        rc, _ = _grun(["git", "rev-parse", "--verify", "%s^{commit}" % tip])
        probes["tip_resolves"] = (rc == 0)
    if probes["tip_resolves"] and safe_patch:
        rc, _ = _grun(["git", "cat-file", "-e", "%s:%s" % (tip, patch)])
        probes["patch_at_tip"] = (rc == 0)
    if not probes["patch_at_tip"] and safe_patch and fields.get("sha256"):
        data = _worktree_bytes(ctx, patch)
        probes["patch_worktree_sha_ok"] = bool(
            data is not None and len(data) <= READY_WORKTREE_MAX_BYTES
            and hashlib.sha256(data).hexdigest() == fields["sha256"].lower())
    if syms:
        if probes["tip_resolves"]:
            for sym in syms:
                rc, _ = _grun(["git", "grep", "-w", "-F", "-e", sym, tip, "--"])
                probes["symbols_found"][sym] = (rc == 0)
        elif safe_patch:
            data = _worktree_bytes(ctx, patch)
            if data is not None and len(data) <= READY_WORKTREE_MAX_BYTES:
                text = data.decode("utf-8", "replace")
                for sym in syms:
                    probes["symbols_found"][sym] = bool(
                        re.search(r"\b%s\b" % re.escape(sym), text))
    return probes


def latest_gated_ready(all_posts, full_lines, probes, terminal_closeout=False):
    """Newest dev IMPL READY post with its artifact gate applied. Pure.

    probes is the ready_probe dict for the newest claim (test seam: pass
    canned probes; the tick passes live probes). None when no dev IMPL
    READY post exists; else {"ts","post","fields","gaps","verdict"} with
    ts shaped like dev_impl_ready_ts (post[:20])."""
    best = newest_impl_ready_post(all_posts)
    if best is None:
        return None
    fields = parse_ready_artifacts([best] + post_body_lines(full_lines, best))
    gaps = ready_artifact_gaps(fields, probes)
    return {"ts": best[:20], "post": best, "fields": fields, "gaps": gaps,
            "verdict": ready_gate_verdict(fields, probes, terminal_closeout)}


def gate_impl_ready(ctx, all_posts, full_lines, terminal_closeout):
    """Newest dev IMPL READY with live git probes. I/O (read-only git)."""
    best = newest_impl_ready_post(all_posts)
    if best is None:
        return None
    fields = parse_ready_artifacts([best] + post_body_lines(full_lines, best))
    probes = ready_probe(ctx, fields)
    gaps = ready_artifact_gaps(fields, probes)
    return {"ts": best[:20], "post": best, "fields": fields, "gaps": gaps,
            "verdict": ready_gate_verdict(fields, probes, terminal_closeout)}


def dev_done_ts(posts):
    """Newest dev DONE declaration ts (post[:20]), or None. Pure.

    Terminal DONE is an all-caps word on the POST header (case-sensitive):
    lowercase "done" (as in "IMPL done") is progress prose, not a terminal
    declaration. Reused by the close-out gate (item D)."""
    found = None
    for post in posts or []:
        m = POST_RE.match(post)
        if not m or m.group(2) != "dev":
            continue
        if DONE_WORD_RE.search(m.group(4)):
            found = post[:20]
    return found


def qe_verified_ts(posts):
    """Newest qe VERIFIED declaration ts (post[:20]), or None. Pure.

    Same all-caps convention as dev DONE: lowercase "verified" is prose.
    Consumed by the phase derivation (item C) and the close-out gate."""
    found = None
    for post in posts or []:
        m = POST_RE.match(post)
        if not m or m.group(2) != "qe":
            continue
        if VERIFIED_WORD_RE.search(m.group(4)):
            found = post[:20]
    return found


def parse_qe_evidence(lines):
    """tip: + diff present: y|n from a QE verdict block. Pure."""
    tip, diff = None, None
    for line in lines or []:
        if tip is None:
            m = re.search(r"\btip\s*:\s*([0-9a-f]{7,40})\b", line, re.I)
            if m:
                tip = m.group(1)
        if diff is None:
            m = QE_DIFF_RE.search(line)
            if m:
                diff = m.group(1).lower()
    if tip is None and diff is None:
        return None
    return {"tip": tip, "diff": diff}


def qe_verify_evidence(all_posts, full_lines):
    """Newest qe READY post with its evidence completeness. Pure.

    None when no qe READY post exists; else {"ts","tip","diff",
    "complete"} with complete iff tip: and diff present: both filed."""
    best = None
    for post in all_posts or []:
        m = POST_RE.match(post)
        if not m or m.group(2) != "qe" or m.group(3) != "READY":
            continue
        best = post
    if best is None:
        return None
    ev = parse_qe_evidence([best] + post_body_lines(full_lines, best))
    return {"ts": best[:20], "tip": (ev or {}).get("tip"),
            "diff": (ev or {}).get("diff"),
            "complete": bool(ev and ev.get("tip") and ev.get("diff"))}


def qe_baseline_posted(posts):
    """Whether any qe post carries a baseline tally. Pure.

    A baseline tally is a qe-authored post mentioning baseline plus a
    result token (PASS/PASSED/tally or an n/m count). Proves the parked
    QE ran the loop/build/baseline while waiting for IMPL READY (#460)."""
    for post in posts or []:
        low = post.lower()
        if "baseline" not in low:
            continue
        if ("pass" in low or "tally" in low or re.search(r"\d+\s*/\s*\d+", post)):
            return True
    return False


def qe_baseline_due(qe_posts, impl_ts):
    """Missing-baseline NEED gate pre-IMPL (#460). Pure.

    True when no IMPL READY is on file and no baseline tally is on
    file: parked QE still owes the pipeline work (loop + build +
    baseline + tally), so the engine must NEED it, not idle-approve."""
    if impl_ts:
        return False
    return not qe_baseline_posted(qe_posts)


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


def order_dedupe_action(stored_oid, rings, ready_ts):
    """file | noop | reuse for a proposed repeat order. Pure.

    noop when a standing READY covers the stored order (READY ts at/after
    the stored order ts, or a READY stands with no stored order at all):
    refiling would mint a duplicate oid + ACK cycle for work already done
    (#461 room 240: 21:50 re-ring after the 21:44 READY stood). A stale
    READY (older than the stored order) does not block: the order
    supersedes it. reuse when the stored order is still live (re-ring it,
    never mint a second oid). Keyed on the exact READY ts, not truthiness.
    """
    if ready_ts:
        if not stored_oid:
            return "noop"
        stored_ts = stored_oid.split(":", 1)[1][:19] if ":" in stored_oid else ""
        if not stored_ts or ready_ts[:19] >= stored_ts:
            return "noop"
        return "file"
    if stored_oid:
        val = (rings or {}).get(stored_oid)
        if val is not None and val not in (-1, RING_ESCALATED):
            return "reuse"
    return "file"


def impl_dedupe_note(stored_oid, rings, ready_ts, prev_dedup):
    """(line, store) for a standing READY, or (None, None). Pure.

    Transition-only: the order-dedup line emits once per READY ts while
    the store keys the exact READY, so a newer READY re-opens evaluation.
    The line carries no oid and the caller files no NOTE (log only)."""
    if not ready_ts or order_dedupe_action(stored_oid, rings, ready_ts) != "noop":
        return None, None
    store = {"for": ready_ts}
    if (prev_dedup or {}).get("for") == ready_ts:
        return None, store
    return ("order-dedup dev IMPL: READY @%s stands (no new oid)" % ready_ts,
            store)


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


def lane_facts_from_boxes(boxes, st, role):
    """(order, post, active, waits) engagement facts from loaded boxes. Pure.

    order = unacked coord order here; post = posts newer than the handled
    cursor (first sight reports no fresh, mirroring new_posts); active =
    non-idle NEXT that is not an input-wait; waits = NEXT names an
    input-wait (audit/qe/dev verdicts, orders, rings)."""
    box = (boxes or {}).get(role) or {}
    st = st or {}
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


def room_lane_facts(room_dir, rst, role):
    """(order, post, active, waits) engagement facts for one lane. Pure
    apart from box reads (delegates to lane_facts_from_boxes)."""
    return lane_facts_from_boxes(load_boxes(room_dir), rst, role)


def starvation_gap(role, probe, facts, st, has_unacked):
    """True when an IDLE lane's input-wait has no engine order behind it. Pure.

    The gap that parked 240/390/54: the lane is IDLE, its NEXT waits on
    input, but no unacked order names the lane and no fresh post is in
    flight — every gate had its chance and none fired. Halt rooms never
    starve; unknown probes (unreadable session) never qualify."""
    if (st or {}).get("halt"):
        return False
    if (st or {}).get("state") not in ("IN_PROGRESS", "CLOSING"):
        return False
    facts = facts or {}
    if not facts.get("waits"):
        return False
    if role in (has_unacked or set()):
        return False
    if facts.get("order") or facts.get("post") or facts.get("active"):
        return False
    return str(probe).startswith("IDLE") or str(probe) == "ABSENT"


def starvation_gaps(nodes):
    """Issues whose tick flagged a starved lane, sorted. Pure."""
    return sorted(n.get("issue") for n in (nodes or []) if n.get("starved"))


def starvation_sweep(boxes, st, probes, has_unacked, halt_now):
    """Sorted starved roles for both lanes. Pure apart from inputs.

    Runs on final same-tick state (all filings reflected in has_unacked);
    a halting room never starves (its NEEDs are moot, its quiet moot)."""
    if halt_now:
        return []
    out = []
    for role in ("dev", "qe"):
        if starvation_gap(role, (probes or {}).get(role),
                          lane_facts_from_boxes(boxes, st, role),
                          st, has_unacked):
            out.append(role)
    return out


def derive_room_phase(issue_closed, dev_done, audit_approved, qe_verified,
                      impl_ready):
    """RECON | IMPL | VERIFY | CLOSE-OUT | CLOSED from lane evidence. Pure.

    CLOSED only when the issue is closed (never originated here); CLOSE-OUT
    when the work is demonstrably finished (dev DONE + audit APPROVED);
    VERIFY while a gated READY stands (or qe VERIFIED proves the era);
    IMPL once the plan is approved; RECON otherwise."""
    if issue_closed:
        return "CLOSED"
    if dev_done and audit_approved:
        return "CLOSE-OUT"
    if impl_ready or qe_verified:
        return "VERIFY"
    if audit_approved:
        return "IMPL"
    return "RECON"


def room_pending_text(phase, dev_next, qe_next, audit_planned):
    """One-line pending derived from the latest NEXTs. Pure (cap 200).

    Lane-tag prefixes keep downstream consumers honest (the backlog swap
    guard matches \\bdev\\b). No NEXTs on a virgin RECON names both lanes
    explicitly so the swap guard still holds; other phaseless silences
    report no NEXT rather than inventing one."""
    if phase == "CLOSED":
        return "CLOSED"
    bits = []
    if dev_next:
        bits.append("dev: %s" % dev_next)
    if qe_next:
        bits.append("qe: %s" % qe_next)
    if audit_planned and audit_planned != "-":
        bits.append("audit: %s" % audit_planned)
    if bits:
        return ("%s: %s" % (phase, " | ".join(bits)))[:200]
    if phase == "RECON":
        return "RECON: awaiting dev PLAN + qe approach (no worker posts yet)"
    return "%s: no lane NEXT on file" % phase


def pending_refresh(cur_pending, new_pending, halt, halt_now):
    """Refreshed pending text or the preserved current one. Pure.

    Halt/manual rooms keep operator text unconditionally; otherwise the
    derived text wins (even when identical, returning it is harmless)."""
    if halt or halt_now or (cur_pending or "").startswith("MANUAL_INTERVENTION"):
        return cur_pending
    return new_pending


def watchdog_invariant(starved, engine_live):
    """Violation strings for the engine-heartbeat invariant. Pure.

    Per-room tickers stay disarmed by doctrine; what must never happen is
    starved lanes while the engine heartbeat is gone. Empty means held."""
    if starved and not engine_live:
        return ["engine heartbeat absent with starved lanes: %s"
                % ",".join("#%d" % i for i in sorted(starved))]
    return []


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
    guard = modal_guard(ctx, [SESS["dev"], SESS["qe"], ctx.audit_sess,
                           SESS["consultant"]])
    for sess, acted, detail in guard:
        if acted or "WOULD" in detail:
            actions.append("modal-guard %s: %s" % (sess, detail))
            if sess == ctx.audit_sess and acted:
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

    a_acts, need_verdict, ap_store, as_store, audit_note = auditor_lifecycle(
        ctx, issue, room_dir, st, boxes, fresh, rooms_all)
    actions.extend(a_acts)
    node["audit_planned"] = audit_note
    r_acts, rel_store = release_tick(ctx, issue, boxes, st)
    actions.extend(r_acts)

    dev_next = boxes["dev"]["next"]

    rings = room_rings(st)
    ack_since = st.get("ack_since") or None
    ack_failures = int(st.get("ack_failures", 0) or 0)
    ad_store = {}  # item F (#461): ack-by deadlines filed this tick
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
        if oid.startswith("audit:"):
            continue  # auditor oids die only via explicit ACK (-1) or teardown
        orole, _, ots = oid.partition(":")
        cur = (boxes.get(orole) or {}).get("coord_last")
        if cur and ots < cur[:19]:
            del rings[oid]  # superseded by a newer order
            continue
        if not ack_since or ots < ack_since[:19]:
            posts = boxes.get(orole, {}).get("all", [])
            if posts and posts[-1][:19] > ots:
                del rings[oid]
    # Item F (#461): ACK deadline nudge. Overdue live orders get one lane
    # ring with NO new oid and NO ring-count increment; the lane's ACK
    # clears it via the normal ring pruning (deadline pruned alongside).
    for _oid in ack_overdue_ids(st.get("ack_deadline"), rings, utcnow()):
        _lane = ref_lane(_oid)
        if _lane not in ("dev", "qe"):
            continue
        _info = (st.get("ack_deadline") or {}).get(_oid) or {}
        if not ack_nudge_due(True, _info.get("nudged")):
            continue
        actions.append("ACK-OVERDUE %s (ack-by %s)" % (_oid, _info.get("by")))
        events.append("ack-overdue-%s" % _lane)
        if _lane not in deaf and probes[_lane].startswith("IDLE") \
                and not session_rung(actions, SESS[_lane], cycle["rung"]):
            actions.append(ring(ctx, SESS[_lane], _lane,
                                "ACK overdue -- reply with ACK:%s" % _oid,
                                oid=_oid, issue=issue))
        ad_store[_oid] = {"by": _info.get("by"), "nudged": 1}
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

    # Artifact-gated IMPL READY (item A, #461): a free-text READY claims
    # nothing until branch:/patch:/tip:/stat:/symbols: verify at the tip.
    #Terminal DONE + audit APPROVED grandfathers old artifact-less READYs.
    # The peer-pointer skip below covers exactly the filed-or-due set (S3):
    # a NOTE-FAILED retry stays due (qo_store persists only on success).
    term_closeout = bool(dev_done_ts(boxes["dev"]["all"])
                         and auditor_approved(boxes["auditor"]["all"],
                                              boxes["auditor"].get("full")))
    gated = gate_impl_ready(ctx, boxes["dev"]["all"], boxes["dev"]["full"],
                            term_closeout)
    impl_ts = gated["ts"] if gated and gated["verdict"] in ("ok", "grandfathered") else None
    rn_store = None
    if gated and gated["verdict"] == "reject":
        prev_rn = st.get("ready_need") or {}
        if prev_rn.get("for") != gated["ts"] or prev_rn.get("gaps") != gated["gaps"]:
            actions.append("NEED dev #%d: IMPL READY @%s unverifiable (%s); re-post with branch:/patch:/tip:/stat:/symbols: lines + optional sha256:" % (issue, gated["ts"], ", ".join(gated["gaps"]) or "unknown"))
            events.append("ready-ungated")
        rn_store = {"for": gated["ts"], "gaps": gated["gaps"]}
    qe_ev = qe_verify_evidence(boxes["qe"]["all"], boxes["qe"]["full"])
    qee_store = None
    if impl_ts and qe_ev and not qe_ev["complete"]:
        prev_qee = st.get("qe_evidence_need") or {}
        if prev_qee.get("for") != qe_ev["ts"]:
            actions.append("NEED qe #%d: verdict @%s missing tip:/diff-present: evidence (advisory; verification continues)" % (issue, qe_ev["ts"]))
            events.append("qe-evidence-missing")
        qee_store = {"for": qe_ev["ts"]}
    qe_order_for = (st.get("qe_order") or {}).get("for")
    qe_will_file = bool(impl_ts and qe_order_due(qe_order_for, impl_ts))
    if qe_baseline_due(boxes["qe"]["all"], impl_ts):
        actions.append("NEED qe baseline #%d: no baseline tally on file pre-IMPL; build testbed @ tip, boot QEMU, run baseline suites, post tally with STATE:/NEXT:" % issue)
        events.append("qe-baseline-missing")
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
        note, ioid = coord_note(role, "INTAKE #%d %s first brief -- read prompts/%s.txt + room pending; ACK it with post_update ref=<id> before starting" % (issue, role, role), ack_by=True)
        if ioid in rings:
            actions.append("order deferred %s intake (oid collision)" % role)
            continue
        if ctx.dry:
            actions.append("WOULD post %s intake order (oid %s)" % (role, ioid))
            rings[ioid] = 0
            rings[first_oid] = -1
            has_unacked.add(role)
            file_order_deadline(ad_store, ioid)
            if role == "dev":
                cycle["dev_order"] = issue
        else:
            if append_box_note(room_dir, role, note):
                rings[ioid] = 0
                rings[first_oid] = -1
                has_unacked.add(role)
                file_order_deadline(ad_store, ioid)
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
        note, boid = coord_note("dev", "BACKLOG assignment #%d (this room #%d): read prompts/dev-%d-backlog.txt; ACK it with post_update ref=<id> before starting" % (target, issue, target), ack_by=True)
        if ctx.dry:
            actions.append("WOULD post backlog order #%d (oid %s)" % (target, boid))
            file_order_deadline(ad_store, boid)
            has_unacked.add("dev")
        else:
            if append_box_note(room_dir, "dev", note):
                rings[boid] = 0
                file_order_deadline(ad_store, boid)
                has_unacked.add("dev")
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
    dd_store = None
    # Item E (#461): a standing (gated) READY covers any promotion — the
    # NEXT-change reset below must not re-ring work already done (room 240:
    # 21:50 re-ring after the 21:44 READY stood). Log once per READY ts,
    # file nothing, ring nothing.
    _dd_line, dd_store = impl_dedupe_note(stored_impl.get("oid"), rings,
                                          impl_ts, st.get("impl_dedup"))
    if _dd_line or dd_store:
        if _dd_line:
            actions.append(_dd_line)
            events.append("dev-impl-deduped")
    else:
        if stored_impl and stored_impl.get("next") != (boxes["dev"]["next"] or ""):
            di_store = {}
    _dev_next = boxes["dev"]["next"] or ""
    _approved = auditor_approved(boxes["auditor"]["all"], boxes["auditor"].get("full"))
    if not dd_store and impl_promotion_due(_dev_next, bool(boxes["dev"]["all"]),
                                          fresh.get("dev"), _approved, rings, has_unacked,
                                          stored_impl):
        note, di_oid = coord_note("dev", "IMPL promotion #%d: plan approved (auditor APPROVED on file); start IMPL per room briefs/notes; ACK it with post_update ref=<id> before starting" % issue, ack_by=True)
        if di_oid in rings:
            actions.append("order deferred dev IMPL (oid collision)")
        elif ctx.dry:
            actions.append("WOULD post dev IMPL order (oid %s)" % di_oid)
            rings[di_oid] = 0
            has_unacked.add("dev")
            file_order_deadline(ad_store, di_oid)
            di_store = {"next": _dev_next, "oid": di_oid}
            cycle["dev_order"] = issue
        elif append_box_note(room_dir, "dev", note):
            rings[di_oid] = 0
            has_unacked.add("dev")
            file_order_deadline(ad_store, di_oid)
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
    elif not dd_store and impl_stalled(_dev_next, bool(boxes["dev"]["all"]), fresh.get("dev"),
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
        note, qoid = coord_note("qe", "IMPL READY #%d dev %s — start verification per prompts/qe.txt + room pending; ACK it with post_update ref=<id> before starting" % (issue, impl_ts), ack_by=True)
        if ctx.dry:
            actions.append("WOULD post qe order IMPL READY (oid %s)" % qoid)
            file_order_deadline(ad_store, qoid)
            has_unacked.add("qe")
        else:
            if append_box_note(room_dir, "qe", note):
                rings[qoid] = 0
                file_order_deadline(ad_store, qoid)
                has_unacked.add("qe")
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

    dec = auditor_decision(boxes["auditor"]["all"], boxes["auditor"].get("full"))
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
    # Item C (#461): starvation invariant on final same-tick state. A lane
    # still waiting on input with no order behind it gets a log-only NEED
    # (the gate that should have fired owns the actual order — no filing,
    # no ring, no double-drive). Halting rooms never starve; node["starved"]
    # keeps the cycle from counting this as quiet.
    _starved_roles = starvation_sweep(boxes, st, probes, has_unacked, halt_now)
    for _role in _starved_roles:
        actions.append("NEED starvation #%d %s: %s, NEXT '%s', no order"
                       % (issue, _role, probes[_role],
                          (boxes[_role]["next"] or "")[:80]))
    node["starved"] = _starved_roles
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
        if isinstance(as_store, dict) and as_store:
            s["audit_session"] = as_store
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
        if dd_store:
            s["impl_dedup"] = dd_store
        else:
            s.pop("impl_dedup", None)
        if rn_store:
            s["ready_need"] = rn_store
        else:
            s.pop("ready_need", None)
        if qee_store:
            s["qe_evidence_need"] = qee_store
        else:
            s.pop("qe_evidence_need", None)
        if ad_store:
            merged_ad = dict(s.get("ack_deadline") or {})
            merged_ad.update(ad_store)
            s["ack_deadline"] = merged_ad
        if isinstance(s.get("ack_deadline"), dict):
            kept = prune_deadlines(s["ack_deadline"], rings)
            if kept:
                s["ack_deadline"] = kept
            else:
                s.pop("ack_deadline", None)
        # Item C (#461): derived phase/state/pending. The audit bit rides
        # only on an outstanding verdict need — an unconditional audit line
        # would make audit_state_demand fire on every tick (S1: auditor
        # churn). Halt/manual rooms keep operator text, always.
        # Halted rooms freeze everything below (phase/state/pending), not
        # just the pending text: a halting room takes no new derivation.
        if not s.get("halt") and not halt_now:
            _dev_done = dev_done_ts(boxes["dev"]["all"])
            _qe_verified = qe_verified_ts(boxes["qe"]["all"])
            _phase = derive_room_phase(st.get("phase") == "CLOSED", _dev_done,
                                       _approved, _qe_verified, impl_ts)
            s["phase"] = _phase
            if _phase == "CLOSE-OUT" and s.get("state") == "IN_PROGRESS":
                s["state"] = "CLOSING"
            _cur_pending = s.get("pending") or ""
            _new_pending = room_pending_text(
                _phase, boxes["dev"]["next"], boxes["qe"]["next"],
                audit_note if need_verdict else None)
            if _new_pending != _cur_pending:
                s["pending"] = pending_refresh(_cur_pending, _new_pending,
                                               s.get("halt"), halt_now)
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
        else:
            _odd = st.get("ack_deadline") or {}
            _opairs = [(o, (_odd.get(o) or {}).get("by"))
                       for o in ack_overdue_ids(_odd, rings, utcnow())]
            write_ack_overdue(room_dir, _opairs)

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
                auditor_decision(boxes["auditor"]["all"], boxes["auditor"].get("full")) or "no verdict")
            planned = node.get("audit_planned") or ("collect verdict" if need_verdict else "-")
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
    dec = auditor_decision(boxes["auditor"]["all"], boxes["auditor"].get("full")) or "none on file"
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
    "order filed", "WAKE ", "audit ring ", "auditor teardown ", "audit brief ")


def cycle_productive(all_actions):
    """True if the cycle did (or, dry, would do) anything beyond passive notes."""
    return any(a.startswith(PRODUCTIVE_PREFIXES) for a in all_actions)


def cycle_progress(all_actions, room_nodes, dry=False):
    """True if the cycle shows real work (engine acts OR worker activity).

    Coordinator/coord.md lines are excluded by construction: BOXES covers
    only dev/qe/auditor and the watchdog fingerprint counts only
    FROM=(dev|qe|audit). ESCALATE/NEED alone are intentionally not progress.
    Starved lanes (item C, #461) are progress: the engine named work and
    must stay alive for it. Dry mode preserves legacy exit semantics (no
    watchdog run, no cursor advance, so frozen signals must not pin quiet
    at 0).
    """
    if cycle_productive(all_actions):
        return True
    if not dry:
        for node in room_nodes or []:
            if node.get("starved"):
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
            _starved_here = starvation_gaps(cycle_nodes)
            if _starved_here:
                ctx.log("NEED cycle starvation: %s (quiet suppressed)"
                        % ",".join("#%d" % i for i in _starved_here))
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


def status_planned_audit(ctx, issue, boxes, st):
    """Next-step read for the audit lane. Read-only (existing readers)."""
    boxes = boxes or {}
    st = st or {}
    verdict = bool(auditor_decision((boxes.get("auditor") or {}).get("all", []), (boxes.get("auditor") or {}).get("full", [])))
    report = bool(latest_audit_report(ctx, issue))
    live = audit_session_live(ctx)
    stored = bool(st.get("audit_pending"))
    demand = audit_demand(boxes, st, None)
    qhead = audit_queue_head(issue, demand, None)
    return audit_planned_text(verdict, report, live, stored, demand, qhead, issue)


def cmd_status(ctx, issues):
    rooms = discover_rooms(issues)
    if not rooms:
        print("no rooms")
        return 2
    facts = {"dev": lane_facts_all(rooms, "dev"),
             "qe": lane_facts_all(rooms, "qe")}
    starved_all = []
    for issue, rdir, st in rooms:
        boxes = load_boxes(rdir)
        hu = {r for r in ("dev", "qe") if any(
            o.startswith(r + ":") and v not in (-1, RING_ESCALATED)
            for o, v in (st.get("rings") or {}).items())}
        rows = []
        for role, sess in (("dev", SESS["dev"]), ("qe", SESS["qe"]),
                           ("audit", SESS["audit"])):
            if role == "audit":
                rep = latest_audit_report(ctx, issue)
                activity = ("report %s" % os.path.basename(rep)) if rep else (
                    auditor_decision(boxes["auditor"]["all"], boxes["auditor"].get("full")) or "no verdict")
                state = audit_state(audit_session_live(ctx))
                rows.append((role, state, activity,
                             status_planned_audit(ctx, issue, boxes, st)))
            else:
                probe = fast_probe(ctx, sess)
                activity = boxes[role]["state"] or boxes[role]["next"] or "-"
                rows.append((role, lane_state(probe, facts[role].get(issue, {}),
                                             facts[role], issue), activity,
                             status_planned_lane(role, boxes[role], st, probe)))
                if starvation_gap(role, probe, facts[role].get(issue, {}), st, hu):
                    starved_all.append(issue)
        print(fmt_table(issue, {"kind": "STATUS", "rest": (st.get("pending") or "")[:60]},
                        rows), flush=True)
    for _v in watchdog_invariant(sorted(set(starved_all)), engine_loop_live()):
        print(_v, flush=True)
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
                "No diff-verification runs until dev posts IMPL READY. While parked: build testbed @ tip, boot QEMU, run the baseline suites from your approach, post the tally with STATE:/NEXT: trailers.\n"
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
    check("ack-by-adds-sla",
          ack_by_ts("2026-11-02T21:28:52") == "2026-11-02T21:43:52Z", True)
    check("ack-by-bad-ts-none",
          ack_by_ts("garbage") is None and ack_by_ts("") is None
          and ack_by_ts(None) is None, True)
    check("ack-overdue-true",
          ack_overdue("2026-11-02T21:43:52Z", "2026-11-02T21:43:53Z"), True)
    check("ack-overdue-false",
          ack_overdue("2026-11-02T21:43:52Z", "2026-11-02T21:43:52Z") is False
          and ack_overdue("2026-11-02T21:43:52Z", "2026-11-02T21:00:00Z") is False
          and ack_overdue(None, "2026-11-02T21:43:53Z") is False, True)
    check("ack-nudge-once",
          ack_nudge_due(True, 0) is True and ack_nudge_due(True, 1) is False
          and ack_nudge_due(False, 0) is False, True)
    check("ack-deadline-pruned-on-ack",
          prune_deadlines({"a": {"by": "x"}, "b": {"by": "y"}},
                          {"a": -1, "b": 1}) == {"b": {"by": "y"}}, True)
    _odl = {"o1": {"by": "2026-11-02T21:00:00Z", "nudged": 0},
            "o2": {"by": "2026-11-02T23:00:00Z", "nudged": 0},
            "o3": {"nudged": 0},
            "o4": {"by": "2026-11-02T21:00:00Z", "nudged": 0}}
    check("ack-overdue-ids",
          ack_overdue_ids(_odl, {"o1": 1, "o2": 1, "o3": 1, "o4": -1},
                          "2026-11-02T22:00:00Z") == ["o1"], True)
    _rl = ring_line("dev", "x", oid="dev:2026-11-02T08:00:00", issue=99,
                    ack_by="2026-11-02T08:15:00Z")
    check("ring-line-ack-by-keeps-oid-last",
          "[oid dev:2026-11-02T08:00:00] -- read inbox/dev.md" in _rl
          and _rl.index(" ack-by 2026-11-02T08:15:00Z ") < _rl.index(" [oid "), True)
    _cn, _coid = coord_note("dev", "x", ack_by=True)
    check("coord-note-ack-by-keeps-oid-last",
          _cn.endswith("[oid %s]" % _coid) and " ack-by " in _cn, True)
    _cn0, _coid0 = coord_note("dev", "x")
    check("coord-note-legacy-no-ack-by", "ack-by" not in _cn0, True)
    _ad = {}
    check("file-order-deadline",
          file_order_deadline(_ad, "dev:2026-11-02T21:28:52") == {"dev:2026-11-02T21:28:52": {"by": "2026-11-02T21:43:52Z", "nudged": 0}}, True)
    check("file-order-deadline-bad-oid",
          file_order_deadline({}, "bogus") == {}, True)
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
    _full = ["2026-11-02T08:03:09Z FROM=dev READY :: IMPL step-a READY",
             "STATE: working :: step-a done",
             "branch: fix/461-a",
             "patch: scripts/state_machine.py",
             "tip: a4d6fdf26",
             "stat: 3 files, 120 insertions",
             "symbols: tick_room, ready_probe",
             "NEXT: waiting auditor IMPL audit",
             "2026-11-02T09:00:00Z FROM=dev NOTE :: later"]
    _rdy_post = _full[0]
    check("ready-parse-body-window",
          post_body_lines(_full, _rdy_post) == _full[1:8], True)
    check("ready-parse-body-stops-at-next-post",
          "2026-11-02T09:00:00Z FROM=dev NOTE :: later" not in post_body_lines(_full, _rdy_post), True)
    check("ready-parse-body-missing-post",
          post_body_lines(_full, "2026-11-02T00:00:00Z FROM=dev NOTE :: nope") == [], True)
    _fields = parse_ready_artifacts([_rdy_post] + post_body_lines(_full, _rdy_post))
    check("ready-parse-all",
          _fields == {"branch": "fix/461-a", "patch": "scripts/state_machine.py",
                      "tip": "a4d6fdf26", "stat": "3 files, 120 insertions",
                      "symbols": "tick_room, ready_probe"}, True)
    check("ready-parse-no-artifacts",
          parse_ready_artifacts(["STATE: x", "NEXT: waiting auditor"]) is None, True)
    _part = parse_ready_artifacts(["branch: b", "patch: p.py", "tip: abc1234", "stat: 1/2"])
    check("ready-parse-missing-symbols",
          "missing-symbols" in ready_artifact_gaps(_part, None), True)
    check("ready-parse-absolute-patch",
          "bad-patch" in ready_artifact_gaps(parse_ready_artifacts(
              ["branch: b", "patch: /abs/p.py", "tip: abc1234", "stat: 1/2",
               "symbols: s"]), None), True)
    check("ready-parse-bad-tip",
          "bad-tip" in ready_artifact_gaps(parse_ready_artifacts(
              ["branch: b", "patch: p.py", "tip: xyz", "stat: 1/2",
               "symbols: s"]), None), True)
    check("ready-parse-stat-slash",
          ready_artifact_gaps(parse_ready_artifacts(
              ["branch: b", "patch: p.py", "tip: abc1234", "stat: 10/25",
               "symbols: s"]), None) == [], True)
    _ok_probes = {"tip_resolves": True, "patch_at_tip": True,
                  "patch_worktree_sha_ok": False,
                  "symbols_found": {"tick_room": True, "ready_probe": True}}
    check("ready-gate-ok",
          ready_gate_verdict(_fields, _ok_probes, False) == "ok", True)
    check("ready-gate-missing-field",
          ready_gate_verdict(_part, None, False) == "reject"
          and ready_artifact_gaps(_part, None) == ["missing-symbols"], True)
    _vf = {"branch": "b", "patch": "p.py", "tip": "abc1234", "stat": "1/2",
           "symbols": "s"}
    _mm = {"tip_resolves": True, "patch_at_tip": False,
           "patch_worktree_sha_ok": False, "symbols_found": {"s": True}}
    check("ready-gate-patch-absent",
          "patch-absent-at-tip" in ready_artifact_gaps(_vf, _mm)
          and ready_gate_verdict(_vf, _mm, False) == "reject", True)
    check("ready-gate-untracked-no-sha",
          ready_artifact_gaps(_vf, _mm) == ["patch-absent-at-tip"], True)
    _sm = {"tip_resolves": True, "patch_at_tip": True,
           "patch_worktree_sha_ok": False,
           "symbols_found": {"tick_room": True, "ready_probe": False}}
    check("ready-gate-symbol-missing",
          "symbols-missing:ready_probe" in ready_artifact_gaps(_fields, _sm)
          and ready_gate_verdict(_fields, _sm, False) == "reject", True)
    check("ready-gate-terminal-grandfather",
          ready_gate_verdict(None, None, True) == "grandfathered", True)
    check("ready-gate-terminal-reject",
          ready_gate_verdict(None, None, False) == "reject", True)
    _lg = latest_gated_ready([_rdy_post], _full, _ok_probes, False) or {}
    check("ready-latest-ok",
          _lg.get("ts") == "2026-11-02T08:03:09Z" and _lg.get("verdict") == "ok", True)
    check("ready-latest-none",
          latest_gated_ready(["2026-11-02T08:03:09Z FROM=dev NOTE :: x"],
                             _full, None, False) is None, True)
    _two = ["2026-11-02T08:03:09Z FROM=dev READY :: IMPL first",
            "2026-11-02T08:20:00Z FROM=dev READY :: IMPL second"]
    _lg2 = latest_gated_ready(_two, _two, None, False) or {}
    check("ready-latest-newest",
          _lg2.get("ts") == "2026-11-02T08:20:00Z" and _lg2.get("verdict") == "reject"
          and _lg2.get("gaps") == ["no-artifacts"], True)
    check("dev-done-detected",
          dev_done_ts(["2026-11-02T08:03:09Z FROM=dev READY :: step-a DONE + APPROVED"]) == "2026-11-02T08:03:09Z", True)
    check("dev-done-lowercase-ignored",
          dev_done_ts(["2026-11-02T08:20:00Z FROM=dev READY :: IMPL done"]) is None, True)
    check("dev-done-wrong-sender",
          dev_done_ts(["2026-11-02T08:03:09Z FROM=qe READY :: DONE"]) is None, True)
    _qfull = ["2026-11-02T08:03:09Z FROM=qe READY :: VERIFY done",
              "tip: a4d6fdf26", "diff present: y"]
    _qev = qe_verify_evidence(["2026-11-02T08:03:09Z FROM=qe READY :: VERIFY done"], _qfull) or {}
    check("qe-evidence-ok",
          _qev.get("complete") and _qev.get("tip") == "a4d6fdf26" and _qev.get("diff") == "y", True)
    _qpart = qe_verify_evidence(["2026-11-02T08:03:09Z FROM=qe READY :: VERIFY done"],
                                ["2026-11-02T08:03:09Z FROM=qe READY :: VERIFY done",
                                 "diff present: n"]) or {}
    check("qe-evidence-partial-no-tip",
          _qpart.get("complete") is False and _qpart.get("tip") is None
          and _qpart.get("diff") == "n", True)
    check("qe-evidence-none",
          qe_verify_evidence(["2026-11-02T08:03:09Z FROM=qe NOTE :: x"], []) is None, True)

    def _git_run(argv, timeout=120, cwd=None):
        try:
            pr = subprocess.run(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL,
                                timeout=timeout, text=True, cwd=cwd)
            return pr.returncode, pr.stdout
        except (OSError, subprocess.TimeoutExpired):
            return 1, ""
    _repo = default_repo()
    _pctx = SimpleNamespace(dev_dir=_repo, run=_git_run)
    _prc, _phead = _git_run(["git", "rev-parse", "HEAD"], timeout=30, cwd=_repo)
    _phead = (_phead or "").strip()
    _git_ok = bool(_prc == 0 and _phead)
    _pfull = {"tip": _phead, "patch": "scripts/state_machine.py",
              "symbols": "tick_room"}
    _pprobes = ready_probe(_pctx, _pfull)
    check("ready-probe-tip-resolves",
          (not _git_ok) or _pprobes["tip_resolves"], True)
    check("ready-probe-patch-at-tip",
          (not _git_ok) or _pprobes["patch_at_tip"], True)
    check("ready-probe-symbol-found",
          (not _git_ok) or _pprobes["symbols_found"].get("tick_room"), True)
    # Canary built by concatenation so the full value never occurs in any
    # tracked file (git grep at the tip would otherwise match this source).
    _canary = "zzz_absent_" + "canary_q7x2"
    _bad = ready_probe(_pctx, {"tip": "deadbee", "patch": "scripts/state_machine.py",
                               "symbols": _canary})
    check("ready-probe-bad-tip", _bad["tip_resolves"] is False, True)
    # Canary built by concatenation so the full string never occurs in any
    # tracked file (git grep at the tip would otherwise match this source).
    _canary = "zzz_absent_" + "canary_q7x2"
    _miss = ready_probe(_pctx, {"tip": _phead, "patch": "no/such/file.xyz",
                                "symbols": _canary})
    check("ready-probe-patch-missing", _miss["patch_at_tip"] is False, True)
    check("ready-probe-symbol-missing",
          (not _git_ok) or _miss["symbols_found"].get(_canary) is False, True)
    _td = tempfile.mkdtemp(prefix="rdy-")
    try:
        with open(os.path.join(_td, "note.txt"), "w") as _f:
            _f.write("alpha tick_room beta\n")
        with open(os.path.join(_td, "note.txt"), "rb") as _f:
            _dg = hashlib.sha256(_f.read()).hexdigest()
        _tctx = SimpleNamespace(dev_dir=_td, run=_git_run)
        _wfields = {"branch": "fix/461-a", "tip": "deadbee", "patch": "note.txt",
                    "stat": "1/1", "sha256": _dg, "symbols": "tick_room"}
        _wprobes = ready_probe(_tctx, _wfields)
        check("ready-probe-worktree-sha-ok", _wprobes["patch_worktree_sha_ok"], True)
        check("ready-probe-worktree-symbol",
              _wprobes["symbols_found"].get("tick_room"), True)
        check("ready-probe-sha-mismatch",
              ready_probe(_tctx, dict(_wfields, sha256="0" * 64))["patch_worktree_sha_ok"] is False, True)
        check("ready-gate-worktree-ok",
              ready_artifact_gaps(_wfields, _wprobes) == [], True)
    finally:
        try:
            os.unlink(os.path.join(_td, "note.txt"))
        except OSError:
            pass
        try:
            os.rmdir(_td)
        except OSError:
            pass
    _dup = ["2026-11-02T08:03:09Z FROM=dev READY :: IMPL x",
            "branch: old",
            "2026-11-02T08:03:09Z FROM=dev READY :: IMPL x",
            "branch: new"]
    check("ready-parse-body-last-match",
          post_body_lines(_dup, "2026-11-02T08:03:09Z FROM=dev READY :: IMPL x") == ["branch: new"], True)
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
    _cboxes = {"dev": {"all": [], "coord_last": None, "next": "waiting auditor IMPL audit"},
               "qe": {"all": [], "coord_last": None, "next": "idle :: waiting"}}
    _cst = {"handled": {}, "rings": {}, "ack_since": None, "state": "IN_PROGRESS"}
    check("lane-facts-delegates",
          lane_facts_from_boxes(_cboxes, _cst, "dev") == {"order": False, "post": False, "active": False, "waits": True}
          and lane_facts_from_boxes(_cboxes, _cst, "qe")["waits"] is False, True)
    _cwait = {"order": False, "post": False, "active": False, "waits": True}
    check("starvation-gap-idle-wait",
          starvation_gap("dev", "IDLE-static", _cwait, _cst, set()), True)
    check("starvation-gap-absent-wait",
          starvation_gap("qe", "ABSENT", dict(_cwait, waits=True), _cst, set()), True)
    check("starvation-gap-excludes-order",
          starvation_gap("dev", "IDLE-static", dict(_cwait, order=True), _cst, set()) is False, True)
    check("starvation-gap-excludes-post",
          starvation_gap("dev", "IDLE-static", dict(_cwait, post=True), _cst, set()) is False, True)
    check("starvation-gap-excludes-active",
          starvation_gap("dev", "IDLE-static", dict(_cwait, active=True), _cst, set()) is False, True)
    check("starvation-gap-excludes-unacked",
          starvation_gap("dev", "IDLE-static", _cwait, _cst, {"dev"}) is False, True)
    check("starvation-gap-excludes-working",
          starvation_gap("dev", "WORKING-markers", _cwait, _cst, set()) is False, True)
    check("starvation-gap-excludes-halt",
          starvation_gap("dev", "IDLE-static", _cwait, dict(_cst, halt=True), set()) is False, True)
    check("starvation-gap-excludes-no-wait",
          starvation_gap("dev", "IDLE-static", dict(_cwait, waits=False), _cst, set()) is False, True)
    check("starvation-gap-excludes-other-state",
          starvation_gap("dev", "IDLE-static", _cwait, dict(_cst, state="HOLD"), set()) is False, True)
    check("starvation-gaps-sorted",
          starvation_gaps([{"issue": 54}, {"issue": 240, "starved": ["qe"]},
                           {"issue": 390, "starved": ["dev"]}]) == [240, 390], True)
    check("starvation-sweep-finds",
          starvation_sweep(_cboxes, _cst, {"dev": "IDLE-static", "qe": "IDLE-static"}, set(), False) == ["dev"], True)
    check("starvation-sweep-halt-suppresses",
          starvation_sweep(_cboxes, dict(_cst, halt=True), {"dev": "IDLE-static", "qe": "IDLE-static"}, set(), False) == []
          and starvation_sweep(_cboxes, _cst, {"dev": "IDLE-static", "qe": "IDLE-static"}, set(), True) == [], True)
    check("phase-recon", derive_room_phase(False, None, False, None, None) == "RECON", True)
    check("phase-impl", derive_room_phase(False, None, True, None, None) == "IMPL", True)
    check("phase-verify-ready",
          derive_room_phase(False, None, True, None, "2026-11-02T08:03:09Z") == "VERIFY", True)
    check("phase-verify-qe",
          derive_room_phase(False, None, False, "2026-11-02T08:03:09Z", None) == "VERIFY", True)
    check("phase-close-out",
          derive_room_phase(False, "2026-11-02T08:03:09Z", True, None, None) == "CLOSE-OUT", True)
    check("phase-closed",
          derive_room_phase(True, "2026-11-02T08:03:09Z", True, "2026-11-02T08:03:09Z", "2026-11-02T08:03:09Z") == "CLOSED", True)
    check("pending-from-next",
          room_pending_text("IMPL", "waiting IMPL order", "idle :: waiting", "ring audit lane") == "IMPL: dev: waiting IMPL order | qe: idle :: waiting | audit: ring audit lane", True)
    check("pending-no-audit-bit",
          room_pending_text("IMPL", "waiting IMPL order", None, None) == "IMPL: dev: waiting IMPL order", True)
    check("pending-virgin-names-lanes",
          room_pending_text("RECON", None, None, None) == "RECON: awaiting dev PLAN + qe approach (no worker posts yet)", True)
    check("pending-closed", room_pending_text("CLOSED", "x", "y", "z") == "CLOSED", True)
    check("pending-cap-200",
          len(room_pending_text("IMPL", "x" * 300, None, None)) == 200, True)
    check("pending-preserves-manual",
          pending_refresh("MANUAL_INTERVENTION: x", "IMPL: y", False, False) == "MANUAL_INTERVENTION: x"
          and pending_refresh("old", "new", True, False) == "old"
          and pending_refresh("old", "new", False, True) == "old"
          and pending_refresh("old", "new", False, False) == "new", True)
    check("watchdog-invariant-engine-absent",
          watchdog_invariant([240], False) == ["engine heartbeat absent with starved lanes: #240"], True)
    check("watchdog-invariant-held-live",
          watchdog_invariant([240], True) == [], True)
    check("watchdog-invariant-held-clean",
          watchdog_invariant([], False) == [], True)
    check("cmd-tick-quiet-suppressed",
          cycle_progress(["NOTE idle"], [{"verdict": {"kind": "UNCHANGED"}, "acked": 0, "starved": ["dev"]}], dry=False) is True
          and cycle_progress(["NOTE idle"], [{"verdict": {"kind": "UNCHANGED"}, "acked": 0, "starved": ["dev"]}], dry=True) is False
          and cycle_progress(["NOTE idle"], [{"verdict": {"kind": "UNCHANGED"}, "acked": 0}], dry=False) is False, True)
    check("qe-verified-detected",
          qe_verified_ts(["2026-11-02T08:03:09Z FROM=qe READY :: step-a VERIFIED"]) == "2026-11-02T08:03:09Z", True)
    check("qe-verified-lowercase-ignored",
          qe_verified_ts(["2026-11-02T08:03:09Z FROM=qe NOTE :: verified-ish"]) is None, True)
    check("qe-verified-wrong-sender",
          qe_verified_ts(["2026-11-02T08:03:09Z FROM=dev READY :: VERIFIED"]) is None, True)
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
    check("audit-trailer-ts",
          audit_trailer_verdict(["2026-11-02T10:01:22Z FROM=audit READY :: verdict ready",
                                 "STATE: x", "NEXT: y", "DECISION: APPROVED step-a"]) == ("2026-11-02T10:01:22", "DECISION: APPROVED step-a"), True)
    check("audit-trailer-none",
          audit_trailer_verdict(["2026-11-02T10:01:22Z FROM=audit READY :: x", "STATE: x"]) == (None, None), True)
    check("audit-verdict-ts-full-trailer",
          audit_verdict_ts_full([], ["2026-11-02T10:01:22Z FROM=audit READY :: x", "DECISION: NEED rework"]) == "2026-11-02T10:01:22", True)
    check("audit-unhandled-first-sight",
          audit_unhandled_verdict(["2026-11-02T10:01:22Z FROM=audit READY :: x"],
                                  ["2026-11-02T10:01:22Z FROM=audit READY :: x", "DECISION: APPROVED y"],
                                  "")[0] == "2026-11-02T10:01:22", True)
    check("audit-unhandled-past",
          audit_unhandled_verdict(["2026-11-02T10:01:22Z FROM=audit DECISION :: APPROVED"],
                                  [], "2026-11-02T10:01:22Z") == (None, None), True)
    check("auditor-decision-trailer",
          "DECISION: APPROVED" in (auditor_decision([], ["2026-11-02T10:01:22Z FROM=audit READY :: x", "DECISION: APPROVED y"]) or ""), True)
    check("auditor-approved-trailer",
          auditor_approved([], ["2026-11-02T10:01:22Z FROM=audit READY :: x", "DECISION: APPROVED y"]) is True, True)
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
    _r240 = "2026-11-02T21:44:17Z"
    _o240 = "dev:2026-11-02T21:28:52"
    check("dedupe-ready-stands",
          order_dedupe_action(_o240, {}, _r240) == "noop", True)
    check("dedupe-new-ready",
          order_dedupe_action("dev:2026-11-02T21:50:02", {}, _r240) == "file", True)
    check("dedupe-live-ring-reuse",
          order_dedupe_action(_o240, {_o240: 1}, None) == "reuse", True)
    check("dedupe-resolved-ring-files",
          order_dedupe_action(_o240, {_o240: -1}, None) == "file", True)
    _ddl, _dds = impl_dedupe_note(_o240, {}, _r240, {})
    check("dedupe-noop-emits-no-oid",
          _ddl is not None and "[oid" not in _ddl
          and "no new oid" in _ddl and _dds == {"for": _r240}, True)
    _ddl2, _dds2 = impl_dedupe_note(_o240, {}, _r240, {"for": _r240})
    check("dedupe-transition-only",
          _ddl2 is None and _dds2 == {"for": _r240}, True)
    _ddl3, _dds3 = impl_dedupe_note(_o240, {}, "2026-11-02T22:36:21Z", {"for": _r240})
    check("dedupe-newer-ready-reopens",
          _ddl3 is not None and _dds3 == {"for": "2026-11-02T22:36:21Z"}, True)
    check("dedupe-no-ready-no-note",
          impl_dedupe_note(_o240, {}, None, {}) == (None, None), True)
    check("impl-due-ready-ts-suppressed",
          impl_promotion_due(_iw, True, [], True, {}, set(), {}) is True
          and order_dedupe_action(None, {}, "2026-11-02T08:03:09Z") == "noop", True)
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
    check("qe-baseline-missing-due",
          qe_baseline_due([], None) is True, True)
    check("qe-baseline-posted-not-due",
          qe_baseline_due(["2026-11-02T08:00:00Z FROM=qe NOTE :: baseline capability 155/155 PASSED STATE: idle NEXT: idle"],
                          None) is False, True)
    check("qe-baseline-impl-suppresses",
          qe_baseline_due([], "2026-11-02T08:03:09Z") is False, True)
    check("qe-baseline-posted-true",
          qe_baseline_posted(["2026-11-02T08:00:00Z FROM=qe NOTE :: baseline 155/155 tally"]) is True, True)
    check("qe-baseline-posted-false",
          qe_baseline_posted(["2026-11-02T08:00:00Z FROM=qe NOTE :: approach only"]) is False, True)
    check("qe-brief-narrows-gate",
          "No diff-verification runs until dev posts IMPL READY." in issue_briefs_text(1, "b", "t", "x")[1]
          and "While parked:" in issue_briefs_text(1, "b", "t", "x")[1], True)
    from types import SimpleNamespace
    check("audit-demand-fresh",
          audit_demand({"dev": {"all": ["2026-11-02T08:00:00Z FROM=dev NEED :: audit x"]}, "qe": {"all": []}},
                       {}, {"dev": ["2026-11-02T08:00:00Z FROM=dev NEED :: audit x"]}) == {"requester": "dev", "text": "2026-11-02T08:00:00Z FROM=dev NEED :: audit x"[-200:]}, True)
    check("audit-demand-pending",
          audit_demand({"dev": {"all": []}, "qe": {"all": []}}, {"pending": "awaiting audit review"}, None) == {"requester": "dev", "text": "awaiting audit review"}, True)
    check("audit-demand-phase",
          audit_demand({"dev": {"all": []}, "qe": {"all": []}}, {"phase": "AUDIT", "pending": "x"}, None) == {"requester": "dev", "text": "x"}, True)
    check("audit-demand-none",
          audit_demand({"dev": {"all": []}, "qe": {"all": []}}, {"pending": "plain work"}, None) is None, True)
    check("audit-demand-key-stable",
          audit_demand_key("dev", "same text") == audit_demand_key("dev", "same text"), True)
    check("audit-demand-key-differs",
          audit_demand_key("dev", "text a") != audit_demand_key("dev", "text b"), True)
    check("audit-brief-tip-missing",
          audit_brief_tip("/nonexistent-999/brief.txt") is None, True)
    check("audit-state-demand-pending",
          audit_state_demand({"pending": "awaiting audit review"}) == {"requester": "dev", "text": "awaiting audit review"}, True)
    check("audit-state-demand-phase",
          audit_state_demand({"phase": "AUDIT", "pending": "x"}) == {"requester": "dev", "text": "x"}, True)
    check("audit-state-demand-none",
          audit_state_demand({"pending": "plain work"}) is None, True)
    check("audit-fresh-need-newest",
          audit_fresh_need({"dev": ["2026-11-02T08:00:00Z FROM=dev NEED :: audit a"],
                            "qe": ["2026-11-02T09:00:00Z FROM=qe NEED :: audit b"]}) == {"requester": "qe", "text": "2026-11-02T09:00:00Z FROM=qe NEED :: audit b"[-200:]}, True)
    check("audit-fresh-need-none",
          audit_fresh_need({"dev": ["2026-11-02T08:00:00Z FROM=dev NOTE :: x"], "qe": []}) is None, True)
    import tempfile as _st_tmp
    _st_fd, _st_path = _st_tmp.mkstemp(prefix="audit-brief-tip-")
    try:
        with os.fdopen(_st_fd, "w") as _st_f:
            _st_f.write("# room inbox\ntip: abc1234 some scope\n")
        check("audit-brief-tip-present", audit_brief_tip(_st_path) == "abc1234", True)
    finally:
        try:
            os.unlink(_st_path)
        except OSError:
            pass
    check("audit-verdict-ts",
          audit_verdict_ts(["2026-11-02T08:00:00Z FROM=auditor NOTE :: x",
                            "2026-11-02T09:00:00Z FROM=audit DECISION :: APPROVED"]) == "2026-11-02T09:00:00", True)
    check("audit-verdict-ts-none", audit_verdict_ts(["2026-11-02T08:00:00Z FROM=auditor NOTE :: x"]) is None, True)
    check("audit-queue-self", audit_queue_head(51, {"requester": "dev", "text": "t"}, None) == 51, True)
    check("audit-queue-none", audit_queue_head(51, None, None) is None, True)
    check("audit-queue-other-older",
          audit_queue_head(52, {"requester": "dev", "text": "t"},
                           [(51, "/r", {"audit_pending": {"since": "2026-11-02T07:00:00Z"}}),
                            (52, "/r", {})]) == 51, True)
    check("audit-queue-oldest-wins",
          audit_queue_head(51, {"requester": "dev", "text": "t"},
                           [(51, "/r", {}),
                            (52, "/r", {"audit_pending": {"since": "2026-11-02T09:00:00Z"}})]) == 52, True)
    check("audit-ring-0", audit_ring_action(0) == "ring", True)
    check("audit-ring-2", audit_ring_action(2) == "ring", True)
    check("audit-ring-3-escalate", audit_ring_action(3) == "escalate", True)
    check("audit-ring-4-await", audit_ring_action(4) == "awaiting", True)
    check("audit-retask-none", audit_retask_gate(None, None, "IDLE", False, False, None, None, False, None, None, {}, None, None, None) == "skip-no-demand", True)
    check("audit-retask-launch", audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", False, False, None, None, False, None, None, {}, None, None, None) == "launch", True)
    check("audit-retask-defer-working",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "WORKING-markers", True, True, "tip", "tip", False, "k", None, "audit:x", {}, None, None) == "defer-working", True)
    check("audit-retask-defer-modal",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "BLOCKED-modal", True, True, "tip", "tip", False, "k", None, "audit:x", {}, None, None) == "defer-working", True)
    check("audit-retask-defer-busy",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", False, "k", None, "audit:x", {}, 54, None) == "defer-busy", True)
    check("audit-retask-skip-done",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", True, "k", "k", "audit:x", {}, None, None) == "skip-done", True)
    check("audit-retask-brief-missing",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, False, None, None, False, "k", None, None, {}, None, None) == "brief-then-ring", True)
    check("audit-retask-tip-changed",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "old", "new", False, "k", None, None, {}, None, None) == "brief-then-ring", True)
    check("audit-retask-new-ring",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", False, "k", None, "audit:ts", {}, None, None) == "ring", True)
    check("audit-retask-rung-awaiting",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", False, "k", None, "audit:ts", {"audit:ts": 4}, None, 9999) == "awaiting", True)
    check("audit-retask-escalate",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", False, "k", None, "audit:ts", {"audit:ts": 3}, None, 9999) == "escalate", True)
    check("audit-retask-ring-again",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", False, "k", None, "audit:ts", {"audit:ts": 1}, None, 9999) == "ring-again", True)
    check("audit-retask-hold-stale",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "ABSENT", False, False, None, None, False, "k", None, None, {}, None, None) == "launch", True)
    check("audit-sla-ok",
          audit_sla_action(None, True, "IDLE-static", "audit:x", 1, 0) == "ok"
          and audit_sla_action(100, True, "IDLE-static", "audit:x", 1, 0) == "ok", True)
    check("audit-sla-re-ring",
          audit_sla_action(901, True, "IDLE-static", "audit:x", 1, 0) == "re-ring", True)
    check("audit-sla-re-ring-once",
          audit_sla_action(901, True, "IDLE-static", "audit:x", 1, 1) == "ok", True)
    check("audit-sla-absent",
          audit_sla_action(1801, False, "ABSENT", None, 0, 0) == "escalate-absent"
          and audit_sla_action(901, False, "ABSENT", None, 0, 0) == "ok", True)
    check("audit-sla-stale-no-kill",
          audit_sla_action(1801, True, "WORKING-markers", "audit:x", 1, 0) == "escalate-stale", True)
    check("audit-sla-stale-early-ok",
          audit_sla_action(901, True, "WORKING-markers", "audit:x", 1, 0) == "ok", True)
    check("audit-sla-missing",
          audit_sla_action(1801, True, "IDLE-static", None, 0, 0) == "escalate-missing", True)
    check("audit-sla-legacy-no-store",
          audit_sla_next({}, None, "ok", "2026-11-02T08:00:00Z") == {}
          and audit_sla_age_s({}, {}, "k", "2026-11-02T08:00:00Z") is None, True)
    check("audit-sla-handled-ok",
          audit_sla_action(9999, False, "ABSENT", None, 0, 0, handled=True) == "ok", True)
    _sla1 = audit_sla_next({}, "dev:ab12cd34", "ok", "2026-11-02T08:00:00Z")
    check("audit-sla-next-key-reset",
          _sla1 == {"key": "dev:ab12cd34", "first": "2026-11-02T08:00:00Z", "rerings": 0}, True)
    _sla2 = audit_sla_next({"sla": _sla1}, "dev:ab12cd34", "re-ring", "2026-11-02T08:30:00Z")
    check("audit-sla-next-rering-bumps",
          _sla2["rerings"] == 1 and _sla2["first"] == "2026-11-02T08:00:00Z", True)
    check("audit-sla-next-new-demand-resets",
          audit_sla_next({"sla": _sla2}, "dev:ff00ff00", "ok", "2026-11-02T09:00:00Z") == {"key": "dev:ff00ff00", "first": "2026-11-02T09:00:00Z", "rerings": 0}, True)
    check("audit-sla-age-max",
          audit_sla_age_s({"since": "2026-11-02T08:00:00Z"}, {"key": "k", "first": "2026-11-02T07:00:00Z"}, "k", "2026-11-02T09:00:00Z") == 7200.0, True)
    check("audit-sla-age-store-only",
          audit_sla_age_s({"since": "2026-11-02T08:30:00Z"}, {}, "k", "2026-11-02T09:00:00Z") == 1800.0, True)
    check("audit-teardown-absent", audit_teardown_gate(False, "ABSENT", False, False, None, None, False, False, 0, 2, True, False) == (0, "absent"), True)
    check("audit-teardown-disabled", audit_teardown_gate(True, "IDLE", True, True, None, None, False, False, 0, 2, False, False) == (0, "disabled"), True)
    check("audit-teardown-hold-dup", audit_teardown_gate(True, "IDLE", True, True, None, None, False, True, 0, 2, True, False) == (0, "hold-dup"), True)
    check("audit-teardown-hold-demand", audit_teardown_gate(True, "IDLE", True, True, {"requester": "dev"}, None, False, False, 0, 2, True, False) == (0, "hold-demand"), True)
    check("audit-teardown-hold-fresh", audit_teardown_gate(True, "IDLE", True, True, None, {"requester": "dev"}, False, False, 0, 2, True, False) == (0, "hold-demand"), True)
    check("audit-teardown-hold-verdict", audit_teardown_gate(True, "IDLE", False, False, None, None, False, False, 0, 2, True, False) == (0, "hold-verdict"), True)
    check("audit-teardown-hold-unhandled", audit_teardown_gate(True, "IDLE", True, False, None, None, False, False, 0, 2, True, False) == (0, "hold-verdict"), True)
    check("audit-teardown-hold-working", audit_teardown_gate(True, "WORKING", True, True, None, None, False, False, 0, 2, True, False) == (0, "hold-working"), True)
    check("audit-teardown-countdown", audit_teardown_gate(True, "IDLE-static", True, True, None, None, False, False, 0, 2, True, False) == (1, "countdown"), True)
    check("audit-teardown-kill", audit_teardown_gate(True, "IDLE-static", True, True, None, None, False, False, 1, 2, True, False) == (2, "kill"), True)
    check("audit-teardown-would-kill", audit_teardown_gate(True, "IDLE-static", True, True, None, None, False, False, 1, 2, True, True) == (2, "would-kill"), True)
    check("audit-teardown-hold-other",
          audit_teardown_gate(True, "IDLE-static", True, True, None, None, True, False, 0, 2, True, False) == (0, "hold-demand"), True)
    check("audit-retask-neg-skip-done",
          audit_retask_gate({"requester": "dev", "text": "t"}, None, "IDLE-static", True, True, "tip", "tip", True, "k", "k", "audit:ts", {"audit:ts": -1}, None, 0) == "skip-done", True)
    check("audit-state-demand-next-plan-review",
          audit_state_demand({"pending": "plain work"},
                             {"dev": {"next": "waiting auditor plan-review"}, "qe": {"next": None}}) == {"requester": "dev", "text": "waiting auditor plan-review"}, True)
    check("audit-state-demand-next-qe",
          audit_state_demand({"pending": "plain work"},
                             {"dev": {"next": None}, "qe": {"next": "waiting audit verdict"}}) == {"requester": "qe", "text": "waiting audit verdict"}, True)
    check("audit-demand-next-fallback",
          audit_demand({"dev": {"all": [], "next": "waiting auditor plan-review"}, "qe": {"all": [], "next": None}},
                       {"pending": "plain work"},
                       {"dev": [], "qe": []}) == {"requester": "dev", "text": "waiting auditor plan-review"}, True)
    check("audit-planned-verdict", audit_planned_text(True, True, True, {"requester": "d"}, {"requester": "d"}, 51, 51) == "verdict on file", True)
    check("audit-planned-report", audit_planned_text(False, True, False, {}, None, None, 51) == "report on file", True)
    check("audit-planned-stored", audit_planned_text(False, False, True, {"requester": "d"}, None, None, 51) == "awaiting verdict", True)
    check("audit-planned-queued", audit_planned_text(False, False, True, {}, {"requester": "d"}, 52, 51) == "queued behind #52", True)
    check("audit-planned-ring", audit_planned_text(False, False, True, {}, {"requester": "d"}, 51, 51) == "ring audit lane", True)
    check("audit-planned-queued-launch", audit_planned_text(False, False, False, {}, {"requester": "d"}, None, 51) == "queued: launch audit", True)
    check("audit-planned-idle", audit_planned_text(False, False, True, {}, None, None, 51) == "idle: no audit requested", True)
    check("audit-planned-absent", audit_planned_text(False, False, False, {}, None, None, 51) == "no audit requested", True)
    check("reset-resume-counts", reset_resume_rings({"next:qe:a": 1, "dev:x": 2, "next:dev:b": 0}) == 2, True)
    check("ring-pointer-override",
          ring(SimpleNamespace(dry=True), "s", "dev", "t", oid="audit:x",
               pointer="/tmp/r/inbox/auditor.md", tag_oid=False).endswith("read /tmp/r/inbox/auditor.md"), True)
    check("ring-oid-audit-tagged",
          "[oid audit:x]" in ring(SimpleNamespace(dry=True), "s", "dev", "t", oid="audit:x"), True)
    check("kill-would-not-run",
          kill_audit_session(SimpleNamespace(dry=True, audit_sess="s", run=lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran"))), "r") == "WOULD kill s (r)", True)
    _rs = {}
    _r1 = room_rings(_rs)
    _r1["audit:x"] = 1
    check("room-rings-shared-virgin", room_rings(_rs) == {"audit:x": 1} and _rs.get("rings") == {"audit:x": 1}, True)
    check("room-rings-keeps-existing", room_rings({"rings": {"a:b": 2}}) == {"a:b": 2}, True)
    check("room-rings-replaces-nondict", room_rings({"rings": [1]}) == {}, True)
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
    t.add_argument("--no-audit-teardown", action="store_true",
                   help="never kill the auditor session (observe only)")
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
