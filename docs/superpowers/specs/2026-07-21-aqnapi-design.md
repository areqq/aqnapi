# aqnapi — zunifikowany klient napisów (spec projektowy)

**Data:** 2026-07-21
**Status:** zaakceptowany
**Cel:** Jednoplikowe narzędzie CLI `aqnapi.py` (Python 3.9+, **wyłącznie biblioteka
standardowa**) będące spójnym, intuicyjnym i wygodnym klientem trzech serwisów
napisowych: **Napisy24.pl**, **napiprojekt.pl** i **OpenSubtitles.com**.
Obsługuje wszystkie realne możliwości pobierania i wysyłania napisów, wraz z
pełną dokumentacją każdego serwisu.

Źródła protokołów: `/home/q/napisy24/API.md`, `/home/q/napiproject/docs/API-napiprojekt.md`,
research REST API OpenSubtitles (2025/2026) oraz analiza FPS z DMnapi.

---

## 1. Ograniczenia i decyzje

- **Tylko stdlib.** Brak zależności pip. Dozwolone: `urllib`, `http.client`,
  `socket`, `ssl`, `json`, `xml.etree`, `hashlib`, `hmac`, `struct`, `lzma`,
  `zipfile`, `gzip`, `base64`, `argparse`, `configparser`, `subprocess`, `os`,
  `re`, `getpass`, `logging`, `dataclasses`, `unittest`.
- **napiprojekt upload wymaga 7z-AES.** Stdlib nie ma AES ani zapisu 7z →
  implementujemy **własny AES-256 + minimalny zapis kontenera 7z** (store, bez
  kompresji). Poprawność weryfikowana w testach przez systemowe `7z`.
- **Pobrane napisy zawsze jako SRT, UTF-8 z BOM, końce wiersza LF.**
- **CLI:** polecenia agregujące (wiele serwisów naraz) + pełne podpolecenia
  per-serwis.
- **OpenSubtitles upload:** tylko dokumentacja (REST nie ma uploadu; istnieje
  wyłącznie legacy XML-RPC — poza zakresem kodu).

## 2. Macierz możliwości

| Operacja | napisy24 (n24) | napiprojekt (np) | opensubtitles (os) |
|---|---|---|---|
| pobieranie po haszu pliku | CheckSub2 / CheckSubAgent | mode=1 | moviehash |
| szukanie po IMDB/tytule/query | webapi.php | katalog (MovieId) | GET /subtitles |
| pobieranie po ID napisu | download.php?napisId | — | POST /download (quota) |
| logowanie / konto | CheckLogin | api_user_account | POST /login (JWT) |
| upload napisów | formularz WWW /dodaj-napisy | mode=512/1024 (7z-AES) | ✗ (tylko docs) |
| powiązanie pliku z filmem | (ChangeData — poza zakresem) | associate2 | n/d |
| usuwanie własnych napisów | ?usun= (best-effort) | n/d (tor webowy) | n/d |

## 3. Architektura (jeden plik, sekcje)

```
aqnapi.py
├─ Nagłówek / stałe / logging
├─ Błędy: AqError, AuthError, NetworkError, NotFoundError, ServerError, ConfigError
├─ Warstwa wspólna
│   ├─ HTTP: http_get/http_post_json (urllib) + raw_http_post (socket, HTTP/1.0 dla n24)
│   ├─ Hashe: oshash() [OSH], md5_10mb() [napiprojekt], file_md5(), bytes_md5()
│   ├─ FPS z pliku: fps_from_file() → MKV(EBML) / AVI(RIFF) / MP4-MOV(ISO BMFF)
│   ├─ Silnik napisów: detect_encoding, detect_format, parse (SRT/MicroDVD/MPL2/TMPlayer),
│   │                  to_srt(), emit_srt_utf8_bom_lf(), normalize_for_napisy24 (CRLF, ≤2 linie, fix-timing)
│   ├─ Krypto: n24_obf/deobf (XOR+reverse+hex), np_encode_password (XOR3+b64)
│   └─ Krypto 7z: aes256 (blok+CBC), sevenzip_kdf, write_7z_aes()  [+ verify przez `7z`]
├─ Konfiguracja: Config (flagi > env > ~/.config/aqnapi/config.ini), cache JWT OS
├─ Klienci: Napisy24Client, NapiprojektClient, OpenSubtitlesClient
└─ CLI: argparse — agregujące + per-serwis; main()
```

Zasada izolacji: każdy klient hermetyzuje jeden serwis (endpointy, kodowanie
pól, parsowanie odpowiedzi) i zwraca wspólne struktury (`SubtitleHit`,
`SubtitleData`, `Account`, `UploadResult`). Warstwa CLI nie zna szczegółów
protokołów — operuje na klientach i wspólnych strukturach.

## 4. Kluczowe algorytmy (zweryfikowane)

### 4.1 Hasze
- **OSH (OpenSubtitles/napisy24 `fh`):** `filesize + Σ(<Q, LE) z pierwszych i
  ostatnich 64 KiB`, mod 2^64; min. 128 KiB; **16 hex** (n24: UPPER; OS: lower).
