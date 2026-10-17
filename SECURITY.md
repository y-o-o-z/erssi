# Security

Please report a security problem privately, through GitHub: open the
[Security tab](https://github.com/y-o-o-z/erssi/security) of this
repository and choose "Report a vulnerability". Do not open a public issue
for it.

Say which version is affected (`erssi --version`) and how to reproduce the
problem. A fix is released as a new signed release, and `erssi --update`
installs it.

Releases are signed with an SSH key (ED25519,
`SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw`). The installer carries
that key and the commit of every release it records:

- `curl … | sh` trusts the script it downloads from `main`; everything the
  script installs is then checked against the key and commits it carries.
- A recorded release is built only if its tag is signed by the release key
  and points to the recorded commit; without ssh-keygen (or git before
  2.34) only the commit is checked, with a warning.
- Any other `--ref` (a branch, a commit) is built unverified, with a
  warning.
- `erssi --update` takes only a newer release signed by the key of the
  installed copy, and reports, never installs, a release tag that moved.

How to check the installer and a release by hand, and the details:
[What the installer verifies](docs/shellter/install.md#what-the-installer-verifies).
What was reviewed and how it is tested:
[docs/shellter/security.md](docs/shellter/security.md).

Only the newest release gets security fixes.
