# Jarvis RTOS — Development Roadmap / Kernel Core
# Copyright (C) 2026 Arnold Hasshold
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

SHELL := /bin/bash

# ==============================================================================
# Jarvis RTOS Microkernel Makefile
# Target architecture: set ARCH to build for a different arch (default: x86_64)
#
# External test suite support:
#   EXTERNAL_TEST_DIR — path to a directory containing additional test sources
#   to compile and link into the kernel.  When set, only selftest files from
#   src/kernel/test/ are built; all other test files are expected in
#   $(EXTERNAL_TEST_DIR)/src/kernel/test/.
#
#   Example: make execute-test x86 debug all EXTERNAL_TEST_DIR=/path/to/tests
# ==============================================================================

# ------------------------------------------------------------------------------
# Architecture selection — must be set before toolchain and source discovery
# ------------------------------------------------------------------------------
ARCH ?= x86_64

SUPPORTED_ARCHS := x86_64 aarch64 riscv64
ifneq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),$(ARCH))
$(error Unsupported architecture '$(ARCH)'. Supported: $(SUPPORTED_ARCHS))
endif

# ------------------------------------------------------------------------------
# External test suite directory
# ------------------------------------------------------------------------------
# When set, only selftest (safe-class) files from src/kernel/test/ are compiled;
# all other test sources are expected under $(EXTERNAL_TEST_DIR)/src/kernel/test/.
# The external tests are compiled with the same CXXFLAGS and linked into the
# kernel binary.  Leave empty to build all kernel-space tests normally.
# ------------------------------------------------------------------------------
EXTERNAL_TEST_DIR ?=

# Architecture stamp — forces rebuild of arch-specific binaries (initrd, fat32)
# when ARCH changes between invocations.
# ------------------------------------------------------------------------------

# Derive upper-case ARCH for CONFIG_ARCH_* define
ARCH_UPPER := $(shell echo $(ARCH) | tr a-z A-Z)

# ------------------------------------------------------------------------------
# Architecture shorthand mapping for unified targets
#   x86  → x86_64   arm → aarch64   riscv → riscv64
#   Already canonical → pass through
# ------------------------------------------------------------------------------
_map_arch = $(or \
    $(filter $(SUPPORTED_ARCHS),$(1)), \
    $(patsubst x86,x86_64,$(patsubst arm,aarch64,$(patsubst riscv,riscv64,$(1)))))

QEMU_UEFI  ?= /opt/homebrew/share/qemu/edk2-$(ARCH)-code.fd

# ------------------------------------------------------------------------------
# Host Environment & OS Detection
# ------------------------------------------------------------------------------
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    GET_SIZE := stat -f%z
    PKG_HINT := "Install with: brew install qemu xorriso"
else
    GET_SIZE := stat -c%s
    PKG_HINT := "Install with: apt-get install qemu-system-x86 xorriso grub-common"
endif

# ------------------------------------------------------------------------------
# No targets yet — targets are defined below.
# 
# Host-OS detection (Darwin/Linux) and selection of the correct
# cross-compiler triplet so bare-metal GCC/ld/objcopy are found
# correctly on both macOS (Homebrew) and Linux (distro packages).
#
# CXXFLAGS / CCFLAGS here are the *base* flags.  Build-type-specific
# flags (e.g. -Og -DCONFIG_DEBUG for debug, -O2 for release) are
# added via target-specific variable overrides on the 'debug'/'release'
# targets.
# ------------------------------------------------------------------------------

# --- Common CXXFLAGS (architecture-independent) ---
# CONFIG_DEFS: extra -D overrides injected per build (e.g. config-matrix runs
# such as -DCONFIG_DEADLINE_ACTION=N). Pass on the command line:
#   make debug CONFIG_DEFS="-DCONFIG_DEADLINE_ACTION=3"
CONFIG_DEFS ?=
CXXFLAGS_COMMON := -std=c++20 -ffreestanding -fno-exceptions -fno-rtti \
                   -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
                   -fno-threadsafe-statics \
                   -Wall -Wextra -Werror \
                   -I src -I src/lib -pipe -MMD -MP \
                   -DCONFIG_ARCH_$(ARCH_UPPER) \
                   -ffunction-sections -fdata-sections \
                   $(CONFIG_DEFS)

# 1. Cross-Compiler-Prefixe basierend auf dem Host-OS
ifeq ($(UNAME_S),Darwin)
    X86_64_TRIPLET  ?= x86_64-elf-
    AARCH64_TRIPLET ?= aarch64-elf-
    RISCV64_TRIPLET ?= riscv64-elf-
    export PATH := /opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:$(PATH)
else
    X86_64_TRIPLET  ?= x86_64-linux-gnu-
    AARCH64_TRIPLET ?= aarch64-linux-gnu-
    RISCV64_TRIPLET ?= riscv64-linux-gnu-
endif

# 2. Architektur-spezifische Werkzeuge und Flags
ifeq ($(ARCH),x86_64)

    CC          := $(X86_64_TRIPLET)gcc
    CXX         := $(X86_64_TRIPLET)g++
    LD          := $(X86_64_TRIPLET)ld
    AR          := $(X86_64_TRIPLET)ar
    OBJCOPY     := $(X86_64_TRIPLET)objcopy

    CXXFLAGS    := $(CXXFLAGS_COMMON) -m64 -mno-red-zone -mgeneral-regs-only -mcmodel=large
    CCFLAGS     := -m64 -static -nostdlib -ffreestanding -O2 -pipe -MMD -MP \
                   -ffunction-sections -fdata-sections

    AS          := nasm
    ASFLAGS     := -f elf64

    OBJCOPY_FMT  := elf64-x86-64
    OBJCOPY_ARCH := i386
    LDFLAGS     := -m elf_x86_64 -nostdlib -no-pie -T linker/linker_$(ARCH).ld \
                   -z max-page-size=0x1000 -Map=build/kernel.map

    GCOV_LIB_DIR    := $(dir $(shell $(CC) -print-file-name=libgcov.a 2>/dev/null))
    QEMU_SYSTEM     := qemu-system-x86_64
    QEMU_ARCH_FLAGS := -boot order=d \
                       -drive if=pflash,format=raw,readonly=on,file=$(QEMU_UEFI)
    QEMU_DEBUG_EXIT := -device isa-debug-exit
    OBJDUMP_DIS_FLAGS := -M intel

else ifeq ($(ARCH),aarch64)

    CC          := $(AARCH64_TRIPLET)gcc
    CXX         := $(AARCH64_TRIPLET)g++
    LD          := $(AARCH64_TRIPLET)ld
    AR          := $(AARCH64_TRIPLET)ar
    OBJCOPY     := $(AARCH64_TRIPLET)objcopy

    CXXFLAGS    := $(CXXFLAGS_COMMON) -march=armv8-a
    CCFLAGS     := -static -nostdlib -ffreestanding -O2 -pipe -MMD -MP \
                   -ffunction-sections -fdata-sections \
                   -I src -I src/lib

    AS          := $(AARCH64_TRIPLET)as
    ASFLAGS     :=

    OBJCOPY_FMT  := elf64-littleaarch64
    OBJCOPY_ARCH := aarch64
    LIBGCC       := $(shell $(CC) --print-libgcc-file-name)
    LD_LIBS      := $(LIBGCC)
    LDFLAGS      := -nostdlib -T linker/linker_$(ARCH).ld -Map=build/kernel.map

    QEMU_SYSTEM     := qemu-system-aarch64
    QEMU_ARCH_FLAGS := -machine virt -cpu cortex-a72 -kernel $(KERNEL_DEBUG)
    QEMU_DEBUG_EXIT  :=
    OBJDUMP_DIS_FLAGS :=

