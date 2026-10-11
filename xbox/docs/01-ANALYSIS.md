# 01 — Analysis of the platform surface

Measured on the decomp at commit `9b76ff00` (2026-10-11) and on the retail USA discs. Numbers are from
`grep` over `src/` excluding `src/lib` (SDK/CRI/runtime) unless stated.

## Code volume

| Area                                            | Lines   | In the port |
|-------------------------------------------------|---------|-------------|
| `src/game/` (engine, player, effects, sound)    | 172,728 | yes         |
| `src/em*`, `pl*`, `wep*`, `st*`, `Sscrn`        | 231,999 | yes         |
| `src/lib/` (SDK, CRI, runtime, libgcc, libm)    |  91,453 | CRI + matrix C code only |
| `src/Tools/`, `src/tools/`, `src/t_*/` (editors)|  60,305 | no          |
| Headers `include/`                              |  44,000 | yes         |

1,012 `.c/.cpp` files, 256 headers. Function and most struct names are Capcom's own, which makes the port
readable; placeholders like `x1C` are fields whose meaning is unknown but whose layout is exact.

## Graphics: GX

110 distinct GX functions, about 4,000 call sites. Top by count:

```
 227 GXPosition3f32     199 GXSetVtxAttrFmt   197 GXSetVtxDesc      187 GXTexCoord2f32
 167 GXColor4u8         141 GXSetTevOrder     129 GXSetTevColorOp   118 GXSetTevAlphaOp
 117 GXSetTevColorIn    114 GXSetTevAlphaIn   103 GXSetBlendMode     93 GXSetNumTevStages
  91 GXSetZMode          88 GXSetNumTexGens    87 GXSetChanCtrl      85 GXBegin
  78 GXClearVtxDesc      75 GXSetNumChans      75 GXLoadPosMtxImm    70 GXSetCurrentMtx
  68 GXPosition3s16      66 GXLoadTexObj       64 GXSetCullMode      54 GXSetTexCoordGen
  54 GXMatrixIndex1u8    54 GXInitTexObj       51 GXLoadTexMtxImm    49 GXSetAlphaUpdate
  48 GXSetProjection     45 GXSetTevDirect     44 GXNormal3s8        40 GXSetTexCoordGen2
  26 GXCopyTex           26 GXSetFog           23 GXLoadTlut         21 GXSetNumIndStages
  12 GXLoadLightObjImm   12 GXCallDisplayList  10 GXSetIndTexOrder    8 GXSetTevIndWarp
   4 GXPeekZ              1 GXSetTevIndBumpXYZ
```

Where they are (call sites per file): `trans.cpp` 466 (the model renderer: `commonModelTrans`,
`shaderSetup`, `materialSetup`, `TextureBlend1-3`, `specularSetup`, `bumpSetup`, `alphaSetup`,
`ShadowCastSetup`, `SelfShadowSetup`, `RefractShaderSetup`, `ThermoShaderSetup`), `shadow.cpp` 299
(projected shadows rendered into I8 textures from the light), `esp_sub.cpp` 217 + `esp*.cpp` ~700
(particle/sprite effects, immediate mode quads), `filter00-0b.cpp` ~800 (post-processing: radial blur,
depth of field, alpha/radial glow, dust, thermal vision, zoom blur, pause blur, masked blur, screen
cross-fade), `id_sys.cpp` 171 (HUD/"interior design" 2D with indirect textures), `sofdec.cpp` 97 (movie
frame blit with YUV→RGB in TEV via swap tables), `main_sub.cpp` 70 (frame setup, EFB/XFB copy),
`mes.cpp` 57 (text), `cloth.cpp`, `mirror.cpp`, `cinesco.cpp`, `fade.cpp`, `Espgen42/43` (water, sand
height fields with their own display lists), `ss_item_draw.cpp` (inventory 3D).

