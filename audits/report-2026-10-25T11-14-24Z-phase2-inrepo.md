# AUDIT REPORT 2026-10-25T11-14-24Z — Phase 2 in-repo agent workflow (#410)

AUDIT SCOPE: `audits/pending_patch.diff`, base `a2b8ea8bc…worktree`, 16 files:
`.github/workflows/build-meson.yml`, `.gitignore`, `prompts/PROMPT-audit.md`,
`scripts/agent_up.sh`, `tools/agents/coordinator/{AGENTS.md,opencode.json}`,
`tools/agents/roles/{README.md,dev,qe,auditor,coordinator}.json`,
`tools/agents/twins/nexios-{dev,qe,audit}.md`,
`tools/check_no_cross_edits.sh`, `tools/meson/gdb_run.py`.

AUDITOR: read-only. No code, commit, merge, or push was performed. Only this report
and `audits/rejected_patch.diff` were written.

## 0. Scope reality check (what was actually compared)

- Recomputed `git diff a2b8ea8bc -- . ':!graphify-out' ':!audits/pending_patch.diff'
  ':!audits/rejected_patch.diff' ':!test-history.txt' ':!prompts/LEARNINGS.md'` is byte-identical
  to the supplied `audits/pending_patch.diff` (1250 lines; `audits/rejected_patch.diff` is this
  audit's own artifact and was excluded). The *full* `git diff a2b8ea8bc` has 21 paths:
  the same 16 plus `prompts/LEARNINGS.md`, `test-history.txt`, `graphify-out/graph.json`
  and `audits/pending_patch.diff`; those four are OUT of scope and were not ruled on.
- Zero `src/**` changes: `git diff a2b8ea8bc --name-only -- src` → 0 files.
  Kernel surface untouched; cross-arch parity (x86_64/aarch64/riscv64) is N/A.
- No staged delta (`git diff --cached` empty); worktree is dirty by design.

## 1. Mandatory retrieval (check 7)

| Query | Result | Disposition |
|---|---|---|
| `graphify query "agent_up.sh role permission fence external_directory /tmp scratch and gdb test harness relationships"` | Kernel/VFS/scheduler nodes + `AGENTS.md`, `configuration.md`; no tooling nodes | USED to confirm the delta has no `src/**` graph impact; IGNORED for fence analysis — the graph does not model `scripts/`/`tools/agents/` (see next row). |
| `graphify path "agent_up.sh" "check_no_cross_edits.sh"` | Both collapse to node `userspace_sh` | IGNORED (stale/non-representative tooling mapping). Per the staleness doctrine, files are the current text for this delta. |
| `graphify query "agent configuration, permission fences, coordinator, scripts and CI process controls (non-kernel)"` | `AGENTS.md`, `configuration.md`, kernel nodes; no workflow/CI node | USED only to confirm `AGENTS.md` is the sole process source of truth; no graph contract for the fence. |
| `obsidian_search_vault "agent_up role fence external_directory permission"` / `"gdb_run gdb port test harness"` | 0 matches | No vault spec or prior tooling audit exists for this subsystem — no hidden contract to apply. |
| `obsidian_search_vault "audits/done"` (filename mode) | Only kernel audit reports (`memory_audit`, `ipc_audit`, `task+scheduler_audit`, …) | IGNORED as non-applicable: priors cover kernel concurrency/memory, not agent fencing. Consequence: the in-repo `tools/agents/roles/README.md` is the *only* fence spec, and it is part of this delta — the audit has to rely on file evidence, not on a vault contract. |

## 2. Verification commands and results

| Command | Result |
|---|---|
| `bash -n scripts/agent_up.sh` | rc 0 |
| `bash -n tools/check_no_cross_edits.sh` | rc 0 |
| `bash tools/check_no_cross_edits.sh --self-test` | 13/13 PASS, `self-test: clean`, rc 0 |
| `bash scripts/agent_up.sh self-test` | **15/15 PASS**, rc 0 (grew from 3) |
| `bash scripts/agent_up.sh comms-test` | `PASSED: doorbell delivered in both directions`, rc 0 |
| `OPENCODE_CONFIG=…/roles/{dev,qe,auditor,coordinator}.json opencode debug config` | rc 0 each; emitted `permission` block equals the file byte-for-byte, **insertion order preserved** (deny rule first, allows after) |
| `opencode debug config` in `$COORD_DIR` with `OPENCODE_CONFIG` unset (the `coord`/`up` launch shape) | **`"permission": null`** — see S2-1 |
| `python3 -m py_compile tools/meson/gdb_run.py` | OK |
| `ruby -ryaml -e 'YAML.load_file(…build-meson.yml)'` | `YAML OK` (parser is available via Ruby on this host; step indentation valid) |
| `git diff a2b8ea8bc --name-only -- src` | 0 files |
| `grep` of embedded heredocs vs `tools/agents/roles/*.json` | all four roles: `embedded == repo` **True** |
| twins vs JSON permission blocks (YAML→dict normalize) | dev/qe/audit twins match their JSON (modulo YAML syntax) |
| 12 concurrent `agent_up.sh state 9999 …` races (two writers/iteration) | final `pending=race12 retries=12` — both keys survived; no lost update observed |
| `/tmp/<name>-copy → /tmp/nexios-9999` symlink + `status --reconstruct` | live `state.json` mtime **changed** (1792927139 → 1792927201) — see S3-1 |
| `git merge-base main main` + `git diff main..main` | 0 lines; the worktree carries the 16-file delta uncommitted — see S3-2 |
| `ln -sfn` repoint (file and dir symlinks) on macOS | rc 0; link replaced, target not followed |
| `target` scan of `tools/gdb/*.gdb` | 62 of 69 scripts have `target … :1234`; 7 do not (`catch-nu`, `debug-crash`, `trace_map_page_gpf`, `track-corruption`, `track-nu`, `vuln004`, `watch-bstate`) |
| `--gdb-port` fail-closed path | only reachable when `port != 1234` (`gdb_script_for_port` returns early on 1234), so default-port behavior is unchanged — no regression for `:1234` |
| rejected_patch.diff `git apply --check` on pristine copies of all 6 targets | rc 0; patched checker self-test 13/13; patched `--base=does-not-exist` → rc 2; patched `--copy` symlink → rc 1 |

Note on `/tmp` fence coverage (focus a): every test/build consumer is covered —
`/tmp/jarvis-meson-*`, `/tmp/jarvis-dump-counts-*`, `/tmp/jarvis_classruns-*` by
`/tmp/jarvis-*`/`/**`; `/tmp/jarvis_config_matrix_*` by `/tmp/jarvis_*`;
`/tmp/matrix-action*` by `/tmp/matrix-*`; `/tmp/qemu-serial.in|.out` by
`/tmp/qemu-serial.*`; `/tmp/agentup-*` by `/tmp/agentup-*`. `-serial pipe:/tmp/qemu-serial`
is a pipe *base*, not a file, so the missing bare name is harmless. No rule makes
`"*": "deny"` unreachable (deny is first, allows after, last-match-wins), and no rule
widens past `/tmp` — verified against the rendered config. See S3-7 for the one consumer
that is no longer covered.

## 3. Findings

### [S2] tools/agents/coordinator/opencode.json:1-22 — the config the coordinator actually loads has NO permission block
`agent_up.sh`'s canonical coordinator launch (`coord_tui_cmd` :488, `ensure_coord_session`
:493-503, `mode_coord` :505-520, `mode_up` hoist :556) runs `env -u OPENCODE_CONFIG … opencode` with
cwd `$COORD_DIR`. Opencode therefore auto-loads `$COORD_DIR/opencode.json` (the symlink to
this file). Running `opencode debug config` in that cwd returns `"permission": null`:
`tools/agents/roles/coordinator.json` (deny-all writes except `/tmp/nexios-*` +
`prompts/LEARNINGS.md`) is **dead config** for the shipped launch path.
WHY: the coordinator is the most privileged role (drives transitions, alone may close out
LEARNINGS) and it is the one role whose fence is not installed. `tools/agents/roles/README.md:11-25` and `tools/agents/coordinator/AGENTS.md:1-13` assert a
cage that is not in force. This
contradicts the delta's central claim and must be fixed before the fence can be called
fail-closed.

