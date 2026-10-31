#!/usr/bin/env python3
"""render_fence.py - single-source permission fence renderer.

Composes `tools/agents/fence/base.json` with `tools/agents/fence/roles/<role>.json`
into one complete opencode config. This is the ONLY place a permission map is
assembled: the tracked files carry intent (polarity, allows, denies), never a
finished map. opencode has no permission `include`, so the emitted artifact is
necessarily concrete - it is rendered per launch, never committed.

Order is significant (opencode: the LAST matching rule wins):
  edit:                "*" -> base namespaces -> role allows -> base denies -> role denies
  external_directory:  "*" -> base namespaces -> role allows

Mirrors are DERIVED: every "/tmp/X" also emits "/private/tmp/X" with the same
decision, because macOS canonicalizes /tmp before matching (#410). A hand-typed
private/tmp entry is an error, not a second opinion.

Exit: 0 on success (file written, or --print/--meta emitted), 2 on any
fail-closed condition (one `render_fence: ERROR:` line, nothing written).
"""

import json
import os
import sys
from typing import Any, NoReturn

ROLES = ("dev", "qe", "auditor", "consultant")
BASE_KEYS = {"$schema", "bash", "external_default", "namespaces", "edit_deny"}
NS_KEYS = {"root", "decision", "external_variants", "edit_variants", "roles"}
DELTA_KEYS = {"role", "edit_default", "edit_allow", "edit_deny",
              "external_allow", "launch", "agent", "config_extra"}
LAUNCH_KEYS = {"cwd", "prompt"}
AGENT_KEYS = {"name", "twin", "description", "mode", "model", "temperature",
              "bash", "alias"}
POLARITY = ("allow", "deny")


class Fail(Exception):
    pass


def fail(msg: str) -> NoReturn:
    raise Fail(msg)


def load_json(path: str, what: str) -> Any:
    try:
        with open(path) as handle:
            return json.load(handle)
    except OSError as exc:
        fail("cannot read %s %s: %s" % (what, path, exc.strerror or exc))
    except ValueError as exc:
        fail("cannot parse %s %s: %s" % (what, path, exc))


def expect(cond, msg):
    if not cond:
        fail(msg)


def closed(obj, allowed, what):
    expect(isinstance(obj, dict), "%s must be an object" % what)
    for key in obj:
        expect(key in allowed, "unknown key %r in %s" % (key, what))


def str_list(value, what):
    expect(isinstance(value, list), "%s must be a list" % what)
    for item in value:
        expect(isinstance(item, str) and item, "%s must hold non-empty strings" % what)
        expect(item != "*", "%s may not restate '*'" % what)
    return list(value)


def mirror(path):
    if path.startswith("/tmp/"):
        return "/private" + path
    return None


def expand(key, repo, role):
    if "{repo}" in key:
        expect(role == "consultant",
               "{repo} is consultant-only (role %s)" % role)
        key = key.replace("{repo}", repo)
    elif role == "consultant" and not key.startswith("/"):
        fail("relative repo path in consultant role: %r" % key)
    return key


def put(target, key, value, origin):
    if key in target:
        fail("duplicate namespace %r (from %s)" % (key, origin))
    target[key] = value


def validate_base(base):
    closed(base, BASE_KEYS, "base fence")
    expect(isinstance(base.get("$schema"), str), "base $schema missing")
    expect(base.get("bash") in ("allow", "ask", "deny"), "base bash invalid")
    expect(base.get("external_default") in POLARITY, "base external_default invalid")
    namespaces = base.get("namespaces")
    expect(isinstance(namespaces, list) and namespaces, "base namespaces missing")
    for ns in namespaces:
        closed(ns, NS_KEYS, "base namespace")
        root = ns.get("root")
        expect(isinstance(root, str) and root.startswith("/tmp/"),
               "namespace must be /tmp/<pattern>: %r" % (root,))
        expect(not root.startswith("/private"), "namespace %r is already mirrored" % root)
        expect("**" not in root, "namespace root may not contain '**': %r" % root)
        expect(ns.get("decision") in POLARITY, "namespace %r decision invalid" % root)
        roles = ns.get("roles")
        if roles is not None:
            expect(isinstance(roles, list) and roles,
                   "namespace %r roles must be a non-empty list" % root)
            for name in roles:
                expect(name in ROLES,
                       "namespace %r scopes unknown role %r" % (root, name))
        for field in ("external_variants", "edit_variants"):
            if field in ns:
                variants = ns[field]
                expect(isinstance(variants, list), "namespace %r %s must be a list"
                       % (root, field))
                for variant in variants:
                    expect(isinstance(variant, str), "namespace %r %s must hold strings"
                           % (root, field))
                    expect(variant == "" or variant.startswith("/"),
                           "namespace %r variant must start with '/': %r" % (root, variant))
    if "edit_deny" in base:
        str_list(base["edit_deny"], "base edit_deny")


