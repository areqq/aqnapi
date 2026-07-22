# aqnapi — natywna wersja C (POC, Cosmopolitan)

Niezależna reimplementacja **podzbioru** aqnapi w czystym C, kompilowana przez
[`cosmocc`](https://github.com/jart/cosmopolitan) do jednej uniwersalnej binarki
APE (`dist/aqnapi-c.com`) działającej na Linux/macOS/Windows/*BSD (x86-64+ARM64).

To **proof of concept** obok wersji Python (`aqnapi.py`). Wersja Python pozostaje
kompletna i referencyjna; wersja C ma być **maksymalnie zgodna bajtowo** w
zakresie, który obejmuje.

## Zakres (stan bieżący)

**Zaimplementowane w C i zweryfikowane bajtowo z wersją Python:**

| Polecenie | Zgodność |
|---|---|
| `hash PLIK` | bajtowo == `aqnapi hash` |
| `fps PLIK` (MKV/AVI/MP4/MOV) | bajtowo == `aqnapi fps` |
| `convert` — **wszystkie formaty wejścia** (SRT/MicroDVD/MPL2/TMPlayer/VTT/ASS) | **plik + stdout** bajtowo == `aqnapi convert` |
| `convert --format srt\|vtt\|ass\|microdvd` (eksport) | bajtowo |
| flagi: `--strip-sdh --keep-tags --no-sanitize --max-display --min-display` | bajtowo |
| dekodowanie wejścia **cp1250** (nie-UTF-8) | bajtowo (Polski) |
| pełna **sanityzacja** (tagi, długie, nakładki, złe/puste czasy) + raport „Korekty" | bajtowo |
| `fpsconv --from --to [--movie]` | bajtowo (z bankierskim zaokrągleniem) |
| `merge` (auto/`--offset`) | bajtowo |
| `split --at [--no-rebase]` | bajtowo |
| `config {init,show,path}` | plik i `show` bajtowo == Python (hasła bez echo, chmod 600) |
| `sync REF TGT --offset\|--anchor` (nieinteraktywny) | plik + stdout bajtowo == `aqnapi sync` |
| `download FILM` (napiprojekt `mode=1`, HTTP+base64) | ścieżka HTTP + pipeline; komunikaty == `aqnapi napiprojekt download` |

**Jeszcze nie w C** (kolejne etapy): plain-HTTP klienci napiprojekt
(search/account/associate/fileinfo) i napisy24 (webapi search, CheckSubAgent+ZIP);
`get` (agregator); upload 7z-AES do napiprojekt; interaktywny `sync` (termios TUI).
**Wymaga TLS (osobny etap: vendorowanie BearSSL + certyfikaty):** OpenSubtitles
(search/download/login) oraz logowanie/upload/delete WWW napisy24.

> `iso-8859-2` jako drugorzędny fallback kodowania oraz kilka rzadkich, niezdefiniowanych
> bajtów cp1250 są uproszczone względem Pythona (nie dotyczy typowych polskich napisów).

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
