#!/bin/sh
# Characterization test: shellter-install.sh orders releases correctly for
# both 1.3.x and calendar (2026.10.0) versions. "cur" checks the installer
# of this tree, "old" the installer of shellter-v1.3.10 (the copy that runs
# erssi --update on a 1.3.10 install), which is skipped when that tag is not
# here. TAP output: a skip shows as a skip in meson, not as a pass.
# Usage: test-installer-versions.sh SOURCE_ROOT cur|old

src=$1
which=$2
case "$which" in cur|old) ;; *) echo "usage: $0 SOURCE_ROOT cur|old" >&2; exit 1 ;; esac
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
failed=0
n=0

pass() { n=$((n + 1)); echo "ok $n - $1"; }
fail() { n=$((n + 1)); echo "not ok $n - $1: $2"; failed=1; }

cp "$src/shellter-install.sh" "$tmp/cur.sh" || exit 1
if [ "$which" = old ]; then
    # shellter-v1.3.10 as the current installer pins it: a clone can keep a
    # stale local tag (the tag was moved on GitHub, and a plain fetch does
    # not move a tag that exists here)
    pinned=$(sed -n '/^RELEASES="/,/^"/p' "$tmp/cur.sh" | awk '$1 == "shellter-v1.3.10" { print $2 }')
    here=$(git -C "$src" rev-parse -q --verify 'shellter-v1.3.10^{commit}' 2>/dev/null)
    if [ -z "$pinned" ]; then
        echo "1..0 # SKIP shellter-install.sh pins no shellter-v1.3.10 in RELEASES"; exit 0
    elif [ -z "$here" ]; then
        echo "1..0 # SKIP no shellter-v1.3.10 here (shallow clone or tarball)"; exit 0
    elif [ "$here" != "$pinned" ]; then
        echo "1..0 # SKIP the local shellter-v1.3.10 is $here, not the pinned $pinned (stale tag; git fetch --tags --force)"; exit 0
    fi
    git -C "$src" show "$pinned:shellter-install.sh" > "$tmp/old.sh" || exit 1
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

name=$which
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

echo "1..$n"
exit "$failed"
