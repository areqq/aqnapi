#!/bin/sh
# Buduje natywny POC w C do uniwersalnej binarki APE przez cosmocc.
#   c/build.sh   ->  dist/aqnapi-c.com
# Pobiera toolchain cosmocc do c/toolchain/ przy pierwszym uruchomieniu.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tc="$here/toolchain"
cc="$tc/bin/cosmocc"
out="$root/dist/aqnapi-c.com"
url="${COSMOCC_URL:-https://cosmo.zip/pub/cosmocc/cosmocc.zip}"

command -v unzip >/dev/null 2>&1 || { echo "Brak 'unzip'"; exit 1; }

if [ ! -x "$cc" ]; then
  echo "Pobieram toolchain cosmocc z $url ..."
  mkdir -p "$tc"
  if command -v curl >/dev/null 2>&1; then curl -fL# -o "$tc/cosmocc.zip" "$url";
  elif command -v wget >/dev/null 2>&1; then wget -O "$tc/cosmocc.zip" "$url";
  else echo "Brak curl/wget"; exit 1; fi
  ( cd "$tc" && unzip -q -o cosmocc.zip && rm -f cosmocc.zip )
fi

mkdir -p "$root/dist"
"$cc" -O2 -Wall -o "$out" "$here/aqnapi.c"
chmod +x "$out"
echo "Gotowe: $out ($(wc -c < "$out") B)"
"$out" --version
