# 00 — Porting plan

How Resident Evil 4 (GameCube) becomes an original-Xbox title, in the order the work should happen. The
measurements that justify each decision are in `01-ANALYSIS.md`; each subsystem has its own design doc.

## The one-paragraph version

The decomp is complete, so we do not reverse-engineer anything: we compile Capcom's game code with clang
for x86 and give it a new floor to stand on. That floor is a reimplementation of the ~170 Nintendo SDK
functions the game actually calls (110 GX, ~60 OS/DVD/AR/PAD/CARD/VI/AX/MIX/AXFX) on top of nxdk, pbkit and
RXDK DirectSound, plus C replacements for the ~230 instructions of PowerPC paired-single assembly in game
code and the two hand-written assembly units (yz2 decompressor, memset). Data on the disc is big-endian, so
the assets are converted offline to the same layout in little-endian (the decomp's own motion tool already
does exactly this to run game code on a PC). Models are drawn from GX display lists embedded in the model
files, so the renderer's core is a display-list interpreter that feeds NV2A, and the TEV shader setups the
game builds per material are mapped onto NV2A register combiners. Everything is developed first as a
32-bit Linux/SDL2/OpenGL **host build** (cheap to debug, runnable in cloud sessions), then the Xbox backend
replaces the host backend behind the same interfaces, exactly as Starship's Fast3D backend and Duke's
POLYMOST shim were done.

## Architecture

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │ game code (src/game, src/em*, src/pl*, src/wep*, src/st*, Sscrn)    │  unchanged, ~500k lines
 │ CRI Sofdec/ADX (src/lib adx_*, sfd_*, mpv_*, mps_*)                 │  compiled as-is minus asm
 ├─────────────────────────────────────────────────────────────────────┤
 │ xbox/port — the SDK surface (same headers the game includes)        │
 │   gx/   GX state machine + DL interpreter + TEV→combiner compiler    │
 │   os/   threads(fibers) alarms time cache-ops report alloc           │
 │   dvd/  DVD* over files, AR*/ARQ* over a RAM block, disc-id          │
 │   pad/  PAD* over SDL2 game controllers                             │
 │   card/ CARD* over save files                                       │
 │   snd/  AX/MIX/AXFX over DirectSound (Xbox) or SDL audio (host)     │
 │   mtx/  PSMTX*/PSVEC* = the SDK's C versions (src/lib mtx.c ...)    │
 ├──────────────────────────┬──────────────────────────────────────────┤
 │ backend: nxdk/pbkit/RXDK │ backend: SDL2 + OpenGL 2.1 (host)        │
 └──────────────────────────┴──────────────────────────────────────────┘
```

Two rules make this tractable:

1. The game's own headers (`include/gx.h`, `include/dvd.h`, `include/pad.h`, `include/card.h`, ...)
   already declare the SDK functions with plain `int` enum parameters. We implement *those* declarations.
   We do not pull in `include/dolphin/`'s real SDK headers except for enum values.
2. Everything the game loads from disc keeps its byte layout. Pointers stay 32-bit (Xbox is 32-bit; the host
   build is `-m32`), so in-place relocation of file offsets to pointers (`calcModelAddr`, `calcTplAddr`,
   `MotionSetCore`) and every offset-based macro in the game keep working.

## Phases

### Phase 0 — Research and scaffolding (this commit)

- Decomp forked, retail discs extracted, platform surface measured (`01-ANALYSIS.md`).
- Design docs for graphics, audio, data, system, compiler.
- Build skeleton: `xbox/Makefile` (nxdk) and `xbox/Makefile.host`, port headers with the full GX/OS/DVD/PAD/
  CARD/AX declarations stubbed, shared `xbox_compat/` from the other ports.
- Cloud tasks written for everything PC-verifiable.

### Phase 1 — Host build links (cloud-friendly)

Goal: `make -f Makefile.host` compiles every non-debug game unit and the CRI libs for 32-bit x86 and links
with the stub platform layer. No data, no rendering; the executable starts, runs `systemStartInit`, and
exits cleanly at the first DVD read.

- `07-COMPILER.md`: force-included header that neuters `register ... asm("rN")`, `asm("")` pins, `#line`,
  `__attribute__` oddities; per-unit warnings; GCC 2.95-isms clang rejects.
