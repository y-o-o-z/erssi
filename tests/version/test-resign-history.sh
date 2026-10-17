#!/bin/sh
# utils/resign-history.sh end to end, on a small synthetic repository: an
# upstream project (one of its commits signed by someone else's key), a fork
# with a branch merged into main, an upstream merge, two release tags and
# their pins, a file to drop from the history and docs citing commit ids.
# A real git filter-repo rewrite, the script, then the push commands it
# prints, through utils/pre-push, to local bare remotes. A throwaway key,
# HOME and TMPDIR inside one temporary directory.
# Usage: test-resign-history.sh SOURCE_ROOT
# Exit: 0 pass, 1 fail, 77 skip.

if [ -z "${RESIGN_TEST_CLEAN:-}" ]; then
    exec env -i RESIGN_TEST_CLEAN=1 PATH="$PATH" TMPDIR="${TMPDIR:-/tmp}" sh "$0" "$@"
fi

root=$(cd "$1" && pwd) || exit 1
command -v git >/dev/null 2>&1 || { echo "skip: no git"; exit 77; }
command -v ssh-keygen >/dev/null 2>&1 || { echo "skip: no ssh-keygen"; exit 77; }
git filter-repo --version >/dev/null 2>&1 || { echo "skip: no git filter-repo"; exit 77; }

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

EMAIL='136268860+y-o-o-z@users.noreply.github.com'
export HOME="$tmp/home" TMPDIR="$tmp/tmpdir" GIT_CONFIG_NOSYSTEM=1 LC_ALL=C
mkdir -p "$HOME/.config" "$TMPDIR" || exit 1
printf 'neverappearsanywhere\n' > "$HOME/.config/git-private-patterns"
ssh-keygen -q -t ed25519 -N '' -C test -f "$tmp/key" || exit 1
ssh-keygen -q -t ed25519 -N '' -C other -f "$tmp/other" || exit 1
# the identity and the signing key only in the old clone (as on the
# owner's machine): the script must take them over into the new one
cat > "$HOME/.gitconfig" <<EOF
[commit]
	gpgsign = false
[tag]
	gpgsign = false
[init]
	defaultBranch = main
[advice]
	detachedHead = false
EOF
# git with signatures checked against the throwaway key only
signers="$tmp/allowed_signers"
printf '%s namespaces="git" %s\n' "$EMAIL" "$(cat "$tmp/key.pub")" > "$signers"
v() {
    git -c gpg.ssh.allowedSignersFile="$signers" -c gpg.program=false -c gpg.openpgp.program=false \
        -c gpg.x509.program=false "$@" >/dev/null 2>&1
}

# every commit gets its own author and committer date (author earlier)
t=1790000000
at() { t=$((t + 100)); GIT_AUTHOR_DATE="$t +0200"; GIT_COMMITTER_DATE="$((t + 7)) +0200"; export GIT_AUTHOR_DATE GIT_COMMITTER_DATE; }
# up MESSAGE [GIT_OPTIONS]: an upstream commit by someone else
up() {
    at; m=$1; shift
    echo "$m" >> "$tmp/up/u" && git -C "$tmp/up" add u &&
        git -C "$tmp/up" -c user.name=up -c user.email=up@example.invalid "$@" commit -q -m "$m"
}
# fc MESSAGE FILE TEXT: a signed fork commit that appends TEXT to FILE
fc() {
    at
    mkdir -p "$(dirname "$tmp/old/$2")" && printf '%s\n' "$3" >> "$tmp/old/$2" &&
        git -C "$tmp/old" add "$2" && git -C "$tmp/old" commit -q -S -m "$1"
}
# release VERSION: meson.build and NEWS name it, a signed commit, the tag
release() {
    at
    printf "project('erssi', 'c',\n  version : '%s',\n" "$1" > "$tmp/old/meson.build"
    printf 'shellter-v%s 2026-10-12  y-o-o-z\n' "$1" > "$tmp/old/NEWS"
    git -C "$tmp/old" add meson.build NEWS && git -C "$tmp/old" commit -q -S -m "erssi Shellter Edition $1"
    at
    git -C "$tmp/old" tag -s -m "erssi Shellter Edition $1" "shellter-v$1"
}

# --- the history before the rewrite ----------------------------------------
git init -q "$tmp/up"
up 'upstream one'
up 'upstream two, signed by its author' -c gpg.format=ssh -c user.signingkey="$tmp/other" -c commit.gpgsign=true
git clone -q "$tmp/up" "$tmp/old" && git -C "$tmp/old" remote rename origin upstream
git -C "$tmp/old" config user.name y-o-o-z && git -C "$tmp/old" config user.email "$EMAIL" &&
    git -C "$tmp/old" config user.signingkey "$tmp/key" && git -C "$tmp/old" config gpg.format ssh
