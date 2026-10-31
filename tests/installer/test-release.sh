#!/bin/sh
# utils/release.sh against throwaway repositories: tags signed with a
# throwaway SSH key (never the release key, given to release.sh with
# --signers), a local bare repository as the public remote, and a stand-in
# gh that keeps "GitHub releases" in a directory. Nothing is built and
# nothing leaves the temporary directory: no network, no chroot, no ssh.
# Usage: test-release.sh SOURCE_ROOT CASE
#   args         the usage errors (exit 2) and --help
#   tag-message  the tag message from NEWS: the title, a blank line and the
#                intro paragraph wrapped at 72; the entries it refuses
#   check        a good tag, before and after it is pushed; a dirty tree, a
#                tag that differs on the remote, an unsigned, a lightweight
#                and a foreign tag, a wrong message, docs and version that
#                do not name the release
#   dry-run      build, verify and all with --dry-run: what they would run,
#                and that nothing is written
#   sums         SHA256SUMS sorted and signed, checked independently with
#                ssh-keygen; kept when it checks out; only verified packages
#   upload       refused before the tag is on the remote; then the release
#                made, the packages first, the sums last, nothing twice
#   confirm      a release "downloaded" by the stand-in gh: good, and each
#                way it can be wrong
#   retag        packages built, verified and signed for a tag that was
#                then made again on another commit: verify, sums and upload
#                refuse them
# Exit: 0 pass, 1 fail, 77 skip.
# shellcheck disable=SC2016 # sh -c scripts take $1 themselves

if [ -z "${RELEASE_TEST_CLEAN:-}" ]; then
    exec env -i RELEASE_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

src=$(cd "$1" && pwd) || exit 1
CASE=$2
RS=$src/utils/release.sh
for t in git ssh-keygen tar xz awk; do
    command -v "$t" >/dev/null 2>&1 || { echo "skip: no $t"; exit 77; }
done
# shellcheck disable=SC2046 # "2 47" -> $1 $2
set -- $(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
{ [ "${1:-0}" -gt 2 ] || { [ "${1:-0}" -eq 2 ] && [ "${2:-0}" -ge 34 ]; }; } ||
    { echo "skip: git before 2.34 cannot check SSH signatures"; exit 77; }

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
[init]
	defaultBranch = main
[advice]
	detachedHead = false
EOF
printf '%s namespaces="git" %s\n' "$EMAIL" "$(cut -d' ' -f1,2 "$tmp/key.pub")" > "$tmp/signers"
# the signer for the packages, made here the way releasing.md says, not
# by release.sh
printf '%s namespaces="erssi-release" %s\n' "$EMAIL" "$(cut -d' ' -f1,2 "$tmp/key.pub")" > "$tmp/release_signers"
export RELEASE_MM_TMPDIR="$tmp/mm"

# the stand-in gh: releases are directories in $tmp/gh/<tag>, the tags
# are those of the bare repository $FAKE_GH_GIT (gh api .../git/ref/tags/
# <tag> --jq .object.sha), every call is a line of $tmp/gh.log
mkdir -p "$tmp/bin" "$tmp/gh" || exit 1
cat > "$tmp/bin/gh" <<'EOF'
#!/bin/sh
echo "$*" >> "$FAKE_GH_LOG"
if [ "$1" = api ]; then
    case $2 in
        repos/*/git/ref/tags/*) ;;
        *) echo "unexpected api call $2" >&2; exit 1 ;;
    esac
    git --git-dir="$FAKE_GH_GIT" rev-parse --verify -q "refs/tags/${2##*/git/ref/tags/}" ||
        { echo 'gh: Not Found (HTTP 404)' >&2; exit 1; }
    exit 0
fi
[ "$1" = release ] || exit 1
sub=$2 tag=$3
shift 3
dir=
while [ $# -gt 0 ]; do
    case $1 in
        --dir) dir=$2; shift 2 ;;
        --repo|--json|--jq|--title) shift 2 ;;
        --*) shift ;;
        *) break ;;
    esac
done
R=$FAKE_GH_RELEASES/$tag
case $sub in
    create) mkdir "$R" ;;
    view|upload|download) [ -d "$R" ] || { echo "release not found" >&2; exit 1; } ;;
