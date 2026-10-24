#!/bin/sh
# NexIOS RTOS — file-backed QEMU launcher for autonomous test runs.
#
# Issue #345: QEMU's stdio/mux chardev piped through expect|tee lets a slow
# host pipe reader block the vCPU thread inside guest Logger output
# (convicted live via GDB: vCPU frozen in Logger::putchar, stub
# unresponsive).  File backends remove ALL host-pipe backpressure from the
# vCPU write path: QEMU writes serial.log/com1.log directly, and expect
# tails the verdict stream while still observing process death (eof).
#
# Usage: qemu-file-log.sh <serial_log> <com1_log> <qemu> [qemu-args...]
#   - <serial_log> must be the guest debugcon backend (Logger/S-lines/
#     TEST SUMMARY/KERNEL PANIC all route there during test runs).
#   - The caller (qemu_test.py) substitutes file paths into the qemu args.
#   - Stdout = tail of <serial_log> (verdict matching); exit code = QEMU's.
#     (QEMU death closes the tail and the wrapper, yielding expect's eof arm.)

SERIAL="$1"
COM1="$2"
shift 2

rm -f "$SERIAL" "$COM1"
# Launch QEMU detached from this script's stdout (all guest output goes to
# the file backends baked into its args); stdin from /dev/null (test runs
# never need console input).
"$@" < /dev/null &
QP=$!
# Give QEMU a moment to truncate/recreate the logs so tail follows the
# fresh inode, not a stale one.
sleep 1
tail -c +1 -F "$SERIAL" &
TP=$!
wait "$QP"
EC=$?
kill "$TP" 2>/dev/null
exit "$EC"
