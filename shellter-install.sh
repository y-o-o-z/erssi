#!/bin/sh
# shellter-install.sh - install and update erssi Shellter Edition without root.
#
#   curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
#   sh shellter-install.sh [--prefix DIR] [--src DIR] [--ref REF] [--repo URL]
#                          [--no-test|--test] [--clean|--no-clean]
#   erssi --update          (= this installer, installed with erssi: --update)
#   erssi --check-update    (= --update --check)
#
# Installs to ~/.local/opt/erssi and links ~/.local/bin/erssi. A missing
# build tool (meson, ninja) is installed into a private Python venv, as a
# ready-made wheel only; where there is none, the installer stops and names
# the package to ask for. Missing libraries are listed with the command for
# your system.
#
# Install: without --ref the newest release recorded below, checked against
# the recorded commit, so neither a moved nor a newly pushed tag changes what
# gets installed; a release marked "signed" must also carry a valid SSH
# signature by the key below (git verify-tag; needs git 2.34+ and
# ssh-keygen). --ref main (or any other ref) is built as is, with a warning.
#
# Update (--update): the newest release on the repository that carries a
# valid signature by the key below - the key of the installed copy, so a
# release pushed by anyone else is skipped - if its version is newer than
# the installed one; --check only reports. Release tags never move (a
# release made again is a new version): a signed tag of the installed
# release that points to another commit is reported, never installed. An
# erssi installed from a branch or a commit (--ref) is offered a release
# only when its version is newer. The settings of the install (what it was
# installed from, the source directory, repository, tests, clean) come from
# <prefix>/share/irssi/shellter-install.conf; an erssi kept up to date by
# another tool names it there as UPDATE_COMMAND, which --update runs.
#
# Inside a user namespace (rootless container, bwrap) root's directories,
# like those of every user the namespace does not map, show as the overflow
# uid; SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 counts it as root's above the
# parent of --src (safe_path). SHELLTER_PROC, SHELLTER_OS_RELEASE and
# SHELLTER_BASE_OPENSSL are for the tests only (a stand-in /proc,
# os-release and FreeBSD base OpenSSL).
#
# Build output goes to a log (~/.local/state/erssi/install.log), shown when a
# step fails. Colours only on a terminal (NO_COLOR turns them off).
# Everything runs from main() at the end of the file, so a download cut
# short by the network does not run half a script.
set -eu

