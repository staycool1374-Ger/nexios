# LEARNINGS — Knowledge Base (FEEDBACK transition artifact)

> Appended after every completed workflow cycle (state machine: FEEDBACK).
> Each entry records what the cycle taught, so background knowledge is
> adapted into the working context instead of being lost between sessions.
> The gate (`scripts/gate.py check-feedback <issue>`) verifies that a closed
> issue has a matching entry here — the FEEDBACK transition is not complete
> without it.

## Entry format

```
### #<issue> — <short title> (<date>)
- **Learned:** what new insight/technique/pitfall emerged this cycle
- **Adapted:** where this knowledge now lives (spec, AGENTS.md, code comment)
- **Style re-surface:** which CODING_STYLE.md rules were relevant and must stay in focus
```

## Entries

<!-- Append new entries below; newest first. -->

### #99 — Multi-arch compile-clean: aarch64 + riscv64 link green (2026-08-25)
- **Learned:** (1) Cross-arch builds with a shared `build/` require the arch-switch clean at PARSE TIME, before `-include $(DEPFILES)` — a mid-build clean (rule prerequisite) is too late because make commits to "object up to date" from the previous arch's `.d` files and never rebuilds it (caused `cannot find build/initrd/initrd.o`). (2) The old `gen_test_registry.py` had a legacy truncation bug: its `#else` depth arithmetic left `disabled_depth` elevated, silently dropping the tail of files (test_stack_alloc.cpp lost 5 real tests from the dormant `generated_tests[]`). (3) AArch64 `ADR_PREL_PG_HI21` can't reach low-VMA symbols from higher-half `.text`; higher-half boot-stack symbols must be linker absolute symbols (`HHDM + low_vma`). (4) riscv64 inline asm uses `mv` (no `mov`); aarch64 uses `mov %0, sp`.
- **Adapted:** Makefile parse-time arch-stamp check + per-arch `mk/cpp-rules.$(ARCH).gen.mk`; `tools/gen_test_registry.py` arch-aware stripping (SCAN/DROP/ARCH mode stack); `linker/linker_aarch64.ld` `_stack_start`/`_stack_end` absolute symbols; scheduler.cpp/task.cpp `CONFIG_ARCH_X86_64` guards.
- **Style re-surface:** CODING_STYLE preprocessor-guard placement (whole contiguous statement groups, x86_64 path byte-identical); minimal-diff discipline; Werror-clean.

### # — gate test entry (2026-08-23)
- **Learned:** gh --jq returns raw text, not JSON
- **Adapted:** scripts/gate.py
- **Style re-surface:** none
