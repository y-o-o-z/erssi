#!/bin/sh
# utils/build-binary.sh: the binary package of a signed erssi Shellter
# Edition release, for the platform it runs on (docs/shellter/releasing.md).
#
# Usage:
#   build-binary.sh [--repo DIR] [--out DIR] [--work DIR] [--keep-work] TAG
#       Builds erssi-<version>-<platform>.tar.xz in --out (default: the
#       current directory) from the release tag TAG (shellter-vYYYY.M.PATCH)
#       of the repository DIR (default: the one this script is in):
#       - the tag must be an annotated tag signed by RELEASE_SIGNER below,
#         the release key that shellter-install.sh also carries;
#       - a fresh clone at the tag's commit, a new build directory
#         (meson -Drelocatable=true -Dbuildtype=release, every feature
#         required), ninja and the full meson test suite: a failed test
#         means no package;
#       - installed into a staging directory, stripped, with a MANIFEST,
#         packed reproducibly: sorted, owner 0, every file dated to the
#         commit (SOURCE_DATE_EPOCH).
#       Everything happens in --work (default: a new temporary directory,
#       removed at the end unless --keep-work); HOME and TMPDIR of the
#       build and its tests are inside it, so ~/.erssi is never touched.
#       The script runs again in an empty environment (env -i) with
#       PATH=CLEAN_PATH below: no CC, CFLAGS, LDFLAGS, PKG_CONFIG_PATH,
#       LD_LIBRARY_PATH, PERL5LIB or test settings of the caller reach
#       the build or its tests, nor http(s)_proxy (behind a proxy, give
#       --repo a local clone). Only TMPDIR (where the work directory
#       goes) and BUILD_BINARY_TESTS are kept, with PATH, HOME=/nonexistent
#       and LC_ALL=C set. No ELF file of the package
#       may have an RPATH or RUNPATH, except the system perl's CORE
#       directory (FreeBSD's perl links libperl that way).
#   build-binary.sh [--repo DIR] [--work DIR] [--keep-work] --verify TARBALL
#       Builds the package TARBALL again here, from the release tag its
#       MANIFEST names, exactly as above (the same checks, the full test
#       suite), and compares the two: exit 0 when they are identical byte
#       for byte (the same sha256), 1 when they differ, with the files
#       that differ and the MANIFEST lines that tell the two builds'
#       compilers and libraries apart. TARBALL must be for this platform.
#   build-binary.sh --sums DIR
#       Writes DIR/SHA256SUMS over the tarballs in DIR (one release) and
#       prints the command that signs it.
#   build-binary.sh --check-stage DIR
#       Only the check that DIR (a meson install --destdir) holds nothing
#       outside the build prefix; for the packager's test.
#   build-binary.sh --check-tree DIR
#       Only the checks of an installed tree (as unpacked: bin/erssi,
#       lib/...) that a package build makes, and the needed= line of its
#       MANIFEST; for the packager's test.
#   build-binary.sh --pack DIR TARBALL WORK
#       Only the packing of the tree DIR into TARBALL (WORK for its
#       temporary files), as a package build packs; for the packager's
#       test.
#
# BUILD_BINARY_TESTS: the names of the tests to run (separated by commas)
# in place of the full suite, for the packager's own test
# (tests/version/test-build-binary.sh) only. A release never sets it.
#
# Exit: 0 package written, 1 refused or failed, 2 usage.

set -u

# who signs the releases (git allowed_signers format) and the key's
# fingerprint (ssh-keygen -lf): the same as in shellter-install.sh, which
# tests/version/test-build-binary.sh checks. A copy of its own, so that
# what is checked never comes from the tree being checked.
RELEASE_SIGNER='136268860+y-o-o-z@users.noreply.github.com namespaces="git" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJmzPHTiL6dPEntggsVu9kJyAsY9KLaKNUW5kejyJOYS'
RELEASE_FINGERPRINT='SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw'
# where the packages are configured: never used at run time unless the
# executable cannot find itself (src/core/relocate.c)
BUILD_PREFIX=/opt/erssi
# the PATH of the build: the system's tools only
CLEAN_PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin

say() { printf 'build-binary: %s\n' "$*"; }
die() { printf 'build-binary: %s\n' "$*" >&2; exit 1; }
usage() {
    printf 'build-binary: %s\n' "$1" >&2
    echo 'usage: build-binary.sh [--repo DIR] [--out DIR] [--work DIR] [--keep-work] TAG' >&2
    echo '       build-binary.sh [--repo DIR] [--work DIR] [--keep-work] --verify TARBALL' >&2
    echo '       build-binary.sh --sums DIR' >&2
    exit 2
}
have() { command -v "$1" >/dev/null 2>&1; }

# the sha256 of file $1 in hex
sha256_of() {
    if have sha256sum; then
        sha256sum < "$1" | awk '{print $1}'
    elif have sha256; then
        sha256 -q < "$1"
    elif have openssl; then
        openssl dgst -sha256 -r < "$1" | awk '{print $1}'
    else
        return 1
    fi
}

# ── SHA256SUMS ───────────────────────────────────────────────────────
write_sums() {
    dir=$1
    [ -d "$dir" ] || die "$dir is not a directory"
    names=$(cd "$dir" && for f in erssi-*.tar.xz; do [ -f "$f" ] && printf '%s\n' "$f"; done | LC_ALL=C sort)
    [ -n "$names" ] || die "no erssi-*.tar.xz in $dir"
    # one release per SHA256SUMS: erssi-<version>-<os><major>-<arch>.tar.xz
    re='^erssi-\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)-[a-z][a-z]*[0-9][0-9]*-[a-z0-9_]*\.tar\.xz$'
    bad=$(printf '%s\n' "$names" | grep -v "$re" | head -n 1)
    [ -z "$bad" ] || die "not a package name: $bad"
    versions=$(printf '%s\n' "$names" | sed "s/$re/\\1/" | sort -u | tr '\n' ' ' | sed 's/ $//')
    case $versions in
        *' '*) die "more than one release in $dir: $versions" ;;
    esac
    tmp="$dir/.SHA256SUMS.$$"
    : > "$tmp" || die "cannot write in $dir"
    for f in $names; do
        if ! sum=$(sha256_of "$dir/$f") || [ -z "$sum" ]; then
            rm -f "$tmp"
            die "cannot compute the sha256 of $f (sha256sum, sha256 or openssl needed)"
        fi
        printf '%s  %s\n' "$sum" "$f" >> "$tmp"
    done
    mv -f "$tmp" "$dir/SHA256SUMS" || die "cannot write $dir/SHA256SUMS"
    say "wrote $dir/SHA256SUMS ($(printf '%s\n' "$names" | wc -l | tr -d ' ') packages of $versions)"
    say "sign it with the release key:"
    say "  cd $dir && ssh-keygen -Y sign -n erssi-release -f <release key> SHA256SUMS"
}