else ifeq ($(ARCH),riscv64)

    CC          := $(RISCV64_TRIPLET)gcc
    CXX         := $(RISCV64_TRIPLET)g++
    LD          := $(RISCV64_TRIPLET)ld
    AR          := $(RISCV64_TRIPLET)ar
    OBJCOPY     := $(RISCV64_TRIPLET)objcopy

    CXXFLAGS    := $(CXXFLAGS_COMMON) -march=rv64imafdc -mabi=lp64d -mcmodel=medany
    CCFLAGS     := -static -nostdlib -ffreestanding -O2 -pipe -MMD -MP \
                   -ffunction-sections -fdata-sections \
                   -DCONFIG_ARCH_RISCV64 \
                   -mcmodel=medany

    AS          := $(RISCV64_TRIPLET)as
    ASFLAGS     :=

    OBJCOPY_FMT  := elf64-littleriscv
    OBJCOPY_ARCH := riscv
    LIBGCC       := $(shell $(CC) --print-libgcc-file-name)
    LD_LIBS      := $(LIBGCC)
    LDFLAGS      := -nostdlib -T linker/linker_$(ARCH).ld -Map=build/kernel.map

    QEMU_SYSTEM     := qemu-system-riscv64
    QEMU_ARCH_FLAGS := -machine virt -bios default
    OBJDUMP_DIS_FLAGS :=

endif

# ----- clang-tidy (static analysis, debug builds) -----
CLANG_TIDY          ?= /opt/homebrew/opt/llvm/bin/clang-tidy
# clang-tidy checks. The three disabled checks are false-positives for an
# OS kernel: int->ptr casts are required for MMIO/physical addresses,
# __cyg_profile_* reserved identifiers are mandated by the gcov ABI, and many
# driver/VMM APIs intentionally use adjacent same-type parameters.
CLANG_TIDY_CHECKS   := bugprone-*,concurrency-*,performance-*,-performance-no-int-to-ptr,-bugprone-reserved-identifier,-bugprone-easily-swappable-parameters
CLANG_TIDY_NCORES   := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# ----- ccache (optional, no-op if not installed) -----
CCACHE := $(shell which ccache 2>/dev/null)
# Prepend ccache-wrapper and include arch-stamp in hash so cross-arch
# builds don't poison each other's cache.
CCACHE_EXTRAFILES := $(ARCH_STAMP)
CXX := $(CCACHE) $(CXX)

# --- Abgeleitete Toolchain-Aliase ---
OBJDUMP := $(patsubst %-objcopy,%-objdump,$(OBJCOPY))
GDB     := $(patsubst %-objcopy,%-gdb,$(OBJCOPY))

# ------------------------------------------------------------------------------
# Output paths
# ------------------------------------------------------------------------------
KERNEL         := build/kernel.elf
KERNEL_DEBUG   := build/kernel-debug.elf
DEBUG_ISO      := debug/nexios-rtos.iso
RELEASE_ISO    := release/nexios-rtos.iso
KERNEL_SYMBOLS := build/kernel.sym
KERNEL_DIS     := build/kernel.dis

FAT32_DISK     := build/fat32.img
QEMU_NET       := -netdev user,id=net0 -device virtio-net-pci,netdev=net0,disable-legacy=on
# Recursive evaluation (use `=`) so that DEBUG_ISO can be overridden
# per-target (e.g. release tests) via target-specific variables or $(eval).
# Kernel debug output goes to the QEMU debugcon port (0xE9) instead of the
# UART, so logging does not warp microsecond-scale scheduling/IPC timing (H2
# investigation).  stdio can be used only by ONE chardev, so serial + debugcon
# + monitor are multiplexed onto a single mux chardev; the test harness still
# parses the combined stream for the TEST SUMMARY.
QEMU_FLAGS     = -cdrom $(DEBUG_ISO) -m 256M \
                -chardev stdio,id=dbg,mux=on \
                -serial chardev:dbg -device isa-debugcon,chardev=dbg -mon chardev=dbg \
                $(QEMU_NET) $(QEMU_ARCH_FLAGS)

# Interactive shell (class=none) flags.  Default: same as QEMU_FLAGS (aarch64 /
# riscv64 already use -serial mon:stdio, no mux).  On x86_64 the test flags use
# the mux chardev, which gives stdin input focus to whichever frontend last
# wrote; during/after selftest the debugcon steals keystrokes away from the
# shell's COM1 polling, making the interactive shell appear input-dead.  Use a
# plain serial console instead so the shell receives input directly; the
# framebuffer window still mirrors the same in/out (kernel Terminal writes to
# both; readline polls COM1 + PS/2 keyboard).  Test runs keep the mux so the
# harness parses the combined debugcon+serial stream for the TEST SUMMARY.
QEMU_FLAGS_INTERACTIVE = $(QEMU_FLAGS)
# For x86_64 add IDE drive for FAT32
ifeq ($(ARCH),x86_64)
QEMU_FLAGS     += -drive file=$(FAT32_DISK),format=raw,if=ide,index=1,media=disk
QEMU_FLAGS_INTERACTIVE = -cdrom $(DEBUG_ISO) -m 256M \
                -serial mon:stdio \
                $(QEMU_NET) $(QEMU_ARCH_FLAGS)
QEMU_FLAGS_INTERACTIVE += -drive file=$(FAT32_DISK),format=raw,if=ide,index=1,media=disk
endif

# For aarch64, load kernel directly instead of via ISO/GRUB
ifeq ($(ARCH),aarch64)
QEMU_FLAGS     = -kernel $(KERNEL_DEBUG) -m 256M -serial mon:stdio $(QEMU_NET) \
                -machine virt -cpu cortex-a72 -display none -no-reboot \
                -semihosting-config enable=on,target=native
endif

# For riscv64, QEMU 11+ has a fw_dynamic bug where ELF entry point is not
# passed to OpenSBI. Use raw binary instead (objcopy strips ELF headers).
KERNEL_RISCV_BIN := build/kernel-riscv64.bin

ifeq ($(ARCH),riscv64)
$(KERNEL_RISCV_BIN): $(KERNEL_DEBUG)
	$(OBJCOPY) -O binary $< $@

QEMU_FLAGS     = -kernel $(KERNEL_RISCV_BIN) -m 256M -serial mon:stdio \
                $(QEMU_NET) -machine virt -bios default -display none -no-reboot
endif

# ------------------------------------------------------------------------------
# Build type stamp
#
# Debug and release share build/.  The stamp ensures make clean is triggered
# when switching build types so stale object files don't leak between builds.
# ------------------------------------------------------------------------------
BUILD_STAMP := build/.build-type

# ------------------------------------------------------------------------------
# Shared build rules (pattern rules, libc, userspace, initrd)
# ------------------------------------------------------------------------------
include mk/rules.mk

