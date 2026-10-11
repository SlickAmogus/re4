# CT-11 — Host (Linux, -m32) build of the whole game

**Status:** open · cloud-friendly · supersedes the link part of CT-03

## Goal

`make -C xbox -f Makefile.host RE4_GAME=1` compiles and links the full game for 32-bit Linux exactly as
the Xbox build already does with nxdk (2026-10-11: `xbox/Makefile`, 549 units, 105 REL modules), so the
cloud can run the game headless against converted assets. Milestone M1 of `00-PORTING-PLAN.md`.

## What exists (read first)

- `xbox/Makefile` sections "REL modules" and "link closure": the working Xbox recipe.
- `xbox/tools/prep.py` (per-unit source rewriting), `gen_units.py` (`units.mk` from the decomp's REL table),
  `isolate_modules.py` (per-module symbol prefixing with llvm-objcopy), `link_resolve.py` (probe link →
  `/alternatename` aliases + `build/link_stubs.c` catch-alls). `xbox/docs/07-COMPILER.md` explains all of it.
- The port layer under `xbox/port/` already builds for the host in `Makefile.host` (stub game).

## Work

1. `Makefile.host`: mirror the Xbox module rules (per-module prepped copies under `build/host/mod/`,
   `-I` of the source's own dir, isolation into `build/host/mod/<m>.a`), headers prepped into
   `build/host/prep/include` and `prep/src/lib`.
2. `isolate_modules.py` on ELF: COMDAT detection is already there for nm `W/V/u` types; verify with
   `llvm-objcopy --redefine-syms` on ELF32 objects; entry names have no leading underscore on ELF (the
   script already chooses `lead` from the symbol style — check it).
3. `link_resolve.py` ELF mode: probe-link with `ld.lld -m elf_i386 --error-limit=0 --no-demangle`
   (or `clang -m32 -fuse-ld=lld`), parse `undefined symbol:` lines, emit `--defsym=U=T` (or a linker
   script with `PROVIDE(U = T);`) instead of `/alternatename`. The mangling is Itanium on the host, so the
   "data type" rule is unnecessary (Itanium does not mangle variable types) but the "signature" rule
   becomes "same function name, different parameter list" — check what remains.
4. `MAP_FIXED_NOREPLACE` MEM1 at 0x80000000 (already in `os_shim.cpp`) must succeed: link the host binary
   with `-Wl,-Ttext-segment=0x08048000` (default) and verify nothing else lands in 0x80000000..0x81840000.
5. Run: `./xbox/build/host/re4_host --data <converted> --headless --frames 600` must reach the title task
   and log the first DVD reads. Missing assets in the cloud: the converted tree is not available; use
   `--data /nonexistent` and verify the game logs `dvd: missing file etc/core.das` and keeps running its
   frame loop (no crash) — that is the acceptance for the cloud. Real-data runs happen locally.

## Acceptance

- Host build links with zero undefined symbols other than the catch-all list in `build/host/link_stubs.c`.
- `re4_host --data /nonexistent --headless --frames 120` runs 120 frames and exits 0; log attached to the PR.
- `xbox/docs/07-COMPILER.md` gets a "Host link" subsection with the differences from the Xbox recipe.
