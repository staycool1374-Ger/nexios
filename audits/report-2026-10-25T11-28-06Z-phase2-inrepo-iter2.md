# AUDIT REPORT 2026-10-25T11-28-06Z — Phase 2 in-repo agent workflow (#410), iteration 2

AUDIT SCOPE: `audits/pending_patch.diff`, base `a2b8ea8bc…worktree`, 16 files:
`.github/workflows/build-meson.yml`, `.gitignore`, `prompts/PROMPT-audit.md`,
`scripts/agent_up.sh`, `tools/agents/coordinator/{AGENTS.md,opencode.json}`,
`tools/agents/roles/{README.md,dev,qe,auditor,coordinator}.json`,
`tools/agents/twins/nexios-{dev,qe,audit}.md`,
`tools/check_no_cross_edits.sh`, `tools/meson/gdb_run.py`.

AUDITOR: read-only. No code, commit, merge, or push was performed. Only this report
was written; `audits/rejected_patch.diff` is unchanged (this iteration is APPROVED).

## 0. Scope reality check (what was actually compared)

- The supplied `audits/pending_patch.diff` is **1375 lines**. Recomputed
  `git diff a2b8ea8bc -- . ':!graphify-out' ':!audits/pending_patch.diff'
  ':!audits/rejected_patch.diff' ':!test-history.txt' ':!prompts/LEARNINGS.md'`
  is byte-identical (`cmp`) and reports 16 files, 1178 insertions, 16 deletions.
- **Verbatim application.** `git apply -R --check audits/rejected_patch.diff` → rc 0,
  so the rejected patch's post-image is present exactly (drop-in, not reimplemented).
