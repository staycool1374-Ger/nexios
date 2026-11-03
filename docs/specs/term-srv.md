# term-srv + log-srv — Policy PD Contracts (issue #51, strangler step a)

Userspace terminal-emulation and log-sink PDs behind `CONFIG_TERM_SRV` /
`CONFIG_LOG_SRV` (default 0). Step-a delivers PD skeletons + SDF rows +
Kconfig OFF shim + loopback/parity + fault demo. No flip: emulation
policy and log sinking stay in-kernel.

## Wire

48 B `TermRpc` / `LogRpc` (mirrors `TimerRpc`; fits
`CONFIG_IPC_FAST_PAYLOAD_BYTES`). Ops: BIND (+ policy ops unimplemented
pre-flip), INJECT_POISON / INJECT_DROP (one-shot fault drills).
Control via `send_sync` (+FAST ≤48B). Bounded wait
(`send_sync_bounded`, `kRpcGuardTicks`); epoch + owner id/gen +
op-echo validated replies; tombstone drain for dead owners.

## SDF rows (empty by structural rule)

TERM_SRV and LOG_SRV rows carry zero caps (`sdf_policy_only` enforces
emptiness; `sdf_compose` rejects any non-empty policy row). Borrowed
mechanics only: serial TX/RX + framebuffer present for term-srv, Klog/
Dmesg *read* for log-srv probes. No MMIO, no IRQ, no DMA, no frames —
device ownership stays with #48 (fb) and the kernel (serial/console).
`sdf_rebind` re-derives nothing (empty row) but bumps the epoch so a
restarted PD can never consume pre-restart replies.

## Bounded ops

Scroll/clear/redraw/status-bar ops carry row budgets; UC MMIO writes
stay out of the PD (no device access at all). Drain paths are bounded
(16-entry tombstone ring, drop-oldest with counter). No spinlock across
`reschedule`; no allocation on any path; OOM-as-error everywhere.

## Pre-flip status

Emulation (Terminal/draw/scroll/prompt) and log sinking stay in-kernel.
The PDs answer BIND and typed `-ENOSYS` for everything else (verified
byte-identical OFF vs ON: the kernel never calls them). The flip
(policy migration, Klog/Dmesg consumer handoff) is a later step with
the §7 gate (audit + test-full debug AND release + delta-0).

## Gates

ON build boots both PDs (daemon PIDs live); parity
`tools/term-srv-parity.sh` OFF == ON S TM sets; fault demo
`tools/term-srv-fault-mon.sh` kill → restart → rebind → re-run with
no admission wedge (serial artifacts under the tool's outdir).
