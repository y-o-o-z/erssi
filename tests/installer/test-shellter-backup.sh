#!/bin/sh
# shellter-install.sh --backup / --restore on homes made up in a temporary
# directory, with the system's tar, gzip and xz behind a stand-in tar that
# refuses every option but -c -h -f -t -v -x and --no-same-owner (what the
# tar of FreeBSD and GNU tar both take). Nothing outside a temporary
# directory is touched; HOME is one inside it.
# Usage: test-shellter-backup.sh SOURCE_ROOT CASE
# Exit: 0 pass, 1 fail, 77 skip.

if [ -z "${SHELLTER_TEST_CLEAN:-}" ]; then
    exec env -i SHELLTER_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

SRC=$1
CASE=$2
INSTALLER=$SRC/shellter-install.sh
FAILED=0
pass() { echo "ok $CASE: $1"; }
fail() { echo "FAIL $CASE: $1"; FAILED=1; }
check() {   # description command...
    d=$1; shift
    if "$@"; then pass "$d"; else fail "$d"; fi
}

for t in tar gzip find; do
    command -v "$t" >/dev/null 2>&1 || { echo "skip: no $t"; exit 77; }
done
command -v python3 >/dev/null 2>&1 || { echo "skip: no python3 to make the bad archives"; exit 77; }

W=$(mktemp -d) && W=$(cd -P "$W" && pwd -P) || exit 1
# the helper processes a case starts (bg below): ended however the case
# ends. chmod first: a failing case may leave a directory no one may enter
BG=""
trap 'for p in $BG; do kill "$p" 2>/dev/null; done; chmod -R u+rwx "$W" 2>/dev/null; rm -rf "$W"' EXIT
trap 'exit 1' HUP INT TERM
# a helper process ended, waited for and taken out of BG: the EXIT trap
# must never kill its number, which another process may have by then
reap() {
    kill "$1" 2>/dev/null
    wait "$1" 2>/dev/null
    rest=""
    for p in $BG; do [ "$p" = "$1" ] || rest="$rest $p"; done
    BG=$rest
}
chgrp "$(id -g)" "$W" || exit 1
HOME=$W/home; export HOME
mkdir -p "$HOME" "$W/shim"
# the stand-in tar: the options of both tars only, each one logged
REAL_TAR=$(command -v tar)
cat > "$W/shim/tar" <<EOF
#!/bin/sh
for a in "\$@"; do
    case "\$a" in
        -c|-h|-f|-t|-v|-x|--no-same-owner) echo "\$a" >> "$W/tar.opts" ;;
        -*) [ "\$a" = - ] || { echo "stand-in tar: option \$a is not one of both tars" >&2; echo "BAD \$a" >> "$W/tar.opts"; exit 2; } ;;
    esac
done
# stand-ins for what is hard to make happen on time: a file that grows
# while it is read (GNU tar's exit 1), another error, an entry --restore
# refuses (a FIFO put into what is written)
case " \$* " in
    *" -c "*)
        for top in "\$@"; do :; done
        if [ -n "\${FAKE_TAR_FIFO:-}" ]; then mkfifo "\$top/home/fifo"; fi
        if [ -n "\${FAKE_TAR_CHANGED:-}" ] || [ -n "\${FAKE_TAR_ERROR:-}" ]; then
            "$REAL_TAR" "\$@" || exit
            for f in \${FAKE_TAR_CHANGED:-}; do echo "tar: \$top/home/\$f: file changed as we read it" >&2; done
            [ -z "\${FAKE_TAR_ERROR:-}" ] || echo "tar: \$top/home/\$FAKE_TAR_ERROR: Cannot open: Permission denied" >&2
            exit 1
        fi ;;
esac
exec "$REAL_TAR" "\$@"
EOF
chmod 755 "$W/shim/tar"
# a UTF-8 locale with translations, for the cases that need one
PL=""
for l in pl_PL.UTF-8 pl_PL.utf8; do
    if [ "$(LC_ALL=$l locale charmap 2>/dev/null)" = UTF-8 ]; then PL=$l; break; fi
done
PATH=$W/shim:$PATH; export PATH

