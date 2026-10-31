#!/bin/sh
# utils/release.sh: the steps of an erssi Shellter Edition release, one
# command each (docs/shellter/releasing.md). Every step checks what it
# needs first and can run again: what is done already is said and kept.
#
# Usage: release.sh [OPTIONS] STEP ARGS
#   check TAG            the local tag TAG (shellter-vYYYY.M.PATCH) is an
#                        annotated tag of that name on a commit, both signed
#                        by a key of --signers; meson.build, NEWS
#                        (utils/check-version.sh), README.md and
#                        docs/shellter/install.md of the tag name its
#                        version; from 2026.10.9 the tag message is what
#                        `tag-message` prints; the working tree is clean;
#                        the tag is not on --remote yet, or is there as the
#                        same tag object (a different one is refused)
#   build TAG --out DIR  the packages of TAG into DIR, built from the local
#                        tag by the tag's own utils/build-binary.sh: Debian
#                        13 on this host when it is Debian 13 (otherwise in
#                        a chroot), Debian 12 and Ubuntu 24.04 in new
#                        mmdebstrap chroots, FreeBSD 14 and 15 over ssh on
#                        the hosts of --freebsd14 and --freebsd15. The tag
#                        reaches a chroot or a host as a git bundle, so it
#                        need not be pushed anywhere. A package already in
#                        DIR (of the same commit) is kept
#   verify DIR           each package in DIR built again by build-binary.sh
#                        --verify in a fresh environment (a new chroot, or
#                        the FreeBSD host of --freebsd14-verify, ...); it
#                        must be the same bytes. Marked as verified in
#                        DIR/logs/<platform>/verified
#   sums DIR             SHA256SUMS over the verified packages (sorted, by
#                        build-binary.sh --sums), signed with --key into
#                        SHA256SUMS.sig (namespace erssi-release) and
#                        checked against --signers
#   upload TAG DIR       the GitHub release of TAG (made from the tag
#                        message when there is none), the packages first,
#                        SHA256SUMS and SHA256SUMS.sig last; only when TAG
#                        is on --remote and on --gh-repo as the same tag
#                        object. Assets already there stay as they are
#                        (confirm checks them). verify, sums and upload
#                        take only packages whose MANIFEST names the
#                        commit of the local tag
#   confirm TAG          the assets of the GitHub release downloaded into a
#                        new directory: the signature of SHA256SUMS and every
#                        sum checked, nothing missing or extra
#   all TAG --out DIR    check, build, verify, sums, upload, confirm, in
#                        that order, stopping at the first that fails. Before
#                        the tag is on --remote it stops after sums and says
#                        what to push; run it again after the push
#   tag-message VERSION  the message of the release tag from NEWS: the title
#                        line, a blank line and the intro paragraph of the
#                        release's NEWS entry, wrapped at 72 columns
#                        (git tag -s -F <file> shellter-vVERSION)
#
# Options (each also from the environment variable named):
#   --dry-run            the checks run; nothing is built, written, signed or
#                        uploaded: each such action is printed instead
#   --repo DIR           the repository (RELEASE_REPO; default: the one this
#                        script is in)
#   --remote NAME|URL    the public remote (RELEASE_REMOTE; default origin)
#   --gh-repo OWNER/NAME the GitHub repository (RELEASE_GH_REPO; default
#                        y-o-o-z/erssi)
#   --signers FILE       the release key(s), allowed_signers format
#                        (RELEASE_SIGNERS; default utils/allowed_signers
#                        beside this script)
#   --key FILE           the key that signs SHA256SUMS (RELEASE_KEY; default
#                        git config user.signingkey of the repository)
#   --out DIR            where build puts the packages and its logs
#   --platforms LIST     the platforms to build (RELEASE_PLATFORMS; default
#                        debian13-amd64 debian12-amd64 ubuntu24-amd64, and
#                        freebsd14-amd64 / freebsd15-amd64 when their host is
#                        given)
#   --freebsd14 TARGET, --freebsd15 TARGET
#                        the ssh destination of the FreeBSD build host
#                        (RELEASE_FREEBSD14, RELEASE_FREEBSD15)
#   --freebsd14-verify TARGET, --freebsd15-verify TARGET
#                        where verify builds them again (default: the same
#                        host; RELEASE_FREEBSD14_VERIFY, ..._15_VERIFY)
#   --mm-tmpdir DIR      TMPDIR of mmdebstrap, which must not be in a 0700
#                        home (RELEASE_MM_TMPDIR; default /var/tmp/<user>-mm)
# Environment only: RELEASE_SSH (the ssh command, default ssh),
# RELEASE_UBUNTU_MIRROR (default http://archive.ubuntu.com/ubuntu),
# RELEASE_UBUNTU_KEYRING (default
# /usr/share/keyrings/ubuntu-archive-keyring.gpg), RELEASE_DEBIAN_MIRROR
# (default: mmdebstrap's).
# Paths given to build and verify may hold letters, digits and ._+/- only
# (mmdebstrap's hooks take them unquoted).
#
# Exit: 0 done, 1 refused or failed, 2 usage.

set -u

say() { printf 'release: %s\n' "$*"; }
die() { printf 'release: %s\n' "$*" >&2; exit 1; }
usage() {
    printf 'release: %s\n' "$1" >&2
    echo 'usage: release.sh [OPTIONS] check|build|verify|sums|upload|confirm|all|tag-message ... (--help)' >&2
    exit 2
}
have() { command -v "$1" >/dev/null 2>&1; }
# a word as the shell reads it back: quoted when it needs to be
shquote() {
    case $1 in
        ''|*[!A-Za-z0-9._+/:=@,%-]*) printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")" ;;
        *) printf '%s' "$1" ;;
    esac
}
# an action: printed with --dry-run, run otherwise
act() {
    if [ "$DRY" = 1 ]; then
        printf 'release: would run:'
        for a in "$@"; do printf ' %s' "$(shquote "$a")"; done
        printf '\n'
        return 0
    fi
    "$@"
}

UTILS=$(cd -- "$(dirname -- "$0")" && pwd) || exit 1
PKGS=git,ca-certificates,gcc,meson,ninja-build,pkgconf,libglib2.0-dev,libssl-dev,libperl-dev,libutf8proc-dev,libgcrypt20-dev,libotr5-dev,libcurl4-openssl-dev,libchafa-dev,python3,perl,binutils,xz-utils,openssh-client,libsodium23
ALL_PLATFORMS="debian13-amd64 debian12-amd64 ubuntu24-amd64 freebsd14-amd64 freebsd15-amd64"

