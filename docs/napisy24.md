# Napisy24.pl — protokół serwisu (dokumentacja dla `aqnapi`)

Dokument opisuje protokół serwisu **Napisy24.pl** w zakresie, w jakim korzysta
z niego narzędzie `aqnapi`. Treść powstała na bazie inżynierii wstecznej
oficjalnego klienta Windows `Napisy24.exe` (Delphi/RAD Studio, wersja
**v1.99.1**) uruchamianego pod Wine oraz na prawdziwym Windows z przechwytywaniem
ruchu HTTP (mitmproxy). Wszystkie ustalenia zostały zweryfikowane na żywym
serwerze.

Podsumowanie: klient desktopowy używa zaciemnianego (obfuskowanego) protokołu
POST pod `/run/`, ale najprostsze i zalecane w reimplementacji ścieżki to
otwarte (plain-text) endpointy: `libs/webapi.php` (wyszukiwanie po IMDb/tytule),
`download.php?napisId=` (pobieranie po id) oraz `CheckSubAgent.php` (pobieranie
po haszu, wspólne konto agenta). Dodawanie napisów przez klienta
(`AddSub.php`) jest zablokowane po stronie serwera — upload realizuje się przez
**formularz WWW** `/dodaj-napisy`.

---

## Transport

* Wszystkie wywołania API klienta to **HTTP** (zwykłe, port 80) do hosta `napisy24.pl`.
* Endpointy znajdują się pod **`/run/`**, np. `http://napisy24.pl/run/CheckSub2.php`.
* Metoda: **POST**, `Content-Type: multipart/form-data`.
* Nagłówek `User-Agent: Mozilla/4.0`, linia żądania używa `HTTP/1.0`.
* Każde żądanie niesie `postAction` (nazwa operacji) oraz `postVer` (wersja
  klienta, `v1.99.1`) jako części tekstowe.
* `subtitles24.com/run/links.php` służy wyłącznie autoaktualizatorowi, nie API.

---

## Zaciemnianie pól (field obfuscation)

Wrażliwe / identyfikujące wartości nie są wysyłane jawnie. Klient stosuje
**odwracalną transformację bajtów** i koduje wynik szesnastkowo:

```
enc[i] = plain[i] XOR ((0x7F + (i + 1)^2) & 0xFF)     # i = indeks bajtu (od 0)
wire   = hex_uppercase( reverse(enc) )                # bajty odwrócone, potem hex
```

Zatem klucz XOR na kolejnych bajtach to `0x80, 0x83, 0x88, 0x8F, 0x98, 0xA3, 0xB0, 0xBF, 0xD0, …`
(`0x7F + n^2`), a cały ciąg bajtów jest odwracany przed zakodowaniem do hex.

Dekodowanie jest dokładną odwrotnością: `bytes.fromhex(wire)` → reverse → XOR
tym samym kluczem.

Pola **jawne** (nie zaciemniane): `postAction`, `postVer`, `n24pref`, `licz`.
Pola **zaciemniane**: `fh`, `md`, `fs`, `fn`, `nl`, `login`, `pass` oraz pola
z haszem filmu w `AddSub`.

W `aqnapi` transformacja jest zaimplementowana jako `n24_obf` (kodowanie) i
`n24_deobf` (dekodowanie, do diagnostyki/testów).

---

## Hasze filmu

Plik filmowy identyfikują dwa niezależne hasze:

| pole | znaczenie | format |
|------|-----------|--------|
| `fh` | **hasz OpenSubtitles/MPC** — `rozmiar_pliku + 64-bitowa suma pierwszych i ostatnich 64 KiB`, czytane jako słowa `int64` little-endian | 16 znaków hex **WIELKIMI** literami |
| `md` | **hasz napiprojekt** — `MD5` z **pierwszych 10 485 760 bajtów** (10 MiB) | 32 znaki hex **małymi** literami |

Serwer dopasowuje po dowolnym z haszy; w testach poprawny sam `fh` wystarczał
do trafienia.

---

## Endpointy

### `CheckSub2.php` — znajdź i pobierz napisy

Części żądania (wartości zaciemniane oznaczono `*`):

