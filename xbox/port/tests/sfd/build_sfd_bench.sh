#!/bin/bash
# xbox/port/tests/sfd/build_sfd_bench.sh -- builds the host Sofdec benchmark (CT-07) on Windows:
# 32-bit, LLVM clang + lld, the 138 CRI units prepared by xbox/tools/prep_cri.py.
#
#   bash xbox/port/tests/sfd/build_sfd_bench.sh [OUTDIR]     (default C:/Claude/RE4-Xbox/gamedata/ct07)
#   MARCH=pentium3 bash ...   to build for the Xbox CPU's instruction set (default: native)
#
# Prepped sources and objects go to OUTDIR (outside the repo); the exe is OUTDIR/sfd_bench[_MARCH].exe.
set -e
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:-C:/Claude/RE4-Xbox/gamedata/ct07}"
CLANG="${CLANG:-C:/Program Files/LLVM/bin/clang.exe}"
MARCH="${MARCH:-native}"
OPT="${OPT:--O2}"
PREP="$OUT/prep"; OBJ="$OUT/hobj_$MARCH"
mkdir -p "$PREP" "$OBJ"
cd "$ROOT"
UNITS=$(py -3 -c "import sys; sys.path.insert(0, 'xbox/tools'); import prep_cri; print(' '.join(n + '.c' for n in prep_cri.CRI_NAMES))")
# the CRI headers are prepped too (bit reader macros in cri/mpv_bit.h) and searched before src/lib/cri
py -3 xbox/tools/prep_cri.py --dir src/lib/cri "$PREP/cri"
CFLAGS="-m32 -march=$MARCH $OPT -DRE4_PORT=1 -include xbox/port/include/re4port/re4_xbox.h -I xbox/port/include -I include -I src -I src/lib -I $PREP/cri -I src/lib/cri -fno-strict-aliasing -Wno-everything"

for u in $UNITS; do
  echo "$u"
done | xargs -P 12 -I{} bash -c '
  u={}
  py -3 xbox/tools/prep_cri.py src/lib/$u "'"$PREP"'"/$u || exit 1
  "'"$CLANG"'" '"$CFLAGS"' -c "'"$PREP"'"/$u -o "'"$OBJ"'"/${u%.c}.o || { echo "FAIL $u"; exit 1; }
'
"$CLANG" $CFLAGS -c xbox/port/tests/sfd/sfd_bench_sdk.c -o "$OBJ/sfd_bench_sdk.o"
"$CLANG" -m32 -march=$MARCH $OPT -DRE4_PORT=1 -include xbox/port/include/re4port/re4_xbox.h -I xbox/port/include -I include \
  -Wno-everything -c xbox/port/tests/sfd/sfd_bench.cpp -o "$OBJ/sfd_bench.o"
# functions the SN linker dead-stripped still reference a few CRI symbols that do not exist: leave them unresolved
"$CLANG" -m32 -fuse-ld=lld "$OBJ"/*.o -o "$OUT/sfd_bench_$MARCH.exe" -Wl,/FORCE:UNRESOLVED $LDEXTRA 2>&1 | grep -v "undefined symbol\|referenced by\|>>>" || true
echo "built $OUT/sfd_bench_$MARCH.exe"
