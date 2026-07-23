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
| `search` / `napiprojekt search` / `napisy24 search` (agreg. + per-serwis) | stdout bajtowo == Python (live) |
| `get` (agregator np→n24, HTTP) | pobiera i zapisuje SRT |
| `napiprojekt download` / `fileinfo` (HTTP) | bajtowo |
| `napisy24 download` (CheckSubAgent+ZIP) / `getid` (download.php+ZIP) | **plik+stdout bajtowo** (ZIP-inflate przez zlib) |
| `napiprojekt upload` (mode=512/1024, **7z-AES**) | własny AES-256+SHA-256+kontener 7z; **archiwum rozpakowywalne przez `7z x`**; odpowiedź serwera == Python |

Własna kryptografia zweryfikowana: **AES-256 (wektor FIPS-197)**, **SHA-256**,
oraz round-trip **7z-AES przez systemowe `7z`** (`aqnapi-c.com _selftest OUT.7z`).

| `update [--check]` | **wariant TLS** (`c/build-tls.sh`): HTTPS do GitHub API przez mbedtls, porównanie wersji, podmiana binarki. `--check` zweryfikowany na żywo (v1.0.0 == najnowsza) |

## Dwa warianty binarki C

- **`dist/aqnapi-c.com`** — build `cosmocc` (`c/build.sh`), lekki, **bez TLS**.
  `update`/`opensubtitles` wypisują, że wymagają wariantu TLS.
- **`dist/aqnapi-c-tls.com`** — build monorepo + `third_party/mbedtls`
  (`c/build-tls.sh`), **z TLS**. Ma działające `update` (HTTPS). Kod TLS jest pod
  `#ifdef AQNAPI_TLS` (włączany flagą `-DAQNAPI_TLS` w buildzie monorepo).

**TLS — jak redbean:** monorepo Cosmopolitan + `third_party/mbedtls` (MbedTLS
2.26). Zweryfikowano na żywo: handshake TLS 1.2 i pełny HTTPS do
`api.opensubtitles.com`/`api.github.com`/`napisy24.pl`. Szczegóły/PoC: [`tls/`](tls/).

**Jeszcze nie w C** (kolejne etapy): pełny klient **OpenSubtitles** (login→JWT,
search, download — fundament `https_fetch`+`json_str` już jest, brakuje parsera
JSON wyników + weryfikacji CA) i **WWW napisy24** (login/upload/delete);
napiprojekt `account`/`associate`, napisy24 `login`/`imdb`; **interaktywny
`sync`** (termios TUI). Do dystrybucji samo-aktualizacji: dodać `aqnapi-c.com`
jako artefakt wydania (obecnie release publikuje tylko wersję Python-APE).

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