E=$HOME/.erssi
# up() FILE: wait until a helper started in the background has written
# FILE (it runs then): an idle machine takes a moment, a loaded one longer
up() {
    i=0
    while [ ! -s "$1" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
    [ -s "$1" ] || { fail "the helper never wrote $1"; exit 1; }
}
run() {   # installer options... -> output in $W/out, exit code in $RC
    RC=0
    sh "$INSTALLER" "$@" > "$W/out" 2>&1 || RC=$?
}
# the same with variables for the installer: VAR=value... -- options.
# Not "VAR=value run ...": FreeBSD's sh does not export assignments put
# before a function call to the commands the function runs
run_env() {
    RC=0
    (
        # shellcheck disable=SC2163 # export NAME=value as given
        while [ "$1" != -- ]; do export "$1"; shift; done
        shift
        sh "$INSTALLER" "$@"
    ) > "$W/out" 2>&1 || RC=$?
}
# the same text, byte for byte; not cmp, which a minimal system lacks
same() { [ -f "$1" ] && [ -f "$2" ] && [ "$(cat "$1"; echo .)" = "$(cat "$2"; echo .)" ]; }
said() { grep -q -- "$1" "$W/out"; }
show() { sed 's/^/    | /' "$W/out"; }
# shellcheck disable=SC2012 # the mode string, of names the test made
mode_of() { ls -ld "$1" | cut -c1-10; }

# a home with what erssi keeps there, in modes erssi would not use
make_home() {   # dir
    h=$1
    mkdir -p "$h/scripts/autorun" "$h/rpe2e" "$h/otr" "$h/logs/IRCnet" "$h/image_cache" "$h/help" "$h/modules"
    printf 'servers = ( );\nsettings = { core = { real_name = "x"; }; };\n' > "$h/config"
    printf 'startup\n' > "$h/startup"
    printf 'default.theme\n' > "$h/default.theme"
    printf 'abc  default.theme\n' > "$h/default-files.sha256"
    printf '{"identity": "secret"}\n' > "$h/rpe2e/keyring.json"
    printf 'old\n' > "$h/rpe2e/keyring.json.bak-1700000000"
    printf 'trust\n' > "$h/rpe2e/replay.cache"
    printf 'otr key\n' > "$h/otr/otr.key"
    printf 'KEY\n' > "$h/fe-web-key.pem"
    printf 'CERT\n' > "$h/fe-web-cert.pem"
    printf 'net = pass\n' > "$h/.credentials"
    printf 'IRCnet nick\n' > "$h/keepnick"
    printf 'away\n' > "$h/away.log"
    printf 'print "hi";\n' > "$h/scripts/hello.pl"
    chmod 755 "$h/scripts/hello.pl"
    ln -s ../hello.pl "$h/scripts/autorun/hello.pl"
    printf 'help\n' > "$h/help/hello"
    printf 'mention\n' > "$h/logs/mentions.log"
    i=0
    while [ $i -lt 20 ]; do printf 'line %s\n' "$i" >> "$h/logs/IRCnet/#chan.log"; i=$((i + 1)); done
    printf 'img\n' > "$h/image_cache/abc"
    printf 'session\n' > "$h/session"
    : > "$h/erssi.lock"
    printf 'debug\n' > "$h/sidepanels.log"
    printf 'name with spaces\n' > "$h/scripts/a file.txt"
    chmod 644 "$h/config" "$h/rpe2e/keyring.json"
    chmod 755 "$h/rpe2e"
}
# what a tree holds: type, mode, link target, content checksum, per path
tree() {   # dir
    (cd "$1" && find . ! -name . -print | LC_ALL=C sort | while IFS= read -r p; do
        if [ -L "$p" ]; then printf 'l %s -> %s\n' "$p" "$(readlink "$p")"
        elif [ -d "$p" ]; then printf 'd %s %s\n' "$(mode_of "$p")" "$p"
        else printf 'f %s %s %s\n' "$(mode_of "$p")" "$(cksum < "$p" | tr -d ' ')" "$p"
        fi
    done)
}
# the same, modes left out (the original home has the user's modes)
content() { tree "$1" | awk '{ if ($1 == "l") print; else { $2 = ""; print } }'; }

# an archive made by hand: python's tarfile, so every bad entry is exact
mkbad() {   # file kind
    python3 -I - "$1" "$2" <<'EOF'
import io, sys, tarfile
out, kind = sys.argv[1], sys.argv[2]
top = "erssi-backup-20260101-000000"
def add(t, name, data=b"x", type_=tarfile.REGTYPE, link="", mode=None):
    i = tarfile.TarInfo(name)
    i.type = type_
    i.mode = 0o600 if type_ != tarfile.DIRTYPE else 0o700
    if mode is not None:
        i.mode = mode
    i.linkname = link
    if type_ == tarfile.REGTYPE:
        i.size = len(data)
        t.addfile(i, io.BytesIO(data))
    else:
        t.addfile(i)
manifest = "erssi-backup 1\nversion 2026.11.0\ncreated now\nhome ~/.erssi\nlogs excluded\nfiles 1\nf\t1\tconfig\n"
with tarfile.open(out, "w:gz", format=tarfile.GNU_FORMAT) as t:
    if kind == "foreign":
        add(t, "something/README")
    else:
        add(t, top, type_=tarfile.DIRTYPE)
        add(t, top + "/home", type_=tarfile.DIRTYPE)
        add(t, top + "/home/config")
        if kind == "manifest-link":
            manifest += "l\tescape\t../../../../outside\n"
        if kind == "manifest-link-dotdot":
            manifest += "l\tscripts/x\t../config/..\n"
        if kind == "manifest-extra":
            manifest += "f\t1\tmissing\n"
        # links of the MANIFEST that would lead out once made one after
        # the other: a1/a2/a3/L made twice (the second ln follows the
        # first into sub/), or a link below another link
        # directories no one may enter or list (0000, 0100, 0300), with a
        # file in each; -bad: and a MANIFEST that does not match
        if kind.startswith("closed"):
            for d, m in (("d0", 0o000), ("d1", 0o100), ("d3", 0o300)):
                add(t, top + "/home/" + d, type_=tarfile.DIRTYPE, mode=m)
                add(t, top + "/home/" + d + "/x")
                manifest += "d\t" + d + "\nf\t1\t" + d + "/x\n"
            if kind == "closed-bad":
                manifest += "f\t1\tmissing\n"
        if kind.startswith("links-"):
            for d in ("a1", "a1/a2", "a1/a2/a3", "sub"):
                add(t, top + "/home/" + d, type_=tarfile.DIRTYPE)
                manifest += "d\t" + d + "\n"
            add(t, top + "/home/f")
            manifest += "f\t1\tf\n"
            manifest = manifest.replace("files 1", "files 2")
            manifest += {
                "links-dup": "l\ta1/a2/a3/L\t../../../sub\nl\ta1/a2/a3/L\t../../../f\n",
                "links-dup-same": "l\ta1/a2/a3/L\t../../../sub\nl\ta1/a2/a3/L\t../../../sub\n",
                "links-dup-file-first": "l\ta1/a2/a3/L\t../../../f\nl\ta1/a2/a3/L\t../../../sub\n",
                "links-below": "l\ta1/L\t../sub\nl\ta1/L/x\t../../f\n",
                "links-through": "l\ta1/L\t../sub\nl\tM\ta1/L\n",
                "links-fine": "l\ta1/a2/a3/L\t../../../sub\nl\ta1/a2/K\t../../f\n",
            }[kind]
        add(t, top + "/MANIFEST", manifest.encode())
        if kind == "absolute":
            add(t, "/tmp/erssi-backup-test-absolute")
        elif kind == "dotdot":
            add(t, top + "/home/../../escaped")
        elif kind == "symlink":
            add(t, top + "/home/link", type_=tarfile.SYMTYPE, link="../../../outside")
            add(t, top + "/home/link/planted")
        elif kind == "hardlink":
            add(t, top + "/home/hard", type_=tarfile.LNKTYPE, link=top + "/home/config")
        elif kind == "fifo":
            add(t, top + "/home/fifo", type_=tarfile.FIFOTYPE)
        elif kind == "device":
            add(t, top + "/home/dev", type_=tarfile.CHRTYPE)
        elif kind == "outside-top":
            add(t, "other/file")
        elif kind == "no-home":
            add(t, top + "/elsewhere")
EOF
}

case "$CASE" in
round-trip)
    make_home "$E"
    umask 000
    run --backup "$W/b.tar.gz" --erssi-version 2026.11.0
    umask 022
    if [ "$RC" != 0 ]; then show; fi
    check "backup succeeds" [ "$RC" = 0 ]
    check "the backup is mode 0600 (made under umask 000)" [ "$(mode_of "$W/b.tar.gz")" = "-rw-------" ]
    check "it says the file holds private keys and passwords" said "Keep this file private"
    check "no temporary file is left next to it" [ -z "$(find "$W" -maxdepth 1 -name '.erssi-backup.*')" ]
    gzip -dc "$W/b.tar.gz" | "$REAL_TAR" -tf - > "$W/list"
    check "the E2E keyring is in it" grep -q '/home/rpe2e/keyring.json$' "$W/list"
    check "its backups too" grep -q '/home/rpe2e/keyring.json.bak-1700000000$' "$W/list"
    check "the fe-web key and the credentials are in it" grep -q '/home/fe-web-key.pem$' "$W/list"
    check "the Mentions log is in it" grep -q '/home/logs/mentions.log$' "$W/list"
    check "the image cache is not" sh -c "! grep -q image_cache '$W/list'"
    check "the /upgrade session and the lock are not" sh -c "! grep -q -e '/home/session\$' -e 'erssi.lock' '$W/list'"
    check "the debug log is not" sh -c "! grep -q sidepanels.log '$W/list'"
    check "the autorun link is not stored as a link (nor a copy)" sh -c "! grep -q autorun/hello.pl '$W/list'"
    gzip -dc "$W/b.tar.gz" | "$REAL_TAR" -xOf - "$(head -n 1 "$W/list" | sed 's|/*$||')/MANIFEST" > "$W/manifest"
    check "the MANIFEST records the autorun link" grep -q "^l	scripts/autorun/hello.pl	../hello.pl\$" "$W/manifest"
    check "the MANIFEST names the version" grep -qx 'version 2026.11.0' "$W/manifest"
    check "the MANIFEST names the home with ~" grep -qx 'home ~/.erssi' "$W/manifest"
    check "only the options of both tars were used" sh -c "! grep -q BAD '$W/tar.opts'"

    run --restore "$W/b.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "restore into a home that is not there yet succeeds" [ "$RC" = 0 ]
    # the original without what a backup leaves out
    rm -rf "$E/image_cache" "$E/session" "$E/erssi.lock" "$E/sidepanels.log" "$E/logs/IRCnet"
    content "$E" > "$W/want"
    # the lock file it held while it swapped the home stays there
    content "$W/new/.erssi" | grep -v ' ./erssi.lock$' > "$W/got"
    check "the restored home has the same files, contents and links" same "$W/want" "$W/got"
    [ "$FAILED" = 0 ] || ! command -v diff >/dev/null 2>&1 || diff "$W/want" "$W/got"
    check "the home is 0700" [ "$(mode_of "$W/new/.erssi")" = "drwx------" ]
    bad=$(tree "$W/new/.erssi" | awk '$1 == "d" && $2 != "drwx------" || $1 == "f" && $2 != "-rw-------" && $2 != "-rwx------"')
    check "every directory is 0700, every file 0600 or 0700" [ -z "$bad" ]
    check "the keyring is 0600 (was 0644)" [ "$(mode_of "$W/new/.erssi/rpe2e/keyring.json")" = "-rw-------" ]
    check "the script stays executable, 0700" [ "$(mode_of "$W/new/.erssi/scripts/hello.pl")" = "-rwx------" ]
    check "the autorun link works" same "$W/new/.erssi/scripts/autorun/hello.pl" "$E/scripts/hello.pl"
    check "no temporary directory is left" [ -z "$(find "$W/new" -name '.erssi-restore.*')" ]
    check "restore used only the options of both tars" sh -c "! grep -q BAD '$W/tar.opts'"
    ;;

