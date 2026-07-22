#!/bin/sh
# Podbij wersję, zacommituj i utwórz tag wydania.
#   scripts/bump.sh 1.1.0
# Następnie:  git push origin main --tags   (uruchomi workflow Release)
set -eu

[ $# -eq 1 ] || { echo "użycie: scripts/bump.sh X.Y.Z"; exit 1; }
v="$1"
case "$v" in
  [0-9]*.[0-9]*.[0-9]*) : ;;
  *) echo "Wersja musi być w formacie X.Y.Z (np. 1.1.0)"; exit 1 ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

sed -i -E "s/^__version__ = \".*\"/__version__ = \"$v\"/" aqnapi.py
new=$(grep -E '^__version__' aqnapi.py)
echo "Ustawiono: $new"

# szybka walidacja składni
python3 -c "import ast; ast.parse(open('aqnapi.py').read())"

git add aqnapi.py
git commit -m "chore: release v$v"
git tag -a "v$v" -m "aqnapi v$v"
echo
echo "Utworzono commit i tag v$v."
echo "Wypchnij:  git push origin main --tags"
