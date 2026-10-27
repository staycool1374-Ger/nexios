# Kernel link: full build (issue #170 Phase 3).
#
# Mirrors mk/rules.mk link inputs byte-for-byte: same TUs (find + arch
# pruning + exclusions, see test_source_set.py), same per-TU flags
# (add_project_arguments in the root meson.build), same link args
# (LDFLAGS mirror per arch), same post-link tail (postlink.py).
#
# Assembly: .S via the C compiler driver (like the Makefile `%.S` rule);
# x86_64-only .asm via nasm (like the `%.asm` rule); syscall_entry.asm
# is DEAD on all arches and stays unassembled.

# (Toolchain programs, flags, and kernel_includes come from the root
# meson.build and are visible here: py3, triplet, cc_prog, objcopy_prog,
# arch_readelf, arch_strip, arch_objdump, nasm_prog, common_flags,
# arch_flags, build_flags, kernel_includes.)
src_root = meson.project_source_root() / 'src'

# --- Test sources: discovery + registry codegen (Phase 2) ---
test_src_dir = src_root / 'kernel' / 'test'
test_find = run_command('find', test_src_dir, '-name', '*.cpp',
  check : true)
test_src_list = []
foreach f : test_find.stdout().strip().split('\n')
  # The registry scans the COMPILED source set: the 5 FPU/SSE TUs are
  # excluded here exactly as in generic_srcs below (mk/rules.mk:26-29 —
  # they fail to compile with GCC 16 on all arches) and in
  # test_source_set.py FPU_EXCLUDED. Scanning them registers tests with
  # no definitions: the non-LTO link retains .lrodata.generated_tests
  # unconditionally and fails (debug only links via LTO slimming).
  if f == '' or f.endswith('test_fpu.cpp') \
      or f.endswith('test_fpu_clone.cpp') \
      or f.endswith('test_fpu_multi.cpp') \
      or f.endswith('test_fpu_sse.cpp') \
      or f.endswith('test_fpu_xmm_all.cpp')
    continue
  endif
  if f != ''
    test_src_list += [f]
  endif
endforeach
test_src_files = files(test_src_list)

test_file_list = configure_file(
  input : files('../tools/meson/test-file-list.in'),
  output : '.test-file-list-' + arch,
  configuration : {'TEST_FILES' : '\n'.join(test_src_list)},
)

test_registry_gen = custom_target('test_registry_gen',
  input : test_src_files + [test_file_list],
  output : 'test_registry.gen.hpp',
  command : [py3, files('../tools/gen_test_registry.py'),
    test_src_dir, '@OUTPUT@',
    '--file-list', test_file_list,
    '--arch', arch])

# --- Test class baked into the image (Phase 4 harness) ---
test_config = custom_target('test_config',
  output : 'test-config.txt',
  command : [py3, files('../tools/meson/gen_test_config.py'),
    get_option('test_class'), '@OUTPUT@'],
  build_by_default : true)

# --- Kernel TU discovery (mirrors mk/rules.mk SRC_CXX_*) ---
generic_find = run_command('find', src_root,
  '-path', src_root / 'kernel' / 'arch', '-prune',
  '-o', '-name', '*.cpp', '-print',
  check : true)
generic_srcs = []
foreach s : generic_find.stdout().strip().split('\n')
  if s == '' or s.endswith('test_fpu.cpp') \
      or s.endswith('test_fpu_clone.cpp') \
      or s.endswith('test_fpu_multi.cpp') \
      or s.endswith('test_fpu_sse.cpp') \
      or s.endswith('test_fpu_xmm_all.cpp')
    continue
  endif
  if get_option('external_test_dir') == ''
    generic_srcs += [s]
  endif
endforeach

# External-test mode: only the selftest subset compiles in-tree
# (mirrors SELFTEST_INFRA/SELFTEST_TESTS in mk/rules.mk).
selftest_infra_rel = [
  'src/kernel/test/test_registry.cpp',
  'src/kernel/test/test_isolate.cpp',
  'src/kernel/test/test_cleanup.cpp',
  'src/kernel/test/test_config.cpp',
  'src/kernel/test/resource_tracker.cpp',
  'src/kernel/test/test_watchdog.cpp',
  'src/kernel/test/test_weak_stubs.cpp',
]
selftest_tests_rel = [
  'src/kernel/test/test_lib.cpp',
  'src/kernel/test/test_checked_ptr.cpp',
  'src/kernel/test/test_block_device.cpp',
  'src/kernel/test/test_fat32.cpp',
  'src/kernel/test/test_vfs_fat32.cpp',
  'src/kernel/test/test_waitpid.cpp',
  'src/kernel/test/test_shell_interaction.cpp',
  'src/kernel/test/test_hal_bits.cpp',
  'src/kernel/test/test_o1_scheduler.cpp',
  'src/kernel/test/test_zombie_cleanup.cpp',
  'src/kernel/test/test_wcet_cleanup.cpp',
  'src/kernel/test/test_idle_cleanup.cpp',
  'src/kernel/test/test_apic_timer.cpp',
  'src/kernel/test/test_lapic.cpp',
  'src/kernel/test/test_ioapic.cpp',
  'src/kernel/test/test_core_isolation.cpp',
  'src/kernel/test/test_load_balancer.cpp',
  'src/kernel/test/test_cache_coloring.cpp',
  'src/kernel/test/test_smp_sync.cpp',
]
if get_option('external_test_dir') != ''
  foreach f : selftest_infra_rel + selftest_tests_rel
    generic_srcs += [meson.project_source_root() / f]
  endforeach
