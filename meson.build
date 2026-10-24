# NexIOS root build (issue #170). Phase 1: toolchain + flags + probe TU.
# Full kernel build (sources/link/packaging/tests) lands in Phases 2-5.
# Make stays authoritative until the Phase 9 cutover.

project('nexios', 'cpp',
  version : '0.5.6-dev',
  meson_version : '>=1.12.0',
  default_options : [
    'cpp_std=c++20',
    'buildtype=debug',
    'b_lto=true',
    'b_pie=false',
    'b_staticpic=false',
    'b_sanitize=none',
    'b_ndebug=false',
    # No Meson-injected warning flags: -Wall/-Wextra come from the explicit
    # per-arch flag lists (byte-mirror of Make). Meson defaults would add
    # -Wall to C targets where Make has none.
    'warning_level=0',
  ])

add_languages('c', required : true)

arch = get_option('arch')
cpu_family = host_machine.cpu_family()
arch_from_cpu = {
  'x86_64' : 'x86_64',
  'aarch64' : 'aarch64',
  'riscv64' : 'riscv64',
}.get(cpu_family, 'unknown')
assert(arch == arch_from_cpu,
  'arch option (' + arch + ') must match the cross file cpu_family (' + cpu_family + ')')

# Single Python interpreter for all custom_targets and tests (PATH-resolved,
# pinned by the operator docs; never apt's 1.3.2).
py3 = find_program('python3', required : true)

# Shared toolchain programs (issue #170 Phase 3). The C compiler comes
# from the cross file; objcopy/strip/readelf/as are triplet-resolved.
# Defined before any flags that invoke tools.
triplet = arch + '-elf-'
cc_prog = find_program(triplet + 'gcc', required : true)
objcopy_prog = find_program(triplet + 'objcopy', required : true)
arch_readelf = find_program(triplet + 'readelf', required : true)
arch_strip = find_program(triplet + 'strip', required : true)
arch_objdump = find_program(triplet + 'objdump', required : false)
nasm_prog = find_program('nasm', required : arch == 'x86_64')

# --- Common flags: byte-mirror of Makefile CXXFLAGS_COMMON (sans -pipe -MMD -MP;
# ninja emits its own depfiles). CONFIG_DEFS map 1:1 to config_defs.
common_flags = [
  '-std=c++20', '-ffreestanding', '-fno-exceptions', '-fno-rtti',
  '-nostdlib', '-nostdinc', '-fno-builtin', '-fno-stack-protector',
  '-fno-threadsafe-statics',
  '-Wall', '-Wextra', '-Werror',
  '-ffunction-sections', '-fdata-sections',
]
arch_upper = arch.to_upper()
common_flags += [
  '-DCONFIG_ARCH_' + arch_upper,
  '-DNEXIOS_ARCH_NAME=' + arch,
  '-DPICOLIBC_VERSION_NUM=' + get_option('picolibc_version'),
]
foreach def : get_option('config_defs')
  common_flags += [def]
endforeach

# --- Arch flags: mirror of the Makefile per-arch blocks.
arch_flags = []
if arch == 'x86_64'
  arch_flags = ['-fno-pie', '-m64', '-mno-red-zone', '-mgeneral-regs-only', '-mcmodel=large']
elif arch == 'aarch64'
  arch_flags = ['-march=armv8-a']
elif arch == 'riscv64'
  arch_flags = ['-march=rv64imafdc', '-mabi=lp64d', '-mcmodel=medany', '-fno-pie']
endif

# --- Build-type flags: mirror of debug/release target overrides.
# -Db_lto=false is the NO_LTO variant (never mix; see .lto-mode marker, Phase 3).
build_flags = []
if get_option('buildtype') == 'release'
  build_flags = ['-g', '-O2', '-fanalyzer', '-DBUILD_TYPE=release']
  analyzer_suppress = [
    'analyzer-null-argument', 'analyzer-possible-null-argument',
    'analyzer-possible-null-dereference', 'analyzer-use-of-uninitialized-value',
    'analyzer-infinite-loop', 'analyzer-malloc-leak',
    'analyzer-undefined-behavior-ptrdiff', 'analyzer-out-of-bounds',
  ]
  foreach w : analyzer_suppress
    if cc.has_argument('-Wno-error=' + w)
      build_flags += ['-Wno-error=' + w]
    endif
  endforeach
