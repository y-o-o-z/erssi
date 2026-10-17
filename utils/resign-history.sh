#!/bin/sh
# resign-history.sh - after a history rewrite with git filter-repo: sign the
# fork's commits again, make the release tags again on them, and get the
# clone ready for the push (docs/shellter/releasing.md, "Re-signing the
# release tags"). POSIX sh.
#
# Usage, in the rewritten clone, with the upstream projects' remotes
# fetched again after the rewrite (git remote add upstream URL; git fetch
# upstream; the same for irssi):
#   sh utils/resign-history.sh [--allow-changes] OLD_REPO
# OLD_REPO is a repository with the history before the rewrite (the clone you
# work in, for example): it is only read. It gives the release tags (their
# commits, messages, taggers and dates), the pins in shellter-install.sh at
# its main, the trees the rewritten releases are compared with, and the
# remotes to add back.
#
# What it does, and refuses on any doubt (nothing is pushed):
# 1. old -> rewritten commits from .git/filter-repo/commit-map; the
#    rewritten main must be the rewrite of OLD_REPO's main, and every
#    release tag of OLD_REPO must agree with the pin of its release.
# 2. The fork's commits - those not on the branches of the upstream remotes
#    (git config prepush.upstream, default "upstream irssi") - are made
#    again with git commit-tree -S on the same tree, with the same author
#    and committer (name, e-mail, date) and message, and their parents
#    mapped (merges keep both). Upstream commits keep their original ids
#    (filter-repo strips their signatures too; their trees must be
#    unchanged). The branches move to the signed commits.
# 3. Every release tag is made again as an annotated tag signed by the
#    release key, on the new commit of its release, with its message, its
#    tagger and its date. A release whose tree is not the old one minus
#    removed files is refused (--allow-changes: listed, not refused).
# 4. One signed commit on main: the pins in shellter-install.sh, and the
#    fork commit ids cited in the .md files and NEWS, mapped to the new
#    ids. Ids inside commit messages stay as filter-repo rewrote them.
# 5. The remotes of OLD_REPO that filter-repo removed or that are missing
#    (origin, staging) are added back with their URLs, the stale copies of
#    their branches (refs/remotes/<remote>/*) are deleted, and the push
#    commands are printed.
set -eu

die() { printf 'resign-history: error: %s\n' "$*" >&2; exit 1; }
say() { printf 'resign-history: %s\n' "$*"; }
ZERO=0000000000000000000000000000000000000000

ALLOW=0
while [ $# -gt 0 ]; do
    case "$1" in
        --allow-changes) ALLOW=1; shift ;;
        -h|--help) sed -n '2,/^set -eu/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option $1" ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || die "usage: resign-history.sh [--allow-changes] OLD_REPO"
OLD=$(cd "$1" && pwd) || die "cannot open $1"
git -C "$OLD" rev-parse -q --verify refs/heads/main >/dev/null || die "$OLD has no main branch"
TOP=$(git rev-parse --show-toplevel) || die "not in a git repository"
cd "$TOP"
GD=$(git rev-parse --absolute-git-dir)
[ "$(cd "$OLD" && git rev-parse --absolute-git-dir)" != "$GD" ] || die "OLD_REPO is this repository - give the history before the rewrite"
MAP=$GD/filter-repo/commit-map
[ -f "$MAP" ] || die "no $MAP - run git filter-repo in this clone first"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the working tree has changes"
[ "$(git symbolic-ref -q HEAD)" = refs/heads/main ] || die "check out main first"

tmp=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM

# git that checks SSH signatures against the rewritten main's
# utils/allowed_signers only (no gpg, whatever the configuration says)
git show main:utils/allowed_signers > "$tmp/signers" 2>/dev/null || die "no utils/allowed_signers on main"
v() {
    git -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$tmp/signers" \
        -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false -c gpg.ssh.program=ssh-keygen \
        "$@" >/dev/null 2>&1
}
# this clone signs with the release key: a signed throwaway commit (not on
# any ref) must verify
probe=$(printf 'probe\n' | git -c gpg.format=ssh commit-tree -S "$(git rev-parse 'main^{tree}')") ||
    die "git commit-tree -S failed - set user.signingkey to the release key and gpg.format to ssh"
v verify-commit "$probe" || die "commits signed here do not verify against utils/allowed_signers - user.signingkey must be the release key"

# the upstream remotes, as in utils/pre-push
up_names=$(git config --get-all prepush.upstream 2>/dev/null) || up_names="upstream irssi"
upglobs=""
for u in $up_names; do
    case "$u" in *[!A-Za-z0-9._-]*) continue ;; esac
    git rev-parse -q --verify "refs/remotes/$u/HEAD" >/dev/null 2>&1 ||
        [ -n "$(git for-each-ref --count=1 "refs/remotes/$u/")" ] || continue
    upglobs="$upglobs --glob=refs/remotes/$u/*"
