# Userspace, libc, initrd, images (issue #170 Phase 3 + Phase 5).
#
# Mirrors mk/rules.mk byte-for-byte: in-tree libc archive, userspace ELF
# links (crt0 first, -Wl,-z,max-page-size), picolibc sysroot + glue +
# verify program (x86_64 only), initrd tree + cpio (stage_initrd.py),
# FAT32 image (mkfat32img.py). The kernel link consumes the staged
# artifacts by absolute builddir path (ninja unifies producer/consumer
# across subdirs); src/meson.build objcopy steps rename symbols to the
# exact _binary_* names the kernel references.

# CC flags mirror Make CCFLAGS (sans -pipe -MMD -MP: ninja emits its own
# depfiles). -I dirs are per-target below, mirroring each Make rule
# exactly (-I order matters: sysroot must precede src/libc for picolibc
# targets so the sysroot errno.h wins over the in-tree one).
cc_usr_flags = cc_base_flags + cc_arch_flags + cc_build_flags
libc_includes = ['-I', meson.project_source_root() / 'src' / 'libc']
crt0_path = meson.project_build_root() / 'src' / 'crt0.o'

# --- In-tree libc archive (mirrors LIBC_A) ---
libc_sources = []
_libc_find = run_command('find',
  meson.project_source_root() / 'src' / 'libc',
  '-name', '*.c', '-o', '-name', '*.S',
  check : true)
foreach s : _libc_find.stdout().strip().split('\n')
  if s != ''
    libc_sources += [s]
  endif
endforeach
libc_a = static_library('c', libc_sources,
  include_directories : include_directories('../src/libc'),
  c_args : cc_usr_flags,
  # No LTO on libc objects: Make CCFLAGS never carry -flto
  # (LTO lives in debug CXXFLAGS/LDFLAGS only).
  override_options : ['b_lto=false'],
  install : false,
)

# --- Userspace discovery (mirrors USERSPACE_SRC; picolibc separate) ---
_user_find = run_command('find', meson.project_source_root() / 'userspace',
  '-path', meson.project_source_root() / 'userspace' / 'picolibc', '-prune',
  '-o', '-name', '*.c', '-print',
  '-o', '-name', '*.S', '-print',
  check : true)
userspace_srcs = []
foreach s : _user_find.stdout().strip().split('\n')
  if s != ''
    userspace_srcs += [s]
  endif
endforeach

# Root vs /bin split (issue #274): names must match the stage_initrd
# root copy list exactly.
_root_names = ['hey.c.elf', 'nullderef.c.elf', 'prime.c.elf']

