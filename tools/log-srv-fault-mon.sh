#!/bin/bash
# log-srv fault demo (issue #51, strangler step c): kill the log-srv PD,
# watch the supervisor restart + rebind it, re-probe via `logprobe`.
# Usage: tools/log-srv-fault-mon.sh <on+fault-inject-iso> <fat32.img> <outdir>
# Artifacts: outdir/{com1.log,debugcon.log,verdict.txt} (raw serial logs).
set -u
ISO="${1:?usage: log-srv-fault-mon.sh <iso> <fat32> <outdir>}"
FAT="${2:?usage: log-srv-fault-mon.sh <iso> <fat32> <outdir>}"
OUT="${3:?usage: log-srv-fault-mon.sh <iso> <fat32> <outdir>}"
mkdir -p "$OUT"
exec tools/srv-pd-fault-mon.py --iso "$ISO" --probe-cmd logprobe \
    --tag "S LOG " --role log-srv --kill-cmd logfault --outdir "$OUT" \
    --fat32 "$FAT" --port 4492