endif

# Arch sources: base dir + arch subdir (mirrors SRC_CXX_ARCH_BASE/ARCH).
arch_base = run_command('find', src_root / 'kernel' / 'arch',
  '-maxdepth', '1', '-name', '*.cpp',
  check : true)
arch_sub = run_command('find', src_root / 'kernel' / 'arch' / arch,
  '-name', '*.cpp',
  check : true)
arch_srcs = []
foreach s : (arch_base.stdout().strip().split('\n')
             + arch_sub.stdout().strip().split('\n'))
  if s != ''
    arch_srcs += [s]
  endif
endforeach

# Arch assembly (.S via CC driver like the Make rule; .asm via nasm).
arch_s_find = run_command('find', src_root / 'kernel' / 'arch' / arch,
  '-name', '*.S',
  check : true)
arch_s = []
foreach s : arch_s_find.stdout().strip().split('\n')
  # virt_dtb.S has its own custom_target below (needs the trimmed-DTB
  # build-order edge); exclude it here to avoid a duplicate definition.
  if s != '' and not s.endswith('/virt_dtb.S')
    arch_s += [s]
  endif
endforeach
generic_s_find = run_command('find', src_root,
  '-path', src_root / 'kernel' / 'arch', '-prune',
  '-o', '-path', src_root / 'libc', '-prune',
  '-o', '-name', '*.S', '-print',
  check : true)
generic_s = []
foreach s : generic_s_find.stdout().strip().split('\n')
  if s != ''
    generic_s += [s]
  endif
endforeach
asm_inputs = []
if arch == 'x86_64'
  # x86_64-only .asm via nasm (mirrors the `%.asm` rule; AS=nasm -f elf64).
  # syscall_entry.asm is DEAD on all arches and stays unassembled.
  nasm_gen = generator(nasm_prog,
    output : '@BASENAME@.o',
    arguments : ['-f', 'elf64', '-o', '@OUTPUT@', '@INPUT@'])
  x86_asm_find = run_command('find', src_root / 'kernel' / 'arch' / arch,
    '-name', '*.asm',
    check : true)
  foreach s : x86_asm_find.stdout().strip().split('\n')
    if s != '' and not s.endswith('/syscall/syscall_entry.asm')
      asm_inputs += [nasm_gen.process(s)]
    endif
  endforeach
endif

kernel_includes = [kernel_includes, include_directories('.')]
# (adds the builddir for the generated test_registry.gen.hpp; the
# source-tree entries come from the root meson.build)

kernel_cpp = files(generic_srcs + arch_srcs)
kernel_asm_s = files(generic_s + arch_s)

# --- crt0 (in-tree userspace entry; userspace links consume it) ---
crt0_o = custom_target('crt0',
  input : files('../src/libc/crt0.S'),
  output : 'crt0.o',
  command : [cc_prog] + cc_base_flags + cc_arch_flags + cc_includes + cc_build_flags + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
)

# --- objcopy flavor per arch (mirrors OBJCOPY_FMT/OBJCOPY_ARCH) ---
if arch == 'x86_64'
  objcopy_fmt = 'elf64-x86-64'
  objcopy_arch = 'i386'
elif arch == 'aarch64'
  objcopy_fmt = 'elf64-littleaarch64'
  objcopy_arch = 'aarch64'
else
  objcopy_fmt = 'elf64-littleriscv'
  objcopy_arch = 'riscv'
endif

# --- Blob objects for the link (objcopy -I binary). ---------------------
# Commands use build-root-relative input paths (custom targets run with
# cwd=build root); objcopy's default symbols derive from that relative
# form (e.g. packaging/initrd.cpio -> _binary_packaging_initrd_cpio_*,
# verified via readelf on built objects). Rename TARGETS are the exact
# _binary_* names the kernel references. A wrong rename source fails
# LOUDLY at link time (undefined _binary_* symbol), never silently.

# Rename sources below use the build-root-relative command form
# ('packaging/<name>'); custom_target commands run with cwd=build root,
# so objcopy's default symbols derive from that relative form
# (verified via readelf on built objects: _binary_packaging_*).