- **napiprojekt `md`:** `MD5(pierwsze 10 485 760 B)`, 32 hex lower.
- **SubtitlesHash (np upload):** MD5 całego pliku napisów.

### 4.2 napisy24 — zaciemnianie (obf)
`enc[i] = plain[i] XOR ((0x7F + (i+1)^2) & 0xFF)`, następnie `reverse(enc)` →
UPPER hex. Pola obf w CheckSub2: `fh, md, fs, fn, nl, login, pass`.
Alternatywy bez obf: `CheckSubAgent.php` (konto agenta `dmnapi:4lumen28`),
`libs/webapi.php` (search), `run/pages/download.php` (po ID). Upload przez
formularz WWW `/dodaj-napisy` (Joomla+CB+RSForm), plik **CRLF**, ≤2 linie/blok,
bez nachodzących czasów.

### 4.3 napiprojekt
- Endpoint główny: `POST http://www.napiprojekt.pl/api/api-napiprojekt3.php`.
  Nie uwierzytelnia (upload zawsze anonim). mode=1 pobieranie
  (`downloaded_subtitles_id`=hash, `_lang`, `_txt=1` → base64 w `<content>`),
  mode=512/1024 upload (pole pliku **`subtitles`** małą literą, archiwum
  `<hash>.zip` z wpisem `<hash>.txt` 7z-AES hasło `iBlm8NTigvru0Jr0`).
- `api-movie-search.php` (GET, `/api/`, omija Cloudflare) → `MovieId` (SHA1) +
  linki IMDB/Filmweb/FDB.
- `api-movie-associate2.php` (**GET, hasło jawne** `pass`, `id_pliku`, `id_filmu`).
- `api_user_account.php` (GET, hasło jawne) → info o koncie.
- `api.php?mode=file_info&client=dreambox&id=<hash>` → `<fps>`.

### 4.4 OpenSubtitles (REST `api.opensubtitles.com/api/v1`)
- Nagłówki: `Api-Key` (zawsze), `User-Agent: aqnapi vX.Y` (wymagany!),
  `Authorization: Bearer <JWT>` dla `/download` i `/logout`.
- `POST /login` {username,password} → `token`, `base_url` (użyj go dalej; VIP →
  `vip-api...`), `user.allowed_downloads`. JWT ~24h — czytamy `exp` z tokena.
- `GET /subtitles` — filtry: query, imdb_id (bez `tt`/zer), tmdb_id, moviehash,
  languages (posort., małe litery), type, season_number, episode_number, year…
  → `data[].attributes.files[].file_id` do pobrania.
- `POST /download` {file_id, [sub_format, in_fps, out_fps, timeshift]} → `link`
  (ważny 3h, zawsze UTF-8) + `remaining`/`reset_time`. Quota naliczana przy tym
  wywołaniu. Anonim 5/24h/IP, free 20/24h, VIP do 1000.
- Params GET: alfabetycznie, małe litery, `+` zamiast spacji (mniej redirectów).
- Limity: 5 req/s (429). Brak endpointu upload w REST.

### 4.5 FPS z pliku (dla MicroDVD→SRT)
- **MKV** (magic `1A45DFA3`): walk EBML, w `TrackEntry` gdy `TrackType(0x83)==1`
  weź `DefaultDuration(0x23E383)` (4B, `>I`, ns) → `fps = 1e9/ns`. VINT jak w
  DMnapi. Zabezpieczyć dzielenie przez 0.
- **AVI** (magic `RIFF`): offset 32 `<I` = µs/frame → `fps = 1e6/µs`.
- **MP4/MOV** (`ftyp` w bajtach 4–8): ISO BMFF, `moov→trak→mdia`; wybierz trak z
  `hdlr.handler_type=='vide'`; z `mdhd` timescale (v0 off+20 / v1 off+28), z `stts`
  policz `fps = Σsample_count·timescale / Σ(sample_count·sample_delta)`.
  Obsłuż `size==1` (64-bit largesize).
- Bramka zaufania: akceptuj wynik tylko gdy `22 < fps < 32`.
- Łańcuch FPS: **plik** → metadane serwisu / `file_info` np → `--fps` → **23.976**.

### 4.6 7z-AES (własna implementacja)
- **AES-256**: tablice S-box/Rcon, key expansion, `encrypt_block`; tryb **CBC**.
- **KDF 7z**: jeden kontekst SHA-256 zasilany `2^19` razy `(salt + pw_utf16le +
  counter_u64_LE)`, `counter` inkrementowany; `key = digest()`. `saltSize=0`,
  `ivSize=16` (losowy `os.urandom`).
- **Kontener 7z**: sygnatura `37 7A BC AF 27 1C` + wersja `00 04`, StartHeader
  (NextHeaderOffset/Size/CRC), spakowany strumień = CBC(pad(plaintext)),
  nagłówek zakodowany: PackInfo, UnpackInfo(folder: coder `06F10701`
  AES256SHA256, właściwości = firstByte `0x53` + `0x0F` + IV), SubStreamsInfo z
  CRC-32 rozpakowanych danych. Padding do wielokrotności 16 (nadmiar odrzucany
  przez rozmiar rozpakowany).