done
[ -n "$upglobs" ] || die "no branches of an upstream remote ($up_names) here - fetch them after the rewrite (git remote add upstream URL && git fetch upstream)"
# shellcheck disable=SC2086 # glob arguments, globbing off
(set -f; git rev-list $upglobs) > "$tmp/upstream"

# 1. the maps: old -> rewritten (filter-repo), rewritten -> old
sed 1d "$MAP" > "$tmp/fr"
awk '{ print $2, $1 }' "$tmp/fr" > "$tmp/rev"
fr_of() { awk -v o="$1" '$1 == o { print $2; exit }' "$tmp/fr"; }

old_main=$(git -C "$OLD" rev-parse 'main^{commit}')
[ "$(fr_of "$old_main")" = "$(git rev-parse 'main^{commit}')" ] ||
    die "this main is not the rewrite of $OLD's main ($old_main) - run filter-repo on a clone of the same main"

# the releases: OLD's tags, each checked against the pin of its release
git -C "$OLD" show "$old_main:shellter-install.sh" 2>/dev/null |
    sed -n '/^RELEASES="/,/^"/p' | awk 'NF >= 2 && $1 ~ /^shellter-v/ { print $1, $2 }' > "$tmp/pins"
git -C "$OLD" for-each-ref --format='%(refname:short)' 'refs/tags/shellter-v*' > "$tmp/tags"
[ -s "$tmp/tags" ] || die "no release tags (shellter-v*) in $OLD"
: > "$tmp/releases"
while read -r t; do
    [ "$(git -C "$OLD" cat-file -t "refs/tags/$t")" = tag ] || die "$t in $OLD is not an annotated tag"
    oc=$(git -C "$OLD" rev-parse "refs/tags/$t^{commit}")
    p=$(awk -v t="$t" '$1 == t { print $2 }' "$tmp/pins")
    if [ -n "$p" ]; then
        [ "$p" = "$oc" ] || die "$t points to $oc in $OLD, but shellter-install.sh records $p"
    else
        say "warning: $t is not recorded in shellter-install.sh at $OLD's main"
    fi
    fc=$(fr_of "$oc")
    if [ -z "$fc" ] || [ "$fc" = "$ZERO" ]; then die "$t: filter-repo kept no commit for $oc"; fi
    # the tree: the old one, without the removed files and nothing else
    git -C "$OLD" ls-tree -r --full-tree "$oc" | sort > "$tmp/old.tree"
    git ls-tree -r --full-tree "$fc" | sort > "$tmp/new.tree"
    comm -13 "$tmp/old.tree" "$tmp/new.tree" > "$tmp/changed"
    if [ -s "$tmp/changed" ]; then
        sed 's/^/    changed or added: /' "$tmp/changed" | head -n 20
        [ "$ALLOW" = 1 ] || die "$t: the rewrite changed the release, not only removed files (--allow-changes to accept)"
    fi
    gone=$(comm -23 "$tmp/old.tree" "$tmp/new.tree" | wc -l | tr -d ' ')
    echo "$t $oc $fc $gone" >> "$tmp/releases"
done < "$tmp/tags"
awk '{ print $2 }' "$tmp/pins" | while read -r p; do
    grep -q " $p " "$tmp/releases" || die "shellter-install.sh records $p, which no release tag in $OLD points to"
done

# 2. the commits to make again: every rewritten commit of the branches and
# the releases, oldest first, upstream (by its old id) or the fork's
git for-each-ref --format='%(refname)' refs/heads/ > "$tmp/heads"
# shellcheck disable=SC2046 # the rewritten branches and release commits
git rev-list --topo-order --reverse $(cat "$tmp/heads") $(awk '{ print $3 }' "$tmp/releases") > "$tmp/commits"
awk -v up="$tmp/upstream" -v rev="$tmp/rev" '
    BEGIN { while ((getline l < up) > 0) u[l] = 1
            while ((getline l < rev) > 0) { split(l, a, " "); o[a[1]] = a[2] } }
    { old = ($1 in o) ? o[$1] : ""
      if (old == "") { print "missing", $1; next }
      print ((old in u) ? "U" : "F"), $1, old }' "$tmp/commits" > "$tmp/kinds"
! grep -q '^missing ' "$tmp/kinds" || die "commit-map does not name $(sed -n 's/^missing //p' "$tmp/kinds" | head -n 1)"
# upstream commits keep their ids, and their trees must be what they were
awk '$1 == "U" { print $2 "^{tree}"; print $3 "^{tree}" }' "$tmp/kinds" |
    git cat-file --batch-check='%(objectname)' > "$tmp/utrees"