mkdir -p "$tmp/old/utils" "$tmp/old/doc" && cp "$signers" "$tmp/old/utils/allowed_signers" &&
    cp "$root/utils/check-version.sh" "$root/utils/pre-push" "$tmp/old/utils/"
# a file with no final newline and no commit id: never touched
printf 'no ids, no final newline' > "$tmp/old/doc/nonl.md"
printf 'RELEASES="\n"\n' > "$tmp/old/shellter-install.sh"
git -C "$tmp/old" add utils doc shellter-install.sh
fc 'fork: the start' secret.txt 'a secret that must go'
fc 'fork: a feature' f 'feature'
git -C "$tmp/old" checkout -q -b side
fc 'side: work' g 'side work'
git -C "$tmp/old" checkout -q main
fc 'fork: more' f 'more'
at
git -C "$tmp/old" merge -q --no-ff -S -m 'Merge branch side' side
release 2026.10.0
up 'upstream three'
git -C "$tmp/old" fetch -q upstream
at
git -C "$tmp/old" merge -q --no-ff -S -m 'Merge upstream' upstream/main
echo 'more secret' >> "$tmp/old/secret.txt" && git -C "$tmp/old" add secret.txt
fc 'fork: the secret again, and more' f 'and more'
# a commit that only changes the dropped file: filter-repo drops it
fc 'fork: the secret only' secret.txt 'only secret'
drop=$(git -C "$tmp/old" rev-parse HEAD)
release 2026.10.1
r0=$(git -C "$tmp/old" rev-parse 'shellter-v2026.10.0^{commit}')
r1=$(git -C "$tmp/old" rev-parse 'shellter-v2026.10.1^{commit}')
feat=$(git -C "$tmp/old" log --format=%H --grep='^fork: a feature$' main)
u1=$(git -C "$tmp/up" rev-parse 'main~2')
u2=$(git -C "$tmp/up" rev-parse 'main~1')
printf 'RELEASES="\nshellter-v2026.10.0 %s signed\nshellter-v2026.10.1 %s signed\n"\n' "$r0" "$r1" > "$tmp/old/shellter-install.sh"
printf '# Upstream\n\n- %s the feature; %s in full; upstream %s; dropped %s.\n' \
    "$(echo "$feat" | cut -c1-8)" "$feat" "$(echo "$u1" | cut -c1-8)" "$(echo "$drop" | cut -c1-8)" > "$tmp/old/docs.md"
git -C "$tmp/old" add shellter-install.sh docs.md
fc 'shellter-install.sh: pins' f 'pins'
git init -q --bare "$tmp/origin.git" && git init -q --bare "$tmp/staging.git"
git -C "$tmp/old" remote add origin "$tmp/origin.git" && git -C "$tmp/old" remote add staging "$tmp/staging.git"
git -C "$tmp/old" config --add remote.origin.pushurl "$tmp/origin.git"
for rr in origin staging; do
    git -C "$tmp/old" push -q "$rr" main 'refs/tags/shellter-v*:refs/tags/shellter-v*' 2>/dev/null || { echo "FAIL setup push"; exit 1; }
done
# a branch on staging with the old history
git -C "$tmp/old" push -q staging main:refs/heads/dev/x 2>/dev/null
git -C "$tmp/old" fetch -q origin && git -C "$tmp/old" fetch -q staging
unset GIT_AUTHOR_DATE GIT_COMMITTER_DATE

# --- the rewrite --------------------------------------------------------------
new="$tmp/new"
git clone -q --no-local "$tmp/origin.git" "$new" || exit 1
# staging fetched before the rewrite: filter-repo rewrites its copies too
git -C "$new" remote add staging "$tmp/staging.git" && git -C "$new" fetch -q staging
# a local clone is not taken for a fresh one; the real procedure clones
# from GitHub and needs no --force
(cd "$new" && git filter-repo --quiet --force --invert-paths --path secret.txt) > "$tmp/fr.out" 2>&1 ||
    { echo "FAIL git filter-repo"; cat "$tmp/fr.out"; exit 1; }
# shellcheck disable=SC2016 # run by sh
check "filter-repo dropped the signatures" sh -c '! git -C "$1" -c gpg.ssh.allowedSignersFile="$2" verify-commit main >/dev/null 2>&1' sh "$new" "$signers"

# without the upstream remote: refused, nothing changed
fr_main=$(git -C "$new" rev-parse main)
(cd "$new" && sh "$root/utils/resign-history.sh" "$tmp/old") > "$tmp/out" 2>&1
check "no upstream remote: refused" [ $? != 0 ]
check "... named" grep -q 'no branches of an upstream remote' "$tmp/out"
check "... main unchanged" [ "$(git -C "$new" rev-parse main)" = "$fr_main" ]

