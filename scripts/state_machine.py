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
import signal
import fcntl
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import time

VERSION = "state-machine v1 2026-10-31 (consultant cutover engine)"

ROOMS_GLOB = "/tmp/nexios-*"
BOXES = ("dev", "qe", "auditor")
SESS = {"dev": "nexios-dev", "qe": "nexios-qe", "audit": "nexios-audit",
        "consultant": "nexios-consultant"}

# Modal signature copied from scripts/nexios-bin/worker-state.sh v3 MODAL_RE.
MODAL_RE = (r"allow once|allow always|permission required|needs approval|"
            r"always allow|enter confirm|reject permission|"
            r"tell opencode what to do differently|needs your decision|"
            r"awaiting approval")

POST_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}T\S*) FROM=(dev|qe|audit|auditor) "
    r"(READY|NEED|NOTE|DECISION) :: (.*)$")
STATE_RE = re.compile(r"^STATE:\s*(.*)$")
NEXT_RE = re.compile(r"^NEXT:\s*(.*)$")

VALID_ROOM_STATES = ("IN_PROGRESS", "AUDITED", "VERIFIED", "PLANNED",
                     "QUEUED", "CLOSED", "HOLD")


RING_LIMIT = 3  # doctrine: ring an order max 3x; then NEED-recreate, never a 4th ring
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
    """Missing STATE:/NEXT: trailer lines after a fresh post (wire format)."""
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
            "If compaction delivery fails, hold for recreate (never work past 100%% with unposted work).\n"
            "scripts/agent_up.sh wake %d %s nexios-coord \"inbox: %s READY \u2014 read inbox/%s.md\"\n"
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
        coord = [l for l in tail12 if " FROM=coord NOTE" in l]
        boxes[box] = {"all": posts, "full": all_lines, "tail": tail12, "state": st, "ctx": cx,
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
            "scripts/agent_up.sh wake %d auditor nexios-coord \"inbox: auditor READY \u2014 read inbox/auditor.md\"\n"
            % (issue, issue, tip, scope, issue))


