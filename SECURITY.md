# Security

Please report a security problem privately, through GitHub: open the
[Security tab](https://github.com/y-o-o-z/erssi/security) of this
repository and choose "Report a vulnerability". Do not open a public issue
for it.

Say which version is affected (`erssi --version`) and how to reproduce the
problem. A fix is released as a new signed release, and `erssi --update`
installs it.

Releases are signed with an SSH key (ED25519,
`SHA256:hF7dvX7vdTfqEsC9LHeujMoHf4NhWdqeBCvN6jXEDAw`); how to check a release
by hand is in [docs/shellter/install.md](docs/shellter/install.md). What was
reviewed and how it is tested: [docs/shellter/security.md](docs/shellter/security.md).

Only the newest release gets security fixes.
