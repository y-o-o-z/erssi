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
#               two places it is unpacked to, live in one of them
#               (tests/live/test_relocatable.py --tree); the build's own tests run are
#               the relocatable live test and a fuzz corpus test whose
#               program is not built by default (BUILD_BINARY_TESTS)
#   tests-fail  a tag whose tests fail: no package
#   reproduce   --verify: the package built again in another work directory
#               is identical (the same sha256), a changed package is reported
#               with the file that differs, and the refusals (no package, a
#               package of another platform) before any build
#   skips       the two cases above skip without ERSSI_TEST_PACKAGER=1, and
#               every case skips when a tool of the packager is not in its
#               PATH (TEST_PACKAGER_PATH stands in for that PATH), and
#               fails then when ERSSI_TEST_PACKAGER=1 asks for it
# package and tests-fail build the tree from scratch twice, with every
# feature: they run only with ERSSI_TEST_PACKAGER=1 (utils/pre-push, CI,
# the release host), not in the source build of an install or an update.
# Every case needs the packager's tools in the packager's own PATH
# (CLEAN_PATH of build-binary.sh): a box where meson or ninja is only in
# the installer's venv, say, skips them; with ERSSI_TEST_PACKAGER=1 a
# missing tool is a failure instead.
# Exit: 0 pass, 1 fail, 77 skip.
# shellcheck disable=SC2016 # the awk programs and sh -c scripts take $1 themselves

root=$(cd "$1" && pwd) || exit 1
CASE=$2

tools=$(sed -n 's/^for t in \(.*\); do$/\1/p' "$root/utils/build-binary.sh")
# the gates, before the run again in an empty environment below (which
# drops these variables)
if [ -z "${BUILD_BINARY_TEST_CLEAN:-}" ]; then
    case $CASE in
        package|tests-fail|reproduce)
            # a package build runs the tests of what it builds: not this one again
            if [ -n "${ERSSI_BUILD_BINARY:-}" ]; then
                echo "skip: inside a package build"; exit 77
            fi
            if [ "${ERSSI_TEST_PACKAGER:-}" != 1 ]; then
                echo "skip: builds a package from scratch - only with ERSSI_TEST_PACKAGER=1"; exit 77
            fi ;;
    esac
    # the tools the packager needs, in the PATH it runs with
    packager_path=${TEST_PACKAGER_PATH:-$(sed -n 's/^CLEAN_PATH=//p' "$root/utils/build-binary.sh")}
    if [ -z "$packager_path" ] || [ -z "$tools" ]; then
        echo "FAIL - no CLEAN_PATH or tool list in build-binary.sh"; exit 1
    fi
    if [ "$CASE" != skips ]; then
        for t in $tools ssh-keygen; do
            PATH=$packager_path command -v "$t" >/dev/null 2>&1 && continue
            # asked for (pre-push, CI, the release host): a missing tool is
            # a failure there, or the package builds would be lost unnoticed
            if [ "${ERSSI_TEST_PACKAGER:-}" = 1 ]; then
                echo "FAIL - $t is not in the packager's PATH ($packager_path), and ERSSI_TEST_PACKAGER=1 asks for these tests"; exit 1
            fi
            echo "skip: $t is not in the packager's PATH ($packager_path)"; exit 77
        done
    fi
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
# a release in the making has NEWS dated YYYY-MM-xx, which check-version
# refuses for a tag: the copy is dated to the 1st of that month
sed '1s/^\(shellter-v[^ ]* [0-9]\{4\}-[0-9]\{2\}\)-xx /\1-01 /' "$R/NEWS" > "$tmp/NEWS" &&
    cat "$tmp/NEWS" > "$R/NEWS" || exit 1
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

    # the checks of an installed tree (--check-tree, as in a package
    # build): needed= from GNU readelf's output and from FreeBSD's (ELF
    # Tool Chain, NEEDED without parentheses), the package's own library
    # left out; a link that is absolute or climbs is refused
    TR=$tmp/tree
    mkdir -p "$TR/bin" "$TR/lib" "$tmp/stub-gnu" "$tmp/stub-etc" || exit 1
    printf '\177ELF fake\n' > "$TR/bin/erssi" && chmod 755 "$TR/bin/erssi" &&
        printf '\177ELF fake\n' > "$TR/lib/libown.so.1" && ln -s erssi "$TR/bin/erssi-link" || exit 1
    cat > "$tmp/stub-gnu/readelf" <<'EOF'
