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

This is done once, when every release tag has to be made again on another
commit. The typical case is a history rewrite with `git filter-repo` that
drops files from the history. The rewrite gives every later commit a new
id and strips every signature: from the commits, so the fork's commits
must be signed again, and from the tags, which turns signed tags into
plain annotated ones. The content of each release must stay what it was,
except for what the rewrite removed.

`utils/resign-history.sh` (POSIX sh) does the whole job between the
rewrite and the push, and it refuses at the first doubt. Nothing is
pushed by it. `tests/version/test-resign-history.sh` runs it end to end
on a synthetic history: a real `git filter-repo`, then the pushes it
prints, through the release hook.

You need git 2.34 or newer, `git filter-repo`, and the release key as
`user.signingkey` with `gpg.format=ssh`. Do it right after a release,
before the release is announced (see [What installed copies do
afterwards](#what-installed-copies-do-afterwards)).

### Steps

1. **The old history.** In your working clone (`~/src/erssi` below),
   bring everything up to date and check that `main` is GitHub's:

   ```sh
   cd ~/src/erssi
   git fetch origin && git fetch --tags --force origin && git fetch staging
   git fetch upstream && git fetch irssi
   git status --short && test "$(git rev-parse main)" = "$(git rev-parse origin/main)" && echo ok
   ```

   The script only reads this clone. It takes from it: the release tags,
   with their commits, messages, taggers and dates; the pins in
   `shellter-install.sh` at `main`; the trees the rewritten releases are
   compared with; and the remotes to add back.

2. **A fresh clone, rewritten.**

   ```sh
   git clone https://github.com/y-o-o-z/erssi.git ~/erssi-rewrite && cd ~/erssi-rewrite
   git filter-repo --invert-paths --path FILE-TO-DROP     # your filter; no --force on a fresh clone
   ```

   Keep the rule files of `--replace-text` and `--replace-message` outside
   the repository, for example in `~/rewrite-rules/` (`blobs.txt`,
   `msgs.txt`). Two rules for them:
   - **They must never touch upstream content.** The upstream projects'
     history is public and keeps its ids. The script refuses a rewrite that
     changed an upstream commit, and names the original commit. Before
     rewriting, check each word that a rule replaces:
     `git -C ~/src/erssi log --remotes=upstream --remotes=irssi -i -G 'WORD' --oneline`
     must print nothing. Otherwise narrow the rule, e.g.
     `regex:word(?!N)==>other`.
   - **`--replace-text` changes releases**, so step 4 then needs
     `--allow-changes`. The script lists, per release, every changed file
     with its line counts and first lines. Read that list, and stop if
     anything is there that the rules should not have changed.

3. **The upstream projects, fetched again.** filter-repo removes `origin`.
   The upstream commits have to be here with their original ids:

   ```sh
   git remote add upstream "$(git -C ~/src/erssi remote get-url upstream)" && git fetch upstream
   git remote add irssi "$(git -C ~/src/erssi remote get-url irssi)" && git fetch irssi
   ```

4. **The re-signing.**

   ```sh
   sh ~/src/erssi/utils/resign-history.sh ~/src/erssi              # add --allow-changes after --replace-text
   ```

   First, the script sets up the fresh clone where it lacks anything:
   - it takes `user.name`, `user.email`, `user.signingkey`, `gpg.format`
     and `gpg.ssh.allowedSignersFile` from the old clone;
   - it installs the release hook, a copy of `utils/pre-push`, as
     `.git/hooks/pre-push`, so that the pushes in step 6 are checked.

   Check with `test -x .git/hooks/pre-push && git config user.signingkey`.
   Then, in order, the script:
   - maps the old commits to the rewritten ones
     (`.git/filter-repo/commit-map`). It checks that the rewritten `main`
     is the rewrite of the old `main`, that every release tag agrees with
     its pin, and that every release tree is the old one with files
     removed and nothing changed or added (`--allow-changes` lists such
     changes instead of refusing);
   - makes every fork commit again: the commits that are not on the
     branches of `upstream` or `irssi` (or of the remotes in
     `git config prepush.upstream`). Each one is made with
     `git commit-tree -S`, on the same tree, with the same author and
     committer (name, e-mail, date) and message, and with its parents
     mapped, so merges keep both parents. Upstream commits keep their
     original ids. Then it moves the branches;
   - makes every release tag again as an annotated tag signed by the
     release key, on the new commit of its release, with its old message,
     tagger and tagger date, and checks the tag and the commit;
   - makes one signed commit on `main`, "shellter-install.sh, docs: commit
     ids of the re-signed history". It holds the new pins and maps the
     fork commit ids cited in the `.md` files and `NEWS`, such as
     `docs/UPSTREAM.md`, to the new ids;
   - names the ids in those files that point to commits the rewrite
     dropped (commits that became empty). Edit those by hand;
   - adds back the remotes of the old clone that are missing (`origin`,
     `staging`, with their push URLs), and deletes our copies of their
     branches (`refs/remotes/<remote>/*`, which filter-repo rewrote);
   - lists the other branches of those remotes that still hold the old
     history, as the old clone knows them. Such a branch keeps serving what
     the rewrite removed. If it holds commits whose content changed or was
     dropped, it says so and prints the delete command, for example `git
     push staging --delete dev/2026.10`. A branch with old ids but
     unchanged content gets "rebase it onto the new main, or delete it".
     Commits outside this rewrite, such as `gh-pages`, get "check them";
   - prints the push commands.

   After any error, start again from step 2 with a new clone.

5. **Look before pushing.** Run `git log --oneline -5` and `git show HEAD`
   (the new pins and the mapped ids). Also check that the tags have their
   dates: `git for-each-ref refs/tags/shellter-v* --format='%(refname:short) %(taggerdate)'`.

6. **Push to staging first, then to origin**, with the commands it
   printed. Before that, deal with every branch it listed in step 4:
   delete it, or rewrite it and push it again the same way. The push of
   `main` and the tags does not change the other branches. GitHub can
   serve old commits by id until it collects them; only GitHub support
   can purge them sooner.

   ```sh
   PREPUSH_RETAG='shellter-v1.3.2 … shellter-v2026.10.3' git push --atomic --force staging main 'refs/tags/shellter-v*:refs/tags/shellter-v*'
   PREPUSH_RETAG='shellter-v1.3.2 … shellter-v2026.10.3' git push --atomic --force origin main 'refs/tags/shellter-v*:refs/tags/shellter-v*'
   ```

   The release hook runs with every check:
   - our copies of the remote's branches are gone, and the remote's old
     tip is not in this clone, so every fork commit is checked: author, AI
     trailers, private data and signature;
   - each tag must be signed, name itself, and be on a signed commit;
   - each tag must be where the new pins record it;
   - `check-version.sh` runs for each tag.

   `PREPUSH_RETAG` only lets the named tags move, with a warning for each
   one. `--atomic` moves `main` (the new pins) and the tags together.
   The hook checks each commit once per push, but for the whole fork
   history (about 400 commits) that still takes a minute or two.

7. **Wait a few minutes, then check from outside.**
   raw.githubusercontent.com serves `shellter-install.sh` from a cache. For
   a few minutes after the push, `curl … | sh` can still get the old pins
   and refuse the moved release ("points to …, but this installer expects
   …"). It fails closed. After that:
   - the CI job "Every pinned release points to its commit" passes;
   - `verify-tag` passes in a new clone;
   - `curl … | sh -s -- --prefix ~/erssi-check --src ~/erssi-check-src`
     installs the newest release with "signed · commit recorded"; remove
     both directories afterwards.

8. **Publish the next PATCH release soon** (see below).

### Commit ids that go stale

- **Commit messages.** filter-repo rewrites the abbreviated ids inside
  commit messages to its own new ids, and the re-signing gives the commits
  new ids again. The ids in the messages of rewritten commits then name
  nothing. They are left as they are.
- **Documentation.** The ids cited in the `.md` files and `NEWS` are
  mapped by the script. Its mapping of `docs/UPSTREAM.md` should be read
  over in step 5.
- **Outside the repository.** Ids in issues, pull requests and links keep
  naming the old commits.
- **Other tags.** Tags that are not release tags and sit on rewritten
  commits are left as filter-repo made them. The script names them, and
  they are not pushed.

### What installed copies do afterwards

`erssi --update` runs the installer of the installed release, so what
users see depends on their version:

- **The release just made (2026.10.3), installed before the
  re-signing.** `erssi --check-update` and `--update` print "the release
  tag shellter-vX now points to a different commit than the installed one
  - not updating; report it", and they install nothing. This lasts until a
  newer release is out, so re-sign before announcing the release and
  publish the next PATCH soon after. The update to it records the new
  commit, and the warning stops.
- **2026.10.2 and older (1.3.10, 2026.10.0 to 2026.10.2).** For them the
  re-signed 2026.10.3 is an ordinary newer release. `erssi --update`
  installs it after checking its signature. Their installer records no
  commit for 2026.10.3, so it does not check one.
- **New installs** with `curl … | sh` take `main`'s installer with the new
  pins, after the cache window of step 7. A saved copy of an older
  installer refuses the re-signed releases ("points to …, but this
  installer expects …"): download the installer again.
- **Clones** keep the old tags until `git fetch --tags --force`. Until
  then the test "installer versions test (shellter-v1.3.10)" is skipped as
  a stale tag.