# release tag -> commit it must point to [signed] (one line per release;
# "signed": the tag must be signed by RELEASE_SIGNER)
RELEASES="
shellter-v1.3.2 f29f28a7d5e782262232cb389a550bd5ee0ab3b6 signed
shellter-v1.3.3 af29ea3f48eb25aa56ed54a84ef9bc4484dd05f9 signed
shellter-v1.3.4 cedeaea6ed9832464b2e29e756bdd815f0ba174f signed
shellter-v1.3.5 eca79190657383b4e649518fd72942087f932f02 signed
shellter-v1.3.6 6fb0c2b6a399423aeac555a597cc2ceffa507efd signed
shellter-v1.3.7 3740d2503fa8fb435f2e49194814c03e736ed3e2 signed
shellter-v1.3.8 d97d9d96f222b501d13d86ab95e035e9d0ee5c18 signed
shellter-v1.3.9 82f691cfc28b0377e06ba4a908b74b2989a1d24f signed
shellter-v1.3.10 ae24cb24479b553a4ce3a251d03866d8e3065671 signed
shellter-v2026.10.0 644648272cecae4f6444155ad3e363970f8f982b signed
shellter-v2026.10.1 c11bb0e745b1d35a4bc7c5a697f31baf3d629303 signed
shellter-v2026.10.2 0de1442fdcb021c90882e0af3de26ccde52f2046 signed
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

  --update       update to the newest signed release when its version is
                 newer than the installed one (also for an erssi installed
                 from a branch or a commit with --ref)
  --check        with --update: only report whether an update is available
  --prefix DIR   install here (default ~/.local/opt/erssi)
  --src DIR      source checkout used for building (default ~/.local/src/erssi)
  --ref REF      release tag, branch or full commit SHA (default: newest
                 verified release)
  --repo URL     git repository (default https://github.com/y-o-o-z/erssi.git)
  --no-test      skip the test suite
  --test         run the test suite (the default)
  --clean        remove the build directory afterwards (small disk quotas)
  --no-clean     keep the build directory (the default)
  --yes          as root: install the missing packages without asking

--src, --repo, --no-test/--test and --clean/--no-clean are kept in
<prefix>/share/irssi/shellter-install.conf and used again by the next
install into the same prefix and by erssi --update, until given again.

As root the installer lists the system packages erssi needs and installs
them after you confirm; on a user account it lists them with the command
for the administrator. There a missing meson or ninja comes from pip, into
a venv next to the source directory (DIR.venv), as a ready-made wheel only:
where pip has none (ninja on FreeBSD), the package is asked for instead.

The source directory and its venv must be yours; the directories above
them yours or root's. In a user namespace (rootless container, bwrap,
unshare) root's directories, like those of every user it does not map,
show as the overflow uid (65534), which may also be a user the namespace
maps (rootless podman's nobody):
  SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1
                 count that uid as root's above the parent of --src (so
                 every unmapped user's directories there are trusted, not
                 only root's; never the parent, --src or its venv); does
                 nothing outside a user namespace or without /proc
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
# the line shows a spinner and the time while the step runs. The symbols and
# the spinner are UTF-8 only when the locale is; otherwise ASCII ("ok").
utf8_locale() {
    cm=""
    have locale && cm=$(locale charmap 2>/dev/null) || cm=""
    if [ -n "$cm" ]; then
        case "$cm" in [Uu][Tt][Ff]-8|[Uu][Tt][Ff]8) return 0 ;; esac
        return 1
    fi
    # no locale command, or it printed nothing: the locale variables
    case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
        *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) return 0 ;;
    esac
    return 1
}
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
    if utf8_locale; then
        UTF8=1 S_OK='✓' S_FAIL='✗' S_SEP='·' S_ARROW='→' S_SKIP='–' S_BUSY='…'
    else
        UTF8=0 S_OK=ok S_FAIL=failed S_SEP=- S_ARROW='->' S_SKIP=- S_BUSY=...
    fi
    STEP=0; STEPS=6; IN_STEP=0; STEP_TITLE=""; LOG=""
}
header() {   # what
    printf '\n  %s%serssi Shellter Edition%s %s%s %s%s\n\n' "$B" "$A" "$N" "$D" "$S_SEP" "$1" "$N"
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
    [ "$TTY" = 1 ] && step_line "${D}${S_BUSY}${N}"
    return 0
}
done_() {   # detail
    step_line "${G}${S_OK}${N} ${D}$1${N}"; printf '\n'; IN_STEP=0
}
skip_() {   # detail
    step_line "${D}${S_SKIP} $1${N}"; printf '\n'; IN_STEP=0
}
warn() {   # shown under the step it belongs to
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then printf '%s' "$CR"; fi
    printf '        %s!%s %s\n' "$A" "$N" "$*"
    if [ "$IN_STEP" = 1 ] && [ "$TTY" = 1 ]; then step_line "${D}${S_BUSY}${N}"; fi
    return 0
}
die() {
    if [ "$IN_STEP" = 1 ]; then step_line "${R}${S_FAIL}${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n' "$R" "$N" "$*" >&2
    exit 1
}
die_log() {   # message - with the end of the build log
    if [ "$IN_STEP" = 1 ]; then step_line "${R}${S_FAIL}${N}"; printf '\n'; IN_STEP=0; fi
    printf '\n  %serror:%s %s\n\n' "$R" "$N" "$1" >&2
    tail -n 15 "$LOG" | sed 's/^/    /' >&2
    printf '\n  %sfull log: %s%s\n' "$D" "$LOG" "$N" >&2
    exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }
spinner_frame() {   # i
    if [ "$UTF8" = 1 ]; then
        case $(($1 % 10)) in
            0) printf '⠋' ;; 1) printf '⠙' ;; 2) printf '⠹' ;; 3) printf '⠸' ;; 4) printf '⠼' ;;
            5) printf '⠴' ;; 6) printf '⠦' ;; 7) printf '⠧' ;; 8) printf '⠇' ;; *) printf '⠏' ;;
        esac
    else
        case $(($1 % 4)) in
            0) printf '|' ;; 1) printf '/' ;; 2) printf '-' ;; *) printf '\134' ;;
        esac
    fi
}

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
        step_line "$A$(spinner_frame "$i")$N $D$(elapsed $(($(now) - STEP_T0)))$N"
        i=$((i + 1))
        sleep 1
    done
    rc=0; wait "$JOB" || rc=$?
    JOB=""
    return "$rc"
}

