#!/bin/sh
# shellter-install.sh - build and install erssi Shellter Edition without root.
#
#   curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
#   sh shellter-install.sh [--prefix DIR] [--src DIR] [--ref REF] [--repo URL]
#                          [--no-test] [--clean]
#
# Installs to ~/.local/opt/erssi and links ~/.local/bin/erssi. Run it again
# to update. Missing build tools (meson, ninja) are installed into a private
# Python venv; missing libraries are listed with the command for your system.
# Without --ref it installs the newest release recorded below and checks it
# against the recorded commit, so neither a moved nor a newly pushed tag
# changes what gets installed; --ref main (or any other ref) is built as is,
# with a warning. --clean removes the build directory after installing (for
# small disk quotas; the next update builds from scratch).
#
# Everything runs from main() at the end of the file, so a download cut
# short by the network does not run half a script.
set -eu

# release tag -> commit it must point to (one line per release)
RELEASES="
shellter-v1.3.2 05a1ad752e6c701cb20e51be196ce880745008c4
"
MESON_PIP="meson==1.12.1"
NINJA_PIP="ninja==1.13.2"

usage() {
    cat <<'USAGE'
erssi Shellter Edition installer

  curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
  sh shellter-install.sh [options]
  curl -fsSL .../shellter-install.sh | sh -s -- [options]

  --prefix DIR   install here (default ~/.local/opt/erssi)
  --src DIR      source checkout used for building (default ~/.local/src/erssi)
  --ref REF      release tag, branch or commit (default: newest verified release)
  --repo URL     git repository (default https://github.com/y-o-o-z/erssi.git)
  --no-test      skip the test suite
  --clean        remove the build directory afterwards (small disk quotas)
USAGE
}

main() {
PREFIX="$HOME/.local/opt/erssi"
SRC="$HOME/.local/src/erssi"
REF=""
REPO="https://github.com/y-o-o-z/erssi.git"
RUN_TESTS=1
CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --src) SRC=$2; shift 2 ;;
        --ref) REF=$2; shift 2 ;;
        --repo) REPO=$2; shift 2 ;;
        --no-test) RUN_TESTS=0; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "shellter-install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
done

if [ -t 1 ]; then B=$(printf '\033[1m'); A=$(printf '\033[38;5;214m'); R=$(printf '\033[38;5;203m'); G=$(printf '\033[38;5;71m'); N=$(printf '\033[0m'); else B= A= R= G= N=; fi
say()  { printf '%s==>%s %s\n' "$A" "$N" "$*"; }
ok()   { printf '  %s✓%s %s\n' "$G" "$N" "$*"; }
warn() { printf '  %s!%s %s\n' "$A" "$N" "$*"; }
die()  { printf '%serror:%s %s\n' "$R" "$N" "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ── the system: which package manager names to suggest ───────────────
OS=$(uname -s)
DISTRO=""
[ -r /etc/os-release ] && DISTRO=$(. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}")
hint_packages() {
    case "$OS $DISTRO" in
        *debian*|*ubuntu*)
            echo "sudo apt install git gcc meson ninja-build pkgconf libglib2.0-dev libssl-dev libperl-dev libutf8proc-dev libgcrypt20-dev libotr5-dev libcurl4-openssl-dev libchafa-dev" ;;
        FreeBSD*)
            echo "pkg install git meson ninja pkgconf glib perl5 utf8proc libgcrypt libotr curl chafa" ;;
        *)
            echo "install the development files for these pkg-config modules: glib-2.0 gmodule-2.0 gio-2.0 openssl (optional: libutf8proc libotr libgcrypt libcurl chafa, Perl with ExtUtils::Embed)" ;;
    esac
}

say "${B}erssi Shellter Edition${N} installer"

# ── build tools ──────────────────────────────────────────────────────
missing=""
for t in git cc perl; do have "$t" || missing="$missing $t"; done
PKGCONFIG=""
for t in pkg-config pkgconf; do have "$t" && { PKGCONFIG=$t; break; }; done
[ -n "$PKGCONFIG" ] || missing="$missing pkg-config"
if [ -n "$missing" ]; then
    printf '  missing:%s\n' "$missing"
    die "install them first, e.g.: $(hint_packages)"
fi
ok "git, cc, perl, $PKGCONFIG"

MESON=meson; NINJA=ninja
if ! have meson || ! have ninja; then
    PY=""
    for p in python3 python3.14 python3.13 python3.12 python3.11 python3.10; do have "$p" && { PY=$p; break; }; done
    [ -n "$PY" ] || die "meson and ninja are missing and so is Python 3: $(hint_packages)"
    VENV="$SRC.venv"
    if [ ! -x "$VENV/bin/meson" ] || [ ! -x "$VENV/bin/ninja" ]; then
        say "meson/ninja not found - installing $MESON_PIP and $NINJA_PIP into $VENV (no root needed)"
        "$PY" -m venv "$VENV" || die "$PY -m venv failed (Debian: apt install python3-venv)"
        "$VENV/bin/pip" install -q "$MESON_PIP" "$NINJA_PIP" || die "pip could not install meson and ninja"
    fi
    PATH="$VENV/bin:$PATH"; export PATH