# ── options ──────────────────────────────────────────────────────────
DRY=0
REPO=${RELEASE_REPO:-}
REMOTE=${RELEASE_REMOTE:-origin}
GH_REPO=${RELEASE_GH_REPO:-y-o-o-z/erssi}
SIGNERS=${RELEASE_SIGNERS:-$UTILS/allowed_signers}
KEY=${RELEASE_KEY:-}
OUT=
PLATFORMS=${RELEASE_PLATFORMS:-}
FB14=${RELEASE_FREEBSD14:-}
FB15=${RELEASE_FREEBSD15:-}
FB14V=${RELEASE_FREEBSD14_VERIFY:-}
FB15V=${RELEASE_FREEBSD15_VERIFY:-}
MM_TMPDIR=${RELEASE_MM_TMPDIR:-}
SSH=${RELEASE_SSH:-ssh}
UBUNTU_MIRROR=${RELEASE_UBUNTU_MIRROR:-http://archive.ubuntu.com/ubuntu}
UBUNTU_KEYRING=${RELEASE_UBUNTU_KEYRING:-/usr/share/keyrings/ubuntu-archive-keyring.gpg}
DEBIAN_MIRROR=${RELEASE_DEBIAN_MIRROR:-}
STEP_NAME=
NARGS=0
A1='' A2=''
while [ $# -gt 0 ]; do
    case $1 in
        --dry-run) DRY=1; shift; continue ;;
        -h|--help) sed -n '2,/^$/s/^# \{0,1\}//p' "$0"; exit 0 ;;
        --repo|--remote|--gh-repo|--signers|--key|--out|--platforms|--freebsd14|--freebsd15|--freebsd14-verify|--freebsd15-verify|--mm-tmpdir)
            if [ $# -lt 2 ] || [ -z "$2" ]; then usage "$1 needs a value"; fi
            case $1 in
                --repo) REPO=$2 ;;
                --remote) REMOTE=$2 ;;
                --gh-repo) GH_REPO=$2 ;;
                --signers) SIGNERS=$2 ;;
                --key) KEY=$2 ;;
                --out) OUT=$2 ;;
                --platforms) PLATFORMS=$2 ;;
                --freebsd14) FB14=$2 ;;
                --freebsd15) FB15=$2 ;;
                --freebsd14-verify) FB14V=$2 ;;
                --freebsd15-verify) FB15V=$2 ;;
                --mm-tmpdir) MM_TMPDIR=$2 ;;
            esac
            shift 2; continue ;;
        -*) usage "unknown option $1" ;;
    esac
    if [ -z "$STEP_NAME" ]; then
        STEP_NAME=$1
    else
        NARGS=$((NARGS + 1))
        case $NARGS in
            1) A1=$1 ;;
            2) A2=$1 ;;
            *) usage "$STEP_NAME: too many arguments" ;;
        esac
    fi
    shift
done
[ -n "$STEP_NAME" ] || usage "no step given"
nargs() {   # the step takes exactly $1 arguments
    [ "$NARGS" = "$1" ] || usage "$STEP_NAME takes $1 argument(s) ($2)"
}
case $STEP_NAME in
    check|confirm) nargs 1 TAG ;;
    build|all) nargs 1 TAG; [ -n "$OUT" ] || usage "$STEP_NAME needs --out DIR" ;;
    verify|sums) nargs 1 DIR ;;
    upload) nargs 2 'TAG DIR' ;;
    tag-message) nargs 1 VERSION ;;
    *) usage "unknown step $STEP_NAME" ;;
esac
case $STEP_NAME in
    tag-message) VERSION=$A1 ;;
    verify|sums) DIR=$A1 ;;
    *) TAG=$A1; DIR=${A2:-$OUT} ;;
esac
case $STEP_NAME in
    tag-message) ;;
    verify|sums) ;;
    *)
        case $TAG in
            shellter-v*) VERSION=${TAG#shellter-v} ;;
            *) usage "$TAG is not a release tag (shellter-vYYYY.M.PATCH)" ;;
        esac ;;
esac
if [ -n "${VERSION:-}" ]; then
    printf '%s\n' "$VERSION" | grep -Eq '^20[0-9]{2}\.([1-9]|1[0-2])\.([0-9]|[1-9][0-9])$' ||
        usage "$VERSION is not a calendar version YYYY.M.PATCH"
fi
for p in $PLATFORMS; do
    case " $ALL_PLATFORMS " in *" $p "*) ;; *) usage "unknown platform $p (one of: $ALL_PLATFORMS)" ;; esac
done
if [ -z "$PLATFORMS" ]; then
    PLATFORMS="debian13-amd64 debian12-amd64 ubuntu24-amd64"
    [ -z "$FB14" ] || PLATFORMS="$PLATFORMS freebsd14-amd64"
    [ -z "$FB15" ] || PLATFORMS="$PLATFORMS freebsd15-amd64"
fi
[ -n "$FB14V" ] || FB14V=$FB14
[ -n "$FB15V" ] || FB15V=$FB15
case $STEP_NAME in
    build|all)
        case " $PLATFORMS " in *" freebsd14-amd64 "*) [ -n "$FB14" ] || usage "freebsd14-amd64 needs --freebsd14 TARGET" ;; esac
        case " $PLATFORMS " in *" freebsd15-amd64 "*) [ -n "$FB15" ] || usage "freebsd15-amd64 needs --freebsd15 TARGET" ;; esac ;;
esac
# a word that goes unquoted into a hook or a remote command line
safe_word() { case $1 in ''|*[!A-Za-z0-9._+/-]*) return 1 ;; esac; return 0; }
for t in "$FB14" "$FB15" "$FB14V" "$FB15V"; do
    [ -z "$t" ] || case $t in -*|*[!A-Za-z0-9._@:-]*) usage "$t is not an ssh destination (user@host or a Host of ~/.ssh/config)" ;; esac
done

if [ "$STEP_NAME" != tag-message ] || [ -n "$REPO" ]; then
    if [ -z "$REPO" ]; then
        REPO=$(git -C "$UTILS" rev-parse --show-toplevel 2>/dev/null) || die "$UTILS is not in a git repository - give --repo"
    fi
    REPO=$(git -C "$REPO" rev-parse --show-toplevel 2>/dev/null) || die "$REPO is not a git repository"
fi