# ── versions and signatures ──────────────────────────────────────────
# "2026.10.0" vs "1.3.10", "1.3.10" vs "1.3.9": 0 when $1 is newer than $2
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
            -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false -c gpg.ssh.program=ssh-keygen \
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
# the tag; none newer than $2: prints nothing (an up to date install). With
# the commit $3 that release $2 was installed from, the tag of $2 is checked
# too: release tags never move (a release made again is a new version), so
# one that now points to another commit - moved by mistake, or an older tag
# object served by a mirror to roll the install back - is reported and
# never installed. Codes: 1 no signed release, 2 $REPO cannot be reached,
# 3 a release cannot be downloaded (its name printed) - a failed download
# is not an unsigned release, and must not end as "up to date".
newest_signed() {
    refs=$(git ls-remote --tags --refs -- "$REPO" 'shellter-v*' 2>>"${LOG:-/dev/null}") || return 2
    tags=$(printf '%s\n' "$refs" |
        sed -n 's|.*refs/tags/\(shellter-v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)$|\1|p' |
        sed 's/^shellter-v//' | sort -t. -k1,1nr -k2,2nr -k3,3nr)
    [ -n "$tags" ] || return 1
    tries=0
    for v in $tags; do
        if ! newer "$v" "$2"; then
            # older, or the installed release without a known commit
            newer "$2" "$v" || [ -z "${3:-}" ] && return 0
            fetch_ref "$1" "shellter-v$v" || { echo "shellter-v$v"; return 3; }
            if signed_tag "$1" "shellter-v$v"; then
                [ "$(git -C "$1" rev-parse "$TAG_OBJ^{commit}")" = "$3" ] ||
                    warn "the release tag shellter-v$v now points to a different commit than the installed one - not updating; report it" >&2
                return 0
            fi
            warn "shellter-v$v is not signed by the erssi Shellter release key - skipped" >&2
            return 0
        fi
        fetch_ref "$1" "shellter-v$v" || { echo "shellter-v$v"; return 3; }
        if signed_tag "$1" "shellter-v$v"; then
            echo "shellter-v$v $TAG_OBJ"; return 0
        fi
        warn "shellter-v$v is not signed by the erssi Shellter release key - skipped" >&2
        tries=$((tries + 1))
        [ "$tries" -lt 5 ] || { warn "five unsigned newer tags - giving up (report it)" >&2; return 1; }
    done
    return 1
}
# ~/.local/bin/erssi -> $PREFIX/bin/erssi only for an install of the user's
# own in the home: never as root, never for a prefix outside the home, and
# never when ~/.local/bin is $PREFIX/bin (the link would replace the
# installed binary). Paths compared as resolved (pwd -P).
bin_link_wanted() {
    [ "$ROOT" = 0 ] || return 1
    p=$(cd "$PREFIX" 2>/dev/null && pwd -P) || return 1
    h=$(cd "$HOME" 2>/dev/null && pwd -P) || return 1
    case "$p" in "$h"/*) ;; *) return 1 ;; esac
    pb=$(cd "$PREFIX/bin" 2>/dev/null && pwd -P) || return 1
    lb=$(cd "$HOME/.local/bin" 2>/dev/null && pwd -P) || lb=""
    [ "$pb" != "$lb" ]
}
# The source directory, its venv and every directory above them must be
# the user's own: whoever can make, change or rename them runs code as this
# user (the venv's meson and ninja, the build files of the checkout), and
# --src is kept for every erssi --update.
# shellcheck disable=SC2012 # find -printf is not POSIX; ls -n is
mode_gid() { ls -ldn "$1" 2>/dev/null | awk '{print $1, $4}'; }
# group write by group $2 (mode $1) is the user's alone: the user's private
# group (the user's gid, named like the user, no other members) and no ACL
private_group() {
    case "$1" in *+) return 1 ;; esac
    [ "$2" = "$(id -g)" ] || return 1
    [ "$(id -gn)" = "$(id -un)" ] || return 1
    have getent || return 1
    members=$(getent group "$(id -gn)" | cut -d: -f4)
    [ -z "$members" ] || [ "$members" = "$(id -un)" ]
}
# an ACL ("+") outside Linux: FreeBSD and macOS (NFSv4/ZFS ACLs) can grant
# write without the mode bits showing it; on Linux the group bits are the
# ACL mask
untrusted_acl() {   # mode
    case "$1" in *+) [ "$(uname -s)" != Linux ] ;; *) return 1 ;; esac
}
# 0 when another user can write to $1
others_can_write() {
    pg=$(mode_gid "$1")
    [ -n "$pg" ] || return 0
    if untrusted_acl "${pg%% *}"; then return 0; fi
    case "${pg%% *}" in
        ????????w*) return 0 ;;
        ?????w*) if private_group "${pg%% *}" "${pg#* }"; then return 1; fi; return 0 ;;
    esac
    return 1
}
safe_dir() {   # dir - when it exists: the user's, and only the user can write to it
    mapped_self
    [ -e "$1" ] || [ -L "$1" ] || return 0
    [ ! -L "$1" ] || die "$1 is a symbolic link - use a directory of your own: --src DIR"
    o=$(owner "$1")
    [ "$o" = "$(id -u)" ] || die "$1 belongs to another user (uid ${o:-unknown}) - use a directory of your own: --src DIR"
    if others_can_write "$1"; then
        die "$1 can be changed by other users - use a directory of your own (--src DIR) or take their write access away"
    fi
}
# In a user namespace (a rootless container, bwrap, unshare) the files of
# every user it does not map, root's among them, show as the kernel's
# overflow uid (65534). SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 counts that
# uid as root's above the parent of --src - so the directories there of
# every unmapped user, not only root's, and of a user the namespace maps to
# that same uid (rootless podman maps its "nobody" to 65534); never for the
# parent, the source directory or its venv. Outside a user namespace, and
# without /proc (FreeBSD), it does nothing. SHELLTER_PROC (test-only, like
# SHELLTER_OS_RELEASE and SHELLTER_BASE_OPENSSL): a stand-in /proc.
in_userns() {   # 0 when /proc/self/uid_map is not the initial (identity) map
    um="${SHELLTER_PROC:-/proc}/self/uid_map"
    [ -r "$um" ] || return 1
    [ "$(awk '{$1 = $1; print}' "$um" 2>/dev/null)" != "0 0 4294967295" ]
}
overflow_uid() {   # the overflow uid inside a user namespace, else nothing
    in_userns || return 0
    ov=$(cat "${SHELLTER_PROC:-/proc}/sys/kernel/overflowuid" 2>/dev/null) || return 0
    case "$ov" in ''|*[!0-9]*|0) return 0 ;; esac
    printf '%s\n' "$ov"
}
# A namespace that does not map the user: id -u is the overflow uid as well,
# so every unmapped user's directory would pass as the user's own - refused
mapped_self() {
    ov=$(overflow_uid)
    [ -z "$ov" ] || [ "$(id -u)" != "$ov" ] ||
        die "your uid is not mapped in this user namespace (id -u is the overflow uid $ov, which the files of every unmapped user show as, so no directory can be told to be yours) - run the installer where your uid is mapped (e.g. unshare --map-current-user, podman --userns=keep-id)"
}
# $1, the resolved parent of the source directory, and every directory above
# it: the user's or root's, and nobody else can write to the parent; above
# it only a sticky directory of root's (/tmp) may be open to others
# (ssh StrictModes). Above the parent, the overflow uid counts as root's
# with SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 (see in_userns).
safe_path() {
    d=$1
    hint="choose a --src under a directory only you can write"
    mapped_self
    ovf=$(overflow_uid)
    trust=""
    [ "${SHELLTER_INSTALL_TRUST_OVERFLOW_UID:-}" != 1 ] || trust=$ovf
    while :; do
        o=$(owner "$d")
        r=$o        # the owner as the rules below see it
        [ -z "$trust" ] || [ "$d" = "$1" ] || [ "$o" != "$trust" ] || r=0
        if [ "$o" != "$(id -u)" ] && [ "$r" != 0 ]; then
            if [ -n "$ovf" ] && [ "$o" = "$ovf" ]; then
                # a user namespace: root's directories show as this uid
                if [ "$d" = "$1" ]; then
                    die "$d belongs to another user (uid $o) - $hint (in this user namespace uid $o is every user outside it, root included; SHELLTER_INSTALL_TRUST_OVERFLOW_UID does not cover the parent of --src)"
                fi
                die "$d belongs to another user (uid $o) - $hint, or, as uid $o is every user outside this user namespace (root included), set SHELLTER_INSTALL_TRUST_OVERFLOW_UID=1 to count it as root's above the parent of --src"
            fi
            die "$d belongs to another user (uid ${o:-unknown}) - $hint"
        fi
        if others_can_write "$d"; then
            # above the parent: a sticky directory of root's without an
            # untrusted ACL (/tmp) is fine
            pg=$(mode_gid "$d")
            m=${pg%% *}
            if [ "$d" = "$1" ] || [ "$r" != 0 ] || untrusted_acl "$m"; then
                die "$d is writable by other users - $hint"
            fi
            case "$m" in
                ?????????[tT]*) ;;
                *) die "$d is writable by other users - $hint" ;;
            esac
        fi
        [ "$d" != / ] || break
        d=$(dirname -- "$d")
    done
}
conf_get() {   # key -> value from shellter-install.conf (never run as shell)
    [ -f "$CONF" ] && sed -n "s/^$1=//p" "$CONF" | tail -n 1
    return 0
}

main() {
unset CDPATH        # cd in $(...) must print nothing
PREFIX="$HOME/.local/opt/erssi"
SRC="" REF="" REPO="" RUN_TESTS="" CLEAN="" UPDATE=0 CHECK=0 YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) optval "$1" "${2:-}" DIR; PREFIX=$2; shift 2 ;;
        --src) optval "$1" "${2:-}" DIR; SRC=$2; shift 2 ;;
        --ref) optval "$1" "${2:-}" TAG; REF=$2; shift 2 ;;
        --repo) optval "$1" "${2:-}" URL; REPO=$2; shift 2 ;;
        --no-test) RUN_TESTS=0; shift ;;
        --test) RUN_TESTS=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --no-clean) CLEAN=0; shift ;;
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
# shellcheck disable=SC2012 # find -printf is not POSIX; ls -n is
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
# the commit it was built from; installs before COMMIT was recorded: the
# checkout in the source directory, if it is still there
INSTALLED_COMMIT=$(conf_get COMMIT)
[ -n "$INSTALLED_COMMIT" ] || [ ! -d "$SRC/.git" ] ||
    INSTALLED_COMMIT=$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null) || INSTALLED_COMMIT=""
# what it was installed from: a release tag, or a branch or commit (--ref).
# Nothing recorded (installed before 2026.10.3): a release, as every
# --update and every install without --ref made one
INSTALLED_REF=$(conf_get REF)

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

T_START=$(now)
if [ "$UPDATE" = 1 ]; then header "update"; else header "install"; fi
field "prefix" "$(tilde "$PREFIX")"
field "source" "$(tilde "$SRC")"
[ -n "$INSTALLED" ] && field "installed" "$INSTALLED"
printf '\n'

if [ "$CHECK" = 0 ]; then
    # one installer at a time on this source directory, taken before
    # anything is written: the log, the meson/ninja venv ($SRC.venv, step 1)
    # and the checkout
    # the source directory as it is: links resolved once, used from here on
    mkdir -p "$(dirname -- "$SRC")"
    d=$(cd -P -- "$(dirname -- "$SRC")" && pwd -P) || die "cannot open $(dirname -- "$SRC")"
    SRC="${d%/}/$(basename -- "$SRC")"
    if [ -L "$SRC" ]; then
        t=$(cd -P -- "$SRC" 2>/dev/null && pwd -P) || die "$SRC is a link, but not to a directory - use --src DIR"
        SRC=$t
    fi
    # the resolved path: checked as the given one was
    case "$SRC" in
        -*|*"
"*) die "invalid directory: $SRC" ;;
    esac
    safe_path "$(dirname -- "$SRC")"
    mkdir "$SRC.lock" 2>/dev/null || die "another installer is using $SRC (if not, remove $SRC.lock)"
    trap 'rmdir "$SRC.lock" 2>/dev/null' EXIT
    safe_dir "$SRC"
    safe_dir "$SRC.venv"
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
# FreeBSD: the OpenSSL of the base system (headers and libraries in /usr,
# no openssl.pc) is enough; meson finds it without pkg-config. Prints its
# version, or "unknown"; 1 when it is not there or the system is not
# FreeBSD. SHELLTER_BASE_OPENSSL: another base directory (tests).
base_openssl() {
    [ "$OS" = FreeBSD ] || return 1
    b=${SHELLTER_BASE_OPENSSL:-/usr}
    [ -f "$b/include/openssl/ssl.h" ] && [ -f "$b/include/openssl/opensslv.h" ] &&
        [ -e "$b/lib/libssl.so" ] && [ -e "$b/lib/libcrypto.so" ] || return 1
    v=$(sed -n 's/.*OPENSSL_VERSION_TEXT[[:space:]]*"OpenSSL \([^[:space:]"]*\).*/\1/p' "$b/include/openssl/opensslv.h" | head -n 1)
    echo "${v:-unknown}"
}
openssl_version() {
    $PKGCONFIG --modversion openssl 2>/dev/null || echo "$(base_openssl) (base system)"
}
# missing features -> $REQ (needed) and $REC (recommended)
check_system() {
    REQ="" REC=""
    have git || REQ="$REQ git"
    have cc || have gcc || have clang || REQ="$REQ cc"
    PKGCONFIG=""
    for t in pkg-config pkgconf; do have "$t" && { PKGCONFIG=$t; break; }; done
    if [ -z "$PKGCONFIG" ]; then
        REQ="$REQ pkgconf glib"
        base_openssl >/dev/null || REQ="$REQ openssl"
    else
        $PKGCONFIG --exists 'glib-2.0 >= 2.32' && $PKGCONFIG --exists gmodule-2.0 && $PKGCONFIG --exists gio-2.0 || REQ="$REQ glib"
        $PKGCONFIG --exists openssl || base_openssl >/dev/null || REQ="$REQ openssl"
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
and_list() {   # words -> "a", "a and b"
    printf '%s' "$1"; [ $# -lt 2 ] || printf ' and %s' "$2"
}
pip_spec() {   # build tool -> the version pip installs
    case "$1" in meson) echo "$MESON_PIP" ;; ninja) echo "$NINJA_PIP" ;; esac
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
        # shellcheck disable=SC2086 # package lists are split on purpose
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
    # only the tools the system lacks come from the venv
    MISSING=""
    have meson || MISSING="meson"
    have ninja || MISSING="${MISSING:+$MISSING }ninja"
    if [ -n "$MISSING" ]; then
        PY=""
        for p in python3 python3.14 python3.13 python3.12 python3.11 python3.10; do have "$p" && { PY=$p; break; }; done
        # a safeguard only: without python3, check_system has already asked
        # for meson and ninja as packages
        case "$MISSING" in *" "*) are=are ;; *) are=is ;; esac
        # shellcheck disable=SC2086 # tool list
        [ -n "$PY" ] || die "$(and_list $MISSING) $are missing, and so is Python 3: $(pm_hint "$(names $MISSING)")"
        VENV="$SRC.venv"
        # made here when missing, and checked before it is built or reused
        [ -e "$VENV" ] || [ -L "$VENV" ] || mkdir "$VENV" || die "cannot create $VENV"
        safe_dir "$VENV"
        # of those, what an earlier run did not put into the venv
        PIP=""
        for t in $MISSING; do [ -x "$VENV/bin/$t" ] || PIP="${PIP:+$PIP }$t"; done
        if [ -n "$PIP" ]; then
            # shellcheck disable=SC2046,SC2086 # tool lists
            warn "$(and_list $PIP) not found - installing $(and_list $(for t in $PIP; do pip_spec "$t"; done)) into $VENV (no root needed)"
            run_logged "$PY" -m venv "$VENV" || die_log "$PY -m venv failed ($(pm_hint python3-venv))"
            # wheels only: meson's is pure Python (any system); ninja has
            # none for some systems (FreeBSD), and its source package builds
            # cmake from source first - half an hour without a word. With
            # no wheel the installer stops here and names the package.
            for t in $PIP; do
                run_logged "$VENV/bin/pip" install -q --only-binary=:all: "$(pip_spec "$t")" ||
                    die_log "pip could not install $t (no network, or no ready-made wheel for this system): $(pm_hint "$(names "$t")")"
            done
        fi
        # after the system's directories: a tool of the system is never
        # shadowed by one of the venv
        PATH="$PATH:$VENV/bin"; export PATH
    fi
    done_ "meson $("$MESON" --version) $S_SEP ninja $("$NINJA" --version) $S_SEP glib $($PKGCONFIG --modversion glib-2.0) $S_SEP openssl $(openssl_version)"
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
    WORK=$SRC           # locked above
    # made here when missing, and checked before anything in it is used
    [ -d "$SRC" ] || mkdir "$SRC" || die "cannot create $SRC - use another --src"
    safe_dir "$SRC"
    if [ -d "$SRC/.git" ]; then
        [ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ] ||
            die "$SRC has local changes - the installer would overwrite them; use another --src"
    else
        # checkout --force must not land on somebody's files
        [ -z "$(ls -A "$SRC" 2>/dev/null)" ] ||
            die "$SRC exists and is not a git checkout - remove it or use another --src"
        git -c init.defaultBranch=main init -q "$SRC" || die "cannot create a git repository in $SRC"
        NEW_SRC=1
    fi
