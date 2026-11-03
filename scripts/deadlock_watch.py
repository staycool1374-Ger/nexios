#!/usr/bin/env python3
"""Deadlock watcher: MOVING / PARKED-OK / DEADLOCK per room + why-chain.

Reads live room state (/tmp/nexios-*/state.json + inboxes) and the engine
log tail. No writes, no ticks, no doorbells — pure diagnosis. A room is
DEADLOCKED only when nothing moved in the window AND at least one blocker
has no live owner (orphan NEED, acked-but-unanswered order, overdue
delivery past escalate, same-NEXT stale past trips, failing merge). Every
verdict cites oids, timestamps, and the actor that owns the next move.
"""
import sys, os, re, json, glob, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "logs", "state-machine", "state-machine.log")
WINDOW_S = 900
TS = r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})Z?"


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)


def parse_ts(line):
    m = re.match(r"(?:DRY )?%s" % TS, line or "")
    if not m:
        return None
    try:
        return datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return None


def room_ids():
    # Live rooms only: issue >= 9000 is the self-test/scratch namespace.
    out = []
    for st in sorted(glob.glob("/tmp/nexios-*/state.json")):
        try:
            n = int(json.load(open(st)).get("issue"))
            if n < 9000:
                out.append(n)
        except (ValueError, OSError, TypeError):
            continue
    return sorted(out)


def load_room(issue):
    try:
        return json.load(open("/tmp/nexios-%d/state.json" % issue))
    except OSError:
        return {}


def recent_log(window_s=WINDOW_S):
    try:
        lines = open(LOG, errors="replace").read().splitlines()
    except OSError:
        return []
    cutoff = now_utc() - datetime.timedelta(seconds=window_s)
    out = []
    for ln in lines:
        if ln.startswith("DRY "):
            continue
        ts = parse_ts(ln)
        if ts and ts >= cutoff:
            out.append((ts, ln))
    return out


ART_RE = re.compile(r"FROM=(dev|qe|audit|auditor) (READY|NEED|DECISION) ::|"
                    r"\bDONE\b|\bVERIFIED\b|^\s*branch\s*:|^\s*tip\s*:|"
                    r"\bbranch\s+[A-Za-z0-9._/-]{2,}\b.*\btip\s+[0-9a-f]{7,40}\b", re.I)


def inbox_artifact_posts(issue, since):
    arts = []
    for box in ("dev", "qe", "auditor"):
        try:
            lines = open("/tmp/nexios-%d/inbox/%s.md" % (issue, box),
                         errors="replace").read().splitlines()
        except OSError:
            continue
        for ln in lines:
            m = re.match(r"%s\s+FROM=" % TS, ln)
            if not m:
                continue
            if "FROM=coord " in ln:
                continue
            try:
                ts = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")
            except ValueError:
                continue
            if ts >= since and ART_RE.search(ln):
                arts.append("%s:%s" % (box, ln[:110]))
    return arts


def live_oid(rings, oid):
    return bool(oid) and (rings or {}).get(oid) not in (None, -1, 96, 97)


