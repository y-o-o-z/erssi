# NexusIRC dla erssi — łatki

[NexusIRC](https://github.com/kofany/nexus) to webowy frontend erssi przez
fe-web (fork The Lounge). Łatki poniżej (seria względem `kofany/nexus`
ba3fb21, nakładane po kolei przez `git am`) robią z niego web, który pokazuje
to samo co terminal erssi, pod marką **erssi@tahio**.

| Łatka | Co daje |
|---|---|
| `0001-recent-mentions-for-erssi-users.patch` | Okno *Recent mentions* działa dla erssi (wcześniej zawsze puste). |
| `0002-erssi-tahio-branding-theme-sign-in.patch` | Marka erssi@tahio, motyw `tahio` (miedź i krem, kroje Geist), nowa strona logowania po polsku. |
| `0003-sign-in-ircnet-map-cache-busting.patch` | Strona logowania z panelem mapy IRCnetu; cache-busting z treści builda. |
| `0004-erssi-journal-history-and-windows.patch` | Historia bez dziur i okna erssi bez kanału (Notices, Mentions, skaner, status sieci) z dziennika `webjournal.pl`. |
| `0005-erssi-windows-ui.patch` | Okna erssi bez przycisków zamknij/dołącz; linie tekstu bez kolumny nicka. |
| `0006-fe-web-close-handshake.patch` | Zamknięcie połączenia z fe-web z handshake, bez błędów SSL w erssi przy restarcie. |
| `0007-erssi-journal-line-order.patch` | Linie wypisane w tej samej milisekundzie zostają w kolejności z erssi. |
| `0008-erssi-journal-client-lines.patch` | Komunikaty skryptów w oknach kanałów (np. `[E2E] …` z `rpe2e.pl`) widoczne w webie na żywo. |
| `0009-fe-web-command-target.patch` | Komenda wpisana w oknie kanału idzie z polem `target` — erssi wykonuje ją w kontekście tego kanału (`/e2e on`, `/topic`, `/kick`). |

## 0001 — Recent mentions

W trybie irssi/erssi serwer nigdy nie wypełniał okna: tablica
`IrssiClient.mentions` była pusta, a zdarzeń `mentions:get`,
`mentions:dismiss` i `mentions:dismiss_all` nikt nie obsługiwał. Łatka
zapamiętuje podświetlenia (bez własnych), do 100 najnowszych, jak The Lounge.
O podświetleniu decyduje flaga fe-web `is_highlight` (reguły terminala erssi,
poprawione w tym forku), a nick w treści służy tylko jako zapas.

## 0002, 0003 — erssi@tahio

- motyw `client/themes/tahio.css` w tożsamości tahio (krem `#f3ede1`, papier
  `#fffdf8`, miedź `#b25f10`), kroje Geist i Geist Mono dołączone lokalnie
  (OFL, `client/themes/tahio/fonts/OFL.txt`); kolumna czasu w Geist Mono,
- strona logowania: formularz po polsku (pokaż/ukryj hasło, Caps Lock,
  komunikat błędu z fokusem w polu hasła) i panel z mapą Europy z drzewem
  łączy w stylu IRCnetu, wygenerowany przez `scripts/generate-ircnet-map.py`
  z danych Natural Earth (domena publiczna); animacja wyłącza się przy
  `prefers-reduced-motion`,
- ikony, manifest i ekran ładowania z marką erssi@tahio,
- cache-busting: vendor chunk nazwany hashem treści, `?v=` liczone z
  `bundle.js`, `style.css` i arkuszy motywów zamiast z numeru wersji —
  lokalna przebudowa bez zmiany wersji nie jest już podawana z cache CDN
  (Cloudflare trzymał stary `bundle.vendor.js` i strona się nie ładowała).

## 0004, 0005 — web pokazuje to samo co terminal

fe-web przesyła tylko zdarzenia na żywo i tylko kanały oraz rozmowy. Wszystko,
co padło, gdy Nexus nie działał (restart, przebudowa), nigdy nie trafiało do
jego bazy, a okien bez kanału web w ogóle nie znał.

Skrypt `webjournal.pl` z
[y-o-o-z/irssi_scripts](https://github.com/y-o-o-z/irssi_scripts) zapisuje
każde okno erssi do `~/.erssi/journal/` (JSONL). `server/erssiJournal.ts`:

- przy starcie dopisuje do bazy wpisy dziennika, których w niej nie ma
  (porównanie: rodzaj, nick, treść, ±5 s — zdarzenia na żywo mają czas
  odbioru przez Nexusa, nie erssi),
- czyta na żywo okna bez kanału i pokazuje je w grupie „erssi” (Notices
  pierwsze, potem Mentions, skaner…); okno statusu sieci trafia do lobby
  sieci; komendy wpisane w tych oknach wykonuje erssi,
- pokazuje na żywo komunikaty klienta i skryptów z okien kanałów (wpisy
  `text` w plikach kanałów, `webjournal.pl` ≥ 1.1.0) — fe-web przesyła tylko
  wiadomości, a bez tego w webie nie byłoby widać np. prośby o wymianę kluczy
  E2E (łatka 0008),
- katalog dziennika: `ERSSI_JOURNAL_DIR`, domyślnie `~/.erssi/journal`; bez
  dziennika Nexus działa jak dotąd.

Baza Nexusa sortuje po czasie w pełnych milisekundach, więc czasy wpisów
jednego pliku są ściśle rosnące (remis = poprzedni + 1 ms, łatka 0007) —
inaczej np. błąd i jego dalszy ciąg zamieniały się miejscami.

Testy: `test/tests/erssiJournal.ts` (kodowanie nazw jak w skrypcie,
deduplikacja, kolejność, rotacja, linia ucięta w środku znaku UTF-8).

## 0006 — zamknięcie połączenia

`FeWebSocket.disconnect()` zrywał połączenie (`terminate()`) bez TLS
close_notify, więc erssi przy każdym restarcie Nexusa wypisywało w Notices
`fe-web-ssl: ... unexpected eof while reading` i `Connection error`. Teraz
najpierw `close()`, `terminate()` dopiero po 1 s. Ten fork erssi i tak
traktuje takie zerwanie jak zwykłe rozłączenie (`fix(fe-web)`).

## Zastosowanie

```sh
cd ~/nexus
git checkout -b local/tahio-theme ba3fb21
git am /ścieżka/do/contrib/nexusirc/0*.patch
NODE_ENV=production yarn build      # klient i serwer
```

W `~/.nexusirc/config.js`: `theme: "tahio"`. Zatrzymuj Nexusa łagodnie
(SIGTERM / Ctrl-C), nie zabijając sesji tmux.
