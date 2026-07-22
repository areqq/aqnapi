# Protokół napiprojekt.pl w narzędziu aqnapi

Dokument opisuje protokół serwisu **napiprojekt.pl** w zakresie
wykorzystywanym przez narzędzie `aqnapi`. Uzupełnia i **koryguje** oficjalny
PDF *„DOKUMENTACJA API NAPIPROJEKT"*.

Wszystkie informacje zostały **zweryfikowane empirycznie** na żywym serwerze
(lipiec 2026) oraz przez analizę oryginalnego klienta desktopowego
(NapiProjekt 2.2.0.2399, Delphi/Indy). Miejsca sprzeczne z PDF-em oznaczono
jako **KOREKTA**.

---

## 1. Endpointy

| URL | Metoda | Auth | Zastosowanie |
|-----|--------|------|--------------|
| `http://www.napiprojekt.pl/api/api-napiprojekt3.php` | POST | ❌ brak | pobieranie (mode=1), upload (mode=512/1024) — **anonimowo** |
| `http://napiprojekt.pl/api/api-movie-search.php` | GET | — | wyszukiwarka katalogu (zwraca `MovieId`, IMDB, Filmweb, FDB) |
| `http://napiprojekt.pl/api/api-movie-associate2.php` | GET | ✅ hasło jawne | powiązanie pliku z filmem (add) |
| `http://napiprojekt.pl/api/api_user_account.php` | GET | ✅ hasło jawne | informacje o koncie |
| `http://napiprojekt.pl/api/api-napiprojekt2.php` | GET | — | starsze łączone API (napisy + okładka) |
| `http://napiprojekt.pl/api/api.php?mode=file_info` | GET | — | info o pliku (m.in. `<fps>`) |
| `http://napiprojekt.pl/unit_napisy/dl.php` | GET | — | legacy: pobieranie napisów (funkcja `f()`) |