# ── the release tag ──────────────────────────────────────────────────
# git 2.34+ (SSH signatures) and ssh-keygen
can_verify() {
    gv=$(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    # shellcheck disable=SC2086 # "2 47" -> $1 $2
    set -- $gv
    [ -n "${2:-}" ] && { [ "$1" -gt 2 ] || { [ "$1" -eq 2 ] && [ "$2" -ge 34 ]; }; } && have ssh-keygen
}
# refs/tags/$2 in repository $1 is an annotated tag named $2 with an SSH
# signature by RELEASE_SIGNER; TAG_OBJ is the tag object checked (read
# once). gpg and gpgsm are disabled - git picks the verifier from the
# signature, and an OpenPGP signature by any key in the user's keyring
# would otherwise pass.
signed_tag() {
    TAG_OBJ=$(git -C "$1" rev-parse --verify -q "refs/tags/$2") || return 1
    [ "$(git -C "$1" cat-file -t "$TAG_OBJ" 2>/dev/null)" = tag ] || return 1
    [ "$(git -C "$1" cat-file tag "$TAG_OBJ" | sed -n '1,/^$/s/^tag //p')" = "$2" ] || return 1
    signers="$WORK/allowed_signers"
    printf '%s\n' "$RELEASE_SIGNER" > "$signers" || return 1
    git -C "$1" -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$signers" \
            -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false -c gpg.ssh.program=ssh-keygen \
            verify-tag --raw "$TAG_OBJ" 2>&1 |
            grep -qxF "Good \"git\" signature for ${RELEASE_SIGNER%% *} with ED25519 key $RELEASE_FINGERPRINT"
}

# ── the platform ─────────────────────────────────────────────────────
# <os><major>-<arch>: debian13-amd64, freebsd14-amd64
platform() {
    case $(uname -s) in
        Linux)
            [ -r /etc/os-release ] || return 1
            # shellcheck disable=SC1091
            os=$(. /etc/os-release && printf '%s' "${ID:-}")
            # shellcheck disable=SC1091
            major=$(. /etc/os-release && printf '%s' "${VERSION_ID:-}" | cut -d. -f1)
            ;;
        FreeBSD)
            os=freebsd
            major=$(freebsd-version -u | cut -d. -f1)
            ;;
        *) return 1 ;;
    esac
    case $(uname -m) in
        x86_64|amd64) arch=amd64 ;;
        aarch64|arm64) arch=arm64 ;;
        *) return 1 ;;
    esac
    [ -n "$os" ] && [ -n "$major" ] || return 1
    printf '%s%s-%s\n' "$os" "$major" "$arch"
}

# the C library: "glibc 2.41" or "freebsd 14.5"
libc_version() {
    case $(uname -s) in
        Linux) getconf GNU_LIBC_VERSION 2>/dev/null | grep '^glibc [0-9]' ;;
        FreeBSD) printf 'freebsd %s\n' "$(freebsd-version -u | sed 's/-.*//')" ;;
        *) return 1 ;;
    esac
}

# the build's tools and libraries, as MANIFEST lines (cc=, meson=, ninja=,
# libs=): what another build must have to make the same bytes
toolchain() {
    printf 'cc=%s\n' "$(cc --version 2>/dev/null | head -n 1)"
    printf 'meson=%s\n' "$(meson --version 2>/dev/null)"
    printf 'ninja=%s\n' "$(ninja --version 2>/dev/null)"
    libs=
    for m in glib-2.0 openssl libotr libgcrypt libcurl chafa libutf8proc; do
        v=$(pkg-config --modversion "$m" 2>/dev/null) || v='?'
        libs="$libs $m=$v"
    done
    printf 'libs=%s\n' "${libs# }"
}