def validate_delta_paths(delta, base, role):
    """Absolute allow/deny keys must live in a declared namespace (or {repo})."""
    roots = [ns["root"] for ns in base["namespaces"]]
    for field in ("edit_allow", "edit_deny", "external_allow"):
        for item in delta.get(field, []):
            if item.startswith("{repo}"):
                continue
            if item.startswith("/private/"):
                fail("delta %s carries a hand-typed /private/tmp entry: %r"
                     % (field, item))
            if item.startswith("/"):
                expect(any(item.startswith(root) for root in roots),
                       "delta %s entry %r is outside the declared namespaces %s"
                       % (field, item, roots))


def validate_delta(delta, role):
    closed(delta, DELTA_KEYS, "delta for role %s" % role)
    expect(delta.get("role") == role, "delta role mismatch: %r != %r"
           % (delta.get("role"), role))
    expect(delta.get("edit_default") in POLARITY, "edit.default required (allow|deny)")
    for field in ("edit_allow", "edit_deny", "external_allow"):
        if field in delta:
            str_list(delta[field], "delta %s" % field)
            for item in delta[field]:
                expect(item.startswith("/") or item.startswith("{repo}") or "/" in item,
                       "delta %s entry must be a path: %r" % (field, item))
    if "launch" in delta:
        launch = delta["launch"]
        closed(launch, LAUNCH_KEYS, "delta launch")
        expect(launch.get("cwd") in ("dev", "qe", "audit", "coord"),
               "launch.cwd must be dev|qe|audit|coord")
    if "agent" in delta:
        agent = delta["agent"]
        closed(agent, AGENT_KEYS, "delta agent")
        expect(isinstance(agent.get("name"), str) and agent["name"], "agent.name required")
        expect(agent.get("mode") in ("subagent", "primary", "all"), "agent.mode invalid")
        expect(isinstance(agent.get("description"), str) and agent["description"],
               "agent.description required")
        if "model" in agent:
            expect(isinstance(agent["model"], str) and agent["model"],
                   "agent.model must be a model id")
        if "temperature" in agent:
            temp = agent["temperature"]
            expect(isinstance(temp, (int, float)) and not isinstance(temp, bool)
                   and 0.0 <= temp <= 1.0,
                   "agent.temperature must be a number in [0, 1]")
        if "bash" in agent:
            bash = agent["bash"]
            if isinstance(bash, dict):
                expect(bool(bash), "agent.bash map is empty")
                for pattern, decision in bash.items():
                    expect(isinstance(pattern, str) and pattern,
                           "agent.bash pattern must be a non-empty string")
                    expect(decision in ("allow", "ask", "deny"),
                           "agent.bash decision must be allow|ask|deny: %r" % (decision,))
            else:
                expect(bash in ("allow", "ask", "deny"),
                       "agent.bash must be allow|ask|deny or a pattern map")
        if "alias" in agent:
            alias = agent["alias"]
            expect(isinstance(alias, list) and alias, "agent.alias must be a non-empty list")
            for name in alias:
                expect(isinstance(name, str) and name.endswith(".md")
                       and "/" not in name and name not in (".", ".."),
                       "agent.alias must be a bare .md filename: %r" % (name,))
    if "config_extra" in delta:
        expect(isinstance(delta["config_extra"], dict), "config_extra must be an object")


def ns_applies(ns, role):
    """A namespace may be scoped to specific roles (default: every role)."""
    roles = ns.get("roles")
    return roles is None or role in roles


