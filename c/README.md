# aqnapi — natywna wersja C (Cosmopolitan)

Niezależna reimplementacja aqnapi w czystym C, kompilowana przez
[`cosmocc`](https://github.com/jart/cosmopolitan) do jednej uniwersalnej binarki
APE (`dist/aqnapi-c.com`) działającej na Linux/macOS/Windows/*BSD (x86-64+ARM64).

To **pełny natywny port** obok wersji Python (`aqnapi.py`) — pokrywa **100%
poleceń** i jest **bajtowo zgodny** z Pythonem dla zaimplementowanego zakresu.
Wersja Python pozostaje kompletna i referencyjna; każda zmiana idzie w OBU
wersjach (zob. `CLAUDE.md`). Wszystko jest tutaj: offline, downloady, uploady
(napiprojekt 7z-AES/`--login`, napisy24 attach/WWW, agregujący `upload`),
account/associate, OpenSubtitles (login/logout/search/download/formats/languages/
guessit), search, sync (interaktywny TUI), config, update, URL Range.

## Zakres (stan bieżący)

**Zaimplementowane w C i zweryfikowane bajtowo z wersją Python:**

| Polecenie | Zgodność |
|---|---|
| `hash PLIK` | bajtowo == `aqnapi hash` |
| `fps PLIK` (MKV/AVI/MP4/MOV) | FPS **oraz czas trwania** (linia `Czas:`) jednym przejściem (`media_from_file`); bajtowo == `aqnapi fps` |
| `convert` — **wszystkie formaty wejścia** (SRT/MicroDVD/MPL2/TMPlayer/VTT/ASS) | **plik + stdout** bajtowo == `aqnapi convert` |
| `convert --format srt\|vtt\|ass\|microdvd` (eksport) | bajtowo |
| flagi: `--strip-sdh --keep-tags --no-sanitize --max-display --min-display` | bajtowo |
| dekodowanie wejścia **cp1250** (nie-UTF-8) | bajtowo (Polski) |
| pełna **sanityzacja** (tagi, długie, nakładki, złe/puste czasy) + raport „Korekty" | bajtowo |
| `fpsconv --from --to [--movie]` | bajtowo (z bankierskim zaokrągleniem) |
| `merge` (auto/`--offset`) | bajtowo |
| `split --at [--no-rebase]` | bajtowo |
| `config {init,show,path}` | plik i `show` bajtowo == Python (hasła bez echo, chmod 600) |
| `sync REF TGT` | **interaktywny TUI** (termios+ANSI: 2 kolumny, TAB, ↑↓/jk, ENTER łączy pary, edycja czasów `,./<>e`, `a` zapis, `q` wyjście) oraz `--offset`/`--anchor` (nieinteraktywny, bajtowo == Python). TUI zweryfikowany przez pty |
| `search` / `napiprojekt search` / `napisy24 search` (agreg. + per-serwis) | stdout bajtowo == Python (live) |
| `get` (agregator np→n24, HTTP) | pobiera i zapisuje SRT |
| `napiprojekt download` / `fileinfo` (HTTP) | bajtowo |
| `napiprojekt account` / `associate` (HTTP GET, hasło jawne) | account: dane konta (parser XML w kolejności dokumentu); associate: powiązanie hasza z `id_filmu`. Werdykt/wyjście bajtowo == Python (live: areq) |
| `napiprojekt cover` (mode=2) / `version` (mode=16) / `report` (mode=64) | okładka+ocena filmu (base64 JPEG, CDATA zdejmowane jak ElementTree) / najnowsza wersja klienta / zgłoszenie złych napisów (`--kind`/`--list`, user_nick/user_password). Bajtowo == Python (live: cover E02, version, report --list/bad-kind) |
| `napisy24 download` (CheckSubAgent+ZIP) / `getid` (download.php+ZIP) | **plik+stdout bajtowo** (ZIP-inflate przez zlib) |
| `napiprojekt upload` (mode=512/1024, **7z-AES**, `--login`) | własny AES-256+SHA-256+kontener 7z; **archiwum rozpakowywalne przez `7z x`**; `--login` → pola `user_nick`/`user_password` (upload przypisany do konta); odpowiedź serwera == Python |
| **URL http(s) jako wejście** (`--movie`/`--srt`/`input`/…) | pobieranie **zakresowe** (HTTP Range): md5-10MiB → 10 MiB, OSH → rozmiar+2×64 KiB, FPS → prefiks 8 MiB (`fmemopen`), napisy → całość; Basic auth z `user:pass@`. **http w obu buildach; https tylko w wariancie TLS.** Hash 1.7 GB filmu bajtowo == Python (live, https) |
| `napisy24 attach` (**AddSubPrg.php**, `--check-only`) | działająca ścieżka API klienta, **plain HTTP → oba buildy**. Dwufazowo Check→Send, pola zaciemniane (`n24_obf`), `hs`=`subtitle_hash`. Powiązanie po haszu, bez wpisu publicznego. Werdykt Check bajtowo == Python (live: OK-2 https, OK-0 lokalnie) |
| `napisy24 login` (CheckLogin) / `imdb` (CheckIMDB) | plain HTTP, multipart klienta; login: pola zaciemniane, imdb: `imdbId` jawne. Wyjście bajtowo == Python (live: login ok/złe hasło, imdb) |
| `napisy24 mediainfo`/`notify`/`trans`/`premieres` | plain HTTP (oba buildy): ChangeData/Notifiemail/Get+SetTrans/GetIMDB.php; pola zaciemniane (`n24_obf`), odpowiedź dekodowana `utf8_replace` (jak Python `.decode(utf-8,replace)`). Bajtowo == Python (live: premieres/trans/mediainfo-auto/notify-off) |
| `napisy24 edit` (`--show`, `--set`, `--srt`) | **wariant TLS**: WWW `/dodaj-napisy?edytuj=` — scraper formularza RSForm (input/select/radio/checkbox), nadpisania + ponowny POST. `--show` bajtowo == Python (live) |

Własna kryptografia zweryfikowana: **AES-256 (wektor FIPS-197)**, **SHA-256**,
oraz round-trip **7z-AES przez systemowe `7z`** (`aqnapi-c.com _selftest OUT.7z`).

| **agregujący `upload`** (`--service np,n24,os`) | orkiestracja per-serwis (domyślnie `np`); wypisuje `[OK/BŁĄD] serwis: komunikat` w kolejności np→n24→os. np: plain HTTP (oba buildy); n24 walidacja+dry-run (wariant TLS); os: stały komunikat. **Bajtowo == Python** (live: np/os/n24-dry, walidacja wszystkich problemów) |
| `update [--check]` | **wariant TLS**: HTTPS do GitHub API przez mbedtls, porównanie wersji, podmiana binarki. `--check` zweryfikowany na żywo |
| `opensubtitles login/logout/search/download/formats/languages/guessit` | **wariant TLS**: pełny klient REST v1 (Api-Key + JWT), HTTPS przez mbedtls, parser JSON. login zapisuje cache tokenu (kompatybilny z Pythonem), logout czyta cache + DELETE. `guessit` — własny pretty-printer JSON (`indent=2`, `ensure_ascii=False`). **Wszystkie bajtowo zgodne z Pythonem** (zweryfikowane kluczem na żywo) |
| `napisy24 weblogin` | **wariant TLS**: logowanie WWW (Joomla/Community Builder `cb-login`) — cookie-jar + skrobanie tokena CSRF + sesja RSForm. Zweryfikowane na żywo („Zalogowano") |
| `napisy24 upload/delete` | **wariant TLS**: upload przez formularz RSForm (multipart, walidacja lokalna ≤2 linie + normalizacja CRLF), delete `?usun=`. Pola auto-wypełniane z `--movie`: `release` przez `n24_release` (bajtowo == Python, bateria 8 nazw), czas trwania + fps z kontenera (`media_from_file`), rozmiar = długość pliku. Bezpiecznie zweryfikowane: `--dry-run` (jak Python) + `delete` przez autoryzowaną sesję (bez realnego wpisu) |

## Dwa warianty binarki C

- **`dist/aqnapi-c.com`** — build `cosmocc` (`c/build.sh`), lekki, **bez TLS**.
  `update`/`opensubtitles` wypisują, że wymagają wariantu TLS.
- **`dist/aqnapi-c-tls.com`** — build monorepo + `third_party/mbedtls`
  (`c/build-tls.sh`), **z TLS**. Ma działające `update` (HTTPS). Kod TLS jest pod
  `#ifdef AQNAPI_TLS` (włączany flagą `-DAQNAPI_TLS` w buildzie monorepo).

**TLS — jak redbean:** monorepo Cosmopolitan + `third_party/mbedtls` (MbedTLS
2.26). Zweryfikowano na żywo: handshake TLS 1.2 i pełny HTTPS do
`api.opensubtitles.com`/`api.github.com`/`napisy24.pl`. Szczegóły/PoC: [`tls/`](tls/).

**Weryfikacja CA — włączona:** wariant TLS ma osadzony bundle Mozilli
(`/zip/cacert.pem`, wbudowany przez `build-tls.sh`) i używa
`MBEDTLS_SSL_VERIFY_REQUIRED`. Zweryfikowane: prawdziwe certy (github/
opensubtitles) przechodzą, a **podstawiony fałszywy CA jest odrzucany** (test
negatywny). Fallback: systemowy `ca-certificates.crt`, ostatecznie brak
weryfikacji, gdy bundla nie ma.

**Pokrycie: 100% poleceń** wersji Python — offline, downloady, uploady
(napiprojekt 7z-AES/`--login`, napisy24 attach/WWW, **agregujący `upload`**),
napiprojekt account/associate, napisy24 login/imdb/attach/WWW, search,
OpenSubtitles (login/logout/search/download/formats/languages/guessit), sync
interaktywny, config, update, URL Range, TLS+CA.

> `iso-8859-2` jako drugorzędny fallback kodowania oraz kilka rzadkich, niezdefiniowanych
> bajtów cp1250 są uproszczone względem Pythona (nie dotyczy typowych polskich napisów).

## Budowanie

```sh
c/build.sh            # -> dist/aqnapi-c.com  (pobierze cosmocc do c/toolchain/ za 1. razem)
COSMOCC_URL=... c/build.sh
./dist/aqnapi-c.com --help
```

### Build natywny (bez Cosmopolitan — gcc/clang/musl, OpenWrt)

Źródło jest przenośnym C99 + POSIX — kompiluje się też zwykłym kompilatorem do
natywnego ELF (przez `#ifdef __COSMOPOLITAN__` include'y `third_party/*` mają
systemowe warianty `<zlib.h>` / `<mbedtls/*.h>`, a `_GNU_SOURCE` włącza
`fmemopen`/`strcasestr`).

```sh
c/build-native.sh                 # host gcc, BAZOWY (offline + HTTP), potrzebuje tylko -lz
AQNAPI_TLS=1 c/build-native.sh    # + HTTPS (opensubtitles, napisy24 WWW, update, url https)
                                  #   wymaga systemowego mbedtls (libmbedtls-dev)
```

Cross-compile pod **OpenWrt** (SDK toolchain):

```sh
CC="$SDK/staging_dir/toolchain-*/bin/mipsel-openwrt-linux-musl-gcc" \
CFLAGS="-O2 -I$STAGING_DIR/usr/include" LDFLAGS="-L$STAGING_DIR/usr/lib" \
AQNAPI_TLS=1 OUT=aqnapi-owrt c/build-native.sh
# na routerze:  opkg install zlib libmbedtls
```

Gotowy **pakiet OpenWrt** (`.ipk`/`.apk`) — formuła i instrukcja w
[`openwrt/`](../openwrt/README.md).

Zależności: **zlib** (rozpakowanie ZIP napisy24); **mbedtls** tylko przy
`AQNAPI_TLS=1`. Weryfikacja CA szuka po kolei: bundle APE (`/zip/cacert.pem`,
tylko cosmo), `$SSL_CERT_FILE`, typowe ścieżki (Debian/OpenWrt
`/etc/ssl/certs/ca-certificates.crt`, BSD `/etc/ssl/cert.pem`, itd.) —
na OpenWrt zainstaluj `ca-bundle`. Build natywny bez TLS nie potrzebuje CA.
Wyjście binarki natywnej jest **bajtowo zgodne** z cosmo/APE i Pythonem.

## Zgodność — weryfikacja

Zgodność bajtowa z wersją Python była sprawdzana przez porównanie wyjść na tych
samych wejściach:

```sh
# przykład: konwersja daje identyczny plik i stdout
diff <(./dist/aqnapi-c.com convert in.srt -o /tmp/c.srt) \
     <(python3 aqnapi.py    convert in.srt -o /tmp/p.srt)
cmp /tmp/c.srt /tmp/p.srt      # -> identyczne
```

Zweryfikowano bajtowo: `hash`, `fps` (MKV/AVI/MP4), `convert` (SRT/MicroDVD/VTT +
sanityzacja + stdout), oraz komunikaty `download` (ścieżka not-found na żywo).

## Implementacja (samowystarczalna, bez zależności)

`aqnapi.c` zawiera własne: MD5 (RFC 1321), hash OSH, parsery FPS (EBML/RIFF/ISO
BMFF), silnik napisów (SRT/MicroDVD/VTT → sanityzacja → SRT UTF-8+BOM/LF),
dekoder base64 i minimalny klient HTTP (gniazda, HTTP/1.0). Brak TLS — stąd
ograniczenie zakresu sieciowego do czystego HTTP (napiprojekt).
