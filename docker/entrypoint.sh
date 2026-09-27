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

# The x86_64 QEMU boot/test path consumes QEMU_UEFI (Makefile
# QEMU_ARCH_FLAGS), not OVMF_CODE; without it the default is a macOS
# Homebrew path absent on Ubuntu and QEMU aborts on the missing pflash
# image. Feed the probe result to both variables.
if [ -z "${QEMU_UEFI:-}" ] && [ -n "${OVMF_CODE:-}" ]; then
    export QEMU_UEFI="$OVMF_CODE"
fi

echo "NEXIOS_TAG=$TAG OVMF_CODE=${OVMF_CODE:-<makefile-auto>} QEMU_UEFI=${QEMU_UEFI:-<makefile-default>}"
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
