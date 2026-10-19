#!/bin/sh
# shellter-install.sh against an isolated PATH of stand-in tools, a local git
# repository and a temporary HOME: no network, no root, nothing outside a
# temporary directory is touched.
# Usage: test-shellter-install.sh SOURCE_ROOT CASE
# Exit: 0 pass, 1 fail, 77 skip.

# a clean environment: nothing from the caller's locale, HOME, XDG or git
if [ -z "${SHELLTER_TEST_CLEAN:-}" ]; then
    exec env -i SHELLTER_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

SRC=$1
CASE=$2
FAILED=0
pass() { echo "ok $CASE: $1"; }
fail() { echo "FAIL $CASE: $1"; FAILED=1; }
check() {   # description command...
    d=$1; shift
    if "$@"; then pass "$d"; else fail "$d"; fi
}

# the first executable $1 in PATH (command -v names builtins)
find_tool() {
    oifs=$IFS; IFS=:
    for d in $PATH; do
        if [ -f "$d/$1" ] && [ -x "$d/$1" ]; then IFS=$oifs; printf '%s\n' "$d/$1"; return 0; fi
    done
    IFS=$oifs
    return 1
}

find_tool git >/dev/null || { echo "skip: no git"; exit 77; }
SCRIPT=$(find_tool script) || SCRIPT=""
UNSHARE=$(find_tool unshare) || UNSHARE=""
KEYGEN=$(find_tool ssh-keygen) || KEYGEN=""
# what the binary cases need, as found before setup narrows PATH
BIN_TOOLS=""
for t in tar xz sha256sum find cmp perl; do
    p=$(find_tool "$t") && BIN_TOOLS="$BIN_TOOLS $t=$p"
done

# stand-in tool $1 from stdin, kept in $W/stand; use_tool puts it in PATH
stand() { cat > "$W/stand/$1" && chmod 755 "$W/stand/$1"; }
use_tool() { for t in "$@"; do cp "$W/stand/$t" "$W/shim/$t"; done; }

setup() {
    W=$(mktemp -d) && W=$(cd -P "$W" && pwd -P) || exit 1
    trap 'rm -rf "$W"' EXIT
    # The BSDs give a new directory the group of its parent, not the
    # creator's: under /tmp (group wheel) every fixture would be in a group
    # with other members, and none of the "private group" cases could pass.
    # Everything below $W inherits the user's own group from here (on
    # Linux too, when $TMPDIR is set-group-ID)
    chgrp "$(id -g)" "$W" || exit 1
    trap 'exit 1' HUP INT TERM
    TEST_W=$W; export TEST_W
    HOME=$W/home; export HOME
    GIT_CONFIG_NOSYSTEM=1; export GIT_CONFIG_NOSYSTEM
    umask 022
    mkdir -p "$HOME" "$W/bin" "$W/shim" "$W/stand"
    for t in sh awk sed grep date mktemp ls cut tr tail head sort rm mkdir rmdir cp mv ln chmod cat \
            dirname basename sleep env uname id git readlink wc touch; do
        p=$(find_tool "$t") || { echo "FAIL $CASE: no $t on this system"; exit 1; }
        ln -s "$p" "$W/bin/$t"
    done

    stand pkg-config <<'EOF'
#!/bin/sh
mode=$1; shift
case "$mode" in
    --exists)
        case " $* " in *openssl*) [ "${FAKE_NO_OPENSSL_PC:-0}" = 1 ] && exit 1 ;; esac
        exit 0 ;;
    --modversion)
        case "$1" in
            glib-2.0) echo 2.80.0 ;;
            openssl) [ "${FAKE_NO_OPENSSL_PC:-0}" = 1 ] && exit 1; echo 3.0.15 ;;
            *) echo 1.0 ;;
        esac ;;
esac
exit 0
EOF
    printf '#!/bin/sh\nexit 0\n' | stand cc
    # perl runs in step 1 (check_system): FAKE_PLANT_VENV makes another
    # user's venv ($W/planted) appear there, after the lock was taken
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\nif [ -n "${FAKE_PLANT_VENV:-}" ] && [ -d "$TEST_W/planted" ]; then\n    mv "$TEST_W/planted" "$FAKE_PLANT_VENV" && echo "$FAKE_PLANT_VENV 4242 -" >> "$TEST_W/lsfake"\nfi\nexit 0\n' | stand perl
    stand meson <<'EOF'
#!/bin/sh
case "$1" in
    --version) echo 1.12.1 ;;
    setup)
        shift
        [ "$1" != --reconfigure ] || shift
        b=$1; shift 2
        p=""
        for a in "$@"; do case "$a" in -Dprefix=*) p=${a#-Dprefix=} ;; esac; done
        mkdir -p "$b" && : > "$b/build.ninja" && printf '%s\n' "$p" > "$b/prefix" || exit 1
        if [ "${FAKE_SLOW:-0}" = 1 ]; then sleep 1.5; fi ;;
esac
exit 0
EOF
    stand ninja <<'EOF'
#!/bin/sh
[ "$1" != --version ] || { echo 1.13.2; exit 0; }
if [ "$1" = -C ] && [ "${3:-}" = install ]; then
    p=$(cat "$2/prefix") && mkdir -p "$p/bin" || exit 1
    # the version of the checkout (release_commit), 9.9.9 without one
    v=9.9.9; [ ! -f "$2/../VERSION" ] || v=$(cat "$2/../VERSION")
    printf '#!/bin/sh\necho "erssi %s (20261010 1200)"\n' "$v" > "$p/bin/erssi" && chmod 755 "$p/bin/erssi"
fi
exit 0
EOF
    stand locale <<'EOF'
#!/bin/sh
[ "$1" = charmap ] && printf '%s\n' "${FAKE_CHARMAP:-}"
exit 0
EOF
    # getent group: the user's group, members $FAKE_GROUP_MEMBERS (none)
    stand getent <<'EOF'
#!/bin/sh
[ "$1" = group ] && echo "$2:x:$("$TEST_W/bin/id" -g):${FAKE_GROUP_MEMBERS:-}"
EOF
    use_tool pkg-config cc perl meson ninja locale getent
    PATH=$W/shim:$W/bin; export PATH
    make_repo
    # the installer refuses a --src under a directory others can write to:
    # $W and every directory above it must pass its own check
    if ! msg=$(SHELLTER_INSTALL_NO_MAIN=1 sh -c '. "$1"; ui_init
            owner() { ls -ldn "$1" 2>/dev/null | awk "{print \$3}"; }
            safe_path "$2"' sh "$SRC/shellter-install.sh" "$W" 2>&1); then
        echo "skip: the temporary directory is not private - set TMPDIR to a directory only you can write:"
        printf '%s\n' "$msg" | sed -n 's/^ *error: //p'
        exit 77
    fi
}

# a local repository with one commit holding the installer under test
make_repo() {
    if ! { git -c init.defaultBranch=main init -q "$W/repo" &&
            cp "$SRC/shellter-install.sh" "$W/repo/" &&
            git -C "$W/repo" add shellter-install.sh &&
            git -C "$W/repo" -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false \
                commit -q -m test; }; then
        echo "FAIL $CASE: cannot make the test repository"; exit 1
    fi
}

run_install() {
    sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test "$@" > "$W/out" 2>&1 < /dev/null
    RC=$?
}

# an installed erssi $2 in prefix $1, set up for --update from the test repository
fake_prefix() {
    mkdir -p "$1/bin" "$1/share/irssi"
    printf '#!/bin/sh\necho "erssi %s (20250101 1200)"\n' "$2" > "$1/bin/erssi"
    chmod 755 "$1/bin/erssi"
    printf 'REPO=%s\n' "$W/repo" > "$1/share/irssi/shellter-install.conf"
}

# --update --check on an older install: fails in "Finding the release", as
# signatures cannot be checked without ssh-keygen in PATH
run_update_check() {
    sh "$SRC/shellter-install.sh" --update --check --prefix "$W/p" > "$W/out" 2>&1 < /dev/null
    RC=$?
}

# no meson or ninja in PATH, but a python3 that builds a venv with a pip
# that installs them; it logs whether the source lock is held when it runs.
# Another user acting while it runs: FAKE_PLANT_SRC=1 or git (they make the
# source directory, empty or a git checkout)
no_meson_python() {
    rm -f "$W/shim/meson" "$W/shim/ninja"
    # pip: one line per run with its arguments in $W/ran-pip; meson==V and
    # ninja==V become tools in its bin that log to $W/ran-venv. No wheel for
    # $FAKE_PIP_NO_WHEEL: refused when that package is binary-only
    # (--only-binary), otherwise "built from the sdist" ($W/pip-sdist)
    stand pip <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$TEST_W/ran-pip"
[ "$1" = install ] || exit 0
binary=""; prev=""
for a in "$@"; do
    case "$a" in --only-binary=*) binary="$binary,${a#--only-binary=}" ;; esac
    [ "$prev" = --only-binary ] && binary="$binary,$a"
    prev=$a
done
for a in "$@"; do
    case "$a" in
        meson==*|ninja==*)
            t=${a%%==*}
            [ "${FAKE_PIP_NO_WHEEL:-}" = "$t" ] || continue
            case "$binary," in
                *,:all:,*|*,"$t",*)
                    echo "ERROR: Could not find a version that satisfies the requirement $a (from versions: none)" >&2
                    exit 1 ;;
            esac
            echo "$t" >> "$TEST_W/pip-sdist" ;;
    esac
done
for a in "$@"; do
    case "$a" in
        meson==*|ninja==*)
            t=${a%%==*}
            # shellcheck disable=SC2016 # the stand-in tool
            { printf '#!/bin/sh\necho %s >> "$TEST_W/ran-venv"\n' "$t"; tail -n +2 "$TEST_W/stand/$t"; } > "${0%/*}/$t" &&
                chmod 755 "${0%/*}/$t" || exit 1 ;;
    esac
done
exit 0
EOF
    stand python3 <<'EOF'
#!/bin/sh
if [ -d "$HOME/.local/src/erssi.lock" ]; then echo "venv locked"; else echo "venv unlocked"; fi >> "$TEST_W/py.log"
[ "$1" = -m ] && [ "$2" = venv ] || exit 0
mkdir -p "$3/bin" && cp "$TEST_W/stand/pip" "$3/bin/"
case "${FAKE_PLANT_SRC:-0}" in
    1) mkdir -p "$HOME/.local/src/erssi" && echo "$HOME/.local/src/erssi 4242 -" >> "$TEST_W/lsfake" ;;
    git) git -c init.defaultBranch=main init -q "$HOME/.local/src/erssi" && echo "$HOME/.local/src/erssi 4242 -" >> "$TEST_W/lsfake" ;;
esac
exit 0
EOF
    use_tool python3
}

# Another user, as far as the installer can tell: $W/lsfake holds "path uid
# mode gid" lines ("-": as it is), and ls -ldn of such a path shows that
# owner (foreign: 4242), mode string or group. id -u is $FAKE_UID when set
# (the user's otherwise), id -un is "tester", id -gn
# $FAKE_GROUP_NAME (default "tester": a user private group), and getent
# group lists the members $FAKE_GROUP_MEMBERS (default none).
another_user() {
    : >> "$W/lsfake"
    stand ls <<'EOF'
#!/bin/sh
if [ "$1" = -ldn ]; then
    f=$(awk -v p="$2" '$1 == p {print $2, $3, ($4 == "" ? "-" : $4)}' "$TEST_W/lsfake" | tail -n 1)
    if [ -n "$f" ]; then
        set -- $f "$2"
        "$TEST_W/bin/ls" -ldn "$4" | awk -v u="$1" -v m="$2" -v g="$3" \
            '{ if (u != "-") $3 = u; if (m != "-") $1 = m; if (g != "-") $4 = g; print }'
        exit
    fi
fi
exec "$TEST_W/bin/ls" "$@"
EOF
    stand id <<'EOF'
#!/bin/sh
case "$*" in
    -u) if [ -n "${FAKE_UID:-}" ]; then echo "$FAKE_UID"; else exec "$TEST_W/bin/id" -u; fi ;;
    -un) echo tester ;;
    -gn) echo "${FAKE_GROUP_NAME:-tester}" ;;
    *) exec "$TEST_W/bin/id" "$@" ;;
esac
EOF
    use_tool ls id
}
# The setup guard once more, as a member of a shared group (another_user,
# FAKE_GROUP_NAME=staff). A directory above $W that the user's private group
# can write to (mode 770, e.g. made under umask 007) passes as private for
# the user, but not for a shared group: there the installer stops at that
# directory before it reaches the one a case is about. 1 in that case.
# Used by the case src-writable-named, which is skipped then.
private_for_shared_group() {
    # shellcheck disable=SC2016 # run by sh
    FAKE_GROUP_NAME=staff SHELLTER_INSTALL_NO_MAIN=1 sh -c '. "$1"; ui_init
            owner() { ls -ldn "$1" 2>/dev/null | awk "{print \$3}"; }
            safe_path "$2"' sh "$SRC/shellter-install.sh" "$W" > /dev/null 2>&1
}
foreign() { for f in "$@"; do printf '%s 4242 -\n' "$f" >> "$W/lsfake"; done; }
fake_owner() { printf '%s %s -\n' "$1" "$2" >> "$W/lsfake"; }
fake_mode() { printf '%s - %s\n' "$1" "$2" >> "$W/lsfake"; }
fake_group() { printf '%s - - %s\n' "$1" "$2" >> "$W/lsfake"; }
no_fakes() { : > "$W/lsfake"; }
# a venv in $1 whose meson and ninja log to $W/ran-venv when run
plant_venv() {
    mkdir -p "$1/bin"
    for t in meson ninja; do
        # shellcheck disable=SC2016
        { printf '#!/bin/sh\necho %s >> "$TEST_W/ran-venv"\n' "$t"; tail -n +2 "$W/stand/$t"; } > "$1/bin/$t"
        chmod 755 "$1/bin/$t"
    done
}

# uname says FreeBSD, no os-release, no openssl.pc; the base OpenSSL is
# looked for in $W/base
freebsd() {
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\necho "${FAKE_UNAME:-Linux}"\n' | stand uname
    use_tool uname
    FAKE_UNAME=FreeBSD FAKE_NO_OPENSSL_PC=1 SHELLTER_OS_RELEASE=$W/none SHELLTER_BASE_OPENSSL=$W/base
    export FAKE_UNAME FAKE_NO_OPENSSL_PC SHELLTER_OS_RELEASE SHELLTER_BASE_OPENSSL
}
# the four files of a base OpenSSL in $1; opensslv.h holds the line $2
base_openssl_files() {
    mkdir -p "$1/include/openssl" "$1/lib"
    : > "$1/include/openssl/ssl.h"
    printf '%s\n' '#ifndef OPENSSL_OPENSSLV_H' "$2" '#endif' > "$1/include/openssl/opensslv.h"
    : > "$1/lib/libssl.so"
    : > "$1/lib/libcrypto.so"
}

# a system that names its package manager: $1 the os-release ID (FreeBSD
# goes through freebsd())
os_release() {
    printf 'ID=%s\n' "$1" > "$W/os-release"
    SHELLTER_OS_RELEASE=$W/os-release; export SHELLTER_OS_RELEASE
}
# shellcheck disable=SC2317 # called through check
pip_runs() { [ "$(wc -l 2>/dev/null < "$W/ran-pip" || echo 0)" -eq "$1" ]; }
# shellcheck disable=SC2317 # some pip run installs $1 and no other build tool
pip_installs_only() {
    o=meson; [ "$1" = meson ] && o=ninja
    grep -q "$1==" "$W/ran-pip" && ! grep -q "$o==" "$W/ran-pip"
}
# shellcheck disable=SC2317 # every pip run with ninja takes it as a wheel only
ninja_binary_only() {
    grep -q 'ninja==' "$W/ran-pip" || return 1
    ! grep 'ninja==' "$W/ran-pip" | grep -vqE -- '--only-binary[= ](:all:|([^ ]*,)?ninja(,[^ ]*)?)( |$)'
}
# shellcheck disable=SC2317 # the venv's $1 ran
venv_ran() { grep -qx "$1" "$W/ran-venv" 2>/dev/null; }
# shellcheck disable=SC2317 # the venv's $1 did not run
venv_not_ran() { ! grep -qx "$1" "$W/ran-venv" 2>/dev/null; }

# shellcheck disable=SC2317 # called through check
ascii_only() { [ "$(LC_ALL=C tr -d '\000-\177' < "$1" | wc -c)" -eq 0 ]; }
# shellcheck disable=SC2317
no_grep() { ! grep -q "$1" "$2"; }
# shellcheck disable=SC2317
none_exist() { for f in "$@"; do [ ! -e "$f" ] && [ ! -L "$f" ] || return 1; done; }
# shellcheck disable=SC2317
no_link() { [ ! -e "$HOME/.local/bin/erssi" ] && [ ! -L "$HOME/.local/bin/erssi" ]; }

