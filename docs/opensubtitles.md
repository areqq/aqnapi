# OpenSubtitles.com — REST API v1

Dokumentacja integracji serwisu [OpenSubtitles.com](https://www.opensubtitles.com) (REST API v1) w narzędziu `aqnapi`. `aqnapi` obsługuje **wyszukiwanie** i **pobieranie** napisów. Upload do OpenSubtitles jest świadomie nieobsługiwany — zobacz sekcję [Upload — niedostępny](#upload--niedostępny).

---

## Adresy bazowe (Base URL)

| Typ konta | Base URL |
|-----------|----------|
| Standardowe | `https://api.opensubtitles.com/api/v1` |
| VIP | `https://vip-api.opensubtitles.com/api/v1` |

Zasady:

- `POST /login` zwraca pole `base_url` (host) — **należy go używać dla kolejnych żądań**.
- Jeśli zwróconym hostem jest `vip-api`, dołączaj token JWT do **każdego** żądania.
- Zawsze podążaj za przekierowaniami (redirectami).

---

## Uwierzytelnianie

Istnieją **dwa niezależne poświadczenia**:

### 1. Api-Key (nagłówek `Api-Key`)

Identyfikuje **aplikację**. Sposób uzyskania:

1. Załóż konto na `opensubtitles.com`.
2. Wejdź w profil → sekcja **„API Consumers”**.
3. Zarejestruj konsumenta — zostanie wygenerowany klucz.

Dostępne dla każdego zarejestrowanego użytkownika.

### 2. Bearer JWT (nagłówek `Authorization: Bearer <token>`)

Identyfikuje **użytkownika**. Uzyskiwany przez `POST /login`.

- Token JWT jest ważny **~24 h**.
- Zawiera claim `exp` (czas Unix), który można odczytać dekodując base64 środkowego segmentu tokena.
- **Na błąd `401` z `/login` należy PRZESTAĆ ponawiać** — logowanie jest rate-limitowane.

### Które endpointy czego wymagają

| Endpoint | Api-Key | Bearer JWT |
|----------|:-------:|:----------:|
| `POST /login` | ✅ | — |
| `GET /subtitles` (search) | ✅ | — |
| `GET /features` | ✅ | — |
| `GET /infos/*` | ✅ | — |
| `GET /utilities/guessit` | ✅ | — |
| `POST /download` | ✅ | ✅ |
| `DELETE /logout` | ✅ | ✅ |

---

## Nagłówki HTTP

Każde żądanie powinno zawierać:

| Nagłówek | Wartość | Uwagi |
|----------|---------|-------|
| `Api-Key` | `<klucz>` | Zawsze wymagany |
| `User-Agent` | `<AppName> v<Version>` | **WYMAGANY i walidowany** — zły lub brakujący → `403` |
| `Content-Type` | `application/json` | Dla żądań `POST` |
| `Accept` | `application/json` | |

Uwaga: odpowiedzi mogą być skompresowane (`gzip`).

---

## `POST /login`

Wymaga tylko `Api-Key`.

**Żądanie (body JSON):**

```json
{
  "username": "twoj_login",
  "password": "twoje_haslo"
}
```

**Odpowiedź:**

| Pole | Typ | Opis |
|------|-----|------|
| `user.allowed_downloads` | int | Dozwolona liczba pobrań / dzień (autorytatywne) |
| `user.level` | string | Poziom konta |
| `user.user_id` | int | Identyfikator użytkownika |
| `user.vip` | bool | Czy konto VIP |
| `base_url` | string | Host do kolejnych żądań |
| `token` | string | JWT |

**Limity `/login`:** 1/s, 10/min, 30/h.

---

## `GET /subtitles` (wyszukiwanie)

Wymaga tylko `Api-Key`. **Wyszukiwanie nie zużywa limitu pobrań (quota).**

### Parametry (wszystkie opcjonalne)

| Parametr | Opis |
|----------|------|
| `query` | Tekst zapytania (np. tytuł) |
| `id` | ID napisów |
| `imdb_id` | IMDb ID **bez `tt` i bez wiodących zer** |
| `tmdb_id` | TMDB ID |
| `type` | `movie` / `episode` / `all` |
| `languages` | Kody języków, rozdzielone przecinkami, **posortowane alfabetycznie, małymi literami** |
| `moviehash` | Hash OSH (patrz niżej) |
| `moviehash_match` | `include` / `only` |
| `year` | Rok |
| `season_number` | Numer sezonu |
| `episode_number` | Numer odcinka |
| `parent_feature_id` | ID nadrzędnego dzieła |
| `parent_imdb_id` | IMDb ID nadrzędnego dzieła |
| `parent_tmdb_id` | TMDB ID nadrzędnego dzieła |
| `uploader_id` | ID uploadera |
| `hearing_impaired` | Napisy dla niesłyszących |
| `foreign_parts_only` | Tylko fragmenty obcojęzyczne |
| `trusted_sources` | Zaufane źródła |
| `machine_translated` | Tłumaczenie maszynowe |
| `ai_translated` | Tłumaczenie AI |
| `order_by` | Pole sortowania |
| `order_direction` | Kierunek sortowania |
| `page` | Numer strony |

### Zasady wydajności (cache)

- Parametry GET **posortowane alfabetycznie**.
- Wartości **małymi literami**.
- `+` zamiast spacji.
- **Pomijaj wartości domyślne.**

### Odpowiedź (JSON)

Pola nagłówkowe: `total_pages`, `total_count`, `per_page`, `page`, `data[]`.

Każdy element `data[]`:

- `.id`
- `.type`
- `.attributes`:
  - `subtitle_id`, `language`, `download_count`, `hearing_impaired`, `hd`, `fps`, `votes`, `ratings`, `from_trusted`, `foreign_parts_only`, `upload_date`, `ai_translated`, `machine_translated`, `release`, `comments`
  - `uploader { uploader_id, name, rank }`
  - `feature_details { feature_id, feature_type, year, title, movie_name, imdb_id, tmdb_id, season_number, episode_number, parent_title, ... }`
  - `url`
  - `files[] { file_id, cd_number, file_name }`

> To wartość `files[].file_id` przekazuje się do `POST /download`.

---

## `POST /download`

Wymaga **obu** nagłówków: `Api-Key` + `Bearer`.

**Żądanie (body JSON):**

| Pole | Typ | Wymagane | Opis |
|------|-----|:--------:|------|
| `file_id` | int | ✅ | ID pliku z `files[].file_id` |
| `sub_format` | string | — | Format wyjściowy |
| `file_name` | string | — | Nazwa pliku |
| `in_fps` + `out_fps` | number | — | Podawane **parami** (konwersja FPS) |
| `timeshift` | number | — | Przesunięcie czasu |
| `force_download` | bool | — | Wymuszenie pobrania |

> **Quota (liczba pobrań) jest naliczana przy TYM wywołaniu**, nie przy pobraniu pliku z `link`.

**Odpowiedź:**

| Pole | Opis |
|------|------|
| `link` | Tymczasowy URL (ważny **max 3 h**, treść zawsze **UTF-8**) |
| `file_name` | Nazwa pliku |
| `requests` | Liczba wykonanych żądań |
| `remaining` | Pozostałe pobrania |
| `message` | Komunikat |
| `reset_time` | Czas resetu limitu |
| `reset_time_utc` | Czas resetu limitu (UTC) |

Następnie należy wykonać `GET` na adres `link` (podążając za redirectami), aby pobrać właściwy plik.

### Limity pobrań / 24 h

| Konto | Limit |
|-------|-------|
| Anonimowe | 5 / IP |
| Darmowe | 20 / dzień (autorytatywne jest pole `allowed_downloads` z `/login`) |
| VIP | do 1000 |

---

## Hash `moviehash` (OSH)

Identyczny jak klasyczny hash OpenSubtitles:

```
hash = filesize
     + suma pierwszych 64 KiB pliku
     + suma ostatnich 64 KiB pliku
```

- Bajty czytane jako **little-endian uint64**.
- Arytmetyka **mod 2^64**.
- Minimalny rozmiar pliku: **131072 B (128 KiB)**.
- Wynik: **16 znaków hex, małymi literami**.

---

## Pozostałe endpointy

| Endpoint | Opis |
|----------|------|
| `GET /infos/formats` | Lista formatów wyjściowych |
| `GET /infos/languages` | Lista języków |
| `GET /utilities/guessit?filename=...` | Parsowanie nazwy pliku → tytuł / sezon / odcinek |
| `GET /discover/popular` | Popularne |
| `GET /discover/latest` | Najnowsze |
| `GET /discover/most_downloaded` | Najczęściej pobierane |
| `GET /features` | Dzieła (filmy/seriale) |
| `GET /ai/*` | Endpointy AI |

---

## Limity globalne

- Globalnie **5 req/s per IP** → przekroczenie zwraca `429`.
- Odpowiedzi zawierają nagłówki `ratelimit-*`.

---

## Komendy aqnapi

### Logowanie / wylogowanie

```bash
aqnapi opensubtitles login
aqnapi opensubtitles logout
```

Token JWT jest cache'owany w `~/.cache/aqnapi/os_token.json` z odczytem claimu `exp` (ważność tokena).

### Wyszukiwanie

```bash
aqnapi opensubtitles search [--query ... | --imdb tt.. | --moviehash HASH] \
    [-l pl,en] [--season N --episode M]
```

| Flaga | Opis |
|-------|------|
| `--query ...` | Wyszukiwanie po tytule / tekście |
| `--imdb tt..` | Wyszukiwanie po IMDb ID |
| `--moviehash HASH` | Wyszukiwanie po hashu OSH |
| `-l pl,en` | Języki (rozdzielone przecinkami) |
| `--season N` | Numer sezonu |
| `--episode M` | Numer odcinka |

### Pobieranie

```bash
aqnapi opensubtitles download FILE_ID [-o out.srt] [--movie FILM]
```

- Wymaga zalogowania (`login`).
- `FILE_ID` to `files[].file_id` z wyników wyszukiwania.
- `-o out.srt` — plik wyjściowy.
- `--movie FILM` — powiązanie z plikiem filmu.

### Informacje i narzędzia

```bash
aqnapi opensubtitles formats
aqnapi opensubtitles languages
aqnapi opensubtitles guessit NAZWA_PLIKU
```

### Komendy zbiorcze (aggregating)

```bash
aqnapi get FILM --service os      # search po moviehash + download
aqnapi search --service os
```

### Poświadczenia (Credentials)

Poświadczenia można podać na trzy sposoby (kolejność priorytetów zależna od implementacji):

| Sposób | Api-Key | Login | Hasło |
|--------|---------|-------|-------|
| Flagi CLI | `--os-api-key` | `--os-user` | `--os-pass` |
| Zmienne środowiskowe | `OS_API_KEY` | `OS_USERNAME` | `OS_PASSWORD` |
| Plik konfiguracyjny | `api_key` | `username` | `password` |

Plik konfiguracyjny: `~/.config/aqnapi/config.ini`, sekcja `[opensubtitles]`:

```ini
[opensubtitles]
api_key = twoj_klucz
username = twoj_login
password = twoje_haslo
```

### Format pobranych napisów

Pobrane napisy są zawsze konwertowane do formatu **SRT, UTF-8 z BOM, zakończenia linii LF**.

---

## Upload — niedostępny

**REST API OpenSubtitles NIE MA endpointu uploadu.** Nie istnieje `POST /upload` w REST.

Upload napisów jest dostępny **wyłącznie** przez przestarzałe API **XML-RPC** na serwerze `api.opensubtitles.org`:

- Metody: `TryUploadSubtitles` → `UploadSubtitles`.
- Treść napisów przesyłana jako **gzip + base64**.
- To API bywa wyłączane lub ograniczane do kont VIP.

Z tego powodu `aqnapi` **świadomie nie implementuje uploadu do OpenSubtitles** — obsługuje tylko **wyszukiwanie** i **pobieranie**.

> Dla porównania: serwisy **n24** i **napiprojekt** mają w `aqnapi` pełną obsługę uploadu.
