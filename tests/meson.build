# Meson test map (issue #170 Phase 4).
#
# QEMU suites: one test() per class from the policy table, generated at
# configure time by tools/meson/list_tests.py (mirrors the
# run_all_classes.sh selection incl. NEXIOS_TEST_CLASSES override).
# timeout = the policy outer_timeout backstop (never the 30 s default);
# per-attempt budgets are the measured+30pct expect_timeout rows.
# is_parallel: false (QEMU mutual exclusion + single serial log set).
# The runner reconfigures/rebuilds per test (idempotent), so plain
# `meson test` works with no extra flags.
#
# Host suites: build-system parity tests (no QEMU), validating the
# current builddir.

list_tests = files('../tools/meson/list_tests.py')
qemu_runner = files('../tools/meson/qemu_test.py')
build_kind = get_option('buildtype') == 'release' ? 'release' : 'debug'
foreach entry : run_command(py3, list_tests, arch,
    get_option('buildtype'),
    check : true).stdout().strip().split('\n')
  parts = entry.split('|')
  if parts.length() == 2
    test('qemu/' + arch + '/' + get_option('buildtype') + '/' + parts[0],
      py3,
      args : [qemu_runner,
        '--arch', arch,
        '--build', build_kind,
        '--class', parts[0],
        '--builddir', meson.project_build_root(),
        '--no-history'],
      timeout : parts[1].to_int(),
      is_parallel : false,
      suite : arch + '-' + get_option('buildtype'))
  endif
endforeach

host_tests = [
  'test_flag_parity',
  'test_source_set',
  'test_gen_registry',
  'test_lto_mode',
  'test_stage_paths',
  'test_policy_timeouts',
  'test_history_lock',
  'test_verdict_parser',
]
foreach name : host_tests
  test('host/' + name,
    py3,
    args : [files('../tools/meson/tests/' + name + '.py'),
      meson.project_build_root(), arch],
    suite : 'host')
endforeach
test('style', py3,
  args : [files('../tools/validate_style.py'), '--quiet'],
  suite : 'host')

# debugd RSP conformance (host-native; mirrors `make test-debugd-host`).
debugd_inc = include_directories('../tools/debugd/include',
  '../tools/debugd/tests')
foreach t : [['test_debugd', 'test_main.cpp', []],
             ['test_debugd_asan', 'test_main.cpp', ['-fsanitize=address,undefined']],
             ['test_debugd_loop', 'test_loop.cpp', []],
             ['test_debugd_loop_asan', 'test_loop.cpp',
              ['-fsanitize=address,undefined']]]
  debugd_exe = executable(t[0],
    files('../tools/debugd/tests/' + t[1]),
    include_directories : debugd_inc,
    c_args : ['-std=c++20', '-Wall', '-Wextra', '-Werror'] + t[2],
    cpp_args : ['-std=c++20', '-Wall', '-Wextra', '-Werror'] + t[2],
    link_args : t[2],
    native : true,
    install : false)
  test('host/debugd/' + t[0], debugd_exe, suite : 'host')
endforeach

# Interactive boot (issue #170 follow-up): one target per build kind, each
# refusing the wrong builddir and any baked test class at run time.
# Framebuffer window (default display), serial on mon:stdio, no GDB stub,
# no watchdog verdict — the guest runs until the user quits QEMU.
boot_target = files('../tools/meson/boot_target.py')
if build_kind == 'release'
  run_target('boot-release',
    command : [py3, boot_target,
      '--mode', 'release', '--arch', arch,
      '--builddir', meson.project_build_root()])
else
  run_target('boot-debug',
    command : [py3, boot_target,
      '--mode', 'debug', '--arch', arch,
      '--builddir', meson.project_build_root()])
endif

# Interactive / record flows (no `meson test` entry — they need a
# terminal or human-driven GDB). Class/script/cmds via environment
# (defaults mirror the most common Make invocations).
gdb_runner = files('../tools/meson/gdb_run.py')
run_target('debug-test',
  command : [py3, gdb_runner, 'debug-test',
    '--arch', arch, '--build', build_kind,
    '--class', run_command('sh', '-c', 'echo ${MESON_TEST_CLASS:-none}').stdout().strip(),
    '--gdb-script', run_command('sh', '-c', 'echo ${MESON_GDB_SCRIPT:-tools/gdb/test-batch.gdb}').stdout().strip()])
