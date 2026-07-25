# CLAUDE.md — aqnapi

Notatki dla Claude Code pracującego w tym projekcie. Najważniejsze fakty,
pułapki i konwencje, których nie widać na pierwszy rzut oka.

## ⚠️ ZASADA NACZELNA — dwie wersje muszą iść w parze

Projekt ma **dwie równoległe implementacje tej samej funkcjonalności**:
- **Python:** `aqnapi.py` (referencyjna, kompletna)
- **C:** `c/aqnapi.c` (natywna, kompilowana przez cosmocc do APE)

**Za każdym razem, gdy cokolwiek dodajesz, poprawiasz lub zmieniasz — zrób to w
OBU wersjach i utrzymaj je zsynchronizowane.** Nie zostawiaj jednej w tyle.

Obowiązkowy tryb pracy przy KAŻDEJ zmianie zachowania:
1. Zmień `aqnapi.py` (+ testy w `tests/test_aqnapi.py`).
2. Wprowadź **tę samą** zmianę w `c/aqnapi.c` (to samo zachowanie, te same
   komunikaty/format wyjścia — cel: **bajtowa zgodność**).
3. Przebuduj obie binarki: `cosmo/build.sh` (Python-APE) oraz `c/build.sh`
   (C-APE), i uruchom testy: `python3 -m unittest discover -s tests`.
4. **Zweryfikuj parność** na tym samym wejściu, np.:
   `diff <(python3 aqnapi.py convert in -o /tmp/p) <(./dist/aqnapi-c.com convert in -o /tmp/c); cmp /tmp/p /tmp/c`.
5. Zaktualizuj dokumentację obu (`README.md`, `docs/`, `c/README.md`) i tabelę
   pokrycia w `c/README.md`.

Jeśli funkcji **nie da się** jeszcze zrobić w C (np. wymaga TLS, którego brakuje
w danym torze budowania), **wyraźnie odnotuj to** w `c/README.md` w sekcji „co
pozostaje" — nie milcz o rozjeździe.

Poświadczenia (w tym **klucz API OpenSubtitles**) są w
`~/.config/aqnapi/config.ini` — używaj ich do weryfikacji end-to-end obu wersji.

## Czym to jest

Jednoplikowy (`aqnapi.py`) klient CLI + moduł do trzech serwisów napisowych:
Napisy24.pl, napiprojekt.pl, OpenSubtitles.com. **Python 3.9+, wyłącznie
biblioteka standardowa** — zero zależności pip, żadnych binarek zewnętrznych w
ścieżce działania.

## URL http(s) jako ścieżka wejściowa

Każdy argument będący **plikiem wejściowym** (`--movie`, `--srt`, `input`,
pozycyjne `file`, `reference`/`target`, listy do `merge`) może być URL-em
`http(s)://` z opcjonalnym `user:pass@` (Basic auth, jak curl -u). Wykrywa to
`is_url()` na poziomie odczytu — **nie** ma podmiany argumentów ani plików
tymczasowych.

Zasada: **pobieraj tylko potrzebne fragmenty (HTTP Range), minimalna pamięć** —
film 1.7 GB nigdy nie leci w całości:
- `md5_10mb` (hash napiprojekt) → tylko pierwsze 10 MiB (`Range: bytes=0-...`).
- `oshash` (OSH) → rozmiar (HEAD/Content-Range) + dwa zakresy po 64 KiB.
- `fps_from_file` → prefiks `FPS_URL_PREFIX` (8 MiB); MP4 z moov na końcu → brak
  FPS z URL-a (fallback). Python parsuje z `io.BytesIO`, C przez `fmemopen`.
- napisy (`read_input_bytes` / C `read_input`) → całość (to kilka KB).

**Pułapka Range:** `url_read_range` dla `start>0` **musi** dostać `206`; przy
`200` (serwer zignorował Range) rzuca błąd — inaczej „ogon" OSH byłby po cichu
zły. `url_size` używa HEAD (Python czyta tylko nagłówki), by nie ściągać ciała.
Nazwa wyjścia pochodzi z nazwy pliku w URL-u (`_input_stem` / C `input_stem`),
nie ze ścieżki serwera. **C:** https działa tylko w wariancie TLS
(`aqnapi-c-tls.com`); http w obu. Regresje: `TestUrlInput` (lokalny serwer Range,
offline).