### [S2] .github/workflows/build-meson.yml:187,211-221 + tools/check_no_cross_edits.sh:129 — the CI role gate is vacuous
`actions/checkout@v4` defaults to depth 1 / one ref. On `pull_request` the checkout is a
detached merge ref with **no local `main`**, so `git merge-base main HEAD` fails and the
script falls back to `BASE=HEAD`; on push to `main`, `main == HEAD` for the same reason.
Either way `git diff HEAD` is empty → `check_no_cross_edits: clean` unconditionally.
Independently, the workflow only triggers on `main` (`.github/workflows/build-meson.yml:12-16`),
so the `testbed → role=qe` branch at :213-221 is unreachable — the QE side of the fence is
never checked by CI at all. WHY: the script header (:38-40) and README call this the
fail-closed backstop; in automation it is a green no-op. Since `bash: allow`, this gate is
the intended enforcement for the two-role split, and it is not enforcing anything.
(Verified locally: `git merge-base main main` → `git diff main..main` = 0 lines while
the 16-file delta is uncommitted in the worktree.)

### [S3] scripts/agent_up.sh:675-704 — `--copy` confinement is string-only; a symlink bypasses both the `/tmp` anchor and "live state is untouched"
The guard rejects nested paths and bad suffixes (`:684-689`) but never resolves the path.
`ln -s /tmp/nexios-9999 /tmp/live-copy` passes every check, and the reconstruct writer
(`open(copy + "/state.json", "w")`, `:708`) follows the symlink and **rewrites the live
state.json in place** — measured mtime change on the live file. A symlink to any writable
directory also lets an agent create `state.json` outside the mailbox. WHY: the advertised
invariant "live state.json is never touched" (usage text :88) is false; the guard
must reject symlinks and compare `realpath`/inode against the live runtime.