#!/bin/sh
echo 'Dynamic section at offset 0x2d8e0 contains 30 entries:'
echo '  Tag        Type                         Name/Value'
case $2 in
    */bin/erssi)
        echo ' 0x0000000000000001 (NEEDED)             Shared library: [libglib-2.0.so.0]'
        echo ' 0x0000000000000001 (NEEDED)             Shared library: [libown.so.1]'
        echo ' 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]' ;;
    *)
        echo ' 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]'
        echo ' 0x000000000000000e (SONAME)             Library soname: [libown.so.1]' ;;
esac
echo ' 0x000000000000000c (INIT)               0x5000'
EOF
    cat > "$tmp/stub-etc/readelf" <<'EOF'
#!/bin/sh
echo
echo 'Dynamic section at offset 0x3b10 contains 26 entries:'
echo '  Tag                Type                 Name/Value'
case $2 in
    */bin/erssi)
        echo '  0x0000000000000001 NEEDED               Shared library: [libglib-2.0.so.0]'
        echo '  0x0000000000000001 NEEDED               Shared library: [libown.so.1]'
        echo '  0x0000000000000001 NEEDED               Shared library: [libc.so.7]' ;;
    *)
        echo '  0x0000000000000001 NEEDED               Shared library: [libc.so.7]'
        echo '  0x000000000000000e SONAME               Library soname: [libown.so.1]' ;;
