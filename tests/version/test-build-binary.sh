#!/bin/sh
# utils/build-binary.sh against a throwaway repository: the files of this
# tree committed there and tagged with a throwaway SSH key (never the
# release key). The packager under test is a copy whose RELEASE_SIGNER and
# checked fingerprint are those of the throwaway key. HOME, git's config
# and TMPDIR are all inside one temporary directory.
# Usage: test-build-binary.sh SOURCE_ROOT CASE
#   verify      the tags it refuses (before any build), and --sums with a
#               signature made and checked the way docs/shellter/releasing.md
#               says
#   package     a package of this tree: the tarball's name, its single top
#               directory, sorted entries owned by 0 and dated to the commit,
#               the MANIFEST of the release format, and erssi running from
#               two places it is unpacked to; the build's own tests run are
#               the relocatable live test (BUILD_BINARY_TESTS)
#   tests-fail  a tag whose tests fail: no package
# Exit: 0 pass, 1 fail, 77 skip.
# shellcheck disable=SC2016 # the awk programs and sh -c scripts take $1 themselves

root=$(cd "$1" && pwd) || exit 1
CASE=$2

# a package build runs the tests of what it builds: not this one again
if [ -n "${ERSSI_BUILD_BINARY:-}" ] && [ "$CASE" != verify ]; then
    echo "skip: inside a package build"; exit 77
fi
# a clean environment: nothing from the caller's HOME, git or signing setup
if [ -z "${BUILD_BINARY_TEST_CLEAN:-}" ]; then
    exec env -i BUILD_BINARY_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

for t in git ssh-keygen tar xz; do
    command -v "$t" >/dev/null 2>&1 || { echo "skip: no $t"; exit 77; }
done
# shellcheck disable=SC2046 # "2 47" -> $1 $2
set -- $(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
{ [ "${1:-0}" -gt 2 ] || { [ "${1:-0}" -eq 2 ] && [ "${2:-0}" -ge 34 ]; }; } ||
    { echo "skip: git before 2.34 cannot check SSH signatures"; exit 77; }
git -C "$root" rev-parse --git-dir >/dev/null 2>&1 || { echo "skip: $root is not a git checkout"; exit 77; }

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
failed=0
ok() { echo "ok - $1"; }
bad() { echo "FAIL - $1"; failed=1; }
check() {   # description command...
    d=$1; shift
    if "$@"; then ok "$d"; else bad "$d"; fi
}

EMAIL='136268860+y-o-o-z@users.noreply.github.com'
export HOME="$tmp/home" TMPDIR="$tmp/tmpdir" GIT_CONFIG_NOSYSTEM=1 LC_ALL=C
mkdir -p "$HOME" "$TMPDIR" || exit 1
chmod 700 "$tmp" "$HOME" "$TMPDIR"
ssh-keygen -q -t ed25519 -N '' -C release -f "$tmp/key" || exit 1
ssh-keygen -q -t ed25519 -N '' -C other -f "$tmp/other" || exit 1
cat > "$HOME/.gitconfig" <<EOF
[user]
	name = y-o-o-z
	email = $EMAIL
	signingkey = $tmp/key
[gpg]
	format = ssh
[commit]
	gpgsign = false
[tag]
	gpgsign = false
[init]
	defaultBranch = main
EOF

# the packager, trusting the throwaway key: the release key and its
# fingerprint must be in it once each, so that a new key there cannot
# leave this copy trusting the real one
BB=$tmp/build-binary.sh
old_key=$(sed -n "s/^RELEASE_SIGNER='[^ ]* namespaces=\"git\" \([^ ]* [^ ']*\).*/\1/p" "$root/utils/build-binary.sh")
old_fp=$(sed -n "s/^RELEASE_FINGERPRINT='\(SHA256:[A-Za-z0-9+/]*\)'$/\1/p" "$root/utils/build-binary.sh")
new_key=$(cut -d' ' -f1,2 "$tmp/key.pub")
new_fp=$(ssh-keygen -lf "$tmp/key.pub" | awk '{print $2}')
if ! { [ -n "$old_key" ] && [ -n "$old_fp" ] &&
        [ "$(grep -cF "$old_key" "$root/utils/build-binary.sh")" = 1 ] &&
        [ "$(grep -cF "$old_fp" "$root/utils/build-binary.sh")" = 1 ]; }; then
    echo "FAIL - the release key or its fingerprint not found once in build-binary.sh"; exit 1
fi
# the same key and fingerprint as the installer's: one release key
if ! { grep -qF "$old_key" "$root/shellter-install.sh" && grep -qF "$old_fp" "$root/shellter-install.sh"; }; then
    echo "FAIL - build-binary.sh and shellter-install.sh trust different keys"; exit 1
fi
awk -v ok="$old_key" -v nk="$new_key" -v of="$old_fp" -v nf="$new_fp" '
    function swap(s, a, b,   i) {
        i = index(s, a)
        return i ? substr(s, 1, i - 1) b substr(s, i + length(a)) : s
    }
    { print swap(swap($0, ok, nk), of, nf) }' "$root/utils/build-binary.sh" > "$BB" || exit 1
if ! { grep -qF "$new_key" "$BB" && grep -qF "$new_fp" "$BB" && ! grep -qF "$old_fp" "$BB"; }; then
    echo "FAIL - the throwaway key did not replace the release key"; exit 1
fi

# the throwaway repository: this tree's files (also those not committed
# yet), committed
R=$tmp/repo
git init -q "$R" || exit 1
(cd "$root" && git ls-files -z -c -o --exclude-standard | tar --null -T - -cf -) | (cd "$R" && tar -xf -) ||
    { echo "FAIL - cannot copy the tree"; exit 1; }
git -C "$R" add -A && git -C "$R" commit -q -m "this tree" || exit 1
VERSION=$(sed -n "s/^  version : '\([^']*\)',$/\1/p" "$R/meson.build")
TAG=shellter-v$VERSION
[ -n "$VERSION" ] || { echo "FAIL - no version in meson.build"; exit 1; }

# the platform as the release format defines it (docs/shellter/releasing.md),
# independently of the packager
# shellcheck disable=SC1091
case $(uname -s) in
    Linux) PLATFORM=$(. /etc/os-release && printf '%s%s' "$ID" "${VERSION_ID%%.*}") ;;
    FreeBSD) PLATFORM=freebsd$(freebsd-version -u | cut -d. -f1) ;;
    *) echo "skip: no packages for $(uname -s)"; exit 77 ;;