esac
case $sub in
    view) ls "$R" ;;
    upload) for f; do cp "$f" "$R/" || exit 1; done ;;
    download) cp "$R"/* "$dir/" ;;
esac
EOF
chmod 755 "$tmp/bin/gh"
# mmdebstrap is found on every host, so the tests do not depend on whether
# it is installed; it is never run for real (--dry-run, or refused before)
printf '#!/bin/sh\necho "fake mmdebstrap must not run" >&2\nexit 99\n' > "$tmp/bin/mmdebstrap"
chmod 755 "$tmp/bin/mmdebstrap"
export FAKE_GH_LOG="$tmp/gh.log" FAKE_GH_RELEASES="$tmp/gh" FAKE_GH_GIT="$tmp/repo.git"
PATH=$tmp/bin:$PATH
# the same platform on every host: Debian 13 is built "on this host"
export RELEASE_HOST_PLATFORM=debian13-amd64

# run release.sh with the throwaway signers: RC, OUT
run() {
    sh "$RS" --signers "$tmp/signers" "$@" > "$tmp/out" 2>&1
    RC=$?
    OUT=$(cat "$tmp/out")
}
says() { printf '%s\n' "$OUT" | grep -q -- "$1"; }
expect() {   # description rc pattern
    if [ "$RC" = "$2" ] && says "$3"; then ok "$1"; else bad "$1 (exit $RC, want $2 and '$3'): $OUT"; fi
}

# NEWS with the entry of $1: title, intro, a second paragraph, a section;
# then an older entry
news() {
    printf 'shellter-v%s %s  y-o-o-z <https://github.com/y-o-o-z/erssi>\n' "$1" "$2"
    printf '\terssi Shellter Edition %s\n\n' "$1"
    printf '\tReady-made binaries for more systems, packages that can be\n'
    printf '\tbuilt again and compared,\n'
    printf '\ta notice of new releases, clickable links, and backup and restore of the whole setup.\n\n'
    printf '\tA second paragraph that is not in the tag message.\n\n'
    printf '\t+ Install\n\t  - something\n\n'
    printf 'shellter-v2026.10.8 2026-10-30  y-o-o-z <https://github.com/y-o-o-z/erssi>\n'
    printf '\terssi Shellter Edition 2026.10.8\n\n\tOlder.\n'
}
# a repository $1 at release $2 (dated $3), tagged with the message from
# NEWS, signed, with a bare remote $1.git as origin that has main only.
# MAKE_README_VERSION puts another version into README.md.
make_repo() {
    r=$1 v=$2
    rm -rf "$r" "$r.git"
    git init -q "$r" && git init -q --bare "$r.git" || exit 1
    mkdir -p "$r/docs/shellter"
    printf "project('erssi', 'c',\n  version : '%s',\n)\n" "$v" > "$r/meson.build"
    news "$v" "$3" > "$r/NEWS"
    printf '[![Release](https://img.shields.io/badge/release-%s-f59e0b.svg)]\n| **erssi Shellter Edition %s** | ... |\n' \
        "${MAKE_README_VERSION:-$v}" "$v" > "$r/README.md"
    printf 'verify-tag shellter-v%s\n\nv=%s p=debian13-amd64\n' "$v" "$v" > "$r/docs/shellter/install.md"
    git -C "$r" add -A && git -C "$r" commit -q -S -m "erssi Shellter Edition $v" || exit 1
    git -C "$r" remote add origin "$r.git" && git -C "$r" push -q origin main 2>/dev/null || exit 1
    sh "$RS" tag-message --repo "$r" "$v" > "$tmp/msg" || { echo "FAIL - no tag message for $v"; exit 1; }
    git -C "$r" tag -s -F "$tmp/msg" "shellter-v$v" || exit 1
}
sha256_hex() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1; else sha256 -q; fi
}
# a package $2 whose MANIFEST names commit $3, in directory $1
fake_package() {
    t=${2%.tar.xz}
    mkdir -p "$tmp/pk/$t" && printf 'commit=%s\ntag=x\n' "$3" > "$tmp/pk/$t/MANIFEST" || exit 1
    (cd "$tmp/pk" && tar -cf - "$t") | xz -c > "$1/$2" || exit 1
    rm -rf "$tmp/pk"
}
# the release directory $1 of 2026.11.0: two packages of commit $2 (by
# default the commit of $TAG in $R), marked as reproduced
make_packages() {
    rm -rf "$1" && mkdir -p "$1" || exit 1
    c=${2:-$(git -C "$R" rev-parse "$TAG^{commit}")} || exit 1
    for p in freebsd14-amd64 debian13-amd64; do
        n=erssi-2026.11.0-$p.tar.xz
        fake_package "$1" "$n" "$c"
        mkdir -p "$1/logs/$p"
        printf '%s  %s\n' "$(sha256_hex < "$1/$n")" "$n" > "$1/logs/$p/verified"
    done
}

R=$tmp/repo
TAG=shellter-v2026.11.0

case $CASE in
args)
    run; expect "no step: usage" 2 'no step given'
    run frobnicate; expect "an unknown step" 2 'unknown step frobnicate'
    run check; expect "check without a tag" 2 'check takes 1 argument'
    run check "$TAG" extra; expect "check with two arguments" 2 'check takes 1 argument'
    run check v2026.11.0; expect "check of a name that is no release tag" 2 'is not a release tag'
    run check shellter-v2026.13.0; expect "check of a month 13" 2 'is not a calendar version'
    run build "$TAG"; expect "build without --out" 2 'build needs --out DIR'
    run all "$TAG"; expect "all without --out" 2 'all needs --out DIR'
    run upload "$TAG"; expect "upload without a directory" 2 'upload takes 2 argument'
    run sums; expect "sums without a directory" 2 'sums takes 1 argument'
    run tag-message; expect "tag-message without a version" 2 'tag-message takes 1 argument'
    run tag-message 2026.11; expect "tag-message of a short version" 2 'is not a calendar version'
    run --frob check "$TAG"; expect "an unknown option" 2 'unknown option --frob'
    run check "$TAG" --repo; expect "an option without its value" 2 '--repo needs a value'
    run build "$TAG" --out "$tmp/o" --platforms 'debian13-amd64 hurd1-amd64'; expect "an unknown platform" 2 'unknown platform hurd1-amd64'
    run build "$TAG" --out "$tmp/o" --platforms freebsd14-amd64; expect "FreeBSD without its host" 2 'freebsd14-amd64 needs --freebsd14 TARGET'
    run build "$TAG" --out "$tmp/o" --freebsd14 -oProxyCommand=x; expect "an ssh destination that is an option" 2 'is not an ssh destination'
    run build "$TAG" --out "$tmp/o" --freebsd14 'a;b'; expect "an ssh destination with a ;" 2 'is not an ssh destination'
    run --help; expect "--help" 0 'Usage: release.sh'
    check "no usage error wrote anything" [ ! -e "$tmp/o" ]
    run verify "$tmp/nowhere"; expect "verify of a missing directory" 1 'no erssi-\*.tar.xz in'
    ;;
tag-message)
    mkdir -p "$R" && news 2026.11.0 2026-11-02 > "$R/NEWS" || exit 1
    git init -q "$R" || exit 1
    run tag-message --repo "$R" 2026.11.0
    want='erssi Shellter Edition 2026.11.0

Ready-made binaries for more systems, packages that can be built again
and compared, a notice of new releases, clickable links, and backup and
restore of the whole setup.'
    check "the title, a blank line, the intro wrapped at 72 (exit $RC)" [ "$RC" = 0 ] && [ "$OUT" = "$want" ]
    # the wrapping, independently: no line over 72, and no line that the
    # first word of the next one would have fitted on
    printf '%s\n' "$OUT" | sed '1,2d' > "$tmp/para"
    check "... no line longer than 72" awk 'length($0) > 72 { exit 1 }' "$tmp/para"
    check "... greedy: the next word never fits on a line" awk 'NR > 1 { split($0, w, " "); if (length(prev) + 1 + length(w[1]) <= 72) exit 1 } { prev = $0 }' "$tmp/para"
    check "... not the second paragraph" sh -c '! grep -q "second paragraph" "$1"' sh "$tmp/para"
    run tag-message --repo "$R" 2026.10.8
    check "an older entry: its own intro" [ "$OUT" = "$(printf 'erssi Shellter Edition 2026.10.8\n\nOlder.')" ]
    run tag-message --repo "$R" 2026.11.1; expect "a version that NEWS has no entry for" 1 'NEWS has no entry shellter-v2026.11.1'
    printf 'shellter-v2026.11.1 (unreleased)\n\terssi Shellter Edition 2026.11.1\n\n\t+ Install\n\t  - x\n' > "$R/NEWS"
    run tag-message --repo "$R" 2026.11.1; expect "an entry without an intro paragraph" 1 'has no intro paragraph'
    printf 'shellter-v2026.11.1 (unreleased)\n\tSomething else\n\n\tIntro.\n' > "$R/NEWS"
    run tag-message --repo "$R" 2026.11.1; expect "an entry without the title line" 1 'does not start with the title line'
    rm -f "$R/NEWS"
    run tag-message --repo "$R" 2026.11.1; expect "no NEWS" 1 'cannot read'
    ;;
check)
    make_repo "$R" 2026.11.0 2026-11-02
    run check --repo "$R" "$TAG"
    expect "a good tag, not pushed yet" 0 'not on origin yet'
    check "... names the signed commit" says "signed tag $TAG, commit $(git -C "$R" rev-parse "$TAG^{commit}")"
    : > "$R/stray"
    run check --repo "$R" "$TAG"; expect "a working tree that is not clean" 1 'is not clean'
    rm -f "$R/stray"
    git -C "$R" push -q origin "$TAG" || exit 1
    run check --repo "$R" "$TAG"; expect "the same tag on the remote" 0 'already on origin, the same tag object'
    # another tag object of that name on the remote: made in a clone
    git clone -q "$R.git" "$tmp/c" && git -C "$tmp/c" tag -d "$TAG" >/dev/null &&
        git -C "$tmp/c" tag -s -m "erssi Shellter Edition 2026.11.0 again" "$TAG" || exit 1
    git -C "$tmp/c" push -q --force origin "$TAG" 2>/dev/null || exit 1
    run check --repo "$R" "$TAG"; expect "another tag object on the remote" 1 'is another tag object'
    check "... a published tag never moves" says 'never moves'
    git -C "$R" push -q --force origin "$TAG" || exit 1
    run check --repo "$R" "$TAG" --remote "$tmp/nowhere.git"; expect "a remote that cannot be reached" 1 'cannot reach'

    git -C "$R" tag -a -m "erssi Shellter Edition 2026.11.1" shellter-v2026.11.1 || exit 1
    run check --repo "$R" shellter-v2026.11.1; expect "an unsigned annotated tag" 1 'is not signed by a key of'
    git -C "$R" -c user.signingkey="$tmp/other" tag -s -m "erssi Shellter Edition 2026.11.2" shellter-v2026.11.2 || exit 1
    run check --repo "$R" shellter-v2026.11.2; expect "a tag signed by another key" 1 'is not signed by a key of'
    git -C "$R" tag shellter-v2026.11.3 || exit 1
    run check --repo "$R" shellter-v2026.11.3; expect "a lightweight tag" 1 'is not an annotated tag'
    run check --repo "$R" shellter-v2026.11.4; expect "a tag that is not there" 1 'no such tag'
    git -C "$R" tag -s -F "$tmp/msg" shellter-v2026.11.5 || exit 1
    run check --repo "$R" shellter-v2026.11.5; expect "a tag that meson.build and NEWS do not name" 1 'check-version'

    # the message from 2026.10.9 on; an older release needs the title only
    R2=$tmp/repo2
    make_repo "$R2" 2026.11.0 2026-11-02
    git -C "$R2" tag -d "$TAG" >/dev/null && git -C "$R2" tag -s -m "erssi Shellter Edition 2026.11.0" "$TAG" || exit 1
    run check --repo "$R2" "$TAG"; expect "a 2026.11 tag with the title only" 1 'is not the one from NEWS'
    make_repo "$R2" 2026.10.7 2026-10-30
    git -C "$R2" tag -d shellter-v2026.10.7 >/dev/null && git -C "$R2" tag -s -m "erssi Shellter Edition 2026.10.7" shellter-v2026.10.7 || exit 1
    run check --repo "$R2" shellter-v2026.10.7; expect "a tag before 2026.10.9 with the title only" 0 'check shellter-v2026.10.7: ok'
    MAKE_README_VERSION=2026.10.8 make_repo "$R2" 2026.11.0 2026-11-02
    run check --repo "$R2" "$TAG"; expect "README.md that names another release" 1 'does not bring the docs'
    check "... names the line" says 'README.md: the release badge does not name 2026.11.0'
    ;;
dry-run)
    make_repo "$R" 2026.11.0 2026-11-02
    run build --repo "$R" "$TAG" --out "$tmp/out-dir" --freebsd15 builder@example --dry-run
    expect "build --dry-run" 0 "build $TAG: done"
    check "... Debian 12 in a bookworm chroot" sh -c 'grep "would run:.*mmdebstrap" "$1" | grep -q " bookworm /dev/null"' sh "$tmp/out"
    check "... Ubuntu 24.04 in noble, with universe and its key" sh -c 'grep "would run:.*mmdebstrap" "$1" | grep -- "--components=main,universe" | grep -q " noble /dev/null http://archive.ubuntu.com/ubuntu"' sh "$tmp/out"
    check "... Debian 13 on this host" grep -q "debian13-amd64: on this host" "$tmp/out"
    check "... the tag from a bundle of the local tag" sh -c 'grep -q "would run: git bundle create .* refs/tags/$2" "$1" && grep -q "fetch -q ~/repo.bundle refs/tags/$2:refs/tags/$2" "$1"' sh "$tmp/out" "$TAG"
    check "... the tag's own build-binary.sh, as builder" sh -c 'grep -q "su builder -c .*sh ~/erssi/utils/build-binary.sh --repo ~/erssi --out ~/out $2" "$1"' sh "$tmp/out" "$TAG"
    check "... FreeBSD 15 over ssh" sh -c 'grep -q "would run: ssh builder@example sh -s" "$1" && grep -q "would run: ssh builder@example cat .*/out/erssi-2026.11.0-freebsd15-amd64.tar.xz" "$1"' sh "$tmp/out"
    check "... no FreeBSD 14 without its host" sh -c '! grep -q freebsd14 "$1"' sh "$tmp/out"
    check "... nothing written" sh -c '[ ! -e "$1" ] && [ ! -e "$2" ]' sh "$tmp/out-dir" "$tmp/mm"
    # exported: FreeBSD sh does not pass an assignment before a function on
    export RELEASE_SSH='ssh -p 2222'
    run build --repo "$R" "$TAG" --out "$tmp/out-dir" --platforms freebsd14-amd64 --freebsd14 b14 --dry-run
    unset RELEASE_SSH
    expect "RELEASE_SSH: the ssh command" 0 'would run: ssh -p 2222 b14 sh -s'
    run build --repo "$R" "$TAG" --out "$R/out" --dry-run; expect "an --out inside the repository" 1 'is inside'

    # packages there already: kept when of the tag's commit, refused otherwise
    D=$tmp/pkgs
    mkdir -p "$D"
    fake_package "$D" erssi-2026.11.0-debian12-amd64.tar.xz "$(git -C "$R" rev-parse "$TAG^{commit}")"
    run build --repo "$R" "$TAG" --out "$D" --platforms debian12-amd64 --dry-run
    expect "build: a package of the commit is kept" 0 'debian12-amd64: erssi-2026.11.0-debian12-amd64.tar.xz is there (commit'
    check "... and not built again" sh -c '! grep -q "would run:.*mmdebstrap" "$1"' sh "$tmp/out"
    fake_package "$D" erssi-2026.11.0-debian12-amd64.tar.xz 0123456789012345678901234567890123456789
    run build --repo "$R" "$TAG" --out "$D" --platforms debian12-amd64 --dry-run
    expect "build: a package of another commit is refused" 1 'built from 0123456789012345678901234567890123456789'

    make_packages "$D"
    rm -f "$D"/logs/*/verified
    rm -f "$D/erssi-2026.11.0-debian12-amd64.tar.xz"
    run verify --repo "$R" "$D" --dry-run
    expect "verify --dry-run of a FreeBSD package without its host" 1 'freebsd14-amd64: no host to build it again on'
    run verify --repo "$R" "$D" --freebsd14 b14 --freebsd14-verify v14 --dry-run
    expect "verify --dry-run" 0 'verify shellter-v2026.11.0: every package reproduced'
    check "... Debian 13 in a new trixie chroot, with the package copied in" sh -c 'grep "would run:.*mmdebstrap" "$1" | grep "copy-in $2/erssi-2026.11.0-debian13-amd64.tar.xz /home/builder" | grep -q " trixie /dev/null"' sh "$tmp/out" "$D"
    check "... build-binary.sh --verify there" sh -c 'grep -q "build-binary.sh --repo ~/erssi --verify ~/erssi-2026.11.0-debian13-amd64.tar.xz" "$1"' sh "$tmp/out"
    check "... FreeBSD 14 on its verify host" sh -c 'grep -q "would run: ssh v14 sh -s" "$1" && ! grep -q "ssh b14" "$1"' sh "$tmp/out"
    check "... nothing marked as verified" sh -c '! ls "$1"/logs/*/verified >/dev/null 2>&1' sh "$D"

    # all: stops before the upload while the tag is only here
    run all --repo "$R" "$TAG" --out "$tmp/all-dir" --platforms debian12-amd64 --dry-run
    expect "all --dry-run before the push: stops after sums" 1 "Now push the release: git push origin main $TAG"
    check "... after check and build" sh -c 'grep -q "check $2: ok" "$1" && grep -q "debian12-amd64: in a new chroot (bookworm)" "$1"' sh "$tmp/out" "$TAG"
    check "... no upload" sh -c '! grep -q "gh release" "$1" && [ ! -e "$2" ]' sh "$tmp/out" "$tmp/gh.log"
    ;;
