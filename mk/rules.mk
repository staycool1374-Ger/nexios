# ==============================================================================
# Shared build rules — included by top-level Makefile
#
# Paths in this file are relative to the including Makefile (project root).
# Variables set by the parent before include (CXXFLAGS, CCFLAGS, ASFLAGS,
# LDFLAGS, KERNEL, KERNEL_DEBUG, ...) are available here.
# ==============================================================================
comma := ,

# ------------------------------------------------------------------------------
# Source discovery & object file mapping
#
# Generic sources (outside arch/) + per-ARCH sources under arch/$(ARCH)/
# ------------------------------------------------------------------------------
SRC_CXX_GENERIC := $(shell find src -path '*/kernel/arch' -prune -o -name '*.cpp' -print)
SRC_ASM_GENERIC := $(shell find src -path '*/kernel/arch' -prune -o -name '*.asm' -print)
# P7 (issue #6): syscall_entry.asm is DEAD on all arches — the LSTAR/sysret
# path was removed (MSR_KERNEL_GS_BASE never written; the sole live syscall
# path is int $0x80 via isr_128).  Unassemble it unconditionally; it is kept
# as a commented historical artifact pending deletion (see syscall_entry.asm).
SRC_ASM_GENERIC := $(filter-out src/kernel/syscall/syscall_entry.asm, $(SRC_ASM_GENERIC))
# Exclude x86-specific NASM files for non-x86 architectures
ifneq ($(ARCH),x86_64)
SRC_ASM_GENERIC := $(filter-out src/kernel/arch/%, $(SRC_ASM_GENERIC))
endif
# Exclude FPU/SSE test files that fail to compile with GCC 16 on all arches
# (they are x86_64 SSE-specific and not portable; register symbols are covered
# by weak stubs in test_weak_stubs.cpp).
SRC_CXX_GENERIC := $(filter-out src/kernel/test/test_fpu.cpp src/kernel/test/test_fpu_clone.cpp src/kernel/test/test_fpu_multi.cpp src/kernel/test/test_fpu_sse.cpp src/kernel/test/test_fpu_xmm_all.cpp, $(SRC_CXX_GENERIC))

# External test suite support
# When EXTERNAL_TEST_DIR is set:
#   1. Only selftest (safe-class) files from src/kernel/test/ are compiled
#   2. External test sources from $(EXTERNAL_TEST_DIR) provide the rest
#   3. External include paths are added
ifneq ($(EXTERNAL_TEST_DIR),)
# Selftest infrastructure — always needed
SELFTEST_INFRA := src/kernel/test/test_registry.cpp \
                  src/kernel/test/test_isolate.cpp \
                  src/kernel/test/test_cleanup.cpp \
                  src/kernel/test/test_config.cpp \
                  src/kernel/test/resource_tracker.cpp \
                  src/kernel/test/test_watchdog.cpp \
                  src/kernel/test/test_weak_stubs.cpp
# Safe-class test files — compiled into every kernel
SELFTEST_TESTS := src/kernel/test/test_lib.cpp \
                  src/kernel/test/test_checked_ptr.cpp \
                  src/kernel/test/test_block_device.cpp \
                  src/kernel/test/test_fat32.cpp \
                  src/kernel/test/test_vfs_fat32.cpp \
                  src/kernel/test/test_waitpid.cpp \
                  src/kernel/test/test_shell_interaction.cpp \
                  src/kernel/test/test_hal_bits.cpp \
                  src/kernel/test/test_o1_scheduler.cpp \
                  src/kernel/test/test_zombie_cleanup.cpp \
                  src/kernel/test/test_wcet_cleanup.cpp \
                  src/kernel/test/test_idle_cleanup.cpp \
                  src/kernel/test/test_apic_timer.cpp \
                  src/kernel/test/test_lapic.cpp \
                  src/kernel/test/test_ioapic.cpp \
                  src/kernel/test/test_core_isolation.cpp \
                  src/kernel/test/test_load_balancer.cpp \
                  src/kernel/test/test_cache_coloring.cpp \
                  src/kernel/test/test_smp_sync.cpp \
                  src/kernel/test/test_smp_verify.cpp \
                  src/kernel/test/test_pcid.cpp \
                  src/kernel/test/test_invpcid.cpp \
                  src/kernel/test/test_lazy_tlb.cpp \
                  src/kernel/test/test_ipi_batching.cpp \
                  src/kernel/test/test_tlb_latency.cpp \
                  src/kernel/test/test_pml4_sync.cpp \
                  src/kernel/test/test_irq_alloc.cpp \
                  src/kernel/test/test_jitter.cpp \
                  src/kernel/test/test_threaded_irqs.cpp \
                  src/kernel/test/test_gic.cpp \
                   src/kernel/test/test_plic.cpp \
                   src/kernel/test/test_freelist_consistency.cpp
