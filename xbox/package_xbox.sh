#!/usr/bin/env bash
# Assembles what to copy to the Xbox: default.xbe + the game data.
#
#   bash xbox/package_xbox.sh <converted dir> <out dir> [--split MB] [--loose]
#   e.g. bash xbox/package_xbox.sh /c/Claude/RE4-Xbox/gamedata/converted /c/Claude/RE4-Xbox/xbox-pak
#
# Default: the data goes into ONE pack file, re4.pak (~1.5 GB, zlib in 64 KB chunks; tools/pack_data.py),
# which the XBE reads directly (port/dvd/pak.cpp). Two files to transfer:
#   default.xbe
#   re4.pak
# --split MB   cut the pack into MB-sized parts re4.pak.000, .001, ... (all parts go in the same folder);
#              useful when an FTP transfer of one 1.5 GB file keeps failing: resend only the broken part
# --loose      the old layout: data/files/... as ~1100 loose files
# <converted dir> may also be a folder that already holds files/ and files_disc2/ (e.g. a previous
# package's data/ folder).
#
# On the Xbox put both in any folder, e.g. E:\Games\RE4\, and launch default.xbe. The log is written to
# re4.log in that folder (or E:\re4\re4.log when the folder is read-only); saves go to saves\ next to it.
#
# Run from Git Bash (not PowerShell's `bash`, which is WSL):
#   "C:\Program Files\Git\bin\bash.exe" xbox/package_xbox.sh <converted dir> <out dir>
set -e
SRC="$1"; OUT="$2"; shift 2 || true
[ -n "$SRC" ] && [ -n "$OUT" ] || { sed -n 2,22p "$0"; exit 2; }
SPLIT=0; LOOSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --split) SPLIT="$2"; shift 2 ;;
        --loose) LOOSE=1; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$HERE/bin/default.xbe" ] || { echo "build first: bash xbox/build_xbox.sh RE4_GAME=1"; exit 1; }
PY="py -3"; command -v py >/dev/null 2>&1 || PY=python3
mkdir -p "$OUT"
cp "$HERE/bin/default.xbe" "$OUT/"
if [ "$LOOSE" = 1 ]; then
    mkdir -p "$OUT/data"
    cp -r "$SRC/files" "$OUT/data/"
    [ -d "$SRC/files_disc2" ] && cp -r "$SRC/files_disc2" "$OUT/data/"
else
    rm -f "$OUT"/re4.pak "$OUT"/re4.pak.[0-9][0-9][0-9]
    if [ "$SPLIT" != 0 ]; then
        $PY "$HERE/tools/pack_data.py" "$SRC" "$OUT/re4.pak" --split "$SPLIT"
    else
        $PY "$HERE/tools/pack_data.py" "$SRC" "$OUT/re4.pak"
    fi
fi
ls -la "$OUT"
echo "ready: $OUT (copy these to one folder on the Xbox, launch default.xbe, then fetch re4.log)"