fi
if [ "$UPDATE" = 1 ]; then
    step "Finding the release"
    can_verify || die "release signatures cannot be checked here (git 2.34+ and ssh-keygen are needed) - update with an explicit release instead: sh shellter-install.sh --ref shellter-v2026.10.2"
    case "$INSTALLED_REF" in
        ''|shellter-v*)
            # a release: a newer one, and its own tag checked
            rc=0; FOUND=$(newest_signed "$WORK" "$INSTALLED" "$INSTALLED_COMMIT") || rc=$?
            FROM_BRANCH="" ;;
        *)
            # a branch or a commit: main keeps the version of the last
            # release until the next one, so its commit says nothing about
            # the release of that version - the newest signed release, and
            # only a newer version counts
            rc=0; FOUND=$(newest_signed "$WORK" 0) || rc=$?
            FROM_BRANCH=$INSTALLED_REF ;;
    esac
    [ "$rc" != 2 ] || die "cannot reach $REPO"
    [ "$rc" != 3 ] || die "cannot download $FOUND from $REPO - nothing was changed; try again later"
    if [ "$rc" != 0 ]; then
        [ -n "$FROM_BRANCH" ] || die "no signed release newer than $INSTALLED found on $REPO"
        die "no signed release found on $REPO"
    fi
    REF=${FOUND%% *}; TAG_OBJ=${FOUND#* }
    if [ -n "$REF" ]; then
        NEWEST=${REF#shellter-v}
        done_ "$REF $S_SEP signed by the erssi Shellter release key"
    else
        NEWEST=$INSTALLED
        done_ "none newer than $INSTALLED"
    fi
    if [ -n "$FROM_BRANCH" ]; then
        printf '\n  Installed from %s; newest release is %s' "$FROM_BRANCH" "$NEWEST"
        if ! newer "$NEWEST" "$INSTALLED"; then
            printf ', not newer than the installed %s - nothing to do.\n\n' "$INSTALLED"
            exit 0
        fi
        printf '.\n'
    elif [ -z "$REF" ]; then
        printf '\n  %serssi Shellter Edition %s is up to date%s (the newest signed release).\n\n' "$B" "$INSTALLED" "$N"
        exit 0
    fi
    if [ "$CHECK" = 1 ]; then
        printf '\n  %sUpdate available: %s %s %s%s\n  Run: erssi --update\n\n' "$B" "$INSTALLED" "$S_ARROW" "$NEWEST" "$N"
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
# release tags never move: a recorded release elsewhere is refused, on an
# update too
if [ -n "$PINNED" ]; then
    [ "$HEAD_SHA" = "$PINNED" ] || die "$REF points to $HEAD_SHA, but this installer expects $PINNED - refusing to build (download the installer again, or report it)"
    VERIFIED="${VERIFIED:+$VERIFIED $S_SEP }commit recorded"
elif [ "$UPDATE" = 0 ]; then
    case "$REF" in
        shellter-v*) warn "$REF is not recorded in this installer - not verified; download the installer again to verify it" ;;
        *) warn "$REF is not a release - built as is, not verified" ;;
    esac