WORK='' BDIR='' RHOST='' RDIR=''
# the directory RDIR on host RHOST removed (after a build or a verify
# there, and by the trap when one is stopped half way)
remote_rm() {
    [ -n "$RHOST" ] || return 0
    # shellcheck disable=SC2086 # SSH: the command and its options
    $SSH "$RHOST" "rm -rf $RDIR" >/dev/null 2>&1 </dev/null
    RHOST=
}
# shellcheck disable=SC2317 # run by the trap
cleanup() {
    [ -z "$WORK" ] || rm -rf "$WORK"
    [ -z "$BDIR" ] || rm -rf "$BDIR"
    remote_rm
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
work() {
    [ -n "$WORK" ] && return 0
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/release.XXXXXX") || die "cannot make a temporary directory"
}

# the sha256 of file $1 in hex
sha256_of() {
    if have sha256sum; then sha256sum < "$1" | awk '{print $1}'
    elif have sha256; then sha256 -q < "$1"
    elif have openssl; then openssl dgst -sha256 -r < "$1" | awk '{print $1}'
    else return 1
    fi
}
# 0 when version $1 >= $2 (YYYY.M.PATCH)
version_ge() {
    printf '%s\n%s\n' "$1" "$2" | awk -F. 'NR == 1 { a = $1 * 1000000 + $2 * 1000 + $3 }
        NR == 2 { b = $1 * 1000000 + $2 * 1000 + $3 } END { exit !(a >= b) }'
}

# ── the tag message ──────────────────────────────────────────────────
# NEWS on stdin; the message of release $1: "erssi Shellter Edition $1",
# a blank line and the intro paragraph of its entry (the lines after the
# title up to the first blank line or "+ " section), wrapped at 72
tag_message() {
    awk -v v="$1" '
        function flush(   n, i, w, line) {
            n = split(text, w, " ")
            line = ""
            for (i = 1; i <= n; i++) {
                if (line == "") line = w[i]
                else if (length(line) + 1 + length(w[i]) <= 72) line = line " " w[i]
                else { print line; line = w[i] }
            }
            if (line != "") print line
        }
        state == 0 && $1 == "shellter-v" v { state = 1; next }
        state == 0 { next }
        state == 1 {
            t = $0; sub(/^[ \t]+/, "", t)
            if (t != "erssi Shellter Edition " v) { err = "the entry of " v " does not start with the title line \"erssi Shellter Edition " v "\""; exit }
            state = 2; next
        }
        state == 2 && /^[ \t]*$/ { next }
        state == 2 || state == 3 {
            if (/^[^ \t]/ || /^[ \t]*$/ || /^[ \t]*\+ /) { exit }
            t = $0; sub(/^[ \t]+/, "", t); sub(/[ \t]+$/, "", t)
            text = text (text == "" ? "" : " ") t
            state = 3; next
        }
        END {
            if (err != "") { print "release: NEWS: " err > "/dev/stderr"; exit 1 }
            if (state == 0) { print "release: NEWS has no entry shellter-v" v > "/dev/stderr"; exit 1 }
            if (text == "") { print "release: NEWS: the entry of " v " has no intro paragraph before its first section" > "/dev/stderr"; exit 1 }
            print "erssi Shellter Edition " v
            print ""
            flush()
        }'
}

# ── the release key ──────────────────────────────────────────────────
# the keys of SIGNERS (allowed_signers: principal [options] keytype key)
# for the namespace $1, in FILE $2; the first principal in PRINCIPAL
signers_for() {
    [ -r "$SIGNERS" ] || die "cannot read the signers file $SIGNERS"
    awk -v ns="$1" '/^[ \t]*(#|$)/ { next }
        { k = ($2 ~ /^(ssh-|ecdsa-|sk-)/) ? 2 : 3
          if ($k == "" || $(k + 1) == "") next
          printf "%s namespaces=\"%s\" %s %s\n", $1, ns, $k, $(k + 1) }' "$SIGNERS" > "$2" ||
        die "cannot write $2"
    [ -s "$2" ] || die "no key in the signers file $SIGNERS"
    PRINCIPAL=$(awk 'NR == 1 { print $1 }' "$2")
}
# git with SSH signatures checked against SIGNERS only (no gpg or gpgsm,
# whatever the git config says), as utils/pre-push does
git_verify() {
    git -C "$REPO" -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$SIGNERS" \
        -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false -c gpg.ssh.program=ssh-keygen \
        "$@" >/dev/null 2>&1
}
# the local tag TAG: an annotated tag of that name, signed, on a signed
# commit; TAG_OBJ and COMMIT set
local_tag() {
    TAG_OBJ=$(git -C "$REPO" rev-parse --verify -q "refs/tags/$TAG") || die "$TAG: no such tag in $REPO (git tag -s -F <message> $TAG)"
    [ "$(git -C "$REPO" cat-file -t "$TAG_OBJ")" = tag ] || die "$TAG is not an annotated tag (git tag -s)"
    [ "$(git -C "$REPO" cat-file tag "$TAG_OBJ" | sed -n '1,/^$/s/^tag //p')" = "$TAG" ] ||
        die "the tag object of $TAG names another tag"
    COMMIT=$(git -C "$REPO" rev-parse --verify -q "$TAG_OBJ^{commit}") || die "$TAG names no commit"
    git_verify verify-tag "$TAG_OBJ" || die "$TAG is not signed by a key of $SIGNERS"
    git_verify verify-commit "$COMMIT" || die "the commit of $TAG ($COMMIT) is not signed by a key of $SIGNERS"
}
# what the remote has for TAG: REMOTE_OBJ (empty: not there); dies when
# the remote cannot be reached
remote_tag() {
    out=$(git -C "$REPO" ls-remote -- "$REMOTE" "refs/tags/$TAG" 2>&1) || die "cannot reach $REMOTE: $(printf '%s\n' "$out" | tail -n 1)"
    REMOTE_OBJ=$(printf '%s\n' "$out" | awk -v r="refs/tags/$TAG" '$2 == r { print $1 }')
}

# ── check ────────────────────────────────────────────────────────────
# the files of the tag's tree that name the release, and what they must
# hold (the release commit brings them to the version)
doc_problems() {
    v=$VERSION
    git -C "$REPO" show "$COMMIT:README.md" 2>/dev/null | grep -qF "badge/release-$v-" ||
        echo "README.md: the release badge does not name $v"
    git -C "$REPO" show "$COMMIT:README.md" 2>/dev/null | grep -qF "**erssi Shellter Edition $v**" ||
        echo "README.md: the version table does not name erssi Shellter Edition $v"
    git -C "$REPO" show "$COMMIT:docs/shellter/install.md" 2>/dev/null | grep -qF "verify-tag shellter-v$v" ||
        echo "docs/shellter/install.md: the verify-tag example does not name shellter-v$v"
    git -C "$REPO" show "$COMMIT:docs/shellter/install.md" 2>/dev/null | grep -qF "v=$v p=" ||
        echo "docs/shellter/install.md: the package example does not name $v (v=$v p=...)"
}
step_check() {
    say "check $TAG"
    local_tag
    say "  signed tag $TAG, commit $COMMIT"
    (cd "$REPO" && sh "$UTILS/check-version.sh" "$TAG" "$TAG") || die "$TAG: meson.build, NEWS and the tag do not agree (utils/check-version.sh)"
    probs=$(doc_problems)
    if [ -n "$probs" ]; then
        printf '%s\n' "$probs" | sed 's/^/release:   /' >&2
        die "$TAG: the release commit does not bring the docs to $VERSION"
    fi
    say "  meson.build, NEWS, README.md and docs/shellter/install.md name $VERSION"
    # the message: the title, and from 2026.10.9 the summary from NEWS
    msg=$(git -C "$REPO" cat-file tag "$TAG_OBJ" | sed '1,/^$/d; /^-----BEGIN .*SIGNATURE-----/,$d')
    if version_ge "$VERSION" 2026.10.9; then
        want=$(git -C "$REPO" show "$COMMIT:NEWS" | tag_message "$VERSION") || die "$TAG: no tag message from its NEWS"
        [ "$msg" = "$want" ] || die "$TAG: the tag message is not the one from NEWS (release.sh tag-message $VERSION > msg; git tag -s -F msg $TAG)"
    else
        [ "$(printf '%s\n' "$msg" | sed -n 1p)" = "erssi Shellter Edition $VERSION" ] || die "$TAG: the tag message does not start with \"erssi Shellter Edition $VERSION\""
    fi
    say "  tag message as NEWS says"
    [ -z "$(git -C "$REPO" status --porcelain 2>/dev/null)" ] || die "the working tree of $REPO is not clean (git status)"
    remote_tag
    if [ -z "$REMOTE_OBJ" ]; then
        say "  not on $REMOTE yet: build and verify from the local tag, then push it"
    elif [ "$REMOTE_OBJ" = "$TAG_OBJ" ]; then
        say "  already on $REMOTE, the same tag object"
    else
        die "$TAG on $REMOTE is another tag object ($REMOTE_OBJ, here $TAG_OBJ) - a published tag never moves; make a new PATCH release"
    fi
    say "check $TAG: ok"
}

# ── the platforms ────────────────────────────────────────────────────
# this host's platform, as build-binary.sh names it
host_platform() {
    case $(uname -s) in
        Linux)
            [ -r /etc/os-release ] || return 1
            # shellcheck disable=SC1091
            os=$(. /etc/os-release && printf '%s' "${ID:-}")
            # shellcheck disable=SC1091
            major=$(. /etc/os-release && printf '%s' "${VERSION_ID:-}" | cut -d. -f1) ;;
        FreeBSD) os=freebsd; major=$(freebsd-version -u | cut -d. -f1) ;;
        *) return 1 ;;
    esac
    case $(uname -m) in
        x86_64|amd64) arch=amd64 ;;
        aarch64|arm64) arch=arm64 ;;
        *) return 1 ;;
    esac
    printf '%s%s-%s\n' "$os" "$major" "$arch"
}
# how platform $1 is built: host, chroot or ssh
how() {
    case $1 in
        freebsd*) echo ssh ;;
        "$HOST_PLATFORM") [ "$2" = build ] && echo host || echo chroot ;;
        *) echo chroot ;;
    esac
}
# the mmdebstrap arguments of platform $1: MM_SUITE, MM_ARGS (words),
# MM_MIRROR
chroot_of() {
    MM_ARGS="" MM_MIRROR=$DEBIAN_MIRROR
    case $1 in
        debian12-amd64) MM_SUITE=bookworm ;;
        debian13-amd64) MM_SUITE=trixie ;;
        ubuntu24-amd64)
            MM_SUITE=noble MM_MIRROR=$UBUNTU_MIRROR
            MM_ARGS="--components=main,universe --keyring=$UBUNTU_KEYRING" ;;
        *) die "no chroot for $1" ;;
    esac
}
target_of() {   # platform build|verify -> the ssh destination
    case $1:$2 in
        freebsd14-amd64:build) echo "$FB14" ;;
        freebsd15-amd64:build) echo "$FB15" ;;
        freebsd14-amd64:verify) echo "$FB14V" ;;
        freebsd15-amd64:verify) echo "$FB15V" ;;
    esac
}
# the tools platforms $1 (list) need for step $2; missing ones are fatal,
# with --dry-run only said
need_tools() {
    for p in $1; do
        case $(how "$p" "$2") in
            chroot) t=mmdebstrap ;;
            ssh) t=$(printf '%s\n' "$SSH" | awk '{print $1}') ;;
            *) continue ;;
        esac
        have "$t" && continue
        if [ "$DRY" = 1 ]; then say "  ($t is needed for $p and is not installed)"; else die "$t is needed for $p"; fi
    done
    case " $1 " in
        *" ubuntu24-amd64 "*)
            [ -r "$UBUNTU_KEYRING" ] && return 0
            if [ "$DRY" = 1 ]; then say "  (no $UBUNTU_KEYRING for ubuntu24-amd64: apt install ubuntu-keyring)"; else die "no $UBUNTU_KEYRING for ubuntu24-amd64 (apt install ubuntu-keyring, or RELEASE_UBUNTU_KEYRING)"; fi ;;
    esac
}
# an absolute path without characters the hooks cannot take
abs_dir() {
    a=$(cd -- "$1" 2>/dev/null && pwd) || return 1
    safe_word "$a" || die "$a: a path for build and verify may hold letters, digits and ._+/- only"
    printf '%s\n' "$a"
}
mm_tmpdir() {
    [ -n "$MM_TMPDIR" ] || MM_TMPDIR=/var/tmp/$(id -un)-mm
    safe_word "$MM_TMPDIR" || die "$MM_TMPDIR: a path for build and verify may hold letters, digits and ._+/- only"
    [ "$DRY" = 1 ] && return 0
    [ -d "$MM_TMPDIR" ] || mkdir -m 755 "$MM_TMPDIR" || die "cannot make $MM_TMPDIR"
}
# the tag as a git bundle, readable in a chroot: BUNDLE
make_bundle() {
    [ -n "${BUNDLE:-}" ] && return 0
    mm_tmpdir
    if [ "$DRY" = 1 ]; then
        BUNDLE=$MM_TMPDIR/release.XXXXXX/repo.bundle
        say "would run: git bundle create $BUNDLE refs/tags/$TAG"
        return 0
    fi
    BDIR=$(mktemp -d "$MM_TMPDIR/release.XXXXXX") || die "cannot make a directory in $MM_TMPDIR"
    chmod 755 "$BDIR"
    BUNDLE=$BDIR/repo.bundle
    git -C "$REPO" bundle create "$BUNDLE" "refs/tags/$TAG" > "$BDIR/bundle.log" 2>&1 || die "cannot bundle $TAG: $(tail -n 1 "$BDIR/bundle.log")"
    chmod 644 "$BUNDLE"
}
# the commands that get the tag from the bundle $1 into the clone $2 and
# check it out (sh, for a chroot's or a host's shell)
from_bundle() {
    printf 'rm -rf %s && git init -q %s && git -C %s fetch -q %s refs/tags/%s:refs/tags/%s && git -C %s -c advice.detachedHead=false checkout -q -b release %s' \
        "$2" "$2" "$2" "$1" "$TAG" "$TAG" "$2" "$TAG"
}
# a new chroot of platform $1 that runs CMD ($2) as the user builder,
# with the files $3 (words) in its home, and copies its directory ~/$4 out
# into $5; mmdebstrap's output into $6
in_chroot() {
    chroot_of "$1"
    cmd=$2 files=$3 sub=$4 dest=$5 log=$6
    set -- --mode=unshare --variant=apt
    for a in $MM_ARGS; do set -- "$@" "$a"; done
    # shellcheck disable=SC2016 # $1: the chroot, in mmdebstrap's hook
    set -- "$@" --include="$PKGS" '--customize-hook=chroot "$1" useradd -m builder'
    for f in $files; do set -- "$@" "--customize-hook=copy-in $f /home/builder"; done
    set -- "$@" "--customize-hook=chroot \"\$1\" su builder -c '$cmd'" \
        "--customize-hook=copy-out /home/builder/$sub $dest" "$MM_SUITE" /dev/null
    [ -z "$MM_MIRROR" ] || set -- "$@" "$MM_MIRROR"
    if [ "$DRY" = 1 ]; then
        act env TMPDIR="$MM_TMPDIR" mmdebstrap "$@"
        return 0
    fi
    env TMPDIR="$MM_TMPDIR" mmdebstrap "$@" > "$log" 2>&1
}
# a new directory on host $1: RDIR
remote_dir() {
    if [ "$DRY" = 1 ]; then
        RDIR=/tmp/erssi-release.XXXXXX
        say "would run: $SSH $1 sh -s  (mktemp -d \${TMPDIR:-/tmp}/erssi-release.XXXXXX)"
        return 0
    fi
    # shellcheck disable=SC2016,SC2086 # run by the remote sh
    RDIR=$(printf '%s\n' 'mktemp -d "${TMPDIR:-/tmp}/erssi-release.XXXXXX"' | $SSH "$1" sh -s) ||
        die "cannot make a directory on $1"
    safe_word "$RDIR" || die "$1 made the directory '$RDIR', which cannot be used"
    RHOST=$1
}
# the bundle onto host $1 (RDIR/repo.bundle)
remote_bundle() {
    make_bundle
    if [ "$DRY" = 1 ]; then
        say "would run: $SSH $1 cat \\> $RDIR/repo.bundle < $BUNDLE"
        return 0
    fi
    # shellcheck disable=SC2086
    $SSH "$1" "cat > $RDIR/repo.bundle" < "$BUNDLE" || die "cannot copy the tag to $1"
}
# the MANIFEST line $2 of package $1
manifest() {
    top=$(xz -dc "$1" 2>/dev/null | tar -tf - 2>/dev/null | head -n 1)
    top=${top%%/*}
    xz -dc "$1" 2>/dev/null | tar -xOf - "$top/MANIFEST" 2>/dev/null | sed -n "s/^$2=//p"
}
# package DIR/$1 is built from COMMIT, the commit of the local tag: a
# package left from a tag that was made again on another commit is never
# verified, signed or uploaded under this one
of_commit() {
    c=$(manifest "$DIR/$1" commit)
    [ "$c" = "$COMMIT" ] ||
        die "$1 was built from ${c:-an unknown commit}, not from $COMMIT, the commit of $TAG - remove it and build again"
}

# ── build ────────────────────────────────────────────────────────────
build_one() {   # platform
    p=$1 name=erssi-$VERSION-$1.tar.xz
    L=$DIR/logs/$p
    if [ -f "$DIR/$name" ]; then
        c=$(manifest "$DIR/$name" commit)
        [ "$c" = "$COMMIT" ] || die "$DIR/$name is there, built from ${c:-an unknown commit}, not $COMMIT - remove it"
        say "  $p: $name is there (commit $COMMIT) - kept"
        return 0
    fi
    act rm -rf "$L"
    act mkdir -p "$L"
    case $(how "$p" build) in
        host)
            say "  $p: on this host, by the build-binary.sh of $TAG"
            if [ "$DRY" = 1 ]; then
                say "would run: git -C $REPO show $COMMIT:utils/build-binary.sh > <work>/build-binary.sh"
                say "would run: sh <work>/build-binary.sh --repo $REPO --out $L/out $TAG > $L/build.log"
            else
                work
                git -C "$REPO" show "$COMMIT:utils/build-binary.sh" > "$WORK/build-binary.sh" ||
                    die "$TAG has no utils/build-binary.sh"
                if ! sh "$WORK/build-binary.sh" --repo "$REPO" --out "$L/out" "$TAG" > "$L/build.log" 2>&1; then
                    tail -n 5 "$L/build.log" >&2
                    die "$p: the build failed (the log: $L/build.log)"
                fi
            fi ;;
        chroot)
            chroot_of "$p"
            say "  $p: in a new chroot ($MM_SUITE)"
            make_bundle
            # shellcheck disable=SC2088 # ~: the builder's home, in the chroot
            in_chroot "$p" "mkdir -p ~/out && $(from_bundle '~/repo.bundle' '~/erssi') && sh ~/erssi/utils/build-binary.sh --repo ~/erssi --out ~/out $TAG > ~/out/build.log 2>&1; echo rc=\$? >> ~/out/build.log" \
                "$BUNDLE" out "$L" "$L/mmdebstrap.log"
            if [ "$DRY" = 0 ] && ! grep -qx 'rc=0' "$L/out/build.log" 2>/dev/null; then
                tail -n 5 "$L/out/build.log" "$L/mmdebstrap.log" >&2 2>/dev/null
                die "$p: the build failed (the logs: $L)"
            fi ;;
        ssh)
            t=$(target_of "$p" build)
            say "  $p: on $t"
            remote_dir "$t"
            remote_bundle "$t"
            # the remote script: sh on stdin, whatever the login shell
            script="set -u; cd $RDIR || exit 1; $(from_bundle "$RDIR/repo.bundle" "$RDIR/erssi") || exit 1; mkdir -p out; sh erssi/utils/build-binary.sh --repo $RDIR/erssi --out $RDIR/out $TAG > build.log 2>&1; rc=\$?; ls out; exit \$rc"
            if [ "$DRY" = 1 ]; then
                say "would run: $SSH $t sh -s, the script on stdin: $script"
                say "would run: $SSH $t cat $RDIR/out/$name > $L/out/$name"
                say "would run: $SSH $t rm -rf $RDIR"
            else
                # shellcheck disable=SC2086 # SSH: the command and its options
                printf '%s\n' "$script" | $SSH "$t" sh -s > "$L/remote.out" 2>&1; rc=$?
                # shellcheck disable=SC2086
                $SSH "$t" "cat $RDIR/build.log" > "$L/build.log" 2>/dev/null
                if [ "$rc" != 0 ] || ! grep -qx "$name" "$L/remote.out"; then
                    tail -n 5 "$L/build.log" "$L/remote.out" >&2 2>/dev/null
                    remote_rm
                    die "$p: the build on $t failed (the logs: $L)"
                fi
                mkdir -p "$L/out" || die "cannot make $L/out"
                # shellcheck disable=SC2086
                if ! $SSH "$t" "cat $RDIR/out/$name" > "$L/out/$name.tmp"; then
                    rm -f "$L/out/$name.tmp"
                    die "$p: cannot copy $name from $t"
                fi
                mv "$L/out/$name.tmp" "$L/out/$name" || die "cannot write $L/out/$name"
                remote_rm
            fi ;;
    esac
    [ "$DRY" = 1 ] && { say "would run: mv $L/out/$name $DIR/$name"; return 0; }
    [ -f "$L/out/$name" ] || die "$p: the build wrote no $name (the logs: $L)"
    c=$(manifest "$L/out/$name" commit)
    [ "$c" = "$COMMIT" ] || die "$p: $name names the commit ${c:-(none)}, not $COMMIT"
    mv "$L/out/$name" "$DIR/$name" || die "cannot move $name into $DIR"
    say "  $p: $name, sha256 $(sha256_of "$DIR/$name")"
}
# DIR made absolute (its parent must exist), outside the repository
out_dir() {
    parent=$(cd -- "$(dirname -- "$DIR")" 2>/dev/null && pwd) || die "$(dirname -- "$DIR") is not a directory"
    DIR=$parent/$(basename -- "$DIR")
    [ -d "$DIR" ] && DIR=$(cd -- "$DIR" && pwd)
    safe_word "$DIR" || die "$DIR: a path for build and verify may hold letters, digits and ._+/- only"
    case $DIR/ in "$REPO"/*) die "$DIR is inside $REPO - the packages would make its tree unclean; use a directory outside it" ;; esac
}
step_build() {
    say "build $TAG for: $PLATFORMS"
    local_tag
    have xz || die "xz is needed"
    HOST_PLATFORM=$(host_platform) || HOST_PLATFORM=
    need_tools "$PLATFORMS" build
    out_dir
    if [ ! -d "$DIR" ]; then act mkdir "$DIR" || die "cannot make $DIR"; fi
    for p in $PLATFORMS; do build_one "$p"; done
    say "build $TAG: done"
}

# ── verify ───────────────────────────────────────────────────────────
# the packages in DIR: NAMES, their VERSION and TAG; dies on a mix
packages() {
    [ -d "$DIR" ] || return 1
    NAMES=$(cd "$DIR" && for f in erssi-*.tar.xz; do [ -f "$f" ] && printf '%s\n' "$f"; done | LC_ALL=C sort)
    [ -n "$NAMES" ] || return 1
    vs=$(printf '%s\n' "$NAMES" | sed -n 's/^erssi-\([0-9]*\.[0-9]*\.[0-9]*\)-.*\.tar\.xz$/\1/p' | sort -u)
    [ -n "$vs" ] || die "$DIR: not a package name: $(printf '%s\n' "$NAMES" | head -n 1)"
    [ "$(printf '%s\n' "$vs" | wc -l | tr -d ' ')" = 1 ] || die "more than one release in $DIR: $(printf '%s\n' "$vs" | tr '\n' ' ')"
    [ -z "${VERSION:-}" ] || [ "$vs" = "$VERSION" ] || die "the packages in $DIR are of $vs, not $VERSION"
    VERSION=$vs TAG=shellter-v$vs
}
platform_of() { n=${1#erssi-"$VERSION"-}; printf '%s\n' "${n%.tar.xz}"; }
verify_one() {   # package name
    name=$1 p=$(platform_of "$1")
    L=$DIR/logs/$p
    of_commit "$name"
    sum=$(sha256_of "$DIR/$name") || die "cannot compute the sha256 of $name"
    if [ -f "$L/verified" ] && [ "$(cat "$L/verified")" = "$sum  $name" ]; then
        say "  $p: reproduced already (sha256 $sum) - kept"
        return 0
    fi
    case " $ALL_PLATFORMS " in *" $p "*) ;; *) die "$name: no way to build $p again here" ;; esac
    act mkdir -p "$L"
    act rm -rf "$L/verify" "$L/verified"
    case $(how "$p" verify) in
        chroot)
            chroot_of "$p"
            say "  $p: built again in a new chroot ($MM_SUITE)"
            make_bundle
            act mkdir -p "$L/verify"
            # shellcheck disable=SC2088 # ~: the builder's home, in the chroot
            in_chroot "$p" "mkdir -p ~/v && $(from_bundle '~/repo.bundle' '~/erssi') && sh ~/erssi/utils/build-binary.sh --repo ~/erssi --verify ~/$name > ~/v/verify.log 2>&1; echo rc=\$? >> ~/v/verify.log" \
                "$BUNDLE $DIR/$name" v "$L/verify" "$L/mmdebstrap-verify.log"
            [ "$DRY" = 0 ] || return 0
            log=$L/verify/v/verify.log
            rc=$(sed -n 's/^rc=//p' "$log" 2>/dev/null | tail -n 1) ;;
        ssh)
            t=$(target_of "$p" verify)
            [ -n "$t" ] || die "$p: no host to build it again on (--${p%-amd64}-verify or --${p%-amd64})"
            say "  $p: built again on $t"
            remote_dir "$t"
            remote_bundle "$t"
            script="set -u; cd $RDIR || exit 1; $(from_bundle "$RDIR/repo.bundle" "$RDIR/erssi") || exit 1; sh erssi/utils/build-binary.sh --repo $RDIR/erssi --verify $RDIR/$name > verify.log 2>&1; echo rc=\$? >> verify.log; cat verify.log"
            if [ "$DRY" = 1 ]; then
                say "would run: $SSH $t cat \\> $RDIR/$name < $DIR/$name"
                say "would run: $SSH $t sh -s, the script on stdin: $script"
                say "would run: $SSH $t rm -rf $RDIR"
                return 0
            fi
            # shellcheck disable=SC2086 # SSH: the command and its options
            $SSH "$t" "cat > $RDIR/$name" < "$DIR/$name" || die "cannot copy $name to $t"
            mkdir -p "$L/verify" || die "cannot make $L/verify"
            log=$L/verify/verify.log
            # shellcheck disable=SC2086
            printf '%s\n' "$script" | $SSH "$t" sh -s > "$log" 2>&1
            remote_rm
            rc=$(sed -n 's/^rc=//p' "$log" | tail -n 1) ;;
        *) die "$p: no way to build it again" ;;
    esac
    case $rc in
        0)
            grep -q "^build-binary: sha256 $sum\$" "$log" || die "$p: build-binary.sh said reproduced, but not with the sha256 $sum (the log: $log)"
            printf '%s  %s\n' "$sum" "$name" > "$L/verified" || die "cannot write $L/verified"
            say "  $p: reproduced (sha256 $sum)" ;;
        3)
            grep -E '^(build-binary: (NOT|  (given|here))|  (only|differs|given:|here:))' "$log" | head -n 20 >&2
            die "$p: NOT reproduced - not published until the difference is understood (the log: $log)" ;;
        *)
            tail -n 8 "$log" >&2 2>/dev/null
            die "$p: could not build it again (the logs: $L)" ;;
    esac
}
step_verify() {
    if ! packages; then
        [ "$DRY" = 1 ] && { say "verify: no packages in $DIR yet - each one that build writes would be built again"; return 0; }
        die "no erssi-*.tar.xz in $DIR"
    fi
    DIR=$(abs_dir "$DIR") || die "$DIR is not a directory"
    say "verify $TAG: $(printf '%s\n' "$NAMES" | tr '\n' ' ')"
    local_tag
    HOST_PLATFORM=$(host_platform) || HOST_PLATFORM=
    need_tools "$(for n in $NAMES; do platform_of "$n"; done)" verify
    mm_tmpdir
    for n in $NAMES; do verify_one "$n"; done
    say "verify $TAG: every package reproduced"
}

# ── sums ─────────────────────────────────────────────────────────────
# SHA256SUMS of directory $1 signed by SIGNERS for erssi releases, sorted,
# each line naming a package of VERSION that is there with that sum, and
# every package listed; the first problem in PROBLEM
sums_ok() {
    PROBLEM=
    d=$1
    [ -f "$d/SHA256SUMS" ] || { PROBLEM="no SHA256SUMS"; return 1; }
    [ -f "$d/SHA256SUMS.sig" ] || { PROBLEM="no SHA256SUMS.sig"; return 1; }
    work
    signers_for erssi-release "$WORK/release_signers"
    good=0
    # shellcheck disable=SC2013 # principals: words without blanks
    for who in $(awk '{print $1}' "$WORK/release_signers" | sort -u); do
        if ssh-keygen -Y verify -f "$WORK/release_signers" -I "$who" -n erssi-release \
                -s "$d/SHA256SUMS.sig" < "$d/SHA256SUMS" > "$WORK/verify.out" 2>&1; then
            good=1 PRINCIPAL=$who
            break
        fi
    done
    [ "$good" = 1 ] || { PROBLEM="SHA256SUMS.sig is not a valid signature of SHA256SUMS by a key of $SIGNERS (namespace erssi-release)"; return 1; }
    awk '{print $2}' "$d/SHA256SUMS" | LC_ALL=C sort -c 2>/dev/null || { PROBLEM="SHA256SUMS is not sorted"; return 1; }
    # the last line without its newline is read (and checked) too
    while IFS= read -r line || [ -n "$line" ]; do
        sum=${line%%  *} f=${line#*  }
        printf '%s\n' "$sum" | grep -Eq '^[0-9a-f]{64}$' || { PROBLEM="SHA256SUMS: not a sum line: $line"; return 1; }
        case $f in
            "erssi-$VERSION-"*.tar.xz) ;;
            *) PROBLEM="SHA256SUMS lists $f, not a package of $VERSION"; return 1 ;;
        esac
        [ -f "$d/$f" ] || { PROBLEM="SHA256SUMS lists $f, which is missing"; return 1; }
        [ "$(sha256_of "$d/$f")" = "$sum" ] || { PROBLEM="$f does not match its sum in SHA256SUMS"; return 1; }
    done < "$d/SHA256SUMS"
    for f in "$d"/erssi-*.tar.xz; do
        [ -f "$f" ] || continue
        awk -v f="$(basename -- "$f")" '$2 == f { found = 1 } END { exit !found }' "$d/SHA256SUMS" ||
            { PROBLEM="$(basename -- "$f") is not in SHA256SUMS"; return 1; }
    done
    return 0
}
step_sums() {
    if ! packages; then
        [ "$DRY" = 1 ] && { say "sums: no packages in $DIR yet - SHA256SUMS would be written and signed over them"; return 0; }
        die "no erssi-*.tar.xz in $DIR"
    fi
    DIR=$(abs_dir "$DIR") || die "cannot use $DIR"
    say "sums $TAG: $(printf '%s\n' "$NAMES" | wc -l | tr -d ' ') packages in $DIR"
    have ssh-keygen || die "ssh-keygen is needed"
    local_tag
    # only what verify reproduced, of the tag's commit, is signed
    for n in $NAMES; do
        of_commit "$n"
        p=$(platform_of "$n")
        [ -f "$DIR/logs/$p/verified" ] || die "$n has not been built again (release.sh verify $DIR)"
        [ "$(cat "$DIR/logs/$p/verified")" = "$(sha256_of "$DIR/$n")  $n" ] ||
            die "$n is not the package that was reproduced (release.sh verify $DIR)"
    done
    if sums_ok "$DIR"; then
        say "  SHA256SUMS and SHA256SUMS.sig are there and check out - kept"
        say "sums $TAG: ok"
        return 0
    fi
    if [ -z "$KEY" ]; then
        KEY=$(git -C "$REPO" config user.signingkey) || die "no --key, and $REPO has no user.signingkey"
    fi
    # shellcheck disable=SC2088 # a "~/" of the git config
    case $KEY in
        "~/"*) KEY=$HOME/${KEY#"~/"} ;;
        key::*) die "user.signingkey is a key, not a file - give --key FILE" ;;
    esac
    [ -r "$KEY" ] || die "cannot read the key $KEY"
    act rm -f "$DIR/SHA256SUMS" "$DIR/SHA256SUMS.sig"
    if [ "$DRY" = 1 ]; then
        act sh "$UTILS/build-binary.sh" --sums "$DIR"
    else
        sh "$UTILS/build-binary.sh" --sums "$DIR" > /dev/null || die "build-binary.sh --sums failed"
    fi
    act ssh-keygen -Y sign -n erssi-release -f "$KEY" "$DIR/SHA256SUMS" || die "ssh-keygen could not sign $DIR/SHA256SUMS"
    if [ "$DRY" = 1 ]; then
        say "would check: SHA256SUMS.sig against $SIGNERS, and every sum"
        return 0
    fi
    sums_ok "$DIR" || die "the new SHA256SUMS does not check out: $PROBLEM"
    say "  wrote SHA256SUMS and SHA256SUMS.sig, signed by $PRINCIPAL"
    say "sums $TAG: ok"
}

# ── upload ───────────────────────────────────────────────────────────
step_upload() {
    say "upload $TAG from $DIR to $GH_REPO"
    local_tag
    remote_tag
    [ -n "$REMOTE_OBJ" ] || die "$TAG is not on $REMOTE yet - push it first (git push $REMOTE main $TAG): a release is uploaded only for a published tag"
    [ "$REMOTE_OBJ" = "$TAG_OBJ" ] || die "$TAG on $REMOTE is another tag object ($REMOTE_OBJ, here $TAG_OBJ) - nothing uploaded"
    packages || die "no erssi-*.tar.xz in $DIR"
    DIR=$(abs_dir "$DIR") || die "cannot use $DIR"
    for n in $NAMES; do of_commit "$n"; done
    sums_ok "$DIR" || die "$DIR: $PROBLEM - nothing uploaded (release.sh sums $DIR)"
    have gh || die "gh (the GitHub CLI) is needed"
    work
    # --remote and --gh-repo must be the same repository: GitHub has the
    # same tag object (read only, so with --dry-run too)
    if ! gh_obj=$(gh api "repos/$GH_REPO/git/ref/tags/$TAG" --jq .object.sha 2>"$WORK/gh.err"); then
        case $(cat "$WORK/gh.err") in
            *"Not Found"*|*"not found"*) die "$GH_REPO has no tag $TAG, though $REMOTE has it - are --remote and --gh-repo the same repository? Nothing uploaded" ;;
            *) die "gh cannot read the tag $TAG of $GH_REPO: $(tail -n 1 "$WORK/gh.err")" ;;
        esac
    fi
    [ "$gh_obj" = "$TAG_OBJ" ] ||
        die "$TAG on $GH_REPO is another tag object (${gh_obj:-none}, here and on $REMOTE $TAG_OBJ) - are --remote and --gh-repo the same repository? Nothing uploaded"
    say "  $GH_REPO has the same tag object"
    if assets=$(gh release view "$TAG" --repo "$GH_REPO" --json assets --jq '.assets[].name' 2>"$WORK/gh.err"); then
        say "  the release $TAG is there"
    else
        case $(cat "$WORK/gh.err") in
            *"not found"*) ;;
            *) die "gh cannot read the release $TAG of $GH_REPO: $(tail -n 1 "$WORK/gh.err")" ;;
        esac
        assets=
        act gh release create "$TAG" --repo "$GH_REPO" --verify-tag --title "erssi Shellter Edition $VERSION" --notes-from-tag ||
            die "gh could not make the release $TAG"
    fi
    # the packages first: the installer reads SHA256SUMS first, and a
    # package it lists but cannot download means a source build
    for group in "$NAMES" "SHA256SUMS SHA256SUMS.sig"; do
        set --
        for n in $group; do
            if printf '%s\n' "$assets" | grep -qxF "$n"; then say "  $n is there - kept"; else set -- "$@" "$DIR/$n"; fi
        done
        [ $# -gt 0 ] || continue
        act gh release upload "$TAG" --repo "$GH_REPO" "$@" || die "gh could not upload $*"
    done
    say "upload $TAG: done (release.sh confirm $TAG checks what GitHub serves)"
}

# ── confirm ──────────────────────────────────────────────────────────
step_confirm() {
    say "confirm $TAG: the assets of $GH_REPO"
    have gh || die "gh (the GitHub CLI) is needed"
    have ssh-keygen || die "ssh-keygen is needed"
    if [ "$DRY" = 1 ]; then
        say "would run: gh release download $TAG --repo $GH_REPO --dir <a new directory>, then check SHA256SUMS.sig and every sum"
        return 0
    fi
    work
    mkdir "$WORK/assets" || die "cannot make $WORK/assets"
    gh release download "$TAG" --repo "$GH_REPO" --dir "$WORK/assets" > "$WORK/gh.out" 2>&1 ||
        die "cannot download the assets of $TAG: $(tail -n 1 "$WORK/gh.out")"
    DIR=$WORK/assets
    packages || die "the release $TAG has no packages"
    sums_ok "$DIR" || die "the release $TAG: $PROBLEM"
    for f in "$DIR"/* "$DIR"/.[!.]*; do
        [ -e "$f" ] || continue
        b=$(basename -- "$f")
        case $b in SHA256SUMS|SHA256SUMS.sig|erssi-"$VERSION"-*.tar.xz) ;; *) die "the release $TAG has an asset that is not part of it: $b" ;; esac
    done
    say "  signed by $PRINCIPAL; every sum matches: $(for n in $NAMES; do platform_of "$n"; done | tr '\n' ' ')"
    say "confirm $TAG: ok"
}

# ── all ──────────────────────────────────────────────────────────────
step_all() {
    step_check
    step_build
    step_verify
    step_sums
    remote_tag
    if [ -z "$REMOTE_OBJ" ]; then
        say "the packages of $TAG are built, reproduced and signed in $DIR."
        say "Now push the release: git push $REMOTE main $TAG; then run release.sh all $TAG --out $DIR again (it keeps what is done and uploads)."
        [ "$DRY" = 1 ] && say "would stop here until $TAG is on $REMOTE"
        exit 1
    fi
    step_upload
    step_confirm
}

case $STEP_NAME in
    tag-message)
        if [ -n "$REPO" ]; then news=$REPO/NEWS; else news=$(git -C "$UTILS" rev-parse --show-toplevel 2>/dev/null)/NEWS; fi
        [ -r "$news" ] || die "cannot read $news"
        tag_message "$VERSION" < "$news" ;;
    check) step_check ;;
    build) step_build ;;
    verify) step_verify ;;
    sums) step_sums ;;
    upload) step_upload ;;
    confirm) step_confirm ;;
    all) DIR=$OUT; step_all ;;
esac
