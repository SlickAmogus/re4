#!/bin/bash
# SessionStart hook for Claude Code cloud sessions (xbox/cloud-tasks/README.md).
# Installs what the 32-bit host build and the port's tests need; a no-op on local machines.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

# 32-bit toolchain for `make -C xbox -f Makefile.host` and `make -C xbox/port/tests` (07-COMPILER.md).
if ! dpkg -s g++-multilib >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq gcc-multilib g++-multilib
fi

# Python packages used by the tools/tests (jsonschema for the TEV catalogue check, pytest for re4conv).
python3 -c "import jsonschema" 2>/dev/null || pip3 install -q jsonschema
python3 -c "import pytest" 2>/dev/null || pip3 install -q pytest

echo "re4 session-start: 32-bit toolchain and python deps ready"
