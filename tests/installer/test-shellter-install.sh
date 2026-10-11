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
# shellcheck disable=SC2317
no_grep() { ! grep -q "$1" "$2"; }
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
*)
    echo "unknown case $CASE"; exit 1 ;;
esac

if [ "$FAILED" != 0 ]; then
    echo "--- last output ---"
    cat "$W/out" 2>/dev/null
fi
exit "$FAILED"
