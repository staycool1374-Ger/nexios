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

### # — gate test entry (2026-08-23)
- **Learned:** gh --jq returns raw text, not JSON
- **Adapted:** scripts/gate.py
- **Style re-surface:** none
