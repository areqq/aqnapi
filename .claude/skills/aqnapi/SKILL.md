---
name: aqnapi
description: Use when downloading, searching, uploading, converting, syncing, or FPS/format-fixing subtitles for movies/series via the aqnapi CLI (Napisy24.pl, napiprojekt.pl, OpenSubtitles.com) — including credential setup, URL/Range input, and the napisy24 public-upload conventions.
---

# aqnapi — klient napisów (Napisy24 / napiprojekt / OpenSubtitles)

Jednoplikowy klient CLI (`aqnapi.py`, tylko stdlib Pythona 3.9+) do pobierania,
wyszukiwania, wysyłania, konwersji i synchronizacji napisów. Ten sam kod jest
dostępny jako binarka: `aqnapi.com` (APE), `aqnapi-c.com` (natywna C, TLS) oraz
pakiety OpenWrt. Wywołania w tym skillu pokazują `python3 aqnapi.py …`; z binarką
zamień na `./aqnapi …` — składnia identyczna.

## Zanim zaczniesz

1. **Konfiguracja poświadczeń** (potrzebna do uploadu i OpenSubtitles):
   ```bash
   python3 aqnapi.py config init      # interaktywnie; zapis do ~/.config/aqnapi/config.ini (chmod 600)
   python3 aqnapi.py config show       # podgląd (hasła zamaskowane)
   python3 aqnapi.py config path       # ścieżka pliku
   ```
   Kolejność źródeł: flagi CLI > zmienne środowiskowe > `config.ini`. Klucz API
   OpenSubtitles i loginy trzymaj w `config.ini`; **nigdy nie wpisuj haseł do
   logów ani komend commitowanych** — `-v` loguje żądania z maskowaniem haseł.

2. **Wejście może być URL-em.** Każdy argument-plik (`--movie`, `--srt`,
   `input`, `file`, `reference`/`target`, listy `merge`) może być
   `http(s)://[user:pass@]host/…` (Basic auth jak `curl -u`). Pobierane są
   **tylko potrzebne fragmenty** (HTTP Range) — hasze i FPS nie ściągają całego
   filmu. `https` w binarce C wymaga wariantu TLS (`aqnapi-c-tls.com`).

## Najczęstsze zadania

### Pobrać napisy do pliku filmowego (po haszu)
```bash
python3 aqnapi.py get film.mkv                     # PL, wiele serwisów
python3 aqnapi.py get film.mkv -l en --service os  # angielskie z OpenSubtitles
python3 aqnapi.py get *.mkv                         # partiami (bez -o)
```
`--service` przyjmuje `np,n24,os` (aliasy: napiprojekt/napisy24/opensubtitles).
`--force` nadpisuje istniejące. Napisy zapisywane **zawsze jako SRT, UTF-8+BOM,
LF**.

### Wyszukać i wybrać
```bash
python3 aqnapi.py search --title "Fauda" --season 5 --episode 2 --pick
python3 aqnapi.py search --imdb tt0111161 --auto --movie film.mkv   # ranking pod plik
```
`--pick` = wybór interaktywny, `--auto` = najlepszy wynik bez pytania
(ranking wg `--movie`/liczby pobrań).

### Konwersja / FPS / łączenie / dzielenie
```bash
python3 aqnapi.py convert napisy.txt -o out.srt          # dowolny format → SRT
python3 aqnapi.py convert in.srt --format vtt -o out.vtt # eksport SRT→VTT/ASS/microdvd
python3 aqnapi.py fps film.mkv                           # FPS + czas trwania z kontenera
python3 aqnapi.py fpsconv in.srt --from 25 --to 23.976 -o out.srt
python3 aqnapi.py fpsconv in.srt --movie film.mkv        # docelowy FPS z filmu
python3 aqnapi.py merge cd1.srt cd2.srt -o full.srt      # offset auto = koniec poprzedniego
python3 aqnapi.py split in.srt --at 00:45:00 -o part     # punkt podziału powtarzalny
```
Formaty wejścia: SRT / MicroDVD / MPL2 / TMPlayer / WebVTT / ASS·SSA. MicroDVD
wymaga FPS — podaj `--fps` lub `--movie`.

### Synchronizacja
```bash
python3 aqnapi.py sync wzor.srt cel.srt --offset -2.5           # proste przesunięcie
python3 aqnapi.py sync wzor.srt cel.srt --anchor 3,5 --anchor 40,44  # regresja z kotwic
python3 aqnapi.py sync wzor.srt cel.srt                         # interaktywne 2-kolumnowe TUI
```
Bez `--offset`/`--anchor` uruchamia się TUI (termios/ANSI) — nie wywołuj go
w trybie nieinteraktywnym; wtedy zawsze podaj `--offset` lub `--anchor`.