else
  # Single LTO flag source: -Db_lto (Meson emits -flto=auto, semantically
  # -flto; the parity test normalizes -flto* to -flto). NO_LTO = -Db_lto=false.
  build_flags = ['-g', '-Og', '-DCONFIG_DEBUG', '-fno-omit-frame-pointer',
    '-fstack-usage', '-DBUILD_TYPE=debug']
endif

add_project_arguments(common_flags + arch_flags + build_flags,
  language : 'cpp')

# Profiling/coverage modes (issue #170 Phase 6; x86_64 only, like Make).
# Dedicated builddirs are mandatory here (flags change every object).
# gcov mode mirrors COVERAGE_LINE_FLAGS (absolute-path .gcno for host-side
# reconstruction); func/sampling modes mirror COVERAGE_FUNC_FLAGS.
if host_machine.cpu_family() == 'x86_64'
  gcov_lib = run_command(cc_prog, '--print-file-name=libgcov.a',
    check : false).stdout().strip()
  if get_option('profile_mode') == 'gcov'
    add_project_arguments(
      ['-fprofile-arcs', '-ftest-coverage', '-fprofile-abs-path'],
      language : ['cpp', 'c'])
  elif get_option('profile_mode') in ['sampling', 'coverage_func', 'coverage_line']
    add_project_arguments(
      ['-finstrument-functions',
       '-finstrument-functions-exclude-file-list=src/kernel/gcov/,gcov_handler.cpp'],
      language : ['cpp', 'c'])
  endif
endif

# C flags: byte-mirror of Makefile CCFLAGS (minus -pipe -MMD -MP; ninja
# depfiles). Applied to ALL C targets (kernel .S, libc, userspace),
# exactly as Make applies CCFLAGS everywhere.
cc_base_flags = ['-static', '-nostdlib', '-ffreestanding', '-O2',
  '-ffunction-sections', '-fdata-sections']
if arch == 'x86_64'
  cc_arch_flags = ['-m64']
elif arch == 'aarch64'
  cc_arch_flags = []
else
  cc_arch_flags = ['-DCONFIG_ARCH_RISCV64', '-mcmodel=medany', '-fno-pie']
endif
if arch == 'aarch64'
  cc_includes = ['-I' + meson.project_source_root() / 'src',
    '-I' + meson.project_source_root() / 'src' / 'lib']
else
  cc_includes = []
endif
if get_option('buildtype') == 'release'
  cc_build_flags = []
else
  cc_build_flags = ['-g', '-Og', '-DCONFIG_DEBUG', '-fno-omit-frame-pointer']
endif
add_project_arguments(cc_base_flags + cc_arch_flags + cc_includes + cc_build_flags,
  language : 'c')

kernel_includes = include_directories('src', 'src/lib')

# Phase 1 probe: one real TU under the exact kernel flag set (incl. -Werror).
# Proves cross-file resolution + flag acceptance per arch. The full kernel
# build lands in src/meson.build (Phase 3).
phase1_probe = static_library('phase1_probe', 'src/lib/version.cpp',
  include_directories : kernel_includes)

# QEMU virt DTB for aarch64 (Phase 3): dumped from QEMU, trimmed, embedded
# via virt_dtb.S `.incbin "build/virt.trimmed.dtb"` — the path resolves
# from the build root. custom_target outputs must be flat, so the trim
# script writes the tracked flat output AND the assembler-visible copy at
# <builddir>/build/virt.trimmed.dtb as one atomic step (same command, so
# the copy can never go stale independently).
# Defined BEFORE the subdir() calls: src/meson.build consumes the trim.
if arch == 'aarch64'
  qemu_aarch64 = find_program('qemu-system-aarch64', required : true)
  virt_dtb_raw = custom_target('virt-dtb-dump',
    output : 'virt.dtb',
    command : [qemu_aarch64, '-machine', 'virt,dumpdtb=@OUTPUT@',
      '-cpu', 'cortex-a72', '-m', '256M'],
  )
  virt_dtb_trimmed = custom_target('virt-dtb-trim',
    input : virt_dtb_raw,
    output : 'virt.trimmed.dtb',
    command : [py3, meson.project_source_root() / 'tools' / 'meson' / 'trim_dtb_staged.py',
      '@INPUT@', '@OUTPUT@',
      meson.project_build_root() / 'build' / 'virt.trimmed.dtb'],
  )
endif

# Packaging (userspace/libc/initrd/images) before src: the kernel link
# consumes blob objects defined there (link_with/objects cross-subdir).
subdir('packaging')

# Test registry codegen (Phase 2). Full source discovery in Phase 3.
subdir('src')

# Meson test map: QEMU suites + host suites (Phase 4).
subdir('tests')