xz)
    command -v xz >/dev/null 2>&1 || { echo "skip: no xz"; exit 77; }
    make_home "$E"
    run --backup "$W/b.tar.xz"
    check "a .tar.xz backup is made" [ "$RC" = 0 ]
    check "it is xz" [ "$(od -An -tx1 -N6 "$W/b.tar.xz" | tr -d ' \n')" = fd377a585a00 ]
    run --restore "$W/b.tar.xz" --home "$W/new/.erssi"
    check "and restored" [ "$RC" = 0 ]
    check "with the keyring" same "$E/rpe2e/keyring.json" "$W/new/.erssi/rpe2e/keyring.json"
    run --backup "$W/b.zip"
    check "another file name is refused" [ "$RC" != 0 ]
    check "nothing is written then" [ ! -e "$W/b.zip" ]
    ;;

logs)
    make_home "$E"
    run --backup "$W/without.tar.gz"
    check "backup without logs succeeds" [ "$RC" = 0 ]
    check "it says how much of logs/ is left out" said "IRC logs left out: logs/, .* in 1 file(s) (--with-logs takes them)"
    check "the channel log is not in it" sh -c "! gzip -dc '$W/without.tar.gz' | '$REAL_TAR' -tf - | grep -q '#chan.log'"
    check "the Mentions log is" sh -c "gzip -dc '$W/without.tar.gz' | '$REAL_TAR' -tf - | grep -q 'logs/mentions.log'"
    run --backup "$W/with.tar.gz" --with-logs
    check "backup with logs succeeds" [ "$RC" = 0 ]
    check "the channel log is in it" sh -c "gzip -dc '$W/with.tar.gz' | '$REAL_TAR' -tf - | grep -q 'logs/IRCnet/#chan.log'"
    check "the debug log too" sh -c "gzip -dc '$W/with.tar.gz' | '$REAL_TAR' -tf - | grep -q 'sidepanels.log'"
    run --restore "$W/with.tar.gz" --home "$W/new/.erssi"
    check "and it is restored" same "$E/logs/IRCnet/#chan.log" "$W/new/.erssi/logs/IRCnet/#chan.log"
    run --restore "$W/with.tar.gz" --with-logs --home "$W/new2/.erssi"
    check "--with-logs is refused with --restore" [ "$RC" = 2 ]
    ;;

