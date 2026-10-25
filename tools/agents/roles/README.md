# Role configs (fencing for the multi-agent workflow, issue #410)

Each file is an opencode permission profile, loaded via `OPENCODE_CONFIG=<repo>/tools/agents/roles/<role>.json`
(absolute path at *runtime* only — the files themselves contain no machine-specific paths; the only
absolute prefix is `/tmp`, and only inside the NAMED scratch namespaces listed below).

Out-of-repo copies are forbidden: `~/.local/state/nexios/roles/*` and `.../coordinator/*` are
SYMLINKS into this repo (canonical content is versioned here). `scripts/agent_up.sh` creates the
links and hard-fails if it finds a real file in their place.

## The fence (same shape for all roles)
- `external_directory: deny` except the NAMED scratch namespaces below — a session touches only its
  own checkout plus its own scratch, never the rest of `/tmp` (no other user's temp files, no
  unrelated sockets/tokens):
  - `/tmp/nexios-*` (+ `/**`) — the issue mailbox runtime created by `scripts/agent_up.sh setup`.
  - `/tmp/jarvis-*`, `/tmp/jarvis_*`, `/tmp/matrix-*`, `/tmp/qemu-serial.*` — kernel test/build
    scratch (`tools/meson/*`, `scripts/run_all_classes.sh`, `tools/deadline_matrix.sh`).
  - `/tmp/agentup-*` (+ `/**`) — `scripts/agent_up.sh` self-test scratch AND the
    per-session `TMPDIR` (`/tmp/agentup-tmp-<session>`). The host `$TMPDIR`
    (`/var/folders/...` on macOS) is deliberately NOT allowed: it is per-boot and
    therefore unfenceable. It is pinned instead of banned-and-ignored, because an
    unfenced TMPDIR is what sent writers to `bash` (unfenced by design) during the
    #410 burn-in.
  - `/tmp/lldb-*` — lldb replay/watchpoint scratch written by `tools/gdb/*.py`
    (`h2_*` scripts). Flat namespace, same shape as `qemu-serial.*`.
  Own-checkout files need no rule (in-cwd); cross-checkout reads are denied (each checkout carries its
  own copy of prompts/specs, so branch-consistent roles fall out naturally).
- `bash: allow` — intentional and explicit (opencode 1.x defaults bash to ask; without this the fence
  would be a click-through farm). The fence was never the shell: cross-edit protection is the
  fail-closed `tools/check_no_cross_edits.sh` gate, not bash denial.
- `edit` is where each role differs (see files). Every role denies editing its own cage
  (`tools/agents/roles/**`). Dev still may edit the twin/coordinator definitions,
  `prompts/PROMPT-*.md` and `scripts/agent_up.sh`, so "cannot widen its own fence" holds
  only for the role JSONs — those other edits are covered by audit + git review, not by
  the dev cage.

**macOS path canonicalization (burn-in 2026-10-25):** `/tmp` is a symlink to
`/private/tmp` and opencode canonicalizes a target before matching rules, so a
`/tmp/nexios-*` allow never matched and every mailbox/state write silently fell
back to `bash` (allowed, and path-gate-free). Every `/tmp/<ns>` rule is therefore
duplicated as `/private/tmp/<ns>` in the role JSONs, the twins, `prompts/PROMPT-audit.md`
and the four embedded fallbacks in `scripts/agent_up.sh` — keep the pairs in
lockstep, and prove fence changes with a tool-only write probe, not by reading the
map.

## Coordinator config (repo-anchored, rendered at launch)
The coordinator's cwd is out of repo, so a relative rule such as
`prompts/LEARNINGS.md` resolved to a SHADOW file there, and `external_directory:
"*": deny` blocked tool reads of the repo entirely (issue #412 S3-2R). The
canonical config therefore carries a literal `{repo}` token instead of a machine
path, and `scripts/agent_up.sh` renders it into
`$NEXIOS_HOME/coordinator/opencode.resolved.json` at launch (absolute,
canonical repo root, JSON-validated, refuses a residual token). The shipped
files contain no machine-specific paths; only the generated resolved file does.

Coordinator read surface: `{repo}/prompts/**` and `{repo}/audits/**` are the only
`external_directory` allowances beyond the shared scratch map; the only repo
write is `{repo}/prompts/LEARNINGS.md` (FEEDBACK closeout).

## Byte-identity families (mechanically enforced)
`tools/agents/verify_fence.py` (stdlib, run by `agent_up.sh self-test` and the
`cross-edit-isolation` CI job) checks: the four embedded fallbacks in
`scripts/agent_up.sh` are byte-identical to the role JSONs; the worker
`external_directory`/`edit` maps are identical across JSONs, twins and
`prompts/PROMPT-audit.md`; the coordinator map is exactly the worker map plus the
two `{repo}` read keys; every `/tmp` rule has its `/private/tmp` mirror with the
same decision; the `audits/report-*.md` glob stays narrowed; the required
`/tmp` namespaces are still present. A fence change that breaks a family fails
the suite — do not "keep them in sync" by eye.

## Twins
Each role has a twin agent definition, canonical in `tools/agents/twins/nexios-<role>.md` (subagent
mode, same permission block) for in-session `@`-mention use (e.g. dev spawning `@nexios-audit`).
`.opencode/agents/nexios-<role>.md` is a RELATIVE SYMLINK into that directory (`.opencode/` is
gitignored, so the versioned copy lives under `tools/agents/`). Keep the pair in sync when changing
either; the JSON is the tmux-launch twin, the markdown the subagent twin.

## Verification (S1, mandatory per fresh session before any task work)
1. `opencode debug config` must show the role's permission block (coordinator:
   `OPENCODE_CONFIG=$NEXIOS_HOME/coordinator/opencode.resolved.json` — the
   template is not what launches).
2. Attempt a blocked edit (must DENY) at depth 1 AND nested; attempt the allowed counterpart (must succeed, then clean up).
3. Stop on any failure. A fence that can't prove itself is not a fence.