| pole | wartość |
|------|---------|
| `postAction` | `CheckSub` |
| `postVer` | `v1.99.1` |
| `fh`* | hasz OpenSubtitles (hex wielkimi literami) |
| `md`* | MD5 pierwszych 10 MiB (hex małymi literami) |
| `fs`* | rozmiar pliku w bajtach (dziesiętnie) |
| `fn`* | nazwa pliku |
| `nl`* | język napisów, np. `pl` |
| `n24pref` | `1` = preferuj napisy od najlepszych tłumaczy |
| `licz` | `1` |

Odpowiedź (jedna linia + opcjonalne dane binarne):

```
OK-<count>|res:WxH|time:HH:MM:SS|fps:23.976|imdb:<id>|ilea:<n>|ftitle:<title>|
fyear:<year>|fgenres:<...>|fwstawil:<uid>|ftlumacz:<translator>|fkorekta:<proof>|
fcover:<url>|fimdb:<id>|napisId:<id>|lp:<n>|tlId:|tlPrc:|tlInfo:||<ZIP BYTES>
```

* `<count>` = liczba pasujących napisów (`0` = brak trafień).
* Gdy `count > 0`, wszystko po separatorze `||` to archiwum **ZIP**
  (`PK\x03\x04 …`) zawierające najlepiej dopasowany `.srt` oraz skrót
  `Napisy24.pl.url`. Plik `.srt` jest w UTF-8.
* Odpowiedzi inne niż `OK-*` to komunikaty błędów.

### `CheckLogin.php` — weryfikacja danych logowania

| pole | wartość |
|------|---------|
| `postAction` | `Logowanie` |
| `postVer` | `v1.99.1` |
| `login`* | login / e-mail konta |
| `pass`* | hasło konta |

Odpowiedź: `login=ok|<userId>` przy sukcesie (np. `login=ok|1091812` — końcowa
liczba to numeryczne `n24userId` konta), w przeciwnym razie
`login=<polski komunikat błędu>` (np. `login=Blad logowania - wpisz poprawnie
login i haslo!`).

### `CheckSubAgent.php` — pobieranie po haszu, jawne, wspólne konto „agenta”

`POST http://napisy24.pl/run/CheckSubAgent.php`, `application/x-www-form-urlencoded`
(NIE multipart, NIE zaciemniane). To niezaciemniany odpowiednik `CheckSub2.php`
i **nie wymaga konta użytkownika** — używa wspólnego loginu agenta:

| pole | wartość |
|------|---------|
| `postAction` | `CheckSub` |
| `ua` | użytkownik-agent, np. `dmnapi` (alt.: `service.subtitles.napisy24`) |
| `ap` | hasło-agent, np. `4lumen28` (alt.: `eqpXLJLp`) |
| `fh` | hasz OpenSubtitles filmu |
| `md` | MD5 pierwszych 10 MiB filmu (hasz napiprojekt) |
| `fs` | rozmiar pliku |
| `fn` | nazwa pliku |
| `nl` | język, np. `PL` |

Odpowiedź ma ten sam format `OK-<count>|meta…||<zip>` co `CheckSub2.php`, ale
jawny — bez zaciemniania pól. Zweryfikowano na żywym serwerze.

### `libs/webapi.php` — wyszukiwanie napisów po IMDb lub tytule

Prosty, otwarty (bez zaciemniania) endpoint znaleziony w open-source'owej
wtyczce enigma2 **DMnapi** i zweryfikowany na żywo.

```
GET http://napisy24.pl/libs/webapi.php?imdb=ttNNNNNNN
GET http://napisy24.pl/libs/webapi.php?title=<name>    (spacje URL-encode)
```

Wyślij nagłówek `User-Agent` (dowolny), opcjonalnie `Referer: http://napisy24.pl`.
Zwraca **XML**, jeden blok `<subtitle>` na wynik, np.:

```xml
<subtitle>
  <id>132888</id>
  <title>Avatar: Fire and Ash</title>
  <altTitle>Avatar: Ogień i popiół</altTitle>
  <imdb>tt1757678</imdb>
  <year>2025</year>
  <release>2160p.iT.WEB-DL...H.265-BYNDR</release>
  <language>pl</language>
  <cd>1</cd>
  <time>03:17:08|||</time>
  <size>37146286480|||</size>
  <fps>24.000</fps>
  <resolution>3840x2076</resolution>
  <author>Jakub Kowalczyk</author>
  <rating>6.0000</rating>
</subtitle>
```

