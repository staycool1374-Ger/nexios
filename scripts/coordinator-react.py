#!/usr/bin/env python3
"""coordinator-react.py -- wake-step-0 advisor for the nexios coordinator.

Reads room runtimes (/tmp/nexios-*) plus argv worker states, prints an
ordered action checklist. READ-ONLY ALWAYS: never writes files, never
rings sessions, never touches git. The coordinator executes; this script
only remembers (runbook rows 1-12 live here, not in anyone's recall).

Usage:
  python3 scripts/coordinator-react.py --tick "351:CHANGED:8" \
      --dev WORKING --qe IDLE [--streak 351:1] [--rooms 351,389,402]

Output sections: NEW POSTS / WORKER CHECKS / ARMED TRIGGERS / ACTIONS
(highest priority first) / NOTHING OWED if the wake owes no one anything.
"""
import argparse, glob, json, os, re, sys

ROOMS_GLOB = '/tmp/nexios-*'
SESS = {'dev': 'nexios-dev', 'qe': 'nexios-qe'}
RUNBOOK_VERSION = 'v3 2026-10-29 (rows 1-12; ts-based new-post)'

def tail(path, n=6):
    try:
        with open(path, errors='replace') as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []

def from_lines(lines):
    """Worker-authored posts only (coordinator writes don't count)."""
    return [l for l in lines if re.match(r'^\d{4}-\d{2}-\d{2}T.* FROM=(dev|qe|audit|auditor) (READY|NEED|NOTE) ::', l)]

def last_state_next(lines):
    st = nx = None
    for l in lines:
        m = re.match(r'^STATE:\s*(.*)', l.strip())
        if m: st = m.group(1)[:100]
        m = re.match(r'^NEXT:\s*(.*)', l.strip())
        if m: nx = m.group(1)[:160]
    return st, nx