blob_objs = []
initrd_cpio_o = custom_target('initrd_cpio.o',
  input : meson.project_build_root() / 'packaging' / 'initrd.cpio',
  output : 'initrd_cpio.o',
  command : [objcopy_prog, '-I', 'binary', '-O', objcopy_fmt, '-B', objcopy_arch,
    '--redefine-sym', '_binary_packaging_initrd_cpio_start=_binary_initrd_cpio_start',
    '--redefine-sym', '_binary_packaging_initrd_cpio_end=_binary_initrd_cpio_end',
    '--redefine-sym', '_binary_packaging_initrd_cpio_size=_binary_initrd_cpio_size',
    'packaging/initrd.cpio', '@OUTPUT@'],
)
blob_objs += [initrd_cpio_o]
fat32_img_o = custom_target('fat32_img.o',
  input : meson.project_build_root() / 'packaging' / 'fat32.img',
  output : 'fat32_img.o',
  command : [objcopy_prog, '-I', 'binary', '-O', objcopy_fmt, '-B', objcopy_arch,
    '--redefine-sym', '_binary_packaging_fat32_img_start=_binary_build_fat32_img_start',
    '--redefine-sym', '_binary_packaging_fat32_img_end=_binary_build_fat32_img_end',
    '--redefine-sym', '_binary_packaging_fat32_img_size=_binary_build_fat32_img_size',
    'packaging/fat32.img', '@OUTPUT@'],
)
blob_objs += [fat32_img_o]

# Embedded probe ELFs (stripped; mirrors VERIFY/ERRNO/FORK_IMG_OBJ).
# Stripped in packaging/; objcopied here with the kernel-visible names.
# The arch split mirrors mk/rules.mk exactly: verify+errno on x86_64
# only (Ring-3 TLS proof is TCG-x86_64), fork-marker on aarch64+riscv64
# only (EL0/U-mode smoke; test_aarch64/test_riscv64 reference it
# unconditionally, so omitting it breaks the off-x86 link).
if arch == 'x86_64'
  probe_pairs = [['libc_verify_stripped.elf', 'libc_verify_img'],
                 ['errno_probe_stripped.elf', 'errno_probe_img']]
elif arch == 'aarch64' or arch == 'riscv64'
  probe_pairs = [['fork_marker_stripped.elf', 'fork_marker_img']]
else
  probe_pairs = []
endif
# (An empty list means zero iterations — the arch simply embeds no probes.)
foreach pair : probe_pairs
  in_name = pair[0]
  stem = pair[1]
  # Relative command path (cwd=build root); rename source is the
  # relative-path default symbol: _binary_packaging_<name with _>.
  rel_in = 'packaging/' + in_name
  rel_sym = '_binary_packaging_' + in_name.replace('.', '_').replace('-', '_')
  probe_o = custom_target(stem + '.o',
    input : meson.project_build_root() / rel_in,
    output : stem + '.o',
    command : [objcopy_prog, '-I', 'binary', '-O', objcopy_fmt, '-B', objcopy_arch,
      '--redefine-sym', rel_sym + '_start=_binary_' + stem + '_start',
      '--redefine-sym', rel_sym + '_end=_binary_' + stem + '_end',
      '--redefine-sym', rel_sym + '_size=_binary_' + stem + '_size',
      rel_in, '@OUTPUT@'],
    build_by_default : true,
  )
  blob_objs += [probe_o]
endforeach

# Arch objects that bypass the generic rules.
extra_link_inputs = []

if arch == 'x86_64'
  # AP trampoline: nasm -f bin (org 0x7000) + objcopy -I binary with the
  # public symbol renames (mirrors mk/rules.mk AP_TRAMPOLINE_*).
  ap_trampoline_bin = custom_target('ap_trampoline.bin',
    input : files('../src/kernel/arch/x86_64/boot/ap_trampoline.nasm'),
    output : 'ap_trampoline.bin',
    command : [nasm_prog, '-f', 'bin', '-o', '@OUTPUT@', '@INPUT@'],
  )
  arch_ap_trampoline = custom_target('arch_ap_trampoline.o',
    input : ap_trampoline_bin,
    output : 'arch_ap_trampoline.o',
    command : [objcopy_prog, '-I', 'binary', '-O', objcopy_fmt,
      '-B', objcopy_arch,
      '--redefine-sym', '_binary_src_ap_trampoline_bin_start=_binary_ap_trampoline_start',
      '--redefine-sym', '_binary_src_ap_trampoline_bin_end=_binary_ap_trampoline_end',
      '--redefine-sym', '_binary_src_ap_trampoline_bin_size=_binary_ap_trampoline_size',
      'src/ap_trampoline.bin', '@OUTPUT@'],
    build_by_default : true,
  )
  extra_link_inputs += [arch_ap_trampoline]
