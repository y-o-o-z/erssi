#!/bin/sh
# shellter-install.sh - install and update erssi Shellter Edition without root.
#
#   curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
#   sh shellter-install.sh [--prefix DIR] [--src DIR] [--ref REF] [--repo URL]
#                          [--no-test] [--clean]
#   erssi --update          (= this installer, installed with erssi: --update)
#   erssi --check-update    (= --update --check)
#
# Installs to ~/.local/opt/erssi and links ~/.local/bin/erssi. Missing build
# tools (meson, ninja) are installed into a private Python venv; missing
# libraries are listed with the command for your system.
#
# Install: without --ref the newest release recorded below, checked against
# the recorded commit, so neither a moved nor a newly pushed tag changes what
# gets installed; a release marked "signed" must also carry a valid SSH
# signature by the key below (git verify-tag; needs git 2.34+ and
# ssh-keygen). --ref main (or any other ref) is built as is, with a warning.
#
# Update (--update): the newest release on the repository that carries a
# valid signature by the key below - the key of the installed copy, so a
# release pushed by anyone else is skipped. Nothing is built when the
# installed erssi is that version already; --check only reports. The
# settings of the install (source directory, repository, tests, clean) come
# from <prefix>/share/irssi/shellter-install.conf; an erssi kept up to date
# by another tool names it there as UPDATE_COMMAND, which --update runs.
#
# Build output goes to a log (~/.local/state/erssi/install.log), shown when a
# step fails. Colours only on a terminal (NO_COLOR turns them off).
# Everything runs from main() at the end of the file, so a download cut
# short by the network does not run half a script.
set -eu

# release tag -> commit it must point to [signed] (one line per release;
# "signed": the tag must be signed by RELEASE_SIGNER)
RELEASES="
shellter-v1.3.2 05a1ad752e6c701cb20e51be196ce880745008c4
shellter-v1.3.3 e5f939902710b3a6f58218bb5a4acd3eec9080bf
shellter-v1.3.4 eba342ff9c4b3eb0fd0d557fe0865b819e3c1fa6
shellter-v1.3.5 cc6eac9cd09dfb088c5cc59978809fa0b7e83acb
shellter-v1.3.6 6c92e621bba125ac1a9c7d050d769ee8b92d1de9
shellter-v1.3.7 0c74bd9925fe366f598957f9012a83aa997056f9
shellter-v1.3.8 f0d20f32f5c1e62d576c2418e7e7662d754f6bd7 signed
shellter-v1.3.9 13bc3163ab794a1a025623c90e023cb6613a567d signed
"
# who signs the releases (git allowed_signers format; utils/allowed_signers).
# A new key also changes the fingerprint checked in signed_tag().
RELEASE_SIGNER='136268860+y-o-o-z@users.noreply.github.com namespaces="git" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJmzPHTiL6dPEntggsVu9kJyAsY9KLaKNUW5kejyJOYS'
MESON_PIP="meson==1.12.1"
NINJA_PIP="ninja==1.13.2"

usage() {
    cat <<'USAGE'
erssi Shellter Edition - install and update

  curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
  sh shellter-install.sh [options]
  erssi --update | erssi --check-update

  --update       update to the newest signed release (nothing to do when
                 the installed erssi is that version)
  --check        with --update: only report whether an update is available
  --prefix DIR   install here (default ~/.local/opt/erssi)
  --src DIR      source checkout used for building (default ~/.local/src/erssi)
  --ref REF      release tag, branch or full commit SHA (default: newest
                 verified release)
  --repo URL     git repository (default https://github.com/y-o-o-z/erssi.git)
  --no-test      skip the test suite
  --clean        remove the build directory afterwards (small disk quotas)
  --yes          as root: install the missing packages without asking

As root the installer lists the system packages erssi needs and installs
them after you confirm; on a user account it lists them with the command
for the administrator.
USAGE
}

# an option that takes a value: present, not empty, not the next option
optval() {
    case "$2" in
        ''|-*) echo "shellter-install.sh: $1 needs a value, e.g. $1 $3 (--help)" >&2; exit 2 ;;
    esac
}