def diagnose(issue, log_lines):
    st = load_room(issue)
    rings = st.get("rings") or {}
    mine = [ln for _ts, ln in log_lines if re.search(r"#%d\b" % issue, ln)]
    arts = inbox_artifact_posts(issue, now_utc() - datetime.timedelta(seconds=WINDOW_S))
    filed = [ln for ln in mine if "order filed" in ln]
    acked = [ln for ln in mine if re.search(r"\back dev\b|\back qe\b|ack-ignored", ln)]
    handed = [ln for ln in mine if "verdict handed" in ln or "testbed staged" in ln]
    progress = arts + [l for l in filed] + handed
    if [l for l in mine if "ack-ignored" in l]:
        progress.append("stale-ack-arrived")

    blockers, waits = [], []

    try:
        now19 = now_utc().strftime("%Y-%m-%dT%H:%M:%S")
        for oid, info in (st.get("delivery") or {}).items():
            by = ((info or {}).get("done_by") or "")[:19]
            if not by or now19 <= by:
                continue
            lane = (oid or "").split(":")[0]
            if lane not in ("dev", "qe"):
                continue
            ev = (info or {}).get("evidence")
            blockers.append(("delivery %s overdue (done-by %s, evidence %s)"
                             % (oid, by, ev),
                             "escalation due; recreate attempted or NEED (no flag)",
                             "%s delivers / engine recreates" % lane))
    except (ValueError, TypeError):
        pass


    def store_deadline(store_key, what):
        d = st.get(store_key) or {}
        oid = d.get("oid")
        if not oid:
            return None
        if live_oid(rings, oid):
            by = ((st.get("ack_deadline") or {}).get(oid) or {}).get("by")
            return "%s %s open (ack-by %s)" % (what, oid, by or "?")
        if oid in rings:
            return "%s %s ACKED-but-unanswered" % (what, oid)
        return "%s %s ledger-gone" % (what, oid)

    for ln in mine:
        if "branch unverifiable" in ln:
            r = store_deadline("closeout_bq", "branch-name order")
            blockers.append(("closeout branch text", r or "ORPHAN: no branch-name order filed", "dev paints it / operator scopes it"))
        if "testbed not clean" in ln and "CLEAN" not in ln:
            r = store_deadline("qe_clean", "CLEAN order")
            blockers.append(("dirty testbed", r or "ORPHAN: no CLEAN order filed", "qe"))
        if "qe-merge" in ln and "failed" in ln:
            r = store_deadline("qe_resolve", "RESOLVE order")
            blockers.append(("merge conflict", r or "ORPHAN: no RESOLVE order (operator must merge by hand)", "qe resolves / operator"))
        if "delivery" in ln and "ESCALATED" in ln:
            blockers.append(("delivery past done-by", "recreate attempted (see delivery-recreate) or NEED (no flag)", "operator watches relaunch"))
        if "stale NEXT persists" in ln:
            strikes = ((st.get("dev_stale") or {}).get("strikes") or 1)
            recre = (st.get("dev_stale") or {}).get("recreated")
            blockers.append(("stale NEXT x%d" % strikes, "recreate %s" % ("done" if recre else "at 3 silent strikes"), "dev posts / engine recreates"))
        if "starvation" in ln and "quiet suppressed" not in ln:
            blockers.append(("starvation", "log-only by design", "engine (no actuator wired)"))
        if "unanswered" in ln:
            blockers.append((ln.split("::", 1)[-1].strip()[:110], "order ACKed, condition persists", "operator"))
    if [ln for ln in mine if "IDLE-accepted" in ln]:
        waits.append("lanes idle, work scan empty")
    ap = st.get("audit_pending")
    if ap:
        waits.append("audit demand open since %s" % (ap.get("since") or "?"))

    moving = bool([a for a in arts if "ACK " not in a.split("::", 1)[-1][:30]] + filed + handed)
    if moving:
        return "MOVING", ["%s" % a[:130] for a in (arts + filed + handed)[:8]], []
    seen, uniq = set(), []
    for b in blockers:
        if b not in seen:
            seen.add(b)
            uniq.append(b)
    blockers = uniq
    orphan = [b for b in blockers if b[1].startswith("ORPHAN")]
    stuck = [b for b in blockers if "ACKED-but-unanswered" in b[1] or "unanswered" in b[1] or "no actuator" in b[1]]
    if orphan or stuck:
        why = ["%s -> %s [owner: %s]" % (t, s, o) for t, s, o in (orphan + stuck)]
        return "DEADLOCK", [], why
    if blockers:
        why = ["%s -> %s [owner: %s]" % (t, s, o) for t, s, o in blockers]
        return "PARKED-OK", [], why
    if waits:
        return "PARKED-OK", [], waits
    return "MOVING", ["no needs, no waits"], []


def main():
    rooms = [int(a) for a in sys.argv[1:] if a.isdigit()] or room_ids()
    log = recent_log()
    code = 0
    for issue in rooms:
        verdict, prog, why = diagnose(issue, log)
        print("#%d: %s" % (issue, verdict))
        for p in prog[:6]:
            print("  + %s" % p[:140])
        for w in why[:8]:
            print("  - %s" % w[:180])
        if verdict == "DEADLOCK":
            code = 2
    return code


if __name__ == "__main__":
    sys.exit(main())
