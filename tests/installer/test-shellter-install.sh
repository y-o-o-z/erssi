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

# stand-in tool $1 from stdin, kept in $W/stand; use_tool puts it in PATH
stand() { cat > "$W/stand/$1" && chmod 755 "$W/stand/$1"; }
use_tool() { for t in "$@"; do cp "$W/stand/$t" "$W/shim/$t"; done; }

setup() {
    W=$(mktemp -d) && W=$(cd -P "$W" && pwd -P) || exit 1
    trap 'rm -rf "$W"' EXIT
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
    printf '#!/bin/sh\necho "erssi 9.9.9 (20261010 1200)"\n' > "$p/bin/erssi" && chmod 755 "$p/bin/erssi"
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

# no meson or ninja in PATH, but a python3 that builds a venv with them; it
# logs whether the source lock is held when it runs. Another user acting
# while it runs: FAKE_PLANT_SRC=1 or git (they make the source directory,
# empty or a git checkout)
no_meson_python() {
    rm -f "$W/shim/meson" "$W/shim/ninja"
    # shellcheck disable=SC2016 # a stand-in script
    printf '#!/bin/sh\necho pip >> "$TEST_W/ran-pip"\nexit 0\n' | stand pip
    stand python3 <<'EOF'
#!/bin/sh
if [ -d "$HOME/.local/src/erssi.lock" ]; then echo "venv locked"; else echo "venv unlocked"; fi >> "$TEST_W/py.log"
[ "$1" = -m ] && [ "$2" = venv ] || exit 0
mkdir -p "$3/bin" && cp "$TEST_W/stand/meson" "$TEST_W/stand/ninja" "$TEST_W/stand/pip" "$3/bin/"
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
# owner (foreign: 4242), mode string or group. id -un is "tester", id -gn
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
    -un) echo tester ;;
    -gn) echo "${FAKE_GROUP_NAME:-tester}" ;;
    *) exec "$TEST_W/bin/id" "$@" ;;
esac
EOF
    use_tool ls id
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

# shellcheck disable=SC2317 # called through check
ascii_only() { [ "$(LC_ALL=C tr -d '\000-\177' < "$1" | wc -c)" -eq 0 ]; }
# shellcheck disable=SC2317
no_grep() { ! grep -q "$1" "$2"; }
# shellcheck disable=SC2317
none_exist() { for f in "$@"; do [ ! -e "$f" ] && [ ! -L "$f" ] || return 1; done; }
# shellcheck disable=SC2317
no_link() { [ ! -e "$HOME/.local/bin/erssi" ] && [ ! -L "$HOME/.local/bin/erssi" ]; }

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
    # write by the user's own private group (no other members, no ACL): allowed
    no_meson_python
    another_user
    V=$HOME/.local/src/erssi.venv
    plant_venv "$V"
    chmod g+w "$V"
    FAKE_GROUP_NAME=staff; export FAKE_GROUP_NAME
    run_install
    unset FAKE_GROUP_NAME
    check "group-writable venv, shared group: refused" [ "$RC" = 1 ]
    check "... named" grep -qF "$V can be changed by other users" "$W/out"
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
    check "... named" grep -qF "$HOME/.local/src is writable by other users - choose a --src under a directory only you can write" "$W/out"
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
    ;;
*)
    echo "unknown case $CASE"; exit 1 ;;
esac

if [ "$FAILED" != 0 ]; then
    echo "--- last output ---"
    cat "$W/out" 2>/dev/null
fi
exit "$FAILED"