fi
done_ "$(echo "$HEAD_SHA" | cut -c1-8)${VERIFIED:+ $S_SEP $VERIFIED}"

# ── 4. build ─────────────────────────────────────────────────────────
step "Building"
if [ -f "$SRC/Build/build.ninja" ]; then
    run_logged "$MESON" setup --reconfigure "$SRC/Build" "$SRC" -Dprefix="$PREFIX" || die_log "configuring the build failed"
else
    run_logged "$MESON" setup "$SRC/Build" "$SRC" -Dprefix="$PREFIX" -Dwith-proxy=yes || die_log "configuring the build failed"
fi
run_logged "$NINJA" -C "$SRC/Build" || die_log "the build failed"
done_ "$(elapsed $(($(now) - STEP_T0)))"

# ── 5. test ──────────────────────────────────────────────────────────
step "Testing"
if [ "$RUN_TESTS" = 1 ]; then
    run_logged "$MESON" test -C "$SRC/Build" || die_log "tests failed (all results: $SRC/Build/meson-logs/testlog.txt)"
    n=$(sed -n 's/^Ok: *\([0-9]*\).*/\1/p' "$LOG" | tail -n 1)
    done_ "${n:+$n tests $S_SEP }$(elapsed $(($(now) - STEP_T0)))"
