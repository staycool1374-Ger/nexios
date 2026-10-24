#!/bin/bash
# NexIOS release environment entrypoint (issue #243).
# Clones $NEXIOS_TAG into /work/nexios (skipped when already present,
# e.g. a mounted workspace), probes OVMF like .github/workflows/ci.yml,
# then execs the given command (default: interactive shell).
set -euo pipefail

TAG="${NEXIOS_TAG:-main}"
REPO="${NEXIOS_REPO:-https://github.com/staycool1374-Ger/nexios.git}"

if [ -z "${OVMF_CODE:-}" ]; then
    for cand in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF.fd \
                /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd \
                /usr/share/OVMF/OVMF_CODE.fd; do
        if [ -f "$cand" ]; then
            export OVMF_CODE="$cand"
            break
        fi
    done
fi

# The x86_64 QEMU boot/test path probes OVMF itself (qemu_test.py:find_ovmf
# mirrors the old OVMF_CODE search); still export the probe result for
# flows that honor QEMU_UEFI/OVMF_CODE (frozen Make targets, dogfood
# images pinned to pre-cutover tags).
if [ -z "${QEMU_UEFI:-}" ] && [ -n "${OVMF_CODE:-}" ]; then
    export QEMU_UEFI="$OVMF_CODE"
fi

echo "NEXIOS_TAG=$TAG OVMF_CODE=${OVMF_CODE:-<auto-probe>} QEMU_UEFI=${QEMU_UEFI:-<probe-default>}"
if [ ! -d /work/nexios/.git ]; then
    rm -rf /work/nexios
    git clone --depth 1 --branch "$TAG" "$REPO" /work/nexios
fi
cd /work/nexios
if [ $# -eq 0 ]; then
    echo "ERROR: no command given (image CMD was overridden with empty args)" >&2
    exit 2
fi
exec "$@"