- **Weryfikacja**: test rozpakowuje archiwum przez `7z x -p...` i porównuje z
  oryginałem (uruchamiany tam, gdzie jest binarka `7z`).

## 5. Silnik napisów (pipeline pobrania)
1. bajty z serwisu; jeśli ZIP (n24) → wybierz największy wpis napisowy.
2. `detect_encoding`: BOM/UTF-8-sig → utf-8 (strict) → cp1250 → iso-8859-2 →
   utf-8(replace).
3. `detect_format`: SRT (`-->`), MicroDVD (`{f}{f}`/`[f][f]`), MPL2 (`[ds][ds]`),
   TMPlayer (`hh:mm:ss[:=]`).
4. konwersja do SRT (MicroDVD wymaga FPS z §4.5).
5. `emit`: **UTF-8 z BOM (`﻿`), końce LF**.
6. zapis (domyślnie `<stem-filmu>.srt` obok filmu, lub `-o`).

Normalizacja **do uploadu napisy24** jest osobna: CRLF, ≤2 linie/blok
(walidacja lokalna), `--fix-timing` docina nachodzenia. Do napiprojekt: plik
tekstowy pakowany do 7z-AES (bez wymogu CRLF).

## 6. CLI

**Agregujące:**
- `aqnapi get FILM [-l pl] [-o OUT] [--service n24,np,os]` — po haszu; próbuje
  serwisów, wybiera najlepszy trafiony (najwięcej pobrań / pierwszy hit), zapis SRT.
- `aqnapi search (--imdb tt.. | --title T | --query Q) [-l pl] [--service ...]`
  — zunifikowana lista wyników (źródło, id, release, pobrania).
- `aqnapi upload --movie FILM --srt SRT [metadane] --service n24,np` — wspólne
  flagi mapowane per-serwis; wysyła do wybranych.
- `aqnapi hash FILM` · `aqnapi fps FILM` · `aqnapi convert IN -o OUT.srt`.

**Per-serwis** (aliasy `n24|napisy24`, `np|napiprojekt`, `os|opensubtitles`):
- `napisy24 {hash,login,download,search,getid,imdb,upload,delete}`
- `napiprojekt {download,search,associate,account,upload,fileinfo}`
- `opensubtitles {login,logout,search,download,formats,languages,guessit}`

Globalne flagi: `-v/--verbose`, `--timeout`, `--config`, `-l/--lang`, `-o/--output`.

## 7. Konfiguracja i dane logowania
Priorytet: **flagi → zmienne środowiskowe → plik**.
`~/.config/aqnapi/config.ini`:
```
[napisy24]      login = ; pass =
[napiprojekt]   user = ; pass =
[opensubtitles] api_key = ; username = ; password =
```
Env: `NAPI24_LOGIN/PASS`, `NAPI_USER/PASS`, `OS_API_KEY/OS_USERNAME/OS_PASSWORD`.
JWT OpenSubtitles cache w `~/.cache/aqnapi/os_token.json` (z odczytem `exp`).
Hasła nigdy nie trafiają do logów (`-v` maskuje).

## 8. Błędy i UX
Wspólna hierarchia wyjątków; czytelne komunikaty PL; kod wyjścia ≠0 przy błędzie;
`-v` loguje żądania/odpowiedzi (bez haseł). Brak trafień → komunikat, nie wyjątek.

## 9. Testy (`unittest`, stdlib)
Jednostkowe (offline): OSH, md5_10mb, obf/deobf n24 (roundtrip), np XOR+b64,
**AES-256 wektory FIPS-197**, round-trip 7z przez binarkę `7z` (skip gdy brak),
parser/emiter SRT, MicroDVD→SRT (znany fps), MPL2/TMPlayer, detekcja kodowania,
wyjście UTF-8+BOM/LF, FPS z syntetycznych nagłówków MKV/AVI/MP4. Testy sieciowe
oznaczone i pomijane domyślnie (`AQNAPI_LIVE=1`).

## 10. Dokumentacja (deliverables)
- `README.md` — przegląd, quickstart, macierz możliwości, konfiguracja, przykłady.
- `docs/napisy24.md`, `docs/napiprojekt.md`, `docs/opensubtitles.md` — pełny
  protokół każdego serwisu (endpointy, pola, hasze, quota, znane błędy).
- `docs/cli.md` — referencja wszystkich poleceń i flag.
- `CLAUDE.md` — najważniejsze informacje dla Claude Code (architektura, pułapki
  protokołów, komendy testów, konwencje kodu).

## 11. Poza zakresem (YAGNI)
Upload/edycja przez XML-RPC OpenSubtitles; tłumaczenia AI OpenSubtitles;
napisy24 ChangeData/rating; GUI; pobieranie całych sezonów jednym poleceniem
(można iterować `search`+`getid`).
