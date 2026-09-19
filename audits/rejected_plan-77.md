# REJECTED PLAN #77 — corrected steps (audit 2026-09-19T18:35:35Z)

The three v3 decisions (converge / retain-for-retry / argv-deferred-loud) are faithfully recorded and the v2 regression items hold (add_task_err both sites, ET_EXEC gate, tls assert, #75 gate, retention-semantics denial pin, dual-path pin gone). The REJECT is driven by one S2 plus four S3 gaps. Correct as follows, then re-submit:

1. Boot order (corrects S2): hoist `ElfLoader::ensure_task()` before the `/etc/rc` runner in `init_task_main` (currently :301, after rc :158-230) and state rc's sync-wait on completion explicitly — a background-loader rc cannot dispatch before its servicing task exists.
2. `reset()` reconcile (corrects S3): `reset()` (elf_loader.cpp:246) must move to `destroy_completed_tcb` like every other never-added path; "never cleanup()+delete on never-added" must name `reset()` explicitly.
3. Retain-free order (corrects S3): specify validate-new-path FIRST, then free retained via `destroy_completed_tcb` — never free-before-validate.
4. Path parity (corrects S3): state how `initrd::find`-visible images resolve through loader `vfs::resolve`/`syscall_path_open`, or add the mapping step; else shell/rc loads fail FILE_NOT_FOUND post-converge.
5. Coherence (corrects S3): renumber the duplicated step 5; fix SIL "step 6"→step 2 and test-strategy "step 3"→step 4; strip decide-later/AFFECTED-FILES language ("EITHER…OR…decide §3.1", "+ memory budget").

Full evidence: audits/report-2026-09-19T18:35:35Z.md (DECISION: REJECTED).
