# Installing and updating erssi Shellter Edition

Back to the [README](../../README.md).

- [Install](#install)
- [Binary packages](#binary-packages)
- [Options](#options)
- [FreeBSD](#freebsd)
- [Update](#update)
- [What the installer verifies](#what-the-installer-verifies)
- [Files in ~/.erssi](#files-in-erssi)
- [Upgrading from 1.3.x](#upgrading-from-13x)
- [Manual build](#manual-build)

## Install

One command, no root:

```sh
curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
```

On Debian 13 and FreeBSD 14 (amd64) this installs the ready-made binary
of the newest release in seconds (see [Binary packages](#binary-packages)).
Anywhere else, with `--source`, or when the binary cannot be used here, it
builds the release from source, about two minutes on a shell box; for
that the installer checks the system first.

- **As root** it lists the packages erssi needs (required and recommended,
  named for apt, dnf, apk, pacman, zypper or FreeBSD pkg) and installs them
  once you confirm (`--yes` skips the question). Root must use its own
  home: `sudo -i` or `su -`.
- **On a user account** it lists the missing packages with the command to
  give the administrator. There a missing `meson` or `ninja` is installed
  for you, only the tool that is missing, from a ready-made Python wheel
  into a venv next to the source directory (`~/.local/src/erssi.venv`; this
  needs python3). Where there is no wheel for the system - ninja on
  FreeBSD - the installer stops and names the package to ask for
  (`pkg install ninja`).

The source build takes the newest release recorded in the installer, only
if its tag points to the recorded commit and carries a valid signature by
the release key (checked with git 2.34+ and ssh-keygen; without them the
installer warns and checks the commit only), and runs the test suite.
Either way erssi is installed to `~/.local/opt/erssi`, linked as
`~/.local/bin/erssi`. Every step is one line; the output of the steps goes
to `~/.local/state/erssi/install.log`, shown when a step fails. Works on
Linux and FreeBSD.

## Binary packages

A release can carry one package per platform:
`erssi-<version>-<platform>.tar.xz` (platforms `debian13-amd64` and
`freebsd14-amd64`), listed in the release's `SHA256SUMS`, which the release
key signs (`SHA256SUMS.sig`). The installer tells the platform from
`uname`, `/etc/os-release` (or `freebsd-version`) and `perl -e 'print $^V'`,
downloads `SHA256SUMS`, `SHA256SUMS.sig` and the package with curl (fetch
on FreeBSD), and installs it only when every check passes:

- `SHA256SUMS` carries a valid SSH signature by the release key, made for
  erssi releases (namespace `erssi-release`; a signature made for git
  commits and tags does not count);
- the package has the sha256 that `SHA256SUMS` gives for it, and every file
  in it is below its one top directory;
- its `MANIFEST` names the release (`version`, `tag`), the commit the
  installer records for it (on `--update`: the commit of the signed release
  tag), your platform and the version of your Perl (erssi embeds Perl, so a
  package runs only with the Perl it was built with);
- the libraries it needs are there (`ldd` finds them all), and its erssi
  starts, says it is that release and finds its own files where it was
  unpacked (`erssi --print-prefix`).

A signature, checksum or manifest that does not verify stops the
installer ("refusing to install, nothing was changed"). A release without
packages, no package for your platform, another Perl, missing libraries
or an erssi that does not start here are not errors: the installer says
why (for missing libraries with the `apt` or `pkg` command that installs
them) and builds from source instead; `--binary` stops there instead.
Without a sha256 tool, xz (GNU tar), curl/fetch, or an ssh-keygen that
can check SSH signatures (OpenSSH 8.1 or newer; the installer tries it on
a signature it carries) the source build runs as well, as it did before
there were packages.

The package unpacks with the owner and the permissions of the installing
user (and its umask), never those stored in it, and a symbolic link or
hard link in it must stay inside its top directory.

Without `--ref` the installer takes the newest release it records. If the
installed erssi is newer than that (the copy of the installer an install
leaves in the prefix records only the releases before its own), it says
so and changes nothing: `erssi --update` finds newer releases, and
`--ref shellter-vX.Y.Z` (or `--rollback`) goes back on purpose.

What goes where:

| Path | What it is |
|---|---|
| `<prefix>/releases/<version>/` | The unpacked release, with its `MANIFEST`. |
| `<prefix>/current` | Link to `releases/<version>`, the release in use, switched in one step (a new link renamed over the old one). |
| `<prefix>/previous` | Link to the release in use before; `--rollback` returns to it. |
| `<prefix>/bin/erssi` | Link to `../current/bin/erssi`; `~/.local/bin/erssi` links to it as before. |
| `<prefix>/share/irssi/shellter-install.conf` | `METHOD=binary`, `VERSION`, `COMMIT`, `PLATFORM` and the settings below. |

The new release is unpacked next to the one in use and moved into place
before `current` is switched, so an interrupted install leaves the release
in use as it was. After the switch, old releases beyond `--keep N` (two by
default; the one `--rollback` returns to counts first) are removed.

With the installer of the installed release (or the `curl … | sh` command
with `-s --` and the same options):

```sh
sh ~/.local/opt/erssi/share/irssi/shellter-install.sh --rollback    # back to the previous release
sh ~/.local/opt/erssi/share/irssi/shellter-install.sh --rollback    # and forward again
sh ~/.local/opt/erssi/share/irssi/shellter-install.sh --update --dry-run   # download and check a newer release, change nothing
sh ~/.local/opt/erssi/share/irssi/shellter-install.sh --uninstall   # remove ~/.local/opt/erssi
```

After `--rollback`, `/upgrade` in a running erssi loads the release it
switched to. `--uninstall` removes the prefix and the `~/.local/bin/erssi`
link to it; `~/.erssi` (configuration, logs, keys) and the source
directory stay. It removes only a prefix this installer installed that
holds nothing but erssi (never the home, `~/.local`, `/usr`, `/usr/local`
or `/opt`).

## Options

| Option | What it does |
|---|---|
| `--prefix` | Where erssi is installed, an absolute path (default `~/.local/opt/erssi`). |
| `--ref <tag\|branch\|commit>` | What to build. Anything that is not a recorded release is built with a warning; a commit as the full SHA. |
| `--src` | The source directory (default `~/.local/src/erssi`). |
| `--no-test` | Skip the test suite (`--test` runs it again). |
| `--clean` | Remove the build directory afterwards, for small disk quotas (`--no-clean` keeps it again). |
| `--yes` | Install the packages as root without asking. |
| `--binary` | Install from the release's package only, and stop if that is not possible (this run only). |
| `--source` | Always build from source, also on `erssi --update` (until `--binary`). |
| `--binary-base <url\|dir>` | Where the packages are: a directory or URL holding `shellter-vX.Y.Z/` (default: the GitHub releases of `y-o-o-z/erssi`; another `--repo` has none unless given). |
| `--keep N` | Old releases of a binary install kept (default 2). |
| `--dry-run` | Check what would be installed (for a package: download and verify it into a temporary directory) and say what would change; change nothing. |
| `--rollback` | Switch a binary install back to the previous release (again: forward). |
| `--uninstall` | Remove the prefix and `~/.local/bin/erssi`; `~/.erssi` stays. |

`--src`, `--repo`, `--no-test`/`--test`, `--clean`/`--no-clean`, `--source`,
`--binary-base` and `--keep` are kept in
`<prefix>/share/irssi/shellter-install.conf`: the next install into the
same prefix and `erssi --update` use them again until you give them again.

The source directory and its venv must be yours, and every directory above
them yours or root's. In a rootless container or another user namespace see
`sh shellter-install.sh --help` (`SHELLTER_INSTALL_TRUST_OVERFLOW_UID`).

## FreeBSD

2026.10.0 builds with no warnings and passes its test suite on FreeBSD
14.5, with the OpenSSL of the base system.

- Packages: `pkg install git glib pkgconf perl5 python3 ninja` (and
  `meson`, or let the installer put it into its venv).
- Recommended: `utf8proc libotr libgcrypt curl chafa`.

Capsicum capability mode works: `/capsicum enter`, or `/set capsicum on`
and `/save` to enter it at every start, which
[docs/capsicum.txt](../capsicum.txt) recommends. erssi can then open files
only below `capsicum_irclogs_path` (`~/irclogs` by default), and `/save`
and `/search` do not work; `/connect` works. The shipped config logs to
`~/.erssi/logs`, so first `/set capsicum_irclogs_path ~/.erssi/logs` (and
`/save`), or autolog and the Mentions log stop writing. `/help capsicum`
has the details.

## Update

```sh
erssi --check-update     # is a newer release available?
erssi --update           # install it
```

`erssi --update` runs the installer shipped with the installed release: it
takes the newest release on GitHub that carries a valid signature by the key
of that installer (a tag pushed by anyone else is skipped) and installs it
into the same prefix, with the settings of the first install: from its
package when there is one for your system (checked as in [Binary
packages](#binary-packages), and the package must name the commit of the
signed tag), otherwise built and tested from source. The installer of
2026.10.3 and older has no binary path: it builds 2026.10.4 from source
once, and updates from 2026.10.4 on use the packages.

**The rules below hold from the 2026.10.3 installer on.** An erssi 2026.10.2
or older runs its own, older installer on its next `erssi --update`. That
installer also counts a signed tag of the installed version on another
commit as an update, and it does not check the recorded commit on an
update. To get the new rules right away, run the install command
(`curl … | sh`, with `-s -- --prefix DIR` if you gave a prefix) once instead
of `erssi --update`. After one update to 2026.10.3 or newer, every later
`erssi --update` follows the new rules.

- Only a newer version is an update. Release tags never move: a release
  made again gets a new version number. If the signed tag of the installed
  release points to another commit than the one installed, `--update` says
  so ("report it") and installs nothing.
- An erssi installed from a branch or a commit (`--ref main`) is told the
  newest release and offered it only when its version is newer than the
  installed one; `main` keeps the version of the last release until the
  next one.
- Nothing is built when erssi is up to date.
- A release that cannot be downloaded stops the update with "cannot
  download"; nothing is changed.
- What `--update` checks: [What the installer verifies](#what-the-installer-verifies).
- `--update` needs git 2.34+ and ssh-keygen to check the signature; without
  them it stops and names the install command above (with your `--prefix`):
  the current installer checks the newest release against the commit it
  records.
- A binary update keeps the release it replaces for `--rollback`.
- In a running erssi, `/upgrade` then loads the new version and keeps your
  windows and channels. Connections over plain text stay up; TLS
  connections cannot be handed to the new process and reconnect once (the
  channels see a quit "[TLS] Client upgrade" and a join).

## What the installer verifies

Releases are signed with an SSH key (ED25519,
`SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw`, public key in
[`utils/allowed_signers`](../../utils/allowed_signers)). The installer
carries that key and, for every release it knows, the commit the release
tag must point to. What it checks depends on how it runs:

1. **The first install, `curl … | sh`.** The script itself is trusted as
   downloaded: it comes from the `main` branch on GitHub over HTTPS and is
   not checked by anything before it runs. Everything after that is checked
   against the key and the commits it carries, so whoever could change the
   script you download could also change those. To check the script
   yourself first, see [Checking the installer
   yourself](#checking-the-installer-yourself) below.
2. **A recorded release** (the default, or `--ref shellter-vX.Y.Z` of a
   release the installer records). Its tag must be an annotated tag that
   names that release, carry a valid SSH signature by the release key
   (OpenPGP signatures and other keys are never accepted, whatever your
   keyring holds) and point to the recorded commit. Otherwise nothing is
   built. The step shows `signed · commit recorded`.
3. **Without ssh-keygen or with git older than 2.34.** The signature cannot
   be checked: the installer warns ("signatures cannot be checked here …
   the recorded commit is still checked") and builds the release only if
   its tag points to the recorded commit. The step shows `commit recorded`.
4. **Any other `--ref`** (a branch such as `main`, a commit, or a release
   tag this copy of the installer does not record yet) is built as it is,
   unverified, after a warning ("not a release - built as is, not
   verified", or "not recorded in this installer - not verified; download
   the installer again to verify it").
5. **`erssi --update`.** It runs the copy of the installer installed with
   erssi (`<prefix>/share/irssi/shellter-install.sh`), so the key is the
   one of your installed release, not of what GitHub serves now. It takes
   the newest `shellter-vX.Y.Z` tag that is an annotated tag of that name
   with a valid signature by that key and a newer version than the
   installed one; tags by any other key, unsigned or lightweight tags are
   named and skipped (five at most). A release that this copy records must
   also point to the recorded commit. A signed tag of the installed release
   that now points to another commit is reported and never installed.
   `--update` needs git 2.34+ and ssh-keygen and stops without them. A new
   release key would mean installing again with `curl … | sh`. This holds
   for an installed copy of 2026.10.3 or newer. The copy of 2026.10.2 or
   older updates once with its own rules (see [Update](#update)), so run
   `curl … | sh` once to switch over at once.
6. **A binary package** (2026.10.4 and newer, on its platforms). Its
   release's `SHA256SUMS` must carry a valid SSH signature by the release
   key in the namespace `erssi-release`, the package must match its sum
   there, and its `MANIFEST` must name that release, the commit this
   installer records for it (on `--update`: the commit of the signed tag
   found as in 5), the platform and the system's Perl. The step shows
   `signed · sha256 verified`, then `commit recorded` (or `commit of the
   signed tag`). A package of a release this copy does not record
   (`--ref shellter-vX.Y.Z`) is checked against the signature only, and
   the step says `release not recorded here`. The package is trusted as
   far as the release key is: the key signs the list of checksums, which
   pins the package, which names its commit.

### Checking the installer yourself

The release hook refuses an unsigned commit on `main`, so every commit
there is signed by the release key and the newest one can be checked. To
check the installer before you run it, take it from a clone instead of
`curl`. gpg is switched off for the check, as in the installer: git picks
the program from the signature, and an OpenPGP signature by any key in
your keyring would pass otherwise.

```sh
git clone --depth 1 https://github.com/y-o-o-z/erssi.git erssi-installer
cd erssi-installer
git -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false \
    -c gpg.format=ssh -c gpg.ssh.allowedSignersFile=utils/allowed_signers verify-commit HEAD
git log -1 --date=short --format='checked: %h %cd %s'
sed -n '/^RELEASES="/,/^"/p' shellter-install.sh | awk 'NF >= 2 { r = $1 } END { print "installs: " r }'
sh shellter-install.sh
```

Before the last command, check three things:

- `verify-commit` printed `Good "git" signature for
  136268860+y-o-o-z@users.noreply.github.com with ED25519 key
  SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw`.
- That fingerprint is the one you got another way: from the project
  website, from an earlier clone, or from the copy of an earlier install
  (`grep SHA256: ~/.local/opt/erssi/share/irssi/shellter-install.sh`). The
  clone checks the commit against a key from the same clone, so only that
  comparison shows the key is the real one.
- The release on the `installs:` line is the newest one on the [releases
  page](https://github.com/y-o-o-z/erssi/releases). A signed but older
  commit of `main` would install an older release.

To check a release tag by hand, in a clone (`git fetch --tags` in a depth-1
clone), with the same options:

```sh
git -c gpg.program=false -c gpg.openpgp.program=false -c gpg.x509.program=false \
    -c gpg.format=ssh -c gpg.ssh.allowedSignersFile=utils/allowed_signers verify-tag shellter-v2026.10.3
```

### Checking a binary package yourself

The release key for packages is the same key in the namespace
`erssi-release` (in `utils/allowed_signers` it is listed for `git`). For
release 2026.10.4 on Debian 13:

```sh
v=2026.10.4 p=debian13-amd64
base=https://github.com/y-o-o-z/erssi/releases/download/shellter-v$v
curl -fsSLO "$base/SHA256SUMS" -fsSLO "$base/SHA256SUMS.sig" -fsSLO "$base/erssi-$v-$p.tar.xz"
echo '136268860+y-o-o-z@users.noreply.github.com namespaces="erssi-release" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJmzPHTiL6dPEntggsVu9kJyAsY9KLaKNUW5kejyJOYS' > release_signers
ssh-keygen -Y verify -f release_signers -I 136268860+y-o-o-z@users.noreply.github.com \
    -n erssi-release -s SHA256SUMS.sig < SHA256SUMS
grep " erssi-$v-$p.tar.xz\$" SHA256SUMS | sha256sum -c -     # FreeBSD: sha256 -c <sum> erssi-$v-$p.tar.xz
tar -xJf "erssi-$v-$p.tar.xz" && cat "erssi-$v-$p/MANIFEST"
```

`ssh-keygen` must print `Good "erssi-release" signature for
136268860+y-o-o-z@users.noreply.github.com with ED25519 key
SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw` (the fingerprint you
got another way, as above), `sha256sum` must print `OK`, and the
`MANIFEST` must name `version=2026.10.4`, `tag=shellter-v2026.10.4`, your
platform and the commit of the release (`git rev-parse
'shellter-v2026.10.4^{commit}'` in a clone with the verified tag, or the
`RELEASES` line of a checked installer).

## Files in ~/.erssi

erssi keeps its configuration in `~/.erssi/`, so it runs side by side with
irssi. Start it inside tmux so the session survives logging out.

The themes and the `startup` file erssi copies into `~/.erssi/` are updated
with erssi, like the configuration files of a system package: a copy you
have not changed is replaced by the new version on the next start; a copy
you changed (also by `/save -formats`) is never touched, and erssi says once
where the new version is. The sha256 of what erssi installed is kept in
`~/.erssi/default-files.sha256`. Symbolic links are left alone.

## Upgrading from 1.3.x

`erssi --update` on 1.3.10 or a 2026.10 release finds 2026.10.3. 1.3.9
and older have no `--update`: run the install command above again (with
the same `--prefix` and `--src` if you gave any), which builds the new
release in the same places. Your `~/.erssi/config` is kept; at the first
start erssi adds what changed, once:

- **The Shellter look stays.** Up to 1.3.10 it was built into erssi; now it
  comes from the default config of a new home. Your config gets the former
  values (theme, nick column width, nick and rank colours, hilight
  colours, WHOIS in the network window, the join count line, 24-bit
  colours) written into it for every setting it does not set yet, so
  nothing you see changes. Settings you set yourself are not touched.
  erssi says so in one line after the banner.
- **E2E statusbar item.** If your info bar is still the one erssi shipped,
  it gets the `e2e` item after `window`. A bar you changed keeps its items:
  `/statusbar additem -after window e2e info`.
- **mentions.pl is built in now.** Its settings (`mentions_window`,
  `mentions_log`, `mentions_log_file`) move to the built-in Mentions
  window, and if the script was in `scripts/autorun` the log stays on.
  Loading the script only unloads it again with a notice; remove
  `~/.erssi/scripts/autorun/mentions.pl` to stop it.
- **Desktop notifications** (new) are off in an existing config:
  `/set term_notify auto` turns them on. The same goes for the Mentions
  log if you did not use mentions.pl: `/set mentions_log on`.
- **E2E with several networks:** keys are now kept per network, and an old
  key of a channel or query is blocked until `/e2e adopt` binds it to one
  network (see
  [End-to-end encryption](configuration.md#end-to-end-encryption)).

## Manual build

Debian / Ubuntu:

```sh
sudo apt install git gcc meson ninja-build pkgconf libglib2.0-dev libssl-dev \
  libperl-dev libutf8proc-dev libgcrypt20-dev libotr5-dev \
  libcurl4-openssl-dev libchafa-dev
```

FreeBSD (OpenSSL comes from the base system):

```sh
pkg install git meson ninja pkgconf glib perl5 utf8proc libgcrypt libotr curl chafa
```

Build, test and install:

```sh
git clone https://github.com/y-o-o-z/erssi.git && cd erssi
meson setup Build -Dprefix="$HOME/.local/opt/erssi" -Dwith-proxy=yes
ninja -C Build && meson test -C Build && ninja -C Build install
```

GLib (2.32 or newer) and OpenSSL are required, and Perl to build (the
build generates sources with it). Perl scripting (libperl), utf8proc,
libcurl + chafa (image preview) and libotr + libgcrypt (OTR) are detected
and used when present. For a system-wide install use
`-Dprefix=/usr/local` and `sudo ninja -C Build install`.
