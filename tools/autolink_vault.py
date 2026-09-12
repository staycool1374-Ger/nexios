#!/usr/bin/env python3
"""Auto-linker (layer 2): deterministic [[wikilink]] cross-references.

Scans vault Markdown notes and (re)generates an idempotent
`## Related` section per note from filename-stem mentions found in
other notes' text. No LLM, stdlib only. Byte-identical outside the
managed block.

Usage:
  python3 tools/autolink_vault.py --vault ~/vaults/jarvis [--apply]

Without --apply: dry-run, prints planned link counts only.
"""
import argparse
import re
import sys
from pathlib import Path

START = "<!-- auto-links:start -->"
END = "<!-- auto-links:end -->"
MAX_LINKS = 15


def stem(path: Path) -> str:
    return path.stem.lower()


def stem_pattern(stem_text: str) -> "re.Pattern[str]":
    # Match stem with . _ - interchangeable, optional trailing .md,
    # bounded so `cspace` doesn't fire inside `cspacer`.
    alt = re.escape(stem_text).replace(r"\-", "[-_.]").replace(r"\_", "[-_.]")
    return re.compile(r"(?<![\w])" + alt + r"(?:\.md)?(?![\w])", re.IGNORECASE)


def existing_targets(text: str) -> set:
    return set(re.findall(r"\[\[([^\]|#]+)", text))


def build_block(targets: list) -> str:
    lines = ["", "## Related", "", START]
    lines += ["- [[" + t + "]]" for t in targets]
    lines += [END, ""]
    return "\n".join(lines)


def strip_block(text: str) -> str:
    idx = text.find("\n## Related\n\n" + START)
    if idx == -1:
        return text.rstrip("\n") + "\n"
    end = text.find(END, idx)
    assert end != -1, "managed block start without end"
    return (text[:idx] + text[end + len(END):]).rstrip("\n") + "\n"


def main() -> int:
    args = argparse.ArgumentParser()
    args.add_argument("--vault", required=True)
    args.add_argument("--apply", action="store_true")
    opts = args.parse_args()

    vault = Path(opts.vault).expanduser()
    notes = sorted(
        [p for p in vault.rglob("*.md") if ".obsidian-mcp" not in p.parts],
        key=lambda p: p.name,
    )
    if not notes:
        print("no notes found", file=sys.stderr)
        return 1

    stems = {p: stem(p) for p in notes}
    patterns = {p: stem_pattern(s) for p, s in stems.items()}
    texts = {p: p.read_text(encoding="utf-8") for p in notes}

    changed = 0
    for note in notes:
        text = texts[note]
        fm = ""
        scan = text
        if text.startswith("---\n"):
            end = text.find("\n---", 4)
            if end != -1:
                fm = text[: end + 4]
                scan = text[end + 4 :]
        body = strip_block(scan)
        have = existing_targets(body)
        hits = []
        for cand, pat in patterns.items():
            if cand == note or cand.stem in have:
                continue
            if pat.search(body):
                hits.append(cand.stem)
        hits = sorted(set(hits))[:MAX_LINKS]
        new_text = fm + (body + build_block(hits) if hits else body)
        if new_text != texts[note]:
            changed += 1
            print(f"{'WRITE' if opts.apply else 'PLAN'} {note.name}: {len(hits)} links")
            if opts.apply:
                note.write_text(new_text, encoding="utf-8")
    print(f"{'wrote' if opts.apply else 'would write'} {changed}/{len(notes)} notes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