# ── release signatures ───────────────────────────────────────────────
# A throwaway release key ($W/key; $W/other is somebody else's): $INSTALLER
# is a copy of the installer under test whose RELEASE_SIGNER and the
# fingerprint signed_tag() checks are those of $W/key, and whose RELEASES
# are the lines of $W/releases. The signatures are real (ssh-keygen, which
# joins the tools in PATH); the test repository gets commits and tags.
release_key() {
    [ -n "$KEYGEN" ] || { echo "skip: no ssh-keygen"; exit 77; }
    # shellcheck disable=SC2046 # "2 47" -> $1 $2
    set -- $(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    { [ "${1:-0}" -gt 2 ] || { [ "${1:-0}" -eq 2 ] && [ "${2:-0}" -ge 34 ]; }; } ||
        { echo "skip: git before 2.34 cannot check SSH signatures"; exit 77; }
    ln -s "$KEYGEN" "$W/bin/ssh-keygen"
    if ! { ssh-keygen -q -t ed25519 -N '' -C release -f "$W/key" &&
            ssh-keygen -q -t ed25519 -N '' -C other -f "$W/other"; }; then
        echo "FAIL $CASE: ssh-keygen"; exit 1
    fi
    : > "$W/releases"
    patch_installer
}
# $INSTALLER from the installer under test, $W/key and $W/releases. The
# release key and its fingerprint are read from the installer and must be
# there once each, so that a new key there cannot leave this copy trusting
# the real one.
patch_installer() {
    old_key=$(sed -n "s/^RELEASE_SIGNER='[^ ]* namespaces=\"git\" \([^ ]* [^ ']*\).*/\1/p" "$SRC/shellter-install.sh")
    old_fp=$(sed -n "s/^RELEASE_FINGERPRINT='\(SHA256:[A-Za-z0-9+/]*\)'\$/\1/p" "$SRC/shellter-install.sh")
    new_key=$(cut -d' ' -f1,2 "$W/key.pub")
    new_fp=$(ssh-keygen -lf "$W/key.pub" | awk '{print $2}')
    if ! { [ -n "$old_key" ] && [ -n "$old_fp" ] &&
            [ "$(grep -cF "$old_key" "$SRC/shellter-install.sh")" = 1 ] &&
            [ "$(grep -cF "$old_fp" "$SRC/shellter-install.sh")" = 1 ]; }; then
        echo "FAIL $CASE: the release key or its fingerprint not found once in the installer"; exit 1
    fi
    INSTALLER=$W/inst.sh
    # literal replacements (a key holds / and +); RELEASES from the file
    awk -v ok="$old_key" -v nk="$new_key" -v of="$old_fp" -v nf="$new_fp" -v rel="$W/releases" '
        function swap(s, a, b,   i) {
            i = index(s, a)
            return i ? substr(s, 1, i - 1) b substr(s, i + length(a)) : s
        }
        skip && /^"$/ { skip = 0 }
        skip { next }
        /^RELEASES="$/ { print; while ((getline l < rel) > 0) print l; close(rel); skip = 1; next }
        { print swap(swap($0, ok, nk), of, nf) }' "$SRC/shellter-install.sh" > "$INSTALLER" || exit 1
    if ! { grep -qF "$new_key" "$INSTALLER" && grep -qF "$new_fp" "$INSTALLER" && ! grep -qF "$old_fp" "$INSTALLER"; }; then
        echo "FAIL $CASE: the throwaway key did not replace the release key"; exit 1
    fi
}
# release_commit VERSION: a commit in the test repository whose checkout
# installs as erssi VERSION (the stand-in ninja reads VERSION); prints it
release_commit() {
    printf '%s\n' "$1" > "$W/repo/VERSION"
    git -C "$W/repo" add VERSION &&
        git -C "$W/repo" -c user.name=test -c user.email=test@example.invalid -c commit.gpgsign=false \
            commit -q --allow-empty -m "erssi $1" && git -C "$W/repo" rev-parse HEAD
}
# rtag TAG COMMIT [KEY]: the annotated tag TAG on COMMIT, signed by $W/key
# (or KEY), replacing a tag of that name
rtag() {
    git -C "$W/repo" -c user.name=test -c user.email=test@example.invalid -c gpg.format=ssh \
        -c user.signingkey="${3:-$W/key}" tag -f -s -m "$1" "$1" "$2" > /dev/null
}
# the installer under test with ARGS, from the test repository
run_release() {
    sh "$INSTALLER" --repo "$W/repo" --no-test "$@" > "$W/out" 2>&1 < /dev/null
    RC=$?
}
# an installed erssi VERSION in $W/p, built from COMMIT [installed as REF]
release_prefix() {
    fake_prefix "$W/p" "$1"
    printf 'COMMIT=%s\n' "$2" >> "$W/p/share/irssi/shellter-install.conf"
    [ -z "${3:-}" ] || printf 'REF=%s\n' "$3" >> "$W/p/share/irssi/shellter-install.conf"
}
run_update() {
    sh "$INSTALLER" --update --no-test --prefix "$W/p" "$@" > "$W/out" 2>&1 < /dev/null
    RC=$?
}
# fn SCRIPT ARGS...: SCRIPT (with ARGS as "$@") in a shell that has the
# installer under test loaded, REPO the test repository; the output in
# $W/fn.out and $W/fn.err, the code in RC
fn() {
    # shellcheck disable=SC2016 # run by sh
    REPO=$W/repo SHELLTER_INSTALL_NO_MAIN=1 sh -c '. "$1"; ui_init; LOG=/dev/null; s=$2; shift 2; eval "$s"' \
        sh "$INSTALLER" "$@" > "$W/fn.out" 2> "$W/fn.err"
    RC=$?
}
# st TAG [NAME]: TAG fetched into a new repository and checked by
# signed_tag as the release NAME (default TAG)
st() {
    rm -rf "$W/st" && git init -q "$W/st" || exit 1
    # shellcheck disable=SC2016 # run by fn
    fn 'fetch_ref "$1" "$2" && signed_tag "$1" "$3"' "$W/st" "$1" "${2:-$1}"
}
# a gpg that accepts every signature and logs that it ran, as one with an
# OpenPGP key of the signer in the user's keyring would: never to be asked
gpg_accepts_all() {
    gfp=$(ssh-keygen -lf "$W/key.pub" | awk '{print $2}')
    {
        printf '#!/bin/sh\n'
        # shellcheck disable=SC2016 # the stand-in script
        printf 'echo ran >> "$TEST_W/ran-gpg"\n'
        printf 'echo "[GNUPG:] NEWSIG"\necho "[GNUPG:] GOODSIG 0123456789ABCDEF release"\n'
        printf 'echo "[GNUPG:] VALIDSIG 0123456789ABCDEF0123456789ABCDEF01234567 2026-10-01 0 4 0 22 8 00 0123456789ABCDEF0123456789ABCDEF01234567"\n'
        printf 'echo "[GNUPG:] TRUST_ULTIMATE 0 pgp"\n'
        printf "echo 'Good \"git\" signature for 136268860+y-o-o-z@users.noreply.github.com with ED25519 key %s'\n" "$gfp"
        printf 'exit 0\n'
    } | stand gpg
    use_tool gpg
    for t in gpg2 gpgsm; do cp "$W/stand/gpg" "$W/shim/$t"; done
}
# git whose fetch fails while FAKE_FETCH_FAIL=1 (the network gone after
# ls-remote, a server error)
fetch_fails() {
    # shellcheck disable=SC2016 # the stand-in script
    printf '#!/bin/sh\nif [ "${FAKE_FETCH_FAIL:-}" = 1 ]; then\n    for a in "$@"; do [ "$a" != fetch ] || { echo "fatal: unable to access the repository" >&2; exit 128; }; done\nfi\nexec "$TEST_W/bin/git" "$@"\n' | stand git
    use_tool git
}
# ptag TAG COMMIT: TAG as an annotated tag with an OpenPGP signature block
ptag() {
    o=$(printf 'object %s\ntype commit\ntag %s\ntagger test <test@example.invalid> 1790000000 +0000\n\n%s\n-----BEGIN PGP SIGNATURE-----\n\niHUEABYKAB0WIQQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\n=AAAA\n-----END PGP SIGNATURE-----\n' \
        "$2" "$1" "$1" | git -C "$W/repo" hash-object -t tag -w --stdin) &&
        git -C "$W/repo" update-ref "refs/tags/$1" "$o"
}

# ── binary packages ──────────────────────────────────────────────────
# The release key of release_key, the real tar, xz, sha256sum, find and
# cmp, and stand-ins: uname says Linux x86_64 ($FAKE_UNAME, $FAKE_UNAME_M),
# freebsd-version $FAKE_FREEBSD_VERSION, os-release Debian 13, perl tells
# the version $FAKE_PERL_V (5.40.1) and runs everything else for real,
# and ldd reports the libraries in $FAKE_LDD_MISSING as not found.
binary_tools() {
    release_key
    for t in tar xz sha256sum find cmp perl; do
        p=$(printf '%s\n' "$BIN_TOOLS" | tr ' ' '\n' | sed -n "s|^$t=||p")
        [ -n "$p" ] || { echo "skip: no $t"; exit 77; }
        [ "$t" = perl ] || ln -s "$p" "$W/bin/$t"
    done
    REAL_PERL=$(printf '%s\n' "$BIN_TOOLS" | tr ' ' '\n' | sed -n 's|^perl=||p')
    # shellcheck disable=SC2016 # stand-in scripts
    {
        printf '#!/bin/sh\n'
        printf 'if [ "$1" = -e ] && [ "$2" = %s ]; then printf "v%%s" "${FAKE_PERL_V:-5.40.1}"; exit 0; fi\n' "'print \$^V'"
        printf 'exec %s "$@"\n' "$REAL_PERL"
    } | stand perl
    # shellcheck disable=SC2016
    printf '#!/bin/sh\nfor l in ${FAKE_LDD_MISSING:-}; do printf "\\t%%s => not found\\n" "$l"; done\nprintf "\\tlibc.so.6 => /lib/libc.so.6\\n"\n' | stand ldd
    # shellcheck disable=SC2016
    printf '#!/bin/sh\ncase "${1:-}" in\n    -m) echo "${FAKE_UNAME_M:-x86_64}" ;;\n    *) echo "${FAKE_UNAME:-Linux}" ;;\nesac\n' | stand uname
    # shellcheck disable=SC2016
    printf '#!/bin/sh\necho "${FAKE_FREEBSD_VERSION:-14.3-RELEASE-p2}"\n' | stand freebsd-version
    use_tool perl ldd uname freebsd-version
    printf 'ID=debian\nVERSION_ID="13"\n' > "$W/os-release"
    SHELLTER_OS_RELEASE=$W/os-release; export SHELLTER_OS_RELEASE
    P=$HOME/.local/opt/erssi
    CONF=$P/share/irssi/shellter-install.conf
}
# make_pkg VERSION COMMIT [PLATFORM]: the package erssi-VERSION-PLATFORM.tar.xz
# of shellter-vVERSION in $W/rel (the --binary-base), its erssi a script
# that prints VERSION ($MF_RUNS_AS) and finds its prefix where it is (the
# command $MF_PREFIX), its MANIFEST with the fields given
# (MF_VERSION, MF_TAG, MF_COMMIT, MF_PLATFORM, MF_PERL override them);
# SHA256SUMS lists every package of that release, unsigned (sign_sums)
make_pkg() {
    pv=$1 pc=$2 pp=${3:-debian13-amd64}
    top="erssi-$pv-$pp" pd="$W/pkg/$pv-$pp" pr="$W/rel/shellter-v$pv"
    rm -rf "$pd" && mkdir -p "$pd/$top/bin" "$pd/$top/lib/irssi/modules" "$pd/$top/share/irssi" "$pr" || exit 1
    # shellcheck disable=SC2016 # the stand-in erssi
    pfx=${MF_PREFIX:-'cd "$(dirname "$0")/.." && pwd -P'}
    # shellcheck disable=SC2016
    printf '#!/bin/sh\nif [ "$1" = --print-prefix ]; then %s; exit; fi\necho "erssi %s (20261018 1200)"\n' \
        "$pfx" "${MF_RUNS_AS:-$pv}" > "$pd/$top/bin/erssi"
    chmod 755 "$pd/$top/bin/erssi"
    : > "$pd/$top/lib/irssi/modules/libfe_web.so"
    cp "$INSTALLER" "$pd/$top/share/irssi/shellter-install.sh"
    {
        echo "built=2026-10-18T12:00:00Z"
        echo "commit=${MF_COMMIT-$pc}"
        echo "libc=glibc 2.41"
        echo "needed=libc.so.6 libglib-2.0.so.0 libperl.so.5.40"
        echo "perl_version=${MF_PERL-5.40.1}"
        echo "platform=${MF_PLATFORM-$pp}"
        echo "tag=${MF_TAG-shellter-v$pv}"
        echo "version=${MF_VERSION-$pv}"
    } > "$pd/$top/MANIFEST"
    tar -C "$pd" -cJf "$pr/$top.tar.xz" "$top" || exit 1
    ( cd "$pr" && sha256sum erssi-*.tar.xz ) > "$pr/SHA256SUMS" || exit 1
}
# sign_sums VERSION [KEY [NAMESPACE]]: SHA256SUMS.sig of shellter-vVERSION
sign_sums() {
    rm -f "$W/rel/shellter-v$1/SHA256SUMS.sig"
    ssh-keygen -q -Y sign -n "${3:-erssi-release}" -f "${2:-$W/key}" "$W/rel/shellter-v$1/SHA256SUMS" 2>/dev/null ||
        { echo "FAIL $CASE: ssh-keygen -Y sign"; exit 1; }
}
# repack VERSION [TAR ARGS...]: the package of make_pkg VERSION (Debian 13)
# tarred again from $W/pkg, with TAR ARGS, listed and signed again
repack() {
    rv=$1; shift
    rr="$W/rel/shellter-v$rv"
    tar -C "$W/pkg/$rv-debian13-amd64" -cJf "$rr/erssi-$rv-debian13-amd64.tar.xz" "$@" "erssi-$rv-debian13-amd64" 2>/dev/null &&
        ( cd "$rr" && sha256sum erssi-*.tar.xz ) > "$rr/SHA256SUMS" && sign_sums "$rv"
}
# good_pkg VERSION COMMIT [PLATFORM]: make_pkg, signed as a release is
good_pkg() { make_pkg "$@" && sign_sums "$1"; }
# record VERSION COMMIT...: the releases this copy of the installer records
record() {
    : > "$W/releases"
    while [ $# -ge 2 ]; do printf 'shellter-v%s %s signed\n' "$1" "$2" >> "$W/releases"; shift 2; done
    patch_installer
}
# the installer under test with ARGS, packages from $W/rel
run_bin() {
    sh "$INSTALLER" --repo "$W/repo" --binary-base "$W/rel" --no-test "$@" > "$W/out" 2>&1 < /dev/null
    RC=$?
}
# a fake commit SHA for the release VERSION (binary installs need no source)
fsha() { printf '%040d' "$(echo "$1" | tr -d .)"; }
# shellcheck disable=SC2317 # called through check
current_is() { [ "$(readlink "$P/current")" = "releases/$1" ] && [ "$("$P/bin/erssi" --version)" = "erssi $1 (20261018 1200)" ]; }
# shellcheck disable=SC2317,SC2012 # the releases kept in $P/releases, sorted
releases_are() { [ "$(cd "$P/releases" && ls -A | sort | tr '\n' ' ')" = "$* " ]; }
# shellcheck disable=SC2317 # nothing changed: 2026.10.3 in use, alone
unchanged() {
    current_is 2026.10.3 && releases_are 2026.10.3 && none_exist "$P/.install.lock" &&
        grep -qx VERSION=2026.10.3 "$CONF"
}

# the ten spinner frames as the installer draws them (sourced only when it
# can be loaded without running main)
spinner() {
    # shellcheck disable=SC2016 # the literal line
    grep -q '^\[ -n "${SHELLTER_INSTALL_NO_MAIN:-}" \] || main "\$@"$' "$SRC/shellter-install.sh" || {
        echo "cannot be sourced"; return; }
    SHELLTER_INSTALL_NO_MAIN=1 sh -c '. "$1"; ui_init; i=0
        while [ "$i" -lt 10 ]; do spinner_frame "$i"; i=$((i + 1)); done' sh "$SRC/shellter-install.sh"
}

setup
case "$CASE" in
ascii)
    FAKE_CHARMAP=ANSI_X3.4-1968; export FAKE_CHARMAP
    fake_prefix "$W/p" 1.3.9
    run_update_check
    check "update --check fails" [ "$RC" = 1 ]
    check "fails in Finding the release" grep -q 'Finding the release.*failed' "$W/out"
    check "output is ASCII only" ascii_only "$W/out"
    check "header with an ASCII separator" grep -qF 'erssi Shellter Edition - update' "$W/out"
    check "names the current installer, for this prefix" \
        grep -qF "curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh -s -- --prefix $W/p" "$W/out"
    check "... not a release of its own" no_grep '[-]-ref shellter-v' "$W/out"
    check "ASCII spinner" [ "$(spinner)" = '|/-\|/-\|/' ]
    # no locale command and no locale variables: ASCII as well
    rm -f "$W/shim/locale"
    run_update_check
    check "no locale command: ASCII only" ascii_only "$W/out"
    check "no locale command: failed" grep -q 'Finding the release.*failed' "$W/out"
    use_tool locale
    # a whole install, not on a terminal
    run_install --clean
    check "install: done" [ "$RC" = 0 ]
    check "install: build directory removed" grep -q '/.local/opt/erssi - build directory removed' "$W/out"
    check "install: steps marked ok" grep -q 'Choosing the release *ok main' "$W/out"
    check "install: ASCII separators" grep -q 'meson 1.12.1 - ninja 1.13.2 - glib 2.80.0 - openssl 3.0.15' "$W/out"
    check "install: skipped tests" grep -q 'Testing *- skipped' "$W/out"
    check "install: ASCII only" ascii_only "$W/out"
    # a terminal: spinner, colours and step lines, still ASCII only
    if [ -n "$SCRIPT" ] && "$SCRIPT" --version 2>/dev/null | grep -q util-linux; then
        FAKE_SLOW=1 TERM=xterm SHELL=$W/bin/sh "$SCRIPT" -qec \
            "sh '$SRC/shellter-install.sh' --repo '$W/repo' --ref main --no-test" "$W/tty" > /dev/null 2>&1 < /dev/null
        RC=$?
        check "on a terminal: installed" [ "$RC" = 0 ]
        check "on a terminal: the spinner ran" grep -q "Building.*$(printf '|\033')" "$W/tty"
        check "on a terminal: ASCII only" ascii_only "$W/tty"
    else
        pass "skip the terminal part: no util-linux script"
    fi
    ;;
utf8)
    FAKE_CHARMAP=UTF-8; export FAKE_CHARMAP
    fake_prefix "$W/p" 1.3.9
    run_update_check
    check "update --check fails" [ "$RC" = 1 ]
    check "header with a middle dot" grep -q 'erssi Shellter Edition.*·' "$W/out"
    check "failed step marked with a cross" grep -q 'Finding the release.*✗' "$W/out"
    check "braille spinner" [ "$(spinner)" = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏' ]
    # no locale command: the locale variables decide
    rm -f "$W/shim/locale"
    unset FAKE_CHARMAP
    LANG=C.UTF-8; export LANG
    run_update_check
    check "LANG=C.UTF-8: header with a middle dot" grep -q 'erssi Shellter Edition.*·' "$W/out"
    check "LANG=C.UTF-8: cross" grep -q 'Finding the release.*✗' "$W/out"
    ;;
link-home)
    # the default prefix in the home: ~/.local/bin/erssi links to it
    run_install
    check "installed" [ "$RC" = 0 ]
    check ".local/bin/erssi is a link" [ -L "$HOME/.local/bin/erssi" ]
    check "... to the installed erssi" [ "$(readlink "$HOME/.local/bin/erssi")" = "$HOME/.local/opt/erssi/bin/erssi" ]
    check "start with the link" grep -q 'start  *~/.local/bin/erssi ' "$W/out"
    check "PATH hint for ~/.local/bin" grep -q 'path  *~/.local/bin is not in PATH' "$W/out"
    # a home reached through a link (FreeBSD: /home -> /usr/home) and the
    # prefix given by its real path: still in the home
    ln -s "$W/home" "$W/homelink"
    rm -f "$W/home/.local/bin/erssi"
    HOME=$W/homelink
    run_install --prefix "$W/home/opt2/erssi"
    HOME=$W/home
    check "home through a link: installed" [ "$RC" = 0 ]
    check "home through a link: linked" [ "$(readlink "$W/home/.local/bin/erssi")" = "$W/home/opt2/erssi/bin/erssi" ]
    rm -f "$W/home/homelink"
    # a link to a directory: replaced by the link, nothing made inside
    mkdir -p "$W/somedir"
    rm -f "$HOME/.local/bin/erssi"
    ln -s "$W/somedir" "$HOME/.local/bin/erssi"
    run_install
    check "link to a directory: replaced" [ "$(readlink "$HOME/.local/bin/erssi")" = "$HOME/.local/opt/erssi/bin/erssi" ]
    check "link to a directory: nothing inside it" [ ! -e "$W/somedir/erssi" ]
    # a file of the user's own there: kept, with a warning
    rm -f "$HOME/.local/bin/erssi"
    echo "my own erssi" > "$HOME/.local/bin/erssi"
    run_install
    check "own file: installed" [ "$RC" = 0 ]
    check "own file: kept" [ "$(cat "$HOME/.local/bin/erssi")" = "my own erssi" ]
    check "own file: warning" grep -q '! ~/.local/bin/erssi exists and is not a link - kept' "$W/out"
    check "own file: start names the installed erssi" grep -q 'start  *~/.local/opt/erssi/bin/erssi ' "$W/out"
    # a directory there: kept
    rm -f "$HOME/.local/bin/erssi"
    mkdir -p "$HOME/.local/bin/erssi"
    run_install
    check "directory: installed" [ "$RC" = 0 ]
    check "directory: kept" [ -d "$HOME/.local/bin/erssi" ]
    check "directory: nothing inside" none_exist "$HOME/.local/bin/erssi/erssi"
    check "directory: warning" grep -q '! ~/.local/bin/erssi exists and is not a link - kept' "$W/out"
    ;;
