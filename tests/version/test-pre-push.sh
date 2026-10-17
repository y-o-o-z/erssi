#!/bin/sh
# utils/pre-push against throwaway repositories: each case is a real
# `git push` to a local bare remote, with the hook installed in the throwaway
# clone, a throwaway SSH signing key (never the release key), and HOME, the
# private patterns file and TMPDIR all inside one temporary directory.
# Build/ is a stand-in whose ninja and meson are stubs, so the hook's build
# and test step runs without building anything.
# Usage: test-pre-push.sh SOURCE_ROOT
# Exit: 0 pass, 1 fail, 77 skip.

# a clean environment: nothing from the caller's HOME, git (GIT_DIR when the
# real pre-push runs this suite) or signing setup
if [ -z "${PREPUSH_TEST_CLEAN:-}" ]; then
    exec env -i PREPUSH_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

root=$(cd "$1" && pwd) || exit 1
command -v git >/dev/null 2>&1 || { echo "skip: no git"; exit 77; }
command -v ssh-keygen >/dev/null 2>&1 || { echo "skip: no ssh-keygen"; exit 77; }

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
failed=0
n=0

pass() { echo "ok $1"; }
fail() { echo "FAIL $1: $2"; failed=1; }

EMAIL='136268860+y-o-o-z@users.noreply.github.com'
export HOME="$tmp/home" TMPDIR="$tmp/tmpdir" GIT_CONFIG_NOSYSTEM=1 LC_ALL=C
export PATH="$tmp/bin:$PATH"
mkdir -p "$HOME/.config" "$TMPDIR" "$tmp/bin" || exit 1
PATTERNS="$HOME/.config/git-private-patterns"

ssh-keygen -q -t ed25519 -N '' -C test -f "$tmp/key" || exit 1
ssh-keygen -q -t ed25519 -N '' -C other -f "$tmp/other" || exit 1
cat > "$HOME/.gitconfig" <<EOF
[user]
	name = y-o-o-z
	email = $EMAIL
	signingkey = $tmp/key
[gpg]
	format = ssh
[commit]
	gpgsign = false
[tag]
	gpgsign = false
[init]
	defaultBranch = main
EOF

# stand-ins for the build and test step: meson notes the HOME and the GIT_*
# variables it was given and, with STUB_MESON_INT=1, interrupts the hook (Ctrl-C during the tests)
printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/ninja"
cat > "$tmp/bin/meson" <<EOF
#!/bin/sh
printf '%s\n' "\$HOME" >> "$tmp/meson-homes"
env | grep '^GIT_' >> "$tmp/meson-gitenv"
[ "\${STUB_MESON_INT:-}" = 1 ] && kill -INT "\$PPID"
exit 0
EOF
chmod +x "$tmp/bin/ninja" "$tmp/bin/meson"

# newrepo: a clone in $r whose base commit is on origin/main, the hook
# installed and the default patterns file
newrepo() {
    n=$((n + 1))
    r="$tmp/r$n"
    git init -q --bare "$r.git" && git init -q "$r" || return 1
    mkdir "$r/utils"
    cp "$root/utils/check-version.sh" "$r/utils/"
    printf '%s namespaces="git" %s\n' "$EMAIL" "$(cat "$tmp/key.pub")" > "$r/utils/allowed_signers"
    printf "project('erssi', 'c',\n  version : '2026.10.0',\n" > "$r/meson.build"
    printf 'shellter-v2026.10.0 2026-10-12  y-o-o-z\n' > "$r/NEWS"
    echo base > "$r/f"
    git -C "$r" add -A && git -C "$r" commit -qS -m base &&
        git -C "$r" remote add origin "$r.git" &&
        git -C "$r" push -q --no-verify origin main 2>/dev/null || return 1
    mkdir "$r/Build" && : > "$r/Build/build.ninja"
    echo /Build/ >> "$r/.git/info/exclude"
    ln -s "$root/utils/pre-push" "$r/.git/hooks/pre-push"
    printf 'neverappearsanywhere\n' > "$PATTERNS"
}