## Twarde ograniczenia (nie łam)

- **Tylko stdlib.** Nie dodawaj `requests`, `py7zr`, `lxml` itd. Dozwolone
  moduły: `urllib`, `socket`, `ssl`, `json`, `xml.etree`, `hashlib`, `struct`,
  `lzma`, `zipfile`, `gzip`, `base64`, `argparse`, `configparser`, `subprocess`,
  `zlib`, `unittest`.
- **Pobrane napisy zawsze:** SRT, UTF-8 **z BOM** (`aqnapi.UTF8_BOM`), końce
  **LF**. To robi `emit_srt()` / `convert_to_srt_bytes()`. Nie zmieniaj tego bez
  wyraźnej prośby.
- Komunikaty użytkownika po polsku.
- Hasła nigdy w logach (maskowane w `_log_request`; `-v` włącza DEBUG).

## Architektura pliku (sekcje w kolejności)

1. Błędy (`AqError` i podklasy).
2. Hasze: `oshash` (OSH/`fh`), `md5_10mb` (napiprojekt/`md`), `file_md5`,
   `bytes_md5`.
3. FPS z pliku: `fps_from_file` → `_fps_mkv` (EBML), `_fps_avi` (RIFF),
   `_fps_mp4` (ISO BMFF). Bramka zaufania `trusted_fps` (22 < fps < 32).
4. Krypto serwisów: `n24_obf`/`n24_deobf`, `np_encode_password`.
5. AES-256 + 7z: `AES`, `aes_cbc_encrypt`, `sevenzip_key`, `write_7z_aes`.
6. Silnik napisów: `detect_encoding`, `detect_format`, parsery
   (SRT/MicroDVD/MPL2/TMPlayer/WebVTT/ASS-SSA), `to_srt`, `emit_srt`, `convert_to_srt_bytes`,
   emittery `cues_to_vtt/ass/microdvd` + `emit_subtitle` (eksport), `strip_sdh_line` (SDH/HI),
   `normalize_for_napisy24`, `check_srt_for_napisy24`.
7. HTTP: `http_get/http_post_json/http_post_multipart`, `build_multipart`,
   `raw_http10_post` (surowy socket dla Napisy24), `HttpResponse`.
8. Konfiguracja: `Config` (flagi > env > ini), cache JWT OS.
9. Klienci: `Napisy24Client`, `NapiprojektClient`, `OpenSubtitlesClient`.
10. API wysokiego poziomu: `download_subtitles`, `search_subtitles`,
    `convert_file` (+ `__all__`). Synchronizacja: `compute_sync_transform`
    (regresja liniowa z par kotwic), `apply_sync`; UI w `_sync_tui`/`_sync_loop`
    (curses — importowany leniwie w środku funkcji, tryb nieinteraktywny przez
    `--offset`/`--anchor`).
11. CLI: `build_parser`, `cmd_*`, `main`. Polecenia: get/search(+`--pick`/`--auto`)/
    upload/hash/fps/convert/fpsconv/merge/split/sync/config + per-serwis.
    `config init/show/path` (hasła bez echo przez getpass, zapis chmod 600).

Klienci zwracają wspólne dataclasy `SubtitleHit` / `UploadResult`. CLI nie zna
szczegółów protokołów.

## Pułapki protokołów (łatwo się potknąć)

- **Napisy24 obf:** pola CheckSub2 (`fh, md, fs, fn, nl, login, pass`) są
  zaciemniane: `XOR (0x7F+(i+1)^2)`, reverse bajtów, UPPER hex. `fh` UPPER.
  Alternatywa bez obf: `CheckSubAgent.php` (konto agenta `dmnapi:4lumen28`) —
  używane przez `download`/`get`, bo nie wymaga logowania.