link-system-prefix)
    # a prefix outside the home: no link in the home
    run_install --prefix "$W/sys/erssi"
    check "installed" [ "$RC" = 0 ]
    check "the binary" [ -x "$W/sys/erssi/bin/erssi" ]
    check "no ~/.local/bin/erssi" no_link
    check "start names the installed erssi" grep -qF "start     $W/sys/erssi/bin/erssi " "$W/out"
    check "PATH hint for the prefix" grep -qF "path      $W/sys/erssi/bin is not in PATH" "$W/out"
    # the prefix in PATH: started as erssi, no hint
    PATH=$W/sys/erssi/bin:$PATH
    run_install --prefix "$W/sys/erssi"
    PATH=$W/shim:$W/bin
    check "in PATH: start erssi" grep -q 'start  *erssi ' "$W/out"
    check "in PATH: no PATH hint" no_grep "not in PATH" "$W/out"
    check "in PATH: still no link" no_link
    ;;
relative-prefix)
    # a relative --prefix: refused before anything is made or downloaded
    # (meson needs an absolute one, and the link would point nowhere)
    mkdir -p "$W/cwd"
    (cd "$W/cwd" && sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test --prefix opt/erssi) > "$W/out" 2>&1 < /dev/null
    RC=$?
    check "refused" [ "$RC" = 2 ]
    check "... named" grep -qF -- '--prefix needs an absolute path' "$W/out"
    check "... nothing made" none_exist "$W/cwd/opt" "$HOME/.local/src/erssi" "$HOME/.local/src/erssi.lock"
    (cd "$W/cwd" && sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test --prefix ./opt) > "$W/out" 2>&1 < /dev/null
    check "./ refused too" [ $? = 2 ]
    run_install --prefix "$W/abs/erssi"
    check "an absolute prefix: installed" [ "$RC" = 0 ]
    ;;
link-root)
    # root (id -u 0, a home owned by root): no link, whatever the prefix
    # shellcheck disable=SC2016 # stand-in scripts
    printf '#!/bin/sh\n[ "$1" = -u ] && { echo 0; exit 0; }\nexec "$TEST_W/bin/id" "$@"\n' | stand id
    # shellcheck disable=SC2016
    printf '#!/bin/sh\nif [ "$1" = -ldn ]; then\n    "$TEST_W/bin/ls" -ldn "$2" | awk %s\nelse\n    exec "$TEST_W/bin/ls" "$@"\nfi\n' \
        "'{\$3 = \"0\"; print}'" | stand ls
    use_tool id ls
    run_install
    check "installed as root" [ "$RC" = 0 ]
    check "the binary" [ -x "$HOME/.local/opt/erssi/bin/erssi" ]
    check "no ~/.local/bin/erssi" no_link
    check "start names the installed erssi" grep -q 'start  *~/.local/opt/erssi/bin/erssi ' "$W/out"
    ;;
link-same-dir)
    # --prefix ~/.local: the binary is ~/.local/bin/erssi itself
    run_install --prefix "$HOME/.local"
    check "installed" [ "$RC" = 0 ]
    check ".local/bin/erssi is a file" [ -f "$HOME/.local/bin/erssi" ]
    check "... not a link" [ ! -L "$HOME/.local/bin/erssi" ]
    check "... and it runs" [ "$("$HOME/.local/bin/erssi" --version 2>&1)" = "erssi 9.9.9 (20261010 1200)" ]
    check "summary names the version" grep -q 'erssi Shellter Edition 9.9.9 is installed' "$W/out"
    check "no warning about the binary" no_grep 'is not a link' "$W/out"
    # ~/.local/bin is a link to the prefix's bin: the same directory too
    rm -rf "$W/home/.local/bin"
    mkdir -p "$W/home/apps/erssi/bin"
    ln -s "$W/home/apps/erssi/bin" "$W/home/.local/bin"
    run_install --prefix "$HOME/apps/erssi"
    check "linked bin dir: installed" [ "$RC" = 0 ]
    check "linked bin dir: the binary is kept" [ ! -L "$HOME/apps/erssi/bin/erssi" ]
    check "linked bin dir: it runs" [ "$("$HOME/apps/erssi/bin/erssi" --version 2>&1)" = "erssi 9.9.9 (20261010 1200)" ]
    # an ln -sf that removes the target first (GNU ln refuses "the same
    # file"; others replace the binary with a link to itself)
    # shellcheck disable=SC2016
    printf '#!/bin/sh\n[ "$1" = -sf ] && "$TEST_W/bin/rm" -f "$3"\nexec "$TEST_W/bin/ln" "$@"\n' | stand ln
    use_tool ln
    rm -f "$W/home/.local/bin"
    run_install --prefix "$HOME/.local"
    check "ln replacing the target: installed" [ "$RC" = 0 ]
    check "ln replacing the target: not a link" [ ! -L "$HOME/.local/bin/erssi" ]
    check "ln replacing the target: it runs" [ "$("$HOME/.local/bin/erssi" --version 2>&1)" = "erssi 9.9.9 (20261010 1200)" ]
    ;;
lock-before-venv)
    # another installer holds the source lock: nothing is built, not even
    # the meson/ninja venv, and its lock and log are left alone
    no_meson_python
    mkdir -p "$HOME/.local/src/erssi.lock" "$HOME/.local/state/erssi"
    echo "the other installer's log" > "$HOME/.local/state/erssi/install.log"
    run_install
    check "refused" [ "$RC" = 1 ]
    check "names the other installer" grep -q 'another installer is using' "$W/out"
    check "no venv built" [ ! -e "$W/py.log" ]
    check "the other installer's lock is kept" [ -d "$HOME/.local/src/erssi.lock" ]
    check "the other installer's log is kept" \
        [ "$(cat "$HOME/.local/state/erssi/install.log")" = "the other installer's log" ]
    ;;
lock-held-during-venv)
    # the venv is built while this installer holds the lock
    no_meson_python
    run_install
    check "installed" [ "$RC" = 0 ]
    check "venv built under the lock" [ "$(head -n 1 "$W/py.log" 2>/dev/null)" = "venv locked" ]
    check "meson from the venv" grep -q 'meson 1.12.1' "$W/out"
    check "the lock is gone afterwards" [ ! -e "$HOME/.local/src/erssi.lock" ]
    ;;
freebsd-base-openssl)
    # FreeBSD: OpenSSL of the base system, no openssl.pc
    freebsd
    base_openssl_files "$W/base" '# define OPENSSL_VERSION_TEXT "OpenSSL 3.0.15 3 Sep 2024"'
    run_install
    check "installed" [ "$RC" = 0 ]
    check "base OpenSSL named" grep -qF 'openssl 3.0.15 (base system)' "$W/out"
    check "no package asked for" no_grep 'pkg install' "$W/out"
    # FreeBSD 13: OpenSSL 1.1.1, two spaces before the text
    base_openssl_files "$W/base" '#  define OPENSSL_VERSION_TEXT  "OpenSSL 1.1.1w-freebsd  11 Sep 2023"'
    run_install
    check "1.1.1: installed" [ "$RC" = 0 ]
    check "1.1.1: version" grep -qF 'openssl 1.1.1w-freebsd (base system)' "$W/out"
    # a header without the version text
    base_openssl_files "$W/base" '/* nothing */'
    run_install
    check "no version text: installed" [ "$RC" = 0 ]
    check "no version text: unknown" grep -qF 'openssl unknown (base system)' "$W/out"
    # no pkg-config at all: pkgconf and glib needed, but not openssl
    rm -f "$W/shim/pkg-config"
    run_install
    check "no pkg-config: refused" [ "$RC" = 1 ]
    check "no pkg-config: pkgconf and glib only" grep -qF 'pkg install pkgconf glib   (as root)' "$W/out"
    ;;
freebsd-no-openssl)
    # FreeBSD without the base OpenSSL files: the package is still needed
    freebsd
    mkdir -p "$W/base"
    run_install
    check "refused" [ "$RC" = 1 ]
    check "pkg install openssl" grep -qF 'pkg install openssl   (as root)' "$W/out"
    # one of the four files missing
    base_openssl_files "$W/base" '# define OPENSSL_VERSION_TEXT "OpenSSL 3.0.15 3 Sep 2024"'
    rm -f "$W/base/lib/libcrypto.so"
    run_install
    check "no libcrypto.so: refused" [ "$RC" = 1 ]
    check "no libcrypto.so: pkg install openssl" grep -qF 'pkg install openssl   (as root)' "$W/out"
    # no pkg-config and no base OpenSSL: pkgconf, glib and openssl
    rm -f "$W/shim/pkg-config"
    run_install
    check "no pkg-config: pkgconf glib openssl" grep -qF 'pkg install pkgconf glib openssl   (as root)' "$W/out"
    use_tool pkg-config
    # Linux with the same files: the fallback is FreeBSD only
    base_openssl_files "$W/base" '# define OPENSSL_VERSION_TEXT "OpenSSL 3.0.15 3 Sep 2024"'
    FAKE_UNAME=Linux
    printf 'ID=debian\n' > "$W/os-release"
    SHELLTER_OS_RELEASE=$W/os-release
    run_install
    check "Linux: refused" [ "$RC" = 1 ]
    check "Linux: sudo apt install libssl-dev" grep -qF 'sudo apt install libssl-dev' "$W/out"
    ;;
src-foreign)
    # a venv, a source directory or a directory above them that belongs to
    # another user: refused before anything in it runs
    no_meson_python
    another_user
    V=$HOME/.local/src/erssi.venv
    plant_venv "$V"
    foreign "$V"
    run_install
    check "their venv: refused" [ "$RC" = 1 ]
    check "their venv: named" grep -qF "$V belongs to another user" "$W/out"
    check "their venv: nothing run" none_exist "$W/ran-venv" "$W/py.log" "$W/ran-pip"
    check "their venv: lock released" none_exist "$HOME/.local/src/erssi.lock"
    # also when meson and ninja are in PATH (the venv is not needed)
    use_tool meson ninja
    run_install
    rm -f "$W/shim/meson" "$W/shim/ninja"
    check "their venv, meson in PATH: refused" [ "$RC" = 1 ]
    # a link where the venv goes, even to a venv of ours (its mode as BSD
    # shows it, so only the link itself gives it away)
    no_fakes
    mv "$V" "$W/myvenv"
    ln -s "$W/myvenv" "$V"
    fake_mode "$V" lrwxr-xr-x
    run_install
    no_fakes
    check "venv is a link: refused" [ "$RC" = 1 ]
    check "venv is a link: named" grep -qF "$V is a symbolic link" "$W/out"
    check "venv is a link: nothing run" none_exist "$W/ran-venv"
    rm -f "$V"
    mv "$W/myvenv" "$V"
    # the same venv, ours: reused
    no_fakes
    run_install
    check "our venv: installed" [ "$RC" = 0 ]
    check "our venv: reused" [ -s "$W/ran-venv" ]
    check "our venv: no venv built" none_exist "$W/py.log"
    # an empty source directory of theirs
    rm -rf "$HOME/.local/src" "$HOME/.local/opt" "$W/ran-venv"
    mkdir -p "$HOME/.local/src/erssi"
    foreign "$HOME/.local/src/erssi"
    run_install
    check "their source dir: refused" [ "$RC" = 1 ]
    check "their source dir: named" grep -qF "$HOME/.local/src/erssi belongs to another user" "$W/out"
    check "their source dir: nothing checked out, no venv" none_exist "$HOME/.local/src/erssi/.git" "$W/py.log"
    # their venv appears after the lock was taken (during step 1)
    no_fakes
    rm -rf "$HOME/.local/src" "$W/ran-venv" "$W/py.log"
    plant_venv "$W/planted"
    FAKE_PLANT_VENV=$V; export FAKE_PLANT_VENV
    run_install
    unset FAKE_PLANT_VENV
    check "venv made meanwhile: refused" [ "$RC" = 1 ]
    check "venv made meanwhile: named" grep -qF "$V belongs to another user" "$W/out"
    check "venv made meanwhile: nothing run" none_exist "$W/ran-venv" "$W/ran-pip"
    # their source dir appears while the venv is built
    no_fakes
    rm -rf "$HOME/.local/src"
    FAKE_PLANT_SRC=1; export FAKE_PLANT_SRC
    run_install
    unset FAKE_PLANT_SRC
    check "source dir made meanwhile: refused" [ "$RC" = 1 ]
    check "source dir made meanwhile: nothing checked out" none_exist "$HOME/.local/src/erssi/.git"
    # ... as a git checkout of theirs
    no_fakes
    rm -rf "$HOME/.local/src"
    FAKE_PLANT_SRC=git; export FAKE_PLANT_SRC
    run_install
    unset FAKE_PLANT_SRC
    check "checkout made meanwhile: refused" [ "$RC" = 1 ]
    check "checkout made meanwhile: named" grep -qF "$HOME/.local/src/erssi belongs to another user" "$W/out"
    check "checkout made meanwhile: not fetched into" none_exist "$HOME/.local/src/erssi/.git/FETCH_HEAD"
    # made by them just before the installer's own mkdir of it
    no_fakes
    rm -rf "$HOME/.local/src"
    stand mkdir <<'EOF'