# Keep all non-test sources, but replace everything under src/kernel/test/
# with only the selftest subset
SRC_CXX_GENERIC := $(filter-out src/kernel/test/%.cpp, $(SRC_CXX_GENERIC))
SRC_CXX_GENERIC += $(SELFTEST_INFRA) $(SELFTEST_TESTS)
# Also exclude arch-specific test files (not selftest infrastructure)
SRC_CXX_ARCH := $(filter-out %test_aarch64.cpp %test_riscv64.cpp, $(SRC_CXX_ARCH))
# Discover external test sources
EXTERNAL_SRC := $(shell find $(EXTERNAL_TEST_DIR)/src -name '*.cpp' 2>/dev/null)
# Map external source paths to build/external/ object files
EXTERNAL_OBJ := $(patsubst $(EXTERNAL_TEST_DIR)/src/%.cpp,build/external/%.o,$(EXTERNAL_SRC))
# Add include paths for external test headers
CXXFLAGS += -I$(EXTERNAL_TEST_DIR)/include
endif
SRC_S_GENERIC   := $(shell find src -path '*/kernel/arch' -prune -o -path '*/libc' -prune -o -name '*.S' -print)

SRC_CXX_ARCH    := $(shell find src/kernel/arch/$(ARCH) -name '*.cpp' 2>/dev/null)
SRC_ASM_ARCH    := $(shell find src/kernel/arch/$(ARCH) -name '*.asm' 2>/dev/null)
SRC_S_ARCH      := $(shell find src/kernel/arch/$(ARCH) -name '*.S' 2>/dev/null)

SRC_CXX_ARCH_BASE := $(shell find src/kernel/arch -maxdepth 1 -name '*.cpp' 2>/dev/null)
# $(strip) is required: GNU Make's $(shell find ...) can leave a leading space
# on the result, which would propagate into every generated object-path stem
# (e.g. ` build/lib/logger.o`) and produce invalid rules that silently no-op.
SRC_CXX         := $(strip $(SRC_CXX_GENERIC) $(SRC_CXX_ARCH_BASE) $(SRC_CXX_ARCH))
SRC_ASM         := $(SRC_ASM_GENERIC) $(SRC_ASM_ARCH)
SRC_S           := $(SRC_S_GENERIC) $(SRC_S_ARCH)
OBJ             := $(SRC_CXX:src/%.cpp=build/%.o) $(SRC_ASM:src/%.asm=build/%.o) $(SRC_S:src/%.S=build/%.o)
ifneq ($(EXTERNAL_TEST_DIR),)
OBJ             += $(EXTERNAL_OBJ)
endif
DEPFILES        := $(shell find build -name '*.d' 2>/dev/null)
-include $(DEPFILES)

# ------------------------------------------------------------------------------
# Libc (shared artifact, same .o files for both builds)
# ------------------------------------------------------------------------------
LIBC_DIR       := src/libc
LIBC_SRC       := $(shell find $(LIBC_DIR) -name '*.c' -o -name '*.S')
LIBC_OBJ       := $(patsubst src/%.c,build/%.o,$(filter %.c,$(LIBC_SRC))) \
                  $(patsubst src/%.S,build/%.o,$(filter %.S,$(LIBC_SRC)))
LIBC_A         := build/libc/libc.a

USERSPACE_SRC  := $(filter-out userspace/picolibc/%,$(shell find userspace -name '*.S' -o -name '*.c' 2>/dev/null))
USERSPACE_ELF  := $(USERSPACE_SRC:%=%.elf)

# picolibc-based user programs (issue #73): separate directory-anchored
# pattern so the generic userspace/%.c.elf rule above can never hijack
# them (silent wrong-libc binary). Built on demand, not in USERSPACE_ELF.
PICOLIBC_SRC   := $(filter-out %/nexios_glue.c,$(shell find userspace/picolibc -name '*.c' 2>/dev/null))
PICOLIBC_ELF   := $(PICOLIBC_SRC:%=%.elf)
# NexIOS libos glue (kernel-ABI _exit for picolibc's exit.c). Compiled
# with src/libc headers (NexIOS-specific); hosted programs see the
# sysroot headers only.
PICOLIBC_GLUE  := userspace/picolibc/nexios_glue.o

