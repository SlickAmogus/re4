#!/usr/bin/env bash
# Builds xbox/bin/default.xbe with nxdk. Mirrors jfduke3d-xbox/build_xbox.sh (see docs/06-LESSONS-FROM-PORTS.md).
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export NXDK_DIR="${NXDK_DIR:-/c/Claude/nxdk}"
export PATH="${NXDK_DIR}/bin:$PATH"

# MSVC's vcvars leaks PC SDK headers into clang's MSVC-compat mode; nxdk-cc passes explicit -I flags.
unset INCLUDE LIB LIBPATH

MAKE=make
[ -x /c/msys64/usr/bin/make.exe ] && MAKE=/c/msys64/usr/bin/make.exe

"$MAKE" -f Makefile "$@" 2>&1 | tee build_xbox.log