#!/bin/sh
for last in "$@"; do :; done
if [ "$last" = "${FAKE_RACE_DIR:-}" ]; then
    "$TEST_W/bin/mkdir" -p "$last" && echo "$last 4242 -" >> "$TEST_W/lsfake"
fi
exec "$TEST_W/bin/mkdir" "$@"
EOF
    use_tool mkdir
    FAKE_RACE_DIR=$HOME/.local/src/erssi; export FAKE_RACE_DIR
    run_install
    unset FAKE_RACE_DIR
    rm -f "$W/shim/mkdir"
    check "source dir made just before mkdir: refused" [ "$RC" = 1 ]
    check "source dir made just before mkdir: nothing checked out" none_exist "$HOME/.local/src/erssi/.git"
    # a parent directory of theirs
    no_fakes
    mkdir -p "$W/other"
    foreign "$W/other"
    run_install --src "$W/other/erssi"
    check "their parent dir: refused" [ "$RC" = 1 ]
    check "their parent dir: named" grep -qF "$W/other belongs to another user" "$W/out"
    check "their parent dir: nothing made" none_exist "$W/other/erssi" "$W/other/erssi.lock"
    # a directory of theirs further up: they could rename ours away
    no_fakes
    mkdir -p "$W/theirs/mine"
    foreign "$W/theirs"
    run_install --src "$W/theirs/mine/erssi"
    check "theirs further up: refused" [ "$RC" = 1 ]
    check "theirs further up: named" grep -qF "$W/theirs belongs to another user" "$W/out"
    check "theirs further up: nothing made" none_exist "$W/theirs/mine/erssi" "$W/theirs/mine/erssi.lock"
    ;;
src-writable)
    # a venv or source directory other users can write to: refused; group
    # write by the user's own private group (no other members, no ACL):
    # allowed. Which directory the shared-group refusals name: the case
    # src-writable-named (a directory above $W that the user's group can
    # write to is refused first)
    no_meson_python
    another_user
    V=$HOME/.local/src/erssi.venv
    plant_venv "$V"
    chmod g+w "$V"
    FAKE_GROUP_NAME=staff; export FAKE_GROUP_NAME
    run_install
    unset FAKE_GROUP_NAME
    check "group-writable venv, shared group: refused" [ "$RC" = 1 ]
    check "... nothing run" none_exist "$W/ran-venv" "$W/py.log"
    run_install
    check "group-writable venv, private group: installed" [ "$RC" = 0 ]
    check "... reused" [ -s "$W/ran-venv" ]
    rm -f "$W/ran-venv"
    FAKE_GROUP_MEMBERS=tester,mallory; export FAKE_GROUP_MEMBERS
    run_install
    check "private group with another member: refused" [ "$RC" = 1 ]
    check "... nothing run" none_exist "$W/ran-venv"
    FAKE_GROUP_MEMBERS=tester
    run_install
    unset FAKE_GROUP_MEMBERS
    check "private group listing only the user: installed" [ "$RC" = 0 ]
    rm -f "$W/ran-venv"
    fake_mode "$V" drwxrwxr-x+
    run_install
    no_fakes
    check "private group with an ACL: refused" [ "$RC" = 1 ]
    check "... nothing run" none_exist "$W/ran-venv"
    rm -f "$W/shim/getent"
    run_install
    use_tool getent
    check "private group, no getent: refused" [ "$RC" = 1 ]
    fake_group "$V" 4243
    run_install
    no_fakes
    check "group write by another group of the same name: refused" [ "$RC" = 1 ]
    check "... nothing run" none_exist "$W/ran-venv"
    chmod g-w,o+w "$V"
    run_install
    check "other-writable venv: refused" [ "$RC" = 1 ]
    check "... nothing run" none_exist "$W/ran-venv"
    chmod o-w "$V"
    chmod o+w "$HOME/.local/src/erssi"
    run_install
    check "other-writable source dir: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$HOME/.local/src/erssi can be changed by other users" "$W/out"
    # umask 007 with a private group (directories 770): installs
    rm -rf "$HOME/.local/src" "$HOME/.local/opt"
    umask 007
    run_install
    check "umask 007, private group: installed" [ "$RC" = 0 ]
    rm -rf "$HOME/.local/src"
    FAKE_GROUP_NAME=staff; export FAKE_GROUP_NAME
    run_install
    unset FAKE_GROUP_NAME
    umask 022
    check "umask 007, shared group: refused" [ "$RC" = 1 ]
    ;;
src-writable-named)
    # the two shared-group refusals of src-writable name the directory
    # they are about; skipped where a directory above $W is writable by the
    # user's group, as a shared group is refused there first
    no_meson_python
    another_user
    if ! private_for_shared_group; then
        echo "skip: a directory above $W is writable by your group - a shared group is refused there first (it runs with a TMPDIR whose directories above are not group-writable)"
        exit 77
    fi
    V=$HOME/.local/src/erssi.venv
    plant_venv "$V"
    chmod g+w "$V"
    FAKE_GROUP_NAME=staff; export FAKE_GROUP_NAME
    run_install
    check "group-writable venv, shared group: refused" [ "$RC" = 1 ]
    check "... the venv named" grep -qF "$V can be changed by other users" "$W/out"
    rm -rf "$HOME/.local/src"
    umask 007
    run_install
    umask 022
    unset FAKE_GROUP_NAME
    check "umask 007, shared group: refused" [ "$RC" = 1 ]
    check "... ~/.local/src named" grep -qF "$HOME/.local/src is writable by other users - choose a --src under a directory only you can write" "$W/out"
    ;;
src-shared-parent)
    # a directory others can write to, even sticky like /tmp: refused
    another_user
    mkdir -p "$W/shared"
    chmod 1777 "$W/shared"
    run_install --src "$W/shared/erssi"
    check "sticky parent: refused" [ "$RC" = 1 ]
    check "sticky parent: named" grep -qF "$W/shared is writable by other users - choose a --src under a directory only you can write" "$W/out"
    check "sticky parent: nothing made" none_exist "$W/shared/erssi" "$W/shared/erssi.lock"
    fake_owner "$W/shared" 0
    run_install --src "$W/shared/erssi"
    no_fakes
    check "sticky parent of root's (/tmp): refused" [ "$RC" = 1 ]
    check "... nothing made" none_exist "$W/shared/erssi" "$W/shared/erssi.lock"
    chmod 0777 "$W/shared"
    run_install --src "$W/shared/erssi"
    check "open parent: refused" [ "$RC" = 1 ]
    # further up: open, or sticky but not root's - refused; sticky and
    # root's (like /tmp) - allowed
    mkdir -p "$W/open/mine"
    chmod 0777 "$W/open"
    run_install --src "$W/open/mine/erssi"
    check "open further up: refused" [ "$RC" = 1 ]
    check "open further up: named" grep -qF "$W/open is writable by other users" "$W/out"
    check "open further up: nothing made" none_exist "$W/open/mine/erssi" "$W/open/mine/erssi.lock"
    fake_owner "$W/open" 0
    run_install --src "$W/open/mine/erssi"
    no_fakes
    check "open further up, root's, not sticky: refused" [ "$RC" = 1 ]
    chmod 1777 "$W/open"
    run_install --src "$W/open/mine/erssi"
    check "sticky further up, not root's: refused" [ "$RC" = 1 ]
    fake_owner "$W/open" 0
    run_install --src "$W/open/mine/erssi"
    no_fakes
    chmod 0755 "$W/open"
    check "sticky further up, root's: installed" [ "$RC" = 0 ]
    check "sticky further up, root's: checked out" [ -d "$W/open/mine/erssi/.git" ]
    # links resolved once: a parent reached through a link
    mkdir -p "$W/real"
    ln -s "$W/real" "$W/via"
    run_install --src "$W/via/erssi"
    check "linked parent: installed" [ "$RC" = 0 ]
    check "linked parent: checked out in the real directory" [ -d "$W/real/erssi/.git" ]
    check "linked parent: the real path kept" grep -qx "SRC=$W/real/erssi" "$HOME/.local/opt/erssi/share/irssi/shellter-install.conf"
    # the source directory itself a link of ours to a directory of ours
    mkdir -p "$W/real2/erssi"
    ln -s "$W/real2/erssi" "$W/srclink"
    run_install --src "$W/srclink"
    check "linked source dir: installed" [ "$RC" = 0 ]
    check "linked source dir: checked out in the target" [ -d "$W/real2/erssi/.git" ]
    # ... to a directory of theirs, or under an open directory: the target counts
    mkdir -p "$W/real3/erssi"
    foreign "$W/real3/erssi"
    ln -s "$W/real3/erssi" "$W/srclink3"
    run_install --src "$W/srclink3"
    no_fakes
    check "link to their directory: refused" [ "$RC" = 1 ]
    check "link to their directory: named" grep -qF "$W/real3/erssi belongs to another user" "$W/out"
    mkdir -p "$W/open2/erssi"
    chmod 0777 "$W/open2"
    ln -s "$W/open2/erssi" "$W/srclink4"
    run_install --src "$W/srclink4"
    chmod 0755 "$W/open2"
    check "link into an open directory: refused" [ "$RC" = 1 ]
    check "link into an open directory: named" grep -qF "$W/open2 is writable by other users" "$W/out"
    # an open directory of ours named like "owner 0, sticky mode"
    P="$W/x:0:abcdefghit"
    mkdir -p "$P/mine"
    chmod 0777 "$P"
    run_install --src "$P/mine/erssi"
    chmod 0755 "$P"
    check "open directory named like root's sticky one: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$P is writable by other users" "$W/out"
    check "... nothing made" none_exist "$P/mine/erssi" "$P/mine/erssi.lock"
    # a link to nothing, or to a file: named in the message
    ln -s "$W/nowhere" "$W/dangling"
    run_install --src "$W/dangling"
    check "dangling link: refused" [ "$RC" = 1 ]
    check "dangling link: named" grep -qF "$W/dangling is a link, but not to a directory" "$W/out"
    : > "$W/afile"
    ln -s "$W/afile" "$W/filelink"
    run_install --src "$W/filelink"
    check "link to a file: refused" [ "$RC" = 1 ]
    check "link to a file: named" grep -qF "$W/filelink is a link, but not to a directory" "$W/out"
    # a link to a directory whose name has a newline: invalid
    NL=$(printf 'a\nb')
    mkdir -p "$W/$NL"
    ln -s "$W/$NL" "$W/nllink"
    run_install --src "$W/nllink"
    check "link to a name with a newline: refused" [ "$RC" = 1 ]
    check "... invalid directory" grep -qF "invalid directory" "$W/out"
    check "... nothing made" none_exist "$W/$NL.lock" "$W/$NL/.git"
    ;;
acl-bsd)
    # FreeBSD and macOS: an ACL ("+") can grant write without the mode
    # bits showing it, so it is not trusted there; on Linux the group bits
    # are the ACL mask and the mode rule stays
    no_meson_python
    another_user
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\necho "${FAKE_UNAME:-Linux}"\n' | stand uname
    use_tool uname
    SHELLTER_OS_RELEASE=$W/none; export SHELLTER_OS_RELEASE
    V=$HOME/.local/src/erssi.venv
    plant_venv "$V"
    fake_mode "$V" drwxr-xr-x+
    FAKE_UNAME=Linux; export FAKE_UNAME
    run_install
    check "Linux, venv with an ACL and no group write: installed" [ "$RC" = 0 ]
    check "... reused" [ -s "$W/ran-venv" ]
    rm -f "$W/ran-venv"
    FAKE_UNAME=FreeBSD
    run_install
    check "FreeBSD, venv with an ACL: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$V can be changed by other users" "$W/out"
    check "... nothing run" none_exist "$W/ran-venv"
    FAKE_UNAME=Darwin
    run_install
    check "macOS, venv with an ACL: refused" [ "$RC" = 1 ]
    no_fakes
    FAKE_UNAME=FreeBSD
    fake_mode "$HOME/.local/src/erssi" drwxr-xr-x+
    run_install
    no_fakes
    check "FreeBSD source dir with an ACL: refused" [ "$RC" = 1 ]
    fake_mode "$HOME/.local/src" drwxr-xr-x+
    run_install
    check "FreeBSD parent with an ACL: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$HOME/.local/src is writable by other users" "$W/out"
    FAKE_UNAME=Linux
    run_install
    no_fakes
    check "Linux parent with an ACL and no group write: installed" [ "$RC" = 0 ]
    # further up: a sticky directory of root's with an ACL
    mkdir -p "$W/tmpacl/mine"
    printf '%s 0 drwxrwxrwt+\n' "$W/tmpacl" >> "$W/lsfake"
    run_install --src "$W/tmpacl/mine/erssi"
    check "Linux, sticky of root's with an ACL further up: installed" [ "$RC" = 0 ]
    FAKE_UNAME=FreeBSD
    run_install --src "$W/tmpacl/mine/erssi"
    no_fakes
    check "FreeBSD, sticky of root's with an ACL further up: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/tmpacl is writable by other users" "$W/out"
    ;;