> **KOREKTA — Cloudflare:** strony i endpointy pod `/ajax/` (np.
> `search_catalog.php`) oraz `/napisy-szukaj` są za Cloudflare i **blokują
> zapytania skryptowe** (HTTP 403 „Just a moment…"). Ścieżka **`/api/` NIE jest
> blokowana** — dlatego wszystkie operacje `aqnapi` kieruje na endpointy `/api/`.

---

## 2. Autoryzacja — NAJWAŻNIEJSZA KOREKTA

Oficjalny PDF opisuje parametr `User_password` kodowany XOR (klucz=3) → base64
i przekazywany do `api-napiprojekt3.php`. **W praktyce:**

- **`api-napiprojekt3.php` NIE uwierzytelnia użytkownika.** Tryby wymagające
  logowania (mode=32 konto, mode=128 skojarzenie) odrzucają **każde** hasło
  (jawne, XOR+base64, MD5) komunikatem „Błędny login lub hasło". Upload
  (mode=512) przechodzi, ale zawsze jako **anonim**.
- **Prawdziwa autoryzacja działa hasłem JAWNYM** (plaintext) w parametrze `pass`
  na dedykowanych endpointach klienta: `api_user_account.php`,
  `api-movie-associate2.php`.

Kodowanie XOR+base64 (z PDF) jest więc de facto **nieużywane / nieaktualne**.
`aqnapi` pozostawia je dla parametru `User_password` (zgodność z PDF), ale do
realnego logowania używa hasła jawnego.

### Kodowanie hasła XOR+base64 (wg PDF — dla kompletności)

```python
import base64
def encode_password(p):           # XOR każdego znaku kluczem 3, potem base64
    return base64.b64encode(bytes(b ^ 3 for b in p.encode())).decode()
```

---

## 3. Hash NapiProjekt

Identyfikator pliku = **MD5 pierwszych 10 485 760 bajtów (10 MiB)** pliku.

```python
import hashlib
def movie_hash(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read(10 * 1024 * 1024)).hexdigest()
```

Używany jako `downloaded_subtitles_id` (pobieranie), nazwa archiwum/wpisu
(upload) oraz `id_pliku` (skojarzenie). W `aqnapi` liczy go funkcja
`md5_10mb()` / `movie_hash()`.

---

## 4. Pobieranie napisów — `mode=1`

POST `api-napiprojekt3.php`:

| Parametr | Wartość |
|----------|---------|
| `mode` | `1` |
| `client`, `client_ver` | dowolne |
| `downloaded_subtitles_id` | hash NapiProjekt |
| `downloaded_subtitles_lang` | `PL` lub `ENG` |
| `downloaded_subtitles_txt` | `1` → napisy w polu `<content>` (base64) |

**Odpowiedź — są napisy:**

```xml
<result>
  <status>success</status>
  <subtitles>
    <id>71f94b53…</id>
    <subs_hash>e23b2810…</subs_hash>   <!-- MD5 pliku napisów -->
    <filesize>41070</filesize>
    <author>Areq</author>
    <uploader>anonim</uploader>
    <upload_date>2026-07-20 12:28:38</upload_date>
    <content><![CDATA[ …base64 napisów… ]]></content>
  </subtitles>
</result>
```

**Odpowiedź — brak napisów:** `<result><response_time>…</response_time></result>`
(brak `<status>` / `<subtitles>`).

> **KOREKTA nazw pól:** MD5 napisów jest w `<subs_hash>` (nie `<md5>`).
> Metadane: `<author>`, `<uploader>`, `<upload_date>`.

### Legacy (alternatywa) — `unit_napisy/dl.php`

```
GET http://napiprojekt.pl/unit_napisy/dl.php?l=PL&f=<hash>&t=<f(hash)>&v=other&kolejka=false&nick=&pass=&napios=posix
```

`f(hash)` — funkcja mieszająca (identyczna w oryginalnym kliencie i DMnapi):

```python
def f(z):
    idx=[0xe,0x3,0x6,0x8,0x2]; mul=[2,2,5,4,3]; add=[0,0xd,0x10,0xb,0x5]; o=[]
    for i in range(5):
        t=add[i]+int(z[idx[i]],16); v=int(z[t:t+2],16); o.append(("%x"%(v*mul[i]))[-1])
    return "".join(o)
```

Odpowiedź to surowe napisy; marker **`NPc0`** (4 bajty) = brak w bazie.

---

## 5. Upload napisów — `mode=512` (nowe) / `mode=1024` (poprawki)

POST **multipart/form-data** na `api-napiprojekt3.php`:

| Parametr | Opis |
|----------|------|
| `mode` | `512` (nowe) lub `1024` (poprawki) |
| `SubtitlesHash` | MD5 **całego** pliku tekstowego napisów |
| `SubtitlesAutor` | autor (może być pusty) |
| `SubtitlesLang` | `PL` / `ENG` |
| `SubtitlesComment` | komentarz (dla poprawek) |
| `OnlyTesting` | `1` → walidacja bez zapisu (tryb testowy) |
| `User_nick`, `User_password` | opcjonalne (i tak ignorowane — patrz niżej) |
| **`subtitles`** (plik) | archiwum 7zip, typ `subtitles/zip` |

Archiwum: **7zip** z hasłem `iBlm8NTigvru0Jr0`, nazwa `<hashFilmu>.zip`,
zawiera jeden wpis `<hashFilmu>.txt` (tak serwer poznaje hash filmu — osobnego
parametru z hashem nie ma).

> **KOREKTA — nazwa pola pliku:** musi być **`subtitles`** (małą literą).
> Nagłówek tabeli w PDF („Subtitles") jest mylący; wielka litera → błąd
> „Brak pliku z napisami". Właściwa jest dopiero nota „w parametrze subtitles".

> **KOREKTA — anonimowość:** upload nie uwierzytelnia (patrz §2). Napisy
> wgrywają się, ale zawsze z `uploader=anonim`, niezależnie od podanego konta.

### Budowa archiwum 7z-AES w aqnapi (czysty Python)

W odróżnieniu od oryginalnego klienta napiproject (który korzystał z biblioteki
`py7zr` albo systemowego binarium `7z`), **`aqnapi` buduje szyfrowane archiwum
7z (AES-256) całkowicie w czystym Pythonie z biblioteki standardowej** —
własną implementacją AES-256 (`class AES`, `aes_cbc_encrypt`), derywacją klucza
7-Zip (`sevenzip_key`, jeden kontekst SHA-256 z solą i liczbą iteracji) oraz
minimalnym zapisem kontenera 7z (`write_7z_aes`, metoda składowania „store" +
kodek `7zAES256SHA256`, id `06 F1 07 01`).

Dzięki temu upload **nie wymaga żadnej zewnętrznej zależności ani zainstalowanego
binarium `7z`** — działa na czystej instalacji Pythona. Hasło archiwum:
`iBlm8NTigvru0Jr0` (stała `SEVENZIP_PASSWORD`).

**Odpowiedź (sukces):**

```xml
<result><upload_subtitles>
  <status>uploaded</status>
  <warning>testing mode was enabled</warning>   <!-- tylko przy OnlyTesting=1 -->
</upload_subtitles></result>
```

**Odpowiedź (błąd):**

```xml
<result><upload_subtitles>
  <status>failed</status>
  <error>Brak pliku z napisami</error>
</upload_subtitles></result>
```

Znane `error`: „Brak pliku z napisami", „Nieznany język napisów", „Błędny
rozmiar/typ/nazwa pliku", „Napisy dla tego filmu znajdują się już na serwerze
NapiProjekt".

---

## 6. Wyszukiwarka katalogu — `api-movie-search.php`

```
GET http://napiprojekt.pl/api/api-movie-search.php?mode=get&client=allplayer&search=<tytuł>
```

- Szuka **tylko po tytule** (po `tt…`/id Filmweb → „no search results”).
- Wyniki **zawierają linki IMDB / Filmweb / FDB** — filtruj po nich, by trafić
  konkretny film.

**Odpowiedź (fragment):**

```xml
<result>
  <status>success</status>
  <movie>
    <description>
      <id>8bf5d4ac4eadbf91d9661e1f4799745c0a33f7e5</id>   <!-- MovieId (id_filmu) -->
      <titles><polish>Avatar: Ogień i popiół</polish>
              <original>Avatar: Fire and Ash</original></titles>
      <year>2025</year>
      <tv_series>0</tv_series>
      <direct_links>
        <imdb_com>https://www.imdb.com/title/tt1757678/</imdb_com>
        <filmweb_pl>https://www.filmweb.pl/film/…-2025-603081</filmweb_pl>
        <fdb_pl>https://fdb.pl/film/258020-…</fdb_pl>
      </direct_links>
    </description>
    <covers><cover_thumb_link>http://napiprojekt.pl/okladki/…_thumb.jpg</cover_thumb_link></covers>
  </movie>
  …
</result>
```

> **`MovieId` (pole `<id>`) to 40-znakowy hash (SHA1)** — NIE numer IMDB ani
> Filmweb, i NIE stary numeryczny id z URL-i typu `…-dla-14671-…`.

---

## 7. Powiązanie pliku z filmem — `api-movie-associate2.php`

```
GET http://napiprojekt.pl/api/api-movie-associate2.php?nick=<login>&pass=<hasło_jawne>&id_pliku=<hashFilmu>&id_filmu=<MovieId>
```

| Parametr | Opis |
|----------|------|
| `nick` | login |
| `pass` | **hasło jawne** (nie kodowane) |
| `id_pliku` | hash NapiProjekt pliku |
| `id_filmu` | `MovieId` (pole `<id>`) z wyszukiwarki |

- **Metoda musi być GET.** POST → „wrong login or password".
- Powiązanie wchodzi **natychmiast** (brak trybu testowego, licznik
  `<przypisane>` konta rośnie od razu).

**Odpowiedzi:**

- sukces: `<result><status>success</status></result>`
- brak `id_filmu`: „Nie potrafie stworzyc obiektu"
- plik ma już powiązanie: „Przypisanie już istnieje…"

> **KOREKTA:** udokumentowany `mode=128`/`256` (`AFA_*`/`RFA_*`) na
> `api-napiprojekt3.php` **nie działa** (endpoint nie autoryzuje). Powiązania
> robi się przez `associate2.php`.

> **OGRANICZENIE:** `associate2.php` udostępnia tylko **dodawanie**.
> Operacje `delete_file_assocation` / `replace_file_assocation` (widoczne w
> kliencie) idą torem webowym/moderacyjnym i nie są dostępne przez to API —
> błędne powiązanie poprawia się na stronie napiprojekt.pl (zalogowany).

---

## 8. Konto użytkownika — `api_user_account.php`

```
GET http://napiprojekt.pl/api/api_user_account.php?user=<login>&pass=<hasło_jawne>
```

**Odpowiedź:**

```xml
<user_info>
  <konto><nick>areq</nick><email>…</email><vip>0</vip>
         <miejsce>1432</miejsce><punkty>140</punkty></konto>
  <napisy><pobrane>306</pobrane><dodane>112</dodane></napisy>
  <poprawki><zaakceptowane>3</zaakceptowane><odrzucone>1</odrzucone></poprawki>
  <filmy><nowe_tytuly>0</nowe_tytuly><przypisane>64</przypisane><linki>30</linki></filmy>
</user_info>
```

Puste ciało (`<result><response_time/></result>` lub brak `<user_info>`) =
błędne dane logowania.

> **KOREKTA:** informacje o koncie są tutaj (hasło jawne), a nie przez
> `mode=32` na `api-napiprojekt3.php` (który zwraca pustkę).

---

## 9. Info o pliku (FPS) — nieudokumentowane

```
GET http://napiprojekt.pl/api/api.php?mode=file_info&client=dreambox&id=<hash>
```

Zwraca m.in. `<fps>` — używane do konwersji MicroDVD → SRT. Parametr
`downloaded_subtitles_FPS` (obecny w kliencie) także dotyczy FPS przy
pobieraniu.

---

## 10. Podsumowanie korekt względem oficjalnego PDF

1. Pole pliku przy uploadzie to **`subtitles`** (małą literą), nie `Subtitles`.
2. **`api-napiprojekt3.php` nie uwierzytelnia** — logowanie działa hasłem
   **jawnym** (`pass`) na `associate2.php` / `api_user_account.php`.
3. Skojarzenie z filmem: **`associate2.php`** (GET, `id_pliku`/`id_filmu`), a nie
   `mode=128/256`.
4. `MovieId` to 40-znakowy hash z wyszukiwarki, nie numer IMDB/Filmweb.
5. Wyszukiwarka katalogu działa pod `/api/` (`api-movie-search.php`) — bez
   Cloudflare; zwraca linki IMDB/Filmweb/FDB.
6. Odpowiedzi upload/associate są zagnieżdżone (`<upload_subtitles>`), status
   sukcesu uploadu = `uploaded`.
7. MD5 napisów w odpowiedzi pobierania jest w `<subs_hash>`.
8. Endpointy nieudokumentowane: `api-movie-search.php`, `api-movie-associate2.php`,
   `api_user_account.php`, `api.php?mode=file_info`, `api-napiprojekt2.php`.

---

## Komendy aqnapi

Poniżej mapowanie protokołu na polecenia narzędzia `aqnapi`. Podpolecenia
per-serwis żyją pod `aqnapi napiprojekt <podpolecenie>`.

### Podpolecenia napiprojekt

```
aqnapi napiprojekt download PLIK [-l PL] [-o out.srt]
```
Pobranie napisów: `mode=1` po haszu NapiProjekt (MD5 pierwszych 10 MiB pliku).
FPS potrzebny do konwersji MicroDVD → SRT jest dociągany m.in. z endpointu
`file_info`, a jeśli to możliwe — odczytywany bezpośrednio z pliku filmowego.

```
aqnapi napiprojekt upload --srt PLIK.srt --movie FILM [-l PL] \
    [--translator autor] [--corrected --comment "..."] [--dry-run]
```
Wysyłka napisów: `mode=512` (nowe) lub `mode=1024` (poprawki — po `--corrected`).
`--movie` to plik filmu, z którego liczony jest hash. Archiwum 7z-AES budowane
jest **lokalnie w czystym Pythonie** (patrz §5). `--dry-run` → `OnlyTesting=1`
(walidacja bez zapisu). `--translator` → `SubtitlesAutor`, `--comment` →
`SubtitlesComment`.

```
aqnapi napiprojekt search "tytuł"
```
Wyszukiwarka katalogu (`api-movie-search.php`). Zwraca `MovieId` (40-znakowy
SHA1) wraz z linkami IMDB / Filmweb.

```
aqnapi napiprojekt associate FILM MOVIE_ID
```
Powiązanie pliku z filmem (`associate2.php`, GET, hasło **jawne**). `FILM` to
plik filmowy (liczony hash → `id_pliku`), `MOVIE_ID` to `id_filmu` z wyszukiwarki.

```
aqnapi napiprojekt account
```
Informacje o koncie (`api_user_account.php`, hasło jawne).

```
aqnapi napiprojekt fileinfo PLIK
```
Odczyt FPS z serwera (`api.php?mode=file_info`).

### Polecenia zbiorcze (wiele serwisów)

```
aqnapi get FILM --service np
aqnapi upload --service np --srt PLIK.srt --movie FILM
```
`get` pobiera napisy dla pliku z wybranego serwisu (`np` = napiprojekt);
`upload` wysyła napisy do wskazanego serwisu.

### Dane logowania

Login i hasło napiprojekt można podać na trzy sposoby (kolejność: flaga →
zmienna środowiskowa → plik konfiguracyjny):

1. flagi `--np-user` / `--np-pass`,
2. zmienne środowiskowe `NAPI_USER` / `NAPI_PASS`,
3. plik `~/.config/aqnapi/config.ini`, sekcja `[napiprojekt]`:

```ini
[napiprojekt]
user = twoj_login
pass = twoje_haslo
```

Hasło do napiprojekt jest przekazywane **jawnie** (patrz §2), więc chroń plik
konfiguracyjny i zmienne środowiskowe.

### Format zapisywanych napisów

Pobrane napisy są **zawsze konwertowane do SRT w kodowaniu UTF-8 z BOM i
końcami wiersza LF**. Napisy w formacie MicroDVD są konwertowane do SRT z
użyciem FPS — odczytywanego z pliku filmowego, gdy jest dostępny, a w przeciwnym
razie z serwera (`file_info`).