# ------------------------------------------------------------------------------
# Generated test registry header
# ------------------------------------------------------------------------------
TEST_REGISTRY_GEN    := src/kernel/test/test_registry.gen.hpp
TEST_REGISTRY_SCRIPT := tools/gen_test_registry.py
TEST_SRC_DIR         := src/kernel/test
# Use the actual compiled test file list (respects Makefile exclusions).
# This avoids referencing symbols from #if 0 / excluded test files.
TEST_SRC_FILES       := $(filter src/kernel/test/%.cpp, $(SRC_CXX))
TEST_REGISTRY_DEPS   := $(TEST_SRC_FILES) $(TEST_REGISTRY_SCRIPT)

# Write the file list to a temporary file so the Python script reads the
# same files the build system compiles (no directory walk).
TEST_FILE_LIST       := build/.test-file-list

$(TEST_FILE_LIST): $(TEST_SRC_FILES)
	@mkdir -p $(dir $@)
	@printf '%s\n' $(TEST_SRC_FILES) > $@

$(TEST_REGISTRY_GEN): $(TEST_FILE_LIST) $(TEST_REGISTRY_DEPS)
	@printf '  %-7s %s\n' 'GEN' '$@'
	@python3 $(TEST_REGISTRY_SCRIPT) $(TEST_SRC_DIR) $@ --file-list $(TEST_FILE_LIST)

# Kernel link targets depend on the generated test registry header.
# This ensures it exists even in sub-make invocations (e.g. release build).
$(KERNEL_DEBUG): $(TEST_REGISTRY_GEN)
$(KERNEL): $(TEST_REGISTRY_GEN)

# test_isolate.cpp #includes the generated header; ensure it exists
# before compiling that translation unit.
build/kernel/test/test_isolate.o: $(TEST_REGISTRY_GEN)

# External test registry is not generated here — the test_registry.gen.hpp
# generated from kernel selftest sources is sufficient.  External tests
# register at runtime via JARVIS_REGISTER_TEST() macros in their compiled
# object code.  The weak symbol register_external_test_classes() in
# test_registry.cpp calls into external registration functions when linked.

# ------------------------------------------------------------------------------
# Phony targets
# ------------------------------------------------------------------------------
.PHONY: help all build check-style run-debug-mode run-release-mode \
        execute-test debug-test debug-shell \
        test symbols objdump debug release profiling rr-record rr-replay \
        run-renode renode-test check-config config-summary test-registry-gen

test-registry-gen: $(TEST_REGISTRY_GEN)

# Default target.  Only print help when `all` is the SOLE goal — otherwise it
# collides with the `all` test class passed positionally to `execute-test`
# (e.g. `make execute-test x86_64 debug all`), which would also trigger this
# help target and interleave/cut the QEMU run.
all:
	@if [ -z "$(MAKECMDGOALS)" ] || [ "$(MAKECMDGOALS)" = "all" ]; then \
	    $(MAKE) help; \
	fi

# ------------------------------------------------------------------------------
# Style / Help
# ------------------------------------------------------------------------------
build: check-style debug

check-style:
	@printf '  %-7s %s\n' 'STYLE' 'Validating kernel sources…'
	@python3 tools/validate_style.py --root src/kernel; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	    printf '  %-7s %s\n' 'STYLE' 'Passed.'; \
	elif [ $$rc -eq 1 ]; then \
	    printf '  %-7s %s\n' 'STYLE' 'Errors found.'; \
	    exit 1; \
	fi

check-config:
	@printf '  %-7s %s\n' 'CONFIG' 'Validating kernel configuration…'
	@ARCH=$(ARCH) python3 tools/check-config.py; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	    printf '  %-7s %s\n' 'CONFIG' 'All checks passed.'; \
	else \
	    printf '  %-7s %s\n' 'CONFIG' 'Errors found!'; \
	    exit 1; \
	fi

config-summary:
	@printf '  %-7s %s\n' 'CONFIG' 'Active configuration:'
	@grep -h '#define CONFIG_' src/kernel/nexios_config.h | grep -v '//' | \
	sed 's/#define /  /' | column -t

help:
	@echo "Jarvis RTOS — Build Targets"
	@echo ""
	@echo "  [A] make (or make all)     Show this help"
	@echo "  [A] make build             Style check + debug ISO"
	@echo "  [A] make check-style       Validate kernel coding style (src/kernel/)"
	@echo "  [A] make check-config      Validate kernel configuration consistency"
	@echo "  [A] make config-summary    Print active configuration values"
	@echo "  [A] make debug             Build debug ISO"
	@echo "  [A] make release           Build production ISO"
	@echo "  [A] make clean             Remove all build artifacts"
	@echo "  [I] make run-debug-mode [<arch>]   Interactive debug shell (QEMU, mon:stdio)"
	@echo "  [I] make run-release-mode [<arch>] Interactive release shell (QEMU, mon:stdio)"
	@echo "         <arch> = x86|x86_64|arm|aarch64|riscv|riscv64  (default: x86_64)"
	@echo "  [A] make execute-test <arch> <build> <class>  Unified test runner"
	@echo "         <arch>  = x86|x86_64|arm|aarch64|riscv|riscv64"
	@echo "         <build> = debug|release"
	@echo "         <class> = none|selftest|all|<name>"
	@echo "           none     → interactive shell (no tests, mon:stdio)"
	@echo "           selftest → safe class, auto-shutdown (CI gate, 120s)"
	@echo "           all      → all classes, auto-shutdown (180s debug / 120s release)"
	@echo "           <name>   → specific test class, auto-shutdown (120s)"
	@echo "  [A] make debug-test <arch> <build> <class> <gdb-script>"
	@echo "         GDB batch surveillance. QEMU with -s -S, runs gdb-script,"
	@echo "         reports panic capture. 120s timeout."
	@echo "  [A] make debug-shell <arch> <build> none <gdb-script> <shell-cmds>"
	@echo "         GDB + serial interaction. Reads commands from text file,"
	@echo "         sends them via serial after GDB continues."
	@echo "  [A] make symbols           Kernel symbol table"
	@echo "  [A] make objdump           Kernel disassembly"
	@echo "  [A] make profiling         GCOV coverage capture"
	@echo "  [I] make rr-record         Record QEMU execution (deterministic replay)"
	@echo "  [I] make rr-replay         Replay recorded session with GDB stub (:1234)"
	@echo "  [I] make run-renode        Boot debug ISO in Renode (x86_64)"
	@echo "  [I] make run-renode RENODE_ARCH=aarch64  Boot AArch64 in Renode"
	@echo "  [I] make run-renode RENODE_ARCH=riscv64  Boot RISC-V 64 in Renode"
	@echo "  [A] make renode-test       Run selftest in Renode (x86_64)"
	@echo "  [A] make renode-test RENODE_ARCH=aarch64  Run selftest on AArch64"
	@echo "  [A] make renode-test RENODE_ARCH=riscv64  Run selftest on RISC-V 64"
	@echo ""
	@echo "  [A] = autonomous (run and done, no interaction needed)"
	@echo "  [I] = interactive (user sits at the QEMU/GDB/Renode console)"
	@echo ""
	@echo "  Examples:"
	@echo "    make run-debug-mode                       # interactive x86_64 debug QEMU"
	@echo "    make run-release-mode                     # interactive x86_64 release QEMU"
	@echo "    make run-release-mode arm                 # interactive AArch64 release QEMU"
	@echo "    make run-release-mode riscv               # interactive RISC-V 64 release QEMU"
	@echo "    make execute-test x86 debug none          # interactive QEMU (verbose)"
	@echo "    make execute-test x86 release all         # full release test suite"
	@echo "    make debug-test x86 debug all tools/gdb/test-batch.gdb  # GDB panic capture"
	@echo "    make debug-shell x86 debug none tools/gdb/init.gdb cmds.txt"