userns)
    # In a user namespace (rootless container, bwrap, unshare) the
    # directories of users it does not map, root's among them, show as the
    # overflow uid. SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 counts that uid as
    # root's above the parent of --src only. The namespace is a stand-in
    # /proc (SHELLTER_PROC: self/uid_map, sys/kernel/overflowuid), the
    # owners come from ls; the real kernel: case userns-kernel
    no_meson_python
    another_user
    mkdir -p "$W/proc/self" "$W/proc/sys/kernel"
    echo 65534 > "$W/proc/sys/kernel/overflowuid"
    U=$(id -u)
    userns_map() { printf '%10s %10s %10s\n' "$U" "$U" 1 > "$W/proc/self/uid_map"; }
    init_map() { printf '%10s %10s %10s\n' 0 0 4294967295 > "$W/proc/self/uid_map"; }
    userns_map
    SHELLTER_PROC=$W/proc; export SHELLTER_PROC
    OPT=SHELLTER_INSTALL_TRUST_OVERFLOW_UID
    # (a) further up: refused without the option, which the message names;
    # installed with it
    mkdir -p "$W/up/mine"
    fake_owner "$W/up" 65534
    run_install --src "$W/up/mine/erssi"
    check "overflow uid further up: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/up belongs to another user (uid 65534)" "$W/out"
    check "... the option named" grep -qF "$OPT=1" "$W/out"
    check "... nothing made" none_exist "$W/up/mine/erssi" "$W/up/mine/erssi.lock"
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=0; export SHELLTER_INSTALL_TRUST_OVERFLOW_UID
    run_install --src "$W/up/mine/erssi"
    check "option 0: refused" [ "$RC" = 1 ]
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1
    run_install --src "$W/up/mine/erssi"
    check "overflow uid further up, option: installed" [ "$RC" = 0 ]
    check "... checked out" [ -d "$W/up/mine/erssi/.git" ]
    # only the overflow uid: another user further up is still refused
    mkdir -p "$W/other/mine"
    foreign "$W/other"
    run_install --src "$W/other/mine/erssi"
    check "another uid further up, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/other belongs to another user (uid 4242)" "$W/out"
    no_fakes
    # (b) the parent, the source directory and the venv: never
    mkdir -p "$W/ovp"
    fake_owner "$W/ovp" 65534
    run_install --src "$W/ovp/erssi"
    check "overflow uid as the parent, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/ovp belongs to another user (uid 65534)" "$W/out"
    check "... the option does not cover it" grep -qF "$OPT does not cover the parent of --src" "$W/out"
    check "... nothing made" none_exist "$W/ovp/erssi" "$W/ovp/erssi.lock"
    no_fakes
    mkdir -p "$W/mine2/erssi"
    fake_owner "$W/mine2/erssi" 65534
    run_install --src "$W/mine2/erssi"
    check "overflow uid as the source dir, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/mine2/erssi belongs to another user (uid 65534)" "$W/out"
    check "... nothing checked out" none_exist "$W/mine2/erssi/.git"
    no_fakes
    V=$HOME/.local/src/erssi.venv
    rm -f "$W/ran-venv" "$W/py.log" "$W/ran-pip"
    plant_venv "$V"
    fake_owner "$V" 65534
    run_install --src "$HOME/.local/src/erssi"
    check "overflow uid as the venv, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$V belongs to another user (uid 65534)" "$W/out"
    check "... nothing run" none_exist "$W/ran-venv" "$W/py.log" "$W/ran-pip"
    no_fakes
    rm -rf "$V"
    # (c) further up and writable by others: like root's, only sticky
    mkdir -p "$W/open/mine"
    chmod 0777 "$W/open"
    fake_owner "$W/open" 65534
    run_install --src "$W/open/mine/erssi"
    check "overflow uid, open further up, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/open is writable by other users" "$W/out"
    check "... nothing made" none_exist "$W/open/mine/erssi" "$W/open/mine/erssi.lock"
    chmod 0775 "$W/open"
    FAKE_GROUP_NAME=staff; export FAKE_GROUP_NAME
    run_install --src "$W/open/mine/erssi"
    unset FAKE_GROUP_NAME
    check "overflow uid, group-writable further up, option: refused" [ "$RC" = 1 ]
    chmod 1777 "$W/open"
    run_install --src "$W/open/mine/erssi"
    check "overflow uid, sticky further up (/tmp), option: installed" [ "$RC" = 0 ]
    run_install --src "$W/open/erssi3"
    check "overflow uid, sticky as the parent, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/open belongs to another user (uid 65534)" "$W/out"
    chmod 0755 "$W/open"
    no_fakes
    # (d) outside a user namespace (the identity map): the option does nothing
    rm -rf "$W/up/mine/erssi" "$W/up/mine/erssi.lock"
    fake_owner "$W/up" 65534
    init_map
    run_install --src "$W/up/mine/erssi"
    check "no user namespace, option: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$W/up belongs to another user (uid 65534)" "$W/out"
    check "... the option not named" no_grep "$OPT" "$W/out"
    check "... nothing made" none_exist "$W/up/mine/erssi" "$W/up/mine/erssi.lock"
    unset SHELLTER_INSTALL_TRUST_OVERFLOW_UID
    run_install --src "$W/up/mine/erssi"
    check "no user namespace, no option: the option not named" no_grep "$OPT" "$W/out"
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1; export SHELLTER_INSTALL_TRUST_OVERFLOW_UID
    # no /proc (FreeBSD), or no overflowuid in it: nothing, no error
    SHELLTER_PROC=$W/noproc
    run_install --src "$W/up/mine/erssi"
    check "no /proc, option: refused" [ "$RC" = 1 ]
    check "... only the owner named" grep -qF "error: $W/up belongs to another user (uid 65534) - choose a --src" "$W/out"
    check "... the option not named" no_grep "$OPT" "$W/out"
    check "... no other error" [ "$(grep -c 'error\|cannot\|No such' "$W/out")" = 1 ]
    SHELLTER_PROC=$W/proc
    userns_map
    rm -f "$W/proc/sys/kernel/overflowuid"
    run_install --src "$W/up/mine/erssi"
    check "no overflowuid, option: refused" [ "$RC" = 1 ]
    check "... the option not named" no_grep "$OPT" "$W/out"
    echo 'x' > "$W/proc/sys/kernel/overflowuid"
    run_install --src "$W/up/mine/erssi"
    check "overflowuid not a number, option: refused" [ "$RC" = 1 ]
    echo 65534 > "$W/proc/sys/kernel/overflowuid"
    run_install --src "$W/up/mine/erssi"
    no_fakes
    check "back in the user namespace, option: installed" [ "$RC" = 0 ]
    # the user's own uid not mapped: id -u is the overflow uid too, so every
    # unmapped user's directory would look like the user's - refused, with
    # or without the option, before anything is made
    FAKE_UID=65534; export FAKE_UID
    mkdir -p "$W/um/mine"
    for opt in 1 ""; do
        SHELLTER_INSTALL_TRUST_OVERFLOW_UID=$opt
        run_install --src "$W/um/mine/erssi"
        check "own uid unmapped${opt:+, option}: refused" [ "$RC" = 1 ]
        check "... named" grep -qF "your uid is not mapped in this user namespace" "$W/out"
        check "... nothing made" none_exist "$W/um/mine/erssi" "$W/um/mine/erssi.lock"
    done
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1
    # safe_dir on its own (the venv and --src checks): a directory of an
    # unmapped user that looks like the user's
    mkdir -p "$W/um/venv"
    fake_owner "$W/um/venv" 65534
    # shellcheck disable=SC2016 # run by sh
    msg=$(SHELLTER_INSTALL_NO_MAIN=1 sh -c '. "$1"; ui_init
            owner() { ls -ldn "$1" 2>/dev/null | awk "{print \$3}"; }
            safe_dir "$2"' sh "$SRC/shellter-install.sh" "$W/um/venv" 2>&1)
    RC=$?
    printf '%s\n' "$msg" > "$W/out"
    check "own uid unmapped, safe_dir: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "your uid is not mapped in this user namespace" "$W/out"
    # outside a user namespace a uid equal to the overflow uid is a real
    # user (nobody): the usual rules
    init_map
    run_install --src "$W/um/mine/erssi"
    check "uid 65534 outside a user namespace: not called unmapped" no_grep "not mapped" "$W/out"
    no_fakes
    unset FAKE_UID
    ;;
userns-kernel)
    # the same in a real user namespace (unshare, only the user mapped):
    # directories above the temporary directory (/ at least) show as the
    # overflow uid; skipped where the kernel or unshare does not allow one
    [ -n "$UNSHARE" ] || { echo "skip: no unshare"; exit 77; }
    in_ns() { "$UNSHARE" --user --map-current-user "$@"; }
    # shellcheck disable=SC2016 # run by sh in the namespace
    ns_root=$(in_ns sh -c 'echo "$(cat /proc/sys/kernel/overflowuid) $(ls -ldn / | awk "{print \$3}")"' 2>/dev/null) ||
        { echo "skip: no user namespace here (unshare --user --map-current-user)"; exit 77; }
    case "$ns_root" in
        [0-9]*" "[0-9]*) ;;
        *) echo "skip: no overflow uid or owner of / in a user namespace ($ns_root)"; exit 77 ;;
    esac
    case "${ns_root% *}${ns_root#* }" in
        *[!0-9]*) echo "skip: not numbers: overflow uid and owner of / ($ns_root)"; exit 77 ;;
    esac
    [ "${ns_root% *}" = "${ns_root#* }" ] ||
        { echo "skip: / does not show as the overflow uid in a user namespace ($ns_root)"; exit 77; }
    mkdir -p "$W/mine"
    in_ns sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test --src "$W/mine/erssi" > "$W/out" 2>&1 < /dev/null
    RC=$?
    check "user namespace, no option: refused" [ "$RC" = 1 ]
    check "... the overflow uid named" grep -qF "belongs to another user (uid ${ns_root% *})" "$W/out"
    check "... the option named" grep -qF "SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1" "$W/out"
    check "... nothing made" none_exist "$W/mine/erssi" "$W/mine/erssi.lock"
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 in_ns sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test \
        --src "$W/mine/erssi" > "$W/out" 2>&1 < /dev/null
    RC=$?
    check "user namespace, option: installed" [ "$RC" = 0 ]
    check "... checked out" [ -d "$W/mine/erssi/.git" ]
    # the parent is never trusted: --src right under /, which shows as the
    # overflow uid (refused before anything is made there)
    SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 in_ns sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --no-test \
        --src /erssi-userns-test > "$W/out" 2>&1 < /dev/null
    RC=$?
    check "user namespace, option, / as the parent: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "/ belongs to another user (uid ${ns_root% *})" "$W/out"
    # a namespace that does not map the user (unshare --user alone): id -u
    # is the overflow uid, like every directory - refused, option or not
    if [ "$("$UNSHARE" --user id -u 2>/dev/null)" = "${ns_root% *}" ]; then
        rm -rf "$W/mine/erssi" "$W/mine/erssi.lock"
        SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 "$UNSHARE" --user sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main \
            --no-test --src "$W/mine/erssi" > "$W/out" 2>&1 < /dev/null
        RC=$?
        check "own uid unmapped, option: refused" [ "$RC" = 1 ]
        check "... named" grep -qF "your uid is not mapped in this user namespace" "$W/out"
        check "... nothing made" none_exist "$W/mine/erssi" "$W/mine/erssi.lock"
    else
        echo "skip: own uid unmapped (unshare --user without a map not allowed here)"
    fi
    ;;
venv-meson-only)
    # the system has ninja, not meson: the venv gets meson only, and the
    # system's ninja builds
    os_release debian
    no_meson_python
    use_tool ninja
    V=$HOME/.local/src/erssi.venv
    run_install
    check "installed" [ "$RC" = 0 ]
    check "pip ran once" pip_runs 1
    check "pip installs meson only" pip_installs_only meson
    check "the warning names meson only" grep -q 'meson not found - installing meson==1.12.1 into' "$W/out"
    check "no ninja in the venv" none_exist "$V/bin/ninja"
    check "the venv's meson ran" venv_ran meson
    check "the system's ninja ran" venv_not_ran ninja
    # the next run reuses the venv's meson: nothing installed
    rm -f "$W/ran-pip" "$W/ran-venv" "$W/py.log"
    run_install
    check "again: installed" [ "$RC" = 0 ]
    check "again: no pip" none_exist "$W/ran-pip" "$W/py.log"
    check "again: the venv's meson ran" venv_ran meson
    # the system loses ninja: only ninja is added, meson stays
    rm -f "$W/shim/ninja" "$W/ran-pip" "$W/ran-venv"
    run_install
    check "ninja gone: installed" [ "$RC" = 0 ]
    check "ninja gone: pip installs ninja only" pip_installs_only ninja
    check "ninja gone: as a wheel only" ninja_binary_only
    check "ninja gone: the venv's ninja ran" venv_ran ninja
    ;;
venv-ninja-only)
    # the system has meson, not ninja: the venv gets ninja only, as a
    # wheel (never built from the sdist), and the system's meson builds
    os_release debian
    no_meson_python
    use_tool meson
    V=$HOME/.local/src/erssi.venv
    run_install
    check "installed" [ "$RC" = 0 ]
    check "pip ran once" pip_runs 1
    check "pip installs ninja only" pip_installs_only ninja
    check "ninja as a wheel only" ninja_binary_only
    check "nothing built from the sdist" none_exist "$W/pip-sdist"
    check "the warning names ninja only" grep -q 'ninja not found - installing ninja==1.13.2 into' "$W/out"
    check "no meson in the venv" none_exist "$V/bin/meson"
    check "the venv's ninja ran" venv_ran ninja
    check "the system's meson ran" venv_not_ran meson
    # a venv from an older installer with both: reused, and its meson does
    # not shadow the system's
    plant_venv "$V"
    rm -f "$W/ran-pip" "$W/ran-venv" "$W/py.log"
    run_install
    check "older venv: installed" [ "$RC" = 0 ]
    check "older venv: no pip" none_exist "$W/ran-pip" "$W/py.log"
    check "older venv: its ninja ran" venv_ran ninja
    check "older venv: the system's meson ran" venv_not_ran meson
    ;;
venv-no-ninja-wheel)
    # no ninja wheel for the platform (FreeBSD): pip is not left to build
    # ninja (and cmake) from source; the installer stops at once and names
    # the package
    freebsd
    base_openssl_files "$W/base" '# define OPENSSL_VERSION_TEXT "OpenSSL 3.0.15 3 Sep 2024"'
    no_meson_python
    use_tool meson
    FAKE_PIP_NO_WHEEL=ninja; export FAKE_PIP_NO_WHEEL
    run_install
    check "FreeBSD: refused" [ "$RC" = 1 ]
    check "FreeBSD: nothing built from the sdist" none_exist "$W/pip-sdist"
    check "FreeBSD: ninja as a wheel only" ninja_binary_only
    check "FreeBSD: the package named" grep -qF 'pkg install ninja   (as root)' "$W/out"
    check "FreeBSD: the causes named" grep -qF 'pip could not install ninja (no network, or no ready-made wheel for this system)' "$W/out"
    check "FreeBSD: nothing installed" none_exist "$HOME/.local/opt/erssi/bin/erssi"
    # without meson as well: meson comes from pip, ninja still stops it
    rm -f "$W/shim/meson" "$W/ran-pip" "$W/pip-sdist"
    run_install
    check "no meson either: refused" [ "$RC" = 1 ]
    check "no meson either: nothing built from the sdist" none_exist "$W/pip-sdist"
    check "no meson either: the package named" grep -qF 'pkg install ninja   (as root)' "$W/out"
    check "no meson either: not meson" no_grep 'pkg install meson' "$W/out"
    # Debian: the package of apt
    FAKE_UNAME=Linux FAKE_NO_OPENSSL_PC=0; export FAKE_UNAME FAKE_NO_OPENSSL_PC
    os_release debian
    rm -rf "$HOME/.local/src/erssi.venv" "$W/ran-pip"
    use_tool meson
    run_install
    check "Debian: refused" [ "$RC" = 1 ]
    check "Debian: the package named" grep -qF 'sudo apt install ninja-build' "$W/out"
    ;;
venv-both)
    # neither meson nor ninja: both into the venv, ninja as a wheel only
    os_release debian
    no_meson_python
    V=$HOME/.local/src/erssi.venv
    run_install
    check "installed" [ "$RC" = 0 ]
    check "meson installed" grep -q 'meson==1.12.1' "$W/ran-pip"
    check "ninja installed" grep -q 'ninja==1.13.2' "$W/ran-pip"
    check "ninja as a wheel only" ninja_binary_only
    check "nothing built from the sdist" none_exist "$W/pip-sdist"
    check "the warning names both" grep -q 'meson and ninja not found - installing meson==1.12.1 and ninja==1.13.2 into' "$W/out"
    check "the venv's meson ran" venv_ran meson
    check "the venv's ninja ran" venv_ran ninja
    rm -f "$W/ran-pip" "$W/py.log"
    run_install
    check "again: installed" [ "$RC" = 0 ]
    check "again: no pip" none_exist "$W/ran-pip" "$W/py.log"
    ;;
stale-nexus)
    # erssi before 2026.10.0 shipped nexus.theme: an install removes it from
    # the prefix and keeps the other themes (and a copy in the home)
    P=$HOME/.local/opt/erssi
    mkdir -p "$P/share/irssi/themes" "$HOME/.erssi"
    : > "$P/share/irssi/themes/nexus.theme"
    : > "$P/share/irssi/themes/shellter.theme"
    : > "$HOME/.erssi/nexus.theme"
    run_install
    check "installed" [ "$RC" = 0 ]
    check "nexus.theme gone from the prefix" none_exist "$P/share/irssi/themes/nexus.theme"
    check "another theme kept" [ -f "$P/share/irssi/themes/shellter.theme" ]
    check "a copy in the home kept" [ -f "$HOME/.erssi/nexus.theme" ]
    ;;
release-signature)
    # signed_tag: an annotated tag of that name with an SSH signature by the
    # release key, nothing else
    release_key
    c=$(release_commit 2026.10.2)
    rtag shellter-v2026.10.2 "$c"
    st shellter-v2026.10.2
    check "signed by the release key: verified" [ "$RC" = 0 ]
    rtag shellter-v2026.10.3 "$c" "$W/other"
    st shellter-v2026.10.3
    check "signed by another key: refused" [ "$RC" = 1 ]
    git -C "$W/repo" tag shellter-v2026.10.4 "$c"
    st shellter-v2026.10.4
    check "lightweight tag: refused" [ "$RC" = 1 ]
    git -C "$W/repo" -c user.name=test -c user.email=test@example.invalid tag -a -m x shellter-v2026.10.5 "$c"
    st shellter-v2026.10.5
    check "annotated, unsigned: refused" [ "$RC" = 1 ]
    # a validly signed tag object of another release, published under this
    # name: the name inside the signed object counts
    rtag shellter-v2026.10.9 "$c"
    git -C "$W/repo" update-ref refs/tags/shellter-v2026.10.6 "$(git -C "$W/repo" rev-parse shellter-v2026.10.9)"
    st shellter-v2026.10.6
    check "signed object of another tag name: refused" [ "$RC" = 1 ]
    st shellter-v2026.10.9
    check "... the same object under its own name: verified" [ "$RC" = 0 ]
    st shellter-v2026.10.2 shellter-v2026.10.3
    check "a verified tag checked as another release: refused" [ "$RC" = 1 ]
    # an OpenPGP signature, with a gpg in PATH that accepts anything
    gpg_accepts_all
    ptag shellter-v2026.10.7 "$c"
    st shellter-v2026.10.7
    check "OpenPGP-signed tag: refused" [ "$RC" = 1 ]
    check "... gpg never ran" none_exist "$W/ran-gpg"
    st shellter-v2026.10.2
    check "the good tag with that gpg in PATH: still verified" [ "$RC" = 0 ]
    ;;
release-newest)
    # newest_signed: the newest release newer than the installed one that
    # the release key signed; unsigned ones are named and skipped, five at most
    release_key
    ns() { fn 'newest_signed "$@"' "$W/work" "$@"; }
    git init -q "$W/work"
    c1=$(release_commit 2026.10.1); rtag shellter-v2026.10.1 "$c1"
    c2=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c2"
    c3=$(release_commit 2026.10.3); rtag shellter-v2026.10.3 "$c3" "$W/other"
    ns 2026.10.1 "$c1"
    check "a newer signed release: found" [ "$RC" = 0 ]
    check "... shellter-v2026.10.2 and its tag object" \
        [ "$(cat "$W/fn.out")" = "shellter-v2026.10.2 $(git -C "$W/repo" rev-parse shellter-v2026.10.2)" ]
    check "... the unsigned newer one named" \
        grep -q 'shellter-v2026.10.3 is not signed by the erssi Shellter release key - skipped' "$W/fn.err"
    # a download that fails is not an unsigned release
    fetch_fails
    FAKE_FETCH_FAIL=1 ns 2026.10.1 "$c1"
    check "download fails: code 3" [ "$RC" = 3 ]
    check "... the release named" [ "$(cat "$W/fn.out")" = shellter-v2026.10.3 ]
    check "... not called unsigned" no_grep 'not signed' "$W/fn.err"
    ns 2026.10.2 "$c2"
    check "the installed one is the newest signed: code 0" [ "$RC" = 0 ]
    check "... nothing printed" [ ! -s "$W/fn.out" ]
    # four unsigned newer releases are skipped, the fifth gives up
    for v in 4 5 6; do rtag "shellter-v2026.10.$v" "$c3" "$W/other"; done
    ns 2026.10.1 "$c1"
    check "four unsigned newer: the signed one found" [ "$(cut -d' ' -f1 "$W/fn.out")" = shellter-v2026.10.2 ]
    rtag shellter-v2026.10.7 "$c3" "$W/other"
    ns 2026.10.1 "$c1"
    check "five unsigned newer: gives up" [ "$RC" = 1 ]
    check "... nothing printed" [ ! -s "$W/fn.out" ]
    check "... and says so" grep -q 'five unsigned newer tags - giving up (report it)' "$W/fn.err"
    # a repository that cannot be reached
    mv "$W/repo" "$W/repo.away"
    ns 2026.10.1 "$c1"
    mv "$W/repo.away" "$W/repo"
    check "no repository: code 2" [ "$RC" = 2 ]
    ;;
