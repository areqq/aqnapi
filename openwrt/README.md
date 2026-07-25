# aqnapi — pakiet OpenWrt

Formuła (`Makefile`) budująca **natywny** pakiet aqnapi na OpenWrt. Ta sama
definicja pakietu produkuje **`.ipk`** albo **`.apk`** — zależnie od wersji
OpenWrt / systemu pakietów (patrz [ipk vs apk](#ipk-vs-apk)).

Pakiet kompiluje jednoplikowe źródło C (`c/aqnapi.c`) **z lokalnego checkoutu
repo** (bez pobierania z sieci ani sum kontrolnych) i instaluje binarkę do
`/usr/bin/aqnapi`. Bez interpretera, bez zależności runtime poza `zlib`
(i `libmbedtls` przy włączonym TLS).

```
openwrt/
  aqnapi/Makefile   # formuła pakietu OpenWrt (buduje c/aqnapi.c z tego repo)
  README.md         # ten plik
```

> Formuła buduje **z sąsiedniego `c/aqnapi.c`** (ścieżka `../../c` względem
> `Makefile`). Dlatego pakiet dodaje się do OpenWrt jako **feed `src-link`**
> wskazujący na `openwrt/`, a nie przez skopiowanie samego `Makefile`.

## ipk vs apk

**Nie budujesz osobno** — o formacie decyduje drzewo OpenWrt, nie ta formuła:

| Wersja OpenWrt | Format | Menedżer |
|---|---|---|
| ≤ 23.05 oraz 24.10 (domyślnie) | **`.ipk`** | `opkg` |
| snapshot / z `CONFIG_USE_APK=y` | **`.apk`** | `apk` |

To samo `make package/aqnapi/compile` wygeneruje pakiet w formacie, którego używa
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

2. **Dodaj repo jako feed** `src-link` (wskazujący katalog `openwrt/` tego repo):

   ```sh
   echo "src-link aqnapi /ścieżka/do/aqnapi/openwrt" >> feeds.conf.default
   ./scripts/feeds update aqnapi
   ./scripts/feeds install aqnapi
   ```

3. **Zależności** (mbedtls/zlib/ca-bundle są w feedzie `base`/`packages`):

   ```sh
   ./scripts/feeds update base packages
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
   find bin/ -name 'aqnapi[-_]*'
   #  bin/packages/<arch>/aqnapi/aqnapi_1.0.15-1_<arch>.ipk        (ipk)
   #  bin/packages/<arch>/aqnapi/aqnapi-1.0.15-r1.apk              (apk)
   ```

## Instalacja na routerze

```sh
# ipk (opkg)
scp aqnapi_1.0.15-1_<arch>.ipk root@router:/tmp/
ssh root@router 'opkg install /tmp/aqnapi_1.0.15-1_<arch>.ipk'

# apk
scp aqnapi-1.0.15-r1.apk root@router:/tmp/
ssh root@router 'apk add --allow-untrusted /tmp/aqnapi-1.0.15-r1.apk'
```

Zależności (`zlib`, a przy TLS `libmbedtls`, `ca-bundle`) `opkg`/`apk` dociągnie
same, jeśli masz skonfigurowane repozytoria; w razie potrzeby doinstaluj ręcznie:

```sh
opkg update && opkg install zlib libmbedtls ca-bundle
```

## Gotowe pakiety z wydań

Każde wydanie (`vX.Y.Z`) buduje pakiety automatycznie (GitHub Actions,
`openwrt/gh-action-sdk`) i dołącza je do
[Releases](https://github.com/areqq/aqnapi/releases) dla kilku architektur:
`x86_64`, `aarch64_cortex-a53`, `mipsel_24kc` (ramips/mt7621), `mips_24kc`
(ath79) jako `.ipk` (23.05) oraz `x86_64` jako `.apk` (snapshot). Możesz je
pobrać zamiast budować samodzielnie.

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

- **Bez pobierania.** Formuła kompiluje `c/aqnapi.c` z tego samego checkoutu
  (`AQNAPI_SRC := $(realpath …/../../c)`) — brak `PKG_SOURCE`, sum kontrolnych
  ani zależności od sieci. Dlatego wymagany jest feed `src-link` (a w CI cały
  repo jest montowany jako feed).
- **Wersja.** `PKG_VERSION` w `Makefile` to etykieta pakietu; trzymaj ją zsynchro-
  nizowaną z `__version__`/`#define VERSION` (workflow ustawia ją z tagu).
- **Architektura.** Powstaje zwykły ELF pod jeden target (mips/arm/…), inaczej
  niż uniwersalne APE z cosmo — dlatego budujesz per-SDK. Wyjście jest bajtowo
  zgodne z wersją cosmo i referencyjną wersją Python.
