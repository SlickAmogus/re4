# CT-07 — CRI Sofdec on x86

**Status:** open · depends on CT-03

## Goal

Decide Plan A vs Plan B for movies (`05-SYSTEM.md` section 7) with numbers: compile the CRI stack from
`src/lib` for x86, feed it a real `.sfd`, measure decode time per frame.

## Steps

1. Units: `src/lib/adx_*.c sfd_*.c mpv_*.c mps_*.c mpvabdec.c cri_cvfs.c gcci.c gcci_sub.c cft*.c dct*.c`
   plus whatever `sofdec.cpp` links (`MWS_PLY_*`, `ADXM_*`, `ADXT_*`). Headers in `src/lib/cri/`.
2. Asm: 11 units carry cache ops (`dcbf/dcbi/dcbst`), `mftb`, MWCC `asm { mr rX, rX }` pins and one
   "small block in dct_ac". Replace through macros in a `cri_x86.h` force-include: cache → no-op, time
   base → `rdtsc`/`clock_gettime`, pins → nothing. Do not edit `src/lib`; generate prepped copies as in
   CT-03.
3. Device layer: `gcci` reads through the GameCube DVD API; provide a tiny host CVFS device (`fopen`
   based) or route to the port's DVD shim.
4. Driver: `xbox/port/tests/sfd_bench.cpp` opens `opening.sfd` (ask the user to attach `files/Movie/
   demo0.sfd`, the smallest), decodes every frame headless (no GX; grab the Y/U/V planes), reports frames,
   resolution, ms/frame on the host, and writes every 30th frame as PGM for eyeballing.
5. Estimate for a 733 MHz Pentium III: scale by a measured ratio (run the same bench with `-m32
   -march=pentium3 -mno-sse2` and divide by the host's clock, or run it under `qemu-i386 -cpu pentium3`
   for a sanity check of instruction set compliance, not speed).

## Deliverables

- Prepped CRI build integrated into `xbox/Makefile.host` behind `RE4_MOVIES=1`.
- `xbox/docs/05-SYSTEM.md` "Movies: measurements" with the numbers and the Plan A/B decision.
- If Plan B: a note on `pl_mpeg` integration and a 60-line ADX decoder (`adx_dec.c`, the standard
  2-coefficient predictor, 18-byte frames of 32 samples).

## Acceptance

Bench runs on an attached `.sfd`; every frame decodes without error; the doc states ms/frame and the
decision.