sums)
    make_repo "$R" 2026.11.0 2026-11-02
    D=$tmp/rel
    make_packages "$D"
    rm -f "$D/logs/freebsd14-amd64/verified"
    run sums --repo "$R" --key "$tmp/key" "$D"; expect "sums: a package not built again is refused" 1 'erssi-2026.11.0-freebsd14-amd64.tar.xz has not been built again'
    make_packages "$D"
    echo changed >> "$D/erssi-2026.11.0-debian13-amd64.tar.xz"
    run sums --repo "$R" --key "$tmp/key" "$D"; expect "sums: a package that is not the one reproduced" 1 'is not the package that was reproduced'
    check "... no SHA256SUMS" [ ! -e "$D/SHA256SUMS" ]
    make_packages "$D"
    run sums --repo "$R" --key "$tmp/key" "$D" --dry-run
    expect "sums --dry-run" 0 'would run: ssh-keygen -Y sign -n erssi-release -f'
    check "... writes nothing" sh -c '[ ! -e "$1/SHA256SUMS" ] && [ ! -e "$1/SHA256SUMS.sig" ]' sh "$D"
    run sums --repo "$R" --key "$tmp/key" "$D"
    expect "sums: written and signed" 0 "signed by $EMAIL"
    want=$(for f in erssi-2026.11.0-debian13-amd64.tar.xz erssi-2026.11.0-freebsd14-amd64.tar.xz; do
        printf '%s  %s\n' "$(sha256_hex < "$D/$f")" "$f"; done)
    check "SHA256SUMS: the packages, sorted, '<sha256>  <name>'" [ "$(cat "$D/SHA256SUMS")" = "$want" ]
    ssh-keygen -Y verify -f "$tmp/release_signers" -I "$EMAIL" -n erssi-release -s "$D/SHA256SUMS.sig" \
        < "$D/SHA256SUMS" > /dev/null 2>&1
    check "the signature verifies, namespace erssi-release" [ $? = 0 ]
    ! ssh-keygen -Y verify -f "$tmp/release_signers" -I "$EMAIL" -n git -s "$D/SHA256SUMS.sig" \
        < "$D/SHA256SUMS" > /dev/null 2>&1
    check "... and not as a git signature" [ $? = 0 ]
    cp "$D/SHA256SUMS.sig" "$tmp/sig.1"
    run sums --repo "$R" --key "$tmp/key" "$D"
    expect "sums again: kept" 0 'are there and check out - kept'
    check "... the signature unchanged" cmp -s "$D/SHA256SUMS.sig" "$tmp/sig.1"
    # the key from git config user.signingkey
    rm -f "$D/SHA256SUMS" "$D/SHA256SUMS.sig"
    run sums --repo "$R" "$D"
    expect "sums: the key of user.signingkey" 0 "signed by $EMAIL"
    rm -f "$D/SHA256SUMS"
    run sums --repo "$R" --key "$tmp/other" "$D"
    expect "sums: a key that is not in --signers" 1 'does not check out: SHA256SUMS.sig is not a valid signature'
    rm -f "$D/SHA256SUMS" "$D/SHA256SUMS.sig"
    echo x > "$D/erssi-2026.11.1-debian13-amd64.tar.xz"
    run sums --repo "$R" --key "$tmp/key" "$D"; expect "sums: two releases in one directory" 1 'more than one release'
    ;;
