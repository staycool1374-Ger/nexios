# engine-herdr T0 — spawn-proof under herdr-core (hc 0.1.18)

Recorded 2026-11-07 by the consultant (issue #507 §6 T0). Verdict up front:

> **S1 fence core PASSES under herdr** — no fallback needed. The engineered
> spawn line from §4 works verbatim, four-value identity is verifiable at
> prompt time, and mismatch → kill works. One substrate reality has to shape
> T2–T7: **opencode is a *plain program* to herdr** — its lifecycle and its
> permission modal are not natively reported; blocked-detection gates on
> screen text via `hc wait --output` (proven true-positive + true-negative).

## 1. What was proven (matrix)

| # | Check | Result | Mechanism |
|---|-------|--------|-----------|
| C1 | fenced terminal comes up live | ✅ | `hc get --json` → `state: live` |
| C2 | child runs in `--cwd` | ✅ | marker file written by the child |
| C3 | `PWD` == `--cwd` in child env | ✅ | `ps eww` on the child program |
| C4 | `OPENCODE_CONFIG` == rendered fence | ✅ | `ps eww` |
| C5 | `NEXIOS_AGENT_ROLE` == role | ✅ | `ps eww` |
| C6 | `TMPDIR` == `/tmp/agentup-tmp-<t>` | ✅ | `ps eww` |
| C7 | identity seam passes correct spawn | ✅ | all four tokens |
| C8 | identity seam rejects mutated role | ✅ | seam returns 1 |
| C9 | kill on mismatch terminates | ✅ | `hc kill` → `state: finished, reason: killed` |
| C10 | transitions on `hc events` | ✅ | typed JSON-RPC lines |
| C11 | `hc wait --output` ends on screen text | ✅ | `reason: output` |
| C12 | wait control (no false positive) | ✅ | unmatching pattern times out |
| C13 | identity on real opencode process | ✅ | four tokens on opencode pid |
| C14 | permission modal true-positive | ✅ (manually recorded) | see §3 |
| C15 | in-cwd write true-negative | ✅ (manually recorded) | see §3 |

Scripted as `scripts/herdr_up.sh selftest` (C1–C12, no model needed) and
`scripts/herdr_up.sh selftest --full` (C13–C15, needs a live model; degrades
to SKIP with evidence when the free-tier quota is exhausted — it was during
the final run; C14/C15 outputs below were recorded live earlier the same day).

## 2. Recorded outputs (raw, abridged)

Spawn line exactly per #507 §4 (identity values on one line, flags after):

```
hc run --bg --name t0-fence --cwd /tmp/hc-t0/cwd \
  env TMPDIR=/tmp/agentup-tmp-t0-fence \
      OPENCODE_CONFIG=$NEXIOS_HOME/rendered/dev/opencode.resolved.json \
      NEXIOS_AGENT_ROLE=dev /Users/arnold/.opencode/bin/opencode
{"address":"t2","agent":"env","cwd":"/tmp/hc-t0/cwd","name":"t0-fence","state":"live","turn":0}
```

Child shell env dump (in-terminal `printf` of PWD/OPENCODE_CONFIG/
NEXIOS_AGENT_ROLE/TMPDIR):

```
PWD=/tmp/hc-t0/cwd
CFG=/Users/arnold/.local/state/nexios/rendered/dev/opencode.resolved.json
ROLE=dev
TMP=/tmp/agentup-tmp-t0-probe
```

Process-env identity on the opencode pid (seam = legacy `check_fenced`
tokens, read from the pane's foreground program):

```
PID 55249 -> PWD=/tmp/hc-t0/cwd  TMPDIR=/tmp/agentup-tmp-t0-fence
             OPENCODE_CONFIG=...rendered/dev/opencode.resolved.json
             NEXIOS_AGENT_ROLE=dev            (all four PASS)
```

Mismatch + kill (launch carries `NEXIOS_AGENT_ROLE=qe`, fence expects `dev`):

```
identity MISMATCH detected for role dev
{"name":"t0-wrong","state":"finished","exit_code":0,"reason":"killed",...}
```

Event stream (`hc events --json`, piped — one JSON line per change):

```
{"method":"event","params":{"type":"created","terminal":"t4",...,"state":"live",...}}
{"method":"event","params":{"type":"foreground","terminal":"t4",...}}
{"method":"event","params":{"type":"finished","terminal":"t4",...,"exit_code":1,"reason":"killed"}}
```

Wait shaping, sentinel printed to the screen after the wait started:

```
{"children":[],"ended":true,"exit_code":null,"reason":"output","terminal":"t8",...}
```

## 3. Blocked true-positive on a permission modal

Sim fence `{"permission":{"edit":"ask","bash":"ask","webfetch":"ask"}}`
(config outside the repo, same fence identity rules), model live, TUI task
"create a file /tmp/hc-t0/blocked-proof.txt …". The opencode TUI raised its
permission modal (screen captured at prompt time):

```
△ Permission required
  ← Access external directory /tmp/hc-t0
  Allow once   Allow always   Reject
```

Waiter started first, then the task — `hc wait --output 'Permission required'`:

```
{"children":[],"ended":true,"exit_code":null,"reason":"output","terminal":"t8","turn":0}
```

True-negative control (real dev fence, in-cwd write): under the rendered
`dev` fence the same task wrote `hello.txt` in cwd, screen showed **0**
matches of "Permission required", and `hc wait --output …` returned
`{"ended":false,"reason":"timeout"}`. (Caution documented below: the sim
fence asks on *every* write — an in-cwd write under `edit:ask` also opens
the same modal; the control must use an in-cwd-allowing fence, i.e. the
real rendered role fence, exactly as the engine will.)

## 4. Structural findings that shape T2–T7 (all embedded in the selftest)

1. **opencode is a plain program for herdr.** Its lifecycle is not agent-
   reported: `hc get` shows `state: live, status: null` while the permission
   modal is open, and bare `hc wait` times out (`"ended":false,"reason":
   "timeout"`) — herdr cannot see the modal natively. Block-decisions in the
   FSM must gate on **screen text via `hc wait --output <pattern>`** until a
   herdr-agent protocol integration exists (or an opencode status reporter).
   The sanctioned pattern is `hc wait <t> --output '<modal header>'`, which
   also ends on blocked/error/exit.
2. **Wait/sentinel ordering.** `--output` only sees text printed *after* the
   wait started: start the waiter, then send the task. Both C11 and C14
   encode this ordering (the first selftest draft raced and failed).
3. **macOS env visibility.** `ps eww` hides environ for Apple-signed
   programs (zsh/sh/sleep show nothing) and shows it for unsigned ones
   (opencode, homebrew python3). The identity seam must (and does) read the
   opencode/agent process env — same target the legacy `check_fenced`
   selects. Env-inheritance quick checks use the homebrew python3 stand-in.
4. **`hc send` to a booting terminal can be dropped** (hc manual): T2's
   spawn helper must wait for readiness (readiness word or short `wait`)
   before the first task, exactly as templates do.
5. **Free-tier model quota is an operational hazard** for --full checks:
   selftest degrades C14/C15 to SKIP with on-screen evidence (quota banner)
   instead of spurious failure; the fast lane stays deterministic (12/12).

## 5. S1 verdict per the T0 gate

> *"If S1 fails here: STOP writing code; record findings on this issue and
> fall back (fallback: tmux substrate + still do T3 schema/T4 slim FSM)."*

S1 does **not** fail. Env inheritance (§2), prompt-time identity with
refusal/mismatch semantics (C7/C8/C9), `hc events` and `wait` shaping
(C10–C12) all hold under `hc run --bg`. The only "shape" consequence is
that blocked-detection for opencode rides on screen-text shaping (§4.1) —
which still satisfies the no-prose-*parsing* goal: matching a stable modal
header string is not a prose regex over worker speech. §5.1's "only three
sources of truth" principle therefore gains a fourth, screen-shaped source
for opencode (or an hc-agent bridge); this nuance is recorded for the spec
elaboration at T1 (docs/engine-herdr/spec.md).

## 6. Reproduction

```
scripts/herdr_up.sh selftest          # 12 checks, seconds
scripts/herdr_up.sh selftest --full   # + 3 model checks (HC_SELFTEST_MODEL to override)
```

Names are unique per run (`t0s-$RANDOM`), leftover terminals are killed and
rm'd on exit, nothing on `main` changes, legacy `/tmp/nexios-*` untouched.