def compose(base, delta, repo, role):
    out = {"$schema": base["$schema"]}
    permission = {"bash": base["bash"]}

    ext = {"*": base["external_default"]}
    for ns in base["namespaces"]:
        if not ns_applies(ns, role):
            continue
        for variant in ns.get("external_variants", [""]):
            pattern = ns["root"] + variant
            put(ext, pattern, ns["decision"], "base namespace")
            put(ext, mirror(pattern), ns["decision"], "base mirror")
    for key in sorted(delta.get("external_allow", [])):
        put(ext, expand(key, repo, role), "allow", "delta external_allow")
    permission["external_directory"] = ext

    edit = {"*": delta["edit_default"]}
    for ns in base["namespaces"]:
        if not ns_applies(ns, role):
            continue
        for variant in ns.get("edit_variants", []):
            pattern = ns["root"] + variant
            put(edit, pattern, ns["decision"], "base edit namespace")
            put(edit, mirror(pattern), ns["decision"], "base mirror")
    for key in sorted(delta.get("edit_allow", [])):
        put(edit, expand(key, repo, role), "allow", "delta edit_allow")
    for key in sorted(base.get("edit_deny", [])):
        put(edit, key, "deny", "base edit_deny")
    for key in sorted(delta.get("edit_deny", [])):
        put(edit, expand(key, repo, role), "deny", "delta edit_deny")
    permission["edit"] = edit

    out["permission"] = permission
    for key, value in delta.get("config_extra", {}).items():
        expect(key in ("mcp", "model"),
               "config_extra may only carry 'mcp'/'model' (got %r)" % key)
        if key == "model":
            expect(isinstance(value, str) and "/" in value,
                   "config_extra model must be a provider/model id")
        expect(key not in out, "config_extra collision on %r" % key)
        out[key] = value
    return out


def check_mirrors(mapping, label):
    for key in list(mapping):
        if key.startswith("/tmp/"):
            twin = mirror(key)
            expect(mapping.get(twin) == mapping[key],
                   "%s: %r has no %r mirror with the same decision" % (label, key, twin))
        elif key.startswith("/private/tmp/"):
            twin = key[len("/private"):]
            expect(mapping.get(twin) == mapping[key],
                   "%s: %r has no %r mirror with the same decision" % (label, key, twin))


def dump(config):
    text = json.dumps(config, indent=2) + "\n"
    expect("{repo}" not in text, "unresolved {repo} token in the rendered config")
    expect("/var/folders" not in text, "host TMPDIR must never be fenced")
    try:
        json.loads(text)
    except ValueError as exc:
        fail("rendered config is not valid JSON: %s" % exc)
    return text


def write_atomic(path, text, label):
    expect(os.path.isabs(path), "%s path must be absolute: %r" % (label, path))
    parent = os.path.dirname(path)
    try:
        os.makedirs(parent, exist_ok=True)
    except OSError as exc:
        fail("cannot create %s: %s" % (parent, exc.strerror or exc))
    tmp = "%s.tmp.%d.%s" % (path, os.getpid(), os.urandom(6).hex())
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                     | getattr(os, "O_NOFOLLOW", 0), 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, path)
    except OSError as exc:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        fail("cannot write %s: %s" % (path, exc.strerror or exc))
    try:
        dir_fd = os.open(parent, os.O_RDONLY)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        pass


def yaml_map(mapping, indent="    "):
    lines = []
    for key, value in mapping.items():
        lines.append('%s%s: %s' % (indent, json.dumps(key), value))
    return "\n".join(lines)


def twin_text(agent, permission):
    head = ["---",
            "description: %s" % json.dumps(agent["description"]),
            "mode: %s" % agent["mode"]]
    if "model" in agent:
        head.append("model: %s" % agent["model"])
    if "temperature" in agent:
        head.append("temperature: %s" % agent["temperature"])
    head.append("permission:")
    bash = agent.get("bash", permission["bash"])
    if isinstance(bash, dict):
        head.append("  bash:")
        head.append(yaml_map(bash))
    else:
        head.append("  bash: %s" % bash)
    head.append("  external_directory:")
    head.append(yaml_map(permission["external_directory"]))
    head.append("  edit:")
    head.append(yaml_map(permission["edit"]))
    head.append("---")
    return "\n".join(head)


def strip_frontmatter(text):
    if not text.startswith("---\n"):
        return text
    end = text.find("\n---\n", 3)
    if end < 0:
        fail("tracked twin has an unterminated frontmatter block")
    return text[end + 5:]


def parse_twin_map(text):
    """Re-parse the emitted frontmatter's permission maps (drift guard)."""
    lines = text.splitlines()
    expect(lines and lines[0] == "---", "generated twin has no frontmatter")
    maps = {"external_directory": {}, "edit": {}, "bash": {}}
    section = None
    for line in lines[1:]:
        if line == "---":
            break
        if line == "  external_directory:":
            section = "external_directory"
            continue
        if line == "  edit:":
            section = "edit"
            continue
        if line == "  bash:":
            section = "bash"
            continue
        if line.startswith("    ") and section:
            key, _, value = line.strip().partition(": ")
            maps[section][json.loads(key)] = value

    return maps