### Hasze i aktualizacja
```bash
python3 aqnapi.py hash film.mkv     # OSH + MD5-10MiB + rozmiar
python3 aqnapi.py update --check    # sprawdź nowsze wydanie
python3 aqnapi.py update            # pobierz i podmień binarkę
```

## Upload napisów

`upload` (agregujący) albo per-serwis (`napiprojekt upload`, `napisy24 upload`).
**Zawsze najpierw `--dry-run`** — waliduje plik lokalnie bez wysyłania.

```bash
# napiprojekt (przypisany do konta) + napisy24 (wpis publiczny) naraz:
python3 aqnapi.py upload --srt napisy.srt --movie film.mkv \
    --service np,n24 --login --translator "TwójNick" --dry-run
```

- **napiprojekt**: `--login` = upload przypisany do konta (kolejka moderacji);
  bez tego anonimowo. Wymaga `--movie` (hash filmu). Pole autora = `--translator`.
- **napisy24 (WWW, wpis publiczny)**: plik musi być poprawny (≤2 linie/blok, bez
  nakładających się czasów, CRLF — pilnuje tego walidacja; `--fix-timing`
  przycina nakładki). `attach` (AddSubPrg) to **inna ścieżka** — powiązanie po
  haszu, **bez** publicznego wpisu w katalogu.
- **OpenSubtitles**: brak uploadu w REST API — świadomie niedostępny.

### Konwencje pól napisy24 (ważne — łatwo pomylić)
- **`--release`**: podaj TYLKO właściwą część wydania. Pomijamy tytuł, rok, a
  dla serialu `SxxExx`; **nie** podajemy nazw trackerów/stron (`[eztv]`,
  `{SPARROW}`, `[Site.tv]`). Kilka wydań rozdziel średnikiem. Gdy pominiesz flagę
  albo podasz `--movie`, narzędzie samo wyciąga release z nazwy pliku
  (`n24_release`). Przykłady: `Oblivion.2013.720p.BDRip.X264-SPARKS` → `720p.BDRip.X264-SPARKS`.
- **`--translator`**: nick użytkownika (np. „Qladiusz”). **Nie** przepisuj nazwisk
  z TREŚCI napisów — linie „Napisy: <ktoś>” to kredyt autora w pliku, a nie pole
  „tłumacz”.
- **`--duration` / `--fps` / `--size`**: gdy pominięte, brane są z kontenera
  `--movie` (czas i fps przez parser MKV/MP4/AVI, rozmiar = długość pliku). Nie
  wyliczaj czasu z ostatniej kwestii `.srt` — kończy się przed końcem odcinka.
- Serial: `--season`, `--episode`, `--episode-title` przełączają typ na „Serial”.

## Polecenia per-serwis (zaawansowane)
- `napisy24`: `download`, `search`, `getid`, `imdb`, `attach`, `edit`
  (edycja własnego wpisu WWW), `mediainfo`, `notify`, `trans`, `premieres`,
  `delete/rm`, `login`, `hash`.
- `napiprojekt`: `download`, `search`, `associate`, `fileinfo`, `upload`,
  `cover` (okładka+ocena), `version`, `report`. (`info`/mediainfo świadomie
  pominięte — wymagałoby zewnętrznego binarium.)
- `opensubtitles`: `login`, `logout`, `search`, `download`, `formats`,
  `languages`, `guessit`.

## Pułapki
- Napisy zawsze wychodzą jako **SRT UTF-8 z BOM, końce LF** — to celowe, nie
  „naprawiaj” tego.
- Sanityzacja jest domyślnie włączona (usuwanie tagów, docinanie ekstremalnych
  i nakładających się czasów, naprawa odwróconych). Wyłącz świadomie:
  `--keep-tags`, `--no-sanitize`, `--max-display`, `--min-display`; SDH/HI:
  `--strip-sdh`.
- Upload jest nieodwracalny i publiczny (napisy24 WWW) lub trafia do moderacji
  (napiprojekt `--login`) — **potwierdź z użytkownikiem i użyj `--dry-run`**
  przed realnym wysłaniem.
- Komunikaty narzędzia są po polsku; kody wyjścia ≠0 oznaczają błąd.

## Pomoc
`python3 aqnapi.py <polecenie> --help`. Pełna referencja: `docs/cli.md`;
protokoły: `docs/napisy24.md`, `docs/napiprojekt.md`, `docs/opensubtitles.md`.