def auditor_lifecycle(ctx, issue, room_dir, st, boxes, fresh):
    """Request-driven audit cycle with an invisible auditor session.

    Dev/qe post NEED-audit (request); the engine generates the tip-pinned
    brief, starts the auditor via `agent_up.sh audit` (headless tmux session,
    never windowed, so its permission modals can only ever meet deny-default),
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
        note = ("%s FROM=coord NOTE :: AUDIT %s (#%d): detail inbox/auditor.md"
                % (utcnow(), "APPROVED" if "APPROVED" in post else "NEED", issue))
        rep = latest_audit_report(ctx, issue)
        if rep:
            note += "; report %s" % os.path.basename(rep)
        if ctx.dry:
            acted.append("WOULD hand audit verdict to %s + ring-if-idle" % target)
        else:
            if append_box_note(room_dir, target, note):
                acted.append("audit verdict handed to %s" % target)
                if target in SESS and fast_probe(ctx, SESS[target]).startswith("IDLE"):
                    acted.append(ring(ctx, SESS[target], target, "audit verdict ready"))
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


def ring(ctx, sess, box, text, channel="inbox"):
    """Doorbell. Idle sessions only (caller checks). inbox = box pointer,
    notes = consultant channel (logs/consultant-notes.md)."""
    if channel == "notes":
        line = "notes: %s -- read logs/consultant-notes.md" % text
    else:
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
                note = ("%s FROM=coord NOTE :: CHECKPOINT pre-compaction (@%d%% ctx): "
                        "post CHECKPOINT NOTE, run /compact, RING" % (utcnow(), pct))
                if append_box_note(room_dir, role, note):
                    actions.append("checkpoint NOTE filed for %s @%d%%" % (role, pct))
                    if role not in deaf and probes[role].startswith("IDLE"):
                        actions.append(ring(ctx, SESS[role], role,
                                            "checkpoint brief @%d%%" % pct))
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
    has_unacked = set()
    for role in ("dev", "qe"):
        b = boxes[role]
        if b["coord_last"] and (not b["all"] or
                                b["coord_last"][:19] > b["all"][-1][:19]):
            has_unacked.add(role)
            order_id = "%s:%s" % (role, b["coord_last"][:19])
            n = rings.get(order_id, 0)
            if role not in deaf and probes[role].startswith("IDLE"):
                if n < RING_LIMIT:
                    actions.append(ring(ctx, SESS[role], role,
                                        "unacked order -- %s" % b["coord_last"][:80]))
                    rings[order_id] = n + 1
                    events.append("ring-%s" % role)
                elif n == RING_LIMIT:
                    r_acts, _rel = recreate_worker(ctx, issue, room_dir, role,
                        "order %s unacked after %d rings" % (b["coord_last"][:19], n),
                        b["state"], b["next"])
                    actions.extend(r_acts)
                    rings[order_id] = -1 if _rel else n + 1
                else:
                    actions.append("recreate-awaiting %s: order %s (no repeat orders)"
                                   % (SESS[role], b["coord_last"][:19]))
            else:
                actions.append("%s WORKING reads at boundary; no ring" % role)
    resume_now = {}
    for role in ("dev", "qe"):
        nx = (boxes[role]["next"] or "")
        if re.search(r"(hold|waiting|awaiting|parked)[^.]*re-?ring|on re-?ring", nx, re.I):
            resume_now[role] = "next:%s:%s" % (role, hashlib.md5(nx.encode()).hexdigest()[:8])
    for oid in list(rings):
        if oid.startswith("next:"):
            _p, _r, _h = oid.split(":")
            if oid not in resume_now.values() or fresh.get(_r):
                del rings[oid]
            continue
        orole, _, ots = oid.partition(":")
        posts = boxes.get(orole, {}).get("all", [])
        if posts and posts[-1][:19] > ots:
            del rings[oid]
    for role, oid in resume_now.items():
        if role in has_unacked or role in deaf:
            continue
        if not probes[role].startswith("IDLE"):
            continue
        n = rings.get(oid, 0)
        if 0 <= n < RING_LIMIT:
            actions.append(ring(ctx, SESS[role], role, "resume per declared NEXT"))
            rings[oid] = n + 1
            events.append("ring-%s-resume" % role)
        elif n == RING_LIMIT:
            r_acts, _rel = recreate_worker(ctx, issue, room_dir, role,
                "parked-awaiting ring unacked after %d rings" % n,
                boxes[role]["state"], boxes[role]["next"])
            actions.extend(r_acts)
            rings[oid] = -1 if _rel else n + 1

    if dev_blocked_on_qe(dev_next) and not probes["qe"].startswith("IDLE"):
        target = pick_backlog(rooms_all, issue)
        if target is None:
            actions.append("NEED backlog empty: dev waits on qe, nothing to swap to")
        else:
            brief = ("BACKLOG SWAP (#%d) -- dev blocked on qe output in #%d\n"
                     "issue: %d\nbranch: none yet (read-only until recalled)\n"
                     "Task: read-only recon of #%d per its room pending/next; "
                     "no branch, no code. Every post ends with STATE/NEXT lines plus a CTX: <n>%% line (pane footer). "
"Post NOTE to inbox/dev.md + RING.\n"
                     "scripts/agent_up.sh wake %d dev nexios-coord \"inbox: dev READY \u2014 read inbox/dev.md\"\n"
                     % (target, issue, target, target, target))
            actions.append(write_brief(ctx, room_dir,
                                       "dev-%d-backlog.txt" % target, brief))
            if "dev" not in deaf and probes["dev"].startswith("IDLE"):
                actions.append(ring(ctx, SESS["dev"], "dev",
                                    "backlog #%d ready" % target))
            events.append("backlog-swap #%d" % target)

    for role in ("dev", "qe"):
        if role not in has_unacked and probes[role].startswith("IDLE") and not any(
                a.startswith("rang " + SESS[role]) or
                a.startswith("WOULD ring " + SESS[role]) for a in actions):
            if not boxes[role]["next"] or boxes[role]["next"].startswith("idle"):
                actions.append("NEED %s idle with no briefed work (no invention)" % role)

    if verdict.get("escalate"):
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
                                    "armed trigger [%s]" % arm.get("on", "")))
            else:
                actions.append("NEED malformed armed do: %s" % do)
        else:
            actions.append("ARMED [%s] fired: %s" % (arm.get("on", ""), do[:120]))
        events.append("armed-fired")

    dec = auditor_decision(boxes["auditor"]["all"])
    qe_ready = any("READY" in l for l in boxes["qe"]["all"][-3:])
    node["merge_ready"] = bool(dec and "APPROVED" in dec and qe_ready)
    qm_store = None
    if not ctx.dry:
        impls = [q for q in fresh.get("dev", [])
                 if (lambda m: m and m.group(3) == "READY" and "IMPL" in q.upper())(POST_RE.match(q))]
        if impls:
            ts = impls[-1][:20]
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
        if any((lambda m: m and m.group(3) == "READY" and "IMPL" in q.upper())(POST_RE.match(q))
               for q in fresh.get("dev", [])):
            actions.append("WOULD stage testbed via qe-merge on fresh IMPL READY")

    failed = any(a.startswith(("RING-FAILED", "STATE-FAILED", "BRIEF-FAILED",
                               "POINTER-FAILED", "STATE-WRITE-FAILED"))
                 for a in actions)
    escalated = bool(verdict.get("escalate"))
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
            state = probes[role] + (" %d%%" % ctxp[role] if ctxp.get(role) is not None else "")
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
            "3. continue the next step above; normal wire discipline (STATE/NEXT + CTX %% line, doorbell every post).\n"
            "scripts/agent_up.sh wake %d %s nexios-coord \"inbox: %s READY \u2014 read inbox/%s.md\"\n"
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


RELEASE_OWNERS = {1: "consultant", 2: "consultant", 3: "operator",
                  4: "consultant", 5: "operator", 6: "consultant", 7: "engine"}
RELEASE_TEXTS = {
    1: "Verify merge: lane branch merged into main; main==origin/main; tree clean.",
    2: "Confirm release version with operator (repo convention + milestone); record as STEP 2 DONE :: vX.Y.Z.",
    3: "OPERATOR: create annotated tag + push (consultant prepares exact commands).",
    4: "Draft release notes (README highlights + milestone + lane evidence) to logs/release-<issue>-notes.md.",
    5: "OPERATOR: create GitHub Release from draft + mirror to Nextcloud target.",
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
    "release #", "HALT ", "learnings draft", "verdict on file", "WOULD ")


def cycle_productive(all_actions):
    """True if the cycle did (or, dry, would do) anything beyond passive notes."""
    return any(a.startswith(PRODUCTIVE_PREFIXES) for a in all_actions)


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
    if not rooms:
        print("no rooms%s" % (" for issues %s" % sorted(issues) if issues else ""))
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
            break
        if os.path.exists(stop_path):
            msg = "STOP sentinel present: stashing + exiting cleanly"
            print(msg, flush=True)
            ctx.log(msg)
            try:
                os.unlink(stop_path)
            except OSError:
                pass
            break
        ctx.tick_seq += 1
        ctx.log("tick #%d rooms=%s" % (ctx.tick_seq, [i for i, _, _ in rooms]))
        cycle_actions = []
        for issue, rdir, st in rooms:
            node, rows = tick_room(ctx, issue, rdir, st, rooms)
            table = fmt_table(issue, node.get("verdict", {}), rows)
            print(table, flush=True)
            ctx.log("issue #%d verdict=%s acked=%d merge_ready=%s" % (
                issue, node.get("verdict", {}).get("kind"),
                node.get("acked", 0), node.get("merge_ready")))
            for a in node.get("actions", []):
                ctx.log("#%d :: %s" % (issue, a))
                cycle_actions.append(a)
        ctx.log(write_graph(ctx))
        if not interval:
            break
        if cycle_productive(cycle_actions):
            quiet = 0
        else:
            quiet += 1
            if quiet >= 3:
                msg = "all quiet for 3 cycles: no work pending, ending cleanly"
                print(msg, flush=True)
                ctx.log(msg)
                break
        time.sleep(interval)
        rooms = discover_rooms(issues, skip_debris=True)
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
        seed = "%s\n\n%s" % (title, (body or "").strip())
        dev_brief = ("%s\n\nIssue: %d\nRole prompt: {repo}/prompts/PROMPT-dev.md\n"
                     "Branch: %s (recon first; feature branch at impl)\n"
                     "Task: RECON phase (read-only, no branch, no code): read the issue "
                     "text above + relevant code/tests; draft the implementation plan; "
                     "post PLAN NOTE to inbox/dev.md + RING (every post ends with STATE/NEXT plus a CTX: <n>%% line). "
"No branches, no code, "
                     "no QEMU until the plan is audit-approved.\n"
                     "scripts/agent_up.sh wake %d dev nexios-coord \"inbox: dev READY \u2014 read inbox/dev.md\"\n"
                     % (seed, issue, a.dev_branch, issue))
        qe_brief = ("%s\n\nIssue: %d\nRole prompt: {repo}/prompts/PROMPT-testdev.md\n"
                    "Branch: none (read-only; testbed stays clean)\n"
                    "Task: parallel RECON (read-only, no branch, no code): draft the "
                    "verification approach for this issue; post NOTE to inbox/qe.md + "
"RING (every post ends with STATE/NEXT plus a CTX: <n>%% line). "
                    "RING. No test runs until dev posts IMPL READY.\n"
                    "scripts/agent_up.sh wake %d qe nexios-coord \"inbox: qe READY \u2014 read inbox/qe.md\"\n"
                    % (seed, issue, issue))
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
    for _p in (t, s, pv):
        _p.add_argument("--repo", default=None)
        _p.add_argument("--agent-up", default=None)
        _p.add_argument("--log-dir", default=None)
        _p.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    issues = None
    if a.cmd != "provision" and not a.all:
        issues = set(int(x) for x in a.issues.split(",") if x.strip()) or None
        if issues is None and not a.all:
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
        return cmd_status(ctx, issues)
    finally:
        ctx.close_log()


if __name__ == "__main__":
    sys.exit(main())
