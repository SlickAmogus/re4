#!/usr/bin/env bash
# Assembles a folder to FTP to the Xbox: default.xbe + the converted asset tree.
#
#   bash xbox/package_xbox.sh <converted dir> <out dir>
#   e.g. bash xbox/package_xbox.sh /c/Claude/RE4-Xbox/gamedata/converted /c/Claude/RE4-Xbox/dist/RE4
#
# Layout on the Xbox (any folder, e.g. E:\Games\RE4\):
#   default.xbe
#   data/files/...          converted disc 1 + disc 2 (re4conv)
#   data/files_disc2/...    disc-2 copies of files that differ between the discs
# The game reads data from <xbe folder>\data (port/xbox_main.cpp re4_data_dir, D: = the XBE's folder).
# The log is written to <xbe folder>\re4.log (or E:\re4\re4.log when the XBE folder is read-only).
set -e
SRC="$1"; OUT="$2"
[ -n "$SRC" ] && [ -n "$OUT" ] || { sed -n 2,12p "$0"; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$HERE/bin/default.xbe" ] || { echo "build first: bash xbox/build_xbox.sh RE4_GAME=1"; exit 1; }
mkdir -p "$OUT/data"
cp "$HERE/bin/default.xbe" "$OUT/"
cp -r "$SRC/files" "$OUT/data/"
[ -d "$SRC/files_disc2" ] && cp -r "$SRC/files_disc2" "$OUT/data/"
du -sh "$OUT"
echo "ready: $OUT (copy the folder to the Xbox, launch default.xbe, then fetch re4.log)"