overwrite)
    make_home "$E"
    printf 'mine\n' > "$W/b.tar.gz"
    run --backup "$W/b.tar.gz"
    check "an existing file is refused" [ "$RC" != 0 ]
    check "it says so" said "exists - refusing to overwrite"
    check "and is left as it was" [ "$(cat "$W/b.tar.gz")" = mine ]
    ln -s "$W/elsewhere" "$W/link.tar.gz"
    run --backup "$W/link.tar.gz"
    check "a link at the name is refused" [ "$RC" != 0 ]
    check "and nothing is made where it points" [ ! -e "$W/elsewhere" ]
    run --backup "$E/inside.tar.gz"
    check "a file inside the home is refused" [ "$RC" != 0 ]
    check "nothing is written there" [ -z "$(find "$E" -name 'inside.tar.gz' -o -name '.erssi-backup.*')" ]
    # the default name: in the current directory, or ~ when that is the home
    (cd "$E/scripts" && sh "$INSTALLER" --backup > "$W/out" 2>&1)
    check "the default file goes to ~ from inside the home" [ -n "$(find "$HOME" -maxdepth 1 -name 'erssi-backup-[0-9]*-[0-9]*.tar.gz')" ]
    ;;

links)
    make_home "$E"
    mkdir -p "$W/outside/lib"
    printf 'external\n' > "$W/outside/lib/ext.pl"
    printf 'shared\n' > "$W/outside/shared.txt"
    ln -s "$W/outside/lib" "$E/scripts/lib"
    ln -s "$W/outside/shared.txt" "$E/one.txt"
    ln -s "$W/outside/shared.txt" "$E/two.txt"
    ln "$E/config" "$E/config.hard"
    ln -s "$E/startup" "$E/startup.abs"
    ln -s missing "$E/dangling"
    ln -s .. "$E/up"
    mkfifo "$E/fifo"
    run --backup "$W/b.tar.gz"
    if [ "$RC" != 0 ]; then show; fi
    check "backup with links out of the home succeeds" [ "$RC" = 0 ]
    check "a link out of the home is followed" said "followed: scripts/lib"
    check "a dangling link is left out, with a note" said "left out: dangling"
    check "a link above the home is left out" said "left out: up"
    check "a FIFO is left out" said "left out: fifo"
    check "no hard link is stored" sh -c "! gzip -dc '$W/b.tar.gz' | '$REAL_TAR' -tvf - | grep -q '^h'"
    run --restore "$W/b.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "and it restores" [ "$RC" = 0 ]
    check "the followed directory is a directory now" sh -c "[ -d '$W/new/.erssi/scripts/lib' ] && [ ! -L '$W/new/.erssi/scripts/lib' ]"
    check "with its file" same "$W/outside/lib/ext.pl" "$W/new/.erssi/scripts/lib/ext.pl"
    check "both names of one outside file are files" sh -c "[ -f '$W/new/.erssi/one.txt' ] && [ -f '$W/new/.erssi/two.txt' ] && [ ! -L '$W/new/.erssi/two.txt' ]"
    check "a hard link becomes a file of its own" [ -f "$W/new/.erssi/config.hard" ] && [ ! -L "$W/new/.erssi/config.hard" ]
    check "with the contents" same "$E/config" "$W/new/.erssi/config.hard"
    check "an absolute link into the home becomes a relative one" [ "$(readlink "$W/new/.erssi/startup.abs")" = startup ]
    ;;

