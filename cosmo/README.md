# aqnapi — wersja uniwersalna (Cosmopolitan APE)

Jedna binarka `aqnapi.com` działająca **na wszystkich platformach** (Linux,
macOS, Windows, FreeBSD/OpenBSD/NetBSD; x86-64 **i** ARM64) — format
[Actually Portable Executable](https://justine.lol/ape.html) z projektu
[Cosmopolitan](https://github.com/jart/cosmopolitan).

## Dlaczego tak (100% zgodność)

Binarka to **Actually Portable Python 3.12.3** (z [cosmo.zip](https://cosmo.zip/))
z wbudowanym, **niezmienionym `aqnapi.py`**. Uruchamia dokładnie ten sam kod co
wersja CPython, więc jest w 100% zgodna z definicji — `aqnapi.py` pozostaje
jedynym źródłem prawdy, a „rozwój równoległy" sprowadza się do przebudowy.

> Przepisywanie w C (przez `cosmocc`) dałoby binarkę natywną, ale **nie**
> gwarantowałoby 100% zgodności (ciągły dryf zachowań, podwójne utrzymanie),
> dlatego świadomie wybrano wariant z wbudowanym Pythonem.

`aqnapi.py` używa wyłącznie modułów obecnych w APE Pythonie (urllib+ssl,
socket, hashlib/hmac, struct, zipfile, gzip, base64, json, xml.etree,
configparser, subprocess, argparse, getpass, **curses**, **termios**). Nie
korzysta z `lzma` ani `ctypes` (jedyne moduły niewkompilowane w APE) — 7z-AES
budowany jest bez kompresji, w czystym Pythonie.

## Budowanie

```sh
cosmo/build.sh                 # -> dist/aqnapi.com  (pobiera interpreter APE, ~39 MB)
PYTHON_APE_URL=... cosmo/build.sh   # własny/zpinowany URL interpretera
```

Wymaga `zip` oraz `curl`/`wget`. Interpreter jest buforowany w
`cosmo/python.ape` (kolejne buildy nie pobierają go ponownie). Mechanizm: APE
jest też archiwum ZIP — `build.sh` wkłada `aqnapi.py` i plik `.args`
(`/zip/aqnapi.py` + `...`), dzięki czemu binarka od razu uruchamia skrypt i
przekazuje argumenty (`sys.argv`).

## Użycie

Identyczne jak wersja Pythona — te same polecenia i flagi:

```sh
./dist/aqnapi.com --help
./dist/aqnapi.com get film.mkv -l pl
./dist/aqnapi.com sync wzor.srt do_sync.srt          # UI curses działa
./dist/aqnapi.com config init
```

## Uruchamianie per platforma

- **Linux/*BSD/macOS:** `./aqnapi.com`. Na macOS przy pobraniu z sieci zdejmij
  kwarantannę: `xattr -d com.apple.quarantine aqnapi.com` (lub prawy-klik →
  Otwórz). Apple Silicon (ARM64) działa z tego samego pliku.
- **Windows:** zmień nazwę na `aqnapi.exe` i uruchom. Zalecana **wersjonowana
  nazwa** (np. `aqnapi-1.0.exe`), by uniknąć fałszywych alarmów antywirusa.
- **Certyfikaty HTTPS** są wbudowane w binarkę (`/zip/share/ssl/`), więc
  OpenSubtitles (HTTPS) działa bez konfiguracji systemowej. Nadpisanie:
  `SSL_CERT_FILE`.
- **`assimilate`** (z `cosmocc`) zamienia „gruby" APE na natywny ELF/Mach-O/PE
  dla bieżącej platformy — opcjonalne, przydatne, gdy binarkę ma uruchamiać
  bezpośrednio inny proces przez `execv` (bez powłoki).

## Weryfikacja zgodności

Cały zestaw testów przechodzi pod interpreterem APE (Python 3.12.3):

```sh
cosmo/python.ape -m unittest discover -s tests    # Ran 62 tests ... OK
```

## Rozmiar / start

~40 MB (pełny CPython + stdlib + OpenSSL + certyfikaty). Start ~10–40 ms,
~19 MB RAM — pomijalny narzut dla CLI/TUI.
