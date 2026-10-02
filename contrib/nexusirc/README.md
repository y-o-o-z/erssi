# NexusIRC — „Recent mentions” dla użytkowników erssi

[NexusIRC](https://github.com/kofany/nexus) (webowy frontend erssi przez
fe-web, fork The Lounge) ma w kliencie okno **Recent mentions**, ale w trybie
irssi/erssi serwer nigdy go nie wypełnia: tablica `IrssiClient.mentions` jest
zadeklarowana i pusta, a zdarzeń `mentions:get`, `mentions:dismiss` i
`mentions:dismiss_all` nikt nie obsługuje.

`0001-recent-mentions-for-erssi-users.patch` (względem `kofany/nexus`
ba3fb21):

- zapamiętuje wiadomości oznaczone jako podświetlenie (bez własnych), do 100
  najnowszych — tak jak The Lounge; o podświetleniu decyduje flaga fe-web
  `is_highlight` (reguły terminala erssi), a nick w treści tylko jako zapas,
  gdy Nexus zna już prawdziwy nick (zaraz po połączeniu bywa pusty lub „*”),
- obsługuje `mentions:get` / `mentions:dismiss` / `mentions:dismiss_all`.

Razem z poprawką fe-web w tym forku (`is_highlight` liczone tak jak w
terminalu) web pokazuje te same wzmianki co okno *Mentions* w erssi.

Zastosowanie w lokalnej kopii Nexusa:

```sh
cd ~/nexus
git checkout -b local/mentions
git am /ścieżka/do/0001-recent-mentions-for-erssi-users.patch
yarn build:server
```