bad-archives)
    make_home "$E"
    tree "$E" > "$W/before"
    for kind in absolute dotdot symlink hardlink fifo device outside-top no-home foreign manifest-link manifest-link-dotdot manifest-extra not-tar truncated; do
        case "$kind" in
            not-tar) printf 'just text\n' | gzip -c > "$W/$kind.tar.gz" ;;
            truncated) mkbad "$W/full.tar.gz" fine && head -c 60 "$W/full.tar.gz" > "$W/$kind.tar.gz" ;;
            *) mkbad "$W/$kind.tar.gz" "$kind" || { fail "cannot make $kind"; continue; } ;;
        esac
        run --restore "$W/$kind.tar.gz"; echo "# $kind: $(grep error: "$W/out")"
        check "$kind: refused" [ "$RC" != 0 ]
        check "$kind: says nothing was changed" said "nothing was changed"
        tree "$E" > "$W/after"
        check "$kind: the home is as it was" same "$W/before" "$W/after"
        check "$kind: no home was moved aside, no temporary directory left" \
            [ -z "$(find "$HOME" -maxdepth 1 \( -name '.erssi.before-restore-*' -o -name '.erssi-restore.*' \))" ]
        check "$kind: nothing written outside" sh -c "[ ! -e '$W/outside' ] && [ ! -e '$HOME/escaped' ] && [ ! -e '$W/escaped' ] && [ ! -e /tmp/erssi-backup-test-absolute ]"
    done
    ;;

running)
    command -v perl >/dev/null 2>&1 || { echo "skip: no perl"; exit 77; }
    make_home "$E"
    run --backup "$W/b.tar.gz"
    check "backup succeeds" [ "$RC" = 0 ]
    mkdir -p "$W/target"
    make_home "$W/target"
    rm -f "$W/target/erssi.lock"
    : > "$W/target/erssi.lock"
    tree "$W/target" > "$W/before"
    # an erssi from 2026.11.0 on: the shared lock on erssi.lock, held
    # until it is killed (however slow the checks below are)
    perl -e 'open(my $f, "<", $ARGV[0]) or die; flock($f, 1) or die; print "locked\n"; $| = 1; sleep 3600' "$W/target/erssi.lock" > "$W/locker" &
    LOCKER=$!; BG="$BG $LOCKER"
    up "$W/locker"
    run --restore "$W/b.tar.gz" --home "$W/target"
    check "restore is refused while the lock is held" [ "$RC" != 0 ]
    check "it says an erssi is running" said "an erssi is running on"
    tree "$W/target" > "$W/after"
    check "the home is as it was" same "$W/before" "$W/after"
    # the same check with flock(1) or lockf(1) where there is no perl
    for tool in flock lockf; do
        command -v "$tool" >/dev/null 2>&1 || continue
        mkdir "$W/no-perl-$tool"
        for t in sh awk sed grep date mktemp ls cut tr head sort rm mkdir cp ln chmod cat find gzip od wc df du id ps readlink env uname basename dirname touch "$tool"; do
            p=$(command -v "$t") && ln -s "$p" "$W/no-perl-$tool/$t"
        done
        run_env PATH="$W/shim:$W/no-perl-$tool" -- --restore "$W/b.tar.gz" --home "$W/target"
        check "with $tool: refused while the lock is held" [ "$RC" != 0 ] && said "an erssi is running on"
        check "with $tool: the lock file stays" [ -f "$W/target/erssi.lock" ]
    done
    reap "$LOCKER"
    # an older erssi, without the lock: an erssi process naming the home
    # a perl named erssi; started through a link, FreeBSD would name it
    # perl (its file), Linux erssi (the link): one of each
    # (it says so once it runs as erssi: until then the process is the
    # shell that starts it, with the shell's arguments)
    cp "$(command -v perl)" "$W/shim/erssi"
    "$W/shim/erssi" -e '$| = 1; print "up\n"; sleep 3600' -- "--home=$W/target" > "$W/old-up" &
    OLD=$!; BG="$BG $OLD"
    up "$W/old-up"
    run --restore "$W/b.tar.gz" --home "$W/target"
    check "restore is refused while an erssi process names the home" [ "$RC" != 0 ]
    check "it names the pid" said "pid $OLD"
    if [ -r /proc/self/status ]; then
        # no ps (a minimal Linux without procps): read from /proc
        mkdir "$W/no-ps"
        for t in sh awk sed grep date mktemp ls cut tr head tail sort rm mkdir cp mv ln chmod cat find gzip od wc df du id readlink env uname basename dirname touch perl; do
            p=$(command -v "$t") && ln -s "$p" "$W/no-ps/$t"
        done
        run_env PATH="$W/shim:$W/no-ps" -- --restore "$W/b.tar.gz" --home "$W/target"
        check "without ps, the process is found in /proc" [ "$RC" != 0 ] && said "pid $OLD"
    fi
    reap "$OLD"
    mkdir "$W/link"
    ln -s "$(command -v perl)" "$W/link/erssi"
    "$W/link/erssi" -e '$| = 1; print "up\n"; sleep 3600' -- --home "$W/target" > "$W/link-up" &
    OLD=$!; BG="$BG $OLD"
    up "$W/link-up"
    run --restore "$W/b.tar.gz" --home "$W/target"
    check "also an erssi started through a link (--home DIR)" [ "$RC" != 0 ]
    check "named by its pid" said "pid $OLD"
    reap "$OLD"
    run --restore "$W/b.tar.gz" --home "$W/target"
    if [ "$RC" != 0 ]; then show; fi
    check "once it is gone, restore succeeds" [ "$RC" = 0 ]
    ;;

