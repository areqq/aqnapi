# aqnapi — referencja poleceń CLI

Wywołanie: `python3 aqnapi.py [OPCJE GLOBALNE] POLECENIE [ARGUMENTY]`
(lub `./aqnapi.py …` po `chmod +x`).

## Opcje globalne

| Opcja | Znaczenie |
|---|---|
| `--version` | wersja narzędzia |
| `-v`, `--verbose` | log żądań HTTP (hasła maskowane) |
| `--timeout SEK` | limit czasu połączeń (domyślnie 30) |
| `--config PLIK` | alternatywny plik konfiguracyjny (domyślnie `~/.config/aqnapi/config.ini`) |

## Dane logowania (wszystkie polecenia sieciowe)

Priorytet **flagi → env → plik ini**:

| Serwis | flagi | env | sekcja ini |
|---|---|---|---|
| napisy24 | `--n24-user` `--n24-pass` | `NAPI24_LOGIN` `NAPI24_PASS` | `[napisy24]` login/pass |
| napiprojekt | `--np-user` `--np-pass` | `NAPI_USER` `NAPI_PASS` | `[napiprojekt]` user/pass |
| opensubtitles | `--os-api-key` `--os-user` `--os-pass` | `OS_API_KEY` `OS_USERNAME` `OS_PASSWORD` | `[opensubtitles]` api_key/username/password |

---

## Polecenia agregujące

### `get FILM [FILM ...]`
Pobiera napisy dla **jednego lub wielu** plików filmowych po haszu, próbując
kolejno serwisów; zapisuje pierwszy trafiony wynik jako SRT (UTF-8+BOM, LF).
Jeśli plik `.srt` już istnieje, **pomija pobieranie** (nie łączy się z siecią) —
chyba że podasz `--force`.

| Opcja | Domyślnie | Znaczenie |
|---|---|---|
| `-l`, `--lang` | `pl` | język napisów |
| `-o`, `--output` | `<film>.srt` | plik wyjściowy (**tylko przy jednym pliku**) |
| `--service` | `np,n24,os` | które serwisy i w jakiej kolejności |
| `--fps` | — | wymuszony FPS do konwersji MicroDVD |
| `--force` | — | nadpisz istniejące napisy zamiast je pomijać |

```bash
aqnapi get film.mkv -l pl
aqnapi get film.mkv -o film.srt --service np,n24
aqnapi get *.mkv                      # wsad: cały katalog, pomija już pobrane
aqnapi get *.mkv --force              # pobierz ponownie wszystkie
```

### `search`
Szuka napisów/filmów w wielu serwisach; wypisuje ujednoliconą listę (serwis, ID,
język, liczba pobrań, tytuł/release).

| Opcja | Znaczenie |
|---|---|
| `--imdb ttNNNNNNN` | szukaj po IMDB |
| `--title "..."` | szukaj po tytule |
| `--query "..."` | fraza (głównie OpenSubtitles) |
| `-l`, `--lang` | filtr języka; **domyślnie `pl`** (spójnie z pobieraniem). Podaj `-l en` lub `-l pl,en`; pozycje bez znanego języka, np. katalog napiprojekt, są zawsze pokazywane |
| `--service` | `n24,np,os` |
| `--season` / `--episode` | dla seriali (OpenSubtitles) |
| `--pick` | wybierz wynik **interaktywnie** (lista) i pobierz |
| `--auto` | pobierz **najlepszy** wynik bez pytania (ranking wg `--movie`/pobrań) |
| `--movie PLIK` | auto-nazwa pod plik filmowy + ranking release do jego nazwy |
| `-o`, `--output` | nazwa pliku wyjściowego |
| `--fps`, flagi sanityzacji | jak przy pobieraniu |

Bez `--pick`/`--auto` polecenie tylko **wypisuje** listę wyników. Z `--pick`
otwiera listę TUI (↑↓ ruch, `ENTER` pobiera, `q` anuluje), a `--auto` bierze
najlepiej dopasowany wynik. Pobieralne są wyniki z napisy24 i OpenSubtitles
(napiprojekt w wyszukiwarce zwraca katalog filmów — pobieraj przez
`napiprojekt download`). Auto-nazwa: z `--movie` → `<film>.srt`, inaczej z release.

```bash
aqnapi search --title "Matrix"                       # tylko lista (domyślnie pl)
aqnapi search --title "Matrix" -l en                 # lista, angielskie
aqnapi search --imdb tt0133093 --pick                # wybór interaktywny + pobranie
aqnapi search --movie Matrix.1999.1080p.mkv --auto   # auto: najlepszy pod nazwę pliku
```