fi
ok "meson $($MESON --version), ninja $($NINJA --version)"

# ── libraries ────────────────────────────────────────────────────────
need=""
$PKGCONFIG --exists 'glib-2.0 >= 2.32' || need="$need glib-2.0"
for m in gmodule-2.0 gio-2.0 openssl; do $PKGCONFIG --exists "$m" || need="$need $m"; done
if [ -n "$need" ]; then
    printf '  missing development files:%s\n' "$need"
    die "$(hint_packages)"
fi
ok "glib $($PKGCONFIG --modversion glib-2.0), openssl $($PKGCONFIG --modversion openssl)"

optional() {    # name, test command, what it gives
    if eval "$2" >/dev/null 2>&1; then ok "$1"; else warn "$1 not found - $3"; fi
}
optional "Perl embedding" "perl -MExtUtils::Embed -e1" "no Perl scripts"
optional "libutf8proc" "$PKGCONFIG --exists libutf8proc || [ -e /usr/include/utf8proc.h ] || [ -e /usr/local/include/utf8proc.h ]" "simpler emoji/width handling"
optional "libcurl + chafa" "$PKGCONFIG --exists 'libcurl chafa'" "no image preview"
optional "libotr + libgcrypt" "$PKGCONFIG --exists 'libotr libgcrypt'" "no OTR"

# ── source ───────────────────────────────────────────────────────────
if [ -z "$REF" ]; then
    # the newest release this installer knows (and can verify)
    REF=$(printf '%s\n' "$RELEASES" | awk 'NF == 2 {r = $1} END {print r}')
    [ -n "$REF" ] || die "no release recorded in this installer - use --ref"
fi
# only this ref, without history: a few MB instead of the whole repository
GITLOG=$(mktemp)
G() { git -c advice.detachedHead=false "$@" >"$GITLOG" 2>&1 || { cat "$GITLOG" >&2; rm -f "$GITLOG"; return 1; }; }
if [ -d "$SRC/.git" ]; then
    [ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] ||
        die "$SRC has local changes - the installer would overwrite them; use another --src"
    say "updating $SRC to $REF"
    G -C "$SRC" fetch -q --depth 1 "$REPO" "$REF" || die "cannot fetch $REF from $REPO"
    G -C "$SRC" checkout -q --force FETCH_HEAD || die "cannot check out $REF"
else
    say "downloading $REF from $REPO"
    mkdir -p "$(dirname "$SRC")"
    G clone -q --depth 1 --branch "$REF" "$REPO" "$SRC" || die "cannot download $REF from $REPO"
fi
rm -f "$GITLOG"
HEAD_SHA=$(git -C "$SRC" rev-parse HEAD)
PINNED=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r {print $2}')
if [ -n "$PINNED" ]; then
    [ "$HEAD_SHA" = "$PINNED" ] || die "$REF points to $HEAD_SHA, but this installer expects $PINNED - refusing to build (download the installer again, or report it)"
    ok "source: $REF, verified ($(echo "$HEAD_SHA" | cut -c1-8))"
else
    case "$REF" in
        shellter-v*) warn "$REF is not recorded in this installer - not verified; download the installer again to verify it" ;;
        *) warn "$REF is not a release - built as is, not verified" ;;
    esac
    ok "source: $REF ($(echo "$HEAD_SHA" | cut -c1-8))"
fi

# ── build, test, install ─────────────────────────────────────────────
say "building (a few minutes on a small shell box)"
if [ -f "$SRC/Build/build.ninja" ]; then
    $MESON setup --reconfigure "$SRC/Build" "$SRC" -Dprefix="$PREFIX" >/dev/null
else
    $MESON setup "$SRC/Build" "$SRC" -Dprefix="$PREFIX" -Dwith-proxy=yes >/dev/null
fi
$NINJA -C "$SRC/Build" >/dev/null || die "build failed - run: $NINJA -C $SRC/Build"
ok "built"
if [ "$RUN_TESTS" = 1 ]; then
    $MESON test -C "$SRC/Build" >/dev/null 2>&1 || die "tests failed - see $SRC/Build/meson-logs/testlog.txt"
    ok "tests passed"
fi
$NINJA -C "$SRC/Build" install >/dev/null || die "install failed"
mkdir -p "$HOME/.local/bin"
ln -sf "$PREFIX/bin/erssi" "$HOME/.local/bin/erssi"
ok "installed: $PREFIX ($("$PREFIX/bin/erssi" --version))"
if [ "$CLEAN" = 1 ]; then
    rm -rf "$SRC/Build"
    ok "build directory removed ($(du -sh "$SRC" "$PREFIX" 2>/dev/null | awk '{print $1}' | paste -sd+ -) used)"
fi

case ":$PATH:" in
    *":$HOME/.local/bin:"*) RUN=erssi ;;
    *) RUN="$HOME/.local/bin/erssi"
       warn "~/.local/bin is not in PATH - add: export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
esac
cat <<EOF

  ${B}Start:${N}     $RUN            (in tmux, so it keeps running when you log out)
  ${B}Connect:${N}   /connect IRCnet
  ${B}Help:${N}      /help, /help <command>
  ${B}Update:${N}    run this installer again

EOF
}

main "$@"