else
    skip_ "skipped"
fi

# ── 6. install ───────────────────────────────────────────────────────
step "Installing"
run_logged "$NINJA" -C "$SRC/Build" install || die_log "installing failed"
# erssi before 2026.10.0 shipped it
rm -f "$PREFIX/share/irssi/themes/nexus.theme"
mkdir -p "$PREFIX/share/irssi"
LINKED=0
if bin_link_wanted; then
    mkdir -p "$HOME/.local/bin"
    l="$HOME/.local/bin/erssi"
    # a link (ours from before, or to anywhere) is replaced; a file or a
    # directory of the user's own there is kept
    if [ -L "$l" ] || [ ! -e "$l" ]; then
        rm -f "$l"
        ln -s "$PREFIX/bin/erssi" "$l"
        LINKED=1
    else
        warn "$(tilde "$l") exists and is not a link - kept; this erssi is $(tilde "$PREFIX/bin/erssi")"
    fi
fi
# erssi --update runs this installer (the copy of the installed release)
cp "$SRC/shellter-install.sh" "$PREFIX/share/irssi/shellter-install.sh" 2>/dev/null || true
{
    echo "# written by shellter-install.sh - the settings erssi --update uses"
    echo "REF=$REF"
    echo "SRC=$SRC"
    echo "REPO=$REPO"
    echo "TESTS=$RUN_TESTS"
    echo "CLEAN=$CLEAN"
    echo "COMMIT=$HEAD_SHA"
} > "$CONF.tmp" && chmod 600 "$CONF.tmp" && mv "$CONF.tmp" "$CONF"
VERSION=$("$PREFIX/bin/erssi" --version | awk '{print $2}')
if [ "$CLEAN" = 1 ]; then
    rm -rf "$SRC/Build"
    done_ "$(tilde "$PREFIX") $S_SEP build directory removed"