# ------------------------------------------------------------------------------
# Build-type stamp check
#
# Triggers clean if:
#   (a) stamp exists and doesn't match 'debug', or
#   (b) no stamp exists but .o files are present (interrupted build).
# ------------------------------------------------------------------------------
check-build-stamp:
	@stamp=; target=debug; \
	if [ -f $(BUILD_STAMP) ]; then \
	    stamp=$$(cat $(BUILD_STAMP)); \
	fi; \
	if [ "$$stamp" != "$$target" ]; then \
	    if [ -n "$$stamp" ]; then \
	        printf '  %-7s %s\n' 'CLEAN' "Build type changed ($$stamp -> $$target)"; \
	    elif ls build/ 2>/dev/null | grep -qv '\.gitkeep\|libc\|initrd\|profiling'; then \
	        printf '  %-7s %s\n' 'CLEAN' 'Stale objects from interrupted build'; \
	    else \
	        target=; \
	    fi; \
	    if [ -n "$$target" ]; then \
	        $(MAKE) clean >/dev/null 2>&1; \
	    fi; \
	fi; \
	mkdir -p build; echo debug > $(BUILD_STAMP)

# ------------------------------------------------------------------------------
# Debug build
# ------------------------------------------------------------------------------
debug: clang-tidy
ifneq ($(NO_LTO),1)
# Link-time optimization: cross-TU inlining/IPO across kernel C++ TUs.
# Scoped to CXXFLAGS (kernel .cpp only) — libc/userspace share CCFLAGS and
# link via raw ld/gcc without the LTO plugin, so they must stay non-LTO.
# The kernel link uses the compiler driver (see mk/rules.mk) to load the
# LTO plugin; LDFLAGS carries -flto for that.
debug: CXXFLAGS += -g -Og -DCONFIG_DEBUG -fno-omit-frame-pointer -fstack-usage -flto
debug: CCFLAGS  += -g -Og -DCONFIG_DEBUG -fno-omit-frame-pointer
debug: LDFLAGS  += -flto
else
# GDB flows (debug-test/debug-shell/rr-record) build without LTO so function
# boundaries survive for clean single-stepping and stack unwinding.
debug: CXXFLAGS += -g -Og -DCONFIG_DEBUG -fno-omit-frame-pointer -fstack-usage
debug: CCFLAGS  += -g -Og -DCONFIG_DEBUG -fno-omit-frame-pointer
endif
debug: $(TEST_REGISTRY_GEN)
debug: check-build-stamp $(KERNEL_DEBUG) stack-usage-report
ifeq ($(ARCH),x86_64)
	cp $(KERNEL_DEBUG) iso/boot/kernel.elf
	@mkdir -p $(dir $(DEBUG_ISO))
	@printf '  %-7s %s\n' 'ISO' '$(DEBUG_ISO)'
	@grub-mkrescue -o $(DEBUG_ISO) iso 2>&1 || \
	 (printf '  %-7s %s\n' 'ERROR' 'grub-mkrescue failed'; echo 'Install grub-pc + xorriso'; exit 1)
	@printf '  %-7s %s\n' 'DONE' 'Debug ISO: $(DEBUG_ISO)'
else ifeq ($(ARCH),riscv64)
	$(OBJCOPY) -O binary $(KERNEL_DEBUG) $(KERNEL_RISCV_BIN)
	@printf '  %-7s %s\n' 'BIN' '$(KERNEL_RISCV_BIN)'
	@printf '  %-7s %s\n' 'DONE' 'Debug kernel: $(KERNEL_DEBUG)'
else
	@printf '  %-7s %s\n' 'DONE' 'Debug kernel: $(KERNEL_DEBUG)'
endif

# ------------------------------------------------------------------------------
# Release build
#
# The sub-make receives CXXFLAGS without -DCONFIG_DEBUG (never added to base).
# The stamp is written first so an interrupted build is detected on next run.
# ------------------------------------------------------------------------------
release: CXXFLAGS += -g -O2 -fanalyzer -Wno-error=analyzer-null-argument -Wno-error=analyzer-possible-null-dereference -Wno-error=analyzer-use-of-uninitialized-value -Wno-error=analyzer-infinite-loop -Wno-error=analyzer-malloc-leak -Wno-error=analyzer-undefined-behavior-ptrdiff
release: $(TEST_REGISTRY_GEN)
release:
ifneq ($(ARCH),x86_64)
	@printf '  %-7s %s\n' 'ERROR' 'Release builds only supported on x86_64 (arch=$(ARCH))'
	@exit 1
endif
	@printf '  %-7s %s\n' 'RELEASE' 'Building release ISO…'
	$(MAKE) clean
	$(MAKE) $(KERNEL) $(INITRD_OBJ) iso/boot CXXFLAGS="$(CXXFLAGS)"
	@printf '  %-7s %s\n' 'SIZE' "$$($(GET_SIZE) $(KERNEL)) bytes"
	cp $(KERNEL) iso/boot/kernel.elf
	@mkdir -p $(dir $(RELEASE_ISO)) $(dir $(BUILD_STAMP))
	@echo release > $(BUILD_STAMP)
	@printf '  %-7s %s\n' 'ISO' '$(RELEASE_ISO)'
	@grub-mkrescue -o $(RELEASE_ISO) iso 2>/dev/null || \
	(printf '  %-7s %s\n' 'ERROR' 'grub-mkrescue failed'; echo $(PKG_HINT); exit 1)
	@mkdir -p $(dir $(RELEASE_ISO))
	@printf '%s\n' "$$(cat initrd/tests/test-config.txt 2>/dev/null || printf safe)" > release/.baked-test-config
	@printf '  %-7s %s\n' 'DONE' "Release ISO: $(RELEASE_ISO) (tests: $$(cat release/.baked-test-config))"
	@echo ""
	@echo "  Validate with:  make release-test"

# ------------------------------------------------------------------------------
# Profiling
# ------------------------------------------------------------------------------
ifneq ($(ARCH),x86_64)
profiling:
	@printf '  %-7s %s\n' 'ERROR' 'Profiling only supported on x86_64 (arch=$(ARCH))'; exit 1
else
profiling: CXX := x86_64-elf-g++
profiling: CXXFLAGS := $(filter-out -target x86_64-elf,$(CXXFLAGS)) \
                       -Wno-type-limits -Wno-unused-but-set-variable \
                       -Wno-class-memaccess \
                       -finstrument-functions -DCONFIG_PROFILING
