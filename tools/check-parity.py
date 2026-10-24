#!/usr/bin/env python3
"""Parity-ledger gate for the build-system cutover → Meson cutover (issue #170).

Reads docs/specs/build-system-parity.tsv (one row per checklist item)
and reports per-row status. This is a process gate, not a semantic
re-verification: the semantic evidence lives in the referenced CI jobs
and test runs (see the `ci_job`/`evidence_url` columns); this tool
enforces that no row is still open and every row cites evidence.

Usage:
    python3 tools/check-parity.py [--ledger PATH] [--report]
    python3 tools/check-parity.py [--ledger PATH] --require-all

Exit codes: 0 = clean (report) / gate satisfied (require-all);
1 = malformed ledger or (require-all only) open/unevidenced rows remain.
"""

import argparse
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_LEDGER = REPO / 'docs' / 'specs' / 'build-system-parity.tsv'
EXPECTED_COLUMNS = ['id', 'item', 'requirement', 'make_behavior',
                    'meson_solution', 'meson_target', 'ci_job',
                    'evidence_url', 'status']


def load_ledger(path):
    """Parse the TSV ledger; return (header, rows)."""
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.reader(f, delimiter='\t')
        rows = [r for r in reader if r and not (len(r) == 1 and r[0].startswith('#'))]
    if not rows:
        return [], []
    header, body = rows[0], rows[1:]
    if header != EXPECTED_COLUMNS:
        print('ledger header mismatch: %r' % (header,), file=sys.stderr)
        return None, []
    bad = [r for r in body if len(r) != len(header)]
    if bad:
        print('ledger has %d malformed row(s) (want %d columns)' % (len(bad), len(header)), file=sys.stderr)
        return None, []
    return header, body


def main():
    """Entry point: report the ledger or enforce the cutover gate."""
    ap = argparse.ArgumentParser(description='Parity-ledger cutover gate.')
    ap.add_argument('--ledger', default=str(DEFAULT_LEDGER),
                    help='path to build-system-parity.tsv')
    ap.add_argument('--report', action='store_true',
                    help='print per-row status table (always exits 0 on a valid ledger)')
    ap.add_argument('--require-all', action='store_true',
                    help='exit 0 only when no row is open and every row cites evidence')
    args = ap.parse_args()

    header, rows = load_ledger(args.ledger)
    if header is None:
        return 1
    idx = {name: header.index(name) for name in EXPECTED_COLUMNS}

    if args.report or not args.require_all:
        print('%-4s %-22s %-8s %s' % ('id', 'item', 'status', 'evidence'))
        for row in rows:
            print('%-4s %-22s %-8s %s' % (
                row[idx['id']], row[idx['item']][:22],
                row[idx['status']], row[idx['evidence_url']][:60]))
        n_open = sum(1 for r in rows if r[idx['status']].strip().lower() == 'open')
        print('rows: %d open: %d' % (len(rows), n_open))
        return 0

    blockers = []
    for row in rows:
        status = row[idx['status']].strip().lower()
        if status == 'open':
            blockers.append('%s (%s): still open' % (row[idx['id']], row[idx['item']]))
        elif not row[idx['evidence_url']].strip():
            blockers.append('%s (%s): no evidence URL' % (row[idx['id']], row[idx['item']]))
    if blockers:
        print('cutover BLOCKED (%d row(s)):' % len(blockers))
        for b in blockers:
            print('  - %s' % b)
        return 1
    print('cutover gate: all %d rows closed with evidence' % len(rows))
    return 0


if __name__ == '__main__':
    sys.exit(main())