esac
case $(uname -m) in
    x86_64|amd64) PLATFORM=$PLATFORM-amd64 ;;
    aarch64|arm64) PLATFORM=$PLATFORM-arm64 ;;
    *) echo "skip: no packages for $(uname -m)"; exit 77 ;;
esac
NAME=erssi-$VERSION-$PLATFORM

# run the packager: RC, its output in $tmp/out.N, the package dir $tmp/pkgs.N
n=0
run_bb() {
    n=$((n + 1))
    OUTDIR=$tmp/pkgs.$n
    sh "$BB" --repo "$R" --out "$OUTDIR" --work "$tmp/work.$n" "$@" > "$tmp/out.$n" 2>&1
    RC=$?
    OUT=$(cat "$tmp/out.$n")
}
nothing_built() {
    [ -z "$(ls -A "$OUTDIR" 2>/dev/null)" ] && [ ! -e "$tmp/work.$n" ] && ! grep -q 'configuring' "$tmp/out.$n"
}
refused() {   # description pattern
    if [ "$RC" = 1 ] && printf '%s\n' "$OUT" | grep -q "$2" && nothing_built; then
        ok "$1"
    else
        bad "$1 (exit $RC): $OUT"
    fi
}

sha256_hex() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1; else sha256 -q; fi
}