git -C "$new" remote add upstream "$tmp/up" && git -C "$new" fetch -q upstream
(cd "$new" && sh "$root/utils/resign-history.sh" "$tmp/old") > "$tmp/out" 2>&1
RC=$?
check "resign-history: done" [ "$RC" = 0 ]
[ "$RC" = 0 ] || cat "$tmp/out"

# --- the history after -----------------------------------------------------
check "the file is gone from the history" [ -z "$(git -C "$new" log --format=%H main -- secret.txt)" ]
fork=$(git -C "$new" rev-list main --not --remotes=upstream)
bad=0
for c in $fork; do v -C "$new" verify-commit "$c" || bad=$((bad + 1)); done
check "every fork commit is signed by the release key" [ "$bad" = 0 ]
# the old ones, less the dropped commit, and the commit with the new pins
check "... all of them, and the commit with the new pins" \
    [ "$(echo "$fork" | wc -l | tr -d ' ')" = "$(git -C "$tmp/old" rev-list --count main --not --remotes=upstream)" ]
check "upstream commits keep their ids" git -C "$new" merge-base --is-ancestor "$u2" main
check "... the one signed by its author too" [ "$(git -C "$new" rev-list main --count --not "$u2")" -gt 0 ]
check "merges kept" [ "$(git -C "$new" rev-list --merges --count main)" = "$(git -C "$tmp/old" rev-list --merges --count main)" ]
# one record per commit; the dropped commit is not in the new history
fmt='%x01%an|%ae|%ad|%cn|%ce|%cd|%B'
git -C "$tmp/old" log --date=raw --topo-order --format="$fmt" main |
    awk 'BEGIN { RS = "\001" } NR > 1 && $0 !~ /\|fork: the secret only\n/ { printf "%s", $0 }' > "$tmp/old.log"
git -C "$new" log --date=raw --topo-order --format="$fmt" main^ |
    awk 'BEGIN { RS = "\001" } NR > 1 { printf "%s", $0 }' > "$tmp/new.log"
check "authors, committers, dates and messages kept" cmp -s "$tmp/old.log" "$tmp/new.log"
check "the last commit: the new pins and ids" [ "$(git -C "$new" log -1 --format=%s main)" = 'shellter-install.sh, docs: commit ids of the re-signed history' ]
check "... signed" v -C "$new" verify-commit main
for tg in shellter-v2026.10.0 shellter-v2026.10.1; do
    check "$tg: signed by the release key" v -C "$new" verify-tag "$tg"
    fmt='%(taggername)|%(taggeremail)|%(taggerdate:raw)|%(contents:subject)'
    check "$tg: tagger, date and message kept" \
        [ "$(git -C "$new" for-each-ref --format="$fmt" "refs/tags/$tg")" = "$(git -C "$tmp/old" for-each-ref --format="$fmt" "refs/tags/$tg")" ]
    check "$tg: the commit is signed" v -C "$new" verify-commit "$tg^{commit}"
    git -C "$tmp/old" ls-tree -r "$tg" | grep -v '	secret.txt$' > "$tmp/t.old"
    git -C "$new" ls-tree -r "$tg" > "$tmp/t.new"
    check "$tg: the old tree without the file" cmp -s "$tmp/t.old" "$tmp/t.new"
    check "$tg: pinned at main" grep -qx "$tg $(git -C "$new" rev-parse "$tg^{commit}") signed" "$new/shellter-install.sh"
done
nfeat=$(git -C "$new" log --format=%H --grep='^fork: a feature$' main)
check "docs: the fork id mapped" grep -qF "$(echo "$nfeat" | cut -c1-8) the feature; $nfeat in full;" "$new/docs.md"
check "docs: the upstream id kept" grep -qF "upstream $(echo "$u1" | cut -c1-8);" "$new/docs.md"
check "docs: the id of the dropped commit named" grep -qF "docs.md:3: $(echo "$drop" | cut -c1-8)" "$tmp/out"
check "docs: a file without ids untouched (no newline added)" cmp -s "$new/doc/nonl.md" "$tmp/old/doc/nonl.md"
check "the identity taken over" [ "$(git -C "$new" config --local user.signingkey)" = "$tmp/key" ]
check "... gpg.format too" [ "$(git -C "$new" config --local gpg.format)" = ssh ]
check "the release hook installed, a copy" cmp -s "$new/.git/hooks/pre-push" "$root/utils/pre-push"
check "... executable" [ -x "$new/.git/hooks/pre-push" ]
check "staging's other branch with the old history named" \
    grep -qF "staging/dev/x: " "$tmp/out"