upload|confirm)
    make_repo "$R" 2026.11.0 2026-11-02
    D=$tmp/rel
    make_packages "$D"
    run sums --repo "$R" --key "$tmp/key" "$D"
    [ "$RC" = 0 ] || { echo "FAIL - sums: $OUT"; exit 1; }
    if [ "$CASE" = confirm ]; then
        git -C "$R" push -q origin "$TAG" || exit 1
        run upload --repo "$R" "$TAG" "$D"
        [ "$RC" = 0 ] || { echo "FAIL - upload: $OUT"; exit 1; }
    fi
    if [ "$CASE" = upload ]; then
        run upload --repo "$R" "$TAG" "$D"
        expect "upload before the tag is pushed: refused" 1 "$TAG is not on origin yet - push it first"
        check "... gh never ran" [ ! -e "$tmp/gh.log" ]
        git -C "$R" push -q origin "$TAG" || exit 1
        # --gh-repo that is not the repository of --remote
        git init -q --bare "$tmp/empty.git" || exit 1
        FAKE_GH_GIT=$tmp/empty.git run upload --repo "$R" "$TAG" "$D"
        expect "upload: --gh-repo without the tag" 1 'y-o-o-z/erssi has no tag shellter-v2026.11.0, though origin has it'
        git clone -q "$R.git" "$tmp/c2" && git -C "$tmp/c2" tag -d "$TAG" >/dev/null &&
            git -C "$tmp/c2" tag -s -m "erssi Shellter Edition 2026.11.0 elsewhere" "$TAG" &&
            git init -q --bare "$tmp/elsewhere.git" && git -C "$tmp/c2" push -q "$tmp/elsewhere.git" "$TAG" 2>/dev/null || exit 1
        FAKE_GH_GIT=$tmp/elsewhere.git run upload --repo "$R" "$TAG" "$D"
        expect "upload: --gh-repo with another tag object" 1 'on y-o-o-z/erssi is another tag object'
        check "... nothing made or uploaded" sh -c '! grep -Eq "^release (create|upload)" "$1"' sh "$tmp/gh.log"
        rm -f "$tmp/gh.log"
        run upload --repo "$R" "$TAG" "$D" --dry-run
        expect "upload --dry-run" 0 'would run: gh release create'
        check "... only looked" sh -c '[ "$(grep -v "^api repos/y-o-o-z/erssi/git/ref/tags/$3 " "$1" | cut -d" " -f2 | sort -u)" = view ] && [ ! -e "$2" ]' sh "$tmp/gh.log" "$tmp/gh/$TAG" "$TAG"
        rm -f "$tmp/gh.log"
        run upload --repo "$R" "$TAG" "$D"
        expect "upload" 0 "upload $TAG: done"
        check "... the release made from the tag" grep -q "^release create $TAG --repo y-o-o-z/erssi --verify-tag --title erssi Shellter Edition 2026.11.0 --notes-from-tag\$" "$tmp/gh.log"
        pk=$(grep -n "^release upload .*erssi-2026.11.0-debian13-amd64.tar.xz .*erssi-2026.11.0-freebsd14-amd64.tar.xz\$" "$tmp/gh.log" | cut -d: -f1)
        su=$(grep -n "^release upload .*/SHA256SUMS .*/SHA256SUMS.sig\$" "$tmp/gh.log" | cut -d: -f1)
        check "... the packages first, SHA256SUMS and its signature last" sh -c '[ -n "$1" ] && [ -n "$2" ] && [ "$1" -lt "$2" ]' sh "$pk" "$su"
        check "... the sums alone in the last upload" sh -c '! grep "^release upload .*SHA256SUMS" "$1" | grep -q tar.xz' sh "$tmp/gh.log"
        rm -f "$tmp/gh.log"
        run upload --repo "$R" "$TAG" "$D"
        expect "upload again: everything kept" 0 'SHA256SUMS.sig is there - kept'
        check "... nothing made or uploaded" sh -c '! grep -Eq "^release (create|upload)" "$1"' sh "$tmp/gh.log"
        cp "$D/erssi-2026.11.0-debian13-amd64.tar.xz" "$tmp/keep.tar.xz"
        echo changed >> "$D/erssi-2026.11.0-debian13-amd64.tar.xz"
        run upload --repo "$R" "$TAG" "$D"
        expect "upload of a package that does not match SHA256SUMS: refused" 1 'does not match its sum in SHA256SUMS - nothing uploaded'
        cp "$tmp/keep.tar.xz" "$D/erssi-2026.11.0-debian13-amd64.tar.xz"
    fi

    if [ "$CASE" = confirm ]; then
        G=$tmp/gh/$TAG
        run confirm --repo "$R" "$TAG"
        expect "confirm" 0 "confirm $TAG: ok"
        check "... names the platforms" says 'every sum matches: debian13-amd64 freebsd14-amd64'
        echo changed >> "$G/erssi-2026.11.0-freebsd14-amd64.tar.xz"
        run confirm --repo "$R" "$TAG"; expect "confirm: a package that does not match" 1 'erssi-2026.11.0-freebsd14-amd64.tar.xz does not match its sum'
        cp "$D/erssi-2026.11.0-freebsd14-amd64.tar.xz" "$G/"
        rm "$G/erssi-2026.11.0-freebsd14-amd64.tar.xz"
        run confirm --repo "$R" "$TAG"; expect "confirm: a package missing" 1 'erssi-2026.11.0-freebsd14-amd64.tar.xz, which is missing'
        cp "$D/erssi-2026.11.0-freebsd14-amd64.tar.xz" "$G/"
        echo notes > "$G/notes.txt"
        run confirm --repo "$R" "$TAG"; expect "confirm: an asset that is not part of a release" 1 'not part of it: notes.txt'
        rm "$G/notes.txt"
        (cd "$G" && rm -f SHA256SUMS.sig && ssh-keygen -q -Y sign -n erssi-release -f "$tmp/other" SHA256SUMS) 2>/dev/null
        run confirm --repo "$R" "$TAG"; expect "confirm: SHA256SUMS signed by another key" 1 'is not a valid signature'
        (cd "$G" && rm -f SHA256SUMS.sig && ssh-keygen -q -Y sign -n git -f "$tmp/key" SHA256SUMS) 2>/dev/null
        run confirm --repo "$R" "$TAG"; expect "confirm: SHA256SUMS signed for git, not erssi-release" 1 'is not a valid signature'
        (cd "$G" && sort -k2r SHA256SUMS > s && mv s SHA256SUMS && rm -f SHA256SUMS.sig &&
            ssh-keygen -q -Y sign -n erssi-release -f "$tmp/key" SHA256SUMS) 2>/dev/null
        run confirm --repo "$R" "$TAG"; expect "confirm: a signed SHA256SUMS out of order" 1 'SHA256SUMS is not sorted'
        # the last line without its newline: its sum is checked all the same
        (cd "$G" && printf '%s' "$(cat "$D/SHA256SUMS")" > SHA256SUMS && rm -f SHA256SUMS.sig &&
            ssh-keygen -q -Y sign -n erssi-release -f "$tmp/key" SHA256SUMS) 2>/dev/null
        check "... (the stand-in SHA256SUMS has no last newline)" sh -c '[ "$(tail -c 1 "$1" | od -An -c | tr -d " ")" != "\\n" ]' sh "$G/SHA256SUMS"
        echo changed >> "$G/erssi-2026.11.0-freebsd14-amd64.tar.xz"
        run confirm --repo "$R" "$TAG"; expect "confirm: the last sum line without its newline is checked" 1 'erssi-2026.11.0-freebsd14-amd64.tar.xz does not match its sum'
        cp "$D/erssi-2026.11.0-freebsd14-amd64.tar.xz" "$D/SHA256SUMS" "$D/SHA256SUMS.sig" "$G/"
        run confirm --repo "$R" "$TAG"; expect "confirm: good again" 0 "confirm $TAG: ok"
        run confirm --repo "$R" shellter-v2026.11.9; expect "confirm: no such release" 1 'cannot download the assets of shellter-v2026.11.9'
    fi
    ;;
