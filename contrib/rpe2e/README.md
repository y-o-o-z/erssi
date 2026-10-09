# End-to-end encryption (RPE2E) for erssi

`rpe2e.pl` is the companion script for the **RPE2E v1.0** protocol from
[repartee](https://github.com/outragedevs/repartee) (`scripts/irssi/rpe2e.pl`,
MIT, repartee authors; licence text in [LICENSE](LICENSE)), byte-compatible
with repartee and the WeeChat `rpe2e.py` script. The wire format is
unchanged. Every change is marked `erssi/y-o-o-z` and listed in the file
header:

- loading: Perl modules from `~/perl5`, a version gate that understands
  erssi's own version numbers (by ABI version, so the binary name does not
  matter), libsodium found as a versioned soname (`libsodium.so.23`) without
  the `-dev` package, a step-by-step guide under `/e2e`;
- fixes (0.2.2): non-ASCII text is UTF-8 encoded once (repartee's script
  encoded erssi's bytes a second time, so "zażółć" arrived garbled); the
  keyring is never overwritten after a failed read or a failed write (a
  corrupt one is kept as `keyring.json.corrupt-<time>`); key-exchange
  NOTICEs are rate-limited and write the keyring only when something
  changed; `...` and `!!` lines are encrypted (only `.cmd`/`!cmd` go out in
  plain text for bots); `/quote privmsg` and tagged lines are encrypted;
  `/e2e export` creates its file 0600 and never overwrites a file;
  `/e2e import` keeps `keyring.json.bak-<time>`.

## Use

`/e2e` without arguments in a channel or query window prints the guide and
the state of that window: whether encryption is on and whose keys you hold.

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
to `~/.erssi/help/` (see Install).

Once on, every message to the channel leaves as `+RPE2E01 …`
(XChaCha20-Poly1305, a separate key per sender and channel). Anyone without
the key sees ciphertext. Key requests travel as CTCP; in `normal` mode erssi
asks: *Pending key exchange from … Run /e2e accept <nick>*. One exception, for
channel bots: a channel line starting with `.` or `!` and a letter (`.op`,
`!seen bob`) goes out in plain text, with a warning.

Encryption catches **every** outgoing line (`server outgoing modify`): plain
text, `/me`, `/msg #channel` — including messages sent from the web client.
Decryption happens before the rest of erssi, so the terminal, fe-web and the
`webjournal.pl` journal all receive plain text. `/e2e` typed in a web channel
window runs in that channel since erssi Shellter Edition 1.3.2 (fe-web
`target` field, commit `6d94f04f`) with NexusIRC patch 0009; before that
fe-web ran it without a window ("not in a channel or query"). In the
terminal it always works.

## Install (no root needed)

```sh
# Perl modules into ~/perl5 (needs the system libsodium, e.g. Debian libsodium23);
# cpanminus is piped straight into perl - no file in a shared /tmp
curl -fsSL https://cpanmin.us | perl - -l ~/perl5 App::cpanminus local::lib
eval "$(perl -I$HOME/perl5/lib/perl5 -Mlocal::lib)"
cpanm FFI::Platypus FFI::CheckLib Crypt::NaCl::Sodium

# from contrib/rpe2e of the erssi source tree
mkdir -p ~/.erssi/scripts/autorun ~/.erssi/help ~/bin
cp rpe2e.pl ~/.erssi/scripts/ && ln -sfn ../rpe2e.pl ~/.erssi/scripts/autorun/rpe2e.pl
cp help/e2e ~/.erssi/help/      # /help e2e
cp rpe2e-from-repartee ~/bin/   # only when moving keys from repartee
```

Keyring: `~/.erssi/rpe2e/keyring.json` (0600, directory 0700). A keyring
that cannot be read is left alone and nothing is saved until it can be (the
outbound gate then refuses to send rather than send plain text); one that is
not valid JSON is moved aside to `keyring.json.corrupt-<time>` and a new one
is started.

## Moving keys from repartee

So that peers do not see a “new key”, erssi can take over the repartee
identity:

```sh
rpe2e-from-repartee --dry-run          # what would be moved
# in erssi first: /script unload rpe2e  (nothing saves while the file is replaced)
rpe2e-from-repartee --channels-off     # write; channels wait for /e2e on
# then in erssi: /script load rpe2e
```

The tool opens the repartee database read-only, decrypts the stored secrets
(`REPARTEE_KEYRING_KEY` from `~/.repartee/.env`) and checks them
cryptographically: it derives the public key from the seed and compares
fingerprints. When a channel appears under several names (`#chan` and
`IRCnet\x1f#chan`) the newest session wins, with a warning when the
variants come from different networks. An existing keyring is replaced
only with `--force` (a backup is kept). Secrets are never printed. Verified
on a real repartee keyring: the moved keys decrypted every `+RPE2E01`
message stored in the channel log.

If a peer knows you under a different `ident@host` (e.g. a new host), their
client warns about the change; the key is the same, so after comparing
fingerprints they run `/e2e reverify <your nick>`.

## Tests

- `prove -I test/lib test/rpe2e.t` — unit tests of `rpe2e.pl` with the irssi
  API mocked and the real crypto modules: UTF-8 in both directions, chunk
  budget, AAD, keyring read/parse/write failures (including a full disk),
  NOTICE flood, bot bypass, export/import, the gate regex, the `/e2e` guide,
  the version gate,
- `python3 test_rpe2e_from_repartee.py` — migration on a synthetic repartee
  database (real libsodium keys, AES-256-GCM secrets), including rejection
  of a wrong key and inconsistent data,
- `test/run-e2e-test.sh [erssi] [rpe2e.pl] [~/nexus]` — two real erssi
  (alice, bob) and a local IRC server (`e2eircd.py`, 127.0.0.1) in a private
  tmux server: `/e2e on`, key exchange, a message, `/me`, `/msg #channel`,
  Polish text, a `...` line, `/quote privmsg`; checks plain text at the
  receiver and that only ciphertext crossed the server. With a built
  NexusIRC directory alice turns E2E on through fe-web (Nexus's fe-web
  client, `target` field), exactly like the web. It never touches a running
  session. Result 2026-10-09 on erssi 1.3.10: 13/13.
