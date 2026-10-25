# AUDIT REPORT 2026-10-25T12-29-02Z
PATCH: audits/pending_patch.diff
FILES: .github/workflows/build-meson.yml, prompts/PROMPT-audit.md, scripts/agent_up.sh, tools/agents/coordinator/AGENTS.md, tools/agents/coordinator/opencode.json, tools/agents/roles/README.md, tools/agents/roles/auditor.json, tools/agents/roles/coordinator.json, tools/agents/roles/dev.json, tools/agents/roles/qe.json, tools/agents/twins/nexios-audit.md, tools/agents/twins/nexios-dev.md, tools/agents/twins/nexios-qe.md, tools/agents/verify_fence.py, tools/check_no_cross_edits.sh, tools/gdb/debug-crash.gdb

## FINDINGS
- [S2] scripts/agent_up.sh:706 — coordinator reuses a live session without re-verifying the fence (S4-9 residual, fail-open)
  WHY: ensure_coord_session() runs check_fenced only inside the creation branch; a pre-patch session (launched with no OPENCODE_CONFIG at all) or a moved repo is silently reused, while workers refuse reuse outright.
- [S3] tools/agents/verify_fence.py:165 — empty frontmatter blocks are silently skipped by F2/F4 truthiness guards
  WHY: {} is falsy but not None (None already early-returns), so a gutted twin/prompt with an empty permission block passes clean instead of failing PARSE.
- [S3] scripts/agent_up.sh:1152 — self-test leaves /tmp/nexios-9999 plus .nexios-runtime/9999 behind
  WHY: the final rm covers the zcan/9991 scratch but not 9999, which the state-writer and hardlink cases create and mutate in live /tmp.
- [S3] tools/check_no_cross_edits.sh:73 — `-h|--help` prints lines 1,32p but the header now ends at line 45
  WHY: the patch grew the contract header, so help truncates mid-contract.
- [S3] scripts/agent_up.sh:539 — state-writer mkstemp sits outside try, raw traceback on missing runtime dir
  WHY: S4-2 promises one-line errors from this writer, but a mkstemp failure still multi-line-traces (fail-closed exit, wrong shape).
- [S3] scripts/agent_up.sh:942 — audit-diff still omits untracked files from the audited artifact (note, no hunk)
  WHY: `git diff` never lists untracked paths, so a brand-new file is invisible to the auditor until committed; the gate (S4-8) covers it, the artifact does not — document "commit new files first".
- [S3] scripts/agent_up.sh:591 — mode_audit and mode_status read state.json with bare json.load, raw traceback on corrupt state (note, no hunk; pre-existing lines)
  WHY: same one-line-error discipline as S4-2 should apply to the state readers.
- [S3] .github/workflows/build-meson.yml:12 — mark cross-edit-isolation REQUIRED in branch protection (note, repo setting not YAML)
  WHY: the job only blocks merges once marked required; until then it reports like the other jobs.

## PATCH
audits/rejected_patch.diff was written and applies clean (git apply --check OK; bash -n and py_compile green on scratch-applied copies): verify-or-rebuild stale coordinator sessions, PARSE-fail empty fence blocks plus is-not-None guards, mkstemp inside try, self-test 9999 cleanup, help range 1,45p.

Verified and accepted as correct (spot scope): S3-1R airtight (--copy constrained to direct /tmp/<name>, shell -ef plus python dev/ino re-check, os.replace never truncates a shared inode); S3-2R render on setup/coord/up/ensure paths, {repo}-less template rejected, widening confined to prompts//audits/ per F6; S3-3R 13 copies byte-identical via F1+F2+F5; S3-4R job runs on push+PR with fetch-depth 0, explicit base, producer-branch role; S4-1/2/3/4/5/6/7/8/10/11 hold as claimed; debug-crash.gdb target line matches gdb_run.py gdb_script_for_port rewrite regex; check 7 satisfied (graphify query plus disposition, vault 0-match, precedent report exists); cross-arch parity N/A (zero src/** files, host tooling plus config plus CI only).

DECISION: REJECTED