INITRD_CPIO    := build/initrd.cpio
INITRD_OBJ     := build/initrd/initrd_cpio.o

FAT32_IMG      := build/fat32.img
FAT32_OBJ      := build/fat32/fat32_img.o

# ------------------------------------------------------------------------------
# AP trampoline blob (issue #25, Phase B3, x86_64 only)
# ------------------------------------------------------------------------------
# The AP startup code must execute at physical 0x7000 in 16-bit real mode,
# so it is assembled position-locked (nasm -f bin, org 0x7000) and embedded
# as a raw binary object (objcopy -I binary pattern, like the initrd).  The
# .nasm extension keeps it out of the generic *.asm ELF rule above.
ifeq ($(ARCH),x86_64)
AP_TRAMPOLINE_SRC := src/kernel/arch/x86_64/boot/ap_trampoline.nasm
AP_TRAMPOLINE_BIN := build/ap_trampoline.bin
AP_TRAMPOLINE_OBJ := build/arch/ap_trampoline.o
EXTRA_LINK_OBJ    := $(AP_TRAMPOLINE_OBJ)

$(AP_TRAMPOLINE_BIN): $(AP_TRAMPOLINE_SRC)
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'NASM' '$@'
	$(AS) -f bin -o $@ $<

$(AP_TRAMPOLINE_OBJ): $(AP_TRAMPOLINE_BIN)
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'OBJCOPY' '$@'
	$(OBJCOPY) -I binary -O $(OBJCOPY_FMT) -B $(OBJCOPY_ARCH) \
	    --redefine-sym _binary_build_ap_trampoline_bin_start=_binary_ap_trampoline_start \
	    --redefine-sym _binary_build_ap_trampoline_bin_end=_binary_ap_trampoline_end \
	    --redefine-sym _binary_build_ap_trampoline_bin_size=_binary_ap_trampoline_size \
	    $< $@
else
EXTRA_LINK_OBJ    :=
endif

# ------------------------------------------------------------------------------
# Pattern rules
# ------------------------------------------------------------------------------
# NOTE: GNU Make 3.81 cannot resolve prerequisites for `.cpp.o` / `.c.o`
# double-extension targets via pattern rules (`build/%.o: src/%.cpp` treats
# `kernel.cpp.o` as a `.cpp.o` suffix chain and looks for `kernel.cpp.cpp`).
# It ALSO silently drops rules produced by `$(eval $(call …))` when the
# stem list is derived from `$(shell …)` (a make-version quirk).  The robust
# workaround is to emit explicit per-file rules into a generated makefile via
# a shell loop and `-include` it.  .asm/.S objects have single-extension
# names (`isr_stubs.o`) so their pattern rules are fine.
GEN_CPP_RULES := mk/cpp-rules.$(ARCH).gen.mk
-include $(GEN_CPP_RULES)

$(GEN_CPP_RULES): mk/rules.mk Makefile
	@mkdir -p $(dir $@)
	@printf '' > $@
	@for f in $(SRC_CXX); do \
	    stem=$${f#src/}; stem=$${stem%.cpp}; \
	    printf 'build/%s.o: %s | check-arch\n\t@mkdir -p $$(dir $$@)\n\t@printf "  %%s %%s\\n" CC $$@\n\t$$(CXX) $$(CXXFLAGS) -c -o $$@ $$<\n' "$$stem" "$$f" >> $@; \
	done


build/%.o: src/%.asm
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'AS' '$@'
	$(AS) $(ASFLAGS) -o $@ $<

build/%.o: src/%.S
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'AS' '$@'
	$(CC) $(CCFLAGS) -c -o $@ $<

ifneq ($(EXTERNAL_TEST_DIR),)
# Pattern rule for external test sources
build/external/%.o: $(EXTERNAL_TEST_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'CC' '$@'
	$(CXX) $(CXXFLAGS) -c -o $@ $<
endif

# ------------------------------------------------------------------------------
# Libc rules
# ------------------------------------------------------------------------------
build/libc/%.o: src/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCFLAGS) -I src/libc -c -o $@ $<

build/libc/%.o: src/libc/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CCFLAGS) -c -o $@ $<