- Replace the asm: `CT-02` (C versions of CalcSk1_x/x2, SQRTF, SINF/COSF, LIMIT_ANGLE, PSQ_L macros,
  memset_asm/memclr_asm/mtxclr_asm, yz2 decoder, GQR setup → no-op).
- Static module table replacing OSLink/REL loading: each `_prolog/_epilog/_unresolved` renamed per module
  (`CT-03`).
- Debug build leftovers: `pG->debug_mode`, `DbgFlagChk`, debug menu, SN file server — compiled but inert.

### Phase 2 — Data loads on the host

Goal: the title task runs, loads `etc/core.das`, `etc/title.das`, the SS data and the first room's archives
through the port's DVD layer from the converted asset directory, with no crash for 1,000 frames headless.

- Asset converter (`CT-04`): yz2 → raw, `.drs/.das/.dat` containers, model `.bin`, `TPL`, FCV motions, SMD/LIT/
  SAR/CAM/EST/... room blocks, SND banks, message/font files, all byte-swapped field by field using the
  struct definitions in `include/`. Output is a mirror of `files/` in little-endian.
- File table vs retail disc audit (`CT-01`): the debug `FileTbl[]` names `em/*.das`, the retail disc has
  `em/*.drs`; find how the names resolve and what the retail disc lacks/has (no `Bio4.sym`, no debug tools).
- ARAM = a 16 MB RAM block; ARQ DMA = memcpy; DVD queue = synchronous file reads completed on the next
  `Dvd.Watcher()`.

### Phase 3 — Renderer on the host (OpenGL reference), then NV2A

Goal: the title screen, then room r100 (village entrance) render correctly on the host; then the same on
Xbox.

- GX state machine (`02-GRAPHICS.md`): vertex descriptors/formats, arrays, matrices, TEV stages, texgen,
  blend/z/alpha/cull, fog, lights, texture objects, TLUTs, EFB copies, display lists.
- Display-list interpreter: decode `GXBegin`-style primitive streams with indexed attributes per the current
  VCD/VAT, dereference arrays (s16/s8/u16/RGBA8 with fixed-point shifts), emit triangles.
- TEV → shader: on the host, generate GLSL per unique TEV configuration (dolphin-style); on Xbox, map to
  register combiners (8 general stages + final), with a table of the game's ~30 distinct material setups
  (`CT-05` catalogues them from `shaderSetup`, `materialSetup`, `TextureBlend*`, `specularSetup`,
  `bumpSetup`, `alphaSetup`, the `filter0*` post-processes, `esp*` effects, `mes.cpp`, `id_sys.cpp`).
- Textures: GC tiled formats (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CI4/CI8/CMPR) → NV2A swizzled
  A8R8G8B8/R5G6B5/A1R5G5B5/DXT1 at conversion time; CI formats expanded with their TLUT; mipmaps kept.
- EFB copies (`GXCopyTex`, 26 call sites: shadows, refraction, thermal, depth-of-field, glow filters) →
  render-to-texture surfaces.
- Indirect texturing (water, heat haze, bump): NV2A texture shaders (bump-env-map / dot-product modes)
  for the common cases, documented fallbacks for the rest.

### Phase 4 — Xbox bring-up

Goal: `default.xbe` boots on hardware (and xemu where possible), shows the title, loads r100, takes input.

- nxdk Makefile with the Starship C++ settings (`NXDK_CXX=y`, big stack, `-fpermissive`), the Duke
  include-path workarounds, `xbox_startup.c` (AC97 reset, video mode, writable-dir probe, logging).
