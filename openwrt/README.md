# aqnapi — pakiet OpenWrt

Formuła (`Makefile`) budująca **natywny** pakiet aqnapi na OpenWrt. Ta sama
definicja pakietu produkuje **`.ipk`** albo **`.apk`** — zależnie od wersji
OpenWrt / systemu pakietów (patrz [ipk vs apk](#ipk-vs-apk)).

Pakiet kompiluje jednoplikowe źródło C (`c/aqnapi.c`) toolchainem docelowym i
instaluje binarkę do `/usr/bin/aqnapi`. Bez interpretera, bez zależności
runtime poza `zlib` (i `libmbedtls` przy włączonym TLS).

```
openwrt/
  aqnapi/
    Makefile      # formuła pakietu OpenWrt
  README.md       # ten plik
```

## ipk vs apk

**Nie budujesz osobno** — o formacie decyduje drzewo OpenWrt, nie ta formuła:

| Wersja OpenWrt | Format | Menedżer |
|---|---|---|
| ≤ 23.05 oraz 24.10 (domyślnie) | **`.ipk`** | `opkg` |
| snapshot / z `CONFIG_USE_APK=y` | **`.apk`** | `apk` |

Ten sam `make package/aqnapi/compile` wygeneruje pakiet w formacie, którego używa
Twoje SDK. Chcesz konkretny format — wybierz odpowiedni **SDK** (patrz niżej) lub
przełącz `Global build settings → Use apk package manager` w `menuconfig`.

## Budowanie przez OpenWrt SDK (zalecane, szybkie)

Nie trzeba budować całego systemu — SDK dla Twojego targetu wystarczy.

1. **Pobierz SDK** dla swojego routera z https://downloads.openwrt.org
   (np. `…/targets/ath79/generic/openwrt-sdk-*-ath79-generic_*.Linux-x86_64.tar.zst`).
   Dla `.apk` weź SDK ze snapshotu lub 24.10 z apk.

   ```sh
   tar --zstd -xf openwrt-sdk-*.tar.zst
   cd openwrt-sdk-*/
   ```

2. **Dodaj pakiet.** Skopiuj katalog formuły do drzewa SDK:

   ```sh
   mkdir -p package/aqnapi
   cp /ścieżka/do/aqnapi/openwrt/aqnapi/Makefile package/aqnapi/
   ```

   (Alternatywnie własny feed: dopisz `src-link aqnapi /ścieżka/do/aqnapi/openwrt`
   do `feeds.conf`, potem `./scripts/feeds update aqnapi && ./scripts/feeds install aqnapi`.)

3. **Zależności feeds** (mbedtls/zlib są w `base`/`packages`):

   ```sh
   ./scripts/feeds update -a
   ./scripts/feeds install libmbedtls zlib ca-bundle
   ```

4. **Konfiguracja i build:**

   ```sh
   make defconfig
   make menuconfig     # opcjonalnie: Multimedia → aqnapi  (M = pakiet)
                       #   pod aqnapi: [*] Enable HTTPS/TLS (mbedtls)
   make package/aqnapi/compile V=s
   ```

5. **Wynik** (ipk lub apk):

   ```sh
   find bin/ -name 'aqnapi_*'
   #  bin/packages/<arch>/base/aqnapi_1.0.14-1_<arch>.ipk        (ipk)
   #  bin/packages/<arch>/base/aqnapi-1.0.14-r1.apk              (apk)
   ```

## Budowanie w pełnym buildroot

Identycznie, tylko w drzewie źródeł OpenWrt: umieść formułę w
`package/aqnapi/Makefile` (lub przez własny feed), a następnie
`make menuconfig` → zaznacz `Multimedia → aqnapi`, `make`. Pakiet trafi do
`bin/packages/<arch>/…`.

## Instalacja na routerze

Skopiuj pakiet na router i zainstaluj:

```sh
# ipk (opkg)
scp aqnapi_1.0.14-1_<arch>.ipk root@router:/tmp/
ssh root@router 'opkg install /tmp/aqnapi_1.0.14-1_<arch>.ipk'

# apk
scp aqnapi-1.0.14-r1.apk root@router:/tmp/
ssh root@router 'apk add --allow-untrusted /tmp/aqnapi-1.0.14-r1.apk'
```

Zależności (`zlib`, a przy TLS `libmbedtls`, `ca-bundle`) `opkg`/`apk` dociągnie
same, jeśli masz skonfigurowane repozytoria; w razie potrzeby doinstaluj ręcznie:

```sh
opkg update && opkg install zlib libmbedtls ca-bundle
```

## TLS / zależności

- **`zlib`** — zawsze (rozpakowanie archiwów ZIP z napisy24).
- **`libmbedtls`** + **`ca-bundle`** — tylko gdy w `menuconfig` zaznaczono
  **Enable HTTPS/TLS (mbedtls)** (domyślnie tak). Daje HTTPS: OpenSubtitles,
  napisy24 WWW/`edit`/`delete`, `update` oraz adresy `https://`.

`ca-bundle` instaluje `/etc/ssl/certs/ca-certificates.crt` — aqnapi weryfikuje
łańcuch CA i sam znajdzie ten plik (albo wskaż inny przez `SSL_CERT_FILE`).

Bez TLS pakiet jest mniejszy i nie ciągnie mbedtls; dostępne są operacje po
HTTP: `download`/`get`/`search`, całe `napiprojekt`
(download/upload/attach/cover/version/report/account/associate) oraz większość
`napisy24` (download/attach/login/imdb/mediainfo/notify/trans/premieres) i cały
tryb offline (convert/hash/fps/merge/split/sync/config).

## Uruchomienie

```sh
aqnapi --help
aqnapi hash /mnt/usb/film.mkv
aqnapi get /mnt/usb/film.mkv -l pl        # pobierze .srt obok pliku
```

Poświadczenia (napisy24 / napiprojekt / OpenSubtitles) trzymaj w
`~/.config/aqnapi/config.ini` na routerze (`aqnapi config init`) lub w zmiennych
środowiskowych (`NAPI24_LOGIN`, `NAPI_USER`, `OS_API_KEY`, …).

## Uwagi

- **Wersja.** `PKG_VERSION` w `Makefile` wskazuje tag `vX.Y.Z` repozytorium
  (klonowany przez `git`). Aktualizując, zmień `PKG_VERSION` (i `PKG_RELEASE`
  przy poprawkach samej formuły).
- **Hash / odtwarzalność.** Domyślnie `PKG_MIRROR_HASH:=skip` (build pobiera
  źródło z GitHuba bez weryfikacji sumy). Dla powtarzalnych buildów: uruchom
  `make package/aqnapi/download V=s`, policz `sha256sum dl/aqnapi-*.tar.*` i wpisz
  wynik w miejsce `skip`.
- **Build lokalny (bez GitHuba).** Jeśli chcesz kompilować z lokalnej kopii
  repo, użyj feeda `src-link` (krok 2, alternatywa) albo w `Makefile` zamień
  `PKG_SOURCE_PROTO:=git`/`PKG_SOURCE_URL` na lokalne źródło.
- **Architektura.** Powstaje zwykły ELF pod jeden target (mips/arm/…), inaczej
  niż uniwersalne APE z cosmo — dlatego budujesz per-SDK. Wyjście jest bajtowo
  zgodne z wersją cosmo i referencyjną wersją Python.