awk '$1 == "U" { print $2 }' "$tmp/kinds" > "$tmp/ucommits"
awk -v c="$tmp/ucommits" '{ t[NR] = $1 } END { i = 0
        while ((getline h < c) > 0) { i += 2; if (t[i - 1] != t[i]) print h } }' "$tmp/utrees" > "$tmp/ubad"
[ ! -s "$tmp/ubad" ] || die "the rewrite changed upstream commits (e.g. $(head -n 1 "$tmp/ubad")) - only the fork's history may change"
awk '$1 == "U" { print $2, $3 }' "$tmp/kinds" > "$tmp/final"
n_up=$(wc -l < "$tmp/final" | tr -d ' ')

final_of() { awk -v c="$1" '$1 == c { print $2; exit }' "$tmp/final"; }
grep '^F ' "$tmp/kinds" > "$tmp/fork" || :
while read -r _ c _; do
    pargs=""
    for p in $(git log -1 --format=%P "$c"); do
        f=$(final_of "$p")
        [ -n "$f" ] || die "no new commit for the parent $p of $c"
        pargs="$pargs -p $f"
    done
    git cat-file commit "$c" | sed '1,/^$/d' > "$tmp/msg"
    # shellcheck disable=SC2086 # the -p options
    new=$(GIT_AUTHOR_NAME=$(git log -1 --format=%an "$c") GIT_AUTHOR_EMAIL=$(git log -1 --format=%ae "$c") \
        GIT_AUTHOR_DATE=$(git log -1 --format=%ad --date=raw "$c") \
        GIT_COMMITTER_NAME=$(git log -1 --format=%cn "$c") GIT_COMMITTER_EMAIL=$(git log -1 --format=%ce "$c") \
        GIT_COMMITTER_DATE=$(git log -1 --format=%cd --date=raw "$c") \
        git -c gpg.format=ssh commit-tree -S "$(git rev-parse "$c^{tree}")" $pargs -F "$tmp/msg") ||
        die "git commit-tree failed for $c"
    v verify-commit "$new" || die "the new commit for $c does not verify"
    echo "$c $new" >> "$tmp/final"
done < "$tmp/fork"
n_fork=$(wc -l < "$tmp/fork" | tr -d ' ')
say "$n_fork fork commits signed again, $n_up upstream commits kept"

while read -r h; do
    old=$(git rev-parse "$h")
    new=$(final_of "$old")
    [ -n "$new" ] || die "no new commit for $h"
    git update-ref "$h" "$new" "$old"
done < "$tmp/heads"
git reset -q --hard main

# old id -> new id, for the fork's commits
awk -v fr="$tmp/fr" '
    BEGIN { while ((getline l < fr) > 0) { split(l, a, " "); o[a[2]] = a[1] } }
    $1 in o && o[$1] != $2 { print o[$1], $2 }' "$tmp/final" > "$tmp/composed"

# 3. the release tags
retag=""
while read -r t oc fc gone; do
    nc=$(final_of "$fc")
    [ -n "$nc" ] || die "$t: no new commit for $fc"
    git -C "$OLD" cat-file tag "refs/tags/$t" | sed '1,/^$/d; /^-----BEGIN .*SIGNATURE-----/,$d' > "$tmp/tagmsg"
    tn=$(git -C "$OLD" for-each-ref --format='%(taggername)' "refs/tags/$t")
    te=$(git -C "$OLD" for-each-ref --format='%(taggeremail)' "refs/tags/$t" | sed 's/^<//; s/>$//')
    td=$(git -C "$OLD" for-each-ref --format='%(taggerdate:raw)' "refs/tags/$t")
    GIT_COMMITTER_NAME=$tn GIT_COMMITTER_EMAIL=$te GIT_COMMITTER_DATE=$td \
        git -c gpg.format=ssh tag -f -s --cleanup=verbatim -F "$tmp/tagmsg" "$t" "$nc" > /dev/null ||
        die "$t: git tag -s failed"
    v verify-tag "$t" || die "$t: the new tag does not verify"
    v verify-commit "$nc" || die "$t: its commit does not verify"
    say "$t: $(echo "$oc" | cut -c1-8) -> $(echo "$nc" | cut -c1-8) (signed; $gone removed files)"
    retag="$retag $t"