aside)
    make_home "$E"
    run --backup "$W/b.tar.gz"
    printf 'changed after the backup\n' > "$E/config"
    printf 'new\n' > "$E/only-here"
    run --restore "$W/b.tar.gz" --dry-run
    check "a dry run succeeds" [ "$RC" = 0 ]
    check "it lists the files" said "rpe2e/keyring.json"
    check "it lists the links" said "scripts/autorun/hello.pl -> ../hello.pl"
    check "it says where the home would go" said "move ~/.erssi aside to ~/.erssi.before-restore-"
    check "a dry run changes nothing" sh -c "[ -e '$E/only-here' ] && [ -z \"\$(find '$HOME' -maxdepth 1 -name '.erssi.before-restore-*')\" ]"
    run --restore "$W/b.tar.gz"
    if [ "$RC" != 0 ]; then show; fi
    check "restore over an existing home succeeds" [ "$RC" = 0 ]
    aside=$(find "$HOME" -maxdepth 1 -name '.erssi.before-restore-*')
    check "the old home is moved aside" [ -n "$aside" ] && [ -f "$aside/only-here" ]
    check "it says where" said "The home before is in ~/.erssi.before-restore-"
    check "the old home keeps its files" grep -q 'changed after' "$aside/config"
    check "the restored home has the backed up config" sh -c "! grep -q 'changed after' '$E/config'"
    check "and not the file made after the backup" [ ! -e "$E/only-here" ]
    # not an erssi home: never moved aside
    mkdir -p "$W/docs"; printf 'x\n' > "$W/docs/letter"
    run --restore "$W/b.tar.gz" --home "$W/docs"
    check "a directory that is no erssi home is refused" [ "$RC" != 0 ] && said "does not look like an erssi home"
    check "and kept" [ -f "$W/docs/letter" ]
    run --restore "$W/b.tar.gz" --home "$HOME"
    check "the home directory itself is refused" [ "$RC" != 0 ]
    # a backup of a newer erssi: a warning, restored all the same
    run --backup "$W/newer.tar.gz" --erssi-version 2099.1.0
    run --restore "$W/newer.tar.gz" --home "$W/n/.erssi" --erssi-version 2026.11.0
    check "a backup of a newer erssi is restored" [ "$RC" = 0 ]
    check "with a warning" said "newer than this erssi 2026.11.0"
    ;;

names)
    # names that are not ASCII: tar lists them as \ooo in the C locale,
    # always so when they are not UTF-8 (a Latin-2 byte)
    make_home "$E"
    L2=$(printf 'l\261.txt')
    printf 'utf8\n' > "$E/zażółć.txt"
    mkdir "$E/scripts/pełne"
    printf 'dir\n' > "$E/scripts/pełne/ąę.pl"
    printf 'latin2\n' > "$E/$L2"
    for pair in "C C" "C $PL" "$PL C"; do
        # shellcheck disable=SC2086 # two words
        set -- $pair
        if [ -z "${2:-}" ]; then echo "skip half: no pl_PL UTF-8 locale"; continue; fi
        rm -rf "$W/new" "$W/b.tar.gz"
        RC=0; LC_ALL=$1 sh "$INSTALLER" --backup "$W/b.tar.gz" > "$W/out" 2>&1 || RC=$?
        if [ "$RC" != 0 ]; then show; fi
        check "backup with LC_ALL=$1 succeeds" [ "$RC" = 0 ]
        RC=0; LC_ALL=$2 sh "$INSTALLER" --restore "$W/b.tar.gz" --home "$W/new/.erssi" > "$W/out" 2>&1 || RC=$?
        if [ "$RC" != 0 ]; then show; fi
        check "and restore with LC_ALL=$2 ($1 backup) succeeds" [ "$RC" = 0 ]
        check "a Latin-2 name comes back byte for byte" same "$E/$L2" "$W/new/.erssi/$L2"
        check "UTF-8 names too" same "$E/scripts/pełne/ąę.pl" "$W/new/.erssi/scripts/pełne/ąę.pl"
        check "and the rest" same "$E/zażółć.txt" "$W/new/.erssi/zażółć.txt"
    done
    ;;

locale-hardlink)
    [ -n "$PL" ] || { echo "skip: no pl_PL UTF-8 locale (where tar does not say \"link to\")"; exit 77; }
    make_home "$E"
    ln "$E/config" "$E/config.hard"
    mkdir -p "$W/outside"; printf 'x\n' > "$W/outside/one"
    ln -s "$W/outside/one" "$E/a.txt"; ln -s "$W/outside/one" "$E/b.txt"
    RC=0; LC_ALL=$PL LANG=$PL sh "$INSTALLER" --backup "$W/b.tar.gz" > "$W/out" 2>&1 || RC=$?
    if [ "$RC" != 0 ]; then show; fi
    check "backup in a Polish locale succeeds" [ "$RC" = 0 ]
    check "no hard link is stored" sh -c "! gzip -dc '$W/b.tar.gz' | LC_ALL=C '$REAL_TAR' -tvf - | grep -q '^h'"
    RC=0; LC_ALL=$PL LANG=$PL sh "$INSTALLER" --restore "$W/b.tar.gz" --home "$W/new/.erssi" > "$W/out" 2>&1 || RC=$?
    if [ "$RC" != 0 ]; then show; fi
    check "and it restores" [ "$RC" = 0 ]
    check "both names are files" sh -c "[ -f '$W/new/.erssi/config.hard' ] && [ -f '$W/new/.erssi/b.txt' ]"
    ;;