run_target('debug-shell',
  command : [py3, gdb_runner, 'debug-shell',
    '--arch', arch, '--build', build_kind,
    '--gdb-script', run_command('sh', '-c', 'echo ${MESON_GDB_SCRIPT:-tools/gdb/init.gdb}').stdout().strip(),
    '--cmds', run_command('sh', '-c', 'echo ${MESON_SHELL_CMDS:-}').stdout().strip()])
run_target('rr-record',
  command : [py3, gdb_runner, 'rr-record',
    '--arch', arch, '--build', build_kind])
run_target('rr-replay',
  command : [py3, gdb_runner, 'rr-replay',
    '--arch', arch, '--build', build_kind])
run_target('dump-counts',
  command : [py3, files('../tools/meson/qemu_test.py'),
    '--arch', arch, '--build', build_kind,
    '--dump-counts', '--no-history'])

# Lint / docs / config / analysis (non-blocking, mirror the Make targets).
run_target('check-style',
  command : [py3, files('../tools/validate_style.py'), '--quiet'])
run_target('clang-tidy',
  command : ['sh', '-c',
    'echo "clang-tidy: run via compile_commands.json (non-blocking; same checks as Makefile CLANG_TIDY_CHECKS)"'])
run_target('doxygen-check',
  command : [py3, files('../tools/check_doxygen.py')])
run_target('check-config',
  command : [py3, files('../tools/meson/run_checks.py'), 'check-config'])
run_target('config-summary',
  command : [py3, '-c',
    'import re,sys; print("\n".join(sorted("  " + m.group(1) for m in (re.match(r"#define\\s+(CONFIG_\\w+)", l) for l in open(sys.argv[1])) if m)))',
    meson.project_source_root() / 'src' / 'kernel' / 'nexios_config.h'])
run_target('test-policy',
  command : ['awk', '-f', files('../tools/test-policy.awk'),
    '-v', 'arch=' + arch, '-v', 'class=safe', '-v', 'dump=1',
    files('../tools/test-policy.tsv')])
run_target('test-policy-check',
  command : [py3, files('../tools/meson/run_checks.py'), 'test-policy-check'])
run_target('stack-usage-report',
  command : [py3, files('../tools/check-stack-usage.py')])
run_target('audit',
  command : [py3, files('../tools/meson/run_checks.py'), 'audit'])
run_target('test-debugd-host',
  command : ['sh', '-c',
    'echo "test-debugd-host: superseded by host/debugd_* meson tests below"; exit 2'])
run_target('symbols',
  command : [arch_readelf, '-s',
    meson.project_build_root() / 'src' / 'kernel-debug.elf'],
  depends : kernel_patched)
run_target('objdump',
  command : [arch_objdump, '-d',
    meson.project_build_root() / 'src' / 'kernel-debug.elf'],
  depends : kernel_patched)
run_target('picolibc',
  command : [py3, files('../tools/meson/run_checks.py'), 'picolibc'])
run_target('user-rust-hello',
  command : [py3, files('../tools/meson/run_checks.py'), 'user-rust-hello',
    arch])
run_target('coverage-build',
  command : ['sh', '-c',
    'echo "coverage-build: configure a dedicated builddir with -Dprofile_mode=<mode> first (x86_64 only)"; exit 2'])
run_target('coverage-class',
  command : ['sh', '-c',
    'echo "coverage-class: use qemu_test.py --arch x86_64 --class <name> in the coverage builddir"; exit 2'])
run_target('coverage',
  command : ['sh', '-c',
    'echo "coverage: run coverage-class per COVERAGE_CLASSES, then tools/coverage_report.py"; exit 2'])
run_target('profiling',
  command : ['sh', '-c',
    'echo "profiling: configure a builddir with -Dprofile_mode=sampling first (x86_64 only)"; exit 2'])
run_target('profile-class',
  command : ['sh', '-c',
    'echo "profile-class: use qemu_test.py in the profiling builddir"; exit 2'])