# ── output ───────────────────────────────────────────────────────────
# One line per step: "  [3/6] Building ........ ✓ 2m 04s". On a terminal
# the line shows a spinner and the time while the step runs.
ui_init() {
    TTY=0
    if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
        TTY=1
        B=$(printf '\033[1m'); D=$(printf '\033[2m'); A=$(printf '\033[38;5;214m')
        R=$(printf '\033[38;5;203m'); G=$(printf '\033[38;5;71m'); N=$(printf '\033[0m')
        CR=$(printf '\r\033[K')
    else
        B='' D='' A='' R='' G='' N='' CR=''
    fi
    STEP=0; STEPS=6; IN_STEP=0; STEP_TITLE=""; LOG=""
}
header() {   # what
    printf '\n  %s%serssi Shellter Edition%s %s· %s%s\n\n' "$B" "$A" "$N" "$D" "$1" "$N"
}
field() { printf '  %s%-9s%s %s\n' "$D" "$1" "$N" "$2"; }
tilde() { case "$1" in "$HOME"/*) printf '~%s' "${1#"$HOME"}" ;; *) printf '%s' "$1" ;; esac; }
now() { date +%s; }
elapsed() {  # seconds -> "47s" / "2m 04s"
    if [ "$1" -lt 60 ]; then printf '%ds' "$1"; else printf '%dm %02ds' $(($1 / 60)) $(($1 % 60)); fi
}
step_line() {   # status text
    printf '%s  %s[%d/%d]%s %-20s %s' "$CR" "$D" "$STEP" "$STEPS" "$N" "$STEP_TITLE" "$1"
}
step() {   # title
    STEP=$((STEP + 1)); STEP_TITLE=$1; STEP_T0=$(now); IN_STEP=1
    [ "$TTY" = 1 ] && step_line "${D}…${N}"
    return 0
}
done_() {   # detail
    step_line "${G}✓${N} ${D}$1${N}"; printf '\n'; IN_STEP=0
}
skip_() {   # detail
    step_line "${D}– $1${N}"; printf '\n'; IN_STEP=0
}
warn() {   # shown under the step it belongs to
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then printf '%s' "$CR"; fi
    printf '        %s!%s %s\n' "$A" "$N" "$*"
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then step_line "${D}…${N}"; fi
    return 0
}
die() {
    if [ "$IN_STEP" = 1 ]; then step_line "${R}✗${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n' "$R" "$N" "$*" >&2
    exit 1
}
die_log() {   # message - with the end of the build log
    if [ "$IN_STEP" = 1 ]; then step_line "${R}✗${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n\n' "$R" "$N" "$1" >&2
    tail -n 15 "$LOG" | sed 's/^/    /' >&2
    printf '\n  %sfull log: %s%s\n' "$D" "$LOG" "$N" >&2
    exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }

# command >> log, with a spinner and the time on a terminal; returns its code
run_logged() {
    printf '\n$ %s\n' "$*" >> "$LOG"
    if [ "$TTY" = 0 ]; then
        "$@" >> "$LOG" 2>&1
        return
    fi
    "$@" >> "$LOG" 2>&1 &
    JOB=$!
    i=0
    while kill -0 "$JOB" 2>/dev/null; do
        case $((i % 10)) in
            0) s='⠋' ;; 1) s='⠙' ;; 2) s='⠹' ;; 3) s='⠸' ;; 4) s='⠼' ;;
            5) s='⠴' ;; 6) s='⠦' ;; 7) s='⠧' ;; 8) s='⠇' ;; *) s='⠏' ;;
        esac
        step_line "$A$s$N $D$(elapsed $(($(now) - STEP_T0)))$N"
        i=$((i + 1))
        sleep 1
    done
    rc=0; wait "$JOB" || rc=$?
    JOB=""
    return "$rc"
}

# ── versions and signatures ──────────────────────────────────────────
# "1.3.10" vs "1.3.9": 0 when $1 is newer than $2
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, ".")
        for (i = 1; i <= (n > m ? n : m); i++) {
            if ((x[i] + 0) > (y[i] + 0)) exit 0
            if ((x[i] + 0) < (y[i] + 0)) exit 1
        }
        exit 1 }'
}
# signatures can be checked here: git 2.34+ and ssh-keygen
can_verify() {
    gv=$(git --version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
    # shellcheck disable=SC2086 # "2 47" -> $1 $2
    set -- $gv
    [ -n "${2:-}" ] && { [ "$1" -gt 2 ] || { [ "$1" -eq 2 ] && [ "$2" -ge 34 ]; }; } && have ssh-keygen
}
# FETCH_HEAD in repository $1 is the annotated tag $2 with an SSH signature
# by RELEASE_SIGNER. FETCH_HEAD is read once: the tag object checked is the
# one built (TAG_OBJ). gpg and gpgsm are disabled - git picks the verifier
# from the signature, and an OpenPGP signature by any key in the user's
# keyring would otherwise pass.
signed_tag() {
    TAG_OBJ=$(git -C "$1" rev-parse --verify -q FETCH_HEAD) || return 1
    [ "$(git -C "$1" cat-file -t "$TAG_OBJ" 2>/dev/null)" = tag ] || return 1
    [ "$(git -C "$1" cat-file tag "$TAG_OBJ" | sed -n '1,/^$/s/^tag //p')" = "$2" ] || return 1
    signers=$(mktemp)
    printf '%s\n' "$RELEASE_SIGNER" > "$signers"
    if git -C "$1" -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$signers" \
            -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false \
            verify-tag --raw "$TAG_OBJ" 2>&1 |
            # the fingerprint of RELEASE_SIGNER (ssh-keygen -lf)
            grep -q '^Good "git" signature for 136268860+y-o-o-z@users.noreply.github.com with ED25519 key SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw$'; then
        rm -f "$signers"; return 0
    fi
    rm -f "$signers"; return 1
}
fetch_ref() {   # repository-dir ref
    git -C "$1" -c advice.detachedHead=false fetch -q --depth 1 -- "$REPO" "$2" >> "${LOG:-/dev/null}" 2>&1
}
# the newest shellter-vX.Y.Z on $REPO newer than $2 with a valid signature,
# checked in the repository $1; unsigned ones are named and skipped. Prints
# the tag; none newer than $2: prints nothing (an up to date install).
newest_signed() {
    refs=$(git ls-remote --tags --refs -- "$REPO" 'shellter-v*' 2>>"${LOG:-/dev/null}") || return 2
    tags=$(printf '%s\n' "$refs" |
        sed -n 's|.*refs/tags/\(shellter-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$|\1|p' |
        sed 's/^shellter-v//' | sort -t. -k1,1nr -k2,2nr -k3,3nr)
    [ -n "$tags" ] || return 1
    tries=0
    for v in $tags; do
        newer "$v" "$2" || return 0
        if fetch_ref "$1" "shellter-v$v" && signed_tag "$1" "shellter-v$v"; then
            echo "shellter-v$v $TAG_OBJ"; return 0
        fi
        warn "shellter-v$v is not signed by the erssi Shellter release key - skipped" >&2
        tries=$((tries + 1))
        [ "$tries" -lt 5 ] || { warn "five unsigned newer tags - giving up (report it)" >&2; return 1; }
    done
    return 1
}
conf_get() {   # key -> value from shellter-install.conf (never run as shell)
    [ -f "$CONF" ] && sed -n "s/^$1=//p" "$CONF" | tail -n 1
    return 0
}

main() {
PREFIX="$HOME/.local/opt/erssi"
SRC="" REF="" REPO="" RUN_TESTS="" CLEAN="" UPDATE=0 CHECK=0 YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) optval "$1" "${2:-}" DIR; PREFIX=$2; shift 2 ;;
        --src) optval "$1" "${2:-}" DIR; SRC=$2; shift 2 ;;
        --ref) optval "$1" "${2:-}" TAG; REF=$2; shift 2 ;;
        --repo) optval "$1" "${2:-}" URL; REPO=$2; shift 2 ;;
        --no-test) RUN_TESTS=0; shift ;;
        --clean) CLEAN=1; shift ;;
        --update) UPDATE=1; shift ;;
        --check) CHECK=1; shift ;;
        --yes|-y) YES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "shellter-install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
done
[ "$CHECK" = 0 ] || [ "$UPDATE" = 1 ] || { echo "shellter-install.sh: --check goes with --update (--help)" >&2; exit 2; }
[ "$UPDATE" = 0 ] || [ -z "$REF" ] || { echo "shellter-install.sh: --update takes the newest signed release; for a given one use --ref without --update" >&2; exit 2; }

ui_init
JOB=""
# Ctrl-C: stop the step running in the background too (Python and meson
# started from a script ignore SIGINT), then leave
trap 'if [ -n "$JOB" ]; then kill -TERM "$JOB" 2>/dev/null; wait "$JOB" 2>/dev/null; fi; [ "$IN_STEP" = 1 ] && printf "\n"; printf "\n  Interrupted.\n\n" >&2; exit 130' HUP INT TERM
owner() { ls -ldn "$1" 2>/dev/null | awk '{print $3}'; }
ROOT=0
[ "$(id -u)" = 0 ] && ROOT=1
if [ "$ROOT" = 1 ]; then
    # su without -, sudo -E: HOME of a user, whose files root would run
    [ "$(owner "$HOME")" = 0 ] ||
        die "running as root with the HOME of another user ($HOME) - use sudo -i or su - (root's own home), or run the installer as that user"
    for d in "$PREFIX" "${SRC:-}"; do
        [ -z "$d" ] || [ ! -e "$d" ] || [ "$(owner "$d")" = 0 ] || die "$d belongs to another user - as root, use a directory of root's"
    done
fi
# the settings of the previous install, unless given again
CONF="$PREFIX/share/irssi/shellter-install.conf"
[ -n "$SRC" ] || SRC=$(conf_get SRC)
[ -n "$REPO" ] || REPO=$(conf_get REPO)
[ -n "$RUN_TESTS" ] || RUN_TESTS=$(conf_get TESTS)
[ -n "$CLEAN" ] || CLEAN=$(conf_get CLEAN)
: "${SRC:=$HOME/.local/src/erssi}" "${REPO:=https://github.com/y-o-o-z/erssi.git}" "${RUN_TESTS:=1}" "${CLEAN:=0}"
for v in "$SRC" "$REPO" "$PREFIX"; do
    case "$v" in
        -*|*"
"*) die "invalid directory or repository: $v" ;;
    esac
done

INSTALLED=""
[ -x "$PREFIX/bin/erssi" ] && INSTALLED=$("$PREFIX/bin/erssi" --version 2>/dev/null | awk '{print $2}')

if [ "$UPDATE" = 1 ]; then
    OTHER=$(conf_get UPDATE_COMMAND)
    if [ -n "$OTHER" ]; then
        header "update"
        field "updater" "$OTHER"
        if [ "$CHECK" = 1 ]; then
            printf '\n  This erssi is updated with: %s\n\n' "$OTHER"
            exit 0
        fi
        printf '\n'
        exec sh -c "$OTHER"
    fi
    [ -n "$INSTALLED" ] || die "no erssi in $PREFIX to update - install it first: curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh"
fi

# root: never a user's XDG_STATE_HOME (su without -)
if [ "$ROOT" = 1 ]; then STATE="$HOME/.local/state/erssi"; else STATE="${XDG_STATE_HOME:-$HOME/.local/state}/erssi"; fi
mkdir -p "$STATE"
if [ "$CHECK" = 1 ]; then
    LOG=$(mktemp)           # --check keeps the log of the last install
    trap 'rm -f "$LOG"' EXIT
else
    LOG="$STATE/install.log"
    rm -f "$LOG"
    ( umask 077; : > "$LOG" )
    mkdir -p "$PREFIX" 2>/dev/null; [ -w "$PREFIX" ] || die "cannot write to $PREFIX"
fi
T_START=$(now)

if [ "$UPDATE" = 1 ]; then header "update"; else header "install"; fi
field "prefix" "$(tilde "$PREFIX")"
field "source" "$(tilde "$SRC")"
[ -n "$INSTALLED" ] && field "installed" "$INSTALLED"
printf '\n'

# ── 1. the system ────────────────────────────────────────────────────
# What erssi needs, as features; each maps to the package names of the
# system's package manager. As root the missing packages are listed and,
# after a yes (or --yes), installed; on a user account the list and the
# command for the administrator are shown.
OS=$(uname -s)
OS_RELEASE=${SHELLTER_OS_RELEASE:-/etc/os-release}
DISTRO=""
# ID and ID_LIKE only, read as text (the file is not run)
[ -r "$OS_RELEASE" ] && DISTRO=$(sed -n -e 's/^ID=//p' -e 's/^ID_LIKE=//p' "$OS_RELEASE" | tr -d '"' | tr '\n' ' ')
PM=""
case "$OS $DISTRO" in
    FreeBSD*) PM=pkg ;;
    *debian*|*ubuntu*) PM=apt ;;
    *fedora*|*rhel*|*centos*|*rocky*|*alma*) PM=dnf ;;
    *alpine*) PM=apk ;;
    *arch*|*manjaro*) PM=pacman ;;
    *suse*) PM=zypper ;;
esac
packages() {   # feature -> package names for $PM
    case "$PM:$1" in
        *:git) echo git ;;
        apt:cc|dnf:cc|pacman:cc|zypper:cc) echo gcc ;;
        apk:cc) echo build-base ;;
        pkg:cc) echo "" ;;
        apt:pkgconf|apk:pkgconf|pacman:pkgconf|pkg:pkgconf) echo pkgconf ;;
        dnf:pkgconf) echo pkgconf-pkg-config ;;
        zypper:pkgconf) echo pkg-config ;;
        *:meson) echo meson ;;
        apt:ninja|dnf:ninja) echo ninja-build ;;
        *:ninja) echo ninja ;;
        apt:glib) echo libglib2.0-dev ;;
        dnf:glib|zypper:glib) echo glib2-devel ;;
        apk:glib) echo glib-dev ;;
        pacman:glib) echo glib2 ;;
        pkg:glib) echo glib ;;
        apt:openssl) echo libssl-dev ;;
        dnf:openssl) echo openssl-devel ;;
        apk:openssl) echo openssl-dev ;;
        zypper:openssl) echo libopenssl-devel ;;
        pacman:openssl|pkg:openssl) echo openssl ;;
        pkg:perl|pkg:perlembed) echo perl5 ;;
        *:perl) echo perl ;;
        apt:perlembed) echo libperl-dev ;;
        dnf:perlembed) echo "perl-devel perl-ExtUtils-Embed" ;;
        apk:perlembed) echo perl-dev ;;
        pacman:perlembed|zypper:perlembed) echo perl ;;
        apt:utf8proc) echo libutf8proc-dev ;;
        dnf:utf8proc|zypper:utf8proc) echo utf8proc-devel ;;
        apk:utf8proc) echo utf8proc-dev ;;
        pacman:utf8proc) echo libutf8proc ;;
        pkg:utf8proc) echo utf8proc ;;
        apt:otr) echo "libotr5-dev libgcrypt20-dev" ;;
        dnf:otr|zypper:otr) echo "libotr-devel libgcrypt-devel" ;;
        apk:otr) echo "libotr-dev libgcrypt-dev" ;;
        pacman:otr|pkg:otr) echo "libotr libgcrypt" ;;
        apt:preview) echo "libcurl4-openssl-dev libchafa-dev" ;;
        dnf:preview|zypper:preview) echo "libcurl-devel chafa-devel" ;;
        apk:preview) echo "curl-dev chafa-dev" ;;
        pacman:preview|pkg:preview) echo "curl chafa" ;;
    esac
}
pm_install_cmd() {   # the command that installs "$@" with $PM
    case "$PM" in
        apt) echo "apt-get install -y -q --no-install-recommends $*" ;;
        dnf) echo "dnf install -y $*" ;;
        apk) echo "apk add --no-cache $*" ;;
        pacman) echo "pacman -S --needed --noconfirm $*" ;;
        zypper) echo "zypper --non-interactive install $*" ;;
        pkg) echo "pkg install -y $*" ;;
    esac
}
pm_hint() {   # what a user tells the administrator
    case "$PM" in
        apt) echo "sudo apt install $*" ;;
        pkg) echo "pkg install $*   (as root)" ;;
        "") echo "install the development files of: $*" ;;
        *) echo "sudo $(pm_install_cmd "$@" | sed 's/ -y//; s/ --noconfirm//; s/ --non-interactive//')" ;;
    esac
}
# missing features -> $REQ (needed) and $REC (recommended)
check_system() {
    REQ="" REC=""
    have git || REQ="$REQ git"
    have cc || have gcc || have clang || REQ="$REQ cc"
    PKGCONFIG=""
    for t in pkg-config pkgconf; do have "$t" && { PKGCONFIG=$t; break; }; done
    if [ -z "$PKGCONFIG" ]; then
        REQ="$REQ pkgconf glib openssl"
    else
        $PKGCONFIG --exists 'glib-2.0 >= 2.32' && $PKGCONFIG --exists gmodule-2.0 && $PKGCONFIG --exists gio-2.0 || REQ="$REQ glib"
        $PKGCONFIG --exists openssl || REQ="$REQ openssl"
    fi
    have perl || REQ="$REQ perl"
    if ! have meson || ! have ninja; then
        # as root from the packages; on an account into a private venv
        if [ "$ROOT" = 1 ] || ! have python3; then
            have meson || REQ="$REQ meson"
            have ninja || REQ="$REQ ninja"
        fi
    fi
    perl -MExtUtils::Embed -e1 >/dev/null 2>&1 || REC="$REC perlembed"
    if [ -n "$PKGCONFIG" ]; then
        $PKGCONFIG --exists libutf8proc || [ -e /usr/include/utf8proc.h ] || [ -e /usr/local/include/utf8proc.h ] || REC="$REC utf8proc"
        $PKGCONFIG --exists 'libotr libgcrypt' || REC="$REC otr"
        $PKGCONFIG --exists 'libcurl chafa' || REC="$REC preview"
    fi
}
names() {   # features -> package names (once each)
    for f in "$@"; do packages "$f"; done | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ' | sed 's/ $//'
}
what() {   # recommended feature -> what it gives
    case "$1" in
        perlembed) echo "Perl scripts" ;; utf8proc) echo "emoji and wide characters" ;;
        otr) echo "OTR" ;; preview) echo "image preview" ;;
    esac
}
if [ "$CHECK" = 1 ]; then
    STEPS=1
else
    step "Checking the system"
    check_system
    # shellcheck disable=SC2086 # feature lists
    NEED=$(names $REQ) WANT=$(names $REC)
    if [ "$ROOT" = 1 ] && [ -n "$PM" ] && [ -n "$NEED$WANT" ]; then
        step_line "${A}!${N} ${D}packages needed${N}"; printf '\n'; IN_STEP=0
        printf '\n        To build erssi, these packages will be installed (%s):\n' "$PM"
        [ -n "$NEED" ] && printf '          %srequired%s      %s\n' "$B" "$N" "$NEED"
        [ -n "$WANT" ] && printf '          %srecommended%s   %s\n' "$B" "$N" "$WANT"
        printf '\n'
        if [ "$YES" = 1 ]; then
            answer=y
        elif [ -t 0 ] || { [ -r /dev/tty ] && (: < /dev/tty) 2>/dev/null; }; then
            printf '        Install them now? [y/N] '
            if [ -t 0 ]; then read -r answer || answer=""; else read -r answer < /dev/tty || answer=""; fi
        elif [ -z "$NEED" ]; then
            answer=n
        else
            die "packages are missing - run the installer on a terminal to confirm, or add --yes"
        fi
        case "$answer" in
            y|Y|yes|YES) ;;
            *)  if [ -n "$NEED" ]; then
                    printf '\n  Installation cancelled - nothing was installed.\n\n'; exit 1
                fi
                WANT=""
                printf '\n'; warn "continuing without the recommended packages"
                ;;
        esac
        if [ -n "$NEED$WANT" ]; then
        STEP=$((STEP - 1)); step "Installing packages"
        if [ "$PM" = apt ]; then run_logged apt-get update -q || die_log "apt-get update failed"; fi
        # shellcheck disable=SC2046 # the command and its package names
        run_logged env DEBIAN_FRONTEND=noninteractive $(pm_install_cmd $NEED $WANT) || die_log "installing the packages failed"
        hash -r 2>/dev/null || true
        check_system
        # shellcheck disable=SC2086
        still=$(names $REQ)
        [ -z "$still" ] || die "still missing after installing the packages: $still"
        done_ "installed"
        STEP=$((STEP - 1)); step "Checking the system"
        else
            STEP=$((STEP - 1)); step "Checking the system"
        fi
    elif [ -n "$NEED" ] || { [ -n "$REQ" ] && [ -z "$PM" ]; }; then
        # shellcheck disable=SC2086
        die "erssi needs packages that only the administrator can install:
      ${NEED:-$REQ}
    $(pm_hint ${NEED:-$REQ})${WANT:+
  recommended as well: $WANT}"
    fi
    if [ -n "$WANT" ] && [ "$ROOT" = 0 ]; then
        for f in $REC; do warn "$(what "$f") not available - recommended: $(names "$f")"; done
    fi
    MESON=meson; NINJA=ninja
    if ! have meson || ! have ninja; then
        PY=""
        for p in python3 python3.14 python3.13 python3.12 python3.11 python3.10; do have "$p" && { PY=$p; break; }; done
        [ -n "$PY" ] || die "meson and ninja are missing and so is Python 3: $(pm_hint "$(names meson ninja)")"
        VENV="$SRC.venv"
        if [ ! -x "$VENV/bin/meson" ] || [ ! -x "$VENV/bin/ninja" ]; then
            warn "meson/ninja not found - installing $MESON_PIP and $NINJA_PIP into $VENV (no root needed)"
            run_logged "$PY" -m venv "$VENV" || die_log "$PY -m venv failed ($(pm_hint python3-venv))"
            run_logged "$VENV/bin/pip" install -q "$MESON_PIP" "$NINJA_PIP" || die_log "pip could not install meson and ninja"
        fi
        PATH="$VENV/bin:$PATH"; export PATH
    fi
    done_ "meson $($MESON --version) · ninja $($NINJA --version) · glib $($PKGCONFIG --modversion glib-2.0) · openssl $($PKGCONFIG --modversion openssl)"
fi

# ── 2. the release ───────────────────────────────────────────────────
# only this ref, without history: a few MB instead of the whole repository.
# Fetching by name works for tags, branches and full commit SHAs alike
# (git clone --branch takes no SHA), so a fresh checkout is git init + fetch.
if [ "$CHECK" = 1 ]; then
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK" "$LOG"' EXIT
    git init -q "$WORK"
else
    WORK=$SRC
    # one installer at a time on this source directory
    mkdir -p "$(dirname "$SRC")"
    mkdir "$SRC.lock" 2>/dev/null || die "another installer is using $SRC (if not, remove $SRC.lock)"
    trap 'rmdir "$SRC.lock" 2>/dev/null' EXIT
    if [ -d "$SRC/.git" ]; then
        [ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] ||
            die "$SRC has local changes - the installer would overwrite them; use another --src"
    else
        # checkout --force must not land on somebody's files
        [ ! -e "$SRC" ] || [ -z "$(ls -A "$SRC" 2>/dev/null)" ] ||
            die "$SRC exists and is not a git checkout - remove it or use another --src"
        mkdir -p "$SRC"
        git -c init.defaultBranch=main init -q "$SRC" || die "cannot create a git repository in $SRC"
        NEW_SRC=1
    fi
fi
if [ "$UPDATE" = 1 ]; then
    step "Finding the release"
    can_verify || die "release signatures cannot be checked here (git 2.34+ and ssh-keygen are needed) - update with an explicit release instead: sh shellter-install.sh --ref shellter-vX.Y.Z"
    rc=0; FOUND=$(newest_signed "$WORK" "$INSTALLED") || rc=$?
    [ "$rc" != 2 ] || die "cannot reach $REPO"
    [ "$rc" = 0 ] || die "no signed release newer than $INSTALLED found on $REPO"
    REF=${FOUND%% *}; TAG_OBJ=${FOUND#* }
    if [ -n "$REF" ]; then
        NEWEST=${REF#shellter-v}
        done_ "$REF · signed by the erssi Shellter release key"
    else
        NEWEST=$INSTALLED
        done_ "none newer than $INSTALLED"
    fi
    if ! newer "$NEWEST" "$INSTALLED"; then
        if newer "$INSTALLED" "$NEWEST"; then
            printf '\n  The installed erssi %s is newer than the newest release (%s) - nothing to do.\n\n' "$INSTALLED" "$NEWEST"
        else
            printf '\n  %serssi Shellter Edition %s is up to date%s (the newest signed release).\n\n' "$B" "$INSTALLED" "$N"
        fi
        exit 0
    fi
    if [ "$CHECK" = 1 ]; then
        printf '\n  %sUpdate available: %s → %s%s\n  Run: erssi --update\n\n' "$B" "$INSTALLED" "$NEWEST" "$N"
        exit 0
    fi
else
    step "Choosing the release"
    if [ -z "$REF" ]; then
        # the newest release this installer knows (and can verify)
        REF=$(printf '%s\n' "$RELEASES" | awk 'NF >= 2 {r = $1} END {print r}')
        [ -n "$REF" ] || die "no release recorded in this installer - use --ref"
    fi
    done_ "$REF"
fi

# ── 3. download and verify ───────────────────────────────────────────
step "Downloading"
SHA_HINT=""
case "$REF" in
    *[!0-9a-f]*) ;;
    *) [ ${#REF} -eq 40 ] || SHA_HINT=" (a commit must be given as the full 40-character SHA)" ;;
esac
if [ "$UPDATE" = 0 ]; then
    if ! fetch_ref "$SRC" "$REF"; then
        [ -n "${NEW_SRC:-}" ] && rm -rf "$SRC/.git"
        die "cannot download $REF from $REPO$SHA_HINT"
    fi
fi
PINNED=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r {print $2}')
SIGNED=$(printf '%s\n' "$RELEASES" | awk -v r="$REF" '$1 == r && $3 == "signed" {print "yes"}')
VERIFIED=""
if [ "$UPDATE" = 1 ]; then
    VERIFIED="signed"           # checked while finding it
else
    TAG_OBJ=$(git -C "$SRC" rev-parse --verify -q FETCH_HEAD) || die "cannot download $REF from $REPO"
    if [ -n "$SIGNED" ]; then
        if can_verify; then
            signed_tag "$SRC" "$REF" ||
                die "$REF does not carry a valid signature by the erssi Shellter release key - refusing to build (report it)"
            VERIFIED="signed"
        else
            warn "signatures cannot be checked here (git 2.34+ and ssh-keygen) - the recorded commit is still checked"
        fi
    fi
fi
# exactly the object that was checked, not whatever FETCH_HEAD is by now
git -C "$SRC" -c advice.detachedHead=false checkout -q --force "$TAG_OBJ^{commit}" >> "$LOG" 2>&1 || die "cannot check out $REF"
HEAD_SHA=$(git -C "$SRC" rev-parse HEAD)
if [ -n "$PINNED" ]; then
    [ "$HEAD_SHA" = "$PINNED" ] || die "$REF points to $HEAD_SHA, but this installer expects $PINNED - refusing to build (download the installer again, or report it)"
    VERIFIED="${VERIFIED:+$VERIFIED · }commit recorded"
elif [ "$UPDATE" = 0 ]; then
    case "$REF" in
        shellter-v*) warn "$REF is not recorded in this installer - not verified; download the installer again to verify it" ;;
        *) warn "$REF is not a release - built as is, not verified" ;;
    esac
fi
done_ "$(echo "$HEAD_SHA" | cut -c1-8)${VERIFIED:+ · $VERIFIED}"

# ── 4. build ─────────────────────────────────────────────────────────
step "Building"
if [ -f "$SRC/Build/build.ninja" ]; then
    run_logged $MESON setup --reconfigure "$SRC/Build" "$SRC" -Dprefix="$PREFIX" || die_log "configuring the build failed"
else
    run_logged $MESON setup "$SRC/Build" "$SRC" -Dprefix="$PREFIX" -Dwith-proxy=yes || die_log "configuring the build failed"
fi
run_logged $NINJA -C "$SRC/Build" || die_log "the build failed"
done_ "$(elapsed $(($(now) - STEP_T0)))"

# ── 5. test ──────────────────────────────────────────────────────────
step "Testing"
if [ "$RUN_TESTS" = 1 ]; then
    run_logged $MESON test -C "$SRC/Build" || die_log "tests failed (all results: $SRC/Build/meson-logs/testlog.txt)"
    n=$(sed -n 's/^Ok: *\([0-9]*\).*/\1/p' "$LOG" | tail -n 1)
    done_ "${n:+$n tests · }$(elapsed $(($(now) - STEP_T0)))"
else
    skip_ "skipped"
fi

# ── 6. install ───────────────────────────────────────────────────────
step "Installing"
run_logged $NINJA -C "$SRC/Build" install || die_log "installing failed"
mkdir -p "$HOME/.local/bin" "$PREFIX/share/irssi"
ln -sf "$PREFIX/bin/erssi" "$HOME/.local/bin/erssi"
# erssi --update runs this installer (the copy of the installed release)
cp "$SRC/shellter-install.sh" "$PREFIX/share/irssi/shellter-install.sh" 2>/dev/null || true
{
    echo "# written by shellter-install.sh - the settings erssi --update uses"
    echo "SRC=$SRC"
    echo "REPO=$REPO"
    echo "TESTS=$RUN_TESTS"
    echo "CLEAN=$CLEAN"
} > "$CONF.tmp" && chmod 600 "$CONF.tmp" && mv "$CONF.tmp" "$CONF"
VERSION=$("$PREFIX/bin/erssi" --version | awk '{print $2}')
if [ "$CLEAN" = 1 ]; then
    rm -rf "$SRC/Build"
    done_ "$(tilde "$PREFIX") · build directory removed"
else
    done_ "$(tilde "$PREFIX")"
fi

# ── summary ──────────────────────────────────────────────────────────
IN_PATH=1
# shellcheck disable=SC2088 # shown, not run
case ":$PATH:" in
    *":$HOME/.local/bin:"*) RUN=erssi ;;
    *) RUN="~/.local/bin/erssi"; IN_PATH=0 ;;
esac
printf '\n'
if [ -n "$INSTALLED" ] && [ "$INSTALLED" != "$VERSION" ]; then
    printf '  %s%sUpdated: %s → %s%s  %s(%s)%s\n\n' "$B" "$G" "$INSTALLED" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
    field "running" "in a running erssi, /upgrade loads $VERSION without disconnecting"
else
    printf '  %s%serssi Shellter Edition %s is installed%s  %s(%s)%s\n\n' "$B" "$G" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
    field "start" "$RUN   (inside tmux, so it keeps running when you log out)"
    field "connect" "/connect IRCnet"
fi
field "update" "erssi --update"
# shellcheck disable=SC2088 # shown, not run
[ "$IN_PATH" = 1 ] || field "path" "~/.local/bin is not in PATH - add to ~/.profile: export PATH=\"\$HOME/.local/bin:\$PATH\""
field "log" "$(tilde "$LOG")"
printf '\n'
}

main "$@"
