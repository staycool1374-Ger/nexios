#!/usr/bin/env python3
"""verify_fence.py - structural gate over the RENDERED agent fences (#414).

#414 removed the hand-maintained copies: the permission map now lives only in
`tools/agents/fence/{base.json,roles/*.json}` and is composed by
`tools/agents/render_fence.py` at launch. So there is no golden file to diff
against (a golden would be the 14th copy) - this gate instead asserts the
PROPERTIES of what the renderer emits, per role:

  F1  the renderer succeeds for every role (fail-closed path is intact)
  F2  polarity is per-role: only dev has edit "*" allow; every role keeps bash
  F3  every /tmp rule has its /private/tmp mirror and vice versa, same decision
      (macOS canonicalizes before matching: an unmirrored rule is dead)
  F4  no host-dependent path leaks: no /var/folders, no $HOME, no "{repo}"
      residue; worker configs carry no repo-absolute path at all; consultant
      repo keys are exactly the documented advisory reads
  F5  no wildcard leaks: "*" appears only as the role's edit_default (last rule
      wins, so a "*" allow anywhere else would undo the fence)
  F6  consultant keeps its mcp block (harness+github); workers have none
  F7  if a rendered config exists under the runtime root, it is FRESH: byte
      equality with a fresh render (a stale file silently defuses the fence)
  F8  every generated subagent file carries its OWN permission block equal to
      the rendered map: an agent file without one inherits the caller's map,
      so the auditor invoked from a dev session would inherit edit:allow

Exit 0 clean, 2 on any failure (never 1: this is a configuration error).
`--self-test` proves each check bites against deliberately broken renders.
Stdlib only; the renderer is invoked as a subprocess so this verifies the real
shipped path, not a private reimplementation.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FENCE = os.path.join(ROOT, "tools", "agents", "fence")
RENDERER = os.path.join(ROOT, "tools", "agents", "render_fence.py")
RUNTIME = os.environ.get(
    "NEXIOS_HOME", os.path.join(os.path.expanduser("~"), ".local", "state", "nexios"))
RENDERED = os.path.join(RUNTIME, "rendered")
ROLES = ("dev", "qe", "auditor", "consultant")
CONSULT_REPO_KEYS = ("{repo}/prompts/**", "{repo}/audits/**", "{repo}/scripts/**", "{repo}/tools/agents/**", "{repo}/logs/**")

FAILS = []


def fail(check, detail):
    FAILS.append("%s: %s" % (check, detail))


def render(role):
    """Fresh render as text, or None (F1 records why)."""
    try:
        proc = subprocess.run(
            [sys.executable, RENDERER, "--role", role, "--print",
             "--repo-root", ROOT, "--fence-root", FENCE],
            capture_output=True, text=True)
    except OSError as exc:
        fail("F1", "%s: cannot run renderer: %s" % (role, exc))
        return None
    if proc.returncode != 0:
        fail("F1", "%s: renderer exited %d: %s"
             % (role, proc.returncode, (proc.stderr or "").strip()))
        return None
    return proc.stdout


def base_fence():
    with open(os.path.join(FENCE, "base.json"), encoding="utf-8") as handle:
        return json.load(handle)


def maps(cfg):
    perm = cfg["permission"]
    return perm.get("external_directory", {}), perm.get("edit", {})


def check_polarity(role, cfg):
    _, edit = maps(cfg)
    want = "allow" if role == "dev" else "deny"
    if edit.get("*") != want:
        fail("F2", "%s: edit '*' is %r, want %r" % (role, edit.get("*"), want))
    if cfg.get("permission", {}).get("bash") != "allow":
        fail("F2", "%s: bash is not allowed (launcher needs it)" % role)


def check_mirrors(role, cfg):
    for sect, table in zip(("external_directory", "edit"), maps(cfg)):
        for key, decision in table.items():
            if key.startswith("/tmp/"):
                twin = "/private" + key
            elif key.startswith("/private/tmp/"):
                twin = key[len("/private"):]
            else:
                continue
            if table.get(twin) != decision:
                fail("F3", "%s %s: %s has no mirror %s (or mismatched)"
                     % (role, sect, key, twin))


def check_paths(role, cfg, repo):
    ext, edit = maps(cfg)
    text = json.dumps(cfg)
    if "/var/folders" in text:
        fail("F4", "%s: host TMPDIR (/var/folders) is allowlisted" % role)
    if "{repo}" in text:
        fail("F4", "%s: unresolved {repo} token" % role)
    if "$HOME" in text or "~/" in text:
        fail("F4", "%s: shell-dependent home path in a config" % role)
    repo_keys = {k for k in list(ext) + list(edit) if k.startswith(repo)}
    if role == "consultant":
        want = {repo + k[len("{repo}"):] for k in CONSULT_REPO_KEYS}
        if repo_keys != want:
            fail("F4", "%s: repo keys %s != %s"
                 % (role, sorted(repo_keys), sorted(want)))
    elif repo_keys:
        fail("F4", "%s: worker carries repo-absolute keys %s"
             % (role, sorted(repo_keys)))


def check_wildcards(role, cfg, repo=None):
    """'*' is legal exactly once per map, as the role's declared default."""
    ext, edit = maps(cfg)
    base = base_fence()
    if ext.get("*") != base["external_default"]:
        fail("F5", "%s: external '*' is %r, want the declared default %r"
             % (role, ext.get("*"), base["external_default"]))
    if edit.get("*") not in ("allow", "deny"):
        fail("F5", "%s: edit '*' carries no decision" % role)
    allowed = [ns["root"] for ns in base["namespaces"]
               if ns.get("roles") is None or role in ns["roles"]]
    for sect, table in (("external_directory", ext), ("edit", edit)):
        for key in table:
            if key == "*":
                continue
            if not key.startswith("/"):
                if sect == "external_directory":
                    fail("F5", "%s: relative external rule %r" % (role, key))
                continue
            real = key[len("/private"):] if key.startswith("/private/tmp/") else key
            if repo and real.startswith(repo):
                continue
            if not any(real.startswith(root) for root in allowed):
                fail("F5", "%s %s rule %r outside the declared namespaces"
                     % (role, sect, key))


