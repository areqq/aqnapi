# aqnapi — wersja uniwersalna (Cosmopolitan APE): decyzja architektoniczna

**Data:** 2026-07-22
**Status:** zaimplementowane i zweryfikowane

## Cel
Jedna binarka na wszystkie platformy (Linux/macOS/Windows/*BSD, x86-64+ARM64),
**100% zgodna** z wersją CPython `aqnapi.py`, rozwijana równolegle.

## Rozważane warianty
1. **Embed APE Python (wybrany).** Actually Portable Python 3.12.3 (cosmo.zip) z
   wbudowanym niezmienionym `aqnapi.py` (zipos `/zip/` + `.args`). Uruchamia ten
   sam kod → zgodność z definicji; jedno źródło prawdy; „rozwój równoległy" = brak
   dodatkowej pracy (przebudowa pakuje aktualny `aqnapi.py`).
2. **Przepisanie w C (`cosmocc`).** Binarka natywna, lepsza wydajność, ale
   **nie** gwarantuje 100% zgodności (dryf zachowań, podwójne utrzymanie,
   reimplementacja AES/7z/HTTP/TLS/curses/parserów). **Odrzucone** — sprzeczne z
   twardym wymogiem 100% zgodności.

Wymóg „100% kompatybilna" przesądza o wariancie 1.

## Ustalenia z weryfikacji (2026-07-22, uruchomione na żywo)
- Prebuilt: `https://cosmo.zip/pub/cosmos/bin/python` = **CPython 3.12.3**, ~39 MB,
  aktywnie utrzymywany.
- Działają: dataclasses/typing/f-stringi, urllib+**ssl (HTTPS z wbudowanymi
  certyfikatami)**, socket, hashlib/hmac, struct, zipfile, gzip, base64, json,
  xml.etree, configparser, subprocess, argparse, getpass, **curses**, **termios**.
- **Niedostępne: `lzma` (`_lzma`) i `ctypes`.** `aqnapi.py` ich **nie używa**
  (7z-AES = „store" + własny AES; brak importu lzma/ctypes) → zero zmian w kodzie.
- Embedding: APE = ZIP; `.args` = `/zip/aqnapi.py` + `...` (przekazuje argv).

## Implementacja
- `cosmo/build.sh` — pobiera i buforuje interpreter (`cosmo/python.ape`), kopiuje
  do `dist/aqnapi.com`, wkłada `aqnapi.py` + `.args`. Wymaga `zip` + `curl`/`wget`.
- `cosmo/README.md` — użycie, uruchamianie per platforma, `assimilate`, certyfikaty.
- `.gitignore` — `dist/`, `cosmo/python.ape`, `__pycache__`.

## Weryfikacja (wykonana)
- `dist/aqnapi.com`: `--version`, `hash`, `convert` (SRT z BOM), lista poleceń,
  **live `napiprojekt search`** — OK.
- **Cały zestaw testów pod interpreterem APE**: `Ran 62 tests … OK`.

## Ograniczenia (do pilnowania)
- Nie wprowadzać zależności od `lzma`/`ctypes` w `aqnapi.py` (złamałoby build APE).
- Prebuilt bywa przebudowywany — dla powtarzalności można zpinować URL/wersję
  (`PYTHON_APE_URL`), ewentualnie własny build przez `ahgamut/superconfigure`.
- Windows: wersjonowana nazwa `.exe` (antywirus); macOS: kwarantanna Gatekeepera.
