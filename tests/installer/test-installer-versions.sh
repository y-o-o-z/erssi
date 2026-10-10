#!/bin/sh
# Characterization test: shellter-install.sh orders releases correctly for
# both 1.3.x and calendar (2026.10.0) versions.
# Usage: test-installer-versions.sh SOURCE_ROOT

src=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
failed=0

pass() { echo "ok $1"; }
fail() { echo "FAIL $1: $2"; failed=1; }

cp "$src/shellter-install.sh" "$tmp/cur.sh" || exit 1
copies="cur"
if git -C "$src" cat-file -e shellter-v1.3.10:shellter-install.sh 2>/dev/null; then
    git -C "$src" show shellter-v1.3.10:shellter-install.sh > "$tmp/old.sh" || exit 1
    copies="cur old"
else
    pass "skip: no shellter-v1.3.10 here (shallow clone)"
fi

# newer A B EXPECTED_EXIT, run with the function loaded from FILE
check_newer() {
    file=$1; a=$2; b=$3; want=$4
    ( eval "$(sed -n '/^newer() {/,/^}/p' "$file")"; newer "$a" "$b" )
    got=$?
    if [ "$got" = "$want" ]; then
        pass "$name: newer $a $b -> $want"
    else
        fail "$name: newer $a $b" "exit $got, want $want"
    fi
}

sedexpr='s|.*refs/tags/\(shellter-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$|\1|p'
tab=$(printf '\t')
lines="0000000000000000000000000000000000000000${tab}refs/tags/shellter-v1.3.10
0000000000000000000000000000000000000000${tab}refs/tags/shellter-v2026.10.0
0000000000000000000000000000000000000000${tab}refs/tags/shellter-v2026.10.0-rc1
0000000000000000000000000000000000000000${tab}refs/tags/shellter-v2026.9.2
0000000000000000000000000000000000000000${tab}refs/tags/v1.3.1"

for name in $copies; do
    f="$tmp/$name.sh"
    check_newer "$f" 2026.10.0 1.3.10 0
    check_newer "$f" 1.3.10 2026.10.0 1
    check_newer "$f" 2026.10.0 2026.10.0 1
    check_newer "$f" 2026.10.1 2026.10.0 0
    check_newer "$f" 2026.11.0 2026.10.9 0
    check_newer "$f" 2027.1.0 2026.12.3 0
    check_newer "$f" 2026.10.10 2026.10.9 0
    check_newer "$f" 1.3.10 1.3.9 0

    if grep -qF "sed -n 's|.*refs/tags/\\(shellter-v[0-9][0-9]*\\.[0-9][0-9]*\\.[0-9][0-9]*\\)\$|\\1|p' |" "$f"; then
        pass "$name: tag filter line present"
    else
        fail "$name: tag filter line" "not found"
    fi
    if grep -qF 'sort -t. -k1,1nr -k2,2nr -k3,3nr' "$f"; then
        pass "$name: sort line present"
    else
        fail "$name: sort line" "not found"
    fi
    got=$(printf '%s\n' "$lines" | sed -n "$sedexpr" | sed 's/^shellter-v//' |
          sort -t. -k1,1nr -k2,2nr -k3,3nr | tr '\n' ' ')
    if [ "$got" = "2026.10.0 2026.9.2 1.3.10 " ]; then
        pass "$name: tag filter and order"
    else
        fail "$name: tag filter and order" "$got"
    fi
done

exit "$failed"
