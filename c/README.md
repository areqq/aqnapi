# aqnapi — natywna wersja C (POC, Cosmopolitan)

Niezależna reimplementacja **podzbioru** aqnapi w czystym C, kompilowana przez
[`cosmocc`](https://github.com/jart/cosmopolitan) do jednej uniwersalnej binarki
APE (`dist/aqnapi-c.com`) działającej na Linux/macOS/Windows/*BSD (x86-64+ARM64).

To **proof of concept** obok wersji Python (`aqnapi.py`). Wersja Python pozostaje
kompletna i referencyjna; wersja C ma być **maksymalnie zgodna bajtowo** w
zakresie, który obejmuje.

## Zakres POC

| Polecenie | C (POC) | Zgodność |
|---|:---:|---|
| `hash PLIK` | ✅ | bajtowo == `aqnapi hash` |
| `fps PLIK` (MKV/AVI/MP4/MOV) | ✅ | bajtowo == `aqnapi fps` |
| `convert IN [-o] [--fps] [--movie]` (SRT/MicroDVD/VTT) | ✅ | **plik i stdout** bajtowo == `aqnapi convert` (z pełną sanityzacją: tagi, docinanie długich, nakładki, złe czasy, puste) |
| `download FILM [-l] [-o] [--fps]` (napiprojekt `mode=1`, HTTP) | ✅ | ścieżka HTTP + base64 + pipeline; komunikaty == `aqnapi napiprojekt download` |

**Poza zakresem POC** (pozostaje w Pythonie): OpenSubtitles i logowanie WWW
napisy24 (TLS), pobieranie ZIP z napisy24, upload 7z-AES, `sync` (curses),
`merge`/`split`/`config`, formaty ASS/MPL2/TMPlayer, transkodowanie
cp1250/iso-8859-2 (POC zakłada wejście UTF-8).

## Budowanie

```sh
c/build.sh            # -> dist/aqnapi-c.com  (pobierze cosmocc do c/toolchain/ za 1. razem)
COSMOCC_URL=... c/build.sh
./dist/aqnapi-c.com --help
```

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
