# Room 420 RECON — test colocation (#420)

Lane: dev (re-seated after terminal respawn). Worktree: /Users/arnold/jarvis-herdr-dev-wt1 (detached HEAD 7b8b59a67, clean).
Transport note: /tmp/nherdr-420/prompts/dev-task.md + posts.jsonl NOT readable from this sandbox (external_directory deny; only nexios/jarvis/agentup/tmp allowlisted). RECON below is rebuilt from issue #420 + in-tree evidence. No kernel code touched (hard gate holds).

## Step 1 — inventory (src/kernel/test/)

- `src/kernel/test/` contains 255 entries (dir listing this session).
- `test_*.cpp` glob truncated at 100 hits; full count via dir = ~240 test TUs + infra (test_registry.cpp, test_isolate.{hpp,cpp}, test_cleanup.{hpp,cpp}, test_config.{hpp,cpp}, resource_tracker.{hpp,cpp}, task_ptr.hpp, watchdogs, ring3_vehicle.hpp, sched_helpers.hpp, selftest.hpp, expected_counts.hpp).
- Subsystem prefixes observed: sched_*, task_*, memory/mempool/pmm/vmm/pml4/pt_merge/page_tables, ipc*/pipe/spsc/queue, cap_* (core/lifecycle/untyped/mmio/mmio_user/irq/irq_notify/msix/iommu/ipc/pager/shm/syscall), vfs*/fat32/ramdisk/initrd/tmpfs/procfs/devfs/fstab, elf*/process/waitpid/secure_exec, drivers (ahci/virtio/blk_req/dma/pci/iocd), hal/arch (apic/lapic/ioapic/gdt/idt/invpcid/pcid/hal_bits/serial/keyboard/rtc/hr_clock), sync/locking/spinlock/mutex_pcp/pcp/pip/queue_pip, timing/deadline/budget/sporadic/ss_deadline/wcet_*, fpu_* (5 TUs excluded from Meson scan: test_fpu/clone/multi/sse/xmm_all), net, shell_*, lib/libc_*, debug/dmesg/klog/profiler/path_tracer, smp_*, stress_hrt/24h/jitter/cpu_load, buildsystem/config/infra/testrunner/core_isolation.
- FPU exclusion is load-bearing (src/meson.build:30-35 + generic_srcs:71-76 mirror mk/rules.mk:26-29 + test_source_set.py FPU_EXCLUDED).

## Step 2 — constraints / runners (no moves yet)

- Meson discovery: `test_src_dir = src_root/kernel/test`; `find *.cpp` minus 5 FPU TUs → `test_src_list` → registry codegen (`gen_test_registry.py --arch`), file-list stamp, `test_registry.gen.hpp` build-order edge.
- External-test mode: `external_test_dir != ''` compiles only `selftest_infra_rel` (7) + `selftest_tests_rel` (19) listed literally in src/meson.build:86-114. Any move MUST update these 26 literals or selftest breaks.
- Class selection: `test-config.txt` via `gen_test_config.py --test_class`; registry scans COMPILED set only.
- Expected counts: `src/kernel/test/test_expected_counts.hpp` + `validate_all_consistency` — per-class tables must stay in sync after any relocation (count-preserving move, names unchanged).
- Runners/CI: `tools/meson/qemu_test.py --class`, `scripts/run_all_classes.sh`, Meson `test()` entries, per-arch selection, `.github/workflows/build-meson.yml` selftest gate, `test-history.txt` rows, coverage (`tools/coverage_report.py` universe = ELF functions, path-sensitive area stats).
- Testbed workflow: new/changed tests land on `testbed` only; `main` = production. #420 hard gate adds: plan → plan-audit APPROVED before any branch/move. Sequencing vs #170: land only on merged Meson cutover, do not rebase it.
- Test-API: `JARVIS_REGISTER_TEST` / `JARVIS_REGISTER_RELEASE_TEST`, TF_* flags, registration macros must work from new locations (include paths via `kernel_includes`; registry generator takes `test_src_dir` as argv — parametrize or per-subdir roots).
- Vault/graphify: vault test logs + graphify refs to `src/kernel/test/*` paths go stale on move (re-index `graphify update .` post-move).

## Step 3 — plan sketch (for plan-audit, NOT implementation)

- P0: freeze inventory (this file) + per-subsystem destination map `src/<subsys>/tests/test_*.cpp` (infra stays: registry/isolate/cleanup/config/tracker/task_ptr/expected_counts).
- P1: build-system cutover (mechanical): parametrize `test_src_dir` discovery over N dirs, keep FPU exclusions identical, update 26 selftest literals, keep generated header edge; prove with `test-file-list` diff = path-prefix only.
- P2: move in small batches per subsystem (no renames, no content edits except include fixes); after each batch: `test-file-list` diff check + `validate_all_consistency` + one representative `qemu_test.py --class` + `make build` Errors 0.
- P3: CI/coverage/vault sweep (workflow refs, coverage areas, vault links, graphify re-index), testbed-first merge per PROMPT-dev, then full `run_all_classes.sh` + audit.
- Non-goals: no test renames, no API changes, no #170 rebase.
- Gaps (transport-blocked): original dev-task.md steps 1-3 text + prior RECON posts.jsonl not verified; request operator copy them into worktree or allowlist /tmp/nherdr-*.

Evidence: worktree clean except this new file; `git status --porcelain` shows `?? docs/engine-herdr/room-420-recon.md`; no src/ touched. Next: await IMPL order ring; will NOT move code until plan-audit APPROVED per #420 hard gate.