profiling: LDFLAGS += -L $(GCOV_LIB_DIR)
profiling: clean $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) linker/linker_$(ARCH).ld iso/boot
	@printf '  %-7s %s\n' 'LD' 'kernel.elf (profiling)'
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ)
	cp $(KERNEL) iso/boot/kernel.elf
	@printf '  %-7s %s\n' 'ISO' '$(DEBUG_ISO)'
	@mkdir -p $(dir $(DEBUG_ISO))
	@grub-mkrescue -o $(DEBUG_ISO) iso 2>/dev/null
	@printf '  %-7s %s\n' 'PROFILE' 'Booting in QEMU…'
	mkdir -p build/profiling
	$(QEMU_SYSTEM) -cdrom $(DEBUG_ISO) -m 256M -serial file:build/profiling/gcda.raw \
	    $(QEMU_NET) -boot order=d -no-reboot -device isa-debug-exit \
	    -drive if=pflash,format=raw,readonly=on,file=$(QEMU_UEFI) 2>&1 | \
	    tee build/profiling/qemu.log
	@printf '  %-7s %s\n' 'PROFILE' 'Extracting coverage data…'
	python3 tools/extract_gcda.py build/profiling/gcda.raw
endif

# ------------------------------------------------------------------------------
# QEMU / Test targets
# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
# Unified test execution targets
#
# Positional arguments:
#   <arch>   — x86|x86_64|arm|aarch64|riscv|riscv64
#   <build>  — debug|release
#   <class>  — none|selftest|all|<class-name>
#   <gdb-script> — path to .gdb script (debug-test only)
#   <shell-cmds> — path to shell-commands text file (debug-shell only)
#
# The match-all rule (%::) at the end of the file silently consumes the
# positional arguments so make does not reject them as unknown targets.
# ------------------------------------------------------------------------------

# ------------------------------------------------------------------------------
# Convenience aliases for interactive QEMU sessions
#   make run-debug-mode [<arch>]   — same as execute-test <arch> debug none
#   make run-release-mode [<arch>] — same as execute-test <arch> release none
# ------------------------------------------------------------------------------

run-debug-mode:
	@: $(eval _ae := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))) $(eval _ARCH := $(call _map_arch,$(word 1,$(_ae))))
	@if [ -z "$(_ARCH)" ]; then \
	    $(MAKE) execute-test $(ARCH) none BUILD=debug CLASS=none; \
	else \
	    $(MAKE) execute-test $(_ARCH) none BUILD=debug CLASS=none ARCH=$(_ARCH); \
	fi

run-release-mode:
	@: $(eval _ae := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))) $(eval _ARCH := $(call _map_arch,$(word 1,$(_ae))))
	@if [ -z "$(_ARCH)" ]; then \
	    $(MAKE) execute-test $(ARCH) none BUILD=release CLASS=none; \
	else \
	    $(MAKE) execute-test $(_ARCH) none BUILD=release CLASS=none ARCH=$(_ARCH); \
	fi

# Public entry: parse positional <arch> <build> <class> from MAKECMDGOALS,
# then forward to the private _do_execute_test target using NAMED variables.
# This avoids re-parsing MAKECMDGOALS inside the worker and removes any
# ambiguity with the %:: catch-all / default `all` goal.
execute-test:
	@: $(eval _ae := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))) $(eval _ARCH := $(call _map_arch,$(word 1,$(_ae)))) $(eval _BUILD := $(or $(BUILD),$(word 2,$(_ae)))) $(eval _CLASS := $(or $(CLASS),$(word 3,$(_ae))))
	@if [ -z "$(_ARCH)" -o -z "$(_BUILD)" -o -z "$(_CLASS)" ]; then \
	    echo "Usage: make execute-test <arch> <build> <class>"; \
	    echo "  arch:  x86|x86_64|arm|aarch64|riscv|riscv64"; \
	    echo "  build: debug|release"; \
	    echo "  class: none|selftest|all|<name>|dump-counts"; \
	    exit 1; \
	fi; \
	case "$(_BUILD)" in debug|release) ;; *) echo "ERROR: build must be 'debug' or 'release'"; exit 1;; esac; \
	$(MAKE) _do_execute_test ARCH=$(_ARCH) BUILD=$(_BUILD) CLASS=$(_CLASS)

# Private worker: all decisions use ARCH/BUILD/CLASS (never MAKECMDGOALS).
_do_execute_test:
	@if [ -z "$(ARCH)" -o -z "$(BUILD)" -o -z "$(CLASS)" ]; then \
	    echo "ERROR: _do_execute_test requires ARCH/BUILD/CLASS"; exit 1; \
	fi; \
	case "$(BUILD)" in debug|release) ;; *) echo "ERROR: build must be 'debug' or 'release'"; exit 1;; esac; \
	mkdir -p initrd/tests; \
	_class_file=$(CLASS); \
	[ "$${_class_file}" = "selftest" ] && _class_file=safe; \
	printf '%s\n' "$${_class_file}" > initrd/tests/test-config.txt; \
 	if [ "$(CLASS)" = "none" ]; then \
 	    if [ "$(BUILD)" = "release" ] && [ "$(ARCH)" = "x86_64" ] && \
 	       [ -f "$(RELEASE_ISO)" ] && [ -f "release/.baked-test-config" ] && \
 	       [ "$$(cat release/.baked-test-config)" = "none" ]; then \
 	        :; \
 	    elif [ "$(BUILD)" = "release" ] && [ "$(ARCH)" = "x86_64" ]; then \
 	        $(MAKE) release ARCH=$(ARCH) || exit 1; \
	    elif [ "$(BUILD)" = "release" ]; then \
	        $(MAKE) $(KERNEL) ARCH=$(ARCH) || exit 1; \
	        if [ "$(ARCH)" = "riscv64" ]; then \
	            $(MAKE) $(KERNEL_RISCV_BIN) ARCH=$(ARCH) KERNEL_DEBUG=$(KERNEL) || exit 1; \
	        fi; \
	    else \
	        $(MAKE) debug ARCH=$(ARCH) || exit 1; \
	    fi; \
	else \
	    $(MAKE) $(BUILD) ARCH=$(ARCH) || exit 1; \
	fi; \
	true $(if $(filter release,$(BUILD)),$(eval DEBUG_ISO := $(RELEASE_ISO)))$(if $(and $(filter release,$(BUILD)),$(filter aarch64,$(ARCH))),$(eval QEMU_FLAGS := $(subst $(KERNEL_DEBUG),$(KERNEL),$(QEMU_FLAGS)))); \
	if [ "$(CLASS)" = "none" ]; then \
	    if command -v $(QEMU_SYSTEM) >/dev/null 2>&1; then \
	        printf '  %-7s %s\n' 'QEMU' 'Starting (Ctrl+A then X to exit)…'; \
	        $(QEMU_SYSTEM) $(QEMU_FLAGS_INTERACTIVE); \
	    else \
	        echo "QEMU missing."; echo $(PKG_HINT); exit 1; \
	    fi; \
	elif [ "$(CLASS)" = "dump-counts" ]; then \
	    $(call _run_dump_counts_qemu); \
	else \
	    $(call _run_test_qemu,Running $(BUILD) class=$(CLASS),$(if $(filter all,$(CLASS)),$(TEST_TIMEOUT_ALL),$(TEST_TIMEOUT_CLASS))); \
	fi

