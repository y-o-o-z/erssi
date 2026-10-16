# Installing and updating erssi Shellter Edition

Back to the [README](../../README.md).

- [Install](#install)
- [Options](#options)
- [FreeBSD](#freebsd)
- [Update](#update)
- [Release signatures](#release-signatures)
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
| `--prefix` | Where erssi is installed (default `~/.local/opt/erssi`). |
| `--ref <tag\|branch\|commit>` | What to build. Anything that is not a recorded release is built with a warning; a commit as the full SHA. |
| `--src` | The source directory (default `~/.local/src/erssi`). |
| `--no-test` | Skip the test suite. |
| `--clean` | Remove the build directory afterwards, for small disk quotas. |
| `--yes` | Install the packages as root without asking. |

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

- A release of the installed version published again with changes (its
  signed tag moved to a new commit) counts as an update too.
- Nothing is built when erssi is up to date.
- `--update` needs git 2.34+ and ssh-keygen to check the signature; without
  them it stops and says how to update with the installer's `--ref`
  instead.
- In a running erssi, `/upgrade` then loads the new version without
  disconnecting.

## Release signatures

Releases are signed with an SSH key (ED25519,
`SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw`, public key in
[`utils/allowed_signers`](../../utils/allowed_signers)). To check a release
by hand, in a clone:

```sh
git -c gpg.ssh.allowedSignersFile=utils/allowed_signers verify-tag shellter-v2026.10.2
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

`erssi --update` on 1.3.10 or a 2026.10 release finds 2026.10.2. 1.3.9
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
