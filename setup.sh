#!/bin/sh
# Sets up a MultiMon build tree: pret/pokeheartgold at the base commit, MultiMon's changes applied,
# and the PC port (pc/) added. Usage: ./setup.sh [directory]   (default: ./pokeheartgold)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-pokeheartgold}
BASE=$(cat "$HERE/patches/BASE_COMMIT")
git clone https://github.com/pret/pokeheartgold.git "$DIR"
cd "$DIR"
git checkout -q "$BASE"
git apply "$HERE/patches/pokeheartgold.patch"
cp -r "$HERE/pc" .
echo "Ready in $DIR. Next: build the DS version once (pret's INSTALL.md), then: python3 pc/tools/pc_build.py"
