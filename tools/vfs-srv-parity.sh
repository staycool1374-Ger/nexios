#!/bin/bash
# vfs-srv parity oracle (issue #47, step-b3).
# Compares host FNV-1a digests over served file bytes with the
# `S VFS <name> digest=` lines printed by the vfs_srv_live class:
#   initrd  /hey.c.elf   — extracted from the linked initrd.cpio
#   fat32   /HELLO.TXT   — parsed out of the FAT32 test image
#   fat32m  /MULTI.TXT   — multi-cluster chain walk, same image
#   ramdisk vfs-srv-probe — deterministic test pattern (600 B)
# procfs has no static oracle (dynamic content); presence of its
# digest line plus the in-harness equality check is the gate there.
#
# Usage: tools/vfs-srv-parity.sh <arch> <builddir>
#   <arch>     x86_64 | aarch64 | riscv64
#   <builddir> Meson builddir configured with
#              -Dconfig_defs=-DCONFIG_VFS_SRV=1
#              (e.g. build/meson-x86_64-v1)
set -u

ARCH="${1:?usage: vfs-srv-parity.sh <arch> <builddir>}"
BUILDDIR="${2:?usage: vfs-srv-parity.sh <arch> <builddir>}"
INITRD="$BUILDDIR/packaging/initrd.cpio"
FATIMG="$BUILDDIR/packaging/fat32.img"
OUTDIR="build/vfs-srv-parity/$(date -u +%Y%m%dT%H%M%SZ)-$ARCH"
mkdir -p "$OUTDIR"

python3 - "$INITRD" "$FATIMG" "$OUTDIR" <<'EOF'
import sys, struct

initrd_path, fat_path, outdir = sys.argv[1], sys.argv[2], sys.argv[3]

def fnv1a(data):
    h = 14695981039346656037
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

# --- initrd: walk newc headers, find ./hey.c.elf ---
img = open(initrd_path, "rb").read()
off = 0
initrd_digest = None
initrd_size = 0
while True:
    assert img[off:off+6] == b"070701", "bad cpio magic at %d" % off
    f = [int(img[off+6+i*8:off+14+i*8], 16) for i in range(13)]
    mode, filesize, namesize = f[1], f[6], f[11]
    name_off = off + 110
    name = img[name_off:name_off + namesize].split(b"\0")[0].decode()
    data_off = (name_off + namesize + 3) & ~3
    if name in ("hey.c.elf", "./hey.c.elf", "/hey.c.elf"):
        initrd_digest = fnv1a(img[data_off:data_off + filesize])
        initrd_size = filesize
    if name == "TRAILER!!!":
        break
    off = (data_off + filesize + 3) & ~3
assert initrd_digest is not None, "hey.c.elf not found in initrd"

# --- FAT32: BPB at LBA 2048, root-dir walk + chain follow ---
d = open(fat_path, "rb").read()
base = 2048 * 512
bps = struct.unpack("<H", d[base+11:base+13])[0]
spc = d[base+13]
rsvd = struct.unpack("<H", d[base+14:base+16])[0]
fats = d[base+16]
fatsz = struct.unpack("<I", d[base+36:base+40])[0]
rootclus = struct.unpack("<I", d[base+44:base+48])[0]
assert (d[base+510], d[base+511]) == (0x55, 0xAA), "no BPB signature"
data_start = base // 512 + rsvd + fats * fatsz

def read_cluster(clus):
    lba = data_start + (clus - 2) * spc
    return d[lba*512:(lba+spc)*512]

def fat_next(clus):
    foff = clus * 4
    sec = foff // 512
    o = base + (rsvd + sec) * 512 + foff % 512
    return struct.unpack("<I", d[o:o+4])[0] & 0x0FFFFFFF

def read_chain(start, size):
    out = b""
    clus, trips = start, 0
    while len(out) < size:
        trips += 1
        assert trips <= 64, "chain too long"
        assert clus < 0x0FFFFFF8, "unexpected EOF"
        out += read_cluster(clus)
        nxt = fat_next(clus)
        if len(out) >= size:
            break
        assert nxt < 0x0FFFFFF8, "premature chain end"
        clus = nxt
    return out[:size]

def find_root(name83):
    for i in range(0, spc * 512, 32):
        e = read_cluster(rootclus)[i:i+32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5 or e[11] & 0x08:
            continue
        nm = e[0:11].decode("ascii")
        if nm == name83:
            lo = struct.unpack("<H", e[26:28])[0]
            hi = struct.unpack("<H", e[20:22])[0]
            sz = struct.unpack("<I", e[28:32])[0]
            return (hi << 16) | lo, sz
    raise AssertionError("file not found: " + name83)

results = {}
clus, sz = find_root("HELLO   TXT")
results["fat32"] = fnv1a(read_chain(clus, sz))
clus, sz = find_root("MULTI   TXT")
results["fat32m"] = fnv1a(read_chain(clus, sz))

# --- ramdisk probe: deterministic 600 B pattern (test writes it) ---
pat = bytes(((i * 7 + 3) & 0xFF) for i in range(600))
results["ramdisk"] = fnv1a(pat)

results["initrd"] = initrd_digest
results["initrd_size"] = initrd_size
for k in ("initrd", "fat32", "fat32m", "ramdisk"):
    print("%s 0x%x" % (k, results[k]))
with open(outdir + "/expected.txt", "w") as f:
    for k in ("initrd", "fat32", "fat32m", "ramdisk"):
        f.write("%s 0x%x\n" % (k, results[k]))
EOF
echo "--- expected digests above ---"

CAPTURE="$OUTDIR/qemu.log"
python3 tools/meson/qemu_test.py --arch "$ARCH" --build debug \
    --builddir "$BUILDDIR" --class vfs_srv_live > "$CAPTURE" 2>&1
RC=$?
tail -n 12 "$CAPTURE"

FAIL=0
for NAME in initrd fat32 fat32m ramdisk; do
    GOT=$(grep -o "S VFS $NAME digest=0x[0-9a-fA-F]*" "$CAPTURE" | tail -n 1 | cut -d= -f2)
    EXP=$(grep "^$NAME " "$OUTDIR/expected.txt" | awk '{print $2}')
    if [ -z "$GOT" ]; then
        echo "VERDICT: MISSING (no S VFS $NAME digest line; qemu rc=$RC)"
        FAIL=1
    elif [ "$(echo "$GOT" | tr 'A-F' 'a-f')" = "$(echo "$EXP" | tr 'A-F' 'a-f')" ]; then
        echo "MATCH $NAME $GOT"
    else
        echo "MISMATCH $NAME guest=$GOT host=$EXP"
        FAIL=1
    fi
done
# procfs digest is dynamic content: presence + in-harness equality is the gate.
if grep -q "S VFS procfs digest=0x" "$CAPTURE"; then
    echo "PRESENT procfs $(grep -o 'S VFS procfs digest=0x[0-9a-fA-F]*' "$CAPTURE" | tail -n 1 | cut -d= -f2)"
else
    echo "VERDICT: MISSING (no S VFS procfs digest line; qemu rc=$RC)"
    FAIL=1
fi
if [ "$FAIL" = 0 ]; then
    echo "VERDICT: MATCH (all oracles agree)"
else
    echo "VERDICT: MISMATCH (see above)"
    exit 1
fi
