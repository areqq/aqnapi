#!/bin/sh
# Build natywny aqnapi (wersja C) STANDARDOWYM kompilatorem — bez Cosmopolitan.
# Produkuje zwykłą binarkę ELF dla bieżącej (lub docelowej) platformy.
#
# Domyślnie build BAZOWY: offline + sieć po HTTP (download/get/search, napiprojekt
# download/upload/attach/cover/version/report/account/associate, napisy24
# download/attach/login/imdb/mediainfo/notify/trans/premieres). Bez HTTPS.
#
# Z TLS (opensubtitles, napisy24 WWW/upload/delete/edit, update, URL https://,
# weblogin) — ustaw AQNAPI_TLS=1; wymaga systemowego mbedtls (nagłówki + biblioteki).
#
# Zależności:
#   - zlib   (rozpakowanie ZIP napisy24)      -> -lz
#   - mbedtls (tylko przy AQNAPI_TLS=1)        -> -lmbedtls -lmbedx509 -lmbedcrypto
#
# Przykłady:
#   ./c/build-native.sh                        # host gcc, bazowy
#   AQNAPI_TLS=1 ./c/build-native.sh           # host gcc, z TLS (systemowy mbedtls)
#
#   # Cross-compile pod OpenWrt (SDK toolchain):
#   . "$SDK/setup"     # ustawia STAGING_DIR itd.  (lub wskaż ręcznie)
#   CC="$SDK/staging_dir/toolchain-*/bin/mipsel-openwrt-linux-musl-gcc" \
#   CFLAGS="-O2 -I$STAGING_DIR/usr/include" \
#   LDFLAGS="-L$STAGING_DIR/usr/lib" \
#   AQNAPI_TLS=1 OUT=aqnapi-owrt ./c/build-native.sh
#   # na routerze: opkg install zlib libmbedtls
set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
OUT=${OUT:-dist/aqnapi-c-native}
CFLAGS=${CFLAGS:--O2}
DEFS=""
LIBS="-lz -lm"
if [ "${AQNAPI_TLS:-0}" = "1" ]; then
    DEFS="-DAQNAPI_TLS"
    LIBS="-lmbedtls -lmbedx509 -lmbedcrypto $LIBS"
fi
mkdir -p "$(dirname -- "$OUT")"
echo "CC=$CC  DEFS=$DEFS  LIBS=$LIBS  OUT=$OUT"
# shellcheck disable=SC2086
$CC $CFLAGS $DEFS -o "$OUT" "$here/aqnapi.c" $LDFLAGS $LIBS
echo "Gotowe: $OUT"
