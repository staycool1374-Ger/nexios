# tools/test-policy.awk — resolver + validator for tools/test-policy.tsv
# The ONLY parser of the policy table (tools/meson/qemu_test.py,
# tools/meson/list_tests.py and scripts/run_all_classes.sh all call this;
# no parsing logic duplicated).
# POSIX awk only (BSD awk + gawk): no gensub, no length(array).
#
# Usage:
#   awk -f tools/test-policy.awk [-v arch=<a>] [-v class=<c|@pseudo>]
#       [-v key=<k>] [-v dump=1] [-v validate=1] tools/test-policy.tsv
# Pseudo-classes: @default (default tier), @group:<name>, @arch:<name>.
# Output: the bare value (`printf "%s"`, no trailing newline) or, with
#   dump=1, `key|value` lines in fixed key order.
# Exit: 0 ok, 1 missing value, 2 usage, 3 unknown key, 4 validation failure.
BEGIN {
    FS = "|"
    nkeys = 9
    keys[1] = "expect_timeout"; keys[2] = "outer_timeout"
    keys[3] = "max_attempts";   keys[4] = "stall_signatures"
    keys[5] = "watchdog_stall"; keys[6] = "expected_panic"
    keys[7] = "expected_empty"; keys[8] = "arch_scope"
    keys[9] = "build_scope"
    for (i = 1; i <= nkeys; i++) knownkey[keys[i]] = 1
    if (validate == 1) { mode = "validate" }
    else if (dump == 1) { mode = "dump" }
    else if (key != "") { mode = "lookup" }
    else {
        print "usage: awk -f test-policy.awk [-v arch=..]" \
            " [-v class=..] [-v key=..] [-v dump=1]" \
            " [-v validate=1] <tsv>" > "/dev/stderr"
        exit 2
    }
    if (arch == "") arch = ""
    if (class == "") class = ""
    nerr = 0
}

function trim(s) { gsub(/^[ \t\r]+|[ \t\r]+$/, "", s); return s }

function fail(msg) { print "test-policy: " msg > "/dev/stderr"; nerr++ }

function tier_of(scope) {
    if (scope == "default") return 0
    if (scope == "group") return 1
    if (scope == "arch") return 2
    if (scope == "class") return 3
    return -1
}

{
    t = trim($0)
    if (t == "" || substr(t, 1, 1) == "#") next
    if (NF != 4) { fail(FILENAME":"FNR": want 4 |-fields, have "NF); next }
    scope = trim($1); sel = trim($2); k = trim($3); val = trim($4)
    tr = tier_of(scope)
    if (tr < 0) { fail(FILENAME":"FNR": unknown scope '"scope"'"); next }
    if (k == "members") {
        if (scope != "group") {
            fail(FILENAME":"FNR": members row outside group scope"); next
        }
        nm = split(val, m, /[ \t]+/)
        for (i = 1; i <= nm; i++) {
            if (m[i] == "") continue
            if (m[i] in member_of) {
                fail(FILENAME":"FNR": class '"m[i]"' in two groups"); next
            }
            member_of[m[i]] = sel
        }
        next
    }
    if (!(k in knownkey)) { fail(FILENAME":"FNR": unknown key '"k"'"); next }
    id = tr SUBSEP sel SUBSEP k
    if ((id in seen) && recval[id] != val) {
        fail(FILENAME":"FNR": conflicting value for "scope"/"sel"/"k); next
    }
    seen[id] = FNR; recval[id] = val
}

function resolve(cls, a, k,   id, g) {
    if (cls == "@default") {
        id = 0 SUBSEP "*" SUBSEP k
        return ((id in seen) ? recval[id] : "")
    }
    if (substr(cls, 1, 7) == "@group:") {
        g = substr(cls, 8)
        id = 1 SUBSEP g SUBSEP k
        return ((id in seen) ? recval[id] : "")
    }
    if (substr(cls, 1, 6) == "@arch:") {
        g = substr(cls, 7)
        id = 2 SUBSEP g SUBSEP k
        return ((id in seen) ? recval[id] : "")
    }
    bv = ""
    id = 0 SUBSEP "*" SUBSEP k
    if ((id in seen)) bv = recval[id]
    if ((cls in member_of)) {
        id = 1 SUBSEP member_of[cls] SUBSEP k
        if ((id in seen)) bv = recval[id]
    }
    if (a != "") {
        id = 2 SUBSEP a SUBSEP k
        if ((id in seen)) bv = recval[id]
    }
    # Class tier: an arch-qualified selector (`arch:class`, issue #170
    # measured+30pct rule) beats the bare class selector, so per-arch
    # baselines override the arch-blind value. Bare rows keep working.
    if (a != "") {
        id = 3 SUBSEP a ":" cls SUBSEP k
        if ((id in seen)) bv = recval[id]
    }
    id = 3 SUBSEP cls SUBSEP k
    if ((id in seen)) bv = recval[id]
    return bv
}

function do_validate(   id, parts, n, kk, vv, ok) {
    for (id in seen) {
        n = split(id, parts, SUBSEP)
        kk = parts[3]; vv = recval[id]
        if (kk == "expect_timeout" || kk == "outer_timeout" || \
            kk == "max_attempts" || kk == "watchdog_stall") {
            ok = (vv ~ /^[0-9]+$/ && vv + 0 > 0)
            if (!ok) fail("not a positive integer: "kk"="vv)
        } else if (kk == "expected_panic" || kk == "expected_empty") {
            if (vv != "0" && vv != "1") fail("not 0/1: "kk"="vv)
        } else if (kk == "stall_signatures") {
            if (vv !~ /^[A-Za-z0-9_,]+$/) fail("bad token list: "kk"="vv)
        } else if (kk == "arch_scope" || kk == "build_scope") {
            if (vv == "") fail("empty scope: "kk)
        }
    }
}

END {
    if (mode == "validate") {
        do_validate()
        exit ((nerr == 0) ? 0 : 4)
    }
    if (!(key in knownkey) && mode == "lookup") {
        print "test-policy: unknown key '"key"'" > "/dev/stderr"
        exit 3
    }
    if (mode == "dump") {
        derr = 0
        for (i = 1; i <= nkeys; i++) {
            v = resolve(class, arch, keys[i])
            if (v == "") {
                print "test-policy: missing key '"keys[i]"'" \
                    " arch="arch" class='"class"'" > "/dev/stderr"
                derr = 1
            } else {
                printf "%s|%s\n", keys[i], v
            }
        }
        exit derr
    }
    v = resolve(class, arch, key)
    if (v == "") {
        print "test-policy: missing key '"key"'" \
            " arch="arch" class='"class"'" > "/dev/stderr"
        exit 1
    }
    printf "%s", v
    exit 0
}
