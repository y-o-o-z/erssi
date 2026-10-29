# Contributing to erssi Shellter Edition

Thank you for helping. This repository is an edition of
[erssi](https://github.com/erssi-org/erssi); fixes that belong to erssi
itself are also offered to erssi-org as pull requests
([docs/UPSTREAM.md](docs/UPSTREAM.md)).

## Questions and bugs

Open an issue: https://github.com/y-o-o-z/erssi/issues. Say which
version you run (`/version` or `erssi --version`), your system and
terminal, and the steps that show the problem. A security problem goes
through a private report instead, see [SECURITY.md](SECURITY.md).

## Building and testing

```sh
meson setup Build
ninja -C Build
meson test -C Build
```

The tests of the release tools (the installer, the packager, the release
hook) are in the suite `tooling`: `meson test -C Build --suite tooling`
runs only them, `--no-suite tooling` everything else (what the installer
runs). A change to those tools runs both.

The tests run with `HOME` inside the build directory, so they never write
into your own `~/.erssi`. To try a build by hand, give it a home of its
own, so it cannot touch your configuration or logs:
`HOME=/tmp/erssi-try Build/src/fe-ansi/erssi --home=/tmp/erssi-try/.erssi`.
For memory errors, build a second tree with
`-Db_sanitize=address,undefined -Db_lundef=false` and run the same tests
there.

## Pull requests

- One topic per commit, with a test for every change in behaviour: a live
  test in `tests/live/` (a real erssi in a terminal against a small fake
  IRC server) or a unit test next to the code.
- The subject says what is true after the commit, with the area first,
  for example `fe-ansi: /window size -right N works`; the body says why.
- Follow the C style of the file you change (tabs, glib, irssi idioms).
  Comments and messages are in English.
- New commands and settings get a help page in `docs/help/in/`.
- Describe the user-visible change in the pull request; the release notes
  in [NEWS](NEWS) are written when a release is made.

Releases are made by the Shellter team with the release hook
`utils/pre-push`; the rules and the procedures (a release, and the
one-time re-signing of the release tags) are in
[docs/shellter/releasing.md](docs/shellter/releasing.md).

Contributions are accepted under the GNU General Public License, version
2 or later, like the rest of the code.