done < "$tmp/releases"
retag=${retag# }

# 4. the pins and the commit ids in the docs
awk -v m="$tmp/composed" '
    BEGIN { while ((getline l < m) > 0) { split(l, a, " "); n[a[1]] = a[2] } }
    /^RELEASES="/ { inr = 1 }
    inr && /^"$/ { inr = 0 }
    inr && NF >= 2 && ($2 in n) { sub($2, n[$2]) }
    { print }' shellter-install.sh > "$tmp/inst" && cat "$tmp/inst" > shellter-install.sh
# abbreviated or full ids of the fork's old commits (7 to 40 hex digits)
# become the same length of the new id; an ambiguous prefix is left alone
git ls-files -- '*.md' NEWS > "$tmp/docs"
while read -r f; do
    awk -v m="$tmp/composed" '
        BEGIN { while ((getline l < m) > 0) { split(l, a, " "); k = substr(a[1], 1, 7)
                    if (k in p && p[k] != a[1]) amb[k] = 1; p[k] = a[1]; n[a[1]] = a[2] } }
        { out = ""; s = $0
          while (match(s, /[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]*/)) {
              w = substr(s, RSTART, RLENGTH); pre = substr(s, 1, RSTART - 1)
              before = (RSTART > 1) ? substr(s, RSTART - 1, 1) : ""
              after = substr(s, RSTART + RLENGTH, 1)
              k = substr(w, 1, 7)
              if (RLENGTH <= 40 && before !~ /[0-9A-Za-z]/ && after !~ /[0-9A-Za-z]/ &&
                  (k in p) && !(k in amb) && substr(p[k], 1, RLENGTH) == w)
                  w = substr(n[p[k]], 1, RLENGTH)
              out = out pre w; s = substr(s, RSTART + RLENGTH) }
          print out s }' "$f" > "$tmp/doc" || die "cannot map the ids in $f"
    if ! cmp -s "$tmp/doc" "$f"; then cat "$tmp/doc" > "$f"; say "commit ids mapped in $f"; fi
done < "$tmp/docs"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    {
        echo "shellter-install.sh, docs: commit ids of the re-signed history"
        echo
        echo "The history was rewritten and its commits signed again, so every"
        echo "fork commit has a new id. The pins of the releases and the commit"
        echo "ids cited in the documentation now name the new commits; the"
        echo "release tags were made again on them ($retag)."
    } > "$tmp/cmsg"
    git -c gpg.format=ssh commit -q -a -S -F "$tmp/cmsg" || die "git commit -S failed"
    v verify-commit HEAD || die "the commit with the new pins does not verify"
    say "new pins and ids committed: $(git log -1 --format='%h %s')"
fi
# the new pins must name the new tags
git show HEAD:shellter-install.sh | sed -n '/^RELEASES="/,/^"/p' | awk 'NF >= 2 && $1 ~ /^shellter-v/ { print $1, $2 }' |
    while read -r t p; do
        [ "$(git rev-parse "refs/tags/$t^{commit}" 2>/dev/null)" = "$p" ] || die "the new pin of $t is not its new tag"
    done

# other tags that filter-repo moved are left as they are (and not pushed)
git for-each-ref --format='%(refname:short) %(objectname) %(*objectname)' refs/tags/ |
    awk -v k="$tmp/kinds" 'BEGIN { while ((getline l < k) > 0) { split(l, a, " "); if (a[1] == "F") f[a[2]] = 1 } }
        $1 !~ /^shellter-v/ && (($2 in f) || ($3 in f)) { print $1 }' > "$tmp/othertags"
[ ! -s "$tmp/othertags" ] || say "warning: other tags on rewritten commits, left as filter-repo made them: $(tr '\n' ' ' < "$tmp/othertags")"

# 5. the remotes, and our copies of their branches
targets=""
for r in $(git -C "$OLD" remote); do
    case " $up_names " in *" $r "*) continue ;; esac
    url=$(git -C "$OLD" config --get "remote.$r.url") || continue
    if ! git config --get "remote.$r.url" >/dev/null 2>&1; then
        git remote add "$r" "$url"
        for pu in $(git -C "$OLD" config --get-all "remote.$r.pushurl" 2>/dev/null); do git config --add "remote.$r.pushurl" "$pu"; done
        say "remote $r added back: $url"
    fi
    git for-each-ref --format='delete %(refname)' "refs/remotes/$r/" | git update-ref --stdin
    targets="$targets $r"
done

say "done. Check, then push - staging first, then origin (each runs utils/pre-push):"
for r in staging origin $targets; do
    case " $targets " in *" $r "*) ;; *) continue ;; esac
    case " ${done_r:-} " in *" $r "*) continue ;; esac
    done_r="${done_r:-} $r"
    printf "  PREPUSH_RETAG='%s' git push --atomic --force %s main 'refs/tags/shellter-v*:refs/tags/shellter-v*'\n" "$retag" "$r"
done