- **Pre-patch reconstruction.** Reverse-applying the rejected patch to copies of its
  six target files and rebuilding the 16-path diff yields **1250 lines**, exactly
  iteration 1's count (`report-2026-10-25T11-14-24Z-phase2-inrepo.md` §0). Only those
  six files changed after iteration 1 (mtime 12:27:23; every other path is 12:08:54;
  `tools/meson/gdb_run.py` is 12:13:14, before iteration 1's 12:14:24 report). The
  current delta is therefore the iteration-1 state plus the rejected patch, with no
  extra paths and no manual edits outside the patch.
- Zero `src/**` changes: `git diff a2b8ea8bc --name-only -- src` → 0 files. Kernel
  surface untouched; cross-arch parity (x86_64/aarch64/riscv64) is N/A.
- Full `git diff a2b8ea8bc` has 21 paths; `prompts/LEARNINGS.md`, `test-history.txt`,
  `graphify-out/` and `audits/pending_patch.diff` are OUT of scope.
  `audits/pending_patch.diff` and `audits/rejected_patch.diff` are tracked and
  modified in the worktree — relevant to S4-6 below.
- No staged delta (`git diff --cached` empty); the worktree is dirty by design.

## 1. Mandatory retrieval (check 7)

| Query | Result | Disposition |
|---|---|---|
| `graphify query "agent_up.sh role permission fence external_directory coordinator config check_no_cross_edits CI gate"` | 34 nodes; kernel/docs (`scripts/gate.py`, `docs/specs/configuration.md`, `prompts/PROMPT-dev.md`, `AGENTS.md`); no `scripts/agent_up.sh` or `tools/agents/*` nodes | USED to confirm the delta has no kernel graph impact; IGNORED for the fence — the graph does not model the agent tooling. Files are the current text for this delta (staleness doctrine). |
| `obsidian_search_vault(vault="nexios", …)` | `VAULT_NOT_FOUND` | The repository name is not a vault id; retried against the configured vault. |
| `obsidian_search_vault(vault="jarvis", "agent_up role fence external_directory permission coordinator")` | 0 matches | No vault spec or prior tooling audit exists for this subsystem. |
| `obsidian_search_vault(vault="jarvis", filename "audits/done")` | 5 kernel audits only (`ipc`, `memory`, `task+scheduler`, `boundary+syscall`, `semaphore`) | IGNORED as non-applicable: priors cover kernel concurrency/memory, not agent fencing. No hidden vault contract to apply. |

## 2. Verification commands and results

| Command | Result |
|---|---|
| Recompute + `cmp` vs supplied patch | 1375 lines, **byte-identical** |
| `git apply -R --check audits/rejected_patch.diff` | rc 0 (`REVERSE_APPLY_OK`) |
| Reverse-apply reconstruction of the pre-patch delta | **1250 lines**, matches iteration 1 |
| `git diff a2b8ea8bc --name-only -- src` | 0 files |
| `bash -n scripts/agent_up.sh`; `bash -n tools/check_no_cross_edits.sh` | rc 0 both |
| `bash tools/check_no_cross_edits.sh --self-test` | 13/13 PASS, rc 0 |
| `bash scripts/agent_up.sh self-test` | **15/15 PASS** |
| `bash scripts/agent_up.sh comms-test` | `PASSED: doorbell delivered in both directions`, rc 0 |
| `check_no_cross_edits.sh --role=dev --base=does-not-exist` | **rc 2**, `base 'does-not-exist' does not resolve` |
| `check_no_cross_edits.sh --role=dev --base=` | **rc 0**, falls back to merge-base (S4-7) |
| `check_no_cross_edits.sh --role=dev --base=a2b8ea8bc` | `clean`, rc 0 (no `src/**` in the delta) |
| `--copy` path is a symlink to the live dir | rc 1, `refusing: --copy must not be a symlink` |
| `--copy` dir contains a `state.json` symlink to live | rc 1, `refusing: copy state.json is a symlink` |
| Live `/tmp/nexios-410/state.json` mtime around the symlink tests | 1792919501 → **1792919501** (unchanged) |
| Scratch 9999 `state.json` hardlinked into a `--copy` dir | rc 0; live 9999 mtime **1792928300 → 1792928354** (S3-1R) |
| `opencode debug config` in a `/tmp` replica symlinked to the repo coordinator files | permission block present: `bash=allow`, `edit["prompts/LEARNINGS.md"]=allow`, `external_directory["*"]=deny` |
| Real `$COORD_DIR` contents | only `AGENTS.md` and `opencode.json`, both symlinks to `tools/agents/coordinator/*` (correct; direct `cd` was blocked by this session's `external_directory` rules, so the replica was used) |
| Embedded role heredocs vs `tools/agents/roles/*.json` | dev/qe/auditor/coordinator: `embedded == repo` **True** |
| `tools/agents/coordinator/opencode.json` vs `roles/coordinator.json` | permission blocks **identical** |
| Twins vs their JSON permission blocks (YAML→dict) | dev/qe/auditor: `twin == repo` **true** |
| `ruby -ryaml` on the workflow; `python3 -m py_compile tools/meson/gdb_run.py` | `YAML_OK`; `GDB_RUN_PYCOMPILE_OK` |
| `grep -rn "lldb-h2-foreign" tools/gdb/` | 3 consumers (`h2_replay_driver.py:85`, `h2_replay_driver2.py:77`, `h2_replay_lldb.py:56`) — S3-3R |
| `grep -c '^target' tools/gdb/debug-crash.gdb` | **0** — S4-5 |
| `git status --porcelain` on the 16 paths | 7 modified (the six patch targets + `gdb_run.py`); the other 9 clean |

## 3. Iteration-1 closure

Status legend: FIXED = the rejected patch resolves the finding as specified;
PARTIALLY FIXED = the main path is closed but a named residual remains;
RESOLVED-BY-DOCUMENTATION = the residual risk is now stated honestly;
STILL OPEN = untouched, carried as tracked follow-up.

| ID | Finding | Status | Evidence |
|---|---|---|---|
| S2-1 | Coordinator launch had no permission block | **FIXED** | `tools/agents/coordinator/opencode.json:3-22` now carries the block; `opencode debug config` in the symlinked replica renders it; `ensure_state_links` (`agent_up.sh:276-277`) keeps the symlinks; `ensure_coord_session` (`:494-503`) launches with cwd=`$COORD_DIR` and no `OPENCODE_CONFIG`. Residual: no launch-time assertion that opencode loaded it (S4-9). |
| S2-2 | CI role gate was vacuous | **FIXED** | `fetch-depth: 0` (`.github/workflows/build-meson.yml:190`); explicit `--base` (`:227-236`); `testbed` in both triggers (`:13-16`); `--base` unresolvable → rc 2 (verified). Residuals: the step lives in the advisory `parity-make-vs-meson` job (S3-4R) and PRs are classified via `GITHUB_REF_NAME` (S4-11). |
| S3-1 | `--copy` symlink bypassed "live untouched" | **PARTIALLY FIXED** | Symlinked dir → rc 1 (`agent_up.sh:700`); symlinked `state.json` → rc 1 (`:719-721`); live 410 mtime unchanged (verified). A **hardlink** still bypasses the `islink` check (`:700`, `:720`) and `open(…,"w")` (`:722`) truncates the shared inode — S3-1R. |
| S3-2 | `audit-diff` could not cover the change set | **PARTIALLY FIXED** | Same-branch path now diffs the worktree (`agent_up.sh:587-593`); verified: pre-patch 1250 → current 1375. But the tool writes into the file it diffs and applies no exclusions (`:586`, `:590`), and hard-codes `merge-base main` (`:585`) so it cannot reproduce an explicit base — S4-6. |
| S3-3 | `audit_branch` unreadable/unwritable | **FIXED** | `ALLOWED` includes `audit_branch` (`agent_up.sh:381`); `mode_audit` reads it (`:440`) and passes it to `mode_run` as `want_branch`. Residual: the key is still undocumented in `PROTOCOL.md` — S4-10. |
| S3-4 | `PROMPT-audit.md` divergent auditor cage | **FIXED** | `prompts/PROMPT-audit.md:7-16` now has the named `external_directory` block; the remaining differences are **stricter** (`bash: "*": ask` at `:24`; `audits/report-*.md` at `:22` vs the JSON's `audits/report-*`), so the unsafe divergence is closed. |
| S3-5 | Dev cage can widen the other cages; README false | **RESOLVED-BY-DOCUMENTATION** | `tools/agents/roles/README.md:24-29` now states the residual (dev may edit twins/coordinator/PROMPT/launcher; covered by audit + git review, not the dev cage). Dev's edit map is unchanged (`tools/agents/roles/dev.json:14-20`), so this is a tracked residual, not a fence fix. |
| S3-6 | `qe-merge` swallowed fetch failure / overstated recovery | **FIXED** | `agent_up.sh:606-607` fails closed on a failed `git fetch origin main`; the conflict message now says "merge aborted; verify with git status" (`:611-613`). |
| S3-7 | Fence drops `/tmp/lldb-h2-foreign.txt` | **STILL OPEN** | 3 consumers remain; no `/tmp/lldb-*` rule in any role config (S3-3R). |
| S4-1 | `audits/report-*` broader than documented | **STILL OPEN** | `tools/agents/roles/auditor.json:21`, twin `:21`; only `PROMPT-audit.md:22` is narrow. |
| S4-2 | State writer durability/error polish | **STILL OPEN** | `agent_up.sh:406-412`: temp file leaks if `os.replace` fails; no directory fsync; raw `int`/`json` tracebacks. |
| S4-3 | Fence does not cover `$TMPDIR` | **STILL OPEN** | `tempfile.mkdtemp`/`mktemp` dirs under `/var/folders/…` are outside every `external_directory` rule. |
| S4-4 | `check_clean_tree` porcelain parsing | **STILL OPEN** | `agent_up.sh:357-367` (`awk '{print $2}'`) mishandles renames/spaces and counts untracked entries. |
| S4-5 | `runtime_dir` canonicality; self-test gaps; `debug-crash.gdb` | **STILL OPEN** | `runtime_dir` (`:100-104`) accepts `000410` as a distinct dir; self-test lacks `ensure_link` repoint and `check_fenced`; `debug-crash.gdb` has 0 `target` lines, so a busy 1234 hard-fails by design (S4-5). |

Process items from iteration 1 §4: retroactive coverage of `c1e523a6f` is
**ACCEPTED** by owner order (not re-litigated); the issue #410 body/AC supersession
is **STILL OPEN** as a doc follow-up; the audit input was hand-built but has been
**re-verified** byte-for-byte; the scope exclusions are unchanged; the old
"self-refuting fence evidence" is resolved for this session (the running auditor
has the narrowed `/tmp/nexios-*` + `/tmp/jarvis-*` set, and `PROMPT-audit.md` now
carries the block).

## 4. Remaining and new findings

### [S3] S3-1R — `--copy` hardlink still reaches the live `state.json`
`scripts/agent_up.sh:700` rejects only `[ -L "$copy" ]`, and `:720` only
`os.path.islink(out_path)`. A hardlink is neither. Measured on a **scratch**
mailbox (`/tmp/nexios-9999`, not 410): `ln /tmp/nexios-9999/state.json
/tmp/nexios-9999-hl-drill/state.json`, then `status --reconstruct 9999 --copy
/tmp/nexios-9999-hl-drill` → rc 0, live mtime changed 1792928300 → 1792928354.
WHY: the advertised invariant "live state.json is never touched" (`:88`) is still
false for a deliberate hardlink, and iteration 1 explicitly asked for a
`realpath`/inode comparison. Fix: write `recon` to a temp file in `$copy` and
`os.replace` it (does not truncate the shared inode), and refuse when
`os.stat(out_path).st_ino/st_dev` equals the live file.

### [S3] S3-2R — coordinator rules are anchored to `$COORD_DIR`, not the repo
The coordinator is launched with cwd=`~/.local/state/nexios/coordinator`
(`agent_up.sh:494-503`), which contains only `AGENTS.md` and `opencode.json`
symlinks. `tools/agents/coordinator/opencode.json:20` allows
`edit["prompts/LEARNINGS.md"]`, which resolves to
`$COORD_DIR/prompts/LEARNINGS.md` (a shadow; the dir does not exist), **not**
`~/jarvis/prompts/LEARNINGS.md`. The repo file is outside the project and is
denied by `external_directory["*"]` (`:5-6`), so the coordinator cannot edit it
with the `edit` tool at all; the same deny also blocks `read`-tool access to the
repo role prompts that `tools/agents/coordinator/AGENTS.md` says to reference.
`bash: "allow"` (`:4`) is the only working path, and it can write the repo file
via a redirect, which is the documented "fence was never the shell" model.
WHY: the allow rule is mis-anchored and invites a non-canonical shadow copy,
contradicting the same AGENTS.md line "never write canonical content out of
repo". Fix: either remove the dead allow and document the bash path, or add the
repo `prompts/`/`audits/` paths to `external_directory` **and** use an absolute
`edit` allow — a deliberate widening that the owner should approve.

### [S3] S3-3R — the `/tmp/lldb-h2-foreign.txt` consumer is still outside the fence
`tools/gdb/h2_replay_driver.py:85`, `h2_replay_driver2.py:77`,
`h2_replay_lldb.py:56` write that path; no role config allows `/tmp/lldb-*`
(`tools/agents/roles/*.json:5-15`, coordinator `:5-15`, `PROMPT-audit.md:7-16`).
The scripts still run (bash unfenced), but an agent's `read` tool on the artifact
is denied. Iteration 1 deliberately left this open; it remains a tracked S3.

### [S3] S3-4R — the role gate is correct but still advisory until cutover
The corrected role check is in `parity-make-vs-meson`
(`.github/workflows/build-meson.yml:214-236`), a job whose header says the
non-required jobs are advisory until the cutover (`.github/workflows/build-meson.yml:3-6`;
issue #170 Phase 7). A failing gate therefore does not block a merge yet. This is
a known project state, not a regression: the computation is no longer vacuous
(verified `--base` rc 2 / clean), but the "fail-closed backstop" claim only holds
once this job (or the step) is required.

### [S4] S4-6 — `audit-diff` writes into the file it diffs and cannot take a base
`agent_up.sh:586-590` redirects `git diff … > audits/pending_patch.diff`; the
shell truncates that tracked file before `git diff` reads the worktree, so the
output contains a self-referential deletion hunk. Reproduced in a scratch git
repo with a tracked output file (the generated diff contained
`--- a/audits/pending_patch.diff` / `+++ b/audits/pending_patch.diff`). On this
worktree `audits/pending_patch.diff` and `audits/rejected_patch.diff` are tracked
and modified, so a real run would also include `prompts/LEARNINGS.md`,
`test-history.txt` and every dirty artifact. `base` is hard-coded to
`merge-base main` (`:585`), so the declared base `a2b8ea8bc` cannot be requested.
The audit input was therefore verified by the clean recompute in §0, not by the
tool. Fix: write to a temp file then `mv`, add `--base`, and exclude the audit
artifacts and out-of-scope paths.

### [S4] S4-7 — `--base=` (empty) silently falls back
`tools/check_no_cross_edits.sh:58` sets `BASE_OVERRIDE=""`, `:136` tests `-n`,
and `:144` falls back to `merge-base main HEAD`. A typo'd/empty `--base=` in CI
or by an operator yields `clean` (rc 0) instead of the fail-closed rc 2 used for
an unresolvable base. `--base` with no value is already rc 2; `--base=` should be
too.

### [S4] S4-8 — the gate exempts itself and drops untracked files
`tools/check_no_cross_edits.sh:167` unconditionally `continue`s past
`tools/check_no_cross_edits.sh`, so a dev (whose edit map allows that path,
`tools/agents/roles/dev.json:14-20`) can weaken the gate and CI's role check
will not flag the gate file itself; `:161-169` also intersects with `git ls-files`,
so a new still-untracked `src/**` file is invisible locally. Both are pre-existing
properties of the delta, not introduced by the patch; the diff review is the
backstop. Fix: drop the blanket exemption (or require the gate change to be
audited), and/or run the gate against a clean CI tree where all files are tracked.

### [S4] S4-9 — coordinator launch does not assert the loaded config
`ensure_coord_session` (`agent_up.sh:494-503`) only checks that
`$COORD_DIR/opencode.json` exists. It does not run the `check_fenced` equivalent
used for workers (`run_session` → `check_fenced`, `:448`, `:462`, `:474-491`),
so a global config or a future opencode auto-load change could silently leave the
coordinator unfenced. The replica test proves today's load works; the launch does
not enforce it.

### [S4] S4-10 — `audit_branch` is writable but undocumented
`ALLOWED` (`agent_up.sh:381`) and `mode_audit` (`:440`) now support it, but
`grep -rn audit_branch scripts/ tools/agents/` finds only those two lines — no
`PROTOCOL.md` or README entry. The single-writer contract is mechanically
complete; the operator-facing documentation is not.

### [S4] S4-11 — PR role is derived from `GITHUB_REF_NAME`
On `pull_request`, GitHub sets `GITHUB_REF_NAME=<n>/merge`, so the role at
`.github/workflows/build-meson.yml:218` is always `dev`. That is correct for a
dev-branch PR, but a PR whose head is a `testbed`/QE branch would also be
classified `dev` (fail-open for the QE side). Use `GITHUB_HEAD_REF` (or the base
branch) for the PR case.

## 5. Process notes

- The applied delta is the iteration-1 state plus the rejected patch: the
  reverse-apply check passes, the reconstructed pre-patch diff is exactly 1250
  lines, and the only files modified after iteration 1 are the six patch targets.
- The coordinator config load was verified in a `/tmp` replica because this
  session's `external_directory` rules blocked a direct `cd` into
  `~/.local/state/nexios/coordinator`. The replica uses symlinks to the same
  repo files; the real directory contains the same two symlinks.
- The audited diff was re-verified by the clean recompute; it was NOT produced by
  `agent_up.sh audit-diff` (S4-6). This is a tooling gap, not an audit-input
  integrity gap.
- Out of scope and untouched: `prompts/LEARNINGS.md`, `test-history.txt`,
  `graphify-out/`, and the two tracked `audits/*.diff` artifacts.
- Issue #410's body still describes the superseded out-of-repo doctrine; update
  it (or record the owner's supersession) before `check-close`. This is a
  documentation/acceptance follow-up, not a code defect.

## 6. Verdict rationale

The two iteration-1 **S2** blockers are closed: the coordinator now loads a
permission block on its actual launch path, and the CI role gate computes a real
change set from an explicit base with full history and both branches. All
config representations (JSON, twin, embedded heredoc, coordinator config) are
byte-equivalent; the only `PROMPT-audit.md` differences are stricter. The applied
patch is verbatim (reverse-apply and 1250-line reconstruction), syntax/self-tests/
comms/YAML/compile are green, and the kernel surface is untouched.

No **S1** or **S2** remains. The remaining **S3/S4** items are:
the `--copy` hardlink residual (S3-1R), the coordinator cwd-anchored rules
(S3-2R), the `/tmp/lldb-*` fence gap (S3-3R), the still-advisory CI job (S3-4R),
and the audit-diff/checker/state/fence/test/doc residuals (S4-1…S4-11). They do
not invalidate the delta's safety posture (no kernel change; the shell is
documented as unfenced; the audit + git review is the backstop), but they must be
tracked — recommend one follow-up issue (or an update to #411) covering
S3-1R…S3-4R and S4-1…S4-11, and the #410 body/AC update before `check-close`.

DECISION: APPROVED
