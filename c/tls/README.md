# TLS w wersji C — jak redbean (Cosmopolitan + MbedTLS)

Prekompilowany `cosmocc` **nie zawiera** TLS. redbean (i każdy program TLS w
Cosmopolitan) buduje się **wewnątrz monorepo `jart/cosmopolitan`** i linkuje
`third_party/mbedtls` (fork MbedTLS 2.26 zoptymalizowany przez jart), z
certyfikatami CA w `usr/share/ssl/root/`. Ta sama droga działa dla aqnapi.

## Dowód (zweryfikowany na żywo)

`https_client.c` w tym katalogu to minimalny klient HTTPS na cosmo-mbedtls.
Zbudowany jako APE i uruchomiony:

```
TLS OK: TLSv1.2 / ECDHE-ECDSA-AES128-GCM-SHA256
HTTP: HTTP/1.1 403 Forbidden          # api.opensubtitles.com (bez Api-Key)
TLS OK: TLSv1.2 / ECDHE-RSA-AES128-GCM-SHA256
HTTP: HTTP/1.1 200 OK                 # napisy24.pl
```

Czyli TLS 1.2 + pełne żądanie/odpowiedź HTTPS działają w jednej binarce APE.

## Przepis budowania (odtwarzalny)

```sh
# 1. źródła monorepo (43 MB)
curl -L -o cosmo.tar.gz \
  https://github.com/jart/cosmopolitan/releases/download/4.0.2/cosmopolitan-4.0.2.tar.gz
tar xzf cosmo.tar.gz && cd cosmopolitan-4.0.2

# 2. zbuduj bibliotekę MbedTLS (pobierze cosmocc 3.9.2 automatycznie)
make -j4 o//third_party/mbedtls/mbedtls.a

# 3. najprościej: wrzuć program do examples/ (linkuje MBEDTLS + NET_HTTPS)
cp .../https_client.c examples/aqtls.c
make -j4 o//examples/aqtls        # -> APE z TLS
./o/examples/aqtls                # live handshake
```

> `examples/` ciągnie szerokie zależności (libcxx/DSP) — wolno. Docelowo lepiej
> zrobić **własny minimalny pakiet** `BUILD.mk` z zależnościami tylko:
> `LIBC_*`, `NET_HTTP`, `NET_HTTPS`, `THIRD_PARTY_MBEDTLS`, `THIRD_PARTY_ZLIB`
> (wzorzec: `examples/BUILD.mk` + `include` w głównym `Makefile`).

## Uwagi

- Nagłówki cosmo-mbedtls używają makr wewnętrznych monorepo (`IsModeDbg()`,
  `UNLIKELY`, `unassert`, `bool` z `libc/integral/normalize.inc`) w dyrektywach
  preprocesora — **nie kompilują się** zwykłym cosmocc. Dlatego kod TLS musi być
  budowany w środowisku monorepo (flagi `-D_COSMO_SOURCE -nostdinc -isystem
  libc/isystem -include libc/integral/normalize.inc …`).
- Do produkcyjnego klienta: włączyć weryfikację CA
  (`mbedtls_ssl_conf_authmode(VERIFY_REQUIRED)` + `mbedtls_x509_crt_parse` z
  osadzonym `cacert.pem` lub certami z `usr/share/ssl/root/`). W dowodzie użyto
  `VERIFY_NONE` wyłącznie do pokazania handshake.

## Co dalej (integracja pełna)

Na tym fundamencie: klient OpenSubtitles REST (login → JWT, `/subtitles`,
`/download` — JSON) oraz logowanie/upload/delete WWW napisy24. Wymaga:
zbudowania aqnapi jako targetu monorepo z MbedTLS, parsera JSON, weryfikacji CA.
Weryfikacja OpenSubtitles wymaga **klucza API użytkownika** (Api-Key).
