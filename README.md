# aqnapi

[![CI](https://github.com/areqq/aqnapi/actions/workflows/ci.yml/badge.svg)](https://github.com/areqq/aqnapi/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/areqq/aqnapi?sort=semver)](https://github.com/areqq/aqnapi/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Zunifikowany klient wiersza poleceń do trzech serwisów napisowych —
**Napisy24.pl**, **napiprojekt.pl** i **OpenSubtitles.com** — w jednym pliku
`aqnapi.py`. Pobieranie i wysyłanie napisów ze spójnym, wygodnym interfejsem.

* **Tylko biblioteka standardowa** Pythona 3.9+ (zero zależności, żaden `pip`,
  żadna binarka zewnętrzna — nawet archiwum 7z-AES dla napiprojekt budowane jest
  w czystym Pythonie).
* **Pobrane napisy zawsze jako SRT**, w UTF-8 **z BOM**, z końcami wiersza **LF**.
  Formaty MicroDVD / MPL2 / TMPlayer / WebVTT (`.vtt`) / SubStation Alpha
  (`.ass`/`.ssa`) są konwertowane do SRT (FPS do MicroDVD odczytywany wprost z
  pliku filmowego: MKV / AVI / MP4 / MOV). Możliwy też **eksport** SRT→VTT/ASS/
  MicroDVD oraz **przeliczanie FPS** (`fpsconv`).
* **Sanityzacja przed zapisem** — usuwanie tagów formatujących, docinanie
  ekstremalnie długich i nakładających się czasów, naprawa odwróconych czasów,
  usuwanie pustych bloków; uszkodzony/nierozpoznany materiał nie jest zapisywany.
  Flagi: `--keep-tags`, `--max-display`, `--min-display`, `--no-sanitize`.
* Działa jako **narzędzie CLI** i jako **wygodny moduł** Pythona.
* Dostępna też jako **jedna uniwersalna binarka** `aqnapi.com` (Cosmopolitan APE)
  na Linux/macOS/Windows/*BSD (x86-64+ARM64) — ten sam kod, 100% zgodności.
  Zob. [`cosmo/README.md`](cosmo/README.md).

Dokumentacja protokołu każdego serwisu: [`docs/napisy24.md`](docs/napisy24.md),
[`docs/napiprojekt.md`](docs/napiprojekt.md),
[`docs/opensubtitles.md`](docs/opensubtitles.md).
Pełna referencja poleceń: [`docs/cli.md`](docs/cli.md).

## Instalacja

**Gotowa binarka (bez Pythona, wszystkie platformy)** — z [Releases](https://github.com/areqq/aqnapi/releases/latest):

```bash
# Linux / macOS / *BSD
curl -L -o aqnapi https://github.com/areqq/aqnapi/releases/latest/download/aqnapi.com
chmod +x aqnapi && ./aqnapi --help
# Windows: pobierz aqnapi.exe z Releases i uruchom
```

Sumy kontrolne: `SHA256SUMS.txt` w każdym wydaniu.

**Z Pythonem (bez instalacji)** — wystarczy Python 3.9+ (realnie 3.7+):

```bash
python3 aqnapi.py --help
chmod +x aqnapi.py && ./aqnapi.py --help
```

## Szybki start

```bash
# hasze pliku (OSH + MD5-10MiB) i FPS
aqnapi hash film.mkv
aqnapi fps  film.mkv

# pobierz napisy dla pliku (próbuje napiprojekt -> napisy24 -> opensubtitles)
aqnapi get film.mkv -l pl
aqnapi get film.mkv -l pl -o film.srt --service np,n24

# wsad: wiele plików naraz; pomija te, dla których .srt już istnieje
aqnapi get *.mkv -l pl            # pomija już pobrane (bez łączenia się z siecią)
aqnapi get *.mkv -l pl --force    # wymuś ponowne pobranie

# szukaj w wielu serwisach
aqnapi search --title "Matrix"
aqnapi search --imdb tt0133093 --service n24,os -l pl

# wybór wyniku i pobranie: interaktywnie lub auto (nazwa pod plik filmowy)
aqnapi search --imdb tt0133093 --pick
aqnapi search --movie Matrix.1999.1080p.mkv --auto

# przeliczenie FPS i eksport do innego formatu
aqnapi fpsconv napisy.srt --from 25 --to 23.976
aqnapi convert napisy.srt -o napisy.vtt

# konwersja dowolnego formatu do SRT (UTF-8+BOM, LF) — offline
aqnapi convert napisy.txt -o napisy.srt --fps 25

# synchronizacja wg wzorca — interaktywnie w dwóch kolumnach (zaznaczasz pary linii)
aqnapi sync wzor.srt do_sync.srt
aqnapi sync wzor.srt do_sync.srt --offset -2.5           # albo bez UI: proste przesunięcie
```

## Macierz możliwości

| Operacja | napisy24 (`n24`) | napiprojekt (`np`) | opensubtitles (`os`) |
|---|:---:|:---:|:---:|
| pobieranie po haszu pliku | ✅ | ✅ | ✅ |
| szukanie po IMDB / tytule / frazie | ✅ | ✅ (katalog) | ✅ |
| pobieranie po ID napisu | ✅ | — | ✅ |
| logowanie / konto | ✅ | ✅ | ✅ (JWT) |
| **upload napisów** | ✅ (formularz WWW) | ✅ (7z-AES) | ✗ (patrz niżej) |
| powiązanie pliku z filmem | — | ✅ | — |
| usuwanie własnych napisów | ✅ (best-effort) | — | — |

> **Upload do OpenSubtitles** nie jest obsługiwany, bo ich REST API v1 nie ma
> endpointu uploadu (istnieje tylko w przestarzałym XML-RPC). Szczegóły:
> [`docs/opensubtitles.md`](docs/opensubtitles.md).

## Struktura poleceń

Dwie warstwy — **agregująca** (wygoda) i **per-serwis** (pełna kontrola):

```
# agregujące
aqnapi get FILM [-l pl] [-o OUT] [--service n24,np,os]
aqnapi search (--imdb tt.. | --title T | --query Q) [-l pl] [--service ...]
aqnapi upload --movie FILM --srt SRT [metadane] --service n24,np
aqnapi hash FILM
aqnapi fps FILM
aqnapi convert WEJŚCIE -o WYJŚCIE.(srt|vtt|ass|sub) [--movie FILM] [--fps 25] [--strip-sdh]
aqnapi fpsconv WEJŚCIE --from 25 --to 23.976 [-o WYJŚCIE]
aqnapi merge CD1 CD2 [...] -o PEŁNY.srt        # łączenie (np. CD1+CD2)
aqnapi split WEJŚCIE --at 01:02:30 -o BAZA     # podział po czasie
aqnapi sync WZÓR CEL          # interaktywna synchronizacja w 2 kolumnach
aqnapi config init            # interaktywne ustawienie loginów/klucza API

# per-serwis (aliasy: n24, np, os)
aqnapi napisy24    {hash,login,download,search,getid,imdb,upload,delete}
aqnapi napiprojekt {download,search,associate,account,upload,fileinfo}
aqnapi opensubtitles {login,logout,search,download,formats,languages,guessit}
```

## Dane logowania i konfiguracja

Priorytet: **flagi → zmienne środowiskowe → plik konfiguracyjny**.

Najszybciej: **`aqnapi config init`** — interaktywnie wpisuje loginy i klucz API
(hasła bez echo), zapisuje plik z uprawnieniami `600`. Podgląd: `aqnapi config
show` (hasła zamaskowane), ścieżka: `aqnapi config path`.

`~/.config/aqnapi/config.ini`:

```ini
[napisy24]
login = twoj_login
pass  = twoje_haslo

[napiprojekt]
user = twoj_login
pass = twoje_haslo

[opensubtitles]
api_key  = twoj_klucz_api
username = twoj_login
password = twoje_haslo
```

Zmienne środowiskowe: `NAPI24_LOGIN` / `NAPI24_PASS`, `NAPI_USER` / `NAPI_PASS`,
`OS_API_KEY` / `OS_USERNAME` / `OS_PASSWORD`.
Flagi: `--n24-user/--n24-pass`, `--np-user/--np-pass`,
`--os-api-key/--os-user/--os-pass`.

Hasła nigdy nie trafiają do logów (`-v/--verbose` je maskuje). Token JWT
OpenSubtitles jest cache'owany w `~/.cache/aqnapi/os_token.json` (z odczytem
czasu wygaśnięcia).

> **OpenSubtitles** wymaga własnego klucza API — załóż konto na
> opensubtitles.com i w profilu (sekcja *API Consumers*) zarejestruj konsumenta.

## Użycie jako moduł

```python
import aqnapi

# hasze i FPS
print(aqnapi.oshash("film.mkv"), aqnapi.md5_10mb("film.mkv"))
print(aqnapi.fps_from_file("film.mp4"))

# pobierz napisy (zwraca bajty SRT: UTF-8+BOM, LF)
data = aqnapi.download_subtitles("film.mkv", lang="pl", services=("np", "n24"))
open("film.srt", "wb").write(data)

# wyszukiwanie
for hit in aqnapi.search_subtitles(title="Matrix", services=("n24",)):
    print(hit.service, hit.sub_id, hit.title)

# konwersja pliku do SRT
aqnapi.convert_file("napisy.txt", "napisy.srt", movie_path="film.mkv")

# bezpośrednio klienci
np = aqnapi.NapiprojektClient()
raw = np.download(aqnapi.md5_10mb("film.mkv"), "PL")
```

Publiczne API modułu wypisane jest w `aqnapi.__all__`.

## Uniwersalna binarka (Cosmopolitan)

Jeden plik `aqnapi.com` na wszystkie platformy (Linux/macOS/Windows/*BSD,
x86-64+ARM64), z wbudowanym **niezmienionym** `aqnapi.py` w Actually Portable
Python 3.12.3 — 100% zgodności, bo to ten sam kod:

```bash
cosmo/build.sh            # -> dist/aqnapi.com
./dist/aqnapi.com --help
```

Szczegóły, uruchamianie per platforma i weryfikacja: [`cosmo/README.md`](cosmo/README.md).

Istnieje też niezależny **natywny POC w C** (`c/aqnapi.c`, kompilowany przez
`cosmocc`) — podzbiór poleceń (`hash`, `fps`, `convert`, `download`) **bajtowo
zgodny** z wersją Python. Zob. [`c/README.md`](c/README.md).

## Testy

```bash
python3 -m unittest discover -s tests -v          # systemowy CPython
cosmo/python.ape -m unittest discover -s tests    # pod interpreterem APE (po build.sh)
```

Testy są offline (dane syntetyczne). Test poprawności archiwum 7z-AES
weryfikuje wynik binarką `7z`, jeśli jest zainstalowana (w przeciwnym razie jest
pomijany).

## Przykłady — upload

```bash
# napiprojekt: upload po haszu pliku (tryb testowy --dry-run nic nie zapisuje)
aqnapi napiprojekt upload --srt film.srt --movie film.mkv -l PL --translator "ja" --dry-run
aqnapi napiprojekt upload --srt film.srt --movie film.mkv -l PL --translator "ja"

# napiprojekt: poprawki (mode=1024)
aqnapi napiprojekt upload --srt film.srt --movie film.mkv --corrected --comment "lepsza synchronizacja"

# napisy24: upload do filmu przez formularz WWW
aqnapi napisy24 upload --srt film.srt --movie film.mkv --imdb tt1757678 \
    --release XviD-ORPHEUS --translator ja --resolution 1920x1080 --duration 02:10:00

# napisy24: upload do odcinka serialu
aqnapi napisy24 upload --srt s04e01.srt --movie s04e01.mkv --imdb tt8080122 \
    --season 4 --episode 1 --release "...s04e01...web-dl" --translator ja \
    --resolution 1920x1080 --duration 00:48:12
```

## Uwaga prawna

Serwisy nie udostępniają oficjalnych publicznych API do wszystkich tych operacji;
protokoły pochodzą z analizy oryginalnych klientów (interoperacyjność) oraz z
publicznej dokumentacji (OpenSubtitles). Używaj zgodnie z regulaminami serwisów.