def check_mcp(role, cfg):
    has = isinstance(cfg.get("mcp"), dict)
    if role == "consultant":
        if not has or "github" not in cfg.get("mcp", {}) \
                or "harness" not in cfg.get("mcp", {}):
            fail("F6", "consultant: mcp block missing harness/github")
    elif has:
        fail("F6", "%s: worker carries an mcp block" % role)


def load(text, role):
    try:
        cfg = json.loads(text)
    except ValueError as exc:
        fail("F1", "%s: rendered output is not JSON: %s" % (role, exc))
        return None
    if not isinstance(cfg, dict) or "permission" not in cfg:
        fail("F1", "%s: rendered output has no permission block" % role)
        return None
    return cfg


def freshness(role, text):
    path = os.path.join(RENDERED, role, "opencode.resolved.json")
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as handle:
        disk = handle.read()
    if disk != text:
        fail("F7", "%s: %s is STALE (re-render at launch)" % (role, path))


def check_agents(role, cfg, agents_dir=None):
    """Generated agent files must be self-contained cages (F8)."""
    with tempfile.TemporaryDirectory(prefix="verify-fence-") as tmp:
        out = agents_dir
        if out is None:
            proc = subprocess.run(
                [sys.executable, RENDERER, "--role", role,
                 "--out", os.path.join(tmp, "opencode.resolved.json"),
                 "--twins-out", os.path.join(tmp, "agents"),
                 "--repo-root", ROOT, "--fence-root", FENCE],
                capture_output=True, text=True)
            if proc.returncode != 0:
                fail("F8", "%s: agent generation failed: %s"
                     % (role, (proc.stderr or "").strip()))
                return
            out = os.path.join(tmp, "agents")
        if not os.path.isdir(out):
            return
        _, edit = maps(cfg)
        with open(os.path.join(FENCE, "roles", role + ".json"), encoding="utf-8") as handle:
            agent_bash = (json.load(handle).get("agent") or {}).get("bash")
        for name in sorted(os.listdir(out)):
            path = os.path.join(out, name)
            with open(path, encoding="utf-8") as handle:
                lines = handle.read().splitlines()
            if not lines or lines[0] != "---":
                fail("F8", "%s/%s: no frontmatter" % (role, name))
                continue
            if "permission:" not in lines:
                fail("F8", "%s/%s: no permission block (would inherit the "
                     "caller's map)" % (role, name))
                continue
            got, sect = {}, None
            for line in lines[1:]:
                if line == "---":
                    break
                if line == "  edit:":
                    sect = "edit"
                elif line.startswith("  ") and not line.startswith("    "):
                    sect = None
                elif line.startswith("    ") and sect == "edit":
                    key, _, value = line.strip().partition(": ")
                    got[json.loads(key)] = value
            if got != edit:
                fail("F8", "%s/%s: edit map differs from the rendered cage"
                     % (role, name))
            if isinstance(agent_bash, dict):
                got_bash, bsec = {}, None
                for line in lines[1:]:
                    if line == "---":
                        break
                    if line == "  bash:":
                        bsec = "bash"
                    elif line.startswith("  ") and not line.startswith("    "):
                        bsec = None
                    elif line.startswith("    ") and bsec == "bash":
                        key, _, value = line.strip().partition(": ")
                        got_bash[json.loads(key)] = value
                if got_bash != agent_bash:
                    fail("F8", "%s/%s: bash policy drifted from agent.bash"
                         % (role, name))
            disk = os.path.join(RENDERED, role, name)
            if os.path.exists(disk):
                with open(disk, encoding="utf-8") as handle:
                    if handle.read() != open(path, encoding="utf-8").read():
                        fail("F8", "%s/%s: on-disk agent file is STALE" % (role, name))