- **Napisy24 attach (`AddSubPrg.php`)** — DZIAŁAJĄCA ścieżka API klienta
  („Dodaj napisy (tylko do programu)"). Dwufazowo przez **plain HTTP**
  (`/run/AddSubPrg.php`, port 80 → działa też w bazowym buildzie C): faza
  **Check** (read-only) → `OK-0` nowe / `OK-1` film znany / `OK-2` duplikat;
  faza **Send** → `OK`. Pola `login,pass,hm,md,hs,fs,tm,dm,fp,im` **zaciemniane**
  (`n24_obf`, jak CheckSub2), `fn` jawne (utf-8), `sf` = surowy plik. `hm` = OSH
  **UPPER**, `hs` = `subtitle_hash` (rozmiar + suma pełnych słów 8B LE, UPPER),
  `im` tylko gdy IMDB niepuste≠0. Multipart z `Content-Transfer-Encoding: 8bit`.
  **postVer=v1.99.1** (endpoint NIE jest wersjonowany — inaczej niż
  `AddSub.php`: „Masz Starą wersję programu!"). Wynik: napisy powiązane **po
  haszu** (znajduje je `download`), **bez publicznego wpisu** → `--check-only`
  (sama faza Check) wystarcza do bezpiecznej weryfikacji. CLI:
  `napisy24 attach --movie … --srt … [--imdb tt..] [--check-only]`.
- **Napisy24 upload (WWW, wpis publiczny)** idzie przez formularz `/dodaj-napisy` (Joomla+CB+
  RSForm), NIE przez `AddSub.php` (zablokowany serwerowo). Plik musi być **CRLF**,
  ≤2 linie/blok, bez nachodzących czasów — pilnuje tego `check_srt_for_napisy24`
  (walidacja lokalna; NIE używać `ajaxValidate`, bo zapisuje wpis).
- **Napisy24 HTTP:** klient używa surowego HTTP/1.0 przez socket
  (`raw_http10_post`), a webapi/upload/delete idą przez `urllib` (HTTPS).
- **napiprojekt pole pliku uploadu = `subtitles`** (małą literą!). Wielka litera
  → „Brak pliku z napisami".
- **napiprojekt `client` MUSI być z whitelisty** serwera. Nieznana nazwa (np.
  `aqnapi`) → upload zwraca `<error><client> odmowa dostępu</error>`. Używamy
  **`pynapi`** (`CLIENT`/`client` w download i upload). Nazwy per-tryb:
  `dreambox` dla `file_info`, `allplayer` dla `search` — nie mieszać.
- **napiprojekt upload z logowaniem DZIAŁA** (flaga `--login`): pola
  **`user_nick`** + **`user_password`** (MAŁĄ literą; hasło `np_encode_password`
  = XOR key=3 → base64) w POST do `api-napiprojekt3.php`. Pusty `SubtitlesAutor`
  → login. Uwierzytelnione uploady idą do kolejki moderacji (rosną `dodane`).
  Bez `--login` upload jest anonimowy. Endpoint konta `api_user_account.php`
  jest **martwy (404)** — nie polegać na `verify_account`; auth idzie wyłącznie
  przez pola POST uploadu. Associate działa hasłem **jawnym** (`pass`) na
  `api-movie-associate2.php` (metoda **GET**).
- **napiprojekt upload = archiwum 7z-AES** o nazwie `<hash>.zip` z wpisem
  `<hash>.txt`, hasło `iBlm8NTigvru0Jr0`. Budujemy je w czystym Pythonie
  (`write_7z_aes`): AES-256-CBC + KDF 7z (SHA-256, 2^19 rund, hasło UTF-16LE) +
  ręczny kontener 7z (store). `saltSize=0`, `ivSize=16`, props firstByte `0x53`,
  drugi bajt `0x0F`. Poprawność sprawdza test przez binarkę `7z`.
- **OpenSubtitles:** `Api-Key` zawsze; `Authorization: Bearer` tylko dla
  `/download` i `/logout`; `User-Agent` WYMAGANY (`aqnapi vX.Y`). Po `/login`
  używaj zwróconego `base_url` (VIP → inny host). Quota naliczana przy
  `/download` (nie przy pobraniu linku). Parametry GET: posortowane, małe litery.
  **Brak uploadu w REST** — świadomie niezaimplementowany.
- **FPS:** łańcuch źródeł w `_resolve_fps`: plik filmowy → metadane serwisu /
  `file_info` napiprojekt → flaga `--fps` → 23.976. Odczyt z pliku tylko gdy
  przejdzie bramkę `22 < fps < 32`.

## Testy i weryfikacja

```bash
python3 -m unittest discover -s tests -v          # cały zestaw (offline)
python3 -m unittest tests.test_aqnapi.Test7z -v   # 7z-AES (weryfikacja binarką 7z)
python3 -c "import ast; ast.parse(open('aqnapi.py').read())"  # składnia 3.9
python3 aqnapi.py --help                            # smoke CLI
```

Testy używają syntetycznych plików (nagłówki MKV/AVI/MP4 budowane ręcznie,
wektor AES-256 z FIPS-197). Nie dodawaj testów sieciowych do domyślnego biegu.

## Wersja uniwersalna (Cosmopolitan APE)

`cosmo/build.sh` produkuje `dist/aqnapi.com` — jeden APE (Linux/macOS/Windows/
*BSD, x86-64+ARM64) = Actually Portable Python 3.12.3 z cosmo.zip + **niezmieniony
`aqnapi.py`** wbudowany w zipos (`/zip/aqnapi.py` + plik `.args`). To gwarantuje
100% zgodności — `aqnapi.py` jest jedynym źródłem prawdy; wersja APE to tylko
opakowanie. **Nie przepisuj w C** (dryf zachowań łamie wymóg 100% zgodności).

Niezależny **natywny port w C**: `c/aqnapi.c` + `c/build.sh` (cosmocc → `dist/
aqnapi-c.com`). To już NIE „POC z 4 poleceń" — pokrywa **większość poleceń**
i jest **100% bajtowo zgodny** z Pythonem dla zaimplementowanego podzbioru:
offline `hash`/`fps`/`convert`/`fpsconv`/`merge`/`split`/`sync` (nieinteraktywny);
sieć po HTTP `download`/`get`, `napiprojekt` (download/fileinfo/upload 7z-AES/
`--login`), `napisy24` (download/getid/search/attach AddSubPrg); wejście z **URL**
(Range). Wariant **TLS** (`c/build-tls.sh` → `dist/aqnapi-c-tls.com`, monorepo
Cosmopolitan + mbedtls) dodaje HTTPS: `opensubtitles`, `napisy24 weblogin/upload/
delete`, `update`, URL `https://`. C ma własne MD5/OSH/SHA-256/AES-256/7z/base64/
HTTP(S)/silnik napisów. **Przy KAŻDEJ zmianie w `aqnapi.py` pilnuj parności C**
(`diff` wyjść, testy live). Świadomie poza portem: interaktywny TUI `sync`
(curses/termios ANSI — jest, ale nieinteraktywny tor) i `config`.

Krytyczne dla zgodności z APE: nie wprowadzaj zależności od **`lzma`** ani
**`ctypes`** — te moduły nie są wkompilowane w APE Pythona (reszta stdlib, w tym
`ssl`/HTTPS z wbudowanymi certyfikatami, `curses`, `termios`, działa). 7z-AES
jest „store" (bez kompresji), więc lzma nie jest potrzebne. Weryfikacja parności:
`cosmo/python.ape -m unittest discover -s tests` (musi dać te same 62 OK).

## Konwencje

- Kod i komentarze zwięzłe, spójne z istniejącym stylem. Docstringi po polsku.
- Nowe operacje: dodaj metodę do właściwego klienta (zwróć wspólną dataclasę),
  potem podłącz w CLI (per-serwis) i ewentualnie w agregacji.
- Przy zmianach w krypto/7z/hashach/FPS/SRT **zawsze** uruchom testy — to
  najłatwiejsze do zepsucia, a błąd jest cichy (serwer odrzuci archiwum,
  konwersja da złe czasy).
- Referencje protokołów: `docs/napisy24.md`, `docs/napiprojekt.md`,
  `docs/opensubtitles.md`; spec projektu: `docs/superpowers/specs/`.