### [S3] scripts/agent_up.sh:582-590 — `audit-diff` cannot produce the delta it is documented to produce
`git diff "$base".."$br"` is empty when `br == main` (`merge-base main main == main`), so the
documented "run in the dev checkout" flow fails with "audit diff empty"; on a feature branch
it silently omits uncommitted edits (this very delta is 1090 insertions / 256 deletions across
16 files, all uncommitted on top of `bef671ba0`).
WHY: audit integrity depends on the diff covering the whole change set; the owner had to
hand-build `audits/pending_patch.diff`, which is exactly the traceability gap this tool was
supposed to close. (`audits/pending_patch.diff` is indeed hand-built — see §4.)

### [S3] scripts/agent_up.sh:381 vs :439 — `audit_branch` is read but not writable through the sanctioned writer
`mode_audit` reads `state["audit_branch"]` (`:439`), but `ALLOWED` (`:381`) omits it, so
`agent_up.sh state` can never set it. Either the audit branch assertion is permanently
skipped (empty string → `check_branch` no-op), or someone hand-edits `state.json` and breaks
the "only sanctioned write path" invariant documented in `PROTOCOL.md`. WHY: the
single-writer contract is incomplete for a field the workflow depends on.

### [S3] prompts/PROMPT-audit.md:6-13 — second, divergent auditor cage; "auditor cage byte-identical" is only partly true
`PROMPT-audit.md` (target of `.opencode/agents/sil3_auditor.md`) now has the narrowed `edit`
rules but **no `external_directory` block at all** (so it cannot grant the named namespaces
and falls back to the default/host policy), `bash: "*": ask` + allowlist (vs
`allow` in the JSON/twin), and `audits/report-*.md` (vs `audits/report-*` in
`tools/agents/roles/auditor.json:16-22` and `tools/agents/twins/nexios-audit.md`). The JSON
and twin are equivalent (verified), so the adoption report's embedded-vs-live auditor-cage S3 is closed for those representations — but the other
auditor launch path is not equivalent and has no narrowed `/tmp` fence at all. Corroboration:
the rules injected into *this* audit session (captured from a tool denial) are the legacy
`external_directory: /tmp/*` allow, not the named namespaces.
WHY: which cage an auditor gets depends on how it is started; the delta's parity claim does
not hold across all four representations.

