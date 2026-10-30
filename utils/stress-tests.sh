#!/bin/sh
# utils/stress-tests.sh: the meson test suite again and again on a loaded
# machine, to find the tests that pass on an idle one only (a fixed sleep
# that is too short, a deadline that a slow run misses). A release gate
# before tagging (docs/shellter/releasing.md, "Testing under load").
#
# Usage: stress-tests.sh [options] [BUILD_DIR] [-- MESON_TEST_ARGS...]
#   BUILD_DIR             the meson build directory (default: Build); it
#                         must be built: the tests run with --no-rebuild
#   -n, --rounds N        run the suite N times (default 3)
#   -l, --load N          N busy processes load the CPUs while the tests
#                         run (default: one per CPU; 0 = none)
#   -j, --parallel N      N runs of the suite at the same time in each
#                         round (default 1)
#   -p, --num-processes N meson test --num-processes N (default: meson's)
#   -h, --help            this text
# Arguments after -- go to meson test as they are (test names, --suite).
# Each run's logs stay in BUILD_DIR/meson-logs/stress-rR-pP.{txt,json}.
# At the end: how often each test that failed did, out of how many runs.
# Exit: 0 every run passed, 1 a test failed (or a run of meson did), 2 usage.

set -u

usage() {
    sed -n '/^# Usage:/,/^# Exit:/s/^# \{0,1\}//p' "$0"
}

die() {
    echo "stress-tests: $1" >&2
    echo "stress-tests: see stress-tests.sh --help" >&2
    exit 2
}

count() {   # option value: a whole number or die
    case $2 in
        ''|*[!0-9]*) die "$1 needs a whole number, not '$2'" ;;
    esac
}

cpus() {
    n=$(getconf _NPROCESSORS_ONLN 2>/dev/null) ||
        n=$(sysctl -n hw.ncpu 2>/dev/null) || n=1
    case $n in ''|*[!0-9]*) n=1 ;; esac
    echo "$n"
}

rounds=3
load=$(cpus)
parallel=1
nproc=
build=

while [ $# -gt 0 ]; do
    case $1 in
        -n|--rounds) [ $# -ge 2 ] || die "$1 needs a value"; count "$1" "$2"; rounds=$2; shift 2 ;;
        -l|--load) [ $# -ge 2 ] || die "$1 needs a value"; count "$1" "$2"; load=$2; shift 2 ;;
        -j|--parallel) [ $# -ge 2 ] || die "$1 needs a value"; count "$1" "$2"; parallel=$2; shift 2 ;;
        -p|--num-processes) [ $# -ge 2 ] || die "$1 needs a value"; count "$1" "$2"; nproc=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option $1" ;;
        *) [ -z "$build" ] || die "one build directory only ($build, $1)"; build=$1; shift ;;
    esac
done
[ -n "$build" ] || build=Build
[ "$rounds" -ge 1 ] || die "--rounds must be at least 1"
[ "$parallel" -ge 1 ] || die "--parallel must be at least 1"
[ -f "$build/build.ninja" ] || die "$build is no meson build directory (no build.ninja)"
command -v meson >/dev/null 2>&1 || die "no meson in PATH"
logs=$build/meson-logs

burners=
runs=
cleanup() {
    for p in $runs $burners; do
        kill "$p" 2>/dev/null
    done
    for p in $runs $burners; do
        wait "$p" 2>/dev/null
    done
    runs=
    burners=
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# a busy process: it stops by itself when this script is gone, even after
# a kill -9 that left the trap no chance (it looks every few thousand turns)
burn() {
    while kill -0 "$1" 2>/dev/null; do
        i=0
        while [ "$i" -lt 5000 ]; do i=$((i + 1)); done
    done
}

i=0
while [ "$i" -lt "$load" ]; do
    burn $$ &
    burners="$burners $!"
    i=$((i + 1))
done

echo "stress-tests: $rounds round(s) of $parallel run(s), $load busy process(es), build $build"
status=0
r=1
while [ "$r" -le "$rounds" ]; do
    runs=
    p=1
    while [ "$p" -le "$parallel" ]; do
        base=stress-r$r-p$p
        rm -f "$logs/$base.json" "$logs/$base.txt"
        # --num-processes and its value, or nothing
        meson test -C "$build" --no-rebuild --print-errorlogs --logbase "$base" \
            ${nproc:+--num-processes "$nproc"} "$@" > "$logs/$base.out" 2>&1 &
        runs="$runs $!"
        p=$((p + 1))
    done
    start=$(date +%s)
    p=1
    for pid in $runs; do
        rc=0
        wait "$pid" || rc=$?
        base=stress-r$r-p$p
        summary=$(grep -E '^(Ok|Fail|Timeout|Expected Fail|Unexpected Pass|Skipped): ' "$logs/$base.out" |
            tr -s ' ' | tr '\n' ' ')
        echo "round $r run $p: exit $rc, $(( $(date +%s) - start )) s: ${summary:-no summary, see $logs/$base.out}"
        [ "$rc" = 0 ] || status=1
        p=$((p + 1))
    done
    runs=
    r=$((r + 1))
done
cleanup

# every run's results: one JSON object per test and line; the keys are
# matched with the quote before them, which an escaped value has not
total=$((rounds * parallel))
report=$(
    r=1
    while [ "$r" -le "$rounds" ]; do
        p=1
        while [ "$p" -le "$parallel" ]; do
            f=$logs/stress-r$r-p$p.json
            [ -f "$f" ] && cat "$f"
            p=$((p + 1))
        done
        r=$((r + 1))
    done | awk -v total="$total" '
        {
            if (!match($0, /^\{"name": "[^"]*"/)) next
            name = substr($0, 11, RLENGTH - 11)
            if (!match($0, /"result": "[A-Z]*"/)) next
            result = substr($0, RSTART + 11, RLENGTH - 12)
            if (result == "OK" || result == "SKIP" || result == "EXPECTEDFAIL") next
            failed[name]++
            how[name] = how[name] (index(how[name], result) ? "" : (how[name] == "" ? "" : ",") result)
        }
        END {
            for (n in failed)
                printf "  %d of %d runs: %s (%s)\n", failed[n], total, n, how[n]
        }' | sort -rn
)
if [ -n "$report" ]; then
    echo "stress-tests: tests that failed:"
    echo "$report"
    status=1
elif [ "$status" != 0 ]; then
    echo "stress-tests: no test failed, but a run of meson did (see $logs/stress-r*-p*.out)"
else
    echo "stress-tests: every test passed in all $total run(s)"
fi
exit "$status"