esac
echo '  0x000000000000000c INIT                 0x5000'
EOF
    chmod 755 "$tmp/stub-gnu/readelf" "$tmp/stub-etc/readelf"
    out=$(PATH="$tmp/stub-gnu:$PATH" sh "$BB" --check-tree "$TR" 2>&1)
    check "--check-tree: needed from GNU readelf" [ "$out" = "needed=libc.so.6 libglib-2.0.so.0" ]
    out=$(PATH="$tmp/stub-etc:$PATH" sh "$BB" --check-tree "$TR" 2>&1)
    check "--check-tree: needed from FreeBSD's readelf" [ "$out" = "needed=libc.so.7 libglib-2.0.so.0" ]
    for l in /etc/passwd ../lib ../../x a/.. ./../lib; do
        ln -sfn "$l" "$TR/bin/bad" || exit 1
        out=$(PATH="$tmp/stub-gnu:$PATH" sh "$BB" --check-tree "$TR" 2>&1)
        check "--check-tree: a link to $l is refused" sh -c 'printf "%s\n" "$1" | grep -q "a link is absolute or has a .. in it: bin/bad -> "' sh "$out"
    done
    unlink "$TR/bin/bad"
    # an RPATH or RUNPATH is refused, in both readelf formats, unless it is
    # the system perl's CORE directory; also in a real ELF file
    perl_core=$(perl -MConfig -e 'print "$Config{archlibexp}/CORE"')
    for f in "gnu (RUNPATH)            Library runpath: [/home/someone/lib]" \
             "etc RPATH                Library rpath: [/home/someone/lib:/usr/lib]" \
             "gnu (RPATH)              Library rpath: [$perl_core]"; do
        # shellcheck disable=SC2086 # the format, then the line
        set -- $f
        kind=$1; shift
        mkdir -p "$tmp/stub-rpath" && sed '/INIT/d' "$tmp/stub-$kind/readelf" > "$tmp/stub-rpath/readelf" &&
            printf "echo '  0x000000000000001d %s'\n" "$*" >> "$tmp/stub-rpath/readelf" &&
            chmod 755 "$tmp/stub-rpath/readelf" || exit 1
        out=$(PATH="$tmp/stub-rpath:$PATH" sh "$BB" --check-tree "$TR" 2>&1)
        case $* in
            *"$perl_core"*) check "--check-tree: the system perl's CORE directory passes" [ "$out" = "needed=$(case $kind in gnu) echo libc.so.6 ;; *) echo libc.so.7 ;; esac) libglib-2.0.so.0" ] ;;
            *) check "--check-tree: refused: $*" sh -c 'printf "%s\n" "$1" | grep -q "an RPATH or RUNPATH in the package - no package: .*/home/someone/lib"' sh "$out" ;;
        esac
    done
    # a perl whose CORE directory climbs out of /usr with ".." is no
    # exception: its RUNPATH is refused like any other
    for core in /usr/../home/someone/CORE /usr/lib/perl/../../../home/someone/CORE; do
        mkdir -p "$tmp/stub-perl" && printf '#!/bin/sh\nprintf %%s %s\n' "$core" > "$tmp/stub-perl/perl" &&
            chmod 755 "$tmp/stub-perl/perl" && sed '/INIT/d' "$tmp/stub-gnu/readelf" > "$tmp/stub-rpath/readelf" &&
            printf "echo '  0x000000000000001d (RUNPATH)            Library runpath: [%s]'\n" "$core" >> "$tmp/stub-rpath/readelf" ||
            exit 1
        out=$(PATH="$tmp/stub-perl:$tmp/stub-rpath:$PATH" sh "$BB" --check-tree "$TR" 2>&1)
        check "--check-tree: a perl CORE of $core is refused" sh -c 'printf "%s\n" "$1" | grep -qF "an RPATH or RUNPATH in the package - no package: bin/erssi: $2"' sh "$out" "$core"
    done
    if command -v cc >/dev/null 2>&1 && command -v readelf >/dev/null 2>&1; then
        RT=$tmp/rtree
        mkdir -p "$RT/bin" && printf 'int main(void) { return 0; }\n' > "$tmp/t.c" &&
            cc -o "$RT/bin/erssi" "$tmp/t.c" -Wl,-rpath,/home/someone/lib 2> /dev/null &&
            readelf -d "$RT/bin/erssi" | grep -q 'someone' || exit 1
        out=$(sh "$BB" --check-tree "$RT" 2>&1)
        check "--check-tree: an ELF file linked with -rpath is refused" sh -c 'printf "%s\n" "$1" | grep -q "an RPATH or RUNPATH in the package - no package: bin/erssi: /home/someone/lib"' sh "$out"
    fi

    # a meson install that put a file outside the prefix is refused and
    # the file named; the prefix alone (and the directories to it) passes
    ST=$tmp/stage
    prefix=$(sed -n 's/^BUILD_PREFIX=//p' "$BB")
    mkdir -p "$ST$prefix/bin" && : > "$ST$prefix/bin/erssi" || exit 1
    sh "$BB" --check-stage "$ST" > "$tmp/stage.out" 2>&1
    check "--check-stage: only the prefix ($prefix) passes" [ $? = 0 ]
    mkdir -p "$ST/etc" && : > "$ST/etc/erssi.conf" || exit 1
    sh "$BB" --check-stage "$ST" > "$tmp/stage.out" 2>&1
    check "--check-stage: a file in /etc is refused" [ $? = 1 ]
    check "--check-stage: and named" grep -q "outside $prefix - no package: .*/etc/erssi.conf" "$tmp/stage.out"

    # packing (--pack, as a package build packs): a tar that fails after
    # writing its archive, as one does for an unreadable file or a full
    # disk, means no package - not a cut-off one, signed
    PK=$tmp/pack
    mkdir -p "$PK/tree/erssi-x/bin" "$PK/out" "$PK/work" "$PK/stub" && echo erssi > "$PK/tree/erssi-x/bin/erssi" || exit 1
    realtar=$(command -v tar)
    printf '#!/bin/sh\ncase " $* " in *" -cf "*) "%s" "$@"; exit 2 ;; esac\nexec "%s" "$@"\n' "$realtar" "$realtar" > "$PK/stub/tar" &&
        chmod 755 "$PK/stub/tar" || exit 1
    PATH="$PK/stub:$PATH" sh "$BB" --pack "$PK/tree/erssi-x" "$PK/out/erssi-x.tar.xz" "$PK/work" > "$tmp/pack.out" 2>&1
    check "--pack: a failing tar is a failure" [ $? = 1 ]
    check "... named" grep -q 'packing failed (tar)' "$tmp/pack.out"
    check "... no tarball" [ -z "$(ls -A "$PK/out")" ]
    sh "$BB" --pack "$PK/tree/erssi-x" "$PK/out/erssi-x.tar.xz" "$PK/work" > "$tmp/pack.out" 2>&1
    check "--pack: packed" [ $? = 0 ]
    check "... the tree in it, sorted" [ "$(xz -dc "$PK/out/erssi-x.tar.xz" | tar -tf - | sed 's|/$||' | tr '\n' ' ')" = "erssi-x erssi-x/bin erssi-x/bin/erssi " ]
    check "... nothing else left" sh -c '[ "$(ls -A "$1/out")" = erssi-x.tar.xz ] && [ ! -e "$1/work/package.tar" ]' sh "$PK"
    # the same with relative paths, from another directory
    rm -f "$PK/out/erssi-x.tar.xz"
    (cd "$PK" && sh "$BB" --pack tree/erssi-x out/erssi-x.tar.xz work) > "$tmp/pack.out" 2>&1
    check "--pack with relative paths: packed" [ $? = 0 ]
    check "... the tree in it" [ "$(xz -dc "$PK/out/erssi-x.tar.xz" | tar -tf - | sed 's|/$||' | tr '\n' ' ')" = "erssi-x erssi-x/bin erssi-x/bin/erssi " ]

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
    # the relocatable live test, and a test whose program ninja does not
    # build by default (meson test must build it, as for the full suite)
    export BUILD_BINARY_TESTS="live relocatable,fe-web fuzz corpus handshake"
    # a caller's environment that would break the build or leak into it:
    # the packager runs in an empty one
    export CC=false LDFLAGS=-Wl,-rpath,/home/someone/lib CFLAGS=-DLEAKED_CFLAGS \
        PKG_CONFIG_PATH=/home/someone/pkgconfig LD_LIBRARY_PATH=/home/someone/lib \
        PERL5LIB=/home/someone/perl5 RPE2E_SCRIPT=/home/someone/rpe2e.pl REPARTEE_BIN=/home/someone/repartee
    run_bb "$TAG"
    unset CC LDFLAGS CFLAGS PKG_CONFIG_PATH LD_LIBRARY_PATH PERL5LIB RPE2E_SCRIPT REPARTEE_BIN
    check "the package is built (exit $RC)" [ "$RC" = 0 ]
    [ "$RC" = 0 ] || { printf '%s\n' "$OUT" | tail -n 30; return; }
    T=$OUTDIR/$NAME.tar.xz
    check "only $NAME.tar.xz is written" [ "$(ls -A "$OUTDIR")" = "$NAME.tar.xz" ]
    check "the work directory is removed" [ ! -e "$tmp/work.$n" ]
    check "it says it ran only BUILD_BINARY_TESTS" grep -q 'WARNING: only the tests live relocatable,fe-web fuzz corpus handshake' "$tmp/out.$n"
    check "both tests ran in the build" grep -q 'tests: Ok: 2 Fail: 0' "$tmp/out.$n"

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
        [ "$(cut -d= -f1 "$M" | tr '\n' ' ')" = "built cc commit libc libs meson needed ninja perl_version platform strip tag version xz " ]
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
    # the tools of the packager's own PATH, not of the caller's
    cp=$(sed -n 's/^CLEAN_PATH=//p' "$root/utils/build-binary.sh")
    check "MANIFEST: cc is the compiler's version" [ "$(manifest cc)" = "$(PATH=$cp cc --version | head -n 1)" ]
    check "MANIFEST: meson and ninja versions" [ "$(manifest meson)" = "$(PATH=$cp meson --version)" -a "$(manifest ninja)" = "$(PATH=$cp ninja --version)" ]
    check "MANIFEST: strip and xz versions" [ "$(manifest strip)" = "$(PATH=$cp strip --version | head -n 1)" -a "$(manifest xz)" = "$(PATH=$cp xz --version | head -n 1)" ]
    check "MANIFEST: libs has glib's version" sh -c 'case " $1 " in *" glib-2.0=$2 "*) ;; *) exit 1 ;; esac' sh "$(manifest libs)" "$(PATH=$cp pkg-config --modversion glib-2.0)"
    needed=$(manifest needed)
    check "MANIFEST: needed are sonames" sh -c 'for l in $1; do case $l in */*|"") exit 1 ;; *.so*) ;; *) exit 1 ;; esac; done' sh "$needed"
    check "MANIFEST: needed has libc and glib" sh -c 'case " $1 " in *" libc.so."*) ;; *) exit 1 ;; esac
        case " $1 " in *" libglib-2.0.so.0 "*) ;; *) exit 1 ;; esac' sh "$needed"
    check "MANIFEST: needed names no library of the package" sh -c 'for l in $1; do [ -z "$(find "$2" -name "$l")" ] || exit 1; done' sh "$needed" "$P"
    check "MANIFEST: needed is sorted" [ "$(printf '%s\n' "$needed" | tr ' ' '\n' | LC_ALL=C sort -u | tr '\n' ' ')" = "$needed " ]
    check "no RPATH or RUNPATH of the caller's LDFLAGS" sh -c 'find "$1" -type f -name "*.so" -o -type f -name erssi | while read -r f; do readelf -d "$f"; done | grep -q someone && exit 1; exit 0' sh "$P"
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
    # the stripped tree as unpacked, live: the themes, /help, a module and
    # a Perl script from there, --print-prefix the unpacked directory
    PYTHONDONTWRITEBYTECODE=1 python3 "$root/tests/live/test_relocatable.py" --tree "$Y/$NAME" > "$tmp/live.out" 2>&1
    rc=$?
    check "live in the unpacked tree (tests/live/test_relocatable.py --tree)" [ "$rc" = 0 ]
    [ "$rc" = 0 ] || cat "$tmp/live.out"
    check "the live test ran its case" grep -q '^  ok   t_unpacked_tree$' "$tmp/live.out"
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