manifest-links)
    make_home "$E"
    tree "$E" > "$W/before"
    for kind in links-dup links-dup-same links-dup-file-first links-below links-through; do
        mkbad "$W/$kind.tar.gz" "$kind"
        run --restore "$W/$kind.tar.gz"
        echo "# $kind: $(grep error: "$W/out")"
        check "$kind: refused" [ "$RC" != 0 ]
        check "$kind: says nothing was changed" said "nothing was changed"
        tree "$E" > "$W/after"
        check "$kind: the home is as it was" same "$W/before" "$W/after"
        check "$kind: nothing made next to the home" [ ! -e "$HOME/f" ] && [ ! -L "$HOME/f" ] && [ ! -e "$W/f" ]
        check "$kind: no temporary directory left" [ -z "$(find "$HOME" -maxdepth 1 -name '.erssi-restore.*')" ]
    done
    mkbad "$W/fine.tar.gz" links-fine
    run --restore "$W/fine.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "two links in one directory tree are made" [ "$RC" = 0 ]
    check "each where it belongs" sh -c "[ -d '$W/new/.erssi/a1/a2/a3/L' ] && [ -f '$W/new/.erssi/a1/a2/K' ] && [ -z \"\$(find '$W/new/.erssi/sub' -type l)\" ]"
    ;;

file-changed)
    make_home "$E"
    run_env FAKE_TAR_CHANGED="logs/IRCnet/#chan.log logs/mentions.log" -- --backup "$W/b.tar.gz" --with-logs
    if [ "$RC" != 0 ]; then show; fi
    check "a log that grew while it was read does not stop the backup" [ "$RC" = 0 ]
    check "it is named" said "logs/IRCnet/#chan.log changed while it was read"
    check "the other one too" said "logs/mentions.log changed while it was read"
    check "the backup is there" [ -f "$W/b.tar.gz" ]
    run_env FAKE_TAR_CHANGED="logs/mentions.log" FAKE_TAR_ERROR="config" -- --backup "$W/c.tar.gz"
    check "with another error from tar the backup fails" [ "$RC" != 0 ]
    check "and nothing is written" [ ! -e "$W/c.tar.gz" ]
    # a real one: GNU tar, a log appended to while it is read
    if "$REAL_TAR" --version 2>/dev/null | grep -q 'GNU tar'; then
        i=0; while [ $i -lt 2000 ]; do printf '%s\n' "line $i of a long log, written before the backup" >> "$E/logs/IRCnet/#big.log"; i=$((i + 1)); done
        ( while :; do printf 'more\n' >> "$E/logs/IRCnet/#big.log"; done ) &
        APP=$!; BG="$BG $APP"
        run --backup "$W/d.tar.gz" --with-logs
        reap "$APP"
        if [ "$RC" != 0 ]; then show; fi
        check "a log written to during a real backup does not stop it" [ "$RC" = 0 ]
    fi
    ;;

lock-unknown)
    # a file system without locks: perl's flock fails with ENOLCK. That
    # says nothing about an erssi: the process list decides, with a word
    command -v perl >/dev/null 2>&1 || { echo "skip: no perl"; exit 77; }
    make_home "$E"
    run --backup "$W/b.tar.gz"
    mkdir -p "$W/perllib"
    cat > "$W/perllib/NoLocks.pm" <<'PM'
package NoLocks;
use Errno;
BEGIN { *CORE::GLOBAL::flock = sub { $! = Errno::ENOLCK(); return 0 }; }
1;
PM
    run_env PERL5LIB="$W/perllib" PERL5OPT="-MNoLocks" -- --restore "$W/b.tar.gz"
    if [ "$RC" != 0 ]; then show; fi
    check "a lock that cannot be taken is not taken for a running erssi" [ "$RC" = 0 ]
    check "it says only the process list was checked" said "only the process list was checked"
    ;;

archive-swapped)
    # the archive replaced while --restore works on it: what is checked
    # must be what is unpacked, a copy read once
    make_home "$E"
    run --backup "$W/good.tar.gz"
    mkbad "$W/evil.tar.gz" symlink
    REAL_GZIP=$(command -v gzip)
    mkdir "$W/shim2"
    cat > "$W/shim2/gzip" <<EOF2
#!/bin/sh
for a in "\$@"; do
    if [ "\$a" = "$W/good.tar.gz" ]; then
        echo read >> "$W/gzip.orig"
        "$REAL_GZIP" "\$@"; rc=\$?
        [ -e "$W/swapped" ] || { cp "$W/evil.tar.gz" "$W/good.tar.gz"; : > "$W/swapped"; }
        exit \$rc
    fi
done
exec "$REAL_GZIP" "\$@"
EOF2
    chmod 755 "$W/shim2/gzip"
    run_env PATH="$W/shim2:$PATH" -- --restore "$W/good.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "the archive as it was when copied is restored" [ "$RC" = 0 ]
    check "gzip never read the file itself" [ ! -e "$W/gzip.orig" ]
    check "nothing of the other archive is there" [ ! -L "$W/new/.erssi/link" ] && [ ! -e "$W/outside" ]
    check "the home is the backed up one" same "$E/config" "$W/new/.erssi/config"
    ;;

closed-dirs)
    make_home "$E"
    tree "$E" > "$W/before"
    mkbad "$W/closed.tar.gz" closed
    run --restore "$W/closed.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "directories of mode 0000, 0100 and 0300 are restored" [ "$RC" = 0 ]
    check "as 0700, with their files" sh -c "for d in d0 d1 d3; do [ \"\$(ls -ld '$W/new/.erssi/'\$d | cut -c1-10)\" = drwx------ ] && [ -f '$W/new/.erssi/'\$d/x ] || exit 1; done"
    check "and the files stay 0600" sh -c "for d in d0 d1 d3; do [ \"\$(ls -ld '$W/new/.erssi/'\$d/x | cut -c1-10)\" = -rw------- ] || exit 1; done"
    check "no temporary directory is left" [ -z "$(find "$W/new" -maxdepth 1 -name '.erssi-restore.*')" ]
    mkbad "$W/closed-bad.tar.gz" closed-bad
    run --restore "$W/closed-bad.tar.gz"
    check "refused when they do not match the MANIFEST" [ "$RC" != 0 ]
    check "and the temporary directory is removed all the same" [ -z "$(find "$HOME" -maxdepth 1 -name '.erssi-restore.*')" ]
    tree "$E" > "$W/after"
    check "the home is as it was" same "$W/before" "$W/after"
    # a TMPDIR of its own: other cases running at the same time use theirs
    mkdir "$W/tmpd"
    run_env TMPDIR="$W/tmpd" -- --restore "$W/closed-bad.tar.gz" --dry-run
    check "a dry run leaves nothing in the temporary directory either" [ -z "$(find "$W/tmpd" ! -path "$W/tmpd")" ]
    ;;