# --- Userspace ELF links (mirror userspace/%.c.elf + %.S.elf) ---
root_elf_targets = []
bin_elf_targets = []
foreach src : userspace_srcs
  parts = src.split('/')
  out_name = parts[parts.length() - 1] + '.elf'
  # Probes with explicit link+strip targets below are skipped here to
  # avoid duplicate target names: errno_probe on x86_64 (embedded for
  # the Ring-3 TLS test), fork-marker on aarch64/riscv64 (embedded for
  # the EL0/U-mode smoke tests) — mirroring mk/rules.mk, where each
  # probe is built only on the arch that embeds it.
  if arch == 'x86_64' and out_name == 'errno_probe.c.elf'
    continue
  endif
  if (arch == 'aarch64' or arch == 'riscv64') and out_name == 'fork-marker.c.elf'
    continue
  endif
  if src.endswith('.S')
    # Freestanding .S: bare link, no crt0/libc (like the %.S.elf rule).
    elf = custom_target(out_name,
      input : files(src),
      output : out_name,
      command : [cc_prog] + cc_usr_flags + ['-o', '@OUTPUT@', '@INPUT@'],
      build_by_default : true)
  else
    # C: crt0 first, in-tree libc, max-page-size (like the %.c.elf rule).
    # Input order IS link order (crt0, objects, archive); '@INPUT@'
    # expands in order so no indexed form is needed.
    elf = custom_target(out_name,
      input : [crt0_path, files(src), libc_a],
      output : out_name,
      command : [cc_prog] + cc_usr_flags + libc_includes
        + ['-Wl,-z,max-page-size=0x1000', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default : true)
  endif
  if out_name in _root_names
    root_elf_targets += [elf]
  else
    bin_elf_targets += [elf]
  endif
endforeach

# --- Picolibc sysroot + glue + verify program (x86_64 only, like Make) ---
if arch == 'x86_64'
  sysroot_stamp = custom_target('picolibc-sysroot',
    output : 'sysroot-stamp',
    command : [py3, files('../tools/meson/run_checks.py'), 'picolibc'],
    build_by_default : false,
  )
  sysroot_inc = meson.project_source_root() / 'build' / 'picolibc' / 'sysroot' / 'include'
  sysroot_lib = meson.project_source_root() / 'build' / 'picolibc' / 'sysroot' / 'lib'
  picolibc_glue = custom_target('picolibc-glue-obj',
    input : files('../userspace/picolibc/nexios_glue.c'),
    output : 'nexios_glue.o',
    command : [cc_prog] + cc_usr_flags
      + ['-I', sysroot_inc] + libc_includes + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
    depends : [sysroot_stamp],
    build_by_default : false,
  )
  picolibc_stubs = custom_target('picolib_stubs.o',
    input : files('../src/libc/picolib_stubs.c'),
    output : 'picolib_stubs.o',
    command : [cc_prog] + cc_usr_flags + libc_includes + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : false,
  )
  picolibc_errno = custom_target('errno.o',
    input : files('../src/libc/errno.c'),
    output : 'errno.o',
    command : [cc_prog] + cc_usr_flags + libc_includes + ['-c', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : false,
  )
  verify_elf = custom_target('libc_verify.c.elf',
    input : [crt0_path, files('../userspace/picolibc/libc_verify.c'),
             picolibc_glue, picolibc_stubs, picolibc_errno],
    output : 'libc_verify.c.elf',
    # NOTE: no -I src/libc here (unlike the generic userspace rule):
    # libc_verify.c uses picolibc headers only, and the in-tree
    # src/libc/stdarg.h stub shadows gcc's complete stdarg.h, breaking
    # picolibc stdio.h (__gnuc_va_list). Verified: sysroot-only compiles
    # clean; adding src/libc reproduces the failure identically under
    # verbatim Make flags (pre-existing latent header conflict, not a
    # Meson regression — Make only passes via its stale committed .elf).
    command : [cc_prog] + cc_usr_flags
      + ['-I', sysroot_inc, '-o', '@OUTPUT@', '@INPUT@',
          '-L', sysroot_lib, '-lc', '-lm'],
    build_by_default : true,
  )
  # Stripped copy for embedding (tmpfs 64 KiB cap; mirrors Make rule).
  verify_stripped = custom_target('libc_verify_stripped.elf',
    input : verify_elf,
    output : 'libc_verify_stripped.elf',
    command : ['sh', '-c', 'cp "$1" "$2" && exec "$0" "$2"',
               arch_strip, '@INPUT@', '@OUTPUT@'],
    build_by_default : true,
  )
  # Errno probe: same link shape, then stripped for embedding.
  # Built explicitly (not via the generic loop) so the stripped variant
  # has a stable target handle for the objcopy step in src/meson.build.
  # Staged to /bin unstripped, like Make's INITRD_BIN_ELF.
  errno_elf = custom_target('errno_probe.c.elf',
    input : [crt0_path, files('../userspace/errno_probe.c'), libc_a],
    output : 'errno_probe.c.elf',
    command : [cc_prog] + cc_usr_flags + libc_includes
      + ['-Wl,-z,max-page-size=0x1000',
          '-o', '@OUTPUT@', '@INPUT@'],
    depends : [libc_a],
    build_by_default : true,
  )
  errno_stripped = custom_target('errno_probe_stripped.elf',
    input : errno_elf,
    output : 'errno_probe_stripped.elf',
    command : ['sh', '-c', 'cp "$1" "$2" && exec "$0" "$2"',
               arch_strip, '@INPUT@', '@OUTPUT@'],
    build_by_default : true,
  )
  bin_elf_targets += [errno_elf]
endif

if arch == 'aarch64' or arch == 'riscv64'
  # Fork-marker probe (mirrors FORK_MARKER_SRC/OBJ, mk/rules.mk:384-409):
  # linked in the generic-rule shape against the in-tree libc, stripped
  # with the arch strip, embedded by src/meson.build. Staged to /bin
  # unstripped, like Make's INITRD_BIN_ELF (which carries every
  # userspace ELF except the three root demos).
  fork_elf = custom_target('fork-marker.c.elf',
    input : [crt0_path, files('../userspace/fork-marker.c'), libc_a],
    output : 'fork-marker.c.elf',
    command : [cc_prog] + cc_usr_flags + libc_includes
      + ['-Wl,-z,max-page-size=0x1000',
          '-o', '@OUTPUT@', '@INPUT@'],
    depends : [libc_a],
    build_by_default : true,
  )
  fork_stripped = custom_target('fork_marker_stripped.elf',
    input : fork_elf,
    output : 'fork_marker_stripped.elf',
    command : ['sh', '-c', 'cp "$1" "$2" && exec "$0" "$2"',
               arch_strip, '@INPUT@', '@OUTPUT@'],
    build_by_default : true,
  )
  bin_elf_targets += [fork_elf]
endif

# --- initrd tree + cpio (mirrors the $(INITRD_CPIO) recipe) ---
initrd_cpio = custom_target('initrd.cpio',
  input : root_elf_targets + bin_elf_targets
    + [meson.project_build_root() / 'src' / 'test-config.txt'],
  output : 'initrd.cpio',
  command : [py3, files('../tools/meson/stage_initrd.py'),
    '--out', '@OUTPUT@',
    '--test-config', meson.project_build_root() / 'src' / 'test-config.txt',
    '--root'] + root_elf_targets + ['--bin'] + bin_elf_targets + [
    '--user-dir', meson.project_source_root() / 'userspace',
    '--tools-dir', meson.project_source_root() / 'tools',
    '--rust-hello', meson.project_source_root() / 'build' / 'user' / 'rust-hello.elf',
    '--rust-targets', meson.project_source_root() / 'rust-targets',
    '--libc-verify', meson.project_build_root() / 'packaging' / 'libc_verify.c.elf',
    '--rust-sources', meson.project_source_root() / 'user' / 'rust-hello',
  ],
  depends : root_elf_targets + bin_elf_targets,
  build_by_default : true,
)

# --- FAT32 image (mirrors the $(FAT32_IMG) recipe) ---
fat32_img = custom_target('fat32.img',
  input : files('../tools/mkfat32img.py'),
  output : 'fat32.img',
  command : [py3, '@INPUT@', '@OUTPUT@'],
  build_by_default : true,
)
