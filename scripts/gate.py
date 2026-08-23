#!/usr/bin/env python3
"""gate.py — mechanical workflow gate for NexIOS (issue #97).

Enforces the AGENTS.md workflow state machine. Rules written in a prompt are
suggestions; rules enforced by this script are invariants.

Subcommands:
  check-close <issue>      Issue may only be closed after a DECISION: APPROVED
                           comment has been posted.
  check-commit <msgfile>   Commit message must reference an open issue (#<n>)
                           unless it is a docs/chore-only commit type.
  check-feedback <issue>   After close, a LEARNINGS.md entry referencing the
                           issue must exist before the FEEDBACK transition is
                           considered complete.

Exit codes: 0 = gate passed, 1 = gate violated.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

REPO = "staycool1374-Ger/nexios"
GH = "/opt/homebrew/bin/gh" if Path("/opt/homebrew/bin/gh").exists() else "gh"
ROOT = Path(__file__).resolve().parent.parent
LEARNINGS = ROOT / "prompts" / "LEARNINGS.md"

# Commit types that never need an issue reference.
ISSUE_FREE_TYPES = {"docs", "chore", "style"}

APPROVED_RE = re.compile(r"DECISION:\s*APPROVED")
ISSUE_REF_RE = re.compile(r"#(\d+)")


def fail(msg: str) -> int:
    print(f"GATE REJECTED: {msg}", file=sys.stderr)
    return 1


def ok(msg: str) -> int:
    print(f"GATE PASSED: {msg}")
    return 0


def gh_json(*args):
    result = subprocess.run(
        [GH, *args], capture_output=True, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh failed: {result.stderr.strip()}")
    out = result.stdout
    if not out.strip():
        return None
    return json.loads(out)


def cmd_check_close(issue: int) -> int:
    comments = gh_json("api", f"repos/{REPO}/issues/{issue}/comments") or []
    bodies = [c.get("body", "") for c in comments]
    if not any(APPROVED_RE.search(c) for c in bodies):
        return fail(
            f"issue #{issue} has no 'DECISION: APPROVED' comment. "
            "Run the auditor (Phase 3) and post the audit evidence first."
        )
    return ok(f"issue #{issue} carries a DECISION: APPROVED comment")


def cmd_check_commit(msgfile: str) -> int:
    msg = Path(msgfile).read_text(encoding="utf-8")
    m = re.match(r"\s*(\w+)", msg)
    ctype = m.group(1).lower() if m and ":" in msg else ""
    if ctype in ISSUE_FREE_TYPES:
        return ok(f"'{ctype}:' commits are exempt from issue references")
    refs = set(ISSUE_REF_RE.findall(msg))
    if not refs:
        return fail(
            "commit message does not reference any issue (#<n>). "
            "Every feature/bug commit must anchor to its GitHub issue."
        )
    for ref in sorted(refs, key=int):
        try:
            issue = gh_json("api", f"repos/{REPO}/issues/{ref}")
            if issue is None:
                return fail(f"cannot resolve #{ref}: empty response")
        except RuntimeError as e:
            return fail(f"cannot resolve #{ref}: {e}")
        if issue.get("state") == "closed":
            print(f"warning: referenced issue #{ref} is already closed")
    return ok(f"commit references issue(s) {', '.join('#'+r for r in sorted(refs))}")


def cmd_check_feedback(issue: int) -> int:
    result = subprocess.run(
        [GH, "api", f"repos/{REPO}/issues/{issue}", "--jq", ".state"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh failed: {result.stderr.strip()}")
    state = result.stdout.strip()
    if state != "closed":
        return fail(f"issue #{issue} is not closed yet; FEEDBACK applies after close")
    if not LEARNINGS.exists():
        return fail(
            f"{LEARNINGS} missing. The FEEDBACK transition requires adapting new "
            "background knowledge into prompts/LEARNINGS.md."
        )
    text = LEARNINGS.read_text(encoding="utf-8")
    if f"#{issue}" not in text:
        return fail(
            f"No LEARNINGS entry references #{issue}. Record what the cycle "
            "taught (new background knowledge, CODING_STYLE re-surfacing) in "
            f"{LEARNINGS} before completing FEEDBACK."
        )
    return ok(f"LEARNINGS entry exists for #{issue}; FEEDBACK transition complete")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
    cmd = sys.argv[1]
    try:
        if cmd == "check-close":
            return cmd_check_close(int(sys.argv[2]))
        if cmd == "check-commit":
            return cmd_check_commit(sys.argv[2])
        if cmd == "check-feedback":
            return cmd_check_feedback(int(sys.argv[2]))
    except (RuntimeError, ValueError) as e:
        return fail(str(e))
    print(f"unknown subcommand: {cmd}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
