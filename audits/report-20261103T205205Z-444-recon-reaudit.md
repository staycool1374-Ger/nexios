# AUDIT REPORT 2026-11-03T205205Z — ISSUE #444 RECON PLAN (RE-AUDIT)
TIP: 404e190b8 (audited tip only; worktree dirty files + other branches out of scope)
PATCH: none (RECON plan audit — no code, no pending_patch.diff at tip)
FILES: src/libc/stdio.c, src/libc/stdio.h, packaging/meson.build, src/kernel/test/test_libc_stdio.cpp

## RETRIEVAL (mandatory)
- graphify BFS 'vsnprintf snprintf printf stdio NULL size handling' depth 3 -> 28 nodes; used stdio.c/vsnprintf/snprintf/printf/print_hex/print_dec heads to confirm emit path, ignored debugd/userspace-nullderef communities. Disposition: used in-tree libc heads; ignored unrelated communities.
- vault 'snprintf null vsnprintf stdio' mode content -> 0 matches. No prior spec; plan-from-tree only.

## TIP VERIFICATION (via `git show 404e190b8:<path>`, read-only)
- Root cause CONFIRMED at tip: src/libc/stdio.c vsnprintf:44-114. `char* end = buf + n - 1` with buf=NULL,n=0 wraps to (char*)-1, then `if (n==0) end=buf` -> end=NULL; loop guard `p<end` false so no emit; then `*p='\0'` (line 112) writes through NULL -> deterministic SIGSEGV. Matches issue repro exactly.
- Secondary defect CONFIRMED: returns `p-buf` (bytes stored), so snprintf(NULL,0,"hello") would return 0, not 5. C requires would-be length.
- Correct libc CONFIRMED: userspace ELFs link in-tree libc_a (packaging/meson.build: `libc_a` from src/libc + `input:[crt0, src, libc_a]`), NOT picolibc. Picolibc sysroot+glue+verify is a separate x86_64-only vehicle (test_libc_stdio.cpp snprintf-bounds covers picolibc r=20/len=15). Dev targets the right file; impl must not touch the picolibc tarball.
- No amended plan since NEED 20:40:44Z: dev inbox last plan is 20:26:57Z (repost 20:32:02Z, no content change). Re-audit is same-evidence; findings stand.
- Arch parity: vsnprintf is arch-independent C; 3-arch link green is the correct parity gate.

## FINDINGS
- [S2] src/libc/stdio.c:132 (printf caller contract) — vsnprintf returning would-be breaks printf's write length
  WHY: printf does `vsnprintf(buf,256)` then `write(buf, r)`; once r becomes would-be (>255 on long formats) the write over-reads the 256B stack buffer (infoleak/potential fault). Plan omits printf. AMEND: clamp write length to stored bytes (min(r, sizeof(buf)-1) / bounded count) alongside the vsnprintf fix; document truncation contract (stdio.h already notes 255-byte truncation).
- [S3] src/libc/stdio.c:44 (loop condition) — count-only mode needs `while(*fmt)`, not `while(*fmt && p<end)`
  WHY: With end==buf==NULL, `p<end` is false and the counter never advances (would return 0, not 5). Plan intent ("count only, zero stores") implies the change but never states it — make it explicit so impl audit passes on first attempt. Precedent: fmt::mini_vsnprintf Sink (need/at separation, cap==0 untouched).
- [S3] src/libc/stdio.h width contract — plan's "%-width parity" tests must assert current contract, not add width support
  WHY: Header documents width/`0` parsed-but-not-applied, %X-lowercase, no %z, ll==l, LP64 widths. Tests asserting applied-width output would fail against the frozen contract; scope is NULL/0 + would-be count only.
- [S3] src/libc/stdio.c:45-47 (NULL arithmetic) — avoid `buf+n-1` and `p-buf` on NULL
  WHY: Both are UB when buf==NULL (even when n==0). Use a separate `total` counter for the return and gate all stores on `n>0 && buf!=NULL`; keep sprintf 0x7FFFFFFF path behaviorally unchanged.

## PLAN DISPOSITION
- Strengths: bounded-store/would-be-counter separation + emit helper is the right shape; NUL-only-when-`n>0 && buf` and count-only-on-NULL/0 invariants correct; keeps `(null)` + no-%n scope; bounds (n==0: zero stores incl. NUL; n>0: <=n-1 chars + NUL) and pending_patch-diff audit protocol correct.
- Blocker: S2 printf clamp must enter the plan before branch cut. No branch/code/tests until amended plan is re-audited.

DECISION: NEED