release-pinned)
    # an install of a recorded release: the tag must point to the recorded
    # commit and carry the release key's signature
    release_key
    c2=$(release_commit 2026.10.2)
    c3=$(release_commit 2026.10.3)
    printf 'shellter-v2026.10.2 %s signed\n' "$c2" > "$W/releases"
    patch_installer
    rtag shellter-v2026.10.2 "$c2"
    run_release
    check "the newest recorded release: installed" [ "$RC" = 0 ]
    check "... signed and the commit recorded" grep -q 'Downloading.*signed - commit recorded' "$W/out"
    check "... version 2026.10.2" grep -q 'erssi Shellter Edition 2026.10.2 is installed' "$W/out"
    check "... the commit kept" grep -qx "COMMIT=$c2" "$HOME/.local/opt/erssi/share/irssi/shellter-install.conf"
    check "... and the release" grep -qx "REF=shellter-v2026.10.2" "$HOME/.local/opt/erssi/share/irssi/shellter-install.conf"
    # moved to another commit, even signed by the release key: refused
    rtag shellter-v2026.10.2 "$c3"
    run_release
    check "tag moved to another commit: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "shellter-v2026.10.2 points to $c3, but this installer expects $c2 - refusing to build" "$W/out"
    check "... nothing built" no_grep 'Building' "$W/out"
    # on the recorded commit, signed by another key: refused
    rtag shellter-v2026.10.2 "$c2" "$W/other"
    run_release --ref shellter-v2026.10.2
    check "signed by another key: refused" [ "$RC" = 1 ]
    check "... named" grep -qF 'shellter-v2026.10.2 does not carry a valid signature by the erssi Shellter release key - refusing to build' "$W/out"
    # without ssh-keygen: a warning, and the commit is still checked
    rm -f "$W/bin/ssh-keygen"
    run_release
    check "no ssh-keygen: installed" [ "$RC" = 0 ]
    check "... with a warning" grep -q 'signatures cannot be checked here' "$W/out"
    check "... the commit recorded" grep -q 'Downloading.*ok.*commit recorded' "$W/out"
    git -C "$W/repo" tag -f shellter-v2026.10.2 "$c3" > /dev/null
    run_release
    check "no ssh-keygen, tag moved: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "shellter-v2026.10.2 points to $c3, but this installer expects $c2" "$W/out"
    ;;
update)
    # erssi --update: the newest release signed by the release key is built
    # and installed into the same prefix
    release_key
    c1=$(release_commit 2026.10.1); rtag shellter-v2026.10.1 "$c1"
    c2=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c2"
    release_prefix 2026.10.1 "$c1" shellter-v2026.10.1
    # the download fails: said so, neither "unsigned" nor "up to date"
    fetch_fails
    FAKE_FETCH_FAIL=1; export FAKE_FETCH_FAIL
    run_update --check
    check "download fails, --check: failed" [ "$RC" = 1 ]
    check "... named" grep -qF "cannot download shellter-v2026.10.2 from $W/repo" "$W/out"
    check "... not called unsigned" no_grep 'not signed' "$W/out"
    check "... not up to date" no_grep 'up to date' "$W/out"
    run_update
    check "download fails, --update: failed" [ "$RC" = 1 ]
    check "... named" grep -qF "cannot download shellter-v2026.10.2 from $W/repo" "$W/out"
    check "... nothing built" none_exist "$HOME/.local/src/erssi/Build"
    check "... nothing installed" [ "$("$W/p/bin/erssi" --version)" = "erssi 2026.10.1 (20250101 1200)" ]
    unset FAKE_FETCH_FAIL
    run_update --check
    check "--check: update available" [ "$RC" = 0 ]
    check "... named" grep -q 'Update available: 2026.10.1 -> 2026.10.2' "$W/out"
    check "... nothing built" none_exist "$HOME/.local/src/erssi/Build"
    run_update
    check "update: done" [ "$RC" = 0 ]
    check "... signed" grep -q 'Finding the release.*shellter-v2026.10.2 - signed by the erssi Shellter release key' "$W/out"
    check "... updated" grep -q 'Updated: 2026.10.1 -> 2026.10.2' "$W/out"
    check "... the new erssi" [ "$("$W/p/bin/erssi" --version)" = "erssi 2026.10.2 (20261010 1200)" ]
    check "... its commit kept" grep -qx "COMMIT=$c2" "$W/p/share/irssi/shellter-install.conf"
    check "... and the release" grep -qx "REF=shellter-v2026.10.2" "$W/p/share/irssi/shellter-install.conf"
    run_update --check
    check "again: up to date" grep -q 'erssi Shellter Edition 2026.10.2 is up to date' "$W/out"
    # a newer release signed by another key: named, not offered
    c3=$(release_commit 2026.10.3); rtag shellter-v2026.10.3 "$c3" "$W/other"
    run_update --check
    check "newer, signed by another key: up to date" grep -q 'is up to date' "$W/out"
    check "... named" grep -q 'shellter-v2026.10.3 is not signed by the erssi Shellter release key' "$W/out"
    run_update
    check "... not built" [ "$("$W/p/bin/erssi" --version)" = "erssi 2026.10.2 (20261010 1200)" ]
    # a release this installer records, on another commit than recorded
    c4=$(release_commit 2026.10.4); rtag shellter-v2026.10.4 "$c4"
    printf 'shellter-v2026.10.4 %s signed\n' "$c3" > "$W/releases"
    patch_installer
    run_update
    check "recorded release on another commit: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "shellter-v2026.10.4 points to $c4, but this installer expects $c3 - refusing to build" "$W/out"
    check "... not installed" [ "$("$W/p/bin/erssi" --version)" = "erssi 2026.10.2 (20261010 1200)" ]
    printf 'shellter-v2026.10.4 %s signed\n' "$c4" > "$W/releases"
    patch_installer
    run_update
    check "on the recorded commit: updated" grep -q 'Updated: 2026.10.2 -> 2026.10.4' "$W/out"
    check "... signed and recorded" grep -q 'Downloading.*signed - commit recorded' "$W/out"
    ;;
update-same-version)
    # Release tags never move: a signed tag of the installed release that
    # now points to another commit is reported, never installed - a newer
    # commit (a tag moved by mistake) or an older one (a mirror serving the
    # previous tag object: a rollback)
    release_key
    c3=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c3"
    old_tag=$(git -C "$W/repo" rev-parse shellter-v2026.10.2)
    c4=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c4"
    release_prefix 2026.10.2 "$c4" shellter-v2026.10.2
    git -C "$W/repo" update-ref refs/tags/shellter-v2026.10.2 "$old_tag"
    moved="the release tag shellter-v2026.10.2 now points to a different commit than the installed one - not updating; report it"
    run_update --check
    check "older tag object of the installed release, --check: no update" [ "$RC" = 0 ]
    check "... reported" grep -qF "$moved" "$W/out"
    check "... no changes offered" no_grep 'available' "$W/out"
    run_update
    check "... --update: nothing built" no_grep 'Building' "$W/out"
    check "... the installed commit kept" grep -qx "COMMIT=$c4" "$W/p/share/irssi/shellter-install.conf"
    # the tag moved on to a newer commit: the same
    c5=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c5"
    run_update
    check "tag moved to a newer commit: reported" grep -qF "$moved" "$W/out"
    check "... nothing built" no_grep 'Building' "$W/out"
    check "... the installed commit kept" grep -qx "COMMIT=$c4" "$W/p/share/irssi/shellter-install.conf"
    # an install from before REF was recorded: a release, as an update or
    # an install without --ref made it
    release_prefix 2026.10.2 "$c4"
    run_update --check
    check "no REF recorded: reported" grep -qF "$moved" "$W/out"
    check "... no changes offered" no_grep 'available' "$W/out"
    # the tag where it was: nothing to say
    rtag shellter-v2026.10.2 "$c4"
    run_update --check
    check "the tag on the installed commit: up to date" grep -q 'erssi Shellter Edition 2026.10.2 is up to date' "$W/out"
    check "... nothing reported" no_grep 'different commit' "$W/out"
    # the installed release cannot be downloaded: said so
    fetch_fails
    FAKE_FETCH_FAIL=1 run_update --check
    check "download of the installed release fails: failed" [ "$RC" = 1 ]
    check "... named" grep -qF "cannot download shellter-v2026.10.2 from $W/repo" "$W/out"
    ;;
update-branch)
    # an erssi installed from a branch (--ref main): main keeps the version
    # of the last release until the next one, so a release of that version
    # is not an update - only a newer version is
    release_key
    c2=$(release_commit 2026.10.2); rtag shellter-v2026.10.2 "$c2"
    cm=$(release_commit 2026.10.2)
    release_prefix 2026.10.2 "$cm" main
    run_update --check
    check "release of the same version, --check: nothing to do" [ "$RC" = 0 ]
    check "... named" grep -qF 'Installed from main; newest release is 2026.10.2, not newer than the installed 2026.10.2 - nothing to do.' "$W/out"
    check "... no changes offered" no_grep 'available' "$W/out"
    check "... nothing reported" no_grep 'different commit' "$W/out"
    run_update
    check "... --update: nothing built" no_grep 'Building' "$W/out"
    check "... the installed commit kept" grep -qx "COMMIT=$cm" "$W/p/share/irssi/shellter-install.conf"
    # an older release only: nothing either
    release_prefix 2026.10.3 "$cm" main
    run_update --check
    check "release older than the branch: nothing to do" grep -qF 'newest release is 2026.10.2, not newer than the installed 2026.10.3' "$W/out"
    # a newer release: offered, and an update makes it a release install
    release_prefix 2026.10.2 "$cm" main
    c3=$(release_commit 2026.10.3); rtag shellter-v2026.10.3 "$c3"
    run_update --check
    check "newer release, --check: offered" grep -q 'Update available: 2026.10.2 -> 2026.10.3' "$W/out"
    check "... from main" grep -qF 'Installed from main; newest release is 2026.10.3.' "$W/out"
    run_update
    check "... --update: updated" grep -q 'Updated: 2026.10.2 -> 2026.10.3' "$W/out"
    check "... the release recorded" grep -qx "REF=shellter-v2026.10.3" "$W/p/share/irssi/shellter-install.conf"
    check "... its commit" grep -qx "COMMIT=$c3" "$W/p/share/irssi/shellter-install.conf"
    # an install records what it was installed from
    run_release --ref main --prefix "$W/q"
    check "install from a branch: done" [ "$RC" = 0 ]
    check "... main recorded" grep -qx "REF=main" "$W/q/share/irssi/shellter-install.conf"
    check "... said to be unverified" grep -q 'main is not a release - built as is, not verified' "$W/out"
    ;;
saved-settings)
    # --no-test and --clean are kept for the next install and erssi
    # --update; --test and --no-clean undo them
    CONF=$HOME/.local/opt/erssi/share/irssi/shellter-install.conf
    run_install --clean
    check "--no-test: saved" grep -qx 'TESTS=0' "$CONF"
    check "--clean: saved" grep -qx 'CLEAN=1' "$CONF"
    check "... build directory removed" none_exist "$HOME/.local/src/erssi/Build"
    sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main > "$W/out" 2>&1 < /dev/null
    check "again without them: still no tests" grep -q 'Testing *- skipped' "$W/out"
    check "... still clean" none_exist "$HOME/.local/src/erssi/Build"
    sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main --test --no-clean > "$W/out" 2>&1 < /dev/null
    check "--test --no-clean: installed" [ $? = 0 ]
    check "... the tests ran" grep -q 'Testing *ok' "$W/out"
    check "... the build directory kept" [ -f "$HOME/.local/src/erssi/Build/build.ninja" ]
    check "... --test saved" grep -qx 'TESTS=1' "$CONF"
    check "... --no-clean saved" grep -qx 'CLEAN=0' "$CONF"
    sh "$SRC/shellter-install.sh" --repo "$W/repo" --ref main > "$W/out" 2>&1 < /dev/null
    check "again without them: the tests run" grep -q 'Testing *ok' "$W/out"
    check "... the build directory kept" [ -f "$HOME/.local/src/erssi/Build/build.ninja" ]
    sh "$SRC/shellter-install.sh" --help > "$W/out" 2>&1
    check "--help names --test" grep -q -- '^  --test ' "$W/out"
    check "--help names --no-clean" grep -q -- '^  --no-clean ' "$W/out"
    check "... and that the settings are kept" grep -qF 'shellter-install.conf' "$W/out"
    check "... and what is verified" grep -q '^What is verified: ' "$W/out"
    ;;
binary-platform)
    # the platform of the packages, from uname, os-release and freebsd-version
    binary_tools
    pid() { fn 'platform_id'; }
    pid
    check "Debian 13 on x86_64: debian13-amd64" [ "$(cat "$W/fn.out")" = debian13-amd64 ]
    printf 'ID="debian"\nVERSION_ID="13.1"\n' > "$W/os-release"
    FAKE_UNAME_M=aarch64 pid
    check "quoted, a minor version, aarch64: debian13-arm64" [ "$(cat "$W/fn.out")" = debian13-arm64 ]
    printf 'ID=ubuntu\nVERSION_ID="24.04"\n' > "$W/os-release"
    pid
    check "Ubuntu 24.04: ubuntu24-amd64" [ "$(cat "$W/fn.out")" = ubuntu24-amd64 ]
    FAKE_UNAME=FreeBSD FAKE_UNAME_M=amd64 pid
    check "FreeBSD 14.3: freebsd14-amd64" [ "$(cat "$W/fn.out")" = freebsd14-amd64 ]
    FAKE_UNAME=FreeBSD FAKE_UNAME_M=amd64 FAKE_FREEBSD_VERSION=15.0-CURRENT pid
    check "FreeBSD 15.0-CURRENT: freebsd15-amd64" [ "$(cat "$W/fn.out")" = freebsd15-amd64 ]
    FAKE_UNAME_M=riscv64 pid
    check "another architecture: none" [ "$RC" = 1 ] && [ ! -s "$W/fn.out" ]
    printf 'ID=debian\n' > "$W/os-release"
    pid
    check "no VERSION_ID (testing): none" [ "$RC" = 1 ]
    printf 'ID=x/../y\nVERSION_ID=13\n' > "$W/os-release"
    pid
    check "an ID that is not a name: none" [ "$RC" = 1 ]
    rm -f "$W/os-release"
    pid
    check "no os-release: none" [ "$RC" = 1 ]
    FAKE_UNAME=Darwin pid
    check "another system: none" [ "$RC" = 1 ]
    fn 'perl_version'
    check "perl v5.40.1: 5.40.1" [ "$(cat "$W/fn.out")" = 5.40.1 ]
    # the link switch renames a new link over the old one: never into the
    # directory the old one points to (as mv would)
    mkdir -p "$W/sw/a" "$W/sw/b" && ln -s a "$W/sw/cur"
    # shellcheck disable=SC2016 # run by fn
    fn 'switch_link "$1" b' "$W/sw/cur"
    check "switch_link: switched" [ "$RC" = 0 ] && [ "$(readlink "$W/sw/cur")" = b ]
    check "... nothing left in the old target" [ -z "$(ls -A "$W/sw/a")" ]
    # shellcheck disable=SC2012 # plain names
    check "... nor beside the link" [ "$(ls -A "$W/sw" | tr '\n' ' ')" = "a b cur " ]
    ;;