debug-test:
	@: $(eval _ae := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))) $(eval _ARCH := $(call _map_arch,$(word 1,$(_ae)))) $(eval _BUILD := $(word 2,$(_ae))) $(eval _CLASS := $(word 3,$(_ae))) $(eval _GDB := $(word 4,$(_ae)))
	@if [ -z "$(_ARCH)" -o -z "$(_BUILD)" -o -z "$(_CLASS)" -o -z "$(_GDB)" ]; then \
	    echo "Usage: make debug-test <arch> <build> <class> <gdb-script>"; \
	    echo "  arch:  x86|x86_64|arm|aarch64|riscv|riscv64"; \
	    echo "  build: debug|release"; \
	    echo "  class: none|selftest|all|<name>"; \
	    echo "  gdb-script: path to GDB batch script (e.g. tools/gdb/test-batch.gdb)"; \
	    exit 1; \
	fi; \
	case "$(_BUILD)" in debug|release) ;; *) echo "ERROR: build must be 'debug' or 'release'"; exit 1;; esac; \
	if [ ! -f "$(_GDB)" ]; then \
	    echo "ERROR: GDB script not found: $(_GDB)"; exit 1; \
	fi; \
	if ! command -v gtimeout >/dev/null 2>&1; then \
	    echo "gtimeout missing (install coreutils)."; exit 1; \
	fi; \
	mkdir -p initrd/tests; \
	if [ "$(_CLASS)" != "none" ]; then \
	    printf '%s\n' "$(_CLASS)" > initrd/tests/test-config.txt; \
	else \
	    printf 'none\n' > initrd/tests/test-config.txt; \
	fi; \
	$(MAKE) $(_BUILD) NO_LTO=1 ARCH=$(_ARCH)
	@true $(if $(filter release,$(_BUILD)),$(eval DEBUG_ISO := $(RELEASE_ISO)))
	rm -f build/gdb-panic-captured build/gdb-serial.log; \
	printf '  %-7s %s\n' 'QEMU' 'Starting with GDB stub (:1234)…'; \
	$(QEMU_SYSTEM) $(QEMU_FLAGS) -s -S -serial file:build/gdb-serial.log -display none -no-reboot $(QEMU_DEBUG_EXIT) & \
	QEMU_PID=$$!; \
	echo "Waiting for QEMU GDB stub (:1234)…"; \
	for i in 1 2 3 4 5 6 7 8; do \
	    nc -z localhost 1234 2>/dev/null && break; \
	    sleep 1; \
	done; \
	printf '  %-7s %s\n' 'GDB' 'Running $(_GDB)…'; \
	gtimeout 120 $(GDB) build/kernel-debug.elf -batch -x $(_GDB); \
	GDB_EXIT=$$?; \
	kill $$QEMU_PID 2>/dev/null; wait $$QEMU_PID 2>/dev/null; \
	if [ -f build/gdb-panic-captured ]; then \
	    echo ""; \
	    echo "!!! KERNEL PANIC CAPTURED !!!"; \
	    echo "See GDB output above."; \
	    rm -f build/gdb-panic-captured; \
	    exit 1; \
	elif [ $$GDB_EXIT -eq 124 ]; then \
	    echo ""; \
	    printf '  %-7s %s\n' 'GDB' 'No panic within 120s (clean boot to shell)'; \
	elif [ $$GDB_EXIT -eq 0 ]; then \
	    echo ""; \
	    printf '  %-7s %s\n' 'GDB' 'No panic (QEMU exited cleanly)'; \
	else \
	    echo ""; \
	    echo "=== debug-test: GDB exit code $$GDB_EXIT (log: build/gdb-serial.log) ==="; \
	    exit $$GDB_EXIT; \
	fi

# ------------------------------------------------------------------------------
# Deterministic replay (QEMU -icount + record/replay for race debugging)
#
# Usage:
#   make rr-record    — boots the debug ISO and records execution to build/rr.log
#   make rr-replay    — replays the recorded session with GDB stub on :1234
#
# During replay, connect GDB and set breakpoints before the race window:
#   $(GDB) build/kernel-debug.elf -ex 'target remote :1234'
# The deterministic -icount ensures the same instruction sequence every replay.
# ------------------------------------------------------------------------------
rr-record: NO_LTO := 1
rr-record: debug
	@printf '  %-7s %s\n' 'RR' 'Recording deterministic trace…'
	@mkdir -p build
	$(QEMU_SYSTEM) $(QEMU_FLAGS) -icount shift=auto,rr=record,rrfile=build/rr.log -display none

rr-replay:
	@if [ ! -f build/rr.log ]; then \
	    echo "No recording found — run 'make rr-record' first."; exit 1; \
	fi
	@printf '  %-7s %s\n' 'RR' 'Replaying with GDB stub on :1234…'
	@echo "Connect GDB:  $(GDB) build/kernel-debug.elf -ex 'target remote :1234'"
	$(QEMU_SYSTEM) $(QEMU_FLAGS) -icount shift=auto,rr=replay,rrfile=build/rr.log -s -S -display none

# ------------------------------------------------------------------------------
# Renode targets
# ------------------------------------------------------------------------------
RENODE_ARCH ?= x86_64
RENODE_SUPPORTED_ARCHS := x86_64 aarch64 riscv64

run-renode: debug
	@if ! command -v renode >/dev/null 2>&1; then \
	    echo "Renode missing. Install with: brew install renode/tap/renode"; exit 1; \
	fi
	@if [ "$(filter $(RENODE_ARCH),$(RENODE_SUPPORTED_ARCHS))" != "$(RENODE_ARCH)" ]; then \
	    echo "Unsupported Renode arch '$(RENODE_ARCH)'. Supported: $(RENODE_SUPPORTED_ARCHS)"; exit 1; \
	fi
	@printf '  %-7s %s\n' 'RENODE' 'Starting Jarvis RTOS ($(RENODE_ARCH))…'
	@printf '  %-7s %s\n' 'SCRIPT' 'tools/renode/jarvis-$(RENODE_ARCH).resc'
	renode --disable-xwt -e "i @tools/renode/jarvis-$(RENODE_ARCH).resc; s"

RENODE_TEST_TIMEOUT ?= 300

.PHONY: renode-test
renode-test:
	@if ! command -v renode >/dev/null 2>&1; then \
	    echo "Renode missing. Install with: brew install renode/tap/renode"; exit 1; \
	fi
	@if [ "$(filter $(RENODE_ARCH),$(RENODE_SUPPORTED_ARCHS))" != "$(RENODE_ARCH)" ]; then \
	    echo "Unsupported Renode arch '$(RENODE_ARCH)'. Supported: $(RENODE_SUPPORTED_ARCHS)"; exit 1; \
	fi
	@mkdir -p initrd/tests
	@printf 'safe\n' > initrd/tests/test-config.txt
	$(MAKE) debug ARCH=$(RENODE_ARCH)
	@printf '  %-7s %s\n' 'RENODE' 'Running tests in Renode ($(RENODE_ARCH))…'
	renode-test tools/renode/test_renode_$(RENODE_ARCH).py \
	    --test-timeout $(RENODE_TEST_TIMEOUT) \
	    -r build/renode-results-$(RENODE_ARCH)
	@printf '  %-7s %s\n' 'RENODE' 'Tests passed.'

# ------------------------------------------------------------------------------
# Test targets
# ------------------------------------------------------------------------------

# Shared expect helper for autonomous test runs.
# Single consistent serial log path for all automated test runs.
TEST_SERIAL_LOG := /tmp/jarvis-serial.log
# Verdict file: the expect script writes its machine-readable verdict here so it
# is never lost to pipe buffering. The Makefile reads it back to set the exit code.
TEST_VERDICT_LOG := /tmp/jarvis-verdict.log