# the MANIFEST of package $1 (on stdout)
tarball_manifest() {
    top=$(xz -dc "$1" 2>/dev/null | tar -tf - 2>/dev/null | head -n 1) || return 1
    top=${top%%/*}
    case $top in erssi-*) ;; *) return 1 ;; esac
    xz -dc "$1" | tar -xOf - "$top/MANIFEST" 2>/dev/null
}

# date -u of epoch $1 in format $2 (GNU date, BSD date)
epoch_date() {
    date -u -d "@$1" "+$2" 2>/dev/null || date -u -r "$1" "+$2"
}

is_elf() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ "$(od -An -c -N4 "$1" | tr -d ' ')" = '177ELF' ]
}

# the checks of an installed tree DIR (the package's top directory),
# before it is packed; dies at the first problem. NEEDED: the shared
# libraries its ELF files need (sonames, sorted), but not the package's own.
check_tree() {
    dir=$1
    [ -x "$dir/bin/erssi" ] || die "no bin/erssi in the installed tree"
    # a link in the package is relative and never climbs ("..") or starts
    # at /: unpacked anywhere it cannot reach out of the tree. The installer
    # refuses a package with any other link.
    bad=$(find "$dir" -type l | while IFS= read -r l; do
            t=$(readlink "$l")
            case /$t/ in
                //*|*/../*) printf '%s -> %s\n' "${l#"$dir"/}" "$t" ;;
            esac
        done)
    [ -z "$bad" ] || die "a link is absolute or has a .. in it: $(printf '%s\n' "$bad" | head -n 1)"
    ELFS=$(find "$dir" -type f | LC_ALL=C sort | while IFS= read -r f; do is_elf "$f" && printf '%s\n' "$f"; done)
    [ -n "$ELFS" ] || die "no ELF files in the package"
    own=$(printf '%s\n' "$ELFS" | sed 's|.*/||')
    # GNU readelf: " 0x...01 (NEEDED)  Shared library: [libc.so.6]";
    # FreeBSD's (ELF Tool Chain): " 0x...01 NEEDED  Shared library: [libc.so.7]"
    NEEDED=$(printf '%s\n' "$ELFS" | while IFS= read -r f; do
            readelf -d "$f" | sed -n 's/.*NEEDED.*Shared library: *\[\(.*\)\].*/\1/p'
        done | LC_ALL=C sort -u | grep -vxF "$own" | tr '\n' ' ' | sed 's/ $//')
    [ -n "$NEEDED" ] || die "readelf found no shared libraries"
    # no RPATH or RUNPATH ("Library rpath: [...]", "Library runpath:
    # [...]" in both formats): a directory of the build machine (from
    # LDFLAGS, say) would be searched for libraries wherever the package
    # runs. Only the system perl's own CORE directory may be there.
    perl_core=$(perl -MConfig -e 'print "$Config{archlibexp}/CORE"' 2>/dev/null)
    # under /usr, and no ".." that could climb out of it
    case /$perl_core/ in
        */../*) perl_core= ;;
        //usr/*) ;;
        *) perl_core= ;;
    esac
    bad=$(printf '%s\n' "$ELFS" | while IFS= read -r f; do
            readelf -d "$f" | sed -n -e 's/.*RPATH.*\[\(.*\)\].*/\1/p' -e 's/.*RUNPATH.*\[\(.*\)\].*/\1/p' |
                tr ':' '\n' | while IFS= read -r p; do
                    [ -n "$perl_core" ] && [ "$p" = "$perl_core" ] && continue
                    printf '%s: %s\n' "${f#"$dir"/}" "$p"
                done
        done)
    [ -z "$bad" ] || die "an RPATH or RUNPATH in the package - no package: $(printf '%s\n' "$bad" | head -n 1)"
}