**Models are display lists.** `commonModelTrans` sets the vertex descriptor (position s16 with per-model
shift, normal s8 or s16, colour RGBA8 optional, texcoord s16 frac 8 or u16 frac 15), binds the arrays with
`GXSetArray` and calls `GXCallDisplayList(p, part->buff_size)` on the primitive stream that follows each
0x20-byte `cPolyHeader`. Skinned models use CPU-skinned position/normal buffers (`CalcSk1_x`, paired-single
asm) double-buffered per frame. The interpreter must support: `GX_DRAW_QUADS/TRIANGLES/TRIANGLE_STRIP/
TRIANGLE_FAN/LINES/LINE_STRIP/POINTS`, index8/index16 per attribute, matrix index attribute (54 sites use
`GXMatrixIndex1u8`), and the vertex formats above.

**EFB copies** (26 `GXCopyTex`, 5 `GXCopyDisp`): shadow maps, refraction (`g_Get_tex_obj`), thermal
scope, DoF/glow filters, pause screen, screen fades. `GXPeekZ` (4) reads depth for lens flares/effects.

**Immediate mode** (`GXBegin` 85): effects, HUD, filters, debug. The port's `GXBegin` writes into a CPU
vertex buffer that is flushed at `GXEnd`-equivalent points (the FIFO write macros in `include/gx.h`
become inline writes into that buffer).

**Lighting**: `GXSetChanCtrl` 87, `GXLoadLightObjImm` 12, light objects built in `trans_lit.cpp`. The
game uses hardware vertex lighting with up to 8 lights, ambient/material colours and attenuation; NV2A has
equivalent fixed-function lighting, or a vertex program.

**Indirect texturing**: 21 `GXSetNumIndStages`, 10 `GXSetIndTexOrder`, 8 `GXSetTevIndWarp`, 1
`GXSetTevIndBumpXYZ`; used by water, heat distortion, bump mapping, and the HUD "id" textures.

## Nintendo SDK calls in game code

```
4160 ARC (archive entry macro, not SDK)
1323 PSMTXMultVec   920 PSVECAdd    688 PSVECScale    660 PSVECSubtract  360 VECNormalize
 350 PSMTXMultVecSR 332 PSMTXConcat 263 PSMTXRotRad   195 PSMTXInverse   164 PSMTXIdentity
 135 PSVECCrossProduct 112 PSVECMag 108 PSVECDotProduct 59 PSMTXCopy      47 PSMTXRotAxisRad
  45 C_MTXOrtho      15 C_MTXLightPerspective 13 C_MTXLookAt 8 C_MTXPerspective
 313 OSReport  31 OSRestoreInterrupts 30 OSDisableInterrupts 24 DCStoreRange 19 DCFlushRange
  17 DCInvalidateRange 11 OSSetErrorHandler 7 OSGetTime 7 OSGetTick 7 OSCheckHeap 7 ARQPostRequest
   5 OSResumeThread 3 OSSuspendThread 3 OSSignalSemaphore 3 OSResetSystem 1 OSCreateThread
  11 AXSetVoiceState 8 AXSetVoiceType 6 AXSetVoiceLoopAddr/EndAddr 5 AXSetVoiceAdpcmLoop 5 AXFreeVoice
   7 VIFlush 4 VIWaitForRetrace 3 VIConfigure 2 VISetNextFrameBuffer 2 VISetBlack
   4 DVDClose 3 DVDConvertPathToEntrynum 3 DVDCancelAsync (+DVDFastOpen, DVDReadAsyncPrio, DVDGetDriveStatus,
     DVDChangeDiskAsync, DVDCompareDiskID, DVDGetFSTLocation in dvd.cpp)
   9 CARDGetResultCode 6 CARDProbeEx 4 CARDGetStatus (+Mount/Unmount/Create/Delete/Format/Open/Close/Read/Write)
   3 PADControlMotor 2 PADReset (+PADInit, PADRead, PADClamp, PADSetAnalogMode)
```