endif

dtb_obj = []
if arch == 'aarch64'
  # QEMU-dumped DTB, trimmed + staged by the root build (virt_dtb_trimmed).
  # virt_dtb.S .incbin path is resolved from the build root, matching the
  # staged copy at <builddir>/build/virt.trimmed.dtb.
  virt_dtb_o = custom_target('virt_dtb.o',
    input : files('../src/kernel/arch/aarch64/virt_dtb.S'),
    output : 'virt_dtb.o',
    command : [cc_prog] + cc_base_flags + cc_arch_flags + cc_includes + cc_build_flags + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
    depends : [virt_dtb_trimmed],
  )
  dtb_obj = [virt_dtb_o]
endif

# --- Link (LDFLAGS mirror per arch; map file for postlink staging) ---
kernel_map = meson.project_build_root() / 'kernel.map'
if arch == 'x86_64'
  link_args = ['-nostdlib', '-no-pie',
    '-Wl,-melf_x86_64',
    '-Wl,-T,' + meson.project_source_root() / 'linker' / 'linker_x86_64.ld',
    '-Wl,-z,max-page-size=0x1000',
    '-Wl,-Map=' + kernel_map]
elif arch == 'aarch64'
  link_args = ['-nostdlib',
    '-Wl,-T,' + meson.project_source_root() / 'linker' / 'linker_aarch64.ld',
    '-Wl,-Map=' + kernel_map,
    '-lgcc']
else
  link_args = ['-nostdlib', '-static',
    '-Wl,-T,' + meson.project_source_root() / 'linker' / 'linker_riscv64.ld',
    '-Wl,-Map=' + kernel_map,
    '-lgcc']
endif
if arch == 'x86_64' and get_option('profile_mode') == 'gcov'
  # libgcov provides the runtime; src/lib/string.hpp satisfies its
  # strlen dependency (mirrors Make COVERAGE_LD_LIBS).
  link_args += [gcov_lib]
endif

kernel_unpatched = executable('kernel-unpatched',
  # Generated registry header as a source: build-order edge so the header
  # is generated before any TU compiles (never fall back to a stale
  # in-tree copy). Meson does not try to compile the .h itself.
  [test_registry_gen] + kernel_cpp + kernel_asm_s + asm_inputs,
  include_directories : kernel_includes,
  objects : blob_objs + extra_link_inputs + dtb_obj,
  link_args : link_args,
  link_depends : files('../linker/linker_' + arch + '.ld'),
  name_suffix : 'elf',
  install : false,
)

# --- post-link: CRC patch, identity asserts, markers, legacy staging ---
postlink_outputs = ['kernel-debug.elf']
if arch == 'riscv64'
  postlink_outputs += ['kernel-riscv64.bin']
endif
kernel_patched = custom_target('kernel-patched',
  input : kernel_unpatched,
  output : postlink_outputs,
  command : [py3, meson.project_source_root() / 'tools' / 'meson' / 'postlink.py',
    '--arch', arch,
    '--buildtype', get_option('buildtype'),
    '--lto', get_option('b_lto') ? 'on' : 'off',
    '--readelf', arch_readelf,
    '--test-class', get_option('test_class'),
    '--input', '@INPUT@',
    '--output', '@OUTPUT0@',
    '--marker', meson.project_build_root() / ('.lto-mode-' + arch),
    '--baked', meson.project_build_root() / ('.baked-test-config-' + arch)]
  + (get_option('buildtype') == 'release' and arch == 'x86_64'
     ? ['--baked-extra', meson.project_source_root() / 'release' / '.baked-test-config']
     : [])
  + (arch == 'riscv64' ? ['--riscv-bin', '@OUTPUT1@'] : [])
    + ['--legacy-dir', meson.project_source_root() / 'build',
       '--objcopy', objcopy_prog,
       '--map', kernel_map],
  depends : [test_config],
  build_by_default : true,
)

# Boot ISO (x86_64 only; mirrors `make debug`/`make release` ISOs).
# aa64/riscv64 boot via -kernel ELF/BIN directly (no GRUB ISO).
if arch == 'x86_64'
  iso_legacy = meson.project_source_root() / 'release' / 'nexios-rtos.iso'
  if get_option('buildtype') != 'release'
    iso_legacy = meson.project_source_root() / 'debug' / 'nexios-rtos.iso'
  endif
  iso_image = custom_target('iso-image',
    input : [kernel_patched, files('../iso/boot/grub/grub.cfg')],
    output : 'nexios-rtos.iso',
    command : [py3, meson.project_source_root() / 'tools' / 'meson' / 'package_iso.py',
      '--kernel', '@INPUT0@',
      '--grub-cfg', '@INPUT1@',
      '--out', '@OUTPUT@',
      '--legacy', iso_legacy],
    build_by_default : true,
  )
endif