# meson install --destdir STAGE put everything under BUILD_PREFIX: a
# file installed elsewhere (an absolute install_dir, /etc say) would be
# missing from the package, so it is refused rather than dropped
check_stage() {
    stage=$1
    [ -d "$stage$BUILD_PREFIX" ] || die "nothing installed under $BUILD_PREFIX"
    outside=$(find "$stage" -path "$stage$BUILD_PREFIX" -prune -o -print | while IFS= read -r p; do
            # the directories leading to the prefix
            case $stage$BUILD_PREFIX/ in
                "$p"/*) continue ;;
            esac
            printf '%s\n' "${p#"$stage"}"
        done)
    [ -z "$outside" ] || die "meson installed files outside $BUILD_PREFIX - no package: $(printf '%s\n' "$outside" | head -n 3 | tr '\n' ' ')"
}

# The tree DIR packed into TARBALL, in sorted order, owned by 0:0, with
# WORK for the file list and the tar; dies when anything fails. The tar is
# written to a file and its status checked before xz runs: in a pipe into
# xz (no pipefail in sh) a tar that stopped half-way (an unreadable file, a
# full disk) left only xz's status, and the cut-off package would have
# been listed in SHA256SUMS and signed.
pack_tree() {   # dir tarball work
    # tar runs in the tree's parent: the work directory and the tarball
    # made absolute first, or relative ones would be taken from there
    ptop=$(cd -- "$(dirname -- "$1")" && pwd) || die "cannot open $(dirname -- "$1")"
    pname=$(basename -- "$1")
    pwork=$(cd -- "$3" && pwd) || die "cannot open $3"
    ptar=$(cd -- "$(dirname -- "$2")" && pwd) || die "cannot open $(dirname -- "$2")"
    ptar=$ptar/$(basename -- "$2")
    (cd "$ptop" && find "$pname" -print | LC_ALL=C sort) > "$pwork/files" || die "cannot list the package"
    if tar --version 2>/dev/null | grep -q 'GNU tar'; then
        set -- --format=gnu --no-recursion --numeric-owner --owner=0 --group=0 -T "$pwork/files"
    else  # bsdtar (FreeBSD)
        set -- --format=ustar -n --numeric-owner --uid 0 --gid 0 -T "$pwork/files"
    fi
    if ! (cd "$ptop" && tar -cf "$pwork/package.tar" "$@"); then
        rm -f "$pwork/package.tar"
        die "packing failed (tar)"
    fi
    if ! xz -T1 -6 -c "$pwork/package.tar" > "$ptar.tmp"; then
        rm -f "$pwork/package.tar" "$ptar.tmp"
        die "packing failed (xz)"
    fi
    rm -f "$pwork/package.tar"
    mv -f "$ptar.tmp" "$ptar" || die "cannot write $ptar"
}

# --verify: the package $1 (given) against $2 (built here), in WORK;
# exit 0 when identical, 1 with what differs
compare_packages() {
    VSUM=$(sha256_of "$1") || die "cannot compute the sha256 of $1"
    SUM=$(sha256_of "$2") || die "cannot compute the sha256 of $2"
    if [ "$VSUM" = "$SUM" ]; then
        say "reproduced: $(basename -- "$1") is identical to the build here"
        say "sha256 $SUM"
        exit 0
    fi
    say "NOT reproduced: $(basename -- "$1") differs from the build here"
    say "  given   sha256 $VSUM"
    say "  here    sha256 $SUM"
    mkdir "$WORK/given" "$WORK/here" || die "cannot make $WORK/given"
    (cd "$WORK/given" && xz -dc "$1" | tar -xf -) || die "cannot unpack $1"
    (cd "$WORK/here" && xz -dc "$2" | tar -xf -) || die "cannot unpack $2"
    (cd "$WORK/given" && find . -print | LC_ALL=C sort) > "$WORK/given.list"
    (cd "$WORK/here" && find . -print | LC_ALL=C sort) > "$WORK/here.list"
    LC_ALL=C comm -3 "$WORK/given.list" "$WORK/here.list" |
        awk -F '\t' '{ if ($1 == "") print "  only here:  " $2; else print "  only given: " $1 }' | head -n 20
    while IFS= read -r f; do
        if [ -f "$WORK/given/$f" ] && [ ! -L "$WORK/given/$f" ] && [ -f "$WORK/here/$f" ]; then
            cmp -s "$WORK/given/$f" "$WORK/here/$f" || printf '  differs:    %s\n' "${f#./}"
        fi
    done < "$WORK/given.list" | head -n 40
    m=$(find "$WORK/given" -name MANIFEST | head -n 1)
    h=$(find "$WORK/here" -name MANIFEST | head -n 1)
    if [ -n "$m" ] && [ -n "$h" ] && ! cmp -s "$m" "$h"; then
        say "the MANIFESTs differ (a different compiler or library makes other bytes):"
        diff "$m" "$h" | sed -n 's/^< /  given: /p; s/^> /  here:  /p'
    fi
    [ "$KEEP" = 1 ] || say "(--keep-work keeps both trees to compare: $WORK/given, $WORK/here)"
    exit 1
}

# ── a package ────────────────────────────────────────────────────────
# a build in an empty environment: run again under env -i (the checks
# and --sums need none of it). BUILD_BINARY_CLEAN marks the second run.
if [ -z "${BUILD_BINARY_CLEAN:-}" ]; then
    case ${1:-} in
        --sums|--check-stage|--check-tree|--pack|-h|--help) ;;
        *) exec env -i BUILD_BINARY_CLEAN=1 PATH="$CLEAN_PATH" HOME=/nonexistent LC_ALL=C \
                TMPDIR="${TMPDIR:-/tmp}" BUILD_BINARY_TESTS="${BUILD_BINARY_TESTS:-}" /bin/sh "$0" "$@" ;;
    esac
fi
REPO=
OUT=.
WORK=
KEEP=0
SUMS=
VERIFY=
while [ $# -gt 0 ]; do
    case $1 in
        --repo) [ $# -ge 2 ] || usage "--repo needs a directory"; REPO=$2; shift 2 ;;
        --out) [ $# -ge 2 ] || usage "--out needs a directory"; OUT=$2; shift 2 ;;
        --work) [ $# -ge 2 ] || usage "--work needs a directory"; WORK=$2; shift 2 ;;
        --keep-work) KEEP=1; shift ;;
        --sums) [ $# -ge 2 ] || usage "--sums needs a directory"; SUMS=$2; shift 2 ;;
        --verify) [ $# -ge 2 ] || usage "--verify needs a package"; VERIFY=$2; shift 2 ;;
        # the checks of an installed tree alone, for the packager's test
        --check-stage)
            [ $# -eq 2 ] || usage "--check-stage takes one directory"
            check_stage "$2"
            exit 0 ;;
        --check-tree)
            [ $# -eq 2 ] || usage "--check-tree takes one directory"
            check_tree "$2"
            printf 'needed=%s\n' "$NEEDED"
            exit 0 ;;
        --pack)
            [ $# -eq 4 ] || usage "--pack takes a directory, a tarball and a work directory"
            pack_tree "$2" "$3" "$4"
            exit 0 ;;
        -h|--help) sed -n '2,/^$/s/^# \{0,1\}//p' "$0"; exit 0 ;;
        -*) usage "unknown option $1" ;;
        *) break ;;
    esac
done

if [ -n "$SUMS" ]; then
    [ $# -eq 0 ] || usage "--sums takes no tag"
    write_sums "$SUMS"
    exit 0
fi
if [ -n "$VERIFY" ]; then
    [ $# -eq 0 ] || usage "--verify takes no tag: the package's MANIFEST names it"
    [ -f "$VERIFY" ] || die "$VERIFY is not a file"
    VERIFY=$(cd -- "$(dirname -- "$VERIFY")" && pwd)/$(basename -- "$VERIFY")
    have xz || die "xz is needed"
    VMANIFEST=$(tarball_manifest "$VERIFY") || die "$VERIFY is not a package (no MANIFEST in it)"
    TAG=$(printf '%s\n' "$VMANIFEST" | sed -n 's/^tag=//p')
    VPLATFORM=$(printf '%s\n' "$VMANIFEST" | sed -n 's/^platform=//p')
    [ -n "$TAG" ] || die "the MANIFEST of $VERIFY names no tag"
    HERE=$(platform) || die "unknown platform: $(uname -s) $(uname -m)"
    [ "$VPLATFORM" = "$HERE" ] || die "$VERIFY is for ${VPLATFORM:-(none)}, this is $HERE - build it again on $VPLATFORM"
    OUT=
else
    [ $# -eq 1 ] || usage "expected one release tag"
    TAG=$1
fi
case $TAG in
    shellter-v[0-9]*.[0-9]*.[0-9]*) VERSION=${TAG#shellter-v} ;;
    *) die "$TAG is not a release tag (shellter-vYYYY.M.PATCH)" ;;
esac
printf '%s\n' "$VERSION" | grep -Eq '^[0-9]{4}\.[0-9]{1,2}\.[0-9]+$' ||
    die "$TAG is not a release tag (shellter-vYYYY.M.PATCH)"

for t in git meson ninja perl tar xz strip readelf od sort cc pkg-config; do
    have "$t" || die "$t is needed"
done
can_verify || die "release signatures cannot be checked here (git 2.34+ and ssh-keygen are needed)"
PLATFORM=$(platform) || die "unknown platform: $(uname -s) $(uname -m)"
LIBC=$(libc_version) || die "unknown C library on $(uname -s) (glibc or FreeBSD's)"
PERL_VERSION=$(perl -e 'print $^V' | sed 's/^v//')
[ -n "$PERL_VERSION" ] || die "cannot read the version of perl"

if [ -z "$REPO" ]; then
    REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null) ||
        die "$(dirname "$0") is not in a git repository - give --repo"
fi
if [ -n "$OUT" ]; then
    mkdir -p "$OUT" || die "cannot make $OUT"
    OUT=$(cd "$OUT" && pwd) || die "cannot use $OUT"
fi
if [ -n "$WORK" ]; then
    [ ! -e "$WORK" ] || die "$WORK exists - the build needs a new directory"
    mkdir -p "$WORK" || die "cannot make $WORK"
    chmod 700 "$WORK" || die "cannot make $WORK private"
else
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/build-binary.XXXXXX") || die "cannot make a temporary directory"
fi
WORK=$(cd "$WORK" && pwd)
# shellcheck disable=SC2317 # run by the trap
cleanup() {
    if [ "$KEEP" = 1 ]; then
        say "the work directory stays: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

NAME=erssi-$VERSION-$PLATFORM
TARBALL=$OUT/$NAME.tar.xz
[ -n "$VERIFY" ] || [ ! -e "$TARBALL" ] || die "$TARBALL exists"

# the build sees nothing of the user's: no ~/.erssi, no git or ssh config
mkdir -m 700 "$WORK/home" "$WORK/tmp" || die "cannot make $WORK/home"
export HOME="$WORK/home" TMPDIR="$WORK/tmp" GIT_CONFIG_NOSYSTEM=1 LC_ALL=C
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES
# a package test running inside this build must not start another one
export ERSSI_BUILD_BINARY=1

SRC=$WORK/src
if [ -n "$VERIFY" ]; then
    # built again by the release's own packager (that of the signed tag,
    # checked here first and by itself again): the same script that made
    # the package, also for releases older than this one
    say "$(basename -- "$VERIFY"): $TAG built again from $REPO for $PLATFORM"
    git clone -q --no-checkout --no-local -- "$REPO" "$SRC" > "$WORK/clone.log" 2>&1 ||
        die "cannot clone $REPO: $(tail -n 1 "$WORK/clone.log")"
    signed_tag "$SRC" "$TAG" || die "$TAG is not an annotated tag signed by the release key - nothing to compare"
    COMMIT=$(git -C "$SRC" rev-parse --verify -q "$TAG_OBJ^{commit}") || die "$TAG names no commit"
    git -C "$SRC" -c advice.detachedHead=false checkout -q "$COMMIT" || die "cannot check out $COMMIT"
    [ -f "$SRC/utils/build-binary.sh" ] || die "$TAG has no utils/build-binary.sh - it has no packages"
    sh "$SRC/utils/build-binary.sh" --repo "$REPO" --out "$WORK/out" --work "$WORK/build-work" "$TAG" ||
        die "the build of $TAG failed - nothing to compare"
    compare_packages "$VERIFY" "$WORK/out/$NAME.tar.xz"
fi
say "$TAG from $REPO for $PLATFORM"

git clone -q --no-checkout --no-local -- "$REPO" "$SRC" > "$WORK/clone.log" 2>&1 ||
    die "cannot clone $REPO: $(tail -n 1 "$WORK/clone.log")"
signed_tag "$SRC" "$TAG" || die "$TAG is not an annotated tag signed by the release key - no package"
COMMIT=$(git -C "$SRC" rev-parse --verify -q "$TAG_OBJ^{commit}") || die "$TAG names no commit"
git -C "$SRC" -c advice.detachedHead=false checkout -q "$COMMIT" || die "cannot check out $COMMIT"
say "signed tag $TAG, commit $COMMIT"
sh "$SRC/utils/check-version.sh" "$SRC" "$TAG" || die "the version of $TAG does not check out - no package"

SOURCE_DATE_EPOCH=$(git -C "$SRC" log -1 --format=%ct "$COMMIT") || die "no commit time"
export SOURCE_DATE_EPOCH
BUILT=$(epoch_date "$SOURCE_DATE_EPOCH" '%Y-%m-%dT%H:%M:%SZ') || die "cannot format the commit time"
STAMP=$(epoch_date "$SOURCE_DATE_EPOCH" '%Y%m%d%H%M.%S') || die "cannot format the commit time"

BUILD=$WORK/build
say "configuring"
# every feature of the release, required; the paths of the work directory
# kept out of the binary (__FILE__ in assertions)
if ! meson setup "$BUILD" "$SRC" --prefix="$BUILD_PREFIX" -Drelocatable=true -Dbuildtype=release \
        -Dwith-perl=yes -Dwith-otr=yes -Dwith-proxy=yes -Dwith-fe-web=yes -Dwith-e2e=yes \
        -Dwith-image-preview=yes -Ddisable-utf8proc=no \
        "-Dc_args=-ffile-prefix-map=$SRC/=erssi/ -ffile-prefix-map=$BUILD/=build/" > "$WORK/setup.log" 2>&1; then
    tail -n 20 "$WORK/setup.log" >&2
    die "configuring the build failed - no package"
fi
say "building"
ninja -C "$BUILD" > "$WORK/build.log" 2>&1 || { tail -n 20 "$WORK/build.log" >&2; die "the build failed - no package"; }
if [ -n "${BUILD_BINARY_TESTS:-}" ]; then
    say "WARNING: only the tests $BUILD_BINARY_TESTS (BUILD_BINARY_TESTS) - never for a release"
    # test names have spaces: split at the commas only, no globbing
    set -f
    IFS=,
    # shellcheck disable=SC2086
    set -- $BUILD_BINARY_TESTS
    unset IFS
    set +f
else
    say "testing (the full suite)"
    set --
fi
# not --no-rebuild: some test programs (the fuzz corpus replays) are not
# built by default, and meson test builds them
if ! meson test -C "$BUILD" --print-errorlogs "$@" > "$WORK/test.log" 2>&1; then
    # what failed inside the tests (meson shows only the end of their
    # output): the failed cases' lines of the whole log
    grep -E '^ *(FAIL|not ok)[ -]' "$BUILD/meson-logs/testlog.txt" 2>/dev/null | head -n 40 | cut -c1-300 >&2
    grep -E '^ *[0-9]+/[0-9]+ .* (FAIL|ERROR|TIMEOUT) ' "$WORK/test.log" >&2
    tail -n 8 "$WORK/test.log" >&2
    die "tests failed - no package (the log: $WORK/test.log with --keep-work)"
fi
say "tests: $(grep -E '^(Ok|Fail|Skipped):' "$WORK/test.log" | tr '\n' ' ' | tr -s ' ')"

say "installing"
STAGE=$WORK/stage
meson install -C "$BUILD" --no-rebuild --destdir "$STAGE" --quiet > "$WORK/install.log" 2>&1 ||
    { tail -n 20 "$WORK/install.log" >&2; die "meson install failed - no package"; }
PKG=$WORK/pkg/$NAME
mkdir "$WORK/pkg" || die "cannot make $WORK/pkg"
check_stage "$STAGE"
mv "$STAGE$BUILD_PREFIX" "$PKG" || die "cannot move the installed tree"
check_tree "$PKG"

# stripped
printf '%s\n' "$ELFS" | while IFS= read -r f; do
    strip --strip-unneeded "$f" || exit 1
done || die "strip failed"

{
    printf 'version=%s\n' "$VERSION"
    printf 'tag=%s\n' "$TAG"
    printf 'commit=%s\n' "$COMMIT"
    printf 'platform=%s\n' "$PLATFORM"
    printf 'perl_version=%s\n' "$PERL_VERSION"
    printf 'libc=%s\n' "$LIBC"
    printf 'built=%s\n' "$BUILT"
    printf 'needed=%s\n' "$NEEDED"
    toolchain
} | LC_ALL=C sort > "$PKG/MANIFEST" || die "cannot write the MANIFEST"

# reproducible: modes from the umask out, every entry dated to the commit,
# in sorted order, owned by 0:0
chmod -R u+rwX,go+rX,go-w "$PKG" || die "chmod failed"
TZ=UTC find "$PKG" -exec touch -h -t "$STAMP" {} + || die "cannot date the files"
pack_tree "$PKG" "$TARBALL" "$WORK"
say "wrote $TARBALL"
say "sha256 $(sha256_of "$TARBALL")"