The `PSMTX*/PSVEC*` routines are paired-single assembly in the SDK, but `src/lib/mtx.c`, `mtxvec.c`,
`vec.c`, `quat.c` hold the SDK's C versions under `C_*` names; the decomp's host tool already links those as
the PS* names. Same for `src/lib/sf_sin.cpp` etc. (fdlibm).

## Assembly in game code

- `docs/naming.md`: 231 instructions at 18 sites of real asm (paired-single, GQR, cache, SPR, exception
  handler) plus 8 whole-asm units; 526 `COMPILER-DIFF` tags and 97 `register ... asm("rN")` pins that
  emit nothing.
- Sites that need C replacements: `trans.cpp` `CalcSk1_x`, `CalcSk1_x2` (skinning: s16 pos + matrix
  index → s16 pos; s8 normals), `PSQ_L_U8`/`PSQ_L_U8_TO`, `setupGQR6`; `math_sub.cpp` `SQRTF` (frsqrte +
  Newton), `SINF`, `COSF` (paired-single polynomial), `LIMIT_ANGLE`; `shape.cpp` `PSQ_L_S16`/`PSQ_ST_S16`
  (morph targets); `Espgen42.cpp`/`espgen45.cpp`/`em2b.cpp`/`dbmodule.cpp` psq loads; `scheduler.cpp`
  GQR setup in `TaskExec_hook`; `exception.cpp` (drop); `main_mem.cpp` and `memset_2.cpp`
  (`memset_asm`, `memclr_asm`, `mtxclr_asm`); `yz2asm.cpp` (the room-archive decompressor; the decomp
  has a Python reference in `tools/motion/yz2.py` and the setup code in `yz2code.cpp`).
- CRI libs with asm (cache/SPR): `mpv_hdec.c mpv_lib.c mpv_mc.c mpv_umc.c sfd_adxt.c sfd_hds.c sfd_mpv.c
  sfd_pts.c adx_dcd5.c adx_tsvr.c cri_cvfs.c`.

## Memory map (debug build, `main_mem.cpp`)

```
0x80000000  MEM1 start (24 MB)
0x80350000  Dvdread_addr   DVD read staging
0x80370000  Sound_addr     sound system tables (SND_DATA_TOP)
0x803F0000  Fifo_addr      GX FIFO, 0x70000 bytes
0x80460000  Xfb_addr       two external framebuffers
0x80578000  Core_addr      etc/core.das
0x807AC000  Option_addr
0x807EC000  Player_addr    player archive
0x80904000  Weapon_addr    weapon archives
0x80974000  Heap_addr      main heap start (13 logical heaps over OSAlloc)
0x817F4000  Heap_end_addr  (~14.5 MB of heap)
0x81800000  Debug_usb_addr / 0x8181FB00 Debug_addr  (debug build only, past 24 MB)
ARAM 16 MB: data units up to 0xD00000 (datactrl.cpp), sound streams at 0x700000/0x740000/0x780000/0x7C0000
```

The ELF must end below 0x80350000 (3.3 MB for the DOL; the retail DOL is 2.07 MB). The REL modules are
loaded into the DLL heap (`cell_dll`). `VALID_PTR(p)` checks `0x80000000..0x82FFFFFF`.

## Threads and timing

`scheduler.cpp`: 18 task slots, each an `OSThread` with its own stack; the main thread runs each task's
thread per frame until it `TaskSleep`s or exits (cooperative, semaphore for priority > 15). `dvd.cpp` uses
DVD and ARQ callbacks (interrupt context on GC). `snd_main.cpp` registers an AX frame callback (every 5 ms
on the DSP). `OSAlarm` is used in `main.cpp`. Everything else is per-frame polling in `main()`.

## Audio

