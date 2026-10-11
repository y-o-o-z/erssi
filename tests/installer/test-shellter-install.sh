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

# stand-in tool $1 from stdin, kept in $W/stand; use_tool puts it in PATH
stand() { cat > "$W/stand/$1" && chmod 755 "$W/stand/$1"; }
use_tool() { for t in "$@"; do cp "$W/stand/$t" "$W/shim/$t"; done; }

setup() {
    W=$(mktemp -d) || exit 1
    trap 'rm -rf "$W"' EXIT
    trap 'exit 1' HUP INT TERM
    TEST_W=$W; export TEST_W
    HOME=$W/home; export HOME
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
    printf '#!/bin/sh\nexit 0\n' | stand perl
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
    use_tool pkg-config cc perl meson ninja locale
    PATH=$W/shim:$W/bin; export PATH
    make_repo
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

# shellcheck disable=SC2317 # called through check
ascii_only() { [ "$(LC_ALL=C tr -d '\000-\177' < "$1" | wc -c)" -eq 0 ]; }

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
*)
    echo "unknown case $CASE"; exit 1 ;;
esac

if [ "$FAILED" != 0 ]; then
    echo "--- last output ---"
    cat "$W/out" 2>/dev/null
fi
exit "$FAILED"