retag)
    # built, verified and signed on commit A; then the tag is deleted and
    # made again on commit B, and pushed
    make_repo "$R" 2026.11.0 2026-11-02
    A=$(git -C "$R" rev-parse "$TAG^{commit}")
    D=$tmp/rel
    make_packages "$D"
    run sums --repo "$R" --key "$tmp/key" "$D"
    [ "$RC" = 0 ] || { echo "FAIL - sums: $OUT"; exit 1; }
    git -C "$R" commit -q -S --allow-empty -m "erssi Shellter Edition 2026.11.0, again" && git -C "$R" tag -d "$TAG" >/dev/null &&
        git -C "$R" tag -s -F "$tmp/msg" "$TAG" && git -C "$R" push -q origin main "$TAG" 2>/dev/null || exit 1
    B=$(git -C "$R" rev-parse "$TAG^{commit}")
    check "the tag is on another commit now" [ "$A" != "$B" ]
    run verify --repo "$R" "$D"
    expect "verify: a package verified for the old commit is not kept" 1 "was built from $A, not from $B, the commit of $TAG"
    run sums --repo "$R" --key "$tmp/key" "$D"
    expect "sums: SHA256SUMS of the old commit is not kept" 1 "was built from $A, not from $B"
    run upload --repo "$R" "$TAG" "$D"
    expect "upload: packages of the old commit are refused" 1 "was built from $A, not from $B"
    check "... gh never ran" [ ! -e "$tmp/gh.log" ]
    # a directory that cannot go unquoted into a command line
    mkdir -p "$tmp/a b" && cp -R "$D" "$tmp/a b/rel" || exit 1
    run upload --repo "$R" "$TAG" "$tmp/a b/rel"
    expect "upload: a directory with a blank is refused" 1 'may hold letters, digits and ._+/- only'
    ;;
*) echo "FAIL - unknown case $CASE"; exit 1 ;;
esac

[ "$failed" = 0 ] || { echo "--- last output ---"; printf '%s\n' "$OUT"; }
exit "$failed"
