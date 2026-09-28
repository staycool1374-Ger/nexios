# NexIOS release test/run environment (issue #243).
# Ubuntu 24.04 with the Linux-triplet cross toolchains the Makefile
# expects on Linux (X86_64/AARCH64/RISCV64_TRIPLET), QEMU + OVMF +
# ISO tooling, gdb-multiarch, and a gtimeout shim (the Makefile
# hard-requires `gtimeout`, a macOS-ism absent on Ubuntu).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git make python3 wget ccache cpio meson ninja-build \
    nasm \
    xorriso mtools dosfstools grub-pc-bin grub-common \
    gcc-x86-64-linux-gnu g++-x86-64-linux-gnu \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    gcc-riscv64-linux-gnu g++-riscv64-linux-gnu \
    qemu-system-x86 qemu-system-arm qemu-system-misc ovmf \
    expect gdb-multiarch coreutils ca-certificates \
 && ln -sf /usr/bin/timeout /usr/local/bin/gtimeout \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/x86_64-linux-gnu-gdb \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/aarch64-linux-gnu-gdb \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/riscv64-linux-gnu-gdb \
 && rm -rf /var/lib/apt/lists/*

# Tag this image was built for; the entrypoint clones it by default.
# Overridden per release by docker/metadata-action (NEXIOS_TAG=$ref).
ARG NEXIOS_TAG=main
ENV NEXIOS_TAG=${NEXIOS_TAG} \
    NEXIOS_REPO=https://github.com/staycool1374-Ger/nexios.git

COPY docker/entrypoint.sh /usr/local/bin/nexios-entrypoint.sh
RUN chmod +x /usr/local/bin/nexios-entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/nexios-entrypoint.sh"]
CMD ["bash", "-c", "echo 'NexIOS $NEXIOS_TAG ready. Try: make build, make execute-test x86_64 debug debug_syscall'; exec bash"]
