# Releasing erssi Shellter Edition (maintainers)

Back to the [README](../../README.md).

- [Rules](#rules)
- [A release](#a-release)
- [Re-signing the release tags](#re-signing-the-release-tags)

## Rules

`utils/pre-push` enforces these rules. Install it once:
`ln -sf ../../utils/pre-push .git/hooks/pre-push`.

- Every commit on `main` is signed by the release key, whose public half
  is in `utils/allowed_signers`.
- A release is an annotated tag `shellter-vYYYY.M.PATCH`, signed by the
  release key, on a signed commit whose `meson.build` and first line of
  `NEWS` name that version (`utils/check-version.sh`).
- **Release tags never move.** A fix to a release is a new PATCH
  release. The hook refuses to move a release tag that is on the remote
  (`git push --force`) or to delete one. It also refuses to create a tag
  that `shellter-install.sh` records on another commit than the recorded
  one. Installed copies of the installer from 2026.10.3 on report a moved
  tag and do not install it (see
  [install.md](install.md#what-the-installer-verifies)). The only
  exception is the one-time re-signing below.

## A release

1. Make a signed commit with `NEWS` and the version in `meson.build`, then
   the release commit `erssi Shellter Edition X`.
2. Tag it: `git tag -s -m "erssi Shellter Edition X" shellter-vX`.
3. Push `main` and the tag. The hook checks both.
4. Make a signed commit on `main` that adds the line
   `shellter-vX <commit> signed` to `RELEASES` in `shellter-install.sh`.
   The subject is `shellter-install.sh: verified and signed release
   shellter-vX`. From then on, `curl … | sh` installs the release and
   checks it against that commit.

## Re-signing the release tags

This is done once, when every release tag has to be made again on
another commit. The typical case is a history rewrite that drops files
from the history: it gives every later commit a new id, and it drops the
commits' signatures. The content of each release must stay what it was,
except for what the rewrite removed.

### 1. Sign the rewritten history again

Every rewritten commit on `main` must carry a valid signature by the
release key again. Without that, the push of `main` and of the tags is
refused ("not signed by the release key", "the release commit … is not
signed"). Commits of the upstream projects keep their ids if the rewrite
does not touch them. They are not checked as long as their remotes
(`upstream`, `irssi`, or those in `git config prepush.upstream`) are
fetched.

### 2. Find the new commit of each release

Use the rewrite tool's map. For git filter-repo that is
`.git/filter-repo/commit-map`, with one "old new" line per commit. List
each release as "tag old new":

```sh
for t in $(git tag -l 'shellter-v*'); do
    old=$(git rev-parse "$t^{commit}")
    new=$(awk -v o="$old" '$1 == o { print $2 }' .git/filter-repo/commit-map)
    echo "$t $old $new"
done > ../retag.txt
```

Every line needs a new commit. Then check that each release changed only
by the removed files:

```sh
while read -r t old new; do echo "== $t"; git diff --stat "$old" "$new"; done < ../retag.txt
```

### 3. Re-sign the tags, each with the message it had

```sh
while read -r t old new; do
    msg=$(git cat-file tag "$t" | sed -n '/^$/,$p' | sed '1d; /^-----BEGIN SSH SIGNATURE-----/,$d')
    git tag -f -s -m "$msg" "$t" "$new"
done < ../retag.txt
```

Check them the way the installer does:

```sh
v() { git -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false \
        -c gpg.format=ssh -c gpg.ssh.allowedSignersFile=utils/allowed_signers "$@"; }
while read -r t old new; do
    v verify-tag "$t" && v verify-commit "$t^{commit}" || echo "BAD $t"
    case "$t" in shellter-v1.*) ;; *) sh utils/check-version.sh "$t" "$t" ;; esac
done < ../retag.txt
```

### 4. Change the pins in shellter-install.sh

In `RELEASES`, replace the commit of every re-signed release with its new
commit:

```sh
while read -r t old new; do sed -i "s/^$t $old /$t $new /" shellter-install.sh; done < ../retag.txt
git diff shellter-install.sh        # one changed line per release, nothing else
```

Make a signed commit: `shellter-install.sh: pins of the re-signed release
tags`. In the body, say why the tags were re-signed.

### 5. Push main and the tags in one atomic push, with the override

`PREPUSH_RETAG` names exactly the tags that may move. The hook prints a
warning for each of them and still runs every other check: the tag
signature, the name inside the tag, the signature of the release commit,
`check-version.sh`, author, AI trailers and private data. A tag it does
not name is still refused.

```sh
PREPUSH_RETAG=$(awk '{ printf "%s ", $1 }' ../retag.txt) \
    git push --atomic --force staging main 'refs/tags/shellter-v*:refs/tags/shellter-v*'
PREPUSH_RETAG=$(awk '{ printf "%s ", $1 }' ../retag.txt) \
    git push --atomic --force origin main 'refs/tags/shellter-v*:refs/tags/shellter-v*'
```

`--atomic` moves `main`, with the new pins, together with the tags. That
way `curl … | sh` never sees pins and tags that disagree. Until both are
on GitHub, the installer refuses the release ("points to …, but this
installer expects …"), and so does the CI job "Every pinned release
points to its commit".

### 6. Check from outside

In a fresh clone:

- the CI job "Every pinned release points to its commit" passes;
- `git verify-tag` passes for each release tag (with the options of
  step 3);
- `curl … | sh` into a temporary prefix installs the newest release with
  "signed · commit recorded".

### What installed copies do afterwards

`erssi --update` runs the installer of the installed release, so what
users see depends on their version:

- **2026.10.3 and newer.** `erssi --check-update` and `--update` print
  "the release tag shellter-vX now points to a different commit than the
  installed one - not updating; report it" for the installed release, and
  install nothing. This lasts until a newer release is out. So publish the
  next PATCH release soon after the re-signing, and say in its notes that
  the warning was expected. Updating to that release records the new
  commit, and the warning stops.
- **2026.10.2 and older** (1.3.10, 2026.10.0 to 2026.10.2). Their
  installer counts a signed tag of the installed version on another
  commit as an update. `erssi --check-update` says "Changes to X
  available", and `erssi --update` rebuilds the same release from the
  re-signed tag. It checks the signature and does not check the pin. This
  happens once; then the new commit is recorded.
- **New installs** with `curl … | sh` take `main`'s installer with the new
  pins and work as before. A saved copy of an older installer refuses the
  re-signed releases ("points to …, but this installer expects …"):
  download the installer again.
- **Clones** keep the old tags until `git fetch --tags --force`. Until
  then, the test "installer versions test (shellter-v1.3.10)" is skipped
  as a stale tag.
