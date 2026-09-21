# AUDIT REPORT 20260918-155728
PATCH: audits/pending_patch.diff
FILES: src/lib/compiler_rt.cpp

## FINDINGS
- [S3] src/lib/compiler_rt.cpp:1 — stale generated build reference remains at mk/cpp-rules.gen.mk:9 (plus harmless Makefile:1270 filter entry)
  WHY: SRC_CXX_GENERIC is find-generated (mk/rules.mk:15) so the deletion auto-drops from the build, but the checked-in gen.mk rule goes stale until regenerated.
- [S3] src/lib/compiler_rt.cpp:1 — deleted __clzdi2/__ctzdi2 safety net while __builtin_clzll/__builtin_ctzll callers remain (src/kernel/arch/hal/bits.hpp:39,77; src/kernel/ipc/ipc.cpp:193; src/kernel/task/scheduler.cpp:597,690; src/kernel/kernel.cpp:1765)
  WHY: x86_64 lowers these builtins inline so zero explicit src refs plus the asserted nm zero-undefined-refs imply link-safety today, but a future flags/arch change emitting libcalls would fail closed at link time with no provider.
- [S3] audits/pending_patch.diff:1 — retrieval/link evidence only partially reproducible from patch + targeted greps
  WHY: src-scoped grep corroborates the asserted graphify no-callers-beyond-definitions disposition, but the vault zero-matches and nm-over-all-objects claims live in issue #182 and are accepted as asserted since object inspection is out of scope and no vault read was performed.

DECISION: APPROVED