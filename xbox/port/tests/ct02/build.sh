#!/bin/sh
# xbox/port/tests/ct02/build.sh -- build + run the CT-02 tests on the Windows host (LLVM) or Linux.
#   sh build.sh [r120.das r120.ref.bin]
set -e
cd "$(dirname "$0")"
MAKE=${MAKE:-make}
command -v "$MAKE" >/dev/null 2>&1 || MAKE=/c/msys64/usr/bin/make.exe
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    LLVM="/c/Program Files/LLVM/bin"
    for m in "" -m32; do
      "$MAKE" clean >/dev/null
      "$MAKE" CC="$LLVM/clang.exe" CXX="$LLVM/clang++.exe" LDLIBS=-fuse-ld=lld EXE=ct02.exe M="$m"
      ./ct02.exe "$@"
    done ;;
  *)
    "$MAKE" && ./ct02 "$@"
    "$MAKE" clean && "$MAKE" M=-m32 && ./ct02 "$@" ;;
esac
