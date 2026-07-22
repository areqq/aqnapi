#!/bin/sh
# Buduje uniwersalną binarkę APE `aqnapi.com` (Cosmopolitan) z aqnapi.py.
#
# Wynik to jeden plik działający na Linux / macOS / Windows / *BSD, x86-64+ARM64
# (Actually Portable Executable). Uruchamia DOKŁADNIE ten sam aqnapi.py co wersja
# CPython, więc jest w 100% zgodny — aqnapi.py pozostaje jedynym źródłem prawdy.
#
# Użycie:
#   cosmo/build.sh                 # zbuduj dist/aqnapi.com
#   PYTHON_APE_URL=... cosmo/build.sh   # własny URL interpretera APE
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
dist="$root/dist"
cache="$here/python.ape"                       # zbuforowany interpreter APE
url="${PYTHON_APE_URL:-https://cosmo.zip/pub/cosmos/bin/python}"
out="$dist/aqnapi.com"

command -v zip >/dev/null 2>&1 || { echo "Brak 'zip' (apt install zip)"; exit 1; }

mkdir -p "$dist"

if [ ! -f "$cache" ]; then
  echo "Pobieram Actually Portable Python z $url ..."
  if command -v curl >/dev/null 2>&1; then
    curl -fL# -o "$cache" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$cache" "$url"
  else
    echo "Brak curl/wget do pobrania interpretera."; exit 1
  fi
fi

cp "$cache" "$out"

# Wbuduj aqnapi.py oraz plik .args (auto-uruchomienie skryptu, przekazanie argv).
work=$(mktemp -d)
cp "$root/aqnapi.py" "$work/aqnapi.py"
# .args: uruchom /zip/aqnapi.py, a '...' rozwija się do argumentów użytkownika.
printf '%s\n' '/zip/aqnapi.py' '...' > "$work/.args"
( cd "$work" && zip -q "$out" aqnapi.py .args )
rm -rf "$work"
chmod +x "$out"

size=$(wc -c < "$out")
echo "Gotowe: $out ($size B)"