swap-locked)
    # the two renames run while the restore holds the exclusive lock of
    # the old home's erssi.lock and of the new one's: a stand-in mv looks
    command -v perl >/dev/null 2>&1 || { echo "skip: no perl"; exit 77; }
    make_home "$E"
    run --backup "$W/b.tar.gz"
    REAL_MV=$(command -v mv)
    mkdir "$W/shim3"
    cat > "$W/shim3/mv" <<EOF3
#!/bin/sh
for a in "\$@"; do :; done
src=""; for b in "\$@"; do [ "\$b" = "\$a" ] || src=\$b; done
if [ -f "\$src/erssi.lock" ]; then
    if "$(command -v perl)" -e 'open(my \$f, "<", \$ARGV[0]) or exit 2; flock(\$f, 5) and exit 1; exit 0' "\$src/erssi.lock"; then
        echo "locked \$src" >> "$W/mv.log"
    else
        echo "free \$src" >> "$W/mv.log"
    fi
fi
exec "$REAL_MV" "\$@"
EOF3
    chmod 755 "$W/shim3/mv"
    run_env PATH="$W/shim3:$PATH" -- --restore "$W/b.tar.gz"
    if [ "$RC" != 0 ]; then show; fi
    check "restore over a home succeeds" [ "$RC" = 0 ]
    check "the old home is moved aside while its lock is held" grep -q "^locked $E\$" "$W/mv.log"
    check "the new home is moved in while its lock is held" grep -q "^locked .*/home\$" "$W/mv.log"
    check "never without" sh -c "! grep -q '^free' '$W/mv.log'"
    check "the lock file is in the new home" [ -f "$E/erssi.lock" ]
    # the same with flock(1) (Linux) or lockf(1) (FreeBSD) where there is
    # no perl
    for tool in flock lockf; do
        command -v "$tool" >/dev/null 2>&1 || continue
        mkdir "$W/no-perl-$tool"
        for t in sh awk sed grep date mktemp ls cut tr head sort rm mkdir cp ln chmod cat find gzip od wc df du id ps readlink env uname basename dirname touch "$tool"; do
            p=$(command -v "$t") && ln -s "$p" "$W/no-perl-$tool/$t"
        done
        : > "$W/mv.log"
        run_env PATH="$W/shim3:$W/shim:$W/no-perl-$tool" -- --restore "$W/b.tar.gz"
        if [ "$RC" != 0 ]; then show; fi
        check "without perl, with $tool: restore succeeds" [ "$RC" = 0 ]
        check "and holds both locks for the renames" [ "$(grep -c '^locked' "$W/mv.log")" = 2 ]
        check "never without them" sh -c "! grep -q '^free' '$W/mv.log'"
        check "the lock file of the home is still there" [ -f "$E/erssi.lock" ]
        check "no warning that it could not lock" sh -c "! grep -q 'could not be locked' '$W/out'"
    done
    ;;

no-diffutils)
    # a minimal system (AlmaLinux 10, Rocky) has neither cmp nor diff:
    # backup and restore with a PATH without them
    make_home "$E"
    mkdir "$W/base"
    for t in sh awk sed grep date mktemp ls cut tr head tail sort rm mkdir rmdir cp mv ln chmod cat find gzip od wc df du id ps readlink env uname basename dirname touch perl flock lockf; do
        p=$(command -v "$t") && ln -s "$p" "$W/base/$t"
    done
    check "no cmp or diff in that PATH" sh -c "[ ! -e '$W/base/cmp' ] && [ ! -e '$W/base/diff' ]"
    run_env PATH="$W/shim:$W/base" -- --backup "$W/b.tar.gz"
    if [ "$RC" != 0 ]; then show; fi
    check "backup without diffutils succeeds" [ "$RC" = 0 ]
    run_env PATH="$W/shim:$W/base" -- --restore "$W/b.tar.gz" --home "$W/new/.erssi"
    if [ "$RC" != 0 ]; then show; fi
    check "restore without diffutils succeeds" [ "$RC" = 0 ]
    check "with the files" same "$E/rpe2e/keyring.json" "$W/new/.erssi/rpe2e/keyring.json"
    check "nothing said about a missing command" sh -c "! grep -q 'not found' '$W/out'"
    run_env PATH="$W/shim:$W/base" -- --restore "$W/b.tar.gz" --home "$W/new/.erssi"
    check "over the restored home too" [ "$RC" = 0 ]
    ;;

self-check)
    make_home "$E"
    run_env FAKE_TAR_FIFO="1" -- --backup "$W/b.tar.gz"
    check "a backup --restore would refuse is not written" [ "$RC" != 0 ]
    check "the file is not there" [ ! -e "$W/b.tar.gz" ]
    check "it says why" said "does not pass the checks of --restore"
    check "no temporary file is left" [ -z "$(find "$W" -maxdepth 1 -name '.erssi-backup.*')" ]
    ;;

*) echo "unknown case $CASE"; exit 1 ;;
esac
[ "$FAILED" = 0 ]