# --verify takes no --out: as run_bb, without it
run_verify() {
    n=$((n + 1))
    OUTDIR=$tmp/pkgs.$n
    sh "$BB" --repo "$R" --work "$tmp/work.$n" --verify "$1" > "$tmp/out.$n" 2>&1
    RC=$?
    OUT=$(cat "$tmp/out.$n")
}
case_reproduce() {
    # refused before any build: not a package, and a package of another platform
    printf 'not a package\n' > "$tmp/junk.tar.xz"
    run_verify "$tmp/junk.tar.xz"
    refused "--verify: a file that is not a package" "is not a package"
    mkdir -p "$tmp/otherplat/erssi-$VERSION-other9-amd64" || exit 1
    printf 'platform=other9-amd64\ntag=%s\n' "$TAG" > "$tmp/otherplat/erssi-$VERSION-other9-amd64/MANIFEST"
    (cd "$tmp/otherplat" && tar -cf - "erssi-$VERSION-other9-amd64" | xz -c > "$tmp/otherplat.tar.xz") || exit 1
    run_verify "$tmp/otherplat.tar.xz"
    refused "--verify: a package of another platform" "is for other9-amd64, this is $PLATFORM"

    # --verify builds with the tag's own packager: in this repository the
    # copy that trusts the throwaway key
    cp "$BB" "$R/utils/build-binary.sh" && git -C "$R" commit -q -a -m "the packager of the test key" || exit 1
    git -C "$R" tag -s -m "erssi Shellter Edition $VERSION" "$TAG" || exit 1
    # one quick test in each build: what is compared is the package
    export BUILD_BINARY_TESTS="test-formats test"
    run_bb "$TAG"
    check "the package is built (exit $RC)" [ "$RC" = 0 ]
    [ "$RC" = 0 ] || { printf '%s\n' "$OUT" | tail -n 30; return; }
    T=$OUTDIR/$NAME.tar.xz
    SUM=$(sha256_hex < "$T")

    # built again from scratch, in another work directory (another path)
    run_verify "$T"
    check "--verify: the package is reproduced (exit $RC)" [ "$RC" = 0 ]
    check "--verify: says reproduced, with the sha256" sh -c 'printf "%s\n" "$1" | grep -q "reproduced: $2 is identical" && printf "%s\n" "$1" | grep -q "sha256 $3"' sh "$OUT" "$NAME.tar.xz" "$SUM"
    check "--verify: writes nothing but its work directory, which goes" sh -c '[ -z "$(ls -A "$1" 2>/dev/null)" ] && [ ! -e "$2" ]' sh "$OUTDIR" "$tmp/work.$n"
    [ "$RC" = 0 ] || printf '%s\n' "$OUT" | tail -n 30

    # a package changed after the build: one file of it
    X=$tmp/changed
    if ! { mkdir "$X" && (cd "$X" && xz -dc "$T" | tar -xf -); }; then bad "unpacking"; return; fi
    changed=$(cd "$X/$NAME" && find share -type f | LC_ALL=C sort | head -n 1)
    [ -n "$changed" ] || { bad "no file under share/ in the package"; return; }
    printf 'changed\n' >> "$X/$NAME/$changed" || exit 1
    mkdir "$tmp/changed-out" || exit 1
    sh "$BB" --pack "$X/$NAME" "$tmp/changed-out/$NAME.tar.xz" "$tmp/changed-out" > /dev/null 2>&1 || { bad "repacking"; return; }
    run_verify "$tmp/changed-out/$NAME.tar.xz"
    check "--verify: a changed package is not reproduced (exit $RC)" [ "$RC" = 3 ]
    check "--verify: says so" sh -c 'printf "%s\n" "$1" | grep -q "NOT reproduced"' sh "$OUT"
    check "--verify: names the file that differs ($changed)" sh -c 'printf "%s\n" "$1" | grep -q "differs: *$2/$3"' sh "$OUT" "$NAME" "$changed"
    [ "$RC" = 3 ] || printf '%s\n' "$OUT" | tail -n 30
    # usage: no --out, no --sums with --verify
    sh "$BB" --verify "$T" --out "$tmp/x" > "$tmp/usage.out" 2>&1
    check "--verify with --out: usage error (exit $?)" grep -q -- '--verify writes no package: no --out' "$tmp/usage.out"
    sh "$BB" --sums "$tmp/x" --verify "$T" > "$tmp/usage.out" 2>&1
    check "--verify with --sums: usage error" grep -q -- '--sums and --verify do not go together' "$tmp/usage.out"
}

