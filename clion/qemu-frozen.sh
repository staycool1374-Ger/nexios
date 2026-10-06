#!/bin/bash
# QEMU x86_64 frozen at boot for IDE/GDB work (issue #280 hunts, debugd E2E).
# Mirrors Makefile QEMU_FLAGS_INTERACTIVE for x86_64 plus `-s -S`:
#   -s  GDB stub on :1234
#   -S  freeze CPU at startup (attach debugger, then `continue`)
# Guest serial is on stdio (mon:stdio) — run this in a terminal
# (or CLion's "QEMU frozen" config with terminal emulation on).
# Ctrl+A then X to exit QEMU.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/debug/nexios-rtos.iso"
IMG="$ROOT/build/fat32.img"
EDK2="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"

[ -f "$ISO" ] || { echo "missing $ISO — run 'make debug' first"; exit 1; }
[ -f "$IMG" ] || { echo "missing $IMG — run 'make debug' first"; exit 1; }
[ -f "$EDK2" ] || { echo "missing $EDK2 (qemu package path?)"; exit 1; }

if pgrep -f 'qemu-system-x86_64.*nexios-rtos' >/dev/null; then
  echo "a nexios QEMU is already running (build/fat32.img lock) — kill it first"
  exit 1
fi

exec qemu-system-x86_64 \
  -cdrom "$ISO" \
  -m 256M \
  -serial mon:stdio \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,disable-legacy=on \
  -boot order=d \
  -drive "if=pflash,format=raw,readonly=on,file=$EDK2" \
  -cpu max \
  -drive "file=$IMG,format=raw,if=ide,index=1,media=disk" \
  -display none \
  -no-reboot \
  -s -S