# Timing model (single source of truth, correctly scaled):
#   - expect waits up to TEST_TIMEOUT_* for the kernel's TEST SUMMARY.
#   - the host stall-watchdog fires EARLIER (WATCHDOG_STALL) so it catches a
#     frozen-but-still-emitting serial stream before expect's coarser timeout.
#   WATCHDOG_STALL must be strictly less than the longest expect timeout (250),
#   else the watchdog can never fire before expect gives up. 30s margin used.
TEST_TIMEOUT_ALL    := 250
TEST_TIMEOUT_CLASS  := 120
WATCHDOG_STALL      := 220

# Usage: $(call _run_test_qemu,<description>,<timeout_sec>)
# Robust design:
#   - The expect driver (tools/run-test.exp) writes its machine-readable VERDICT
#     to $(TEST_VERDICT_LOG); the full serial stream is teed to $(TEST_SERIAL_LOG).
#     Writing the verdict to a dedicated file (not just stdout/pipe) guarantees it
#     is never lost to pipe buffering or a killed tee.
#   - After expect returns, the Makefile reads $(TEST_VERDICT_LOG) and replays it
#     to stdout, then sets the exit code from it.
#   - The expect logic lives in a committed .exp file (not an inline -c string) so
#     TCL quoting/backslash-escaping is correct and maintainable.
define _run_test_qemu
	if ! command -v $(QEMU_SYSTEM) >/dev/null 2>&1; then echo "QEMU missing."; echo $(PKG_HINT); exit 1; fi; \
	if ! command -v expect >/dev/null 2>&1; then echo "expect missing."; exit 1; fi; \
	printf '  %-7s %s\n'  'TEST'   '$(1)…'; \
	rm -f "$(TEST_SERIAL_LOG)" "$(TEST_VERDICT_LOG)" /tmp/test-start.timestamp /tmp/test-end.timestamp; \
	date +%s > /tmp/test-start.timestamp; \
	\
	( \
	    trap "exit 0" TERM; \
	    LAST_SIZE=-1; \
	    STALL=0; \
	    while true; do \
	        sleep 1; \
	        CUR_SIZE=$$(wc -c < "$(TEST_SERIAL_LOG)" 2>/dev/null || echo 0); \
	        if [ "$$CUR_SIZE" = "$$LAST_SIZE" ]; then \
	            STALL=$$((STALL + 1)); \
	            if [ $$STALL -ge $(WATCHDOG_STALL) ]; then \
	                echo "[HOST-WATCHDOG] Tests interrupted — no serial output for $(WATCHDOG_STALL) s" | tee -a "$(TEST_SERIAL_LOG)" "$(TEST_VERDICT_LOG)"; \
	                pkill -f "$(QEMU_SYSTEM).*nexios-rtos" 2>/dev/null; \
	                exit 1; \
	            fi; \
	        else \
	            STALL=0; \
	            LAST_SIZE=$$CUR_SIZE; \
	        fi; \
	    done; \
	) & \
	MONITOR_PID=$$!; \
	\
	expect tools/run-test.exp $(2) "$(TEST_VERDICT_LOG)" "$(TEST_SERIAL_LOG)" \
	    $(QEMU_SYSTEM) $(QEMU_FLAGS) -display none -no-reboot $(QEMU_DEBUG_EXIT) \
	    2>&1 | gstdbuf -oL tee "$(TEST_SERIAL_LOG)"; \
	date +%s > /tmp/test-end.timestamp; \
	\
	pkill -f "$(QEMU_SYSTEM).*nexios-rtos" 2>/dev/null || true; \
	kill $$MONITOR_PID 2>/dev/null; \
	wait $$MONITOR_PID 2>/dev/null; \
	\
	if [ -f "$(TEST_VERDICT_LOG)" ]; then \
	    VERDICT=$$(cat "$(TEST_VERDICT_LOG)"); \
	else \
	    VERDICT="RESULT: UNKNOWN (no verdict file — expect aborted)"; \
	fi; \
	echo "$$VERDICT"; \
	START_S=$$(cat /tmp/test-start.timestamp 2>/dev/null || echo 0); \
	END_S=$$(cat /tmp/test-end.timestamp 2>/dev/null || echo 0); \
	echo "[HOST] start=$${START_S}s end=$${END_S}s elapsed=$$(( END_S - START_S ))s"; \
	case "$$VERDICT" in \
	    RESULT:\ PASS*) \
	        echo "[HOST] TESTS PASSED"; \
	        ;; \
	    *) \
	        echo "[HOST] TESTS FAILED — see $(TEST_SERIAL_LOG)"; \
	        echo "VERDICT: $$VERDICT"; \
	        exit 1; \
	        ;; \
	esac
endef

# Usage: $(call _run_dump_counts_qemu)
# Runs QEMU, captures all TCOUNT lines to stdout, exits cleanly.
DUMP_COUNTS_LOG := /tmp/jarvis-dump-counts.log
define _run_dump_counts_qemu
	if ! command -v $(QEMU_SYSTEM) >/dev/null 2>&1; then echo "QEMU missing."; echo $(PKG_HINT); exit 1; fi; \
	printf '  %-7s %s\n'  'TCOUNT' 'Dumping per-class registration counts…'; \
	rm -f "$(DUMP_COUNTS_LOG)"; \
	_DQ_FLAGS=$$(echo "$(QEMU_FLAGS)" | sed 's|-serial mon:stdio|-serial file:$(DUMP_COUNTS_LOG)|'); \
	$(QEMU_SYSTEM) $$_DQ_FLAGS -display none -no-reboot $(QEMU_DEBUG_EXIT) 2>&1 & \
	QEMU_PID=$$!; \
	sleep 30; \
	kill $$QEMU_PID 2>/dev/null; wait $$QEMU_PID 2>/dev/null || true; \
	echo "=== Per-class test registration counts ==="; \
	grep '\[TCOUNT\]' "$(DUMP_COUNTS_LOG)" || echo "(no TCOUNT lines found)"; \
	rm -f "$(DUMP_COUNTS_LOG)"
endef

# ------------------------------------------------------------------------------
# Debug shell: GDB + serial interaction from a shell-commands text file
# ------------------------------------------------------------------------------