else
    done_ "$(tilde "$PREFIX")"
fi

# ── summary ──────────────────────────────────────────────────────────
IN_PATH=1
if [ "$LINKED" = 1 ]; then
    # shellcheck disable=SC2088 # shown, not run
    case ":$PATH:" in
        *":$HOME/.local/bin:"*) RUN=erssi ;;
        *) RUN="~/.local/bin/erssi"; IN_PATH=0 ;;
    esac
else
    case ":$PATH:" in
        *":$PREFIX/bin:"*) RUN=erssi ;;
        *) RUN=$(tilde "$PREFIX/bin/erssi"); IN_PATH=0 ;;
    esac
fi
printf '\n'
if [ -n "$INSTALLED" ] && [ "$INSTALLED" != "$VERSION" ]; then
    printf '  %s%sUpdated: %s %s %s%s  %s(%s)%s\n\n' "$B" "$G" "$INSTALLED" "$S_ARROW" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
    field "running" "in a running erssi, /upgrade loads $VERSION without disconnecting"
else
    printf '  %s%serssi Shellter Edition %s is installed%s  %s(%s)%s\n\n' "$B" "$G" "$VERSION" "$N" "$D" "$(elapsed $(($(now) - T_START)))" "$N"
    field "start" "$RUN   (inside tmux, so it keeps running when you log out)"
    field "connect" "/connect IRCnet"
fi
field "update" "erssi --update"
if [ "$IN_PATH" = 0 ] && [ "$LINKED" = 1 ]; then
    # shellcheck disable=SC2088 # shown, not run
    field "path" "~/.local/bin is not in PATH - add to ~/.profile: export PATH=\"\$HOME/.local/bin:\$PATH\""
elif [ "$IN_PATH" = 0 ]; then
    field "path" "$(tilde "$PREFIX/bin") is not in PATH"
fi
field "log" "$(tilde "$LOG")"
printf '\n'
}

[ -n "${SHELLTER_INSTALL_NO_MAIN:-}" ] || main "$@"
