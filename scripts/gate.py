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
                            considered complete. Worktree or refs/heads/main.
   self-test                Deterministic self-test (scratch repo + fixture
                            gh, no network).

Exit codes: 0 = gate passed, 1 = gate violated.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = "staycool1374-Ger/nexios"
GH = os.environ.get(
    "NEXIOS_GH_BIN",
    "/opt/homebrew/bin/gh" if Path("/opt/homebrew/bin/gh").exists() else "gh",
)
ROOT = Path(__file__).resolve().parent.parent


def repo_root() -> Path:
    """Repo under test (NEXIOS_REPO seam: self-test scratch repos)."""
    return Path(os.environ.get("NEXIOS_REPO", str(ROOT)))


def main_learnings_text(root: Path) -> str:
    """prompts/LEARNINGS.md as committed on refs/heads/main ("" when absent).

    Worktree-independent on purpose: the gate must see entries already
    shipped to main from any checkout (#425). Never touches the network.
    """
    result = subprocess.run(
        ["git", "-C", str(root), "show", "refs/heads/main:prompts/LEARNINGS.md"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return ""
    return result.stdout
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
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh failed: {result.stderr.strip()}")
    state = result.stdout.strip()
    if state != "closed":
        return fail(f"issue #{issue} is not closed yet; FEEDBACK applies after close")
    root = repo_root()
    learnings = root / "prompts" / "LEARNINGS.md"
    hits = []
    if learnings.exists() and f"#{issue}" in learnings.read_text(encoding="utf-8"):
        hits.append("worktree")
    if f"#{issue}" in main_learnings_text(root):
        hits.append("refs/heads/main")
    if not hits:
        return fail(
            f"No LEARNINGS entry references #{issue} (checked worktree and "
            "refs/heads/main). Record what the cycle taught (new background "
            "knowledge, CODING_STYLE re-surfacing) in prompts/LEARNINGS.md "
            "before completing FEEDBACK."
        )
    return ok(
        f"LEARNINGS entry exists for #{issue} ({', '.join(hits)}); "
        "FEEDBACK transition complete"
    )


def _self_test() -> int:
    """Deterministic gate self-test (scratch repo + fixture gh, no network).

    Proves the #425 fix: the entry is found on refs/heads/main even when
    the worktree (feature checkout) does not have it, and the pre-push
    worktree-only flow still passes. Prints one PASS/FAIL line per case.
    """
    total = passed = 0

    def check(name: str, cond: bool) -> None:
        nonlocal total, passed
        total += 1
        if cond:
            passed += 1
            print(f"PASS: {name}")
        else:
            print(f"FAIL: {name}")

    with tempfile.TemporaryDirectory(prefix="gate-st-") as tmp:
        repo = Path(tmp) / "repo"
        repo.mkdir()
        run = lambda *a, **kw: subprocess.run(  # noqa: E731
            list(a), cwd=str(repo), capture_output=True, text=True, **kw
        )
        run("git", "init", "-q", "-b", "main", check=True)
        run("git", "config", "user.email", "t@t", check=True)
        run("git", "config", "user.name", "t", check=True)
        (repo / "prompts").mkdir()
        (repo / "prompts" / "LEARNINGS.md").write_text(
            "# L\n\n### #9000 — baseline\n", encoding="utf-8"
        )
        run("git", "add", "-A", check=True)
        run("git", "commit", "-qm", "base", check=True)
        gh_fixtures = Path(tmp) / "gh"
        gh_fixtures.mkdir()
        # Fixture gh answers lowercase like real gh (gh api --jq .state
        # yields "closed"/"open"); uppercase variants would fail the gate.
        for name, state in (("gh-open", "open"), ("gh-closed", "closed")):
            p = gh_fixtures / name
            p.write_text(f"#!/bin/sh\nprintf '{state}\\n'\n", encoding="utf-8")
            p.chmod(0o755)
        base_env = {
            "NEXIOS_REPO": str(repo),
            "NEXIOS_GH_BIN": str(gh_fixtures / "gh-closed"),
        }

        def gate(*args: str, closed: bool = True) -> "subprocess.CompletedProcess[str]":
            env = dict(os.environ)
            env.update(base_env)
            if not closed:
                env["NEXIOS_GH_BIN"] = str(gh_fixtures / "gh-open")
            return subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), *args],
                capture_output=True,
                text=True,
                env=env,
            )

        # 1. worktree-only entry passes (pre-push flow preserved).
        r = gate("check-feedback", "9000")
        check("worktree-only entry passes", r.returncode == 0)
        # 2. entry only on main passes from a feature checkout (the #425 fix).
        run("git", "checkout", "-qb", "fix/st-feature", check=True)
        r = gate("check-feedback", "9000")
        check("main-only entry passes from feature checkout", r.returncode == 0)
        # 3. entry nowhere fails.
        r = gate("check-feedback", "9001")
        check("missing entry fails", r.returncode != 0)
        # 4. open issue fails even with an entry.
        r = gate("check-feedback", "9000", closed=False)
        check("open issue fails", r.returncode != 0)
        # 5. missing main ref does not crash the worktree path.
        noroot = Path(tmp) / "noroot"
        noroot.mkdir()
        run3 = lambda *a, **kw: subprocess.run(  # noqa: E731
            list(a), cwd=str(noroot), capture_output=True, text=True, **kw
        )
        run3("git", "init", "-q", "-b", "trunk", check=True)
        run3("git", "config", "user.email", "t@t", check=True)
        run3("git", "config", "user.name", "t", check=True)
        (noroot / "prompts").mkdir()
        (noroot / "prompts" / "LEARNINGS.md").write_text(
            "# L\n\n### #9000 — baseline\n", encoding="utf-8"
        )
        run3("git", "add", "-A", check=True)
        run3("git", "commit", "-qm", "base", check=True)
        env = dict(os.environ)
        env.update(base_env)
        env["NEXIOS_REPO"] = str(noroot)
        r = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()),
             "check-feedback", "9000"],
            capture_output=True, text=True, env=env,
        )
        check("worktree entry passes with no main ref", r.returncode == 0)
    print(f"gate self-test: {passed}/{total} PASS")
    return 0 if passed == total else 1


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    cmd = sys.argv[1]
    if cmd == "self-test":
        return _self_test()
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
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
