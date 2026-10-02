# Fork y-o-o-z/erssi

Fork [erssi-org/erssi](https://github.com/erssi-org/erssi) z poprawkami
wynikającymi z codziennego używania erssi na IRCnet. Gałąź `main` = upstream
+ poniższe zmiany (każda w osobnym commicie, z opisem przyczyny).

| Zmiana | Problem, który usuwa |
|---|---|
| `fix(sidepanels)`: okno Notices nie przejmuje cudzych okien | Skrypt tworzący okno przy starcie (np. *Mentions*) tracił je — erssi przemianowywał okno nr 1 na „Notices”. |
| `feat(sidepanels)`: przycinanie do szerokości panelu z „…” | Długie nazwy (np. `#bash.org.pl`) były ucinane przez ramkę w pół słowa. |
| `feat(themes)`: sekcja `fe-text` w motywach dla `fe-ansi` | Motywy sprzed 1.3.0 traciły formaty paneli i paska (erssi czytało tylko `fe-ansi`). |
| `feat(nick-colors)`: 24-bit w `nick_hash_colors` + poprawne indeksowanie | Kolory nicków nie mogły pasować do motywu 24-bit; niepoprawne wpisy palety mogły być wylosowane. |
| `fix(fe-web)`: `is_highlight` jak w terminalu | Web nie oznaczał „hej yooz, …” jako wzmianki bez osobnego `/hilight`; `/me` nigdy. |
| `feat(anti-floodnet)`: `anti_floodnet_notices` | Komunikaty Anti-Floodnet (lokalne `printtext`) nie dały się ukryć `/ignore` ani skryptem. |
| `feat(themes)`: motyw `shellter` | Ciemny motyw w barwach shellter.me, kolumna nicków, panele bez urywania nazw. |
| `feat(fe-web)`: komenda z polem `target` wykonuje się w kontekście kanału | Komendy z weba nie miały okna: `/e2e on`, `/topic`, `/kick` bez kanału kończyły się „not in a channel”. |
| `fix(fe-web)`: zerwanie bez TLS close_notify = zwykłe rozłączenie | Każdy restart weba wypisywał w Notices `fe-web-ssl: ... unexpected eof while reading` i `Connection error`. |
| `docs(contrib)`: łatki NexusIRC (`contrib/nexusirc/`) | Web erssi@tahio: wzmianki, motyw `tahio`, strona logowania, historia bez dziur i okna Notices/Mentions z `webjournal.pl`. |

Testy: `meson test -C Build` (m.in. `tests/fe-common/core/test-nick-palette.c`).

## Budowa bez roota (np. konto na shellu)

```sh
git clone https://github.com/y-o-o-z/erssi.git ~/src/erssi && cd ~/src/erssi
meson setup Build -Dprefix=$HOME/.local/opt/erssi -Dwith-perl=yes -Dwith-proxy=yes \
  -Dwith-otr=yes -Dwith-fe-web=yes -Dwith-image-preview=yes -Ddisable-utf8proc=no
ninja -C Build && meson test -C Build && ninja -C Build install
~/.local/opt/erssi/bin/erssi
```

Synchronizacja z upstreamem: `git fetch upstream && git merge upstream/main`.
