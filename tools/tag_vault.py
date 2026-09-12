#!/usr/bin/env python3
"""Tagger: deterministic kind + subsystem frontmatter tags for vault notes.

Kind from path/stem; subsystem from keyword hits (>=2 occurrences).
Owns only the `tags:` frontmatter key; all other keys and the body
stay byte-identical. Idempotent.

Usage:
  python3 tools/tag_vault.py --vault ~/vaults/jarvis [--apply]
"""
import argparse
import re
import sys
from pathlib import Path

# token -> sys/<tag>; short tokens (<=4 chars) match on boundaries.
SUBSYS = {
    "memory": ["pmm", "vmm", "mempool", "oom", "page_table", "heap"],
    "scheduler": ["scheduler", "sporadic", "ready queue", "reschedule"],
    "ipc": ["ipc", "message queue", "notify", "endpoint"],
    "abi": ["syscall", "abi"],
    "vfs": ["vfs", "fat32", "tmpfs", "procfs", "vnode"],
    "sync": ["mutex", "semaphore", "spinlock", "waiter"],
    "cap": ["cspace", "untyped", "msix", "iommu", "mmio"],
    "drivers": ["ahci", "virtio", "pci", "driver"],
    "elf": ["elf", "runelf"],
    "arch": ["aarch64", "riscv", "smp", "gdt", "apic"],
    "net": ["net", "tcp", "nic"],
    "shell": ["shell", "ksh"],
    "test": ["coverage", "harness", "test class"],
    "boot": ["boot", "grub", "multiboot"],
    "debug": ["profiling", "gcov", "trace"],
}
HITS_REQUIRED = 2


def kind_of(path: Path, head: str) -> str:
    name = path.stem.lower()
    parent = path.parent.name
    if parent == "audits":
        if name.startswith("report-"):
            return "report"
        if name.startswith("plan-"):
            return "plan"
        return "audit"
    if name in ("test-harness", "coverage"):
        return "test-spec"
    if "IMPLEMENTED" in head:
        return "spec-impl"
    return "spec-design"


def count(token: str, text: str) -> int:
    if len(token) <= 4 and " " not in token:
        pat = r"(?<![\w])" + re.escape(token) + r"(?![\w])"
    else:
        pat = re.escape(token)
    return len(re.findall(pat, text))


def split_frontmatter(text: str):
    if text.startswith("---\n"):
        end = text.find("\n---", 4)
        if end != -1:
            return text[: end + 4], text[end + 4:]
    return "", text


def set_tags(fm: str, tags: list) -> str:
    inner = fm
    if inner.startswith("---\n"):
        inner = inner[4:]
    inner = inner.strip("\n")
    if inner.endswith("---"):
        inner = inner[:-3].strip("\n")
    lines = [ln for ln in inner.splitlines() if not re.match(r"^tags\s*:", ln)]
    tagline = "tags: [" + ", ".join(tags) + "]"
    return "---\n" + tagline + ("\n" + "\n".join(lines) if lines else "") + "\n---"


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
    changed = 0
    for note in notes:
        text = note.read_text(encoding="utf-8")
        fm, body = split_frontmatter(text)
        body = re.sub(r"\A(---[ \t]*\n)+", "", body.lstrip("\n"))
        low = body.lower()
        tags = ["kind/" + kind_of(note, "\n".join(body.splitlines()[:10]))]
        for sys_tag, tokens in SUBSYS.items():
            if sum(count(t, low) for t in tokens) >= HITS_REQUIRED:
                tags.append("sys/" + sys_tag)
        new_fm = set_tags(fm, sorted(set(tags)))
        new_text = new_fm + "\n" + body.lstrip("\n")
        if new_text != text:
            changed += 1
            print(f"{'WRITE' if opts.apply else 'PLAN'} {note.name}: {tags}")
            if opts.apply:
                note.write_text(new_text, encoding="utf-8")
    print(f"{'wrote' if opts.apply else 'would write'} {changed}/{len(notes)} notes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