case_verify() {
    git -C "$R" tag light-tag && git -C "$R" tag shellter-v2026.1.0 || exit 1
    run_bb shellter-v2026.1.0
    refused "a lightweight tag" "not an annotated tag signed by the release key"
    git -C "$R" tag -a -m unsigned shellter-v2026.1.1 || exit 1
    run_bb shellter-v2026.1.1
    refused "an unsigned annotated tag" "not an annotated tag signed by the release key"
    git -C "$R" -c user.signingkey="$tmp/other" tag -s -m other shellter-v2026.1.2 || exit 1
    run_bb shellter-v2026.1.2
    refused "a tag signed by another key" "not an annotated tag signed by the release key"
    # a signed tag object named for another release, under this name
    git -C "$R" tag -s -m renamed shellter-v2026.1.3 &&
        git -C "$R" update-ref refs/tags/shellter-v2026.1.4 "$(git -C "$R" rev-parse shellter-v2026.1.3)" || exit 1
    run_bb shellter-v2026.1.4
    refused "a signed tag under another name" "not an annotated tag signed by the release key"
    run_bb light-tag
    refused "not a release tag name" "not a release tag"
    run_bb shellter-v9999.1.0
    refused "a tag that is not there" "not an annotated tag signed by the release key"
    # signed, but the version of the tree is another one
    git -C "$R" tag -s -m wrong shellter-v2026.1.5 || exit 1
    run_bb shellter-v2026.1.5
    if [ "$RC" = 1 ] && printf '%s\n' "$OUT" | grep -q "does not check out - no package" &&
            [ -z "$(ls -A "$OUTDIR")" ] && [ ! -e "$tmp/work.$n" ]; then
        ok "a signed tag of another version"
    else
        bad "a signed tag of another version (exit $RC): $OUT"
    fi

    # SHA256SUMS over the tarballs of one release, sorted, signed and
    # checked with the commands of releasing.md
    D=$tmp/release
    mkdir "$D" && echo one > "$D/erssi-2026.10.4-freebsd14-amd64.tar.xz" &&
        echo two > "$D/erssi-2026.10.4-debian13-amd64.tar.xz" && echo three > "$D/notes.txt" || exit 1
    sh "$BB" --sums "$D" > "$tmp/sums.out" 2>&1
    check "--sums succeeds" [ $? = 0 ]
    check "--sums writes SHA256SUMS" [ -f "$D/SHA256SUMS" ]
    want=$(for f in erssi-2026.10.4-debian13-amd64.tar.xz erssi-2026.10.4-freebsd14-amd64.tar.xz; do
        printf '%s  %s\n' "$(sha256_hex < "$D/$f")" "$f"; done)
    check "SHA256SUMS: the tarballs only, sorted, '<sha256>  <name>'" [ "$(cat "$D/SHA256SUMS")" = "$want" ]
    check "--sums prints the signing command" grep -q 'ssh-keygen -Y sign -n erssi-release -f <release key> SHA256SUMS' "$tmp/sums.out"
    (cd "$D" && ssh-keygen -Y sign -n erssi-release -f "$tmp/key" SHA256SUMS) 2>/dev/null
    check "signed into SHA256SUMS.sig (armored)" grep -q '^-----BEGIN SSH SIGNATURE-----$' "$D/SHA256SUMS.sig"
    printf '%s namespaces="erssi-release" %s\n' "$EMAIL" "$new_key" > "$tmp/release_signers"
    ssh-keygen -Y verify -f "$tmp/release_signers" -I "$EMAIL" -n erssi-release -s "$D/SHA256SUMS.sig" \
        < "$D/SHA256SUMS" > /dev/null 2>&1
    check "the signature verifies" [ $? = 0 ]
    printf 'x\n' >> "$D/SHA256SUMS"
    ! ssh-keygen -Y verify -f "$tmp/release_signers" -I "$EMAIL" -n erssi-release -s "$D/SHA256SUMS.sig" \
        < "$D/SHA256SUMS" > /dev/null 2>&1
    check "a changed SHA256SUMS does not verify" [ $? = 0 ]
    echo three > "$D/erssi-2026.10.5-debian13-amd64.tar.xz"
    sh "$BB" --sums "$D" > "$tmp/sums.out" 2>&1
    check "--sums refuses two releases" [ $? = 1 ]
    check "--sums names them" grep -q 'more than one release in .*: 2026.10.4 2026.10.5' "$tmp/sums.out"
    rm -f "$D"/erssi-* "$D/SHA256SUMS"
    sh "$BB" --sums "$D" > "$tmp/sums.out" 2>&1
    check "--sums refuses a directory without tarballs" [ $? = 1 ]
    check "--sums writes no SHA256SUMS then" [ ! -e "$D/SHA256SUMS" ]
}

