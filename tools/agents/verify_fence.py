#!/usr/bin/env python3
"""verify_fence.py - mechanical consistency gate for the agent fences (#412).

The role fences are duplicated across five kinds of file: the role JSONs, the
coordinator config, the subagent twins, the audit prompt and the embedded
fallbacks inside scripts/agent_up.sh. Drift between copies silently defuses a
fence, and a visual diff cannot prove identity. This checks the families the
#410 burn-in and the #412 audit made load-bearing:

  F1  heredoc fallbacks byte-identical to the repo role JSONs
  F2  worker external_directory map identical across JSONs, twins, audit prompt
  F3  coordinator family identical, and exactly workers + the {repo} read keys
  F4  edit maps identical inside their family
  F5  every /tmp rule mirrored as /private/tmp (and vice versa), same decision
  F6  no un-narrowed audits/report-*; {repo} only under prompts/ or audits/
  F7  the /tmp namespaces the tooling writes to are still allowed (+ mirrors)

Exit 0 clean, 2 on any drift (never 1: drift is a configuration error).
Stdlib only.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROLES = os.path.join(ROOT, "tools", "agents", "roles")
COORD = os.path.join(ROOT, "tools", "agents", "coordinator", "opencode.json")
TWINS = os.path.join(ROOT, "tools", "agents", "twins")
AUDIT_PROMPT = os.path.join(ROOT, "prompts", "PROMPT-audit.md")
AGENT_UP = os.path.join(ROOT, "scripts", "agent_up.sh")

ROLE_JSON = {r: os.path.join(ROLES, r + ".json")
             for r in ("dev", "qe", "auditor", "coordinator")}
TWIN = {r: os.path.join(TWINS, "nexios-%s.md" % ("audit" if r == "auditor" else r))
        for r in ("dev", "qe", "auditor")}
REPO_READ_KEYS = [("{repo}/prompts/**", "allow"),
                  ("{repo}/audits/**", "allow")]

FAILS = []


def fail(check, detail):
    FAILS.append((check, detail))


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def load_json(path):
    try:
        return json.loads(read(path))
    except ValueError as exc:
        fail("PARSE", "%s: %s" % (path, exc))
        return None


def frontmatter(path):
    """Return the mapping block of a markdown agent file's YAML frontmatter.

    Deliberately strict: only `key: value` / `key:` lines at even indentation
    with a bare or double-quoted key. Anything else is a parse error, not a
    silently skipped line (a fence must never be skipped by a formatter).
    """
    text = read(path)
    if not text.startswith("---\n"):
        fail("PARSE", "%s: no YAML frontmatter" % path)
        return None
    body = text.split("\n---\n", 1)[0][4:]
    out = {}
    stack = [(-1, out)]
    for lineno, line in enumerate(body.split("\n"), start=2):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent % 2:
            fail("PARSE", "%s:%d: odd indentation" % (path, lineno))
            return None
        stripped = line.strip()
        if stripped.startswith('"'):
            match = re.match(r'^("(?:[^"\\]|\\.)*")\s*:\s*(.*)$', stripped)
            if not match:
                fail("PARSE", "%s:%d: bad quoted key" % (path, lineno))
                return None
            key = json.loads(match.group(1))
            value = match.group(2)
        else:
            match = re.match(r"^([A-Za-z0-9_*./-]+)\s*:\s*(.*)$", stripped)
            if not match:
                fail("PARSE", "%s:%d: not `key: value`" % (path, lineno))
                return None
            key = match.group(1)
            value = match.group(2)
        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack:
            fail("PARSE", "%s:%d: dedent past root" % (path, lineno))
            return None
        parent = stack[-1][1]
        if value == "":
            node = {}
            parent[key] = node
            stack.append((indent, node))
        else:
            if value in ("allow", "deny", "ask", "true", "false"):
                parsed = value if value in ("allow", "deny", "ask") else (value == "true")
            else:
                match = re.match(r'^"((?:[^"\\]|\\.)*)"$', value)
                if match:
                    parsed = json.loads('"%s"' % match.group(1))
                else:
                    parsed = value
            parent[key] = parsed
    return out


def heredocs(path):
    """Exact bytes of each write_role_*() fallback body."""
    text = read(path)
    found = {}
    for role in ("dev", "qe", "auditor", "coordinator"):
        pattern = (r"write_role_%s\(\) \{\n  cat > \"\$1\" <<'EOF'\n"
                   r"(.*?)\nEOF\n\}" % role)
        match = re.search(pattern, text, re.S)
        if not match:
            fail("PARSE", "%s: no write_role_%s heredoc" % (path, role))
        else:
            found[role] = match.group(1) + "\n"
    return found


def ext_map(config):
    return list(config["permission"]["external_directory"].items())


def edit_map(config):
    return list(config["permission"]["edit"].items())


def main():
    configs = {role: load_json(path) for role, path in ROLE_JSON.items()}
    coord_config = load_json(COORD)
    twins = {role: frontmatter(path) for role, path in TWIN.items()}
    audit_prompt = frontmatter(AUDIT_PROMPT)
    embedded = heredocs(AGENT_UP)
    for label, block in (("nexios-dev.md", twins["dev"]),
                         ("nexios-qe.md", twins["qe"]),
                         ("nexios-audit.md", twins["auditor"]),
                         ("PROMPT-audit.md", audit_prompt)):
        if block is not None and not block.get("permission"):
            fail("PARSE", "%s: empty permission block (skipped, not clean)"
                 % label)
    if FAILS:
        report()
        return 2

    # F1 - embedded fallbacks are the repo files, byte for byte.
    for role in ("dev", "qe", "auditor", "coordinator"):
        want = read(ROLE_JSON[role])
        got = embedded.get(role, "")
        if got != want:
            fail("F1", "%s heredoc differs from %s" % (role, ROLE_JSON[role]))

    # F2 - one worker external_directory map across seven files.
    worker = ext_map(configs["dev"])
    family = [("dev.json", worker),
              ("qe.json", ext_map(configs["qe"])),
              ("auditor.json", ext_map(configs["auditor"]))]
    if twins["dev"] is not None:
        family.append(("nexios-dev.md", ext_map(twins["dev"])))
    if twins["qe"] is not None:
        family.append(("nexios-qe.md", ext_map(twins["qe"])))
    if twins["auditor"] is not None:
        family.append(("nexios-audit.md", ext_map(twins["auditor"])))
    if audit_prompt is not None:
        family.append(("PROMPT-audit.md", ext_map(audit_prompt)))
    for name, got in family:
        if got != worker:
            fail("F2", "%s external_directory differs from dev.json" % name)

    # F3 - coordinator family is identical, and exactly workers + {repo} keys.
    coord_roles = ext_map(configs["coordinator"])
    coord_cfg = ext_map(coord_config) if coord_config else None
    if coord_cfg != coord_roles:
        fail("F3", "coordinator/opencode.json external_directory differs from "
                   "roles/coordinator.json")
    want_coord = [worker[0]] + REPO_READ_KEYS + worker[1:]
    if coord_roles != want_coord:
        fail("F3", "coordinator external_directory is not workers + %s"
                   % ([k for k, _ in REPO_READ_KEYS],))
    for role, block in (("dev", twins["dev"]), ("qe", twins["qe"]),
                        ("auditor", twins["auditor"])):
        if block and set(ext_map(block)) & set(REPO_READ_KEYS):
            fail("F3", "%s twin carries a coordinator-only {repo} rule" % role)

    # F4 - edit maps identical inside each family.
    pairs = [("dev.json", edit_map(configs["dev"]), "nexios-dev.md",
              edit_map(twins["dev"]) if twins["dev"] is not None else None),
             ("qe.json", edit_map(configs["qe"]), "nexios-qe.md",
              edit_map(twins["qe"]) if twins["qe"] is not None else None),
             ("auditor.json", edit_map(configs["auditor"]), "nexios-audit.md",
              edit_map(twins["auditor"]) if twins["auditor"] is not None else None),
             ("auditor.json", edit_map(configs["auditor"]), "PROMPT-audit.md",
              edit_map(audit_prompt) if audit_prompt is not None else None)]
    for left, left_map, right, right_map in pairs:
        if right_map is not None and left_map != right_map:
            fail("F4", "%s edit map differs from %s" % (left, right))
    coord_edit = edit_map(configs["coordinator"])
    if coord_config and edit_map(coord_config) != coord_edit:
        fail("F4", "coordinator/opencode.json edit map differs from "
                   "roles/coordinator.json")
    if not any(k.startswith("{repo}/") for k, _ in coord_edit):
        fail("F4", "coordinator edit map has no repo-anchored LEARNINGS rule "
                   "(relative patterns resolve to a shadow under the coord cwd)")

    # F5 - /tmp and /private/tmp mirrors carry the same decision, both ways.
    for name, mapping in family + [("coordinator.json", coord_roles)]:
        decisions = dict(mapping)
        for key, value in mapping:
            if key.startswith("/private/tmp/"):
                continue
            if not key.startswith("/tmp/"):
                continue
            mirror = "/private" + key
            if mirror not in decisions:
                fail("F5", "%s: %s has no %s mirror" % (name, key, mirror))
            elif decisions[mirror] != value:
                fail("F5", "%s: %s and %s disagree" % (name, key, mirror))
        for key in decisions:
            if key.startswith("/private/tmp/"):
                plain = key[len("/private"):]
                if plain not in decisions:
                    fail("F5", "%s: %s has no %s mirror" % (name, key, plain))

    # F6 - narrowed report glob, and {repo} never escapes the repo subtrees.
    texts = [(os.path.relpath(path, ROOT), read(path)) for path in
             list(ROLE_JSON.values()) + [COORD, AUDIT_PROMPT] +
             list(TWIN.values()) + [AGENT_UP]]
    for name, text in texts:
        for match in re.finditer(r"audits/report-\*(?!\.md)", text):
            fail("F6", "%s: un-narrowed audits/report-* glob" % name)
            break
        # The launcher legitimately manipulates the token itself; only the
        # permission files must keep every {repo} inside a repo subtree.
        if name == os.path.relpath(AGENT_UP, ROOT):
            continue
        for match in re.finditer(r"\{repo\}(?!/prompts/|/audits/)", text):
            fail("F6", "%s: {repo} outside prompts//audits/: %r"
                       % (name, text[match.start():match.start() + 40]))
            break

    # F7 - the namespaces scripts and tooling actually write to are present.
    required = ["/tmp/nexios-*", "/tmp/nexios-*/**", "/tmp/jarvis-*",
                "/tmp/jarvis_*", "/tmp/matrix-*", "/tmp/qemu-serial.*",
                "/tmp/agentup-*", "/tmp/agentup-*/**", "/tmp/lldb-*"]
    have = dict(worker)
    for key in required:
        if key not in have:
            fail("F7", "worker map lost required namespace %s" % key)
        if have.get("/private" + key) is None:
            fail("F7", "worker map lost the %s mirror" % ("/private" + key))

    report()
    return 2 if FAILS else 0


def report():
    if not FAILS:
        print("verify_fence: F1-F7 clean (7 checks, 13 files)")
        return
    for check, detail in FAILS:
        print("FAIL %s: %s" % (check, detail))
    print("verify_fence: %d drift(s)" % len(FAILS))


if __name__ == "__main__":
    sys.exit(main())