### `upload`
Wysyła napisy do wybranych serwisów (wspólne flagi mapowane per-serwis).
Domyślnie `--service np`.

| Opcja | Znaczenie |
|---|---|
| `--srt PLIK` | **wymagane** — plik napisów |
| `--movie FILM` | plik filmowy (hash; wymagany dla napiprojekt) |
| `--service` | `np`, `n24` (OpenSubtitles nieobsługiwany) |
| `-l`, `--lang` | język (`PL`/`ENG`/…) |
| `--imdb --title --title-pl --year --release --translator --sync --proof` | metadane |
| `--resolution WxH --duration HH:MM:SS --size BAJTY --fps` | dane techniczne |
| `--season --episode --episode-title` | serial |
| `--corrected --comment "..."` | poprawki (napiprojekt mode=1024) |
| `--fix-timing` | docina nachodzące czasy (napisy24) |
| `--dry-run` | walidacja bez wysyłki |

```bash
aqnapi upload --movie film.mkv --srt film.srt --service np --translator ja
```

### `hash FILM`
Wypisuje hasze OSH (`fh`) i MD5-10MiB (`md`), rozmiar i nazwę.

### `fps FILM`
Odczytuje FPS z pliku filmowego (MKV / AVI / MP4 / MOV). Oznacza wynik spoza
bramki zaufania `22 < fps < 32`.

### `convert WEJŚCIE`
Konwertuje dowolny obsługiwany format (SRT / MicroDVD / MPL2 / TMPlayer / WebVTT
`.vtt` / SubStation Alpha `.ass`/`.ssa`) do wybranego formatu (domyślnie SRT
UTF-8+BOM, LF). Format wyjścia z `--format` lub rozszerzenia `-o`. Offline.

| Opcja | Znaczenie |
|---|---|
| `-o`, `--output` | plik wyjściowy (rozszerzenie wyznacza format) |
| `--format` | `srt` / `vtt` / `ass` / `microdvd` (nadpisuje rozszerzenie) |
| `--movie FILM` | plik filmowy do odczytu FPS (MicroDVD) |
| `--fps` | wymuszony FPS |
| flagi sanityzacji | patrz sekcja „Sanityzacja" (`--strip-sdh` itd.) |

```bash
aqnapi convert napisy.txt -o napisy.srt --fps 25
aqnapi convert napisy.sub --movie film.mkv           # MicroDVD -> SRT (FPS z filmu)
aqnapi convert napisy.srt -o napisy.vtt              # eksport SRT -> WebVTT
aqnapi convert napisy.srt -o napisy.ass --strip-sdh  # eksport do ASS bez SDH
```

### `fpsconv WEJŚCIE`
Przelicza czasy przy zmianie klatkażu (`nowy = (from/to) · stary`) — gdy napisy
pasują do wydania o innym FPS niż film.

| Opcja | Znaczenie |
|---|---|
| `--from FPS` | FPS źródłowy napisów (np. `25`) |
| `--to FPS` | FPS docelowy (np. `23.976`) albo z `--movie` |
| `--movie FILM` | odczytaj docelowy FPS z pliku filmowego |
| `-o`, `--output` | wyjście (domyślnie `<wejście>.<to>fps.srt`) |
| `--format` | format wyjściowy jak w `convert` |

```bash
aqnapi fpsconv napisy.srt --from 25 --to 23.976
aqnapi fpsconv napisy.srt --from 25 --movie film.mkv   # to = FPS filmu
```

### `merge PLIK PLIK [PLIK ...]`
Łączy kilka plików napisów w jeden (np. CD1+CD2). Każdy kolejny plik jest
przesuwany w czasie: domyślnie o **koniec poprzedniego** (auto-złączenie), albo
o wartość z `--offset` (powtarzalną — po jednej na kolejny plik).

| Opcja | Znaczenie |
|---|---|
| `-o`, `--output` | wyjście (domyślnie `<pierwszy>.merged.srt`) |
| `--offset SEK` | przesunięcie kolejnego pliku (powtarzalne); domyślnie auto |
| `--fps` | FPS dla MicroDVD |
| `--format` | format wyjściowy (`srt`/`vtt`/`ass`/`microdvd`) |

```bash
aqnapi merge film.cd1.srt film.cd2.srt -o film.srt          # auto-złączenie
aqnapi merge cd1.srt cd2.srt --offset 3120 -o film.srt      # CD2 od 52:00
```

