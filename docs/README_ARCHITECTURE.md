# erssi — architecture documentation

Developer-facing documentation about how the erssi codebase is put together.
Start here, then pick one of the two documents below.

## The two documents

### [`ARCHITECTURE.md`](ARCHITECTURE.md) — the overview

Read this first. It explains the shape of the codebase:

1. **The layers** — core → IRC protocol → frontend-agnostic UI → frontends,
   plus the cross-cutting subsystems (E2E, OTR, image preview, Perl).
2. **Directory map** — what lives where, as it is in the tree today.
3. **The signal system** — the gtk-like bus every layer communicates through.
4. **Message flow** — how one inbound IRC line travels from the socket to the
   screen (and to the browser).
5. **Module pattern** — `_init`/`_deinit`, per-server state via `MODULE_DATA`,
   with `src/irc/flood/` and `src/irc/anti-floodnet/` as reference modules.
6. **`credential.c`** — why the credential-encryption subsystem is disabled.
7. **Shellter subsystems** — E2E, OTR, image preview, fe-web, Perl.
8. **Building and testing** — meson options and why tests run under `meson test`.
9. **Memory management** — the GLib allocation rule.
10. **Where to look** — a task → file index.

**Best for:** understanding the design and finding your way around.

### [`QUICK_REFERENCE.md`](QUICK_REFERENCE.md) — the cheatsheet

A terser lookup guide: the signal names and signatures, the module
registration skeleton, the essential helper functions (`event_get_params`,
`ignore_check`, the `settings_*` and `signal_*` families), the GLib memory
idioms, the common per-server/per-channel loops, and the handful of gotchas
that bite newcomers.

**Best for:** quick lookup and copy-paste while you are writing code.

## See also

- [`../README.md`](../README.md) — what the Shellter Edition adds on top of erssi.
- [`UPSTREAM.md`](UPSTREAM.md) — how this edition tracks erssi/irssi upstream.
- [`signals.txt`](signals.txt) — the full upstream signal reference.
- [`shellter/`](shellter/) — configuration, features, security and release docs.

---

*Describes the tree as of the 2026.11.x series. erssi is the modern irssi by
Jerzy “kofany” Dąbrowski and the erssi-org team; the Shellter Edition is
maintained by the Shellter team.*