- Memory: a 24 MB "MEM1" arena + 16 MB "ARAM" block from `MmAllocateContiguousMemoryEx` (textures and
  display lists must be GPU-visible), the game's fixed addresses redirected (`05-SYSTEM.md`).
- Fibers for the 18 game tasks; a real thread for audio pumping.
- Pad mapping (`05-SYSTEM.md`), rumble, 480p/720p modes.

### Phase 5 — Audio

Goal: sound effects, BGM streams, reverb on real hardware with 5.1 out (`03-AUDIO.md`).

- SE banks: DSP-ADPCM → Xbox ADPCM offline; one DirectSound hardware voice per AX voice (64 max in game);
  `MIX*` pan/aux → mixbin volumes; `AXFX` reverb/chorus/delay → I3DL2 reverb on the EP.
- Streams: four ARAM stream buffers → DirectSound streaming buffers fed by the existing `snd_str*` logic.
- MIDI sequences (`snd_seq*`): the game's own sequencer drives AX voices from a DLS bank; keep it.

### Phase 6 — Movies, saves, disc 2, polish

- Sofdec: compile the CRI MPEG-1/ADX stack for x86 (11 units have cache/SPR asm to stub); fallback plan is
  pl_mpeg + a 60-line ADX decoder if the CRI path is too slow at 733 MHz.
- CARD → `E:\UDATA\<title>\` save files with the same 8 KB-block images the game writes.
- Both discs merged into one asset tree; `DVDChangeDiskAsync` and the disc-swap screen become no-ops.
- Performance: batch display-list output into vertex buffers, cache texture uploads, cut CPU skinning cost
  (SSE), measure r100 at 30 fps (the GC target) and see what 60 costs.
- 720p/widescreen: the game already renders cinemascope bars (`cinesco.cpp`); a 16:9 projection is a
  natural upgrade once the base works.

## Milestones (what "done" looks like at each step)

| # | Milestone                                                   | Verified on |
|---|-------------------------------------------------------------|-------------|
| M1| Host build compiles and links every non-debug unit          | cloud       |
| M2| Title task runs headless against converted assets           | cloud       |
| M3| Title screen and r100 render on host OpenGL                 | cloud (screenshots) |
| M4| XBE boots to title on Xbox, pad works                       | local       |
| M5| r100 playable on Xbox, no audio                             | local       |
| M6| SE + BGM + reverb, 5.1                                      | local       |
| M7| Chapter 1-1 complete end to end, saves, movies              | local       |
| M8| Full game, both discs, 480p/720p options                    | local       |

## Risks, in order

1. **Compiler drift.** 500k lines of 1999-era C++ under clang. Mitigation: start the host build on day one
   and fix categories, not files (most errors will repeat the same ten patterns).
2. **TEV coverage.** GX has 16 TEV stages, 3 colour channels, konst colours, swap tables, indirect
   stages; NV2A has 8 combiner stages and texture shaders. The game uses at most ~9 stages
   (`shaderReset` touches 16 but materials use 2–6). Mitigation: the catalogue in `CT-05`; a GLSL host
   path that is exact, so Xbox differences are visible side by side.
3. **Memory.** 24 MB MEM1 + 16 MB ARAM + x86 code (~6–8 MB) + GPU copies of textures and vertex data must
   fit in 64 MB. Mitigation: textures converted offline live once (GPU-visible) and are not duplicated;
   ARAM contents that are only staging (room blocks) can be streamed from HDD instead.
4. **Audio fidelity.** The AX mixer runs effects on the DSP with its own reverb algorithms. Mitigation: the
   Xbox EP has I3DL2 reverb; tune presets per room type rather than emulating AXFX bit-exactly.
5. **Retail vs debug data.** The decomp is the Nov 25 2004 debug build; the retail disc is a later compile
   of the same tree. Formats look identical (same file list, same CRI libs, same source paths in the DOL) but
   any struct that changed between builds will show up as corrupt data. Mitigation: `CT-01` audit plus
   per-format validation in the converter (sizes, offsets in range, magic words).