# the skips: run this script again as a plain meson test would (no
# ERSSI_TEST_PACKAGER), and with a packager PATH that lacks the tools
case_skips() {
    for c in package tests-fail reproduce; do
        out=$(env -i PATH="$PATH" TMPDIR="$TMPDIR" sh "$0" "$root" "$c" 2>&1)
        rc=$?
        check "$c skips without ERSSI_TEST_PACKAGER (exit $rc)" [ "$rc" = 77 ]
        check "$c says why" [ "$out" = "skip: builds a package from scratch - only with ERSSI_TEST_PACKAGER=1" ]
        out=$(env -i PATH="$PATH" TMPDIR="$TMPDIR" ERSSI_TEST_PACKAGER=1 ERSSI_BUILD_BINARY=1 sh "$0" "$root" "$c" 2>&1)
        check "$c skips inside a package build" [ "$?" = 77 ]
    done
    mkdir -p "$tmp/emptybin" || exit 1
    for c in verify package tests-fail; do
        out=$(env -i PATH="$PATH" TMPDIR="$TMPDIR" ERSSI_TEST_PACKAGER=1 TEST_PACKAGER_PATH="$tmp/emptybin" \
            sh "$0" "$root" "$c" 2>&1)
        rc=$?
        check "$c fails without the packager's tools when asked for (exit $rc)" [ "$rc" = 1 ]
        check "$c names the tool" sh -c 'case $1 in "FAIL - git is not in the packager'"'"'s PATH ($2), and ERSSI_TEST_PACKAGER=1 asks for these tests") ;; *) exit 1 ;; esac' sh "$out" "$tmp/emptybin"
    done
    out=$(env -i PATH="$PATH" TMPDIR="$TMPDIR" TEST_PACKAGER_PATH="$tmp/emptybin" sh "$0" "$root" verify 2>&1)
    rc=$?
    check "verify skips without the packager's tools when not asked for (exit $rc)" [ "$rc" = 77 ]
    check "verify says why" [ "$out" = "skip: git is not in the packager's PATH ($tmp/emptybin)" ]
    # only meson missing, as with a meson in the installer's venv
    mkdir -p "$tmp/nomeson" || exit 1
    for t in $tools ssh-keygen; do
        [ "$t" = meson ] && continue
        p=$(command -v "$t") || continue
        ln -s "$p" "$tmp/nomeson/$t" || exit 1
    done
    out=$(env -i PATH="$PATH" TMPDIR="$TMPDIR" TEST_PACKAGER_PATH="$tmp/nomeson" sh "$0" "$root" verify 2>&1)
    rc=$?
    check "verify skips when only meson is not in the packager's PATH (exit $rc)" [ "$rc" = 77 ]
    check "verify names meson" [ "$out" = "skip: meson is not in the packager's PATH ($tmp/nomeson)" ]
}

case $CASE in
    skips) case_skips ;;
    verify) case_verify ;;
    package) case_package ;;
    tests-fail) case_tests_fail ;;
    reproduce) case_reproduce ;;
    *) echo "unknown case $CASE"; exit 1 ;;
esac
exit $failed
