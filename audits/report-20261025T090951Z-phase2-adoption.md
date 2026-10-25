# AUDIT REPORT 20261025T090951Z
PATCH: N/A (adoption-review; nothing to merge yet — merge awaits APPROVED)
FILES: scripts/agent_up.sh

## FINDINGS
- [S3] scripts/agent_up.sh:238 — with_state_lock defined, zero call sites, and the primitive is broken-if-used
  WHY: the fcntl LOCK_EX is taken in one short-lived python3 process that exits (releasing the lock) before "$@" runs, so even a future caller gets zero mutual exclusion; harmless today (no state writer exists — status is read-only, reconstruct writes the COPY only) but the A2(i) guarantee is presence-only until rewritten as a single process holding the fd across the child.
- [S3] scripts/agent_up.sh:471 — status --reconstruct --copy accept pattern is loose (/tmp/*drill*|*staging*|*copy* matches any /tmp path containing those words)
  WHY: confinement still holds (mandatory /tmp anchor plus the explicit live-dir refuse at :476, proven to abort per QE B1), but a tighter anchor would fail closed on typos instead of accepting them as copies (carried from prior report-20261025T084137Z-phase2-stepper.md S3#3).
- [S3] scripts/agent_up.sh:420 — mode_qe_merge asserts clean tree but never asserts checkout/branch (comment says "run in the QE checkout", no check_branch testbed)
  WHY: a wrong-checkout invocation would stage a --no-commit merge on the wrong branch; recoverable via --abort with zero data loss, but a one-line branch assertion would fail closed instead of relying on operator discipline.
- [S3] scripts/agent_up.sh:385 — mode_up opens the nexios-coord watch window before ensure_coord_session creates the session (creation happens later inside mode_coord)
  WHY: the Terminal mirror attaches to a nonexistent session and shows an error; cosmetic ordering only, zero safety impact (workers + coordinator themselves launch correctly).
- [S3] scripts/agent_up.sh:89 — runtime_dir echoes /tmp/nexios-$1 with no numeric validation of the issue argument (also single-quote-interpolated into python3 -c at :269)
  WHY: operator-supplied input only (coordinator passes a numeric issue; no remote or untrusted path reaches it), but a non-numeric issue escapes the runtime dir; validate numeric to fail closed.
- [S3] scripts/agent_up.sh:131 — embedded write_role_auditor cage is broader than the live v1 roles/auditor.json (edit allow on /tmp/* vs live /tmp/nexios-*/*; absolute /Users/arnold/jarvis/audits paths vs live relative; no bash/external_directory keys)
  WHY: bootstrap-only by construction (ensure_roles :160-166 fires only when files are absent — existing v1 files always win) and flagged in-script as ASSUMPTION (:130, :164); the auditor cage is coordinator best-effort and permissive by design — auditor is read-only by role, /tmp scratch writes are the intended outlet, and the fail-closed backstop for bypasses is tools/check_no_cross_edits.sh, not this file.

## VERIFIED (no finding)
- Commit 43ebb1713 stat lists scripts/agent_up.sh ONLY (1 file, +561 insertions) — zero kernel/toolchain surface; message paren-refs (#410) with no Fixes/Closes/Resolves keywords.
- Committed blob == worktree (both 561 lines, diff IDENTICAL); bash -n clean.
- Launcher: tmux-direct launch (:287), OPENCODE_CONFIG fencing with -u scrub (:289, :320, :336-338), ps-based check_fenced (:303-315, no harness path), branch guards (:217-221, enforced :259/:362-363); harness referenced only in comments explaining why it is NOT used (:27-33).
- state.json is parsed exclusively via python3 json (:269, :455, :481, :486-496); remaining greps touch only ps output (:305-310) and tmux pane captures (:520, :529) — prior grep-parses-state gap is closed.
- state_get helper is gone (prior S3#2 resolved by removal); with_state_lock unwired status carried above as S3.
- Doorbell: single-line enforced (:435), has-session precheck (:436), tmux send-keys both directions; PROTOCOL.md + header document pointer-only/idle-only/never-dialogs/no-poll, and the script contains no polling loop.
- Role-bootstrap fidelity: embedded dev/qe edit semantics match live files; auditor delta is bootstrap-only as analyzed above.
- Out-of-repo bin copy is absent (superseded/deleted per header :3-6 — consistent, reference-only); prior out-of-repo report 20261025T084137Z (3x S3 APPROVED) dispositions carried above.
- prompts/LEARNINGS.md worktree modification is coordinator closeout scope — explicitly out of scope, not flagged.

DECISION: APPROVED