# the entries of tarball $1: name uid gid mtime, one per line, in order
tar_entries() {
    python3 -c '
import sys, tarfile
with tarfile.open(sys.argv[1]) as t:
    for m in t.getmembers():
        print(m.name, m.uid, m.gid, m.mtime, m.uname or "-", m.gname or "-")' "$1"
}
# the links of tarball $1 that the installer refuses: a symlink whose
# target is absolute or has a ".." component, a hard link out of the top
# directory; one per line
tar_bad_links() {
    python3 -c '
import sys, tarfile
with tarfile.open(sys.argv[1]) as t:
    for m in t.getmembers():
        top = m.name.split("/")[0]
        if m.issym() and (m.linkname.startswith("/") or ".." in m.linkname.split("/")):
            print(m.name, "->", m.linkname)
        elif m.islnk() and not m.linkname.startswith(top + "/"):
            print(m.name, "->", m.linkname)' "$1"
}
manifest() { sed -n "s/^$1=//p" "$M"; }

case_package() {
    command -v python3 >/dev/null 2>&1 || { echo "skip: no python3"; exit 77; }
    git -C "$R" tag -s -m "erssi Shellter Edition $VERSION" "$TAG" || exit 1
    COMMIT=$(git -C "$R" rev-parse "$TAG^{commit}")
    EPOCH=$(git -C "$R" log -1 --format=%ct "$COMMIT")
    BUILT=$(date -u -d "@$EPOCH" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u -r "$EPOCH" +%Y-%m-%dT%H:%M:%SZ)
    export BUILD_BINARY_TESTS="live relocatable"
    run_bb "$TAG"
    check "the package is built (exit $RC)" [ "$RC" = 0 ]
    [ "$RC" = 0 ] || { printf '%s\n' "$OUT" | tail -n 30; return; }
    T=$OUTDIR/$NAME.tar.xz
    check "only $NAME.tar.xz is written" [ "$(ls -A "$OUTDIR")" = "$NAME.tar.xz" ]
    check "the work directory is removed" [ ! -e "$tmp/work.$n" ]
    check "it says it ran only BUILD_BINARY_TESTS" grep -q 'WARNING: only the tests live relocatable' "$tmp/out.$n"
    check "the relocatable live test ran in the build" grep -q 'tests: Ok: 1 Fail: 0' "$tmp/out.$n"

    tar_entries "$T" > "$tmp/entries" || { bad "the tarball cannot be read"; return; }
    check "one top directory $NAME/" awk -v n="$NAME" '$1 != n && index($1, n "/") != 1 { exit 1 }' "$tmp/entries"
    check "the top directory comes first" [ "$(head -n 1 "$tmp/entries" | cut -d' ' -f1)" = "$NAME" ]
    cut -d' ' -f1 "$tmp/entries" > "$tmp/names"
    LC_ALL=C sort "$tmp/names" > "$tmp/names.sorted"
    check "the entries are sorted" cmp -s "$tmp/names" "$tmp/names.sorted"
    check "every entry is owned by 0:0, no names" awk '$2 != 0 || $3 != 0 || ($5 != "-" && $5 != "root") || ($6 != "-" && $6 != "root" && $6 != "wheel") { exit 1 }' "$tmp/entries"
    check "no link is absolute or has a .. in it" [ -z "$(tar_bad_links "$T")" ]
    check "every entry is dated to the commit" awk -v t="$EPOCH" '$4 != t { exit 1 }' "$tmp/entries"

    X=$tmp/x1
    if ! { mkdir "$X" && (cd "$X" && xz -dc "$T" | tar -xf -); }; then bad "unpacking"; return; fi
    P=$X/$NAME
    M=$P/MANIFEST
    check "MANIFEST: the keys of the release format, sorted" \
        [ "$(cut -d= -f1 "$M" | tr '\n' ' ')" = "built commit libc needed perl_version platform tag version " ]
    check "MANIFEST: version" [ "$(manifest version)" = "$VERSION" ]
    check "MANIFEST: tag" [ "$(manifest tag)" = "$TAG" ]
    check "MANIFEST: commit" [ "$(manifest commit)" = "$COMMIT" ]
    check "MANIFEST: platform" [ "$(manifest platform)" = "$PLATFORM" ]
    check "MANIFEST: perl_version" [ "$(manifest perl_version)" = "$(perl -e 'print $^V' | sed 's/^v//')" ]
    case $(uname -s) in
        Linux) libc=$(getconf GNU_LIBC_VERSION) ;;
        FreeBSD) libc="freebsd $(freebsd-version -u | sed 's/-.*//')" ;;
    esac
    check "MANIFEST: libc ($libc)" [ "$(manifest libc)" = "$libc" ]
    check "MANIFEST: built is the commit time ($BUILT)" [ "$(manifest built)" = "$BUILT" ]
    needed=$(manifest needed)
    check "MANIFEST: needed are sonames" sh -c 'for l in $1; do case $l in */*|"") exit 1 ;; *.so*) ;; *) exit 1 ;; esac; done' sh "$needed"
    check "MANIFEST: needed has libc and glib" sh -c 'case " $1 " in *" libc.so."*) ;; *) exit 1 ;; esac
        case " $1 " in *" libglib-2.0.so.0 "*) ;; *) exit 1 ;; esac' sh "$needed"
    check "MANIFEST: needed names no library of the package" sh -c 'for l in $1; do [ -z "$(find "$2" -name "$l")" ] || exit 1; done' sh "$needed" "$P"
    check "MANIFEST: needed is sorted" [ "$(printf '%s\n' "$needed" | tr ' ' '\n' | LC_ALL=C sort -u | tr '\n' ' ')" = "$needed " ]
    check "the binary is stripped" sh -c '! readelf -S "$1" | grep -q "\.symtab"' sh "$P/bin/erssi"
    check "the modules are in the package" [ -n "$(find "$P" -name libe2e_core.so)" -a -n "$(find "$P" -name libfe_web.so)" -a -n "$(find "$P" -name Irssi.so)" ]
    check "the Perl modules are in the package" [ -n "$(find "$P" -name Irssi.pm)" ]

    # the content runs, from two places it was unpacked to
    Y=$tmp/elsewhere/y2
    if ! { mkdir -p "$Y" && (cd "$Y" && xz -dc "$T" | tar -xf -); }; then bad "unpacking again"; return; fi
    for p in "$P" "$Y/$NAME"; do
        real=$(cd "$p" && pwd -P)
        v=$(HOME="$tmp/run-home" "$p/bin/erssi" --version 2>&1)
        check "$p: --version" sh -c 'printf "%s\n" "$1" | grep -q "^erssi $2 ("' sh "$v" "$VERSION"
        check "$p: --print-prefix" [ "$(HOME="$tmp/run-home" "$p/bin/erssi" --print-prefix 2>&1)" = "$real" ]
        if command -v ldd >/dev/null 2>&1; then
            check "$p: every library is found" sh -c '! ldd "$1" 2>&1 | grep -q "not found"' sh "$p/bin/erssi"
        fi
    done
    check "erssi wrote nothing to its HOME" [ ! -e "$tmp/run-home" ]
}

case_tests_fail() {
    # the version tree test fails on this commit
    printf '#!/bin/sh\necho "FAIL - this commit"\nexit 1\n' > "$R/tests/version/test-version-tree.sh"
    git -C "$R" commit -q -a -m "a failing test" && git -C "$R" tag -s -m "erssi Shellter Edition $VERSION" "$TAG" || exit 1
    export BUILD_BINARY_TESTS="version tree test"
    run_bb "$TAG"
    if [ "$RC" = 1 ] && printf '%s\n' "$OUT" | grep -q "tests failed - no package" &&
            printf '%s\n' "$OUT" | grep -q "version tree test" &&
            [ -z "$(ls -A "$OUTDIR")" ] && [ ! -e "$tmp/work.$n" ]; then
        ok "a failed test: no package, the test named"
    else
        bad "a failed test (exit $RC): $OUT"
    fi
}

case $CASE in
    verify) case_verify ;;
    package) case_package ;;
    tests-fail) case_tests_fail ;;
    *) echo "unknown case $CASE"; exit 1 ;;
esac
exit $failed
