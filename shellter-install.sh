#!/bin/sh
# shellter-install.sh - build and install erssi Shellter Edition without root.
#
#   curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
#   sh shellter-install.sh [--prefix DIR] [--src DIR] [--ref REF] [--repo URL] [--no-test]
#
# Installs to ~/.local/opt/erssi and links ~/.local/bin/erssi. Run it again
# to update. Missing build tools (meson, ninja) are installed into a private
# Python venv; missing libraries are listed with the command for your system.
set -eu

PREFIX="$HOME/.local/opt/erssi"
SRC="$HOME/.local/src/erssi"
REF=""
REPO="https://github.com/y-o-o-z/erssi.git"
RUN_TESTS=1

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX=$2; shift 2 ;;
        --src) SRC=$2; shift 2 ;;
        --ref) REF=$2; shift 2 ;;
        --repo) REPO=$2; shift 2 ;;
        --no-test) RUN_TESTS=0; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
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
    have python3 || die "meson and ninja are missing and so is python3: $(hint_packages)"
    VENV="$SRC.venv"
    if [ ! -x "$VENV/bin/meson" ] || [ ! -x "$VENV/bin/ninja" ]; then
        say "meson/ninja not found - installing them into $VENV (no root needed)"
        python3 -m venv "$VENV" || die "python3 -m venv failed (Debian: apt install python3-venv)"
        "$VENV/bin/pip" install -q --upgrade meson ninja || die "pip could not install meson and ninja"
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
if [ -d "$SRC/.git" ]; then
    say "updating $SRC"
    git -C "$SRC" fetch -q --tags origin
else
    say "cloning $REPO"
    mkdir -p "$(dirname "$SRC")"
    git clone -q "$REPO" "$SRC"
fi
if [ -z "$REF" ]; then
    # the newest release of this edition, else main
    REF=$(git -C "$SRC" tag -l 'shellter-v*' --sort=-version:refname | head -1)
    [ -n "$REF" ] || REF=main
fi
case "$REF" in
    main|master) git -C "$SRC" checkout -q "$REF" && git -C "$SRC" merge -q --ff-only "origin/$REF" ;;
    *) git -C "$SRC" checkout -q "$REF" ;;
esac
ok "source: $REF ($(git -C "$SRC" rev-parse --short HEAD))"

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