### `split WEJŚCIE --at CZAS`
Dzieli plik na części w podanych punktach czasowych (`--at` powtarzalny).
Domyślnie czasy każdej części po pierwszej są **zerowane** (start od 0) —
wygodne przy dzieleniu na CD; `--no-rebase` zachowuje czasy bezwzględne.

| Opcja | Znaczenie |
|---|---|
| `--at CZAS` | punkt podziału (`hh:mm:ss,mmm` lub sekundy); powtarzalny; **wymagany** |
| `-o`, `--output` | baza nazwy (części: `<baza>.partN.<ext>`; domyślnie `<wejście>`) |
| `--no-rebase` | nie zeruj czasów części |
| `--fps`, `--format` | jak wyżej |

```bash
aqnapi split film.srt --at 01:02:30,000 -o film        # -> film.part1.srt, film.part2.srt
aqnapi split film.srt --at 20:00 --at 40:00            # 3 części
```

### `config {init,show,path}`
Zarządza plikiem konfiguracyjnym `~/.config/aqnapi/config.ini` (lub `--config`).

| Podpolecenie | Działanie |
|---|---|
| `init` | interaktywne wpisanie poświadczeń (hasła bez echo); zapis z uprawnieniami `600`. Enter zostawia obecną wartość |
| `show` | pokaż konfigurację (hasła zamaskowane) |
| `path` | wypisz ścieżkę pliku |

```bash
aqnapi config init
aqnapi config show
aqnapi --config ./moje.ini config init
```

### `sync REFERENCE TARGET`
Synchronizuje czasy pliku `TARGET` do wzorca `REFERENCE`. Wyznacza transformację
liniową `nowy = skala · stary + offset` na podstawie **par kotwic** (linii, które
w obu plikach odpowiadają sobie w treści): 1 para = samo przesunięcie, 2+ par =
skala + offset (koryguje różnice FPS/dryf).

**Tryb interaktywny (domyślny, wymaga terminala):** oba pliki wyświetlane są w
**dwóch kolumnach** (lewa = wzór, prawa = do synchronizacji). Pod kolumnami
**panel podglądu** pokazuje pełną treść i czasy bieżącej (lub zaznaczonej) linii
z obu stron — łatwiej dobrać pary po treści, gdy tekst jest przycięty w kolumnie.

| Klawisz | Akcja |
|---|---|
| `TAB` | przełącz aktywną kolumnę |
| `↑`/`↓` lub `k`/`j` | poruszanie kursorem |
| `PgUp`/`PgDn`, `Home`/`End` | szybkie przewijanie |
| `ENTER` / `SPACE` | zaznacz bieżącą linię; po zaznaczeniu w obu kolumnach para jest **łączona** |
| `u` | cofnij ostatnią parę |
| `,` / `.` | (kolumna CEL) przesuń bieżącą linię o −0,1 s / +0,1 s |
| `<` / `>` | (kolumna CEL) przesuń o −1 s / +1 s |
| `e` | (kolumna CEL) wpisz dokładny czas startu (`hh:mm:ss,mmm` lub sekundy) |
| `a` | zastosuj i zapisz |
| `q` / `Esc` | wyjście bez zapisu |

Edycja czasów (`,` `.` `<` `>` `e`) działa na kolumnie **CEL**, zachowuje długość
bloku (przesuwa start i koniec) i można ją łączyć z parami kotwic lub stosować
samodzielnie (bez par → zapisywane są same ręczne korekty). Pasek stanu pokazuje
na bieżąco liczbę par oraz wyliczone `scale`/`offset`. Wynik zapisywany jest jako
SRT (UTF-8+BOM, LF), domyślnie `<target>.synced.srt`.

**Tryb nieinteraktywny (bez UI):**

| Opcja | Znaczenie |
|---|---|
| `--offset SEK` | proste przesunięcie o SEK sekund (może być ujemne) |
| `--anchor R,T` | para kotwic: nr linii wzorca, nr linii celu (1-based); powtarzalna |
| `-o`, `--output` | plik wyjściowy (domyślnie `<target>.synced.srt`) |
| `--fps` | FPS dla MicroDVD przy wczytaniu |

```bash
aqnapi sync wzor.srt do_sync.srt                 # interaktywnie (2 kolumny)
aqnapi sync wzor.srt do_sync.srt --offset -2.5   # przesuń o -2,5 s
aqnapi sync wzor.srt do_sync.srt --anchor 1,1 --anchor 250,248   # skala+offset
```

---

## Per-serwis: `napisy24` (alias `n24`)

