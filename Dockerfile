# NexIOS release test/run environment (issue #243).
# Ubuntu 24.04 with the Linux-triplet cross toolchains the Makefile
# expects on Linux (X86_64/AARCH64/RISCV64_TRIPLET), QEMU + OVMF +
# ISO tooling, gdb-multiarch, and a gtimeout shim (the Makefile
# hard-requires `gtimeout`, a macOS-ism absent on Ubuntu).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Buildx target arch (auto-filled: amd64/arm64). The x86 GRUB modules
# (grub-pc-bin, grub-efi-amd64-bin) exist only in the amd64 archive, not
# in Ubuntu ports — on arm64 they arrive via the download-and-extract
# step below, so BOTH legs build UEFI-bootable x86 ISOs. Without them
# grub-mkrescue produces an ISO with no UEFI entry and OVMF falls
# through to PXE (issue #243). aarch64/riscv64 boot via -kernel ELF
# and never touch GRUB.
ARG TARGETARCH

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git make python3 python3-pip wget ccache cpio \
    nasm \
    xorriso mtools dosfstools grub-common \
    $(if [ "$TARGETARCH" = "amd64" ]; then echo grub-pc-bin grub-efi-amd64-bin; fi) \
    gcc-x86-64-linux-gnu g++-x86-64-linux-gnu \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    gcc-riscv64-linux-gnu g++-riscv64-linux-gnu \
    qemu-system-x86 qemu-system-arm qemu-system-misc ovmf \
    expect gdb-multiarch coreutils ca-certificates \
 && ln -sf /usr/bin/timeout /usr/local/bin/gtimeout \
 && ln -sf /usr/bin/stdbuf /usr/local/bin/gstdbuf \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/x86_64-linux-gnu-gdb \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/aarch64-linux-gnu-gdb \
 && ln -sf /usr/bin/gdb-multiarch /usr/local/bin/riscv64-linux-gnu-gdb \
  # Bare-metal x86_64-elf-* toolchain for tools/build-picolibc.sh
  # (tools/picolibc-x86_64-elf.ini): the x86_64-linux-gnu triplet, NOT the
  # host-native /usr/bin/gcc — on arm64 hosts the native compiler is
  # AArch64 and rejects the x86 flags (-m64/-mno-red-zone, issue #243).
  # Triplet packages install on every host arch (ports carry the cross
  # toolchains, mirroring the unconditional aarch64/riscv64 lines above).
  # as/strip were missing entirely although the .ini references them.
  && ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc \
  && ln -sf /usr/bin/x86_64-linux-gnu-g++ /usr/local/bin/x86_64-elf-g++ \
  && ln -sf /usr/bin/x86_64-linux-gnu-as /usr/local/bin/x86_64-elf-as \
  && ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld \
  && ln -sf /usr/bin/x86_64-linux-gnu-ar /usr/local/bin/x86_64-elf-ar \
  && ln -sf /usr/bin/x86_64-linux-gnu-objcopy /usr/local/bin/x86_64-elf-objcopy \
  && ln -sf /usr/bin/x86_64-linux-gnu-strip /usr/local/bin/x86_64-elf-strip \
  && rm -rf /var/lib/apt/lists*

# x86 GRUB modules on arm64 hosts (issue #243, verified in-container):
# apt-get install would drag the whole amd64 dependency tree (libc6:amd64
# vs the arm64 userland — unresolvable), and the extracted amd64
# grub-mkimage binary could not execute on arm64 anyway. Instead
# download + dpkg-deb-extract (no dependency resolution, no execution):
# the .mod files are pure target data, and the NATIVE grub-mkimage builds
# x86_64-efi + i386-pc images from them (proven: EFI-BUILD-OK +
# PC-BUILD-OK on the arm64 image). The amd64 leg is untouched (both
# packages already installed above unconditionally). The ports source is
# scoped to arm64 first — otherwise apt queries amd64 indexes on
# ports.ubuntu.com (404 -> exit 100).
RUN if [ "$(dpkg --print-architecture)" != "amd64" ]; then \
    dpkg --add-architecture amd64 \
    && sed -i 's/^Components:/Architectures: arm64\nComponents:/' /etc/apt/sources.list.d/ubuntu.sources \
    && printf 'deb [arch=amd64] http://archive.ubuntu.com/ubuntu noble noble-updates noble-security main universe\n' > /etc/apt/sources.list.d/amd64.list \
    && apt-get update \
    && cd /tmp && apt-get download grub-pc-bin:amd64 grub-efi-amd64-bin:amd64 \
    && dpkg-deb -x grub-pc-bin*.deb / && dpkg-deb -x grub-efi-amd64-bin*.deb / \
    && rm -f /tmp/grub*.deb && rm -rf /var/lib/apt/lists/*; \
  fi

# Pinned pip meson/ninja (tools/build-picolibc.sh pins meson 1.12.0,
# ninja 1.13.2): Ubuntu 24.04 apt ships meson 1.3.2, whose link-based
# compiler sanity check fails under the freestanding flags
# (-nostdlib -static); 1.12 checks compile-only (issue #243).
RUN pip install --break-system-packages "meson==1.12.0" "ninja==1.13.2"

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
