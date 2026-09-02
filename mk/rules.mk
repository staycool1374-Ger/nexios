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

USERSPACE_SRC  := $(shell find userspace -name '*.S' -o -name '*.c' 2>/dev/null)
USERSPACE_ELF  := $(USERSPACE_SRC:%=%.elf)

INITRD_CPIO    := build/initrd.cpio
INITRD_OBJ     := build/initrd/initrd_cpio.o

FAT32_IMG      := build/fat32.img
FAT32_OBJ      := build/fat32/fat32_img.o

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

# ------------------------------------------------------------------------------
# Initrd
# ------------------------------------------------------------------------------
$(INITRD_CPIO): $(USERSPACE_ELF) initrd/tests/test-config.txt
	@printf '  %-7s %s\n' 'CPIO' 'initrd.cpio'
	@mkdir -p initrd_root/etc initrd_root/tmp initrd_root/tests
	@printf 'tmpfs /tmp\n' > initrd_root/etc/fstab
	@printf '#!/bin/sh\n' > initrd_root/etc/rc
	@printf '# Init script\n' >> initrd_root/etc/rc
	@if [ ! -z "$(USERSPACE_ELF)" ]; then cp $(USERSPACE_ELF) initrd_root/; fi
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

$(KERNEL_DEBUG): $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) check-arch linker/linker_$(ARCH).ld
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'LD' 'kernel-debug.elf'
	@$(if $(filter -flto,$(LDFLAGS)),$(CXX) $(subst -Map=,-Wl$(comma)-Map=,$(filter-out -m elf_x86_64,$(LDFLAGS))) -flto,$(LD) $(LDFLAGS)) -o $@ $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(LD_LIBS)
	@printf '  %-7s %s\n' 'CRC' 'Patching code CRC…'
	@python3 tools/patch_code_crc.py $@
	@printf '  %-7s %s\n' 'SIZE' "$$($(GET_SIZE) $@) bytes"

$(KERNEL): $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) check-arch linker/linker_$(ARCH).ld
	@mkdir -p $(dir $@)
	@printf '  %-7s %s\n' 'LD' 'kernel.elf'
	@$(if $(filter -flto,$(LDFLAGS)),$(CXX) $(subst -Map=,-Wl$(comma)-Map=,$(filter-out -m elf_x86_64,$(LDFLAGS))) -flto,$(LD) $(LDFLAGS)) -o $@ $(OBJ) $(INITRD_OBJ) $(FAT32_OBJ) $(LD_LIBS)
	@printf '  %-7s %s\n' 'CRC' 'Patching code CRC…'
	@python3 tools/patch_code_crc.py $@
	@printf '  %-7s %s\n' 'SIZE' "$$($(GET_SIZE) $@) bytes"

# ------------------------------------------------------------------------------
# ISO boot helper
# ------------------------------------------------------------------------------
iso/boot:
	@mkdir -p $@