Capcom's driver (`snd*.cpp`, 22 units, ~15k lines) over AX: `AXInitEx`, `MIXInit`, `AXARTInit`,
`AXRegisterCallback`, `AXSetCompressor`, `AXSetMode`. Voices: `AXAcquireVoice(30, cb, 0)`, `AXSetVoiceAddr`
(DSP-ADPCM nibble addresses), `AXSetVoiceAdpcm`/`AdpcmLoop`, `AXSetVoiceLpf`, `AXSetVoiceSrc`/`SrcRatio`,
`AXSetVoiceState`, `AXSetVoicePriority`, `MIXInitChannel`/`MIXSetInput`/`Pan`/`SPan`/`AuxA`/`AuxB`,
`AXFXReverbHi/Std`, `AXFXChorus`, `AXFXDelay` on AUX A/B. Four stream works read ADPCM streams from DVD into
ARAM ring buffers (`snd_str*`); `snd_seq*` is a MIDI-like sequencer over a DLS wavetable (`bgm/bio4midi.*`).
`ADXT_SetOutputMono` is the only ADX call outside movies.

## Movies

`sofdec.cpp` drives CRI Sofdec (`MWS_PLY_*`, `ADXM_ExecMain` once per frame): MPEG-1 video decoded by
`mpv_*`/`mps_*` into a frame pool, ADX audio, read through CVFS from `Movie/*.sfd` (disc 1: opening,
demo0/1, adaend; disc 2 has its own set). The frame is blitted with 97 GX calls (YUV planes through TEV swap
tables). The whole stack is C in `src/lib` (18k lines in `mpv_*`/`sfd_*`).

## Retail disc vs the decomp's debug build

| Item                        | Debug build (decomp)               | Retail USA (our discs)                       |
|-----------------------------|------------------------------------|----------------------------------------------|
| Game ID                     | G4BE08                             | G4BE08                                       |
| `main.dol`                  | built Nov 25 2004, 114 RELs        | 2,168,544 bytes, sha1 `5aa53851…`, later build |
| Source paths in the DOL     | `D:/Bio4/Prog/*.cpp`               | same                                         |
| CRI library builds          | Sep 22 / Oct 8 2004                | same                                         |
| `files/Rel/*.rel`           | Sscrn, st*, Tools, t_*             | disc 1: Sscrn, st1_0-3, st2_0-4, st4_0; disc 2: Sscrn, st2_4, st3_0-3, st4_0 |
| `files/em/*.drs`            | enemy/player/weapon archives with embedded RELs | 128 archives, same names         |
| `files/St1..St4/`           | room archives `rNNN.das` + `rNNN_NN.dat` | St1 34 files, St2 42, St4 14, St3 (disc 2) 42 |
| `files/Evd/`                | cutscene data                      | 88 files                                     |
| `files/etc/`, `ss/`, `op/`, `bgm/`, `font/`, `Movie/` | present               | present                     |
| `Bio4.sym`, `Bio4.<mod>.sym`| present (debug symbols)            | absent                                       |

The retail DOL cannot be rebuilt from the decomp (it is a different compile), but we do not need to: we
compile the decomp's source. What we need from retail is the data, and the file list matches the names the
code loads. Open question for `CT-01`: `FileTbl[]` in `dvd.cpp` names `em/em10.das` while both discs carry
`em/em10.drs`; find the rename (apploader? `DVDConvertPathToEntrynum` fallback? a later table?).

## Debug-build leftovers to neutralise

`pG->debug_mode` (81 uses in `src/game`), `DbgFlagChk(pG, DBG_*)`, `db_menu.cpp` (loads `t_*.rel`),
`db_log.cpp` (on-screen log), `eprintf.cpp` (debug text through GX), `exception.cpp` (error handler with
symbol lookup), `t_bugcheck.cpp`, `dbmodule.cpp` (wireframe spheres with display lists), the SN file server
(`file.cpp` `SETROOT:` paths, `HDRead`), `OSReport`. All compile; the port turns the switches off and routes
`OSReport`/`pLog` to the Xbox log file.