| Podpolecenie | Opis |
|---|---|
| `hash FILM` | hasze pliku |
| `login` | weryfikacja danych logowania (CheckLogin) |
| `download FILM [FILM ...] [-l pl] [-o OUT] [--force]` | pobranie po haszu (CheckSubAgent); wiele plików, pomija istniejące |
| `search (--imdb tt.. \| --title T)` | wyszukiwanie (webapi.php) |
| `getid ID [-o OUT] [--movie FILM] [--force]` | pobranie po napisId (download.php) |
| `imdb ttNNNNNNN` | walidacja/rozwiązanie IMDB (CheckIMDB) |
| `upload --srt … --movie … [metadane] [--dry-run] [--fix-timing]` | upload przez formularz WWW |
| `delete ID` (alias `rm`) | usunięcie własnego napisu (best-effort) |

## Per-serwis: `napiprojekt` (alias `np`)

| Podpolecenie | Opis |
|---|---|
| `download FILM [FILM ...] [-l PL] [-o OUT] [--force]` | pobranie po haszu (mode=1); wiele plików, pomija istniejące |
| `search "tytuł"` | wyszukiwarka katalogu (MovieId + IMDB/Filmweb) |
| `associate FILM MOVIE_ID` | powiązanie pliku z filmem (associate2) |
| `account` | informacje o koncie (api_user_account) |
| `fileinfo FILM` | FPS z serwera (api.php?mode=file_info) |
| `upload --srt … --movie … [-l PL] [--translator A] [--corrected --comment C] [--dry-run]` | upload (mode=512/1024, 7z-AES) |

## Per-serwis: `opensubtitles` (alias `os`)

| Podpolecenie | Opis |
|---|---|
| `login` | logowanie (JWT, cache) |
| `logout` | wylogowanie |
| `search [--query \| --imdb \| --moviehash] [-l pl,en] [--season --episode]` | wyszukiwanie (/subtitles) |
| `download FILE_ID [-o OUT] [--movie FILM] [--force]` | pobranie (wymaga login); pomija istniejący plik (oszczędza limit) |
| `formats` | lista formatów wyjściowych |
| `languages` | lista języków |
| `guessit NAZWA_PLIKU` | parsowanie nazwy pliku |

---

## Sanityzacja — bezpieczne korekty przed zapisem

Każde **pobranie/konwersja** przepuszcza napisy przez zestaw bezpiecznych korekt
(nie zmieniają treści poza usunięciem tagów i pustych linii). Po zapisie
wypisywany jest krótki raport, np. `Korekty: usunięto tagi w 12, skrócono 3 zbyt
długich, naprawiono 2 nakładek`.

Domyślnie stosowane:
- **usuwanie tagów formatujących** — HTML (`<i>`, `<b>`, `<font …>`, …) oraz
  nawiasy klamrowe ASS/MicroDVD (`{\an8}`, `{y:i}`);
- **docinanie ekstremalnie długich** czasów wyświetlania (próg `--max-display`,
  domyślnie 10 s);
- **naprawa nakładających się** czasów (koniec docięty do początku następnego);
- **naprawa odwróconego/zerowego** czasu (koniec ≤ start);
- **usuwanie pustych bloków** i przenumerowanie.

Wykrywanie uszkodzeń: jeśli z niepustego wejścia nie uda się odczytać żadnej
linii, plik **nie jest zapisywany** (błąd) — w trybie `get` narzędzie próbuje
wtedy kolejnego serwisu.

| Flaga (przy `get`, `convert`, `download`, `getid`) | Znaczenie |
|---|---|
| `--keep-tags` | nie usuwaj tagów formatujących |
| `--strip-sdh` | usuń oznaczenia SDH/HI: `[odgłosy]`, `(opisy)`, `MÓWCA:`, `♪` (opcjonalne) |
| `--max-display SEK` | próg „ekstremalnie długo" (domyślnie 10) |
| `--min-display SEK` | minimalny czas wyświetlania — wydłuża zbyt krótkie (domyślnie 0 = wyłączone) |
| `--no-sanitize` | wyłącz wszystkie korekty |

```bash
aqnapi get film.mkv                      # z korektami (domyślnie)
aqnapi get film.mkv --keep-tags          # zachowaj kursywę itp.
aqnapi convert napisy.txt --max-display 7 --min-display 1
aqnapi get film.mkv --no-sanitize        # surowo, bez korekt
```

## Kody wyjścia

| Kod | Znaczenie |
|---|---|
| `0` | sukces |
| `1` | nie znaleziono / błąd sieci / serwera / pliku |
| `2` | błąd uwierzytelnienia lub złe argumenty |
| `130` | przerwano (Ctrl-C) |
