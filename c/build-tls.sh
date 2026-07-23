#!/bin/sh
# Buduje wariant C z TLS (HTTPS: update + OpenSubtitles) — jak redbean:
# w monorepo Cosmopolitan, linkując third_party/mbedtls. Wynik: dist/aqnapi-c-tls.com
#
# Pierwsze uruchomienie jest CIĘŻKIE (pobiera cosmocc ~392 MB + źródła 43 MB,
# kompiluje mbedtls i zależności examples). Kolejne są szybkie (cache o/).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
mono="$here/mono/cosmopolitan-4.0.2"
ver=4.0.2

command -v tar >/dev/null && command -v curl >/dev/null || { echo "wymagane: curl, tar"; exit 1; }

if [ ! -d "$mono" ]; then
  echo "Pobieram źródła monorepo Cosmopolitan $ver ..."
  mkdir -p "$here/mono"
  curl -fL# -o "$here/mono/cosmo.tar.gz" \
    "https://github.com/jart/cosmopolitan/releases/download/$ver/cosmopolitan-$ver.tar.gz"
  ( cd "$here/mono" && tar xzf cosmo.tar.gz && rm -f cosmo.tar.gz )
fi

cd "$mono"
# 1) biblioteka mbedtls (pobierze cosmocc automatycznie przy pierwszym razie)
make -j"$(nproc)" o//third_party/mbedtls/mbedtls.a

# 2) wrzuć aqnapi.c jako przykład z -DAQNAPI_TLS (deps examples zawierają mbedtls)
cp "$here/aqnapi.c" examples/aqnapic.c
grep -q 'examples/aqnapic.o' examples/BUILD.mk || \
  printf '\no/$(MODE)/examples/aqnapic.o: private CPPFLAGS += -DAQNAPI_TLS -Wno-error\n' >> examples/BUILD.mk

# 3) zbuduj APE z TLS
make -j"$(nproc)" o//examples/aqnapic

mkdir -p "$root/dist"
cp o/examples/aqnapic "$root/dist/aqnapi-c-tls.com"
# wbuduj bundle CA do APE (zipos /zip/cacert.pem) — weryfikacja certyfikatów TLS
if command -v zip >/dev/null 2>&1 && [ -f "$here/cacert.pem" ]; then
  cp "$here/cacert.pem" "$root/dist/cacert.pem"
  ( cd "$root/dist" && zip -qj aqnapi-c-tls.com cacert.pem && rm -f cacert.pem )
fi
chmod +x "$root/dist/aqnapi-c-tls.com"
echo "Gotowe: $root/dist/aqnapi-c-tls.com ($(wc -c < "$root/dist/aqnapi-c-tls.com") B)"
"$root/dist/aqnapi-c-tls.com" --version