### [S3] tools/agents/roles/dev.json:16-24 (and twins/nexios-dev.md) — dev can still widen the *other* cages; README claim is false
Dev's edit map allows `"*"` and denies only `tools/agents/roles/**` and the two test trees.
It can therefore rewrite `tools/agents/twins/nexios-audit.md` (the auditor subagent cage),
`tools/agents/coordinator/opencode.json`, `prompts/PROMPT-audit.md` and `scripts/agent_up.sh`
(the launcher's `check_fenced`/`ensure_link` logic). `tools/agents/roles/README.md:24-25` states "Every role denies editing its own cage … so a session cannot widen its own fence",
which is false for dev. WHY: a compromised/over-eager dev session can weaken the audit cage
by editing a file it is allowed to touch; only human/git review catches it. Either deny those
paths for dev or state the residual risk honestly (patch does the latter).

### [S3] scripts/agent_up.sh:599,601-605 — `qe-merge` swallows fetch failure and overstates recovery
`git fetch origin main 2>/dev/null || true` proceeds to merge a possibly stale `origin/main`
and reports success; the conflict path always prints "code state restored" even when
`git merge --abort` fails (no `MERGE_HEAD` for pre-merge failures). WHY: a stale or
unfetched merge is reported as a clean QE catch-up; the operator gets a false success signal
on the exact step that gates the testbed.

### [S3] fence narrowing drops an existing `/tmp` consumer: `/tmp/lldb-h2-foreign.txt`
`tools/gdb/h2_replay_driver.py:85`, `h2_replay_driver2.py:77`, `h2_replay_lldb.py:56` write
`/tmp/lldb-h2-foreign.txt`. The old `"/tmp/*": allow` covered it; the narrowed namespaces do
not (`/tmp/lldb-*` is absent). The scripts themselves still run (bash is unfenced by design),
but an agent using its `read` tool on that artifact now hits deny — a fail-closed change in
behavior for a real consumer. WHY: either add `/tmp/lldb-*` (or move the file under
`/tmp/jarvis-*`) or document the exclusion; silently losing a path that was reachable is the
regression the fence review is supposed to catch.

### [S4] scripts/agent_up.sh:357-362 — `check_clean_tree` porcelain parsing
`git status --porcelain | awk '{print $2}'` mishandles renames (`R  old -> new` → only the old
path is seen; `->` becomes a token) and paths with spaces, and it counts non-ignored untracked
entries as dirt (the dev checkout here has 842). Exemptions are prefix-matched per token, so
the two configured exemptions still work, but it is a fail-open edge in a fail-closed guard.

### [S4] report glob is broader than documented
`tools/agents/roles/auditor.json:21`, the twin and the embedded fallback allow
`audits/report-*`, i.e. any file name/extension in `audits/`, while `PROMPT-audit.md:12` and
the usage docs say `audits/report-*.md`. Use `audits/report-*.md` everywhere.

### [S4] scripts/agent_up.sh:395-414 — state writer durability/error polish
`os.replace` is atomic, but the tmp file leaks if the replace fails, the directory is not
fsynced (crash can lose the rename), and `int(value)` / `json.load` failures surface as raw
tracebacks instead of the `fail`-style message. No lost-update or stale-inode path: the lock
lives on a never-replaced sidecar and is held across read→replace (verified, §2).

### [S4] fence does not cover the host temp dir actually used by `gdb_run.py`
`gdb_run.py` uses `tempfile.mkdtemp(prefix="jarvis-meson-gdb-")` and `agent_up.sh` uses
`mktemp`; on macOS both land under `/var/folders/…`, which no role allows. This is
pre-existing (the old `/tmp/*` did not cover it either) and bash-launched tools still work,
but the README's scratch inventory implies `/tmp` only. Document or allow `$TMPDIR`.

### [S4] scripts/agent_up.sh:100-104,716-745 — minor test/validation gaps
`runtime_dir` accepts non-canonical numerics (`000410` → a different directory than `410`)
and unbounded length; the 15-case self-test does not exercise the `ensure_link` *repoint*
branch (verified manually: `ln -sfn` replaces the link, does not follow) or `check_fenced`
(needs live opencode). `tools/gdb/debug-crash.gdb` is tracked and has no `target … :1234`
line, so a port-busy run of that script now hard-fails instead of printing the manual-connect
note — intended fail-closed, but a behavior change worth recording.

## 4. Process deviations (explicit, not softened)

1. **Audit bypassed for items 1-2.** `c1e523a6f` was merged to `main` as `bef671ba0` on the
   owner's "push all" order **without a prior audit**, violating the mandatory Phase-3 gate in
   `AGENTS.md`. This report is therefore partly retroactive: the in-repo role configs, twins,
   coordinator config and the `/tmp` narrowing were already on `main` when reviewed.
   `43ebb1713` (script adoption only) *was* audited APPROVED
   (`audits/report-20261025T090951Z-phase2-adoption.md`); the rest of the script delta and the
   gdb/CI/checker hunks are prospective.
2. **Issue #410 body contradicts the shipped doctrine.** It still says role configs are
   "stored out-of-repo" and its acceptance criterion says "No orchestrator state, inbox, or
   session config committed to the repo". Owner order superseded that with the in-repo
   doctrine, but the issue was not updated. This is an open doc/acceptance gap (not a code
   defect): update the body/AC or record the supersession on the issue before `check-close`.
3. **The audit input was hand-built.** `audits/pending_patch.diff` was not produced by the
   sanctioned `agent_up.sh audit-diff` (which would have emitted an empty diff on `main` —
   S3-2). The 16-file diff was verified to match `git diff a2b8ea8bc`, but the tooling gap is
   real and is covered by the rejected patch.
4. **Scope exclusion.** `prompts/LEARNINGS.md`, `test-history.txt`, `graphify-out/` are dirty
   in the same worktree and were excluded from the 16-file scope; they were not audited.
5. **Self-refuting fence evidence.** The session running this audit had the legacy
   `/tmp/*` external-directory allowance, not the narrowed set (the `PROMPT-audit.md`
   divergence above). Only
   `OPENCODE_CONFIG=roles/*.json` was proven to render the narrowed fence; the running
   auditor path was not.
6. **Prior S3 disposition.** Of the 6 S3s in `report-20261025T090951Z-phase2-adoption.md`:
   **4 fixed** (`with_state_lock` removed and replaced by the locked single-writer;
   qe-merge checkout/branch asserts; coord-watch ordering via the `ensure_coord_session`
   hoist; numeric `runtime_dir`), **2 partial** (`--copy` anchor tightened but still
   symlink-bypassable, S3-1; auditor cage: JSON/twin/embedded now equal but
   `PROMPT-audit.md` still diverges). No regression in the fixed items, but the
   `--copy` bypass and the ineffective CI wiring (S2-2) mean #411's tooling S3s are not yet
   closed end-to-end.

## 5. Rejected patch

`audits/rejected_patch.diff` is `git apply`-able against this worktree (verified `git apply
--check` on pristine copies, and the patched checker self-test is 13/13 with `--base`
fail-closed). It fixes the two S2s (coordinator permission block; non-vacuous CI role gate
with explicit `--base`, full-history checkout, `testbed` triggers), the `--copy` symlink
bypass, `audit-diff` worktree coverage, the `audit_branch` key, qe-merge fetch fail-closed
and the README claim. It deliberately leaves the `PROMPT-audit.md` divergence and the
`/tmp/lldb-h2-foreign.txt` narrowing gap unfixed (a re-audit should still flag both), and it
does not touch the dev cage (dev must be able to edit tooling) or the kernel.

## 6. Verdict rationale

The delta is mechanically sound (syntax, self-tests, config parse, secret hygiene, zero
kernel surface) and closes most prior S3s. It is rejected because two fail-open safety-process
controls are defeated: the coordinator runs with no permission block at all, and the CI
cross-edit backstop reports green without classifying anything. Both are S2 under SIL 3
fail-closed doctrine and both have cheap, verifiable fixes (supplied).

DECISION: REJECTED