`<id>` to `napisId` używany do pobrania. (Starsze wersje DMnapi parsowały
prostszy schemat `<id>/<title>/<season>/<episode>/<imdb>/<language>/<size>/<fps>/<year>/<release>`
— zestaw pól może się różnić.) `webapi.php` zwraca maksymalnie ~25 wyników na
zapytanie, więc nie wylistuje całego długiego sezonu naraz.

### `run/pages/download.php?napisId=N` — pobierz napisy po id

```
GET http://napisy24.pl/run/pages/download.php?napisId=132888
```

→ **ZIP** zawierający plik napisów (`.srt` lub MicroDVD `.txt`, tzn.
`[frame][frame]text`) plus skrót `Napisy24.pl.url`. Bierze się największy plik
w archiwum.

### `CheckIMDB.php` — weryfikacja / rozwiązanie identyfikatora IMDb

Pole `imdbId` (**tekst jawny**, format `ttNNNNNNN`, musi mieć 9 znaków).
Zwraca `title|year||ttID|…` dla poprawnego id lub linię zaczynającą się od
`INVALID` w przeciwnym razie. Używane przez ścieżkę uploadu do otagowania filmu
identyfikatorem IMDb — **nie zwraca napisów**.

### `AddSub.php` — upload napisów przez klienta *(ZABLOKOWANY po stronie serwera)*

Pełny format przewodowy przechwycony z prawdziwego klienta Windows. W
przeciwieństwie do `CheckSub2.php` **wszystkie pola są TEKSTEM JAWNYM — żadne
nie jest zaciemniane.** Multipart, części w tej kolejności:

| pole | wartość | uwagi |
|------|---------|-------|
| `postAction` | `AddSub` | |
| `postVer` | `v1.99.1` | |
| `form_dodaj_tytul` | tytuł filmu | Content-Type utf-8 |
| `form_dodajTyp` | `1`=film, `2`=serial | |
| `form_dodajIMDB` | `ttNNNNNNN` | |
| `form_dodaj_polskiTytul` | tytuł polski | |
| `form_dodaj_rok` | rok | |
| `form_dodaj_nrSezonu` / `form_dodaj_nrOdcinka` | sezon / odcinek | `0` dla filmów |
| `form_dodaj_tytulOdcinka` | tytuł odcinka | |
| `form_dodaj_tlumaczenie` / `_dopasowanie` / `_korekta` | tłumacz / synchro / korekta | |
| `form_dodaj_wydanie` | wydanie, np. `1080p` | |
| `form_dodajNapis1_plik` | **sam plik `.srt`** | część plikowa: `filename="…srt"`, `Content-Transfer-Encoding: binary`, treść = SUROWY tekst srt (NIE spakowany). Klient re-koduje UTF-8→us-ascii. |
| `form_dodaj_srt` | `1` | flaga |
| `form_dodaj_jezyk` | `1` | numeryczny id języka (1 = polski) |
| `form_dodajIloscPlyt` | `1` | liczba płyt CD |
| `form_czas_cd1` | `HH:MM:SS` | czas trwania |
| `form_rozdzielczosc_cd1` | `WxH` | rozdzielczość |
| `form_wielkosc_cd1` | rozmiar filmu w bajtach | dziesiętnie |
| `form_fps_cd1` | fps | |
| `form_hash_cd1` | hasz OpenSubtitles filmu | 16 hex WIELKIMI |
| `form_hashsub_cd1` | **hasz napisów** | 16 hex WIELKIMI — patrz niżej |
| `n24userId` | numeryczny id z sukcesu `CheckLogin` (`login=ok\|<id>`) | **bez login/pass** |

Hasz napisów (`form_hashsub_cd1`): jak OSH, ale sumujący CAŁY plik —
`rozmiar_pliku + Σ(wszystkie 8-bajtowe słowa little-endian), mod 2^64`, hex
wielkimi literami. Liczony na re-kodowanych przez klienta bajtach srt.

