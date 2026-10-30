# Releasing erssi Shellter Edition (maintainers)

Back to the [README](../../README.md).

- [Rules](#rules)
- [A release](#a-release)
- [release.sh](#releasesh)
- [Testing under load](#testing-under-load)
- [Binary packages](#binary-packages)
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
- **A tag is pushed only after its packages are built and verified.** The
  packages are built from the local tag. Until it is pushed, a tag whose
  build fails on one platform (a test that fails only there) can still be
  deleted and made again. Once pushed, it cannot: the fix is a new PATCH
  release, as 2026.10.8 was for the FreeBSD packages of 2026.10.7.

## A release

Before the first step, run the tests under load on the commit to be
tagged (see [Testing under load](#testing-under-load)). Every run must
pass.

`utils/release.sh` (POSIX sh) runs each step with one command. Every step
first checks what it needs. What is done already it says and keeps, so a
step, or the whole run, can be run again after a failure. `--dry-run`
prints what a step would run and changes nothing (its checks still run).

1. Make a signed commit with `NEWS`, the version in `meson.build` and the
   docs brought to the release (the badge and the version table of
   `README.md`, the examples in `docs/shellter/install.md`). Then make the
   release commit `erssi Shellter Edition X`, which dates `NEWS`.
2. Tag it, here only, with the message made from `NEWS`:

   ```sh
   sh utils/release.sh tag-message X > ../tag-message
   git tag -s -F ../tag-message shellter-vX
   ```

   From 2026.11.0 the message is the title line `erssi Shellter Edition
   X`, a blank line and the intro paragraph of the release's `NEWS` entry,
   wrapped at 72 columns. The GitHub release takes its notes from it.
   Before 2026.11.0 the message was the title line only.
3. `sh utils/release.sh check shellter-vX` checks the tag: an annotated tag
   of that name, signed by the release key (`utils/allowed_signers`), on a
   signed commit; `meson.build`, `NEWS` (`utils/check-version.sh`),
   `README.md` and `install.md` name the version; the message is the one
   from `NEWS`; the working tree is clean; and the tag is not on GitHub
   yet. A tag that is on GitHub as the same tag object passes (a run
   again after the push); another tag object of that name is refused.
4. `sh utils/release.sh build shellter-vX --out ~/release-X --freebsd14
   HOST14 --freebsd15 HOST15` builds the packages from the local tag (see
   [release.sh](#releasesh)).
5. `sh utils/release.sh verify ~/release-X` builds each package again in a
   fresh environment. It must be the same bytes.
6. `sh utils/release.sh sums ~/release-X` writes and signs `SHA256SUMS`.
7. Push `main` and the tag. The hook checks both.
8. `sh utils/release.sh upload shellter-vX ~/release-X`, then `sh
   utils/release.sh confirm shellter-vX` (see [Signing and
   uploading](#signing-and-uploading)). `upload` refuses while the tag is
   not on GitHub.
9. Make a signed commit on `main` that adds the line
   `shellter-vX <commit> signed` to `RELEASES` in `shellter-install.sh`.
   The subject is `shellter-install.sh: verified and signed release
   shellter-vX`. Push it. From then on, `curl … | sh` installs the release and
   checks it against that commit; the packages are there already, so it
   installs them.

`sh utils/release.sh all shellter-vX --out ~/release-X …` runs steps 3 to 6
and stops with what to push. Run it again after the push: it keeps what
is done, then uploads and confirms.

## release.sh

`sh utils/release.sh --help` lists the steps and options. What the
steps run:

- **build** takes each platform's packager from the tag itself (its
  `utils/build-binary.sh`). The tag goes into each build environment as a
  git bundle (`git bundle create … refs/tags/shellter-vX`), so it need not
  be on GitHub. `debian13-amd64` is built on the host when the host is
  Debian 13, and in a `trixie` chroot otherwise. `debian12-amd64` and
  `ubuntu24-amd64` are built in new mmdebstrap chroots, as described in
  [Debian 12 and Ubuntu 24.04](#debian-12-and-ubuntu-2404).
  `freebsd14-amd64` and `freebsd15-amd64` are built over ssh on the hosts
  given: the bundle goes into a new directory made with `mktemp -d` there,
  the build runs from a clone of it, and the package comes back. The
  remote commands run under `sh -s`, whatever the login shell is. A
  package already in `--out` is kept if its `MANIFEST` names the tag's
  commit, and refused otherwise. The logs go to `--out/logs/<platform>/`.
- **verify** runs `build-binary.sh --verify` on each package in a new
  environment: a new mmdebstrap chroot for Debian and Ubuntu (also for
  Debian 13 built on the host), and for FreeBSD the host of
  `--freebsd14-verify` (default: the build host; `--verify` builds in a
  new clone and work directory). A package that reproduces is marked in
  `logs/<platform>/verified` with its sha256. One that does not reproduce
  stops the release, with the files and the `MANIFEST` lines that differ.
- **sums** signs only packages marked as reproduced with that sha256.
- **verify**, **sums** and **upload** take only packages whose `MANIFEST`
  names the commit of the local tag: packages left from a tag that was
  deleted and made again on another commit are refused, also when they
  were verified and signed before.
- **upload** and **confirm** use `gh` (see [Signing and
  uploading](#signing-and-uploading)). Before it uploads, **upload**
  checks that `--gh-repo` has the tag as the same tag object as
  `--remote`.

The hosts, users and paths of the machines that build a release are
options or environment variables, never part of the repository:

| Option | Variable | Default |
|---|---|---|
| `--repo DIR` | `RELEASE_REPO` | the repository of the script |
| `--remote NAME\|URL` | `RELEASE_REMOTE` | `origin` |
| `--gh-repo OWNER/NAME` | `RELEASE_GH_REPO` | `y-o-o-z/erssi` |
| `--signers FILE` | `RELEASE_SIGNERS` | `utils/allowed_signers` |
| `--key FILE` | `RELEASE_KEY` | `git config user.signingkey` |
| `--platforms LIST` | `RELEASE_PLATFORMS` | Debian 12 and 13, Ubuntu 24.04, and FreeBSD where a host is given |
| `--freebsd14 HOST`, `--freebsd15 HOST` | `RELEASE_FREEBSD14`, `RELEASE_FREEBSD15` | none |
| `--freebsd14-verify HOST`, `--freebsd15-verify HOST` | `RELEASE_FREEBSD14_VERIFY`, `RELEASE_FREEBSD15_VERIFY` | the build host |
| `--mm-tmpdir DIR` | `RELEASE_MM_TMPDIR` | `/var/tmp/<user>-mm` |
| | `RELEASE_SSH` | `ssh` (for example `ssh -p 2222`) |
| | `RELEASE_UBUNTU_MIRROR`, `RELEASE_UBUNTU_KEYRING`, `RELEASE_DEBIAN_MIRROR` | as in the recipes below |

A host is a destination ssh takes (`user@host`, or a `Host` of
`~/.ssh/config`). Keep the variables of your machines in a file outside
the repository and source it before a release. Paths given to `build`
and `verify` may hold letters, digits and `._+/-` only, since mmdebstrap's
hooks take them as words.

`tests/installer/test-release.sh` (the `release` tests of the suite
`tooling`) runs release.sh on throwaway repositories, with a throwaway
key, a local bare repository as the remote and a stand-in `gh`. It
covers the arguments, the tag message, `check`, `--dry-run` of `build`,
`verify` and `all`, `sums` (checked again with `ssh-keygen`), `upload`
(the order of the assets, refused before the push) and `confirm` (each way
a release can be wrong). It runs nothing over the network, in a chroot
or over ssh.

## Testing under load

A package is built while other builds and VMs run, so its tests run on a
loaded machine. A test that waits a fixed time, or has a short deadline,
can pass on an idle machine and fail there (this happened in 2026.10.7
and 2026.10.8). `utils/stress-tests.sh` (POSIX sh) runs the suite of a
build directory several times while busy processes load every CPU, and
prints how often each test failed:

```sh
utils/stress-tests.sh Build                    # 3 rounds, one busy process per CPU
utils/stress-tests.sh -n 2 -l 2 -j 2 Build     # 2 rounds of 2 runs at once, 2 busy processes
utils/stress-tests.sh -n 5 Build -- "live keepnick"  # one test; meson test arguments after --
```

It exits non-zero when any test failed in any run. The busy processes are
ended when it ends (also on ^C); the logs of each run stay in
`Build/meson-logs/stress-r*-p*.*`. Run it before tagging a release: a test
that fails there is fixed (it waits for a condition, with a long
deadline), not run again until it passes. `--help` lists the options.

## Binary packages

A release has a binary package for each supported platform, built from
its signed tag by `utils/build-binary.sh` (POSIX sh) on that platform.
The platforms are `debian12-amd64`, `debian13-amd64`, `ubuntu24-amd64`,
`freebsd14-amd64` and `freebsd15-amd64` (from 2026.10.7; Debian 13 and
FreeBSD 14 since 2026.10.4, FreeBSD 15 since 2026.10.6). arm64 waits for
a real arm64 machine: under qemu-user emulation tests of fe-web fail
because the emulator lacks the kernel's socket diagnostics, so such a
build proves nothing.

### What a package is

- **Relocatable.** The build is configured with `-Drelocatable=true`. erssi
  then finds its prefix when it starts: the directory two levels above the
  real path of its executable (`/proc/self/exe` on Linux, `sysctl
  KERN_PROC_PATHNAME` on FreeBSD), so `<prefix>/bin/erssi`. The modules,
  the Perl modules (`Irssi.pm` and its `.so` files), `/help`, the themes,
  the scripts, the `startup` file and the system config come from there,
  wherever the tree is unpacked. If the executable cannot be found, erssi
  uses the compiled paths; `G_MESSAGES_DEBUG=erssi` shows why.
  `erssi --print-prefix` prints the prefix in use. A build without the
  option uses the compiled paths, as before. In the installer's layout
  (`<prefix>/releases/<version>/`), an entry of a saved `help_path`
  (`:`-separated) or `perl_use_lib` (space-separated) that lies inside
  another release becomes the same path under this release, with one
  warning line naming both. So does an entry under the path a source
  install into the same prefix used before (`<prefix>/share/irssi/help`,
  `<prefix>/lib/…/irssi/perl`). The other entries stay as they are, in their
  order, and a list that is then the default leaves the config. `/SET -default`
  writes no default into the config.
- **The tarball** `erssi-<version>-<platform>.tar.xz` has one top
  directory, `erssi-<version>-<platform>/`. It holds the installed tree
  (`bin/erssi`, `lib/…`, `share/…`, `include/…`), stripped, and a
  `MANIFEST` of sorted `key=value` lines:

  | key | value |
  |---|---|
  | `built` | the commit's time, UTC (`2026-10-18T12:00:00Z`) |
  | `cc` | the first line of `cc --version` (from 2026.10.7) |
  | `commit` | the tag's commit, 40 hex digits |
  | `libc` | `glibc 2.41` or `freebsd 14.5` |
  | `libs` | `pkg-config --modversion` of the libraries built against: `glib-2.0=… openssl=… libotr=… libgcrypt=… libcurl=… chafa=… libutf8proc=…` (`?` for one pkg-config does not know; from 2026.10.7) |
  | `meson`, `ninja` | their versions (from 2026.10.7) |
  | `strip`, `xz` | the first line of their `--version` (from 2026.10.7) |
  | `needed` | the shared libraries the binary and its modules need (sonames from `readelf -d`, space-separated, sorted) |
  | `perl_version` | `perl -e 'print $^V'` without the `v` (`5.40.1`) |
  | `platform` | `<os><major>-<arch>`: `os` is the `ID` of `/etc/os-release` or `freebsd`; `major` is from `VERSION_ID` or `freebsd-version -u`; `arch` is `amd64` (`x86_64`) or `arm64` (`aarch64`) |
  | `tag` | `shellter-v<version>` |
  | `version` | the version |

  The packing is reproducible: the entries sorted, owner and group 0,
  every entry dated to the commit (`SOURCE_DATE_EPOCH`), the modes
  without the umask, and no paths of the build directory in the binary.
  Two builds of a tag on the same platform with the same tools (the
  `cc`, `meson`, `ninja`, `libs`, `perl_version` and `libc` lines) are
  the same bytes; `build-binary.sh --verify` checks it (see
  [Checking a package by building it again](#checking-a-package-by-building-it-again)).
- **The release assets:** every tarball, `SHA256SUMS` (lines
  `<sha256>  <tarball>`, sorted) and `SHA256SUMS.sig`, an SSH signature by
  the release key in the namespace `erssi-release`.

### What build-binary.sh does

`sh utils/build-binary.sh [--out DIR] shellter-vX`:

1. Clones the repository it is in (or `--repo DIR`) into a new work
   directory. It checks that `shellter-vX` is an annotated tag named
   `shellter-vX`, signed by the release key. The key and its fingerprint
   are the ones `shellter-install.sh` carries. It also checks that the
   tree names that version (`utils/check-version.sh`).
2. Configures a new build directory: `-Drelocatable=true
   -Dbuildtype=release`, with Perl, OTR, the proxy, fe-web, E2E, the
   image preview and utf8proc all required. The hardening flags of
   `meson.build` apply as in every build.
3. Builds the tree and runs the **full** `meson test` suite, with HOME and
   TMPDIR inside the work directory. A failed test means no package.
4. Installs into a staging directory and strips the ELF files. It
   refuses a file installed outside the prefix (it would be missing from
   the package) and a symbolic link whose target is absolute or has a
   `..` in it (the installer refuses those too). Then it writes the
   `MANIFEST` and packs the tarball into `--out` (default: the current
   directory).

The work directory goes at the end (`--keep-work` keeps it, with the
logs: `setup.log`, `build.log`, `test.log`). Nothing of `~/.erssi` or
the user's git config is used.

The script runs again in an empty environment (`env -i`). It sets
exactly these variables:
- `PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin`;
- `HOME=/nonexistent` (the build and its tests then get a home in the
  work directory);
- `LC_ALL=C`;
- `TMPDIR`, yours or `/tmp` (where the work directory goes);
- `BUILD_BINARY_TESTS`, yours or empty (empty is the full suite);
- `BUILD_BINARY_CLEAN=1`, which marks the second run.

Everything else of your shell is dropped: `CC`, `CFLAGS`, `LDFLAGS`,
`PKG_CONFIG_PATH`, `LD_LIBRARY_PATH`, `PERL5LIB`, the interop test
settings (`RPE2E_SCRIPT`, `REPARTEE_BIN`), and also `http_proxy`,
`https_proxy` and `no_proxy`. meson, ninja and the compilers must be
the system's. Behind a proxy, a `--repo` URL cannot be cloned: clone the
repository first (with the proxy) and pass the local clone, as in the
examples below (`--repo ~/erssi`). The build itself needs no network.

No ELF file of the package may have an RPATH or RUNPATH, except the
system perl's `CORE` directory (under `/usr`, without `..`), which
FreeBSD's perl uses to find libperl. Each step prints one line.

### Debian 13

`release.sh build` does what this section and the next two describe; the
recipes are for a build by hand, and they say what the build needs. By
hand, the tag must be on GitHub (they clone from there); release.sh hands
the chroot the local tag instead.

Build on a Debian 13 host, or in a clean Debian 13 chroot, with packages
from the Debian 13 archive only:

```sh
sudo apt install git ca-certificates gcc meson ninja-build pkgconf libglib2.0-dev \
  libssl-dev libperl-dev libutf8proc-dev libgcrypt20-dev libotr5-dev \
  libcurl4-openssl-dev libchafa-dev python3 perl binutils xz-utils openssh-client \
  libsodium23
```

A chroot without root, made by mmdebstrap (`sudo apt install mmdebstrap`
once). `--mode=unshare` needs the user's ranges in `/etc/subuid` and
`/etc/subgid`:

```sh
mmdebstrap --mode=unshare --variant=apt \
  --include=git,ca-certificates,gcc,meson,ninja-build,pkgconf,libglib2.0-dev,libssl-dev,libperl-dev,libutf8proc-dev,libgcrypt20-dev,libotr5-dev,libcurl4-openssl-dev,libchafa-dev,python3,perl,binutils,xz-utils,openssh-client,libsodium23 \
  --customize-hook='chroot "$1" useradd -m builder' \
  --customize-hook='chroot "$1" su builder -c "git clone https://github.com/y-o-o-z/erssi.git ~/erssi && sh ~/erssi/utils/build-binary.sh --repo ~/erssi --out ~/out shellter-vX"' \
  --customize-hook='copy-out /home/builder/out .' \
  trixie /dev/null
```

The build runs as an ordinary user (`builder`), not as root. The chroot is
thrown away at the end, and the tarball is copied out to `./out`.

### Debian 12 and Ubuntu 24.04

The same chroot as for Debian 13, of the other release. `--mode=unshare`
maps root to one of your subordinate uids, which must be able to enter
the temporary directory: give mmdebstrap a `TMPDIR` that is not inside a
0700 home (`mkdir -m 755 /var/tmp/$USER-mm`).

```sh
PKGS=git,ca-certificates,gcc,meson,ninja-build,pkgconf,libglib2.0-dev,libssl-dev,libperl-dev,libutf8proc-dev,libgcrypt20-dev,libotr5-dev,libcurl4-openssl-dev,libchafa-dev,python3,perl,binutils,xz-utils,openssh-client,libsodium23
HOOK='chroot "$1" su builder -c "git clone https://github.com/y-o-o-z/erssi.git ~/erssi && sh ~/erssi/utils/build-binary.sh --repo ~/erssi --out ~/out shellter-vX"'
# Debian 12
TMPDIR=/var/tmp/$USER-mm mmdebstrap --mode=unshare --variant=apt --include="$PKGS" \
  --customize-hook='chroot "$1" useradd -m builder' --customize-hook="$HOOK" \
  --customize-hook='copy-out /home/builder/out ./out-debian12' bookworm /dev/null
# Ubuntu 24.04: its archive key, and universe (libotr5-dev, libchafa-dev,
# libutf8proc-dev are there); on Debian: sudo apt install ubuntu-keyring
TMPDIR=/var/tmp/$USER-mm mmdebstrap --mode=unshare --variant=apt --components=main,universe \
  --keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg --include="$PKGS" \
  --customize-hook='chroot "$1" useradd -m builder' --customize-hook="$HOOK" \
  --customize-hook='copy-out /home/builder/out ./out-ubuntu24' noble /dev/null http://archive.ubuntu.com/ubuntu
```

### FreeBSD 14 and 15

Build on a FreeBSD 14 or 15 host or VM (each makes its own package), with packages from `pkg` (OpenSSL
from the base system):

```sh
pkg install git meson ninja pkgconf glib perl5 utf8proc libgcrypt libotr curl chafa python3 \
  py312-sqlite3 libsodium
git clone https://github.com/y-o-o-z/erssi.git ~/erssi
sh ~/erssi/utils/build-binary.sh --out ~/out shellter-vX
```

`py312-sqlite3` is Python's sqlite3 module, which FreeBSD packages
apart from Python: take the one of the `python3` installed (`python3 -V`;
3.12 in the 2026 quarterly packages). The rpe2e-from-repartee test needs
it and libsodium (`libsodium23` on Debian); without them that test is
skipped, with the reason in the test log.

`MANIFEST` records the perl version (from `perl5` of the quarterly
branch) and the FreeBSD version the package was built on.

### Checking a package by building it again

`sh utils/build-binary.sh [--repo DIR] [--keep-work] --verify TARBALL`
builds TARBALL again on this machine and compares the two. It reads the
tag and the platform from the package's `MANIFEST`, refuses a package
of another platform, checks that the tag is signed by the release key,
and runs the build of that tag's own `utils/build-binary.sh` (so an older
release is built by the packager that made it), with its full test
suite. Exit 0 and `reproduced` when the sha256 is the same; exit 3 and
`NOT reproduced` otherwise, with the entries only in one of them, the
files that differ and the `MANIFEST` lines that differ; exit 1 when it
refuses or the build fails (nothing was compared). With `--keep-work`
both unpacked trees and the build's logs stay in the work directory.
`--out` and `--sums` do not go with `--verify`. The tools it needs are
those of a package build on that platform (below).

Before a release's packages are signed, each is checked on a second,
clean machine or chroot of its platform (for Debian and Ubuntu a new
mmdebstrap chroot, for FreeBSD a second VM or the same one after `pkg
upgrade` changed nothing): `--verify` must say `reproduced`. A package
that does not reproduce is not published until the difference is
understood.

### Signing and uploading

`release.sh sums DIR` signs the packages in one directory on the machine
with the release key:

```sh
sh utils/release.sh sums ~/release-X       # --key FILE, or user.signingkey
```

It refuses a package that `verify` has not reproduced. It writes
`SHA256SUMS` with `build-binary.sh --sums` (lines `<sha256>  <tarball>`,
sorted by name) and signs it:
`ssh-keygen -Y sign -n erssi-release -f <release key> SHA256SUMS` writes
`SHA256SUMS.sig`. The release key is the key that signs the commits and
tags (`user.signingkey`). It then checks the signature and the sums the
way the installer does: the allowed signer is the key of
`utils/allowed_signers` with the namespace `erssi-release`. By hand:

```sh
printf '%s namespaces="erssi-release" %s\n' 136268860+y-o-o-z@users.noreply.github.com \
  "$(cut -d' ' -f3,4 ~/src/erssi/utils/allowed_signers)" > release_signers
ssh-keygen -Y verify -f release_signers -I 136268860+y-o-o-z@users.noreply.github.com \
  -n erssi-release -s SHA256SUMS.sig < SHA256SUMS
sha256sum -c SHA256SUMS
rm release_signers
```

`release.sh upload shellter-vX DIR` uploads them to the GitHub release of
the tag, which it makes first if there is none (`gh release create
--verify-tag --notes-from-tag`). **The tarballs go first, SHA256SUMS and
SHA256SUMS.sig last.** The installer reads SHA256SUMS first. A tarball it
lists but cannot download makes it build from source, with a warning
(with `--binary` it refuses), so every install in between would take
minutes instead of seconds. By hand:

```sh
gh release upload shellter-vX erssi-X-*.tar.xz
gh release upload shellter-vX SHA256SUMS SHA256SUMS.sig
```

`upload` refuses while the tag is not on GitHub, or is another tag
object there, and when the signature or a sum in DIR does not check out.
An asset already in the release is left as it is: a published asset
does not change.

`release.sh confirm shellter-vX` downloads every asset of the release into
a new directory (`gh release download`) and checks it as the installer
would: the signature of `SHA256SUMS`, each sum, and that no package is
missing or unlisted and no other file is there.

Between the tag's push (step 7 of [A release](#a-release)) and the upload,
`erssi --update` builds the release from source, as before.

The assets are then at
`https://github.com/y-o-o-z/erssi/releases/download/shellter-vX/<asset>`.

### Testing the packager

`tests/version/test-build-binary.sh` runs `build-binary.sh` on a
throwaway repository holding this tree, tagged with a throwaway key:
- `build-binary verify`: the tags it refuses, and `--sums`;
- `build-binary package`: a whole package of the tree. It checks the
  name, the top directory, the order, owner and dates of the entries,
  the `MANIFEST` and erssi running from two places;
- `build-binary tests-fail`: no package when a test fails;
- `build-binary reproduce`: `--verify` on a package of the tree, built
  again in another work directory, says `reproduced`; on the same
  package with one file changed it says `NOT reproduced` and names the
  file; it refuses what is not a package and a package of another
  platform before building anything;
- `build-binary skips`: how the others skip.

`package`, `tests-fail` and `reproduce` build the tree from scratch with
every feature, a few minutes each (`reproduce` twice). They run only with
`ERSSI_TEST_PACKAGER=1`: `utils/pre-push` and the CI's build job (`erssi-ci/build`) set
it, and so must the release host before it builds packages:

```sh
ERSSI_TEST_PACKAGER=1 meson test -C Build
```

Without it they skip. So does the source build of an install or of
`erssi --update`, which must not depend on the packager's tools, on
every feature being there, or take minutes more. Every case also skips
when a tool of the packager (`git meson ninja perl tar xz strip readelf
od sort cc pkg-config ssh-keygen`) is missing from its own PATH. That is the case,
for example, with a meson in the installer's venv. With
`ERSSI_TEST_PACKAGER=1` a missing tool fails instead, so a pre-push, CI
or release host cannot lose the package builds without a word. Inside a package
build, `package`, `tests-fail` and `reproduce` skip as well. The copy of the tree
gets NEWS dated to the 1st of the month when the release is still
`YYYY-MM-xx`, so a push before the release commit can run them. In them
the build's own tests are limited to one or two (`BUILD_BINARY_TESTS`, which a
release never sets). `live relocatable` (tests/live/test_relocatable.py)
installs a relocatable build to two different places, and runs erssi
from both. In a build without the option it checks that the compiled
paths stay.

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
     must print nothing. Otherwise narrow the rule, for example
     `regex:word(?!N)==>other`, and check the narrowed form. `-G` takes a
     POSIX regex, so write it as an extended one:
     `git -C ~/src/erssi log --remotes=upstream --remotes=irssi -i -E -G 'word([^N]|$)' --oneline`
     must print nothing. The script's refusal in step 4 is the final
     check either way.
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
   - looks at the other branches of those remotes, as the old clone knows
     them:
     - A branch that holds commits whose content the rewrite changed or
       dropped keeps serving what was removed. The script names it with
       the delete command, for example `git push staging --delete
       dev/2026.10`.
     - A branch with commits that this rewrite did not see (only on
       staging, say) gets "check them for what was removed".
     - Branches whose content is unchanged, such as `gh-pages` (the web
       site), the `upstream/*` bases of pull requests to erssi-org, or a
       feature branch that never touched the removed files, are named
       once as "content unchanged … no action needed". Do not delete or
       rebase them;
   - prints the push commands.

   After any error, start again from step 2 with a new clone.

5. **Look before pushing.** Run `git log --oneline -5` and `git show HEAD`
   (the new pins and the mapped ids). Also check that the tags have their
   dates: `git for-each-ref refs/tags/shellter-v* --format='%(refname:short) %(taggerdate)'`.

6. **Push to staging first, then to origin**, with the commands it
   printed. Before that, deal with every branch it listed in step 4 as
   holding changed or dropped commits: delete it, or rewrite it and push it
   again the same way. Look through the "check them" branches. Leave the
   branches it called "no action needed" alone. The push of
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
   - the commit status `erssi-ci/installer` passes (it checks that every
     pinned release points to its commit);
   - the commit statuses: after a rewrite every release tag has a new
     commit, and the CI tests each of them again, one after another, with
     `main`. That can take hours; until a job has run, its status shows
     "pending", which is not a failure;
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
