# AUDIT REPORT POST-exit-record
PATCH: (single-file change, src/kernel/task/exit_record.cpp)
FILES: src/kernel/task/exit_record.cpp
## FINDINGS
- [S3] src/kernel/task/exit_record.cpp:57 — memcpy copies raw source tail bytes after an embedded NUL where strncpy would NUL-pad, so r.name bytes can differ from the old sequence if t.name is ever not NUL-padded past its terminator.
  WHY: Every ring reader uses r.name solely as a NUL-terminated C-string (shell.cpp:2056 Terminal::write) or ignores it entirely (finders compare only id/valid/end_tick; tests assert id/exit_code/ticks, never name bytes), and r.name[15]==0 is unconditionally guaranteed by the memset, so any such tail divergence is unobservable and C-string behavior is identical for all three input classes (NUL-padded short, exact-length, unterminated), with no over-read (15 of 16 readable t.name bytes) and no over-write (16 of 16 writable r.name bytes), no truncation-class warning possible from fixed-size memset/memcpy, and no ENSURE/error-path or build-conditional change (S4/S5 parity intact).
DECISION: APPROVED