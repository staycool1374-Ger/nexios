/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file test_shell_commands.cpp
/// @brief Shell command-surface tests (milestone v0.4.10 issue #125):
///        68 of the 90 functions in src/services/shell.cpp were never
///        entered — the built-ins tests never invoke.  Every test drives a
///        real command through `Shell::execute()` with terminal capture and
///        asserts on the produced text.
/// @note  Deliberately NOT executed: `reboot`, `exit`/`shutdown`, `logout`
///        (invoke cmd_exit), `selftest` (re-enters the test harness),
///        `read` and `less`-on-a-real-file and `ping` (block on input or the
///        network).  `sleep` is only ever called with 0.
///        Tests that create VFS objects or change cwd always undo it.

#include <test.hpp>
#include <logger.hpp>
#include <services/shell.hpp>
#include <services/terminal/terminal.hpp>
#include <services/program.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Capture buffer size — large enough for `help`/`dmesg`/`tasks`.
constexpr size_t k_capture_size = 4096;

/// @brief Executes a shell command line with terminal capture enabled.
/// @param buffer Nullable: pass nullptr when the output is irrelevant (the
///               command is executed, nothing is captured).
void run_shell(const char *command, char *buffer, size_t size) {
    if (buffer) {
        buffer[0] = '\0';
        service::Terminal::capture_begin(buffer, size);
    }
    service::Shell::execute(command);
    if (buffer)
        service::Terminal::capture_end();
}

/// @brief Freestanding substring search (no strstr in the kernel lib).
bool has(const char *haystack, const char *needle) {
    if (!*needle)
        return true;
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return true;
    }
    return false;
}

/// @brief Counts occurrences of one character in a NUL-terminated buffer.
size_t count_char(const char *text, char wanted) {
    size_t found = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (text[i] == wanted)
            ++found;
    }
    return found;
}

/// @brief Guarantees "/", "/dev", "/proc" and "/tmp" are mounted — only "/"
/// is mounted at boot; the rest appear after the first VFS-touching restore.
void ensure_standard_mounts() {
    if (vfs::resolve("/tmp") == nullptr)
        vfs::reset_and_remount();
}

/// @brief Capture buffer for full top frames (task table exceeds 4 KiB).
char top_out[8192];

/// @brief Fixture gate: top test tasks park here until terminated.
sync::Semaphore top_gate;

/// @brief Blocking fixture entry (issue #172 top tests).
void top_block_entry() {
    top_gate.wait();
}

/// @brief Names a fixture task (bounded copy, NUL-terminated).
void top_name_task(TaskControlBlock *t, const char *name) {
    if (!t || !name)
        return;
    size_t i = 0;
    while (name[i] && i + 1 < CONFIG_TASK_NAME_LEN) {
        t->name[i] = name[i];
        ++i;
    }
    t->name[i] = '\0';
}

/// @brief Spins until the timer tick advances (separates top sample
/// frames so the sliding window is non-empty).
void top_next_tick() {
    uint64_t t0 = arch::Timer::ticks();
    for (uint64_t i = 0; i < 10000000; ++i) {
        if (arch::Timer::ticks() != t0)
            break;
        arch::pause();
    }
}

/// @brief Index of needle in haystack, or UINT64_MAX when absent.
uint64_t top_index_of(const char *hay, const char *needle) {
    for (uint64_t i = 0; hay[i]; ++i) {
        uint64_t j = 0;
        while (needle[j] && hay[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return i;
    }
    return static_cast<uint64_t>(-1);
}

} // namespace

// Runmode: kernel
// Testidea: The terminal capture facility is the observation channel for the
// whole class — it must exist (a live Terminal instance backed by the
// framebuffer) and it must see what a command writes.
// Input: Terminal::instance(); capture around Shell::execute("echo ...").
// Expect: instance non-null; captured text contains the echoed argument and
//         none of the unknown-command marker.
// Depends: service::Terminal, service::Shell
JARVIS_TEST(shell_capture_observes_command_output,
            "PRE: vfsd, iocd | POST: none") {
    bool live = service::Terminal::instance() != nullptr;

    char out[k_capture_size];
    run_shell("echo zz-capture-marker", out, sizeof(out));
    bool saw_marker = has(out, "zz-capture-marker");
    bool rejected = has(out, "Unbekannter Befehl");

    JARVIS_ASSERT(live);
    JARVIS_ASSERT(saw_marker);
    JARVIS_ASSERT(!rejected);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `listprog` walks the program registry with count()/get() and
// prints "(keine)" when it is empty; the registry API itself is fail-closed
// (out-of-range get and unknown find both return nullptr).
// Input: Shell::execute("listprog"); ProgramRegistry::count()/get(count())/
//        find("no-such-program").
// Expect: Output has the "Registrierte Programme" header; get(count()) and
//         find() of an unknown name are nullptr.
// Depends: service::Shell, service::ProgramRegistry
JARVIS_TEST(shell_listprog_walks_program_registry,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    run_shell("listprog", out, sizeof(out));

    const size_t registered = service::ProgramRegistry::count();
    const service::ProgramRegistry::Program *past_end =
        service::ProgramRegistry::get(registered);
    const service::ProgramRegistry::Program *missing =
        service::ProgramRegistry::find("no-such-program-xyz");

    bool header_ok = has(out, "Registrierte Programme");
    bool empty_listed = registered != 0 || has(out, "(keine)");

    JARVIS_ASSERT(header_ok);
    JARVIS_ASSERT(empty_listed);
    JARVIS_ASSERT(past_end == nullptr);
    JARVIS_ASSERT(missing == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `run` resolves through ProgramRegistry::find: an unknown program
// name is reported, and the bare `run` (no argument) prints the usage block
// with the registry listing instead of dispatching anything.
// Input: Shell::execute("run no-such-program") and Shell::execute("run").
// Expect: First output names the missing program; second shows
//         "Usage: run <program> [&]" and the program list header.
// Depends: service::Shell, service::ProgramRegistry
JARVIS_TEST(shell_run_unknown_program_and_usage,
            "PRE: vfsd, iocd | POST: none") {
    char missing[k_capture_size];
    run_shell("run no-such-program-xyz", missing, sizeof(missing));
    bool named = has(missing, "no-such-program-xyz");

    char usage[k_capture_size];
    run_shell("run", usage, sizeof(usage));
    bool usage_ok = has(usage, "Usage: run");
    bool lists_ok = has(usage, "Verfuegbare Programme");

    JARVIS_ASSERT(named);
    JARVIS_ASSERT(usage_ok);
    JARVIS_ASSERT(lists_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The job-control built-ins are documented stubs / no-ops: `fg`
// and `bg` state that job control is not implemented, `disown` echoes the
// task id it released, `ulimit` refuses on an embedded system, and `wait`
// with no children returns immediately instead of blocking.
// Input: Shell::execute("jobs"), ("fg"), ("bg"), ("disown 7"), ("ulimit"),
//        ("wait").
// Expect: "Background tasks" header; both job-control stubs say
//         "not fully implemented"; disown echoes "disowned task 7"; ulimit
//         reports "not implemented"; wait completes (measured by returning).
// Depends: service::Shell, Scheduler::task_count/task_at
JARVIS_TEST(shell_job_control_stubs, "PRE: vfsd, iocd | POST: none") {
    char jobs[k_capture_size];
    run_shell("jobs", jobs, sizeof(jobs));

    char fg[k_capture_size];
    run_shell("fg", fg, sizeof(fg));

    char bg[k_capture_size];
    run_shell("bg", bg, sizeof(bg));

    char disown[k_capture_size];
    run_shell("disown 7", disown, sizeof(disown));

    char ulimit[k_capture_size];
    run_shell("ulimit", ulimit, sizeof(ulimit));

    run_shell("wait", nullptr, 0);

    JARVIS_ASSERT(has(jobs, "Background tasks"));
    JARVIS_ASSERT(has(fg, "not fully implemented"));
    JARVIS_ASSERT(has(bg, "not fully implemented"));
    JARVIS_ASSERT(has(disown, "disowned task 7"));
    JARVIS_ASSERT(has(ulimit, "not implemented"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The alias table is a real lifecycle: define, show a single
// alias, show all, resolve through `type`, then remove — and a lookup after
// removal reports "not found" rather than returning a stale entry.
// Input: Shell::execute("alias zz=version"), ("alias zz"), ("alias"),
//        ("type zz"), ("unalias zz"), ("alias zz").
// Expect: Definition silent; "zz='version'" after define; the listing
//         contains zz; type reports "is an alias for"; after unalias the
//         lookup says "not found".
// Depends: service::Shell
JARVIS_TEST(shell_alias_define_resolve_remove,
            "PRE: vfsd, iocd | POST: none") {
    run_shell("alias zz=version", nullptr, 0);

    char single[k_capture_size];
    run_shell("alias zz", single, sizeof(single));

    char all[k_capture_size];
    run_shell("alias", all, sizeof(all));

    char typed[k_capture_size];
    run_shell("type zz", typed, sizeof(typed));

    run_shell("unalias zz", nullptr, 0);

    char gone[k_capture_size];
    run_shell("alias zz", gone, sizeof(gone));

    JARVIS_ASSERT(has(single, "zz='version'"));
    JARVIS_ASSERT(has(all, "zz='version'"));
    JARVIS_ASSERT(has(typed, "is an alias for"));
    JARVIS_ASSERT(has(typed, "version"));
    JARVIS_ASSERT(has(gone, "not found"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Every executed line is appended to the history ring and
// `history` replays the most recent entries including the line just run.
// Input: Shell::execute("echo zz-hist-marker") then Shell::execute("history").
// Expect: The history listing contains the marker command.
// Depends: service::Shell
JARVIS_TEST(shell_history_records_executed_lines,
            "PRE: vfsd, iocd | POST: none") {
    run_shell("echo zz-hist-marker", nullptr, 0);

    char out[k_capture_size];
    run_shell("history", out, sizeof(out));

    JARVIS_ASSERT(has(out, "zz-hist-marker"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `type` distinguishes the three resolution outcomes — a shell
// built-in, an unknown name — and reports usage when called bare.
// Input: Shell::execute("type echo"), ("type zzz-no-such-cmd"), ("type").
// Expect: "is a shell built-in" for echo; ": not found" for the unknown
//         name; "Usage: type" for the bare call.
// Depends: service::Shell
JARVIS_TEST(shell_type_classifies_names, "PRE: vfsd, iocd | POST: none") {
    char builtin[k_capture_size];
    run_shell("type echo", builtin, sizeof(builtin));

    char unknown[k_capture_size];
    run_shell("type zzz-no-such-cmd", unknown, sizeof(unknown));

    char usage[k_capture_size];
    run_shell("type", usage, sizeof(usage));

    JARVIS_ASSERT(has(builtin, "is a shell built-in"));
    JARVIS_ASSERT(has(unknown, ": not found"));
    JARVIS_ASSERT(has(usage, "Usage: type"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `set` is a three-way dispatch: with no argument it reports the
// positional-arg count, with -x/+x it toggles options (and an unknown option
// is refused), and with plain words it installs positional parameters that
// `shift` then consumes.
// Input: Shell::execute("set a b c"), ("set"), ("shift"), ("set"),
//        ("set -x"), ("set +x"), ("set -q").
// Expect: positional count 3 → 2 after shift; the bare listing carries the
//         "Shell options" header; -x/+x are accepted silently; -q reports
//         "unknown option".
// Depends: service::Shell
JARVIS_TEST(shell_set_options_and_positional_shift,
            "PRE: vfsd, iocd | POST: none") {
    run_shell("set a b c", nullptr, 0);

    char before[k_capture_size];
    run_shell("set", before, sizeof(before));

    run_shell("shift", nullptr, 0);

    char after[k_capture_size];
    run_shell("set", after, sizeof(after));

    char unknown[k_capture_size];
    run_shell("set -q", unknown, sizeof(unknown));

    run_shell("set -x", nullptr, 0);
    run_shell("set +x", nullptr, 0);
    run_shell("shift 2", nullptr, 0);

    JARVIS_ASSERT(has(before, "Shell options"));
    JARVIS_ASSERT(has(before, "positional args: 3"));
    JARVIS_ASSERT(has(after, "positional args: 2"));
    JARVIS_ASSERT(has(unknown, "unknown option"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `printf` implements the %s/%d/%u/%c/%% conversions and the
// \n/\t escape set — every conversion must render its argument, and the
// length of the rendered text proves no conversion was skipped.
// Input: Shell::execute("printf X%dY%sZ%u\n 42 str 9").
// Expect: Captured text is exactly "X42YstrZ9\n".
// Depends: service::Shell
JARVIS_TEST(shell_printf_conversions, "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    run_shell("printf X%dY%sZ%u\\n 42 str 9", out, sizeof(out));

    bool prefix_ok = has(out, "X42YstrZ9");
    size_t newlines = count_char(out, '\n');

    JARVIS_ASSERT(prefix_ok);
    JARVIS_ASSERT(newlines >= 1);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `test`/`[` evaluate string equality, arithmetic comparison and
// the file predicates, and every accepted form must dispatch (an unparsed
// form would surface as an unknown command).
// Input: Shell::execute("test abc = abc"), ("test 3 -lt 10"),
//        ("[ -d / ]"), ("[ -f / ]"), ("test -z ''"), ("test ! abc = xyz").
// Expect: None of the forms produce the unknown-command marker; the file
//         predicate forms resolve a real directory (no "not found" from a
//         path lookup).
// Depends: service::Shell, vfs::resolve
JARVIS_TEST(shell_test_operator_forms, "PRE: vfsd, iocd | POST: none") {
    char eq[k_capture_size];
    run_shell("test abc = abc", eq, sizeof(eq));

    char lt[k_capture_size];
    run_shell("test 3 -lt 10", lt, sizeof(lt));

    char isdir[k_capture_size];
    run_shell("[ -d / ]", isdir, sizeof(isdir));

    char isfile[k_capture_size];
    run_shell("[ -f / ]", isfile, sizeof(isfile));

    char empty[k_capture_size];
    run_shell("test -z ''", empty, sizeof(empty));

    char negated[k_capture_size];
    run_shell("test ! abc = xyz", negated, sizeof(negated));

    JARVIS_ASSERT(!has(eq, "Unbekannter Befehl"));
    JARVIS_ASSERT(!has(lt, "Unbekannter Befehl"));
    JARVIS_ASSERT(!has(isdir, "Unbekannter Befehl"));
    JARVIS_ASSERT(!has(isfile, "Unbekannter Befehl"));
    JARVIS_ASSERT(!has(empty, "Unbekannter Befehl"));
    JARVIS_ASSERT(!has(negated, "Unbekannter Befehl"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The trap table supports install, list and remove; a listed trap
// shows its signal number and handler, and removing it by signal empties the
// table again.
// Input: Shell::execute("trap") (empty), ("trap handler 2"), ("trap"),
//        ("trap 2"), ("trap").
// Expect: Empty listing has no entries; after install the listing shows
//         "2: handler"; after removal the listing is entry-free again.
// Depends: service::Shell
JARVIS_TEST(shell_trap_install_list_remove, "PRE: vfsd, iocd | POST: none") {
    char empty[k_capture_size];
    run_shell("trap", empty, sizeof(empty));

    run_shell("trap handler 2", nullptr, 0);

    char installed[k_capture_size];
    run_shell("trap", installed, sizeof(installed));

    run_shell("trap 2", nullptr, 0);

    char removed[k_capture_size];
    run_shell("trap", removed, sizeof(removed));

    bool empty_has_handler = has(empty, "handler");
    bool installed_ok = has(installed, "2: handler");
    bool removed_has_handler = has(removed, "handler");

    JARVIS_ASSERT(!empty_has_handler);
    JARVIS_ASSERT(installed_ok);
    JARVIS_ASSERT(!removed_has_handler);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `umask` prints the current mask as three octal digits and
// accepts a new one; `times` renders shell running time in h/m/s.  The
// original mask is restored so shell state is unchanged for later tests.
// Input: Shell::execute("umask"), ("umask 022"), ("umask"),
//        ("umask <original>"), ("times").
// Expect: First print has 3 digits; after "umask 022" the value is "022";
//         times output carries the "shell running time" label and "s".
// Depends: service::Shell
JARVIS_TEST(shell_umask_roundtrip_and_times,
            "PRE: vfsd, iocd | POST: none") {
    char initial[k_capture_size];
    run_shell("umask", initial, sizeof(initial));

    run_shell("umask 022", nullptr, 0);

    char set_mask[k_capture_size];
    run_shell("umask", set_mask, sizeof(set_mask));

    // Restore the mask that was active before the test.
    char restore[16] = "umask ";
    size_t digit_count = 0;
    for (size_t i = 0; initial[i] && initial[i] != '\n' && digit_count < 3;
         ++i) {
        if (initial[i] >= '0' && initial[i] <= '7')
            restore[6 + digit_count++] = initial[i];
    }
    restore[6 + digit_count] = '\0';
    run_shell(digit_count == 3 ? restore : "umask 000", nullptr, 0);

    char times[k_capture_size];
    run_shell("times", times, sizeof(times));

    size_t digits = 0;
    for (size_t i = 0; initial[i] && initial[i] != '\n'; ++i)
        ++digits;

    JARVIS_ASSERT(digits == 3);
    JARVIS_ASSERT(has(set_mask, "022"));
    JARVIS_ASSERT(has(times, "shell running time"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The directory stack is LIFO and pushd/popd also change the
// working directory; an empty stack is reported instead of underflowing, and
// the original cwd is restored afterwards.
// Input: Shell::execute("dirs"), ("pushd /tmp"), ("dirs"), ("popd"),
//        ("dirs"), ("cd /"), ("pwd").
// Expect: Empty first; after pushd the stack lists /tmp; after popd it is
//         empty again; pwd is "/" after the final cd.
// Depends: service::Shell, vfs::resolve
JARVIS_TEST(shell_directory_stack_lifo, "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();

    char before[k_capture_size];
    run_shell("dirs", before, sizeof(before));

    run_shell("pushd /tmp", nullptr, 0);

    char pushed[k_capture_size];
    run_shell("dirs", pushed, sizeof(pushed));

    run_shell("popd", nullptr, 0);

    char popped[k_capture_size];
    run_shell("dirs", popped, sizeof(popped));

    // Restore the working directory for later tests.
    run_shell("cd /", nullptr, 0);
    char pwd[k_capture_size];
    run_shell("pwd", pwd, sizeof(pwd));

    // `cd /` restores the canonical root exactly (absolute targets keep
    // their leading '/' per issue #141).
    bool at_root_exact =
        pwd[0] == '/' && (pwd[1] == '\n' || pwd[1] == '\0');

    JARVIS_ASSERT(has(before, "Directory stack"));
    JARVIS_ASSERT(has(before, "(empty)"));
    JARVIS_ASSERT(has(pushed, "/tmp"));
    JARVIS_ASSERT(has(popped, "(empty)"));
    JARVIS_ASSERT(at_root_exact);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `cd` validates the target through the VFS: a missing path and a
// non-directory both report their specific error, while a real directory
// updates the working directory that `pwd` prints.
// Input: Shell::execute("cd /no-such-dir-zz"), ("cd /dev/tty"),
//        ("cd /tmp"), ("pwd"), ("cd /"), ("pwd").
// Expect: "no such directory" and "not a directory" respectively; pwd shows
//         exactly "/tmp" after the successful cd and exactly "/" after the
//         restore (issue #141 fixed: absolute targets keep their leading
//         '/').
// Depends: service::Shell, vfs::resolve
JARVIS_TEST(shell_cd_error_paths_and_pwd, "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();

    char missing[k_capture_size];
    run_shell("cd /no-such-dir-zz", missing, sizeof(missing));

    char notdir[k_capture_size];
    run_shell("cd /dev/tty", notdir, sizeof(notdir));

    run_shell("cd /tmp", nullptr, 0);
    char in_tmp[k_capture_size];
    run_shell("pwd", in_tmp, sizeof(in_tmp));

    run_shell("cd /", nullptr, 0);
    char at_root[k_capture_size];
    run_shell("pwd", at_root, sizeof(at_root));

    // Absolute targets keep their leading '/' (issue #141 fixed): the
    // canonical cwd after `cd /tmp` is exactly "/tmp".
    bool in_tmp_exact = in_tmp[0] == '/' && in_tmp[1] == 't' &&
                        in_tmp[2] == 'm' && in_tmp[3] == 'p' &&
                        (in_tmp[4] == '\n' || in_tmp[4] == '\0');
    bool at_root_exact =
        at_root[0] == '/' && (at_root[1] == '\n' || at_root[1] == '\0');

    JARVIS_ASSERT(has(missing, "no such directory"));
    JARVIS_ASSERT(has(notdir, "not a directory"));
    JARVIS_ASSERT(in_tmp_exact);
    JARVIS_ASSERT(at_root_exact);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The filesystem built-ins form a create→list→read→delete cycle
// that is ResourceTracker-clean: touch creates, ls lists, cat reads the
// (empty) file without a lookup error, rm removes it and the second ls no
// longer shows it; mkdir/rmdir do the same for a directory.  Missing paths
// are reported by cat and by less.
// Input: touch/ls/cat/rm/rmdir on /tmp/sc_* paths, then cat and less on a
//        missing path.  All removals run BEFORE any assertion.
// Expect: ls shows the created name; cat of the created file produces no
//         "No such file"; the second ls no longer lists it; cat and less of
//         a missing path both report "No such file".
// Depends: service::Shell, vfs::create_err/unlink_err, tmpfs
JARVIS_TEST(shell_filesystem_builtin_cycle, "PRE: vfsd, iocd | POST: none") {
    ensure_standard_mounts();

    run_shell("touch /tmp/sc_file", nullptr, 0);
    char listed[k_capture_size];
    run_shell("ls /tmp", listed, sizeof(listed));

    char cat[k_capture_size];
    run_shell("cat /tmp/sc_file", cat, sizeof(cat));

    run_shell("mkdir /tmp/sc_dir", nullptr, 0);
    char dir_listed[k_capture_size];
    run_shell("ls /tmp", dir_listed, sizeof(dir_listed));

    // Cleanup before asserting: an early return must not leak entries.
    run_shell("rm /tmp/sc_file", nullptr, 0);
    run_shell("rmdir /tmp/sc_dir", nullptr, 0);

    char after[k_capture_size];
    run_shell("ls /tmp", after, sizeof(after));

    char cat_missing[k_capture_size];
    run_shell("cat /tmp/sc_no_such_file", cat_missing, sizeof(cat_missing));

    char less_missing[k_capture_size];
    run_shell("less /tmp/sc_no_such_file", less_missing,
              sizeof(less_missing));

    JARVIS_ASSERT(has(listed, "sc_file"));
    JARVIS_ASSERT(has(dir_listed, "sc_dir"));
    JARVIS_ASSERT(!has(cat, "No such file"));
    JARVIS_ASSERT(!has(after, "sc_file"));
    JARVIS_ASSERT(!has(after, "sc_dir"));
    JARVIS_ASSERT(has(cat_missing, "No such file"));
    JARVIS_ASSERT(has(less_missing, "No such file"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The driver registry built-ins report the driver table and refuse
// an unknown driver name; the ELF background-load cancellation is a no-op
// when no load is in flight.
// Input: Shell::execute("modlist"), ("modprobe zz-no-such-driver"),
//        ("cancel-load").
// Expect: "Verfuegbare Treiber" header; the unknown driver is reported as
//         not found; cancel-load reports "not loading".
// Depends: service::Shell, kernel::DriverRegistry, kernel::elf::ElfLoader
JARVIS_TEST(shell_driver_and_loader_commands,
            "PRE: vfsd, iocd | POST: none") {
    char modlist[k_capture_size];
    run_shell("modlist", modlist, sizeof(modlist));

    char modprobe[k_capture_size];
    run_shell("modprobe zz-no-such-driver", modprobe, sizeof(modprobe));

    char cancel[k_capture_size];
    run_shell("cancel-load", cancel, sizeof(cancel));

    JARVIS_ASSERT(has(modlist, "Verfuegbare Treiber"));
    JARVIS_ASSERT(has(modprobe, "nicht gefunden"));
    JARVIS_ASSERT(has(cancel, "not loading"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `dmesg` renders the kernel log ring, `-h` renders the
// human-readable form, and `-i` injects an entry that the plain form then
// shows — the inject path is the only way to make the ring's content
// deterministic.
// Input: Shell::execute("dmesg -i 1 2 zz-dmesg-marker"), ("dmesg"),
//        ("dmesg -h"), ("dmesg -i") (missing operand usage).
// Expect: Inject prints "ok"; the plain listing contains the marker; the
//         human form is non-empty; the malformed inject prints its usage.
// Depends: service::Shell, kernel::log::DmesgService
JARVIS_TEST(shell_dmesg_inject_and_render, "PRE: vfsd, iocd | POST: none") {
    char injected[k_capture_size];
    run_shell("dmesg -i 1 2 zz-dmesg-marker", injected, sizeof(injected));

    char plain[k_capture_size];
    run_shell("dmesg", plain, sizeof(plain));

    char human[k_capture_size];
    run_shell("dmesg -h", human, sizeof(human));

    char usage[k_capture_size];
    run_shell("dmesg -i", usage, sizeof(usage));

    JARVIS_ASSERT(has(injected, "ok"));
    JARVIS_ASSERT(has(plain, "zz-dmesg-marker"));
    JARVIS_ASSERT(human[0] != '\0');
    JARVIS_ASSERT(has(usage, "usage: dmesg"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `lspci` prints the PCI device tree on x86_64 (or the
// unsupported-architecture notice elsewhere) and always produces output —
// it must never be mistaken for an unknown command.
// Input: Shell::execute("lspci").
// Expect: Non-empty output carrying "PCI" and no unknown-command marker.
// Depends: service::Shell, arch::pci_device_count/pci_devices
JARVIS_TEST(shell_lspci_lists_device_tree, "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    run_shell("lspci", out, sizeof(out));

    JARVIS_ASSERT(out[0] != '\0');
    JARVIS_ASSERT(has(out, "PCI"));
    JARVIS_ASSERT(!has(out, "Unbekannter Befehl"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `ifconfig` fails closed when no NIC is present and otherwise
// reports the interface — either way it produces output and is never an
// unknown command.
// Input: Shell::execute("ifconfig").
// Expect: Non-empty output, no unknown-command marker.
// Depends: service::Shell, kernel::gs::get_nic
JARVIS_TEST(shell_ifconfig_reports_interface_state,
            "PRE: vfsd, iocd | POST: none") {
    char out[k_capture_size];
    run_shell("ifconfig", out, sizeof(out));

    JARVIS_ASSERT(out[0] != '\0');
    JARVIS_ASSERT(!has(out, "Unbekannter Befehl"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Every built-in must reject a missing operand with its usage
// string instead of dispatching on garbage — this is the argument-validation
// contract shared by the whole command table.
// Input: Bare invocations of mkdir, rm, rmdir, unalias, source, printf,
//        which, sleep, export and the "." script alias.
// Expect: Each prints a "Usage:" line naming its own command.
// Depends: service::Shell
JARVIS_TEST(shell_missing_operand_usage_contract,
            "PRE: vfsd, iocd | POST: none") {
    struct Case {
        const char *command;
        const char *expected;
    };
    const Case cases[] = {
        {"mkdir", "Usage: mkdir"},    {"rm", "Usage: rm"},
        {"rmdir", "Usage: rmdir"},    {"unalias", "Usage: unalias"},
        {"source", "Usage: source"},  {".", "Usage: source"},
        {"printf", "Usage: printf"},  {"which", "Usage: which"},
        {"sleep", "Usage: sleep"},    {"export", "Usage: export"},
    };
    constexpr size_t case_count = sizeof(cases) / sizeof(cases[0]);

    bool results[case_count] = {};
    for (size_t i = 0; i < case_count; ++i) {
        char out[k_capture_size];
        run_shell(cases[i].command, out, sizeof(out));
        results[i] = has(out, cases[i].expected);
    }

    for (size_t i = 0; i < case_count; ++i)
        JARVIS_ASSERT(results[i]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `source` refuses anything that is not a readable regular file
// rather than executing a directory or a missing path; an unknown command
// name is reported with the unknown-command marker.
// Input: Shell::execute("source /no-such-script-zz"), ("source /"),
//        ("zzz-totally-unknown-command").
// Expect: Both source forms report "cannot read"; the unknown command prints
//         "Unbekannter Befehl" followed by the offending name.
// Depends: service::Shell, vfs::resolve
JARVIS_TEST(shell_source_rejects_unreadable_targets,
            "PRE: vfsd, iocd | POST: none") {
    char missing[k_capture_size];
    run_shell("source /no-such-script-zz", missing, sizeof(missing));

    char directory[k_capture_size];
    run_shell("source /", directory, sizeof(directory));

    char unknown[k_capture_size];
    run_shell("zzz-totally-unknown-command", unknown, sizeof(unknown));

    JARVIS_ASSERT(has(missing, "cannot read"));
    JARVIS_ASSERT(has(directory, "cannot read"));
    JARVIS_ASSERT(has(unknown, "Unbekannter Befehl"));
    JARVIS_ASSERT(has(unknown, "zzz-totally-unknown-command"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: `version` and `meminfo` are the two report built-ins that only
// read kernel state — both must produce their report without touching the
// filesystem — and a VFS built-in whose backend refuses must surface the
// mapped VfsError text through the shared shell_vfs_error() path.
// Input: Shell::execute("version"), ("meminfo"),
//        ("mkdir /no-such-parent-zz/child").
// Expect: version and meminfo each produce non-empty output and are not
//         rejected as unknown commands; the failing mkdir reports the mkdir
//         prefix plus the "Path or vnode not found" VfsError string.
// Depends: service::Shell, vfs::mkdir_err, errors::error_string
JARVIS_TEST(shell_version_meminfo_and_vfs_error,
            "PRE: vfsd, iocd | POST: none") {
    char version[k_capture_size];
    run_shell("version", version, sizeof(version));

    char meminfo[k_capture_size];
    run_shell("meminfo", meminfo, sizeof(meminfo));

    char vfs_error[k_capture_size];
    run_shell("mkdir /no-such-parent-zz/child", vfs_error, sizeof(vfs_error));

    JARVIS_ASSERT(version[0] != '\0');
    JARVIS_ASSERT(!has(version, "Unbekannter Befehl"));
    JARVIS_ASSERT(meminfo[0] != '\0');
    JARVIS_ASSERT(!has(meminfo, "Unbekannter Befehl"));
    JARVIS_ASSERT(has(vfs_error, "mkdir"));
    JARVIS_ASSERT(has(vfs_error, "not found"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cpuinfo renders every contracted section (issue #172).
// Input: Shell::execute("cpuinfo") with terminal capture.
// Expect: Vendor/Family/Model/Stepping/Brand/Features labels, APIC +
//         STATUS + CURRENT TASK + TICKS columns, Uptime footer, BSP row,
//         and "--" IDLE% on the very first call (no baseline yet).
// Depends: service::Shell, arch cpuid helpers
JARVIS_TEST(shell_cpuinfo_fields_present, "PRE: none | POST: none") {
    char out[4096];
    run_shell("cpuinfo", out, sizeof(out));
    bool ok = has(out, "Vendor") && has(out, "Family") &&
              has(out, "Model") && has(out, "Stepping") &&
              has(out, "Brand") && has(out, "Features") &&
              has(out, "APIC") && has(out, "STATUS") &&
              has(out, "CURRENT TASK") && has(out, "TICKS") &&
              has(out, "Uptime") && has(out, "BSP") && has(out, "--");
    JARVIS_ASSERT(ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: top snapshot header contract (issue #172, -d 0 = one frame).
// Input: Shell::execute("top -d 0") with large capture.
// Expect: load-average line, per-CPU bar line, TOT mean/max line,
//         sys/user split labels, full task-table header incl. PD_USE%.
// Depends: service::Shell
JARVIS_TEST(shell_top_snapshot_header, "PRE: none | POST: none") {
    run_shell("top -d 0", top_out, sizeof(top_out));
    bool ok = has(top_out, "load average") && has(top_out, "CPU0") &&
              has(top_out, "TOT") && has(top_out, "avg") &&
              has(top_out, "max") && has(top_out, "sys") &&
              has(top_out, "user") && has(top_out, "PID") &&
              has(top_out, "PD_USE%");
    JARVIS_ASSERT(ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: top sorts CPU% desc with PID tiebreak (issue #172, linux
// default).  Two blocked fixtures: B created first (lower PID, frozen
// exec) and A (higher PID, seeded +5s exec).  A must sort first despite
// the higher PID; B shows 0.0.
// Input: Two top frames separated by one tick (non-empty window).
// Expect: index("topA01") < index("topB01").
// Depends: service::Shell, Scheduler task walk
JARVIS_TEST(shell_top_sort_order, "PRE: none | POST: none") {
    top_gate.init(0, 1);
    auto *task_b = TaskControlBlock::create(top_block_entry, 6, 10000);
    auto *task_a = TaskControlBlock::create(top_block_entry, 5, 10000);
    JARVIS_ASSERT(task_a != nullptr);
    JARVIS_ASSERT(task_b != nullptr);
    top_name_task(task_a, "topA01");
    top_name_task(task_b, "topB01");
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task_b);
        Scheduler::add_task(*task_a);
    }
    // Deterministic window: reset the global ring, capture the baseline
    // frame, seed, wait one tick, capture the measuring frame.
    service::Shell::top_reset_samples();
    run_shell("top -d 0", top_out, sizeof(top_out));
    task_a->exec_ns_total += 5000000000ULL;
    top_next_tick();
    run_shell("top -d 0", top_out, sizeof(top_out));
    uint64_t ia = top_index_of(top_out, "topA01");
    uint64_t ib = top_index_of(top_out, "topB01");
    kernel::test::terminate_and_drain(*task_a);
    kernel::test::terminate_and_drain(*task_b);
    JARVIS_ASSERT(ia != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(ib != static_cast<uint64_t>(-1));
    JARVIS_ASSERT(ia < ib);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: PD_USE% math + 90% flag + non-periodic dash (issue #172).
// A periodic fixture (period 10000 ticks) shows seeded percentages;
// the idle task (NO_PERIOD) shows a dash in PD_USE%.
// Input: exec_period_ns seeded to 88% then 91% of the period.
// Expect: "88%" without "88%!", then "91%!"; idle row ends with "-".
// Depends: service::Shell
JARVIS_TEST(shell_top_pd_use_math_and_flag, "PRE: none | POST: none") {
    top_gate.init(0, 1);
    auto *task = TaskControlBlock::create(top_block_entry, 5, 10000);
    JARVIS_ASSERT(task != nullptr);
    top_name_task(task, "topPD01");
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task);
    }
    // 1 tick = 1e6 ns; period_ns = 10000 * 1e6.
    task->exec_period_ns = 8800000000ULL; // 88%
    run_shell("top -d 0", top_out, sizeof(top_out));
    bool seen88 = has(top_out, "88%") && !has(top_out, "88%!");
    task->exec_period_ns = 9100000000ULL; // 91% -> flag
    run_shell("top -d 0", top_out, sizeof(top_out));
    bool seen91 = has(top_out, "91%!");
    // Idle row (NO_PERIOD): last field of its task-table line is "-".
    // The NAME field renders padded (" idle        "); per-CPU "(idle)"
    // current-task markers must not match instead.
    uint64_t ishell = top_index_of(top_out, " idle        ");
    bool shell_dash = false;
    if (ishell != static_cast<uint64_t>(-1)) {
        uint64_t inl = ishell;
        while (top_out[inl] && top_out[inl] != '\n')
            ++inl;
        for (uint64_t k = (inl >= 8 ? inl - 8 : 0); k < inl; ++k) {
            if (top_out[k] == '-')
                shell_dash = true;
        }
    }
    kernel::test::terminate_and_drain(*task);
    JARVIS_ASSERT(seen88);
    JARVIS_ASSERT(seen91);
    JARVIS_ASSERT(shell_dash);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: affinity pin marker (issue #172): single-CPU mask renders
// "N!", contiguous multi-CPU mask renders "a-b" without "!".
// Input: Two fixtures with masks 1<<1 and 0b11.
// Expect: Owning rows contain "1!" and "0-1" respectively.
// Depends: service::Shell
JARVIS_TEST(shell_top_affinity_pin_marker, "PRE: none | POST: none") {
    top_gate.init(0, 1);
    auto *pin = TaskControlBlock::create(top_block_entry, 5, 10000);
    auto *multi = TaskControlBlock::create(top_block_entry, 5, 10000);
    JARVIS_ASSERT(pin != nullptr);
    JARVIS_ASSERT(multi != nullptr);
    top_name_task(pin, "toppin01");
    top_name_task(multi, "topmulti01");
    pin->cpu_affinity = 1ULL << 1;
    multi->cpu_affinity = 0b11ULL;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*pin);
        Scheduler::add_task(*multi);
    }
    run_shell("top -d 0", top_out, sizeof(top_out));
    uint64_t ipin = top_index_of(top_out, "toppin01");
    uint64_t imulti = top_index_of(top_out, "topmulti01");
    bool pin_ok = false;
    bool multi_ok = false;
    if (ipin != static_cast<uint64_t>(-1)) {
        for (uint64_t k = ipin; k < ipin + 76 && top_out[k]; ++k) {
            if (top_out[k] == '1' && top_out[k + 1] == '!')
                pin_ok = true;
        }
    }
    if (imulti != static_cast<uint64_t>(-1)) {
        for (uint64_t k = imulti; k < imulti + 76 && top_out[k]; ++k) {
            if (top_out[k] == '0' && top_out[k + 1] == '-' &&
                top_out[k + 2] == '1')
                multi_ok = true;
        }
    }
    kernel::test::terminate_and_drain(*pin);
    kernel::test::terminate_and_drain(*multi);
    JARVIS_ASSERT(pin_ok);
    JARVIS_ASSERT(multi_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: zombie section lists PID/NAME/EXIT and vanishes when empty
// (issue #172).  A terminated fixture (exit 3, not yet drained) must
// appear; after drain_zombie_list the section header is gone.
// Input: terminate fixture with code 3; top before/after drain.
// Expect: Row with name + exit 3 present; then "Zombies (" absent.
// Depends: service::Shell, Scheduler zombie list
JARVIS_TEST(shell_top_zombie_rows_and_omission, "PRE: none | POST: none") {
    top_gate.init(0, 1);
    auto *task = TaskControlBlock::create(top_block_entry, 5, 10000);
    JARVIS_ASSERT(task != nullptr);
    top_name_task(task, "topzomb01");
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task);
    }
    Scheduler::terminate(*task, 3);
    run_shell("top -d 0", top_out, sizeof(top_out));
    uint64_t iz = top_index_of(top_out, "topzomb01");
    bool row_ok = false;
    if (iz != static_cast<uint64_t>(-1)) {
        uint64_t inl = iz;
        while (top_out[inl] && top_out[inl] != '\n')
            ++inl;
        // EXIT column = last 7 chars of the line; must show 3.
        for (uint64_t k = (inl >= 7 ? inl - 7 : 0); k < inl; ++k) {
            if (top_out[k] == '3')
                row_ok = true;
        }
    }
    bool section_ok = has(top_out, "Zombies (");
    Scheduler::drain_zombie_list();
    run_shell("top -d 0", top_out, sizeof(top_out));
    bool omitted = !has(top_out, "Zombies (");
    JARVIS_ASSERT(row_ok);
    JARVIS_ASSERT(section_ok);
    JARVIS_ASSERT(omitted);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: sys/user split attribution (issue #172): a user fixture
// with seeded exec shows a saturated CPU% row; sys/user labels render.
// Input: is_user_ fixture + 5s exec seed, two frames over one tick.
// Expect: sys/user labels present; user row at 100.0 (clamped).
// Depends: service::Shell
JARVIS_TEST(shell_top_sys_user_split, "PRE: none | POST: none") {
    top_gate.init(0, 1);
    auto *task = TaskControlBlock::create(top_block_entry, 5, 10000);
    JARVIS_ASSERT(task != nullptr);
    top_name_task(task, "topuser01");
    task->is_user_ = true;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*task);
    }
    service::Shell::top_reset_samples();
    run_shell("top -d 0", top_out, sizeof(top_out));
    task->exec_ns_total += 5000000000ULL;
    top_next_tick();
    run_shell("top -d 0", top_out, sizeof(top_out));
    uint64_t iu = top_index_of(top_out, "topuser01");
    bool row_max = false;
    if (iu != static_cast<uint64_t>(-1)) {
        for (uint64_t k = iu; k < iu + 76 && top_out[k]; ++k) {
            if (top_out[k] == '1' && top_out[k + 1] == '0' &&
                top_out[k + 2] == '0' && top_out[k + 3] == '.' &&
                top_out[k + 4] == '0')
                row_max = true;
        }
    }
    bool labels = has(top_out, "sys ") && has(top_out, "user ");
    kernel::test::terminate_and_drain(*task);
    JARVIS_ASSERT(labels);
    JARVIS_ASSERT(row_max);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: top usage contract (issue #172).
// Input: "top -d xyz" and "top extra operand".
// Expect: "Usage: top" for both.
// Depends: service::Shell
JARVIS_TEST(shell_top_usage_contract, "PRE: none | POST: none") {
    char out[k_capture_size];
    run_shell("top -d xyz", out, sizeof(out));
    bool first = has(out, "Usage: top");
    run_shell("top surplus", out, sizeof(out));
    bool second = has(out, "Usage: top");
    JARVIS_ASSERT(first);
    JARVIS_ASSERT(second);
    JARVIS_TEST_PASS();
}

void register_shell_commands_tests() {
    Logger::info("Registering shell command-surface tests");
    JARVIS_REGISTER_TEST(shell_capture_observes_command_output);
    JARVIS_REGISTER_TEST(shell_listprog_walks_program_registry);
    JARVIS_REGISTER_TEST(shell_run_unknown_program_and_usage);
    JARVIS_REGISTER_TEST(shell_job_control_stubs);
    JARVIS_REGISTER_TEST(shell_alias_define_resolve_remove);
    JARVIS_REGISTER_TEST(shell_history_records_executed_lines);
    JARVIS_REGISTER_TEST(shell_type_classifies_names);
    JARVIS_REGISTER_TEST(shell_set_options_and_positional_shift);
    JARVIS_REGISTER_TEST(shell_printf_conversions);
    JARVIS_REGISTER_TEST(shell_test_operator_forms);
    JARVIS_REGISTER_TEST(shell_trap_install_list_remove);
    JARVIS_REGISTER_TEST(shell_umask_roundtrip_and_times);
    JARVIS_REGISTER_TEST(shell_directory_stack_lifo);
    JARVIS_REGISTER_TEST(shell_cd_error_paths_and_pwd);
    JARVIS_REGISTER_TEST(shell_filesystem_builtin_cycle);
    JARVIS_REGISTER_TEST(shell_driver_and_loader_commands);
    JARVIS_REGISTER_TEST(shell_dmesg_inject_and_render);
    JARVIS_REGISTER_TEST(shell_lspci_lists_device_tree);
    JARVIS_REGISTER_TEST(shell_ifconfig_reports_interface_state);
    JARVIS_REGISTER_TEST(shell_missing_operand_usage_contract);
    JARVIS_REGISTER_TEST(shell_source_rejects_unreadable_targets);
    JARVIS_REGISTER_TEST(shell_version_meminfo_and_vfs_error);
    JARVIS_REGISTER_TEST(shell_cpuinfo_fields_present);
    JARVIS_REGISTER_TEST(shell_top_snapshot_header);
    JARVIS_REGISTER_TEST(shell_top_sort_order);
    JARVIS_REGISTER_TEST(shell_top_pd_use_math_and_flag);
    JARVIS_REGISTER_TEST(shell_top_affinity_pin_marker);
    JARVIS_REGISTER_TEST(shell_top_zombie_rows_and_omission);
    JARVIS_REGISTER_TEST(shell_top_sys_user_split);
    JARVIS_REGISTER_TEST(shell_top_usage_contract);
}