$(LIBC_A): $(LIBC_OBJ)
	@printf '  %-7s %s\n' 'AR' 'libc.a'
	$(AR) rcs $@ $^

# ------------------------------------------------------------------------------
# Userspace ELFs
# ------------------------------------------------------------------------------
userspace/%.S.elf: userspace/%.S
	@printf '  %-7s %s\n' 'CC' '$@'
	$(CC) $(CCFLAGS) -o $@ $<

userspace/%.c.elf: userspace/%.c $(LIBC_A) build/libc/crt0.o
	@printf '  %-7s %s\n' 'CC' '$@'
	$(CC) $(CCFLAGS) -I src/libc -o $@ build/libc/crt0.o $< -L build/libc -lc

# picolibc-based programs (issue #73): static pattern rule (beats the
# generic implicit rule deterministically — first-match-wins applies).
# In-tree crt0.o first object, single _start (picocrt never linked;
# -nostartfiles via CCFLAGS -nostdlib, no specs, no toolchain crt
# files). Order-only sysroot edge: readiness gates the link but
# sysroot mtime never forces an ELF rebuild.
$(PICOLIBC_ELF): userspace/picolibc/%.c.elf: userspace/picolibc/%.c $(PICOLIBC_GLUE) build/libc/picolib_stubs.o | $(PICOLIBC_SYSROOT)/lib/libc.a $(PICOLIBC_SYSROOT)/lib/libm.a
	@printf '  %-7s %s\n' 'CC' '$@'
	$(CC) $(CCFLAGS) -I $(PICOLIBC_SYSROOT)/include -o $@ build/libc/crt0.o $< $(PICOLIBC_GLUE) build/libc/picolib_stubs.o -L $(PICOLIBC_SYSROOT)/lib -lc -lm

$(PICOLIBC_GLUE): userspace/picolibc/nexios_glue.c
	@printf '  %-7s %s\n' 'CC' '$@'
	$(CC) $(CCFLAGS) -I $(PICOLIBC_SYSROOT)/include -I src/libc -c -o $@ $<

# ------------------------------------------------------------------------------
# Initrd
# ------------------------------------------------------------------------------
# Default test selection baked into a plain `make debug`/`make release` ISO.
# Test flows (execute-test/debug-test) overwrite this file directly; this
# rule only fires on fresh checkouts (e.g. CI) where it does not exist yet.
initrd/tests/test-config.txt:
	@mkdir -p initrd/tests
	@printf 'none\n' > $@
$(INITRD_CPIO): $(USERSPACE_ELF) initrd/tests/test-config.txt
	@printf '  %-7s %s\n' 'CPIO' 'initrd.cpio'
	@mkdir -p initrd_root/etc initrd_root/tmp initrd_root/tests
	@printf 'tmpfs /tmp\n' > initrd_root/etc/fstab
	@printf '#!/bin/sh\n' > initrd_root/etc/rc
	@printf '# Init script\n' >> initrd_root/etc/rc
	@if [ ! -z "$(USERSPACE_ELF)" ]; then cp $(USERSPACE_ELF) initrd_root/; fi
	@if [ -f userspace/picolibc/libc_verify.c.elf ]; then cp userspace/picolibc/libc_verify.c.elf initrd_root/; fi
	cp initrd/tests/test-config.txt initrd_root/tests/test-config.txt
	cd initrd_root && find . -print0 | cpio -o -H newc -0 --quiet > ../$@
	@rm -rf initrd_root

$(INITRD_OBJ): $(INITRD_CPIO)
	@printf '  %-7s %s\n' 'OBJCOPY' '$@'
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O $(OBJCOPY_FMT) -B $(OBJCOPY_ARCH) \
	    --redefine-sym _binary_build_initrd_cpio_start=_binary_initrd_cpio_start \
	    --redefine-sym _binary_build_initrd_cpio_end=_binary_initrd_cpio_end \
	    --redefine-sym _binary_build_initrd_cpio_size=_binary_initrd_cpio_size \
	    $< $@

# ------------------------------------------------------------------------------
# FAT32 disk image
# ------------------------------------------------------------------------------
$(FAT32_IMG): tools/mkfat32img.py
	@printf '  %-7s %s\n' 'FATIMG' '$@'
	@mkdir -p $(dir $@)
	python3 tools/mkfat32img.py $@