def emit_twin(delta, permission, fence_root, twins_out):
    agent = delta.get("agent")
    if not agent or not twins_out:
        return None
    twin = agent.get("twin")
    expect(twin, "agent.twin is required to generate a twin file")
    src = os.path.join(os.path.dirname(fence_root.rstrip("/")), "twins", twin)
    body = ""
    try:
        with open(src) as handle:
            body = strip_frontmatter(handle.read())
    except OSError as exc:
        fail("cannot read twin body %s: %s" % (src, exc.strerror or exc))
    text = twin_text(agent, permission) + "\n" + body
    parsed = parse_twin_map(text)
    expect(parsed["external_directory"] == permission["external_directory"],
           "generated twin external_directory drifted from the rendered map")
    expect(parsed["edit"] == permission["edit"],
           "generated twin edit map drifted from the rendered map")
    if isinstance(agent.get("bash"), dict):
        expect(parsed["bash"] == agent["bash"],
               "generated twin bash map drifted from agent.bash")
    written = []
    for name in [agent["name"] + ".md"] + list(agent.get("alias", [])):
        dest = os.path.join(twins_out, name)
        write_atomic(dest, text, "twin")
        written.append(dest)
    return ", ".join(written)


def parse_args(argv):
    args = {"role": None, "out": None, "repo_root": None, "fence_root": None,
            "twins_out": None, "print": False, "meta": False, "field": None}
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--print":
            args["print"] = True
        elif arg == "--meta":
            args["meta"] = True
        elif arg in ("--role", "--out", "--repo-root", "--fence-root",
                     "--twins-out", "--field"):
            i += 1
            expect(i < len(argv), "%s needs a value" % arg)
            args[arg[2:].replace("-", "_")] = argv[i]
        elif arg in ("-h", "--help"):
            sys.stdout.write(__doc__)
            raise SystemExit(0)
        else:
            fail("unknown argument: %r" % arg)
        i += 1
    return args


def main(argv):
    args = parse_args(argv)
    role = args["role"]
    expect(role in ROLES, "unknown role: %r (want %s)" % (role, "|".join(ROLES)))
    fence_root = args["fence_root"] or "" or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "fence")
    base: Any = load_json(os.path.join(fence_root, "base.json"), "base fence")
    delta: Any = load_json(os.path.join(fence_root, "roles", role + ".json"),
                           "delta for role %s" % role)
    validate_base(base)
    validate_delta(delta, role)
    validate_delta_paths(delta, base, role)

    repo = args["repo_root"] or os.getcwd()
    expect(os.path.isabs(repo), "repo root must be absolute: %r" % repo)
    expect(os.path.isdir(os.path.join(repo, "prompts")),
           "repo root %s has no prompts/ (wrong checkout?)" % repo)
    repo = os.path.realpath(repo)

    if args["meta"]:
        meta = {"role": role,
                "launch": delta.get("launch", {}),
                "agent": delta.get("agent", {})}
        field = args["field"]
        if field:
            value = meta["launch"].get(field, meta["agent"].get(field, ""))
            sys.stdout.write("%s\n" % ("" if value is None else value))
        else:
            sys.stdout.write(json.dumps(meta, indent=2) + "\n")
        return 0

    config: Any = compose(base, delta, repo, role)
    check_mirrors(config["permission"]["external_directory"], "external_directory")
    check_mirrors(config["permission"]["edit"], "edit")
    text = dump(config)

    if args["print"]:
        sys.stdout.write(text)
        return 0

    out = args["out"] or ""
    expect(out, "--out is required (or use --print)")
    real_out = os.path.realpath(os.path.dirname(os.path.abspath(out)))
    inside = os.path.realpath(repo)
    expect(not (real_out == inside or real_out.startswith(inside + os.sep)),
           "refusing to write inside the repo: %s" % out)
    write_atomic(out, text, "rendered config")
    twin = emit_twin(delta, config["permission"], fence_root, args["twins_out"])
    if twin:
        sys.stderr.write("render_fence: twin %s\n" % twin)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Fail as exc:
        sys.stderr.write("render_fence: ERROR: %s\n" % exc)
        sys.exit(2)
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001 - never a raw traceback
        sys.stderr.write("render_fence: ERROR: %s: %s\n"
                         % (type(exc).__name__, exc))
        sys.exit(2)