> **Status — klientowa ścieżka `AddSub.php` jest ZABLOKOWANA po stronie
> serwera.** Nawet oryginalny, najnowszy oficjalny klient Windows, wysyłając to
> dokładne żądanie, dostaje `Problem: Masz Stara wersje programu!`. Ponowne
> wysłanie poprawnego żądania z dowolnym `postVer` (v2.0.0, v3.0.0, v1.100.0, …)
> daje ten sam wynik — to nie jest kontrola numeru wersji; serwer odrzuca
> *wszystkie* uploady klienta tym komunikatem. **Napisy dodaje się zamiast tego
> przez formularz WWW — patrz [Dodawanie napisów przez stronę WWW](#dodawanie-napisów-przez-stronę-www-dodaj-napisy).**

### `ChangeData.php` — powiąż lokalny plik z filmem / zaktualizuj media info

To wysyła przycisk „Wyślij” klienta dla rozpoznanego filmu. Multipart,
wszystkie wartości zaciemniane oprócz `postVer`:

| pole | wartość (odzaciemniona) |
|------|-------------------------|
| `postAction`/`type` | `mediainfo` (wysyłane jako `type`) |
| `postVer` | `v1.99.1` |
| `login`* / `pass`* | dane logowania |
| `data`* | `<napisId>\|<duration>\|<resolution>\|<fps>`, np. `498221902\|00:47:45\|1920x1080\|23.976` |
| `fh`* | hasz OpenSubtitles filmu |
| `md`* | MD5 pierwszych 10 MiB filmu |

Kojarzy hasz pliku filmowego (`fh`/`md`) + techniczne media info z istniejącym
rekordem napisów (`napisId`). **Nie** niesie treści napisów. Odpowiedź `data=ok`
przy sukcesie. Po tym wywołaniu `CheckSub2` zwraca `OK-1` dla hasza, ale z
**pustym ZIP** — to powiązanie hasza, nie nowy plik napisów.

### `GetRelease.php` — lista nazw wydań dla identyfikatora IMDb

To samo żądanie (`imdbId=ttNNNNNNN`). Zwraca listę (rozdzieloną nowymi liniami)
znanych nazw wydań dla tytułu (np. `720p.HDTV.X264-DIMENSION`, `HDTV.XviD-FUM`,
…), używaną do podpowiadania wydania przy uploadzie.

> Endpointy `CheckIMDB`/`GetRelease` jedynie walidują id / listują nazwy wydań
> dla formularza uploadu w kliencie desktopowym. Faktyczne **wyszukiwanie
> napisów po IMDb** realizuje osobne, prostsze web API (`libs/webapi.php`).

### Pozostałe endpointy (zaobserwowane, niezaimplementowane)

`AddSubPrg.php` (kontrola duplikatów przed uploadem, `postAction=Check`),
`ChangeData.php` (edycja/ocena), `SetTrans.php` / `GetTrans.php` (tłumaczenia),
`GetIMDB.php`, `Notifiemail.php` / `NotifiSMS.php` (powiadomienia), `SMS.php`,
`links.php` (aktualizator), `ShowInfo.php` (strona info WWW).

> **Zalecana reimplementacja:** używaj `libs/webapi.php` (wyszukiwanie po
> imdb/tytule) + `download.php?napisId=` (pobieranie) i/lub `CheckSubAgent.php`
> (dopasowanie po haszu). Te ścieżki całkowicie omijają zaciemnianie.
> `napiprojekt.pl` udostępnia też własne API (`api/api-napiprojekt3.php`),
> którego DMnapi używa równolegle.

---

## Dodawanie napisów przez stronę WWW (`/dodaj-napisy`)

Skoro klientowe `AddSub.php` jest zablokowane, napisy dodaje się przez publiczny
**formularz WWW** pod `https://napisy24.pl/dodaj-napisy`. Serwis to Joomla +
Community Builder (logowanie) + **RSForm! Pro** (formularz). Cały przepływ
działa po HTTP w jednej sesji ciasteczek — **bez przeglądarki** — i jest
zaimplementowany w `aqnapi` (upload dla filmów i seriali oraz delete). Wywołania
tutaj to **HTTPS** ze zwykłym przeglądarkowym `User-Agent`.

### 1. Logowanie (Community Builder)

1. `GET https://napisy24.pl/` i wyskrob token CSRF Joomli — input
   `name="<32-hex>" value="1"`.
2. `POST https://napisy24.pl/cb-login` (`application/x-www-form-urlencoded`):

   | pole | wartość |
   |------|---------|
   | `username` | login lub e-mail |
   | `passwd` | hasło |
   | `remember` | `yes` |
   | `option` | `com_comprofiler` |
   | `view` | `login` |
   | `op2` | `login` |
   | `return` | (puste) |
   | `loginfrom` | `loginmodule` |
   | `<32-hex token>` | `1` |

   Sukces, gdy zwrócona strona zawiera `Wyloguj`. Zachowaj ciasteczka.
3. `GET https://napisy24.pl/dodaj-napisy` raz, by ustanowić sesję RSForm.

### 2. Rozwiązanie metadanych (opcjonalne) — `run/pages/tlumaczenia_function.php`

Jawny helper używany przez formularz przez AJAX (`GET`):

* **Film / tytuł i rok:** `?imdbId=ttNNNNNNN&sezon=&odcinek=`
  → `type|title|polishTitle|year|season|episode|episodeTitle|imdbOverride`
  (`type` = `1` film / `2` serial).
* **Rozwiązanie odcinka:** `?sImdb=tt<series>&sezon=<n>&odcinek=<m>`
  → `tt<episode>|<episodeTitle>`. Tylko odcinki już znane napisy24 zwracają
  dedykowany id odcinka (np. S04E01 → `tt30227600`); **nieznane odcinki cofają
  się do `tt<series>|` (id serialu, pusty tytuł)**, więc wtedy podaj tytuł
  odcinka samodzielnie.

### 3. Wysłanie — `POST https://napisy24.pl/dodaj-napisy`

`multipart/form-data`. Nazwy pól to notacja tablicowa RSForm `form[...]`.
Wspólne pola:

| pole | wartość | uwagi |
|------|---------|-------|
| `form[form_typ]` | `Film` lub `Serial` | |
| `form[form_dodajIMDB]` | `ttNNNNNNN` | id filmu lub **id odcinka** dla serialu (z helpera; może być id serialu) |
| `form[form_dodaj_tytul]` | tytuł | **wymagane** (`Wpisz tytuł` gdy puste) |
| `form[form_dodaj_polskiTytul]` | tytuł polski | |
| `form[form_dodaj_rok]` | rok | |
| `form[form_dodaj_wydanie]` | nazwa wydania | **wymagane** (`Nieprawidłowy release`) |
| `form[form_dodaj_hash]` | (puste) | **zostaw PUSTE** — to wewnętrzny „oczekiwany” hasz napisy24, nie OSH/MD5; prawdziwy hasz jest odrzucany jako `Nieprawidłowe dane` |
| `form[form_dodaj_tlumaczenie]` | tłumacz | **wymagane** (`Dodaj informację kto tłumaczył`) |
| `form[form_dodaj_dopasowanie]` / `_korekta` | autor synchro / korekty | opcjonalne |
| `form[form_dodaj_rozdzielczosc]` | `WxH`, np. `1920x1080` | |
| `form[form_dodaj_fps][]` | `23.976` / `24.000` / `25.000` / `29.970` / `inne` | |
| `form[form_dodaj_jezyk][]` | `Polski` / `Angielski` / `Inny` | |
| `form[form_dodajIloscPlyt]` | `1` | liczba płyt CD |
| `form[form_czas_cd1]` | `HH:MM:SS` | **wymagane** (`Nieprawidłowy czas trwania`) |
| `form[form_wielkosc_cd1]` | rozmiar filmu w bajtach | **wymagane** (`Nieprawidłowa wielkość`) |
| `form[form_dodajNapis1_plik]` | **plik `.srt`** (część plikowa) | patrz reguły pliku poniżej |
| `form[dodajTlumaczenie]` | `Dodaj` | przycisk wysyłki |
| `form[formId]` | `7` | id formularza RSForm |
| `form[remId]` | (puste) | |
| `form[form_dodajTlumaczenieId]` | `0` | |

Dla **serialu** (`form[form_typ]=Serial`) wyślij dodatkowo:

| pole | wartość |
|------|---------|
| `form[realtxt]` | tytuł serialu (tekst wyszukiwania) |
| `form[serial][]` | `tt<series>` — musi być istniejącą wartością opcji w selekcie `serial` formularza |
| `form[form_dodaj_nrSezonu]` | numer sezonu |
| `form[form_dodaj_nrOdcinka]` | numer odcinka |
| `form[form_dodaj_tytulOdcinka]` | tytuł odcinka |
| `form[form_dodaj_cover]` | `Serialu` |

Formularz deklaruje 4 sloty plikowe; wyślij `form[form_dodajNapis1_plik]` jako
prawdziwy plik, a `form[form_dodajNapis2_plik]`…`4_plik` jako puste części
plikowe.

**Sukces:** odpowiedź zawiera `Napisy Dodane/Zmienione - dziękujemy`, a końcowy
URL to `…/dodaj-napisy?skipcache=rsform<hex>`.

### Reguły pliku napisów (walidacja po stronie serwera)

napisy24 waliduje treść `.srt` i odrzuca ją (jako `Nieprawidłowy plik`), jeśli
nie są spełnione warunki:

* **Końce linii to CRLF (`\r\n`).** Plik z samym LF sprawia, że walidator źle
  liczy dialogi i odrzuca go z *„Następujące linie posiadaja więcej niż 2
  sublinie: …”* — to **fałszywy** raport wynikający wyłącznie z końców linii (to
  **nie** kwestia BOM UTF-8). W `aqnapi` `normalize_for_napisy24()` wymusza CRLF.
* **Najwyżej 2 linie tekstu na dialog.** Więcej → *„…więcej niż 2 sublinie”*
  (prawdziwy błąd).
* **Brak nakładających się czasów.** Nakładka → *„Napisy posiadają nachodzące na
  siebie linijki: N”*. Opcja `--fix-timing` przycina koniec dialogu do początku
  następnego.

> ⚠️ **`ajaxValidate` zapisuje wpis.** RSForm udostępnia
> `POST /component/rsform/?task=ajaxValidate` (multipart, te same pola), które
> zwraca *szczegółowy* wynik walidacji — JSON `{"formComponents":[…]}` gdy
> poprawny, albo jawny polski powód gdy nie. **Ale wywołanie go faktycznie
> tworzy wpis napisów nawet w trybie „walidacji”**, więc NIE wolno go używać
> jako suchego pre-checka (podwoiłby każdy upload). Dlatego `aqnapi` waliduje
> plik **lokalnie** (`check_srt_for_napisy24`) i wysyła tylko raz, przez
> `/dodaj-napisy`.

### Edycja / usunięcie własnych napisów

* **Formularz edycji:** `GET https://napisy24.pl/dodaj-napisy?edytuj=<napisId>` —
  ten sam formularz, wypełniony rekordem (dodaje prawe menu
  Podgląd / Edytuj / Wgraj poprawki / Uaktualnij / Usuń).
* **Usunięcie:** `GET`, potem `POST https://napisy24.pl/dodaj-napisy?usun=<napisId>`
  (`application/x-www-form-urlencoded`):

  | pole | wartość |
  |------|---------|
  | `form[form_usunPowod]` | powód usunięcia (tekst) |
  | `form[btnSend]` | `Usuń napisy` |
  | `form[usunId]` | `<napisId>` |
  | `form[formId]` | `8` |

  Strona sukcesu zawiera `Napisy usunięte`. (W praktyce usuwanie bywa
  zawodne — traktuj jako best-effort i najpierw dopracuj poprawność uploadu.)

> Publiczna strona szczegółów napisów to
> `index.php?option=com_content&napisId=<id>`, dostępna też z wyników
> wyszukiwania (rozwiń wiersz wyniku → **Moderuj** otwiera formularz edycji).

---

## Komendy aqnapi

`aqnapi` mapuje powyższy protokół na komendy CLI. Podpolecenia specyficzne dla
serwisu żyją pod `aqnapi napisy24 …` (alias: `aqnapi n24 …`). Poniżej mapowanie
komenda → endpoint/ścieżka protokołu.

### Dane logowania

Dane konta są pobierane w kolejności priorytetu:

1. **Flagi:** `--n24-user`, `--n24-pass`.
2. **Zmienne środowiskowe:** `NAPI24_LOGIN`, `NAPI24_PASS`.
3. **Plik konfiguracyjny:** `~/.config/aqnapi/config.ini`, sekcja `[napisy24]`,
   klucze `login` / `pass`.

```ini
[napisy24]
login = twoj_login
pass  = twoje_haslo
```

Uwaga o wynikach: pobrane napisy są zawsze konwertowane do **SRT w UTF-8 z BOM,
z końcami linii LF**. (Wymóg CRLF dotyczy wyłącznie ścieżki uploadu do napisy24
i jest tam wymuszany osobno.)

### `aqnapi napisy24 hash PLIK`

Wypisuje oba hasze pliku filmowego: **OSH** (hasz OpenSubtitles/MPC, pole `fh`)
oraz **MD5 pierwszych 10 MiB** (hasz napiprojekt, pole `md`). Nie łączy się z
serwerem. Odpowiada sekcji [Hasze filmu](#hasze-filmu).

### `aqnapi napisy24 login`

Weryfikuje dane logowania przez **`CheckLogin.php`** (`postAction=Logowanie`,
zaciemnione `login`/`pass`). Dane pobierane z flag/env/config jak wyżej.

### `aqnapi napisy24 download PLIK [-l pl] [-o out.srt]`

Pobiera napisy dopasowane po haszu przez **`CheckSubAgent.php`** — używa
wspólnego konta agenta, **bez logowania** i bez zaciemniania. `aqnapi` liczy
`fh`/`md`/`fs`/`fn` z pliku, rozpakowuje ZIP z odpowiedzi i zapisuje wynik
(`-o`, domyślnie obok pliku) jako SRT UTF-8-BOM/LF. `-l` ustawia język (domyślnie
`pl`).

### `aqnapi napisy24 search --imdb ttNNNNNNN | --title "..."`

Wyszukuje napisy przez **`libs/webapi.php`** (po IMDb lub tytule). Parsuje
zwrócony XML `<subtitle>` i wypisuje trafienia wraz z `id` (`napisId`) do
późniejszego pobrania przez `getid`.

### `aqnapi napisy24 getid ID [-o out.srt]`

Pobiera napisy po identyfikatorze przez **`download.php?napisId=ID`**. Bierze
największy plik z ZIP i zapisuje jako SRT UTF-8-BOM/LF.

### `aqnapi napisy24 imdb ttNNNNNNN`

Waliduje / rozwiązuje identyfikator IMDb przez **`CheckIMDB.php`** (pole
`imdbId`, tekst jawny). Zwraca tytuł/rok; nie zwraca napisów.

### `aqnapi napisy24 upload --srt PLIK.srt --movie FILM [opcje]`

Wysyła napisy przez **formularz WWW `/dodaj-napisy`** (logowanie Community
Builder + POST RSForm), zgodnie z sekcją
[Dodawanie napisów przez stronę WWW](#dodawanie-napisów-przez-stronę-www-dodaj-napisy).
Wymaga loginu i hasła. Przed wysłaniem `aqnapi` waliduje plik **lokalnie**
(`check_srt_for_napisy24`) i normalizuje go do CRLF (`normalize_for_napisy24`).

Opcje metadanych (mapowane na pola `form[...]`):

* `--imdb ttNNNNNNN` — `form[form_dodajIMDB]`
* `--title "..."` / `--title-pl "..."` — tytuł / tytuł polski
* `--year`, `--release` — rok, nazwa wydania
* `--translator`, `--sync`, `--proof` — tłumacz / synchro / korekta
* `--resolution WxH` — `form[form_dodaj_rozdzielczosc]`
* `--duration HH:MM:SS` — `form[form_czas_cd1]`
* `--size`, `--fps` — rozmiar filmu w bajtach, fps
* `--season N` / `--episode M` / `--episode-title "..."` — dla seriali
  (przełącza `form[form_typ]` na `Serial`)
* `-l/--lang` (domyślnie `PL`), `--corrected`, `--comment`
* `--fix-timing` — przytnij nakładające się czasy dialogów
* `--dry-run` — tylko waliduj plik lokalnie, nic nie wysyłaj

### `aqnapi napisy24 delete ID`

Usuwa własne napisy: `POST /dodaj-napisy?usun=<ID>` (pola `form[form_usunPowod]`,
`form[btnSend]=Usuń napisy`, `form[usunId]`, `form[formId]=8`). Wymaga logowania.
Usuwanie bywa zawodne — traktuj jako best-effort.

### Komendy zbiorcze (wiele serwisów naraz)

`aqnapi` udostępnia też polecenia agregujące wiele serwisów (napiprojekt / n24 /
opensubtitles); dla Napisy24 użyj przełącznika `--service n24`:

* `aqnapi get FILM --service n24` — pobierz napisy po haszu (przez
  `CheckSubAgent.php`, konto agenta, bez logowania).
* `aqnapi search --service n24 [--imdb … | --title …]` — wyszukiwanie przez
  `libs/webapi.php`.
* `aqnapi upload --service n24 [opcje jak wyżej]` — upload przez formularz
  `/dodaj-napisy`.