$(FAT32_OBJ): $(FAT32_IMG)
	@printf '  %-7s %s\n' 'OBJCOPY' '$@'
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O $(OBJCOPY_FMT) -B $(OBJCOPY_ARCH) $< $@

# ------------------------------------------------------------------------------
# Kernel link rules
#
# Both debug and release use the same LDFLAGS and same .o files.  The
# difference in build type is driven entirely by CXXFLAGS on the .o files.
# ------------------------------------------------------------------------------
# .PHONY check that guarantees the arch stamp matches the target arch.
# The actual arch-switch clean is performed at PARSE TIME in the Makefile
# (before .d files are -included), so this target only persists the stamp.
# It runs as an order-only prerequisite of every emitted compile rule (see
# GEN_CPP_RULES above), which is now a cheap no-op safety net.
.PHONY: check-arch
check-arch:
	@mkdir -p $$(dirname $(ARCH_STAMP)); echo $(ARCH) > $(ARCH_STAMP)

# Issue #75: the libc_verify ELF is embedded for the kernel test (the
# initrd is not mounted in test boot, so the test cannot resolve it).
# The image is stripped first: tmpfs caps files at 64 KiB and the
# unstripped static binary is ~174 KiB (stripped ~32 KiB).
# x86_64 only — other archs have no picolibc sysroot.
ifeq ($(ARCH),x86_64)
VERIFY_IMG_OBJ := build/initrd/libc_verify_img.o
VERIFY_IMG_SRC := userspace/picolibc/libc_verify.c.elf
VERIFY_IMG_STRIPPED := build/initrd/libc_verify_stripped.elf
else
VERIFY_IMG_OBJ :=
VERIFY_IMG_SRC :=
VERIFY_IMG_STRIPPED :=
endif

$(VERIFY_IMG_STRIPPED): $(VERIFY_IMG_SRC)
	@mkdir -p $(dir $@)
	cp $< $@
	$(X86_64_TRIPLET)strip $@

$(VERIFY_IMG_OBJ): $(VERIFY_IMG_STRIPPED)
	@mkdir -p $(dir $@)
	$(OBJCOPY) -I binary -O $(OBJCOPY_FMT) -B $(OBJCOPY_ARCH) \
	    --redefine-sym _binary_build_initrd_libc_verify_stripped_elf_start=_binary_libc_verify_img_start \
	    --redefine-sym _binary_build_initrd_libc_verify_stripped_elf_end=_binary_libc_verify_img_end \
	    --redefine-sym _binary_build_initrd_libc_verify_stripped_elf_size=_binary_libc_verify_img_size \
	    $< $@

$(KERNEL_DEBUG): $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(VERIFY_IMG_OBJ) $(EXTRA_LINK_OBJ) check-arch linker/linker_$(ARCH).ld
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'LD' 'kernel-debug.elf'
	@$(if $(filter -flto,$(LDFLAGS)),$(CXX) $(subst -Map=,-Wl$(comma)-Map=,$(filter-out -m elf_x86_64,$(LDFLAGS))) -flto,$(LD) $(LDFLAGS)) -o $@ $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(VERIFY_IMG_OBJ) $(EXTRA_LINK_OBJ) $(LD_LIBS)
	@printf '  %-7s %s\n' 'CRC' 'Patching code CRC…'
	@python3 tools/patch_code_crc.py $@
	@printf '  %-7s %s\n' 'SIZE' "$$($(GET_SIZE) $@) bytes"

$(KERNEL): $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(VERIFY_IMG_OBJ) $(EXTRA_LINK_OBJ) check-arch linker/linker_$(ARCH).ld
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'LD' 'kernel.elf'
	@$(if $(filter -flto,$(LDFLAGS)),$(CXX) $(subst -Map=,-Wl$(comma)-Map=,$(filter-out -m elf_x86_64,$(LDFLAGS))) -flto,$(LD) $(LDFLAGS)) -o $@ $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(VERIFY_IMG_OBJ) $(EXTRA_LINK_OBJ) $(LD_LIBS)
	@printf '  %-7s %s\n' 'CRC' 'Patching code CRC…'
	@python3 tools/patch_code_crc.py $@
	@printf '  %-7s %s\n' 'SIZE' "$$($(GET_SIZE) $@) bytes"

# ------------------------------------------------------------------------------
# ISO boot helper
# ------------------------------------------------------------------------------
iso/boot:
	@mkdir -p $@
