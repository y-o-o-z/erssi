# Installing and updating erssi Shellter Edition

Back to the [README](../../README.md).

- [Install](#install)
- [Options](#options)
- [FreeBSD](#freebsd)
- [Update](#update)
- [What the installer verifies](#what-the-installer-verifies)
- [Files in ~/.erssi](#files-in-erssi)
- [Upgrading from 1.3.x](#upgrading-from-13x)
- [Manual build](#manual-build)

## Install

One command, no root, about two minutes on a shell box:

```sh
curl -fsSL https://raw.githubusercontent.com/y-o-o-z/erssi/main/shellter-install.sh | sh
```

The installer checks the system first.

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

It then builds the newest release recorded in the installer, only if its
tag points to the recorded commit and carries a valid signature by the
release key (checked with git 2.34+ and ssh-keygen; without them the
installer warns and checks the commit only), runs the test suite and
installs to `~/.local/opt/erssi`, linked as `~/.local/bin/erssi`. Every step
is one line; the build output goes to `~/.local/state/erssi/install.log`,
shown when a step fails. Works on Linux and FreeBSD.

## Options

| Option | What it does |
|---|---|
| `--prefix` | Where erssi is installed, an absolute path (default `~/.local/opt/erssi`). |
| `--ref <tag\|branch\|commit>` | What to build. Anything that is not a recorded release is built with a warning; a commit as the full SHA. |
| `--src` | The source directory (default `~/.local/src/erssi`). |
| `--no-test` | Skip the test suite (`--test` runs it again). |
| `--clean` | Remove the build directory afterwards, for small disk quotas (`--no-clean` keeps it again). |
| `--yes` | Install the packages as root without asking. |

`--src`, `--repo`, `--no-test`/`--test` and `--clean`/`--no-clean` are kept
in `<prefix>/share/irssi/shellter-install.conf`: the next install into the
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
erssi --update           # build and install it
```

`erssi --update` runs the installer shipped with the installed release: it
takes the newest release on GitHub that carries a valid signature by the key
of that installer (a tag pushed by anyone else is skipped), builds, tests
and installs it into the same prefix, with the settings of the first install.

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
- In a running erssi, `/upgrade` then loads the new version without
  disconnecting.

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
