#!/bin/bash
# Block-srv parity oracle (issue #47, step-b1).
# Compares the host FNV-1a digest over the first 16 FAT32 sectors with
# the `S BLK digest=` line printed by block_srv_read_image_digest.
#
# Usage: tools/block-srv-parity.sh <arch> <builddir>
#   <arch>     x86_64 | aarch64 | riscv64
#   <builddir> Meson builddir configured with
#              -Dconfig_defs=-DCONFIG_BLOCK_SRV=1
#              (e.g. build/meson-x86_64-b1)
set -u

ARCH="${1:?usage: block-srv-parity.sh <arch> <builddir>}"
BUILDDIR="${2:?usage: block-srv-parity.sh <arch> <builddir>}"
IMG="$BUILDDIR/packaging/fat32.img"
OUTDIR="build/block-srv-parity/$(date -u +%Y%m%dT%H%M%SZ)-$ARCH"
mkdir -p "$OUTDIR"

EXPECTED=$(python3 - "$IMG" <<'EOF'
import sys
path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read(16 * 512)
if len(data) < 16 * 512:
    sys.exit("short image: %d bytes" % len(data))
h = 14695981039346656037
for b in data:
    h ^= b
    h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
print("0x%x" % h)
EOF
)
echo "$EXPECTED" > "$OUTDIR/expected.txt"
echo "expected: $EXPECTED"

CAPTURE="$OUTDIR/qemu.log"
python3 tools/meson/qemu_test.py --arch "$ARCH" --build debug \
    --builddir "$BUILDDIR" --class block_srv_live > "$CAPTURE" 2>&1
RC=$?
tail -n 30 "$CAPTURE"

GOT=$(grep -o 'S BLK digest=0x[0-9a-fA-F]*' "$CAPTURE" | tail -n 1 | cut -d= -f2)
GOT_LOWER=$(echo "$GOT" | tr 'A-F' 'a-f')
EXP_LOWER=$(echo "$EXPECTED" | tr 'A-F' 'a-f')
echo "$GOT" > "$OUTDIR/captured.txt"
if [ -z "$GOT" ]; then
    echo "VERDICT: MISSING (no S BLK digest line; qemu rc=$RC)" | tee "$OUTDIR/verdict.txt"
    exit 1
fi
if [ "$GOT_LOWER" = "$EXP_LOWER" ]; then
    echo "VERDICT: MATCH ($GOT)" | tee "$OUTDIR/verdict.txt"
fi
# Issue #47 step-b2: report (never gate on) the PD-published IRQ stats.
IRQ_LINE=$(grep -o 'S BLK irq=[0-9]*/[0-9]*/[0-9]*/[0-9]*' "$CAPTURE" | tail -n 1)
if [ -n "$IRQ_LINE" ]; then
    echo "IRQSTATS: $IRQ_LINE" | tee -a "$OUTDIR/verdict.txt"
else
    echo "IRQSTATS: absent (poll-only row or pre-b2 PD)" | tee -a "$OUTDIR/verdict.txt"
fi
if [ "$GOT_LOWER" = "$EXP_LOWER" ]; then
    exit 0
fi
echo "VERDICT: MISMATCH expected=$EXPECTED got=$GOT" | tee "$OUTDIR/verdict.txt"
exit 1
