# End-to-end encryption (RPE2E) for erssi

`rpe2e.pl` is the companion script for the **RPE2E v1.0** protocol from
[repartee](https://github.com/outragedevs/repartee) (`scripts/irssi/rpe2e.pl`,
MIT, repartee authors), byte-compatible with repartee and the WeeChat
`rpe2e.py` script. This copy has five small changes, each marked
`erssi/y-o-o-z` and described in the file header: Perl modules from
`~/perl5`, a version gate that understands erssi's own version numbers,
libsodium found as a versioned soname (`libsodium.so.23`) without the `-dev`
package, a regex fix for a Perl warning, and a step-by-step guide
under `/e2e`. The protocol code is untouched.

## Use

`/e2e` without arguments in a channel window prints the guide and the state of
that window: whether encryption is on and whose keys you hold.

First encrypted conversation on a channel:

1. in the channel window: `/e2e on`,
2. the other person does the same (repartee, irssi, erssi or WeeChat with
   `rpe2e.py`),
3. if *Pending key exchange from <nick>* appears after the first message,
   accept it: `/e2e accept <nick>` (or start it yourself:
   `/e2e handshake <nick>`),
4. `/e2e list` shows the peer as `[trusted]`; compare fingerprints over
   another channel: `/e2e verify <nick>`.

Each channel is switched on separately (`/e2e on` in its window), and so are
private conversations.

```
/e2e on                 encrypt this channel
/e2e off                stop encrypting
/e2e status             identity and channel state
/e2e list               trusted peers here (-all: everything)
/e2e handshake <nick>   exchange keys with someone
/e2e accept <nick>      accept a key exchange request
/e2e decline <nick>     decline it
/e2e fingerprint        your key fingerprint
/e2e verify <nick>      compare fingerprints with a peer
/e2e help               all commands
```

`/help e2e` describes every subcommand once `help/e2e` is copied
to `~/.erssi/help/`.

Once on, every message to the channel leaves as `+RPE2E01 …`
(XChaCha20-Poly1305, a separate key per sender and channel). Anyone without
the key sees ciphertext. Key requests travel as CTCP; in `normal` mode erssi
asks: *Pending key exchange from … Run /e2e accept <nick>*.

Encryption catches **every** outgoing line (`server outgoing modify`): plain
text, `/me`, `/msg #channel` — including messages sent from the web client.
Decryption happens before the rest of erssi, so the terminal, fe-web and the
`webjournal.pl` journal all receive plain text. `/e2e` typed in a web channel
window works with fe-web `target` support (y-o-o-z/erssi 1.3.2 and
NexusIRC patch 0009).

## Install (no root needed)

```sh
# Perl modules into ~/perl5 (needs the system libsodium, e.g. Debian libsodium23)
curl -fsSL -o /tmp/cpanm https://cpanmin.us
perl /tmp/cpanm -l ~/perl5 --notest App::cpanminus local::lib
eval "$(perl -I$HOME/perl5/lib/perl5 -Mlocal::lib)"
cpanm FFI::Platypus FFI::CheckLib Crypt::NaCl::Sodium

# from contrib/rpe2e of the erssi source tree
mkdir -p ~/.erssi/scripts/autorun ~/bin
cp rpe2e.pl ~/.erssi/scripts/ && ln -sfn ../rpe2e.pl ~/.erssi/scripts/autorun/rpe2e.pl
cp rpe2e-from-repartee ~/bin/   # only when moving keys from repartee
```

Keyring: `~/.erssi/rpe2e/keyring.json` (0600, directory 0700).

## Moving keys from repartee

So that peers do not see a “new key”, erssi can take over the repartee
identity:

```sh
rpe2e-from-repartee --dry-run          # what would be moved
rpe2e-from-repartee --channels-off     # write; channels wait for /e2e on
# then in erssi: /script load rpe2e
```

The tool opens the repartee database read-only, decrypts the stored secrets
(`REPARTEE_KEYRING_KEY` from `~/.repartee/.env`) and checks them
cryptographically: it derives the public key from the seed and compares
fingerprints. When a channel appears under several names (`#chan` and
`IRCnet\x1f#chan`) the newest session wins. An existing keyring is replaced
only with `--force` (a backup is kept). Secrets are never printed. Verified
on a real repartee keyring: the moved keys decrypted every `+RPE2E01`
message stored in the channel log.

If a peer knows you under a different `ident@host` (e.g. a new host), their
client warns about the change; the key is the same, so after comparing
fingerprints they run `/e2e reverify <your nick>`.

## Tests

- `python3 test_rpe2e_from_repartee.py` — migration on a synthetic repartee
  database (real libsodium keys, AES-256-GCM secrets), including rejection
  of a wrong key and inconsistent data,
- `test/run-e2e-test.sh [erssi] [rpe2e.pl] [~/nexus]` — two real erssi
  (alice, bob) and a local IRC server (`e2eircd.py`, 127.0.0.1) in a private
  tmux server: `/e2e on`, key exchange, a message, `/me`, `/msg #channel`;
  checks plain text at the receiver and that only ciphertext crossed the
  server. With a built NexusIRC directory alice turns E2E on through fe-web
  (Nexus's fe-web client, `target` field), exactly like the web. It never
  touches a running session. Result on erssi 1.3.2: 11/11.