# cm GIT_COMMIT_OPTIONS... : a commit that adds the line $LINE (default
# "change $n") to f
cm() {
    printf '%s\n' "${LINE:-change $n}" >> "$r/f"
    git -C "$r" add f && git -C "$r" commit -q "$@"
}

# push NAME ok|refused [GREP] -- REFSPEC...: git push $to REFSPEC... from $r
# ($to is origin unless a case sets it); with GREP, the output must match it
# (grep -F). Every push must leave TMPDIR empty.
to=origin
push() {
    name=$1; want=$2; shift 2
    pat=
    [ "$1" != -- ] && { pat=$1; shift; }
    shift
    out=$(cd "$r" && git push "$to" "$@" 2>&1)
    rc=$?
    case $want in
        ok) [ "$rc" = 0 ] && good=1 || good=0 ;;
        *) [ "$rc" != 0 ] && good=1 || good=0 ;;
    esac
    if [ "$good" = 1 ] && { [ -z "$pat" ] || printf '%s\n' "$out" | grep -qF -- "$pat"; }; then
        pass "$name: $want"
    else
        fail "$name" "want $want${pat:+ with \"$pat\"}, exit $rc: $(printf '%s' "$out" | tr '\n' '|')"
    fi
    left=$(ls -A "$TMPDIR")
    if [ -z "$left" ]; then
        pass "$name: no temp leftovers"
    else
        fail "$name: temp leftovers" "$left"
    fi
    rm -rf "${TMPDIR:?}"/* "${TMPDIR:?}"/.[!.]* 2>/dev/null
}

# --- commits: author, AI trailers ------------------------------------------
newrepo || exit 1
cm -S -m 'signed change'
push 'signed commit to main' ok -- main

newrepo
# the trailer is built from pieces so that this file does not carry it
cm -S -m 'x' -m "Co-Authored-By: Cl""aude <noreply@anthr""opic.com>"
push 'AI trailer' refused 'AI attribution trailer' -- main

newrepo
cm -S --author='someone <someone@example.invalid>' -m 'x'
push 'foreign author' refused 'author/committer' -- main

# --- commit signatures ----------------------------------------------------
newrepo
cm -m 'unsigned'
push 'unsigned commit to main' refused 'not signed by the release key' -- main

newrepo
cm -m 'unsigned'
push 'unsigned commit as HEAD:main' refused 'not signed by the release key' -- HEAD:main

newrepo
echo other >> "$r/f"
git -C "$r" add f && git -C "$r" -c user.signingkey="$tmp/other" commit -qS -m 'other key'
push 'commit signed by another key to main' refused 'not signed by the release key' -- main

newrepo
cm -m 'unsigned parent'
cm -S -m 'signed tip'
push 'unsigned commit under a signed tip' refused 'unsigned parent' -- main

newrepo
cm -m 'unsigned'
push 'unsigned commit to another branch' ok -- HEAD:refs/heads/dev/x
# already on origin/dev/x, but new to origin's main
push 'the same unsigned commit, then to main' refused 'not signed by the release key' -- HEAD:main

# the release flow: a private remote first, then the public main
newrepo
git init -q --bare "$r.staging.git" && git -C "$r" remote add staging "$r.staging.git"
cm -m 'unsigned'
to=staging
push 'unsigned commit to a second remote' ok -- HEAD:refs/heads/dev/y
to=origin
push 'the same unsigned commit, then to main of the first' refused 'not signed by the release key' -- HEAD:main

# the author, AI-trailer and private-data checks cover every commit the
# remote pushed to does not have, also one already on another remote: a
# pattern added after the commit went to staging ...
newrepo
git init -q --bare "$r.staging.git" && git -C "$r" remote add staging "$r.staging.git"
LINE='host secret-box.example.lan'; cm -S -m 'x'; LINE=
to=staging
push 'a commit to a second remote, before its pattern' ok -- HEAD:main
printf 'secret-box\n' > "$PATTERNS"
to=origin
push 'the same commit, then to the first, with the pattern' refused 'private data in the diff' -- HEAD:main

# ... or a push to staging that skipped the hook
newrepo
git init -q --bare "$r.staging.git" && git -C "$r" remote add staging "$r.staging.git"
cm -S --author='someone <someone@example.invalid>' -m 'x'
to=staging
push 'foreign author to a second remote, --no-verify' ok -- --no-verify HEAD:main
to=origin
push 'the same commit, then to the first' refused 'author/committer' -- HEAD:main

# a commit the remote has on another branch is not new to it
newrepo
LINE='host secret-box.example.lan'; cm -S -m 'x'; LINE=
push 'a commit to another branch, before its pattern' ok -- HEAD:refs/heads/dev/z
printf 'secret-box\n' > "$PATTERNS"
push 'the same commit, then to main of the same remote' ok -- HEAD:main

# after a history rewrite our copies of the remote's branches are rewritten
# too (git filter-repo): when they do not match what the remote reports,
# they leave nothing out
newrepo
git init -q --bare "$r.staging.git" && git -C "$r" remote add staging "$r.staging.git"
cm -S -m 'signed'
to=staging
push 'signed main to a second remote' ok -- main
git -C "$r" commit -q --amend -m 'rewritten, unsigned'
git -C "$r" update-ref refs/remotes/staging/main HEAD
push 'rewritten main over a rewritten copy of the remote main' refused 'not signed by the release key' -- --force main
# ... also when the remote's old tip is gone from here (filter-repo prunes it)
git -C "$r" reflog expire --expire=now --all && git -C "$r" gc -q --prune=now
push 'the same, the remote tip not here any more' refused 'not signed by the release key' -- --force main
to=origin

# a push to a URL instead of a remote name: that remote's branches count
newrepo
cm -S -m 'x'
to=$r.git
push 'signed commit to main by URL' ok -- HEAD:main
to=origin
cm -S --author='someone <someone@example.invalid>' -m 'theirs'
push 'foreign author to a branch, --no-verify' ok -- --no-verify HEAD:refs/heads/dev/f
to=$r.git
push 'a new branch by URL over what the remote has' ok -- HEAD:refs/heads/dev/g
to=origin

# commits of the upstream projects (irssi, erssi-org) are theirs: on a
# remote that does not have them yet they are not checked
upstream_repo() {   # a repository with one commit by someone else, as remote $1
    git init -q "$r.up" &&
        echo up > "$r.up/u" && git -C "$r.up" add u &&
        git -C "$r.up" -c user.name=up -c user.email=up@example.invalid commit -q -m 'upstream' &&
        git -C "$r" remote add "$1" "$r.up" && git -C "$r" fetch -q "$1" 2>/dev/null &&
        git -C "$r" merge -q -S --allow-unrelated-histories -m 'merge upstream' "$1/main" >/dev/null
}
newrepo
upstream_repo upstream
git init -q --bare "$r.fresh.git" && git -C "$r" remote add fresh "$r.fresh.git"
to=fresh
push 'upstream history to a new remote' ok -- main
push 'upstream history as a new branch' ok -- HEAD:refs/heads/dev/u
to=origin

newrepo
upstream_repo theirs
git init -q --bare "$r.fresh.git" && git -C "$r" remote add fresh "$r.fresh.git"
to=fresh
push 'upstream history from a remote not named as upstream' refused 'author/committer' -- main
git -C "$r" config prepush.upstream 'erssi-org theirs'
push 'the same with prepush.upstream naming it' ok -- main
# never a remote with the fork's own URL, or with the destination's (each
# push to a new remote that does not have the history yet)
fresh_remote() { git init -q --bare "$r.$1.git" && git -C "$r" remote add "$1" "$r.$1.git"; to=$1; }
git -C "$r" remote set-url theirs https://github.com/y-o-o-z/erssi-dev.git
fresh_remote fresh2
push 'an upstream remote with the fork'"'"'s URL' refused 'author/committer' -- main
git -C "$r" remote set-url theirs "$r.up"
git -C "$r" remote set-url --push theirs git@github.com:Y-O-O-Z/erssi.git
push '... as its push URL' refused 'author/committer' -- main
git -C "$r" config --unset remote.theirs.pushurl
fresh_remote fresh3
git -C "$r" remote set-url theirs "$r.fresh3.git"
push 'an upstream remote with the destination'"'"'s URL' refused 'author/committer' -- main
git -C "$r" remote set-url theirs "$r.up"
push 'the same upstream remote with its own URL again' ok -- main
to=origin

# main created on an empty remote: everything it gets is new to it
newrepo
git init -q --bare "$r.fresh.git" && git -C "$r" remote add fresh "$r.fresh.git"
cm -S -m 'signed'
to=fresh
push 'signed history to an empty remote main' ok -- main
to=origin

newrepo
git init -q --bare "$r.fresh.git" && git -C "$r" remote add fresh "$r.fresh.git"
cm -m 'unsigned'
to=fresh
push 'unsigned commit to an empty remote main' refused 'not signed by the release key' -- main
to=origin

# main moved on the remote since our last fetch (its tip is unknown here):
# a force push is checked against our copy of the remote's main
newrepo
git clone -q "$r.git" "$r.other" &&
    echo elsewhere > "$r.other/g" && git -C "$r.other" add g &&
    git -C "$r.other" commit -qS -m elsewhere &&
    git -C "$r.other" push -q origin main 2>/dev/null
cm -m 'unsigned'
push 'unsigned commit, force push over an unknown main' refused 'not signed by the release key' -- --force main

# many unsigned commits: the first 10 are named, then it stops
newrepo
i=0
while [ "$i" -lt 12 ]; do cm -m "unsigned $i"; i=$((i + 1)); done
push 'twelve unsigned commits to main' refused 'stopped after 10' -- main

# a release tag and main in one push: each ref is checked
newrepo
cm -m 'unsigned'
git -C "$r" tag -s -m 'release' shellter-v2026.10.0
push 'release tag and unsigned main in one push' refused 'not signed by the release key' -- refs/tags/shellter-v2026.10.0 main

# a deleted branch: nothing is created on the remote
newrepo
cm -m 'unsigned'
push 'branch for the delete' ok -- HEAD:refs/heads/dev/x
push 'delete a branch' ok -- :refs/heads/dev/x

# --- release tags ---------------------------------------------------------
newrepo
git -C "$r" tag -s -m 'release' shellter-v2026.10.0
push 'signed release tag' ok -- refs/tags/shellter-v2026.10.0

newrepo
cm -m 'unsigned'
git -C "$r" tag -s -m 'release' shellter-v2026.10.0
push 'signed release tag of an unsigned commit' refused 'release commit' -- refs/tags/shellter-v2026.10.0

# a release tag on the remote is never moved or deleted, even signed and
# forced; nor is one that shellter-install.sh records pushed on another
# commit (a delete that skipped the hook, then the tag again)
newrepo
base=$(git -C "$r" rev-parse HEAD)
git -C "$r" tag -s -m 'release' shellter-v2026.10.0
push 'release tag for the move' ok -- refs/tags/shellter-v2026.10.0
printf 'RELEASES="\nshellter-v2026.10.0 %s signed\n"\n' "$base" > "$r/shellter-install.sh"
git -C "$r" add shellter-install.sh && git -C "$r" commit -qS -m 'pin'
cm -S -m 'a fix'
push 'the pin and the fix to main' ok -- main
git -C "$r" tag -f -s -m 'release' shellter-v2026.10.0 >/dev/null
push 'release tag moved' refused 'release tags never move' -- --force refs/tags/shellter-v2026.10.0
push 'release tag deleted' refused 'release tags are never deleted' -- :refs/tags/shellter-v2026.10.0
push 'release tag deleted, the hook skipped' ok -- --no-verify :refs/tags/shellter-v2026.10.0
push 'release tag pushed again on another commit' refused 'shellter-install.sh records' -- refs/tags/shellter-v2026.10.0
git -C "$r" tag -f -s -m 'release' shellter-v2026.10.0 "$base" >/dev/null
push 'release tag pushed again on its recorded commit' ok -- refs/tags/shellter-v2026.10.0

# a one-time re-signing: PREPUSH_RETAG names the release tags that may move
# or be deleted; every other check still runs
newrepo
git -C "$r" tag -s -m 'release' shellter-v2026.10.0
git -C "$r" tag -s -m 'release' shellter-v1.3.2
push 'release tags for the re-signing' ok -- refs/tags/shellter-v2026.10.0 refs/tags/shellter-v1.3.2
cm -S -m 'rewritten'
git -C "$r" tag -f -s -m 'release' shellter-v2026.10.0 >/dev/null
git -C "$r" tag -f -s -m 'release' shellter-v1.3.2 >/dev/null
export PREPUSH_RETAG=shellter-v2026.10.1
push 're-signing, another tag named' refused 'release tags never move' -- --force refs/tags/shellter-v2026.10.0
export PREPUSH_RETAG=shellter-v2026.10.0
push 're-signing, only one of two tags named' refused 'shellter-v1.3.2: release tags never move' -- --force refs/tags/shellter-v2026.10.0 refs/tags/shellter-v1.3.2
export PREPUSH_RETAG='shellter-v2026.10.0 shellter-v1.3.2'
git -C "$r" tag -f -s -u "$tmp/other" -m 'release' shellter-v2026.10.0 >/dev/null
push 're-signing, a tag signed by another key' refused 'not signed by the release key' -- --force refs/tags/shellter-v2026.10.0
cm -m 'unsigned'
git -C "$r" tag -f -s -m 'release' shellter-v2026.10.0 >/dev/null
push 're-signing, a tag of an unsigned commit' refused 'release commit' -- --force refs/tags/shellter-v2026.10.0
git -C "$r" tag -f shellter-v2026.10.0 >/dev/null
push 're-signing, a lightweight tag' refused 'must be annotated' -- --force refs/tags/shellter-v2026.10.0
git -C "$r" reset -q --hard HEAD^
# the pins of this checkout (the new ones) still count: a re-signed tag on
# another commit than they record is refused
x=$(git -C "$r" rev-parse HEAD)
printf 'RELEASES="\nshellter-v2026.10.0 %s signed\n"\n' "$(git -C "$r" rev-parse HEAD^)" > "$r/shellter-install.sh"
git -C "$r" add shellter-install.sh && git -C "$r" commit -qS -m 'new pins'
git -C "$r" tag -f -s -m 'release' shellter-v2026.10.0 "$x" >/dev/null
push 're-signing, a tag elsewhere than the new pins' refused 'shellter-install.sh records' -- --force refs/tags/shellter-v2026.10.0
printf 'RELEASES="\nshellter-v2026.10.0 %s signed\n"\n' "$x" > "$r/shellter-install.sh"
git -C "$r" add shellter-install.sh && git -C "$r" commit -qS -m 'new pins, right'
push 're-signing, both named' ok 'PREPUSH_RETAG' -- --force refs/tags/shellter-v2026.10.0 refs/tags/shellter-v1.3.2
push 're-signing, a named tag deleted' ok 'PREPUSH_RETAG' -- :refs/tags/shellter-v1.3.2
unset PREPUSH_RETAG

newrepo
git -C "$r" tag shellter-v2026.10.0
push 'lightweight release tag' refused 'must be annotated' -- refs/tags/shellter-v2026.10.0

newrepo
push 'HEAD as a release tag' refused 'must be annotated' -- HEAD:refs/tags/shellter-v2026.10.0

newrepo
git -C "$r" tag -s -m 'not a release' mytag
push 'signed tag pushed under a release name' refused 'names another tag' -- mytag:refs/tags/shellter-v2026.10.0

newrepo
# an unsigned tag object that names the release, behind another local name
git -C "$r" tag -a -m 'release' shellter-v2026.10.0 &&
    git -C "$r" tag x shellter-v2026.10.0 && git -C "$r" tag -d shellter-v2026.10.0 >/dev/null
push 'unsigned release tag under another local name' refused 'not signed by the release key' -- x:refs/tags/shellter-v2026.10.0

newrepo
git -C "$r" tag -s -u "$tmp/other" -m 'release' shellter-v2026.10.0
push 'release tag signed by another key' refused 'not signed by the release key' -- refs/tags/shellter-v2026.10.0

newrepo
git -C "$r" tag -s -m 'release' shellter-v2026.11.0
push 'release tag that NEWS does not name' refused 'check-version.sh refused' -- refs/tags/shellter-v2026.11.0

newrepo
git -C "$r" tag -s -m 'a tree' shellter-v2026.10.0 'HEAD^{tree}'
push 'release tag of a tree' refused 'does not point to a commit' -- refs/tags/shellter-v2026.10.0

# --- private patterns -----------------------------------------------------
newrepo
printf '#chan\n' > "$PATTERNS"
LINE='join #chan now'; cm -S -m 'x'; LINE=
push '#chan is a pattern' refused 'private data in the diff' -- main

newrepo
printf '# a comment\n#\n\nsecretword\n' > "$PATTERNS"
LINE='x # a comment, and a # alone'; cm -S -m 'x'; LINE=
push '"# comment" and "#" are comments' ok -- main

newrepo
printf '# a comment\n#\n\nsecretword\n' > "$PATTERNS"
LINE='the secretword'; cm -S -m 'x'; LINE=
push 'a pattern after comments' refused 'private data in the diff' -- main

newrepo
: > "$PATTERNS"
cm -S -m 'x'
push 'empty patterns file' refused 'PREPUSH_NO_PATTERNS=1' -- main

newrepo
printf '# only a comment\n\n' > "$PATTERNS"
cm -S -m 'x'
push 'comment-only patterns file' refused 'PREPUSH_NO_PATTERNS=1' -- main

newrepo
rm -f "$PATTERNS"
cm -S -m 'x'
push 'missing patterns file' refused 'PREPUSH_NO_PATTERNS=1' -- main

newrepo
: > "$PATTERNS"
cm -S -m 'x'
export PREPUSH_NO_PATTERNS=1
push 'empty patterns file, PREPUSH_NO_PATTERNS=1' ok 'check skipped' -- main
unset PREPUSH_NO_PATTERNS

newrepo
printf '(\n' > "$PATTERNS"
cm -S -m 'x'
push 'invalid pattern' refused 'invalid pattern' -- main

# --- temporary files ------------------------------------------------------
newrepo
cm -S -m 'x'
export STUB_MESON_INT=1
push 'interrupted during the tests' refused -- main
unset STUB_MESON_INT

# --- a push from a linked worktree: git gives the hook GIT_DIR there -------
newrepo
git -C "$r" worktree add -q -b wt "$r.wt" 2>/dev/null
r="$r.wt"
mkdir "$r/Build" && : > "$r/Build/build.ninja"
cm -S -m 'x'
push 'from a linked worktree' ok -- HEAD:main

# the hook never gave meson the real HOME, only a throwaway one
if [ -s "$tmp/meson-homes" ] && ! grep -qv "^$TMPDIR/" "$tmp/meson-homes"; then
    pass "meson ran with a throwaway HOME"
else
    fail "meson ran with a throwaway HOME" "$(tr '\n' ' ' < "$tmp/meson-homes" 2>/dev/null)"
fi

# nor git's GIT_DIR and the like: a test that runs git (git init, git commit
# in a scratch directory) would work on the pushing repository instead
if [ ! -s "$tmp/meson-gitenv" ]; then
    pass "meson ran without GIT_* variables"
else
    fail "meson ran without GIT_* variables" "$(sort -u "$tmp/meson-gitenv" | tr '\n' ' ')"
fi

exit "$failed"
