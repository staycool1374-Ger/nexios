#!/bin/bash
# term-srv fault demo (issue #51, strangler step c): kill the term-srv PD,
# watch the supervisor restart + rebind it, re-run the parity probe.
# Usage: tools/term-srv-fault-mon.sh <on+fault-inject-iso> <fat32.img> <outdir>
# Artifacts: outdir/{com1.log,debugcon.log,verdict.txt} (raw serial logs).
set -u
ISO="${1:?usage: term-srv-fault-mon.sh <iso> <fat32> <outdir>}"
FAT="${2:?usage: term-srv-fault-mon.sh <iso> <fat32> <outdir>}"
OUT="${3:?usage: term-srv-fault-mon.sh <iso> <fat32> <outdir>}"
mkdir -p "$OUT"
exec tools/srv-pd-fault-mon.py --iso "$ISO" --probe /bin/term-srv-parity.c.elf \
    --tag "S TM " --role term-srv --kill-cmd termfault --outdir "$OUT" \
    --fat32 "$FAT" --port 4491
