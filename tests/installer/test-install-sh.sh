#!/bin/sh
# install.sh (the inherited installer with the package manager and sudo):
# create_symlinks for the local install to ~/.local, where the binary is
# ~/.local/bin/erssi itself, with a temporary HOME. Nothing is built; the
# function is run with install.sh sourced in bash.
# Usage: test-install-sh.sh SOURCE_ROOT
# Exit: 0 pass, 1 fail, 77 skip.

if [ -z "${INSTALL_SH_TEST_CLEAN:-}" ]; then
    exec env -i INSTALL_SH_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

src=$1
command -v bash >/dev/null 2>&1 || { echo "skip: no bash"; exit 77; }
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
failed=0
pass() { echo "ok $1"; }
fail() { echo "FAIL $1"; failed=1; }
check() {   # description command...
    d=$1; shift
    if "$@"; then pass "$d"; else fail "$d"; fi
}

HOME=$tmp/home; export HOME
real_ln=$(command -v ln) || exit 1
# an ln -sf that removes the target first, like the BSDs' and macOS's
# (GNU ln refuses to link a file to itself)
mkdir -p "$tmp/bsd" || exit 1
cat > "$tmp/bsd/ln" <<EOF
#!/bin/sh
[ "\$1" = -sf ] && rm -f "\$3"
exec "$real_ln" "\$@"
EOF
chmod 755 "$tmp/bsd/ln"

# local_install [DIR]: the binary installed to ~/.local, then create_symlinks
# with DIR first in PATH (without DIR: the system's ln)
local_install() {
    rm -rf "$HOME"
    mkdir -p "$HOME/.local/bin"
    printf '#!/bin/sh\necho erssi\n' > "$HOME/.local/bin/erssi"
    chmod 755 "$HOME/.local/bin/erssi"
    # shellcheck disable=SC2016 # run by bash
    PATH="${1:+$1:}$PATH" bash -c '. "$1/install.sh"; install_path=$HOME/.local; create_symlinks' \
        bash "$src" > "$tmp/out" 2>&1
    RC=$?
}
# shellcheck disable=SC2317 # called through check
is_binary() { [ -f "$HOME/.local/bin/erssi" ] && [ ! -L "$HOME/.local/bin/erssi" ] &&
    [ "$("$HOME/.local/bin/erssi")" = erssi ]; }
# shellcheck disable=SC2317 # called through check
no_failure() { ! grep -q 'Failed' "$tmp/out"; }

local_install
check "local install: done" [ "$RC" = 0 ]
check "... the binary kept" is_binary
check "... no failure reported" no_failure
local_install "$tmp/bsd"
check "local install, an ln that removes the target: done" [ "$RC" = 0 ]
check "... the binary kept" is_binary
check "... no failure reported" no_failure

if [ "$failed" != 0 ]; then
    echo "--- last output ---"
    cat "$tmp/out"
fi
exit "$failed"