debug-shell:
	@: $(eval _ae := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))) $(eval _ARCH := $(call _map_arch,$(word 1,$(_ae)))) $(eval _BUILD := $(word 2,$(_ae))) $(eval _CLASS := $(word 3,$(_ae))) $(eval _GDB := $(word 4,$(_ae))) $(eval _CMDS := $(word 5,$(_ae)))
	@if [ -z "$(_ARCH)" -o -z "$(_BUILD)" -o -z "$(_CLASS)" -o -z "$(_GDB)" -o -z "$(_CMDS)" ]; then \
	    echo "Usage: make debug-shell <arch> <build> none <gdb-script> <shell-commands>"; \
	    echo "  arch:  x86|x86_64|arm|aarch64|riscv|riscv64"; \
	    echo "  build: debug|release"; \
	    echo "  class: must be 'none' (debug-shell requires interactive boot)"; \
	    echo "  gdb-script: path to GDB batch script (e.g. tools/gdb/init.gdb)"; \
	    echo "  shell-commands: text file with one command per line"; \
	    exit 1; \
	fi; \
	case "$(_BUILD)" in debug|release) ;; *) echo "ERROR: build must be 'debug' or 'release'"; exit 1;; esac; \
	if [ "$(_CLASS)" != "none" ]; then \
	    echo "ERROR: debug-shell requires class=none"; exit 1; \
	fi; \
	if [ ! -f "$(_GDB)" ]; then echo "ERROR: GDB script not found: $(_GDB)"; exit 1; fi; \
	if [ ! -f "$(_CMDS)" ]; then echo "ERROR: shell commands file not found: $(_CMDS)"; exit 1; fi; \
	$(MAKE) $(_BUILD) NO_LTO=1 ARCH=$(_ARCH)
	@true $(if $(filter release,$(_BUILD)),$(eval DEBUG_ISO := $(RELEASE_ISO)))
	printf 'none\n' > initrd/tests/test-config.txt; \
	printf '  %-7s %s\n' 'SHELL' 'Starting debug session ($(_CMDS))…'; \
	expect -c ' \
	    fconfigure stdout -buffering none; \
	    set timeout 120; \
	    spawn $(QEMU_SYSTEM) $(QEMU_FLAGS) -s -S -display none -no-reboot $(QEMU_DEBUG_EXIT); \
	    for {set i 0} {$$i < 8} {incr i} { \
	        if {[catch {exec nc -z localhost 1234}] == 0} { break }; \
	        after 1000; \
	    }; \
	    exec $(GDB) build/kernel-debug.elf -batch -x $(_GDB) &; \
	    expect { \
	        -re {jarvis\$$ } { } \
	        timeout { puts "TIMEOUT: shell prompt not reached"; exit 1 } \
	    }; \
	    set f [open $(_CMDS) r]; \
	    while {[gets $$f line] >= 0} { \
	        puts ">>> $$line"; \
	        send "$$line\r"; \
	        expect { \
	            -re {jarvis\$$ } { } \
	            timeout { puts "TIMEOUT after: $$line"; exit 1 } \
	        }; \
	    }; \
	    close $$f; \
	    puts "=== DEBUG SHELL COMPLETE ==="; \
	    exit 0; \
	'; \
	rc=$$?; \
	pkill -f "$(QEMU_SYSTEM).*nexios-rtos" 2>/dev/null || true; \
	exit $$rc

test: $(KERNEL)
	@echo "=========================================="
	@echo " Launching Kernel Internal Self-Tests..."
	@echo "=========================================="
	@echo "Validating Userspace Core Integrity..."
	$(MAKE) $(USERSPACE_ELF)
	@echo "Userspace components compiled securely."
	@echo "Trigger interactive tests at terminal context via 'selftest'."

# ------------------------------------------------------------------------------
# Utility targets
# ------------------------------------------------------------------------------
symbols: $(KERNEL)
	@printf '  %-7s %s\n' 'SYMBOLS' '$(KERNEL_SYMBOLS)'
	nm -n $(KERNEL) > $(KERNEL_SYMBOLS)

objdump: $(KERNEL)
	@printf '  %-7s %s\n' 'OBJDUMP' '$(KERNEL_DIS)'
	$(OBJDUMP) -d $(OBJDUMP_DIS_FLAGS) --no-show-raw-insn $(KERNEL) > $(KERNEL_DIS)

clean:
	@printf '  %-7s %s\n' 'CLEAN' 'Removing build artifacts…'
	rm -rf build $(DEBUG_ISO) $(RELEASE_ISO) iso/boot/kernel.elf $(INITRD_CPIO) $(LIBC_A)
	rm -f $(USERSPACE_ELF)
	rm -f $(BUILD_STAMP)
	rm -rf initrd_root debug release profiling
	rm -f $(FAT32_DISK)
	rm -f *.d
	rm -f $(TEST_REGISTRY_GEN) $(TEST_FILE_LIST)
	rm -rf build/external

# ------------------------------------------------------------------------------
# clang-tidy static analysis (debug builds only)
# ------------------------------------------------------------------------------
CLANG_TIDY_FILTER := src/kernel/test/%.cpp src/lib/test.cpp src/lib/compiler_rt.cpp src/lib/cxxabi.cpp src/lib/new.cpp
ifneq ($(EXTERNAL_TEST_DIR),)
CLANG_TIDY_FILTER += $(EXTERNAL_TEST_DIR)/src/%.cpp
endif
CLANG_TIDY_SRCS := $(filter-out $(CLANG_TIDY_FILTER), $(SRC_CXX_GENERIC) $(SRC_CXX_ARCH))
CLANG_TIDY_EXCL := $(addprefix --extra-arg=-I,src src/kernel src/lib)
CLANG_TIDY_DEFS := --extra-arg=-DCONFIG_ARCH_$(ARCH_UPPER) --extra-arg=-DCONFIG_DEBUG

.PHONY: clang-tidy
clang-tidy:
	@if command -v $(CLANG_TIDY) >/dev/null 2>&1; then \
		printf '  %-7s %s\n' 'TIDY' 'clang-tidy (bugprone,concurrency,performance)...';\
		$(CLANG_TIDY) --checks=$(CLANG_TIDY_CHECKS) --quiet \
			--extra-arg=-std=c++20 \
			--extra-arg=-ffreestanding --extra-arg=-fno-exceptions \
			--extra-arg=-fno-rtti --extra-arg=-fno-builtin \
			--extra-arg=-fno-stack-protector --extra-arg=-fno-threadsafe-statics \
			$(CLANG_TIDY_EXCL) $(CLANG_TIDY_DEFS) \
			$(CLANG_TIDY_SRCS) 2>&1 || true; \
		printf '  %-7s %s\n' 'TIDY' 'Done'; \
	else \
		printf '  %-7s %s\n' 'TIDY' 'clang-tidy not found, skipping'; \
	fi

# ------------------------------------------------------------------------------
# Module audit: cross-reference a .hpp/.cpp pair for consistency
# Example: make audit HPP=src/kernel/task/scheduler.hpp CPP=src/kernel/task/scheduler.cpp
# ------------------------------------------------------------------------------
.PHONY: audit
audit:
	@if [ -z "$(HPP)" ] || [ -z "$(CPP)" ]; then \
		echo "Error: Please specify HPP and CPP files. Example: make audit HPP=kernel/include/ready_queue.hpp CPP=kernel/src/ready_queue.cpp"; \
		exit 1; \
	fi
	@if [ -z "$$OPENROUTER_API_KEY" ]; then \
		echo "Error: OPENROUTER_API_KEY environment variable is not set."; \
		exit 1; \
	fi
	@python3 tools/audit_module.py $(HPP) $(CPP)

# ------------------------------------------------------------------------------
# Stack usage report
# ------------------------------------------------------------------------------
.PHONY: stack-usage-report
stack-usage-report:
	@python3 tools/check-stack-usage.py || true

# ------------------------------------------------------------------------------
# Match-all rule: silently consume positional arguments for unified targets
# (e.g. `make execute-test x86_64 debug memory`).  Must be NON-TERMINAL (`%:`,
# not `%::`): a terminal `%::` match-anything rule shadows every non-terminal
# pattern rule (including `build/%.o: src/%.cpp`), turning C++ compiles into
# no-ops.  As a non-terminal rule it only catches goals with no real rule
# (the positional args) and loses to specific pattern rules for real targets.
# Keep it last so it never precedes real targets.
# ------------------------------------------------------------------------------
%:
	@true