def audit(renders):
    repo = os.path.realpath(ROOT)
    for role in ROLES:
        text = renders[role]
        cfg = load(text, role)
        if cfg is None:
            continue
        check_polarity(role, cfg)
        check_mirrors(role, cfg)
        check_paths(role, cfg, repo)
        check_wildcards(role, cfg, repo)
        check_mcp(role, cfg)
        check_agents(role, cfg)
        freshness(role, text)


def self_test():
    """Each check must reject a deliberately broken render."""
    base = {role: render(role) for role in ROLES}
    for role, text in base.items():
        if text is None:
            print("self-test: cannot render %s" % role)
            return 2

    def rejects(fn, role, mutate, *extra):
        cfg = json.loads(base[role])
        mutate(cfg)
        saved = len(FAILS)
        before = list(FAILS)
        del FAILS[:]
        fn(role, cfg, *extra)
        bit = len(FAILS) > 0
        del FAILS[:]
        FAILS.extend(before)
        return bit

    def drop_mirror(cfg):
        for sect in ("external_directory", "edit"):
            table = cfg["permission"][sect]
            for key in list(table):
                if key.startswith("/tmp/"):
                    table.pop("/private" + key, None)
                    return

    repo = os.path.realpath(ROOT)
    probes = [
        ("F2 polarity", rejects(check_polarity, "qe",
                                lambda c: c["permission"]["edit"].__setitem__("*", "allow"))),
        ("F3 mirror", rejects(check_mirrors, "dev", drop_mirror)),
        ("F4 host tmp", rejects(check_paths, "dev",
                                lambda c: c["permission"]["edit"].__setitem__(
                                    "/var/folders/x", "allow"), repo)),
        ("F4 {repo}", rejects(check_paths, "dev",
                              lambda c: c["permission"]["edit"].__setitem__(
                                  "{repo}/x", "allow"), repo)),
        ("F5 wildcard", rejects(check_wildcards, "qe",
                                lambda c: c["permission"]["external_directory"].__setitem__(
                                    "/tmp/*", "allow"), repo)),
        ("F5 order leak", rejects(check_wildcards, "qe",
                                  lambda c: c["permission"]["external_directory"].__setitem__(
                                      "*", "allow"), repo)),
        ("F6 mcp", rejects(check_mcp, "dev",
                           lambda c: c.__setitem__("mcp", {"x": {}}))),
    ]
    with tempfile.TemporaryDirectory(prefix="verify-fence-st-") as tmp:
        agents = os.path.join(tmp, "agents")
        proc = subprocess.run(
            [sys.executable, RENDERER, "--role", "auditor",
             "--out", os.path.join(tmp, "opencode.resolved.json"),
             "--twins-out", agents, "--repo-root", ROOT, "--fence-root", FENCE],
            capture_output=True, text=True)
        if proc.returncode != 0:
            print("self-test: cannot generate an agent file")
            return 2
        target = os.path.join(agents, "sil3_auditor.md")
        body = open(target, encoding="utf-8").read()
        stripped = "\n".join(line for line in body.splitlines()
                             if line != "permission:")
        open(target, "w", encoding="utf-8").write(stripped)
        probes.append(("F8 self-cage", rejects(
            check_agents, "auditor", lambda c: None, agents)))

    bad = [name for name, ok in probes if not ok]
    if bad:
        print("self-test: checks do not bite: %s" % ", ".join(bad))
        return 2
    print("verify_fence self-test: %d/%d checks bite" % (len(probes), len(probes)))
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    renders = {role: render(role) for role in ROLES}
    audit(renders)
    if FAILS:
        for line in FAILS:
            print("verify_fence: FAIL %s" % line)
        print("verify_fence: %d failure(s)" % len(FAILS))
        return 2
    print("verify_fence: OK (%d roles, rendered-output checks)" % len(ROLES))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