binary-install)
    # a release with a package for the platform: installed from it, without
    # a build, after checking the signature, the checksum and the MANIFEST
    binary_tools
    c=$(fsha 2026.10.4)
    record 2026.10.4 "$c"
    good_pkg 2026.10.4 "$c"
    mkdir -p "$HOME/.erssi"
    run_bin
    check "installed" [ "$RC" = 0 ]
    check "... the platform" grep -q 'Checking the system.*debian13-amd64 - perl 5.40.1' "$W/out"
    check "... signed and checked" grep -q 'Downloading.*erssi-2026.10.4-debian13-amd64.tar.xz - signed - sha256 verified' "$W/out"
    check "... the MANIFEST checked" grep -q 'Checking the package.*00000000 - commit recorded - libraries present' "$W/out"
    check "... said so" grep -q 'erssi Shellter Edition 2026.10.4 is installed' "$W/out"
    check "... current is releases/2026.10.4" current_is 2026.10.4
    check "... bin/erssi a link into current" [ "$(readlink "$P/bin/erssi")" = ../current/bin/erssi ]
    check "... linked from ~/.local/bin" [ "$(readlink "$HOME/.local/bin/erssi")" = "$P/bin/erssi" ]
    check "... nothing built" no_grep 'Building' "$W/out"
    check "... no source directory" none_exist "$HOME/.local/src/erssi" "$HOME/.local/src/erssi.lock"
    check "... no stage or lock left" releases_are 2026.10.4
    check "... and no lock" none_exist "$P/.install.lock"
    check "... METHOD=binary" grep -qx METHOD=binary "$CONF"
    check "... VERSION" grep -qx VERSION=2026.10.4 "$CONF"
    check "... COMMIT" grep -qx "COMMIT=$c" "$CONF"
    check "... PLATFORM" grep -qx PLATFORM=debian13-amd64 "$CONF"
    check "... the release" grep -qx REF=shellter-v2026.10.4 "$CONF"
    check "... --binary-base kept for --update" grep -qx "BINARY_BASE=$W/rel" "$CONF"
    check "... the installer of the release copied" cmp -s "$P/share/irssi/shellter-install.sh" "$INSTALLER"
    check "... the MANIFEST kept" [ -f "$P/releases/2026.10.4/MANIFEST" ]
    check "... output ASCII only" ascii_only "$W/out"
    # the same release again: the tree in use is kept, nothing else changes
    run_bin
    check "again: done" [ "$RC" = 0 ]
    check "... the tree in use kept" grep -q 'releases/2026.10.4 was already there' "$W/out"
    check "... still current" current_is 2026.10.4
    check "... no previous" none_exist "$P/previous"
    # the release in use, but another package of it: refused
    MF_RUNS_AS=2026.10.4 MF_COMMIT=$c make_pkg 2026.10.4 "$c"
    printf 'extra=1\n' >> "$W/pkg/2026.10.4-debian13-amd64/erssi-2026.10.4-debian13-amd64/MANIFEST"
    tar -C "$W/pkg/2026.10.4-debian13-amd64" -cJf "$W/rel/shellter-v2026.10.4/erssi-2026.10.4-debian13-amd64.tar.xz" erssi-2026.10.4-debian13-amd64
    ( cd "$W/rel/shellter-v2026.10.4" && sha256sum erssi-*.tar.xz ) > "$W/rel/shellter-v2026.10.4/SHA256SUMS"
    sign_sums 2026.10.4
    run_bin
    check "another package of the release in use: refused" [ "$RC" = 1 ]
    check "... named" grep -q 'is in use but holds another package - nothing was changed' "$W/out"
    check "... still current" current_is 2026.10.4
    check "... no stage left" releases_are 2026.10.4
    # --ref of a release the installer does not record: the signature is
    # checked, the commit only for its form
    good_pkg 2026.10.5 "$(fsha 2026.10.5)"
    run_bin --ref shellter-v2026.10.5
    check "unrecorded release: installed" [ "$RC" = 0 ]
    check "... said so" grep -q 'Checking the package.*release not recorded here' "$W/out"
    check "... current" current_is 2026.10.5
    check "... previous is 2026.10.4" [ "$(readlink "$P/previous")" = releases/2026.10.4 ]
    # links within the package are fine; tar unpacks with the user's own
    # owner and modes
    make_pkg 2026.10.7 "$(fsha 2026.10.7)"
    PT="$W/pkg/2026.10.7-debian13-amd64/erssi-2026.10.7-debian13-amd64"
    ln -s libfe_web.so "$PT/lib/irssi/modules/libfe_web.so.0"
    ln -s modules "$PT/lib/irssi/mods"
    ln "$PT/MANIFEST" "$PT/share/irssi/MANIFEST.copy"
    repack 2026.10.7
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\necho "$*" >> "$TEST_W/tar.log"\nexec "$TEST_W/bin/tar" "$@"\n' > "$W/shim/tar" && chmod 755 "$W/shim/tar"
    run_bin --ref shellter-v2026.10.7
    rm -f "$W/shim/tar"
    check "links within the package: installed" [ "$RC" = 0 ] && current_is 2026.10.7
    check "... the links kept" [ "$(readlink "$P/releases/2026.10.7/lib/irssi/mods")" = modules ]
    check "... unpacked without the archive's owners and modes" grep -q -- '--no-same-owner --no-same-permissions -xJf' "$W/tar.log"
    # names as tar -C DIR . writes them, with a leading ./
    make_pkg 2026.10.6 "$(fsha 2026.10.6)"
    ( cd "$W/pkg/2026.10.6-debian13-amd64" && tar -cJf "$W/rel/shellter-v2026.10.6/erssi-2026.10.6-debian13-amd64.tar.xz" ./erssi-2026.10.6-debian13-amd64 )
    ( cd "$W/rel/shellter-v2026.10.6" && sha256sum erssi-*.tar.xz ) > "$W/rel/shellter-v2026.10.6/SHA256SUMS"
    sign_sums 2026.10.6
    run_bin --ref shellter-v2026.10.6
    check "names with a leading ./: installed" [ "$RC" = 0 ] && current_is 2026.10.6
    ;;
binary-refused)
    # a package that does not verify is never installed, and nothing changes:
    # 2026.10.3 stays in use, alone
    binary_tools
    c3=$(fsha 2026.10.3) c4=$(fsha 2026.10.4)
    record 2026.10.3 "$c3"
    good_pkg 2026.10.3 "$c3"
    run_bin
    check "2026.10.3 installed" current_is 2026.10.3
    record 2026.10.3 "$c3" 2026.10.4 "$c4"
    R4=$W/rel/shellter-v2026.10.4
    refused() {   # description, the message expected
        run_bin --binary
        check "$1: refused" [ "$RC" = 1 ]
        check "... $2" grep -qF "$2" "$W/out"
        check "... nothing changed" unchanged
        check "... nothing built" no_grep 'Building' "$W/out"
    }
    sigfail="the SHA256SUMS of shellter-v2026.10.4 does not carry a valid signature by the erssi Shellter release key - refusing to install, nothing was changed"
    make_pkg 2026.10.4 "$c4"
    printf 'something else\n' > "$W/else"
    ssh-keygen -q -Y sign -n erssi-release -f "$W/key" "$W/else" 2>/dev/null && cp "$W/else.sig" "$R4/SHA256SUMS.sig"
    refused "signature of another file" "$sigfail"
    sign_sums 2026.10.4 "$W/other"
    refused "signed by another key" "$sigfail"
    sign_sums 2026.10.4 "$W/key" git
    refused "signed for git, not for releases" "$sigfail"
    sign_sums 2026.10.4 "$W/key" file
    refused "signed for another namespace" "$sigfail"
    rm -f "$R4/SHA256SUMS.sig"
    : > "$R4/SHA256SUMS.sig"
    refused "an empty signature" "$sigfail"
    # SHA256SUMS changed after signing: another tarball with its sum
    good_pkg 2026.10.4 "$c4"
    cp "$R4/SHA256SUMS.sig" "$W/good.sig"
    MF_COMMIT=$c3 make_pkg 2026.10.4 "$c4"
    cp "$W/good.sig" "$R4/SHA256SUMS.sig"
    refused "SHA256SUMS changed after signing" "$sigfail"
    # the tarball replaced, SHA256SUMS signed: the sum differs
    good_pkg 2026.10.4 "$c4"
    cp "$R4/SHA256SUMS" "$W/good.sums" && cp "$R4/SHA256SUMS.sig" "$W/good.sig"
    MF_COMMIT=$c3 make_pkg 2026.10.4 "$c4"
    cp "$W/good.sums" "$R4/SHA256SUMS" && cp "$W/good.sig" "$R4/SHA256SUMS.sig"
    refused "tarball replaced" "erssi-2026.10.4-debian13-amd64.tar.xz does not match the signed SHA256SUMS - refusing to install, nothing was changed"
    # signed packages whose MANIFEST does not say what is installed
    MF_VERSION=2026.10.5 good_pkg 2026.10.4 "$c4"
    refused "MANIFEST of another version" "says version=2026.10.5, not 2026.10.4 - refusing to install"
    MF_TAG=shellter-v2026.10.5 good_pkg 2026.10.4 "$c4"
    refused "MANIFEST of another tag" "says tag=shellter-v2026.10.5, not shellter-v2026.10.4"
    MF_PLATFORM=freebsd14-amd64 good_pkg 2026.10.4 "$c4"
    refused "MANIFEST of another platform" "says platform=freebsd14-amd64, not debian13-amd64"
    MF_COMMIT=$c3 good_pkg 2026.10.4 "$c4"
    refused "MANIFEST with another commit than recorded" "says commit=$c3, not $c4"
    MF_COMMIT='' good_pkg 2026.10.4 "$c4"
    refused "MANIFEST without a commit" "says commit=(none), not $c4"
    MF_RUNS_AS=2026.10.9 good_pkg 2026.10.4 "$c4"
    refused "an erssi of another version inside" 'the packaged erssi says "erssi 2026.10.9 (20261018 1200)", not 2026.10.4'
    MF_PREFIX='echo /usr/local' good_pkg 2026.10.4 "$c4"
    refused "an erssi that is not relocatable" 'the packaged erssi does not find its own files (--print-prefix: /usr/local)'
    MF_PREFIX='exit 1' good_pkg 2026.10.4 "$c4"
    refused "an erssi without --print-prefix" 'the packaged erssi does not find its own files (--print-prefix: nothing)'
    # a name outside the top directory
    make_pkg 2026.10.4 "$c4"
    mkdir -p "$W/pkg/evil" && : > "$W/pkg/evil/x"
    tar -C "$W/pkg" -cJf "$R4/erssi-2026.10.4-debian13-amd64.tar.xz" 2026.10.4-debian13-amd64 evil
    ( cd "$R4" && sha256sum erssi-*.tar.xz ) > "$R4/SHA256SUMS" && sign_sums 2026.10.4
    refused "files outside the top directory" "holds files outside erssi-2026.10.4-debian13-amd64/ - refusing to install"
    # a name that goes up from the top directory
    make_pkg 2026.10.4 "$c4"
    : > "$W/pkg/2026.10.4-debian13-amd64/evil"
    ( cd "$W/pkg/2026.10.4-debian13-amd64" &&
        tar -cJf "$R4/erssi-2026.10.4-debian13-amd64.tar.xz" --transform 's|^evil$|erssi-2026.10.4-debian13-amd64/../../evil|' \
            erssi-2026.10.4-debian13-amd64 evil 2>/dev/null )
    ( cd "$R4" && sha256sum erssi-*.tar.xz ) > "$R4/SHA256SUMS" && sign_sums 2026.10.4
    # shellcheck disable=SC2016 # run by sh
    check "... the test package holds the name" sh -c 'tar -tJf "$1" 2>/dev/null | grep -qx "erssi-2026.10.4-debian13-amd64/../../evil"' sh "$R4/erssi-2026.10.4-debian13-amd64.tar.xz"
    refused "a name going up from the top directory" "holds files outside erssi-2026.10.4-debian13-amd64/ - refusing to install"
    # links that lead out of the package
    PT="$W/pkg/2026.10.4-debian13-amd64/erssi-2026.10.4-debian13-amd64"
    make_pkg 2026.10.4 "$c4"
    ln -s /etc/passwd "$PT/share/irssi/abs"
    repack 2026.10.4
    refused "a symbolic link to an absolute path" "holds a symbolic link whose target is absolute or has a .. in it - refusing to install"
    make_pkg 2026.10.4 "$c4"
    ln -s ../../../outside "$PT/share/irssi/up"
    repack 2026.10.4
    refused "a symbolic link above the top directory" "holds a symbolic link whose target is absolute or has a .. in it"
    # a chain whose every link stays inside when read alone: d leads out
    make_pkg 2026.10.4 "$c4"
    ln -s . "$PT/share/irssi/a" && ln -s a/.. "$PT/share/irssi/b" &&
        ln -s b/.. "$PT/share/irssi/c" && ln -s c/.. "$PT/share/irssi/d"
    repack 2026.10.4
    refused "a chain of symbolic links that leads out" "holds a symbolic link whose target is absolute or has a .. in it"
    make_pkg 2026.10.4 "$c4"
    ln -s x/../y "$PT/share/irssi/xy"
    repack 2026.10.4
    refused "a symbolic link with .. inside its target" "holds a symbolic link whose target is absolute or has a .. in it"
    make_pkg 2026.10.4 "$c4"
    : > "$PT/share/irssi/h1" && ln "$PT/share/irssi/h1" "$PT/share/irssi/h2"
    repack 2026.10.4 --transform 'flags=RSh;s|^erssi-2026.10.4-debian13-amd64/share/irssi/h[12]$|etc/passwd|'
    # shellcheck disable=SC2016 # run by sh
    check "... the test package holds the link" sh -c 'tar -tvJf "$1" | grep -q " link to etc/passwd\$"' sh "$R4/erssi-2026.10.4-debian13-amd64.tar.xz"
    refused "a hard link to a file outside the package" "holds a hard link to a file outside erssi-2026.10.4-debian13-amd64/ - refusing to install"
    # a missing library: named, with the package to install
    good_pkg 2026.10.4 "$c4"
    FAKE_LDD_MISSING="libchafa.so.0 libperl.so.5.40 libunknown.so.7"; export FAKE_LDD_MISSING
    refused "missing libraries" "no binary install (--binary): the package needs libraries this system lacks"
    check "... the libraries named" grep -qF 'libraries missing: libchafa.so.0 libperl.so.5.40 libunknown.so.7' "$W/out"
    check "... the apt command" grep -qF 'install: sudo apt install libchafa0t64 libperl5.40' "$W/out"
    check "... the one without a known package" grep -qF 'the package that has libunknown.so.7 (no package known here)' "$W/out"
    unset FAKE_LDD_MISSING
    # the good package after all that: installed
    run_bin --binary
    check "the good package: installed" [ "$RC" = 0 ] && current_is 2026.10.4
    ;;
binary-switch)
    # releases side by side: current switched, previous kept for --rollback,
    # --keep N old ones
    binary_tools
    for v in 1 2 3 4 5 6; do good_pkg "2026.10.$v" "$(fsha "2026.10.$v")"; done
    record 2026.10.1 "$(fsha 2026.10.1)"
    run_bin
    check "2026.10.1 installed" current_is 2026.10.1
    record 2026.10.2 "$(fsha 2026.10.2)"
    run_bin
    check "2026.10.2: current" current_is 2026.10.2
    check "... previous is 2026.10.1" [ "$(readlink "$P/previous")" = releases/2026.10.1 ]
    # shellcheck disable=SC2012 # plain names
    check "... the old release untouched by the switch" [ "$(ls -A "$P/releases/2026.10.1" | tr '\n' ' ')" = "MANIFEST bin lib share " ]
    check "... said Updated" grep -q 'Updated: 2026.10.1 -> 2026.10.2' "$W/out"
    check "... and /upgrade" grep -qF 'in a running erssi, /upgrade loads 2026.10.2 (TLS connections reconnect once)' "$W/out"
    record 2026.10.3 "$(fsha 2026.10.3)"; run_bin
    record 2026.10.4 "$(fsha 2026.10.4)"; run_bin
    check "2026.10.4: current" current_is 2026.10.4
    check "... two old releases kept (the default)" releases_are 2026.10.2 2026.10.3 2026.10.4
    check "... previous is 2026.10.3" [ "$(readlink "$P/previous")" = releases/2026.10.3 ]
    record 2026.10.5 "$(fsha 2026.10.5)"
    run_bin --keep 1
    check "--keep 1: one old release" releases_are 2026.10.4 2026.10.5
    check "... KEEP kept" grep -qx KEEP=1 "$CONF"
    # an install interrupted after its stage was made (an older installer
    # killed): the next one removes it
    mkdir -p "$P/releases/.stage.left"
    record 2026.10.6 "$(fsha 2026.10.6)"
    run_bin
    check "next install: the stage of an interrupted one removed" releases_are 2026.10.5 2026.10.6
    run_bin --keep 0 --ref shellter-v2026.10.5
    check "--keep 0: refused" [ "$RC" = 2 ]
    check "... why" grep -qF -- '--keep must be at least 1 - a running erssi uses the release it was started from until /upgrade' "$W/out"
    check "... nothing changed" current_is 2026.10.6 && releases_are 2026.10.5 2026.10.6
    run_bin --ref shellter-v2026.10.5
    check "--keep 1, back to 2026.10.5: the release it leaves kept" current_is 2026.10.5 && releases_are 2026.10.5 2026.10.6
    check "... as previous" [ "$(readlink "$P/previous")" = releases/2026.10.6 ]
    # an installer that records only older releases (the copy an install
    # leaves in the prefix): no switch back without --ref
    record 2026.10.4 "$(fsha 2026.10.4)"
    run_bin --dry-run
    check "older recorded release, --dry-run: nothing to do" [ "$RC" = 0 ]
    check "... said so" grep -qF 'The installed 2026.10.5 is newer than shellter-v2026.10.4, the newest release this installer records - nothing was changed (erssi --update finds newer ones; --ref shellter-v2026.10.4 installs that one on purpose).' "$W/out"
    check "... no plan to switch" no_grep 'would' "$W/out"
    run_bin
    check "... without --dry-run: nothing changed" current_is 2026.10.5
    check "... nothing downloaded" no_grep 'Downloading' "$W/out"
    run_bin --ref shellter-v2026.10.4
    check "--ref names it: switched back on purpose" current_is 2026.10.4
    # the source build: the same
    rm -rf "$P"
    release_prefix 2026.10.9 "$(fsha 2026.10.9)" shellter-v2026.10.9
    sh "$INSTALLER" --repo "$W/repo" --prefix "$W/p" --no-test > "$W/out" 2>&1 < /dev/null
    check "source build of an older release: nothing changed" [ $? = 0 ] && no_grep 'Building' "$W/out"
    check "... said so" grep -qF 'The installed 2026.10.9 is newer than shellter-v2026.10.4' "$W/out"
    check "... the installed erssi kept" [ "$("$W/p/bin/erssi" --version)" = "erssi 2026.10.9 (20250101 1200)" ]
    ;;
