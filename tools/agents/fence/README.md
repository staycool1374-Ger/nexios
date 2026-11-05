# Agent fence — one base, one delta per role, rendered at launch

`opencode` has no permission `include`, so every running agent needs a complete
concrete config. Before #414 that complete map was copied 13 times (role JSONs,
coordinator config, three twins, the audit prompt, four shell heredocs) and the
copies could silently drift apart — a drifted copy defuses a fence.

Now there is exactly one place that expresses intent and exactly one place that
assembles a map:

```
tools/agents/fence/base.json          shared namespaces + shared edit denies
tools/agents/fence/roles/<role>.json  per-role polarity, allows/denies, launch, agent
tools/agents/render_fence.py          composes, validates, writes (the only assembler)
```

Nothing in the repo is a finished permission map. Rendered artifacts live outside
the checkout:

```
$NEXIOS_HOME/rendered/<role>/opencode.resolved.json     config for a launched session
<session cwd>/.opencode/agents/<agent>.md               generated subagent definitions
```

## Launch flow

`scripts/agent_up.sh` renders before every launch and asserts afterwards:

1. `render_role_config <role>` → `$NEXIOS_HOME/rendered/<role>/opencode.resolved.json`
   (atomic, mode 0600, refuses to write inside the repo).
2. `install_agents <cwd>` → `nexios-dev.md`, `nexios-qe.md`, `nexios-audit.md`
   and the `sil3_auditor.md` alias into `<cwd>/.opencode/agents/` (gitignored,
   launcher-owned).
3. `tmux` starts `opencode` with `OPENCODE_CONFIG=<rendered>`,
   `NEXIOS_AGENT_ROLE=<role>` and `TMPDIR=/tmp/agentup-tmp-<session>`, plus
   role-delta launch flags (auditor only: `--agent nexios-audit --auto`;
   every other role launches bare and interactive).
4. `check_fenced` walks the pane's own process tree and requires all four values
   in the process environment; a mismatch kills the session.

A subagent file deliberately carries its own copy of the cage: opencode inherits
the caller's permissions when an agent file has no `permission` block, so the
auditor invoked from a dev session would otherwise inherit `edit: allow`.

## Order is the policy

`opencode` applies the **last** matching rule. The renderer emits, in order:

- `edit`: `"*"` → base namespaces → role allows → base denies → role denies
- `external_directory`: `"*"` → base namespaces → role allows
- `bash` (roles with `launch.agent` only, today just the auditor): the role's
  `agent.bash` map verbatim. Deny entries must be the tail — the renderer
  rejects any allow/ask after the first deny, and the F2 pin test
  (`verify_fence.py` self-test) proves it bites. (F9 covers consultant
  *edit*-map ordering, not bash.)

`/tmp/<name>` mirrors are **derived**, never typed: macOS canonicalizes `/tmp`
to `/private/tmp` before matching, so every rule is emitted twice with the same
decision. A hand-written `/private/tmp` entry in a source file is an error.

## Changing a fence

Edit `base.json` (shared) or `roles/<role>.json` (one role). Never edit a
rendered artifact, a twin, or an agent file under `.opencode/`.

- `base.json.namespaces[]`: `root`, `decision`, optional `external_variants` /
  `edit_variants` (suffixes such as `/**` and `/**/*`).
- role delta keys are confined: an absolute `edit_allow`/`edit_deny`/
  `external_allow` entry must sit under a declared namespace root (a
  hand-typed `/private/tmp` entry is an error, and `/etc/...` cannot be
  introduced by a delta at all). `{repo}` entries are exempt (expanded per
  launch, validated by exact-set matching instead).
- role delta: `edit_default`, `edit_allow`, `edit_deny`, `external_allow`,
  `launch` (`cwd`, `prompt`, optional `agent`, optional `auto`),
  `agent` (`name`, `twin`, `description`, `mode`, optional `model`,
  `temperature`, `bash`, `alias`), `config_extra` (only the consultant uses
  it, for `mcp`). `launch.agent` must equal the role's own `agent.name`
  and requires `agent.mode` `primary`|`all`.
- `{repo}` is allowed in any role delta and expands to the checkout being
  launched (consultant: whole repo r+w except `src/`).

Dev keeps bash as the documented maintenance path and has no edit-tool access to
this directory (`tools/agents/fence/**`, `.opencode/agents/**` are denied). The
cross-edit gate therefore does NOT forbid fence edits by a dev change set: the
sanctioned way to change a fence is a deliberate, reviewable edit, and the gate
would otherwise block the maintenance path the owner kept open.

## Role-scoped namespaces

A base namespace may carry `"roles": [...]`; it is then composed into those
roles only (no `roles` key = every role). This is how the auditor's `/tmp/*`
scratch exists without granting a blanket `/tmp` to the workers:
`compose()` skips non-matching entries and `verify_fence` F5 applies the same
filter, so a `/tmp/*` rule appearing in a worker's map is still a failure.

## The auditor's shell is the boundary, not its edit map

The auditor keeps `edit_default: deny` (it writes only `audits/rejected_patch.diff`
and `audits/report-*.md`) and needs no MCP. It launches headless with
`--agent nexios-audit --auto`: `ask` rules auto-approve, explicit `deny` rules
still hold. The bash map is reads plus a deny tail (destructive, mutating-VCS,
network-exfil, wrapper and session-driving verbs) instead of an allowlist with
a bare `*`: under `--auto` an allowlist-with-`*` would auto-approve everything,
so the tail is the control. Residual holes are documented, not closed:
`cp`/`mv`/glob-writes reach the repo through bash (same class as the historic
`edit`-vs-shell gap); `python3 <script>` runs arbitrary files; `git apply`
reproduces patches (intended). The controls that remain are the ones that
actually apply:

- `--auto` never overrides `deny`; a denial ends that command (recorded as a
  limitation, never retried or wrapped).
- `audits/rejected_patch.diff` is the only sanctioned output; the developer
  applies it, never the auditor (`PROMPT-audit.md`).
- `tools/check_no_cross_edits.sh` reviews a change set, so an auditor that
  modified a source file is visible in `git diff` and in the audit report the
  developer reads before applying anything.
- `tmux ls*` is allowed for room inspection; no session-attach rights.

## Verification

```
scripts/agent_up.sh render-self-test    # 50 checks: polarity, mirrors, determinism,
                                        # {repo}, twins, and 17 fail-closed probes
scripts/agent_up.sh fence show <role>   # the composed map, without launching
python3 tools/agents/verify_fence.py    # rendered-output properties (F1-F8)
python3 tools/agents/verify_fence.py --self-test
```

The gate checks properties rather than a golden file (a golden would be one more
copy): per-role polarity (plus the auditor session-bash pin), mirror
completeness, no host-dependent path, no wildcard leak, consultant `mcp`,
self-contained agent cages, consultant src-deny ordering (F9), and freshness of
any config already on disk under `$NEXIOS_HOME/rendered/`.
