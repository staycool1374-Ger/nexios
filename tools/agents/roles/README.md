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
  - `/tmp/agentup-*` — `scripts/agent_up.sh` self-test scratch.
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

## Twins
Each role has a twin agent definition, canonical in `tools/agents/twins/nexios-<role>.md` (subagent
mode, same permission block) for in-session `@`-mention use (e.g. dev spawning `@nexios-audit`).
`.opencode/agents/nexios-<role>.md` is a RELATIVE SYMLINK into that directory (`.opencode/` is
gitignored, so the versioned copy lives under `tools/agents/`). Keep the pair in sync when changing
either; the JSON is the tmux-launch twin, the markdown the subagent twin.

## Verification (S1, mandatory per fresh session before any task work)
1. `opencode debug config` must show the role's permission block.
2. Attempt a blocked edit (must DENY) at depth 1 AND nested; attempt the allowed counterpart (must succeed, then clean up).
3. Stop on any failure. A fence that can't prove itself is not a fence.