binary-rollback-uninstall)
    binary_tools
    for v in 4 5; do good_pkg "2026.10.$v" "$(fsha "2026.10.$v")"; done
    record 2026.10.4 "$(fsha 2026.10.4)" 2026.10.5 "$(fsha 2026.10.5)"
    run_bin --ref shellter-v2026.10.4
    run_bin
    check "2026.10.5 over 2026.10.4" current_is 2026.10.5
    R="$P/share/irssi/shellter-install.sh"
    sh "$R" --rollback --dry-run > "$W/out" 2>&1
    check "--rollback --dry-run: done" [ $? = 0 ]
    check "... said what it would do" grep -qF 'would switch ~/.local/opt/erssi/current from releases/2026.10.5 to releases/2026.10.4' "$W/out"
    check "... nothing changed" current_is 2026.10.5
    sh "$R" --rollback > "$W/out" 2>&1
    check "--rollback: done" [ $? = 0 ]
    check "... 2026.10.4 in use" current_is 2026.10.4
    check "... said so" grep -q 'Rolled back: 2026.10.5 -> 2026.10.4' "$W/out"
    check "... /upgrade" grep -qF '/upgrade loads 2026.10.4' "$W/out"
    check "... previous is 2026.10.5" [ "$(readlink "$P/previous")" = releases/2026.10.5 ]
    check "... VERSION recorded" grep -qx VERSION=2026.10.4 "$CONF"
    check "... COMMIT recorded" grep -qx "COMMIT=$(fsha 2026.10.4)" "$CONF"
    check "... no lock left" none_exist "$P/.install.lock"
    sh "$R" --rollback > "$W/out" 2>&1
    check "--rollback again: back to 2026.10.5" current_is 2026.10.5
    rm -rf "$P/releases/2026.10.4"
    sh "$R" --rollback > "$W/out" 2>&1
    check "previous release gone: refused" [ $? = 1 ]
    check "... named, after error:" grep -q '^  error: no previous release to roll back to' "$W/out"
    check "... nothing changed" current_is 2026.10.5
    sh "$R" --rollback --update > "$W/out" 2>&1
    check "--rollback with --update: usage error" [ $? = 2 ]
    # --uninstall: the prefix and the link go, ~/.erssi stays
    mkdir -p "$HOME/.erssi" && : > "$HOME/.erssi/config"
    : > "$P/notes"
    sh "$R" --uninstall > "$W/out" 2>&1
    check "a file of the user's in the prefix: refused" [ $? = 1 ]
    check "... named" grep -qF 'holds notes, which erssi did not install - nothing was removed' "$W/out"
    check "... nothing removed" current_is 2026.10.5
    rm -f "$P/notes"
    sh "$R" --uninstall --dry-run > "$W/out" 2>&1
    check "--uninstall --dry-run: done" [ $? = 0 ]
    check "... said what it would do" grep -qF 'would remove ~/.local/opt/erssi and the link ~/.local/bin/erssi; ~/.erssi would stay' "$W/out"
    check "... nothing removed" current_is 2026.10.5
    sh "$R" --uninstall > "$W/out" 2>&1
    check "--uninstall: done" [ $? = 0 ]
    check "... the prefix removed" none_exist "$P"
    check "... the link removed" no_link
    check "... ~/.erssi kept" [ -f "$HOME/.erssi/config" ]
    # shellcheck disable=SC2088 # the text shown
    check "... said so" grep -qF '~/.erssi (your configuration, logs and keys)' "$W/out"
    sh "$INSTALLER" --uninstall > "$W/out" 2>&1
    check "nothing installed: refused" [ $? = 1 ]
    # a prefix that holds the home, or is shared: never removed
    mkdir -p "$HOME/share/irssi" && : > "$HOME/share/irssi/shellter-install.conf"
    sh "$INSTALLER" --uninstall --prefix "$HOME" > "$W/out" 2>&1
    check "--prefix ~: refused" [ $? = 1 ]
    check "... named" grep -q 'holds your home - nothing was removed' "$W/out"
    check "... nothing removed" [ -f "$HOME/.erssi/config" ]
    mkdir -p "$HOME/.local/share/irssi" && : > "$HOME/.local/share/irssi/shellter-install.conf"
    sh "$INSTALLER" --uninstall --prefix "$HOME/.local" > "$W/out" 2>&1
    check "--prefix ~/.local: refused" [ $? = 1 ]
    check "... named" grep -q 'is shared with other software - nothing was removed' "$W/out"
    ;;
binary-dry-run)
    # --dry-run downloads and checks, and changes nothing
    binary_tools
    c=$(fsha 2026.10.4)
    record 2026.10.4 "$c"
    good_pkg 2026.10.4 "$c"
    run_bin --dry-run
    check "--dry-run: done" [ "$RC" = 0 ]
    check "... checked the package" grep -q 'Downloading.*signed - sha256 verified' "$W/out"
    check "... said what it would do" grep -qF 'unpack erssi-2026.10.4-debian13-amd64.tar.xz into ~/.local/opt/erssi/releases/2026.10.4' "$W/out"
    check "... nothing changed" none_exist "$P" "$HOME/.local/state" "$HOME/.local/src" "$HOME/.local/bin"
    MF_VERSION=2026.10.5 good_pkg 2026.10.4 "$c"
    run_bin --dry-run
    check "--dry-run of a bad package: refused" [ "$RC" = 1 ]
    check "... nothing changed" none_exist "$P" "$HOME/.local/state"
    run_bin --dry-run --source
    check "--dry-run --source: done" [ "$RC" = 0 ]
    check "... the source plan" grep -qF 'download shellter-v2026.10.4 from' "$W/out"
    check "... no package checked" no_grep 'Downloading' "$W/out"
    check "... nothing changed" none_exist "$P" "$HOME/.local/state" "$HOME/.local/src"
    ;;
binary-update)
    # erssi --update of a binary install: the newer signed release, from its
    # package, through the installer of the installed release
    binary_tools
    c4=$(release_commit 2026.10.4); rtag shellter-v2026.10.4 "$c4"
    record 2026.10.4 "$c4"
    good_pkg 2026.10.4 "$c4"
    run_bin
    check "2026.10.4 installed" current_is 2026.10.4
    c5=$(release_commit 2026.10.5); rtag shellter-v2026.10.5 "$c5"
    good_pkg 2026.10.5 "$c5"
    # as erssi --update runs it: the installer of the release in use, with
    # the prefix erssi finds for itself (its release directory)
    up() { uv=$1; shift; sh "$P/current/share/irssi/shellter-install.sh" --update --prefix "$P/releases/$uv" "$@" > "$W/out" 2>&1 < /dev/null; RC=$?; }
    up 2026.10.4 --check
    check "--check: update available" grep -q 'Update available: 2026.10.4 -> 2026.10.5' "$W/out"
    check "... nothing changed" current_is 2026.10.4
    up 2026.10.4
    check "--update: done" [ "$RC" = 0 ]
    check "... signed tag found" grep -q 'Finding the release.*shellter-v2026.10.5 - signed by the erssi Shellter release key' "$W/out"
    check "... from the package" grep -q 'Downloading.*erssi-2026.10.5-debian13-amd64.tar.xz - signed - sha256 verified' "$W/out"
    check "... its commit is the tag's" grep -q 'Checking the package.*commit of the signed tag' "$W/out"
    check "... updated" grep -q 'Updated: 2026.10.4 -> 2026.10.5' "$W/out"
    check "... /upgrade" grep -qF '/upgrade loads 2026.10.5 (TLS connections reconnect once)' "$W/out"
    check "... 2026.10.5 in use" current_is 2026.10.5
    check "... 2026.10.4 kept for --rollback" [ "$(readlink "$P/previous")" = releases/2026.10.4 ]
    check "... COMMIT" grep -qx "COMMIT=$c5" "$CONF"
    check "... no source directory" none_exist "$HOME/.local/src/erssi"
    up 2026.10.5
    check "again: up to date" grep -q 'erssi Shellter Edition 2026.10.5 is up to date' "$W/out"
    check "... nothing downloaded" no_grep 'Downloading' "$W/out"
    # a signed package of a newer release that names another commit than
    # its signed tag: refused
    c6=$(release_commit 2026.10.6); rtag shellter-v2026.10.6 "$c6"
    MF_COMMIT=$c5 good_pkg 2026.10.6 "$c6"
    up 2026.10.5
    check "MANIFEST commit not the tag's: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "says commit=$c5, not $c6 - refusing to install" "$W/out"
    check "... nothing changed" current_is 2026.10.5
    check "... and kept" releases_are 2026.10.4 2026.10.5
    # no package of the newer release for this platform: the source build
    # takes the release found already, and says what it skipped once
    rm -rf "$W/rel/shellter-v2026.10.6"
    good_pkg 2026.10.6 "$c6" freebsd14-amd64
    c7=$(release_commit 2026.10.7); rtag shellter-v2026.10.7 "$c7" "$W/other"
    up 2026.10.5
    check "no package for the newer release: updated from source" [ "$RC" = 0 ] && grep -q 'Building' "$W/out"
    check "... 2026.10.6 built" grep -q 'Updated: 2026.10.5 -> 2026.10.6' "$W/out"
    check "... its commit" grep -qx "COMMIT=$c6" "$CONF"
    check "... the unsigned 2026.10.7 named once" [ "$(grep -c 'shellter-v2026.10.7 is not signed' "$W/out")" = 1 ]
    ;;
binary-old-keygen)
    # an ssh-keygen that cannot check SSH signatures (before OpenSSH 8.1, no
    # -Y) and a git before 2.34: no binary here, the source build as before
    # this installer had packages
    binary_tools
    c4=$(release_commit 2026.10.4); rtag shellter-v2026.10.4 "$c4"
    record 2026.10.4 "$c4"
    good_pkg 2026.10.4 "$c4"
    # shellcheck disable=SC2016 # stand-in scripts
    printf '#!/bin/sh\nfor a in "$@"; do [ "$a" != -Y ] || { echo "unknown option -- Y" >&2; echo "usage: ssh-keygen [-q] [-b bits] ..." >&2; exit 1; }; done\nexec "$TEST_W/bin/ssh-keygen" "$@"\n' | stand ssh-keygen
    # shellcheck disable=SC2016
    printf '#!/bin/sh\n[ "$1" != --version ] || { echo "git version 2.30.2"; exit 0; }\nexec "$TEST_W/bin/git" "$@"\n' | stand git
    use_tool ssh-keygen git
    run_bin
    check "old ssh-keygen: installed" [ "$RC" = 0 ]
    check "... one line why" grep -qF 'this ssh-keygen cannot check SSH signatures (OpenSSH 8.1 or newer is needed) - no binary package' "$W/out"
    check "... built from source" grep -q 'building from source instead' "$W/out" && grep -q 'Building' "$W/out"
    check "... with the warning of the source build" grep -q 'signatures cannot be checked here' "$W/out"
    check "... not called tampering" no_grep 'refusing' "$W/out"
    check "... nothing downloaded" no_grep 'Downloading.*sha256' "$W/out"
    check "... METHOD=source" grep -qx METHOD=source "$CONF"
    run_bin --binary
    check "old ssh-keygen, --binary: refused" [ "$RC" = 1 ]
    check "... named" grep -qF 'no binary install (--binary): this ssh-keygen cannot check SSH signatures' "$W/out"
    # an ssh-keygen that can: a SHA256SUMS without a line for this platform
    # is a source build before its signature counts - even a bad one
    rm -f "$W/shim/ssh-keygen" "$W/shim/git"
    rm -rf "$W/rel/shellter-v2026.10.4"
    make_pkg 2026.10.4 "$c4" freebsd14-amd64
    sign_sums 2026.10.4 "$W/other"
    run_bin
    check "no line for the platform, badly signed: built from source" [ "$RC" = 0 ]
    check "... the reason" grep -q 'no package for debian13-amd64 in shellter-v2026.10.4' "$W/out"
    check "... not refused" no_grep 'refusing' "$W/out"
    # with a line for this platform, the bad signature is refused
    make_pkg 2026.10.4 "$c4"
    sign_sums 2026.10.4 "$W/other"
    run_bin
    check "a line for the platform, badly signed: refused" [ "$RC" = 1 ]
    check "... named" grep -qF 'does not carry a valid signature by the erssi Shellter release key - refusing to install' "$W/out"
    ;;
binary-fallback)
    # no package for this system: the source build, as before
    binary_tools
    c4=$(release_commit 2026.10.4); rtag shellter-v2026.10.4 "$c4"
    record 2026.10.4 "$c4"
    good_pkg 2026.10.4 "$c4" freebsd14-amd64
    run_bin --binary
    check "--binary, no package for the platform: refused" [ "$RC" = 1 ]
    check "... named" grep -qF 'no binary install (--binary): no package for debian13-amd64 in shellter-v2026.10.4' "$W/out"
    check "... nothing built" none_exist "$HOME/.local/src/erssi" "$P/bin/erssi"
    run_bin
    check "without --binary: installed" [ "$RC" = 0 ]
    check "... the reason" grep -q 'Downloading.*no package for debian13-amd64 in shellter-v2026.10.4' "$W/out"
    check "... building from source" grep -q 'building from source instead' "$W/out"
    check "... built" grep -q 'Building' "$W/out"
    check "... signed and recorded" grep -q 'Downloading.*signed - commit recorded' "$W/out"
    check "... METHOD=source" grep -qx METHOD=source "$CONF"
    check "... a file, no link" [ -f "$P/bin/erssi" ] && [ ! -L "$P/bin/erssi" ]
    check "... no empty releases directory left" none_exist "$P/releases"
    # a package that the signed SHA256SUMS lists but that is not there (not
    # uploaded yet, a network error): no package, not tampering
    good_pkg 2026.10.4 "$c4"
    rm -f "$W/rel/shellter-v2026.10.4/erssi-2026.10.4-debian13-amd64.tar.xz"
    run_bin --binary
    check "listed package missing, --binary: refused" [ "$RC" = 1 ]
    check "... named" grep -qF 'no binary install (--binary): erssi-2026.10.4-debian13-amd64.tar.xz is listed in the signed SHA256SUMS but cannot be downloaded' "$W/out"
    check "... not called tampering" no_grep 'refusing' "$W/out"
    run_bin
    check "listed package missing: built from source" [ "$RC" = 0 ] && grep -q 'Building' "$W/out"
    check "... one line why" [ "$(grep -c 'is listed in the signed SHA256SUMS but cannot be downloaded' "$W/out")" = 1 ]
    check "... METHOD=source" grep -qx METHOD=source "$CONF"
    # no binary packages in the release at all
    rm -rf "$W/rel/shellter-v2026.10.4"
    run_bin
    check "no packages at all: built from source" grep -q 'no binary packages found for shellter-v2026.10.4' "$W/out"
    check "... installed" [ "$RC" = 0 ]
    # a package for another perl, or that needs missing libraries
    good_pkg 2026.10.4 "$c4"
    FAKE_PERL_V=5.36.0 run_bin
    check "another perl: built from source" grep -q 'the package is built for perl 5.40.1, this system has perl 5.36.0' "$W/out"
    check "... installed" [ "$RC" = 0 ] && grep -qx METHOD=source "$CONF"
    FAKE_LDD_MISSING=libotr.so.5 run_bin
    check "missing library: the apt command" grep -qF 'install: sudo apt install libotr5t64' "$W/out"
    check "... built from source" grep -q 'Building' "$W/out"
    # binary, then --source: the build replaces the link, not the release
    run_bin
    check "binary over the source build" current_is 2026.10.4
    run_bin --source
    check "--source: built" grep -q 'Building' "$W/out"
    check "... no package checked" no_grep 'sha256 verified' "$W/out"
    check "... bin/erssi a file again" [ -f "$P/bin/erssi" ] && [ ! -L "$P/bin/erssi" ]
    check "... the release tree untouched" [ "$("$P/releases/2026.10.4/bin/erssi" --version)" = "erssi 2026.10.4 (20261018 1200)" ]
    check "... USE=source kept" grep -qx USE=source "$CONF"
    run_bin
    check "next install: still from source" grep -q 'Building' "$W/out"
    run_bin --binary
    check "--binary: from the package again" current_is 2026.10.4
    check "... and USE=auto" grep -qx USE=auto "$CONF"
    # a source build whose install fails: the binary install stays usable
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\n[ "${3:-}" != install ] || exit 1\nexec "$TEST_W/stand/ninja" "$@"\n' > "$W/shim/ninja"
    run_bin --source
    check "--source, the install fails: failed" [ "$RC" = 1 ] && grep -q 'installing failed' "$W/out"
    check "... bin/erssi still the link into current" current_is 2026.10.4
    use_tool ninja
    # no ssh-keygen: no package can be checked, the source build runs
    rm -f "$W/bin/ssh-keygen"
    run_bin
    check "no ssh-keygen: built from source" grep -q 'no ssh-keygen to check the signature of a binary package' "$W/out"
    check "... installed" [ "$RC" = 0 ]
    ;;
*)
    echo "unknown case $CASE"; exit 1 ;;
esac

if [ "$FAILED" != 0 ]; then
    echo "--- last output ---"
    cat "$W/out" 2>/dev/null
fi
exit "$FAILED"