def load_room(rdir):
    try:
        with open(os.path.join(rdir, 'state.json')) as f:
            st = json.load(f)
    except (OSError, ValueError):
        return None
    room = {'dir': rdir, 'issue': st.get('issue'), 'phase': st.get('phase'),
            'pending': (st.get('pending') or '')[:220],
            'armed': st.get('armed') or [], 'state': st}
    room['handled'] = st.get('handled') or {}
    room['mark'] = False
    room['boxes'] = {}
    for box in ('dev', 'qe', 'auditor'):
        lines = tail(os.path.join(rdir, 'inbox', box + '.md'), 12)
        try:
            with open(os.path.join(rdir, 'inbox', box + '.md'), errors='replace') as f:
                allposts = from_lines(f.read().splitlines())
        except OSError:
            allposts = []
        posts = from_lines(lines)
        since = room['handled'].get(box)
        if since is None:
            fresh = []  # first sight: init cursor, report nothing (no flood)
            room['mark'] = True
        else:
            fresh = [x for x in allposts if x[:20] > since[:20]]
        room['boxes'][box] = {'posts': posts, 'count': len(allposts),
                              'last': posts[-1][:180] if posts else None,
                              'new': fresh[-3:],
                              'cursor_init': since is None}
        room['boxes'][box]['state'], room['boxes'][box]['next'] = last_state_next(lines)
        # unacked coordinator order: newest coord NOTE vs newest worker post
        coord = [l for l in lines if ' FROM=coord NOTE' in l]
        room['boxes'][box]['coord_last'] = coord[-1][:160] if coord else None
    return room

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tick', default='', help='"<issue>:<CHANGED|UNCHANGED>[:<n>]"')
    ap.add_argument('--dev', default='?', help='dev worker state: WORKING|IDLE|...')
    ap.add_argument('--qe', default='?', help='qe worker state')
    ap.add_argument('--streak', action='append', default=[], help='"<issue>:<n>" unchanged streak')
    ap.add_argument('--rooms', default='', help='comma list of issues to scan (default: all)')
    ap.add_argument('--mark', action='store_true', help='advance handled cursors to current lasts (run after processing a wake)')
    a = ap.parse_args()
    only = set(a.rooms.split(',')) if a.rooms else None
    rooms = []
    for rdir in sorted(glob.glob(ROOMS_GLOB)):
        r = load_room(rdir)
        if r and r['issue'] is not None and (only is None or str(r['issue']) in only):
            rooms.append(r)
    tick_issue = (a.tick.split(':')[0] if a.tick else '')
    actions = []  # (prio, text)
    print('== coordinator-react %s ==' % RUNBOOK_VERSION)
    print('tick: %s | dev=%s qe=%s' % (a.tick or '-', a.dev, a.qe))
    # 1. new posts per room (rows 1-5)
    for r in rooms:
        for box in ('dev', 'qe', 'auditor'):
            b = r['boxes'][box]
            if b['new']:
                actions.append((1, '#%s %s NEW POSTS x%d: %s'
                    % (r['issue'], box, len(b['new']), ' // '.join(x[:150] for x in b['new']))))
    # 2. worker checks (rows 6-9)
    wstates = {'dev': a.dev, 'qe': a.qe}
    for role, sess in SESS.items():
        st = wstates[role]
        if st == '?':
            actions.append((50, 'worker %s state unknown (pass --%s); cannot clear rows 6-9' % (sess, role)))
    # 3. armed triggers (row: evaluate each)
    for r in rooms:
        for arm in r['armed']:
            on, do = arm.get('on', ''), arm.get('do', '')
            actions.append((30, '#%s ARMED [%s] -> %s (verify fired/hold from box tails)'
                % (r['issue'], on, do[:140])))
    # 4. unacked orders -> ring candidates (rows 6-7)
    for r in rooms:
        for role, sess in SESS.items():
            b = r['boxes'][role]
            if b['coord_last'] and (not b['posts'] or b['coord_last'][:19] > b['posts'][-1][:19]):
                w = wstates[role]
                if w == 'IDLE':
                    actions.append((10, 'RING %s: unacked order in #%s %s box (%s) -- send: inbox: <short> - read inbox/%s.md'
                        % (sess, r['issue'], role, b['coord_last'][:100], role)))
                else:
                    pass  # WORKING reads at boundary; never ring, never nag
    # 5. idle + nothing -> backlog (row 8)
    for role, sess in SESS.items():
        if wstates[role] == 'IDLE':
            if not any(x[1].startswith('RING %s' % sess) for x in actions):
                actions.append((20, '%s IDLE with no unacked order: assign next backlog task (never relay idleness)' % sess))
    # 6. unchanged streaks (row 10)
    for s in a.streak:
        try:
            iss, n = s.split(':'); n = int(n)
        except ValueError:
            continue
        if n >= 3:
            actions.append((5, '#%s UNCHANGED x%d ESCALATE: sess_state -> ring once -> checkpoint/compact -> recreate'
                % (iss, n)))
    # 7. owner-blocked rooms (row 11)
    for r in rooms:
        if re.search(r'owner|retire|close issue|accept-and-retire|merge order|needs merge', r['pending'], re.I):
            actions.append((60, '#%s owner-gated: %s' % (r['issue'], r['pending'][:140])))
    actions.sort(key=lambda x: x[0])
    print('--- ACTIONS (priority order) ---')
    hit = False
    for prio, text in actions:
        if prio >= 50 and len([x for x in actions if x[0] < 50]):
            continue
        hit = True
        print('[P%d] %s' % (prio, text))
    if not hit:
        print('NOTHING OWED.')
    if a.mark:
        for r in rooms:
            if r['mark'] or any(r['boxes'][b]['new'] for b in ('dev', 'qe', 'auditor')):
                st = r['state']
                h = st.get('handled') or {}
                for b in ('dev', 'qe', 'auditor'):
                    posts_here = []
                    try:
                        with open(os.path.join(r['dir'], 'inbox', b + '.md'), errors='replace') as f:
                            posts_here = from_lines(f.read().splitlines())
                    except OSError:
                        pass
                    if posts_here:
                        h[b] = max(x[:20] for x in posts_here)  # max ts, not last line: clocks skew across sessions
                st['handled'] = h
                with open(os.path.join(r['dir'], 'state.json'), 'w') as f:
                    json.dump(st, f, indent=2)
        print('cursors advanced.')
    print('--- (advisory only; coordinator executes; verify before acting) ---')

main()