check "... with the command to delete it" grep -qF "git push staging --delete dev/x" "$tmp/out"
check "origin added back" [ "$(git -C "$new" config --get remote.origin.url)" = "$tmp/origin.git" ]
check "... with its push URL" [ "$(git -C "$new" config --get remote.origin.pushurl)" = "$tmp/origin.git" ]
check "staging kept" [ "$(git -C "$new" config --get remote.staging.url)" = "$tmp/staging.git" ]
check "no stale copies of their branches" [ -z "$(git -C "$new" for-each-ref refs/remotes/origin refs/remotes/staging)" ]
cmd_staging="PREPUSH_RETAG='shellter-v2026.10.0 shellter-v2026.10.1' git push --atomic --force staging main 'refs/tags/shellter-v*:refs/tags/shellter-v*'"
check "the push to staging printed" grep -qF "  $cmd_staging" "$tmp/out"
check "... before origin" [ "$(grep -n 'git push' "$tmp/out" | head -n 1 | grep -c staging)" = 1 ]

# --- the printed pushes, through the release hook -------------------------
for rr in staging origin; do
    cmd=$(sed -n "s/^  \(PREPUSH_RETAG=.* $rr main .*\)$/\1/p" "$tmp/out")
    (cd "$new" && eval "$cmd") > "$tmp/push.$rr" 2>&1
    check "push to $rr: accepted" [ $? = 0 ]
    check "... the re-signing named" grep -q 'warning: refs/tags/shellter-v2026.10.1 is moved on the remote (PREPUSH_RETAG' "$tmp/push.$rr"
done
git clone -q --no-local "$tmp/origin.git" "$tmp/check" 2>/dev/null
check "a new clone: the newest tag verifies" v -C "$tmp/check" verify-tag shellter-v2026.10.1
check "... main verifies" v -C "$tmp/check" verify-commit main
check "... the pins point to the tags" grep -qx "shellter-v2026.10.0 $(git -C "$tmp/check" rev-parse 'shellter-v2026.10.0^{commit}') signed" "$tmp/check/shellter-install.sh"

# --- a rewrite that changed a release, not only removed files: refused ----
# (origin holds the new history now: a clone of the old one)
git clone -q --no-local "$tmp/old" "$tmp/new2"
printf 'feature==>changed\n' > "$tmp/replace"
(cd "$tmp/new2" && git filter-repo --quiet --force --replace-text "$tmp/replace") > /dev/null 2>&1
git -C "$tmp/new2" remote add upstream "$tmp/up" 2>/dev/null; git -C "$tmp/new2" fetch -q upstream
before=$(git -C "$tmp/new2" rev-parse main)
(cd "$tmp/new2" && sh "$root/utils/resign-history.sh" "$tmp/old") > "$tmp/out2" 2>&1
check "a changed release: refused" [ $? != 0 ]
check "... named" grep -q 'shellter-v2026.10.0: the rewrite changed the release' "$tmp/out2"
check "... nothing changed" [ "$(git -C "$tmp/new2" rev-parse main)" = "$before" ]
check "... the release, the file and the lines named" grep -qF 'shellter-v2026.10.0: f: changed (-1 +1 lines)' "$tmp/out2"
check "... the old line" grep -qF -- '- feature' "$tmp/out2"
(cd "$tmp/new2" && sh "$root/utils/resign-history.sh" --allow-changes "$tmp/old") > "$tmp/out2" 2>&1
check "--allow-changes: done" [ $? = 0 ]
check "... the changes listed" grep -qF 'shellter-v2026.10.1: f: changed' "$tmp/out2"

# --- a rewrite that changed upstream commits: refused, by their old ids ---
git clone -q --no-local "$tmp/old" "$tmp/new3"
printf 'upstream==>UP\n' > "$tmp/replace"
(cd "$tmp/new3" && git filter-repo --quiet --force --replace-text "$tmp/replace") > /dev/null 2>&1
git -C "$tmp/new3" remote add upstream "$tmp/up" 2>/dev/null; git -C "$tmp/new3" fetch -q upstream
before=$(git -C "$tmp/new3" rev-parse main)
(cd "$tmp/new3" && sh "$root/utils/resign-history.sh" --allow-changes "$tmp/old") > "$tmp/out3" 2>&1
check "changed upstream commits: refused" [ $? != 0 ]
check "... the original id named" grep -qF "the rewrite changed 3 upstream commits, e.g. $u1" "$tmp/out3"
check "... nothing changed" [ "$(git -C "$tmp/new3" rev-parse main)" = "$before" ]

if [ "$failed" != 0 ]; then
    echo "--- resign-history output ---"; cat "$tmp/out"
    for rr in staging origin; do echo "--- push to $rr ---"; cat "$tmp/push.$rr" 2>/dev/null; done
fi
exit "$failed"
