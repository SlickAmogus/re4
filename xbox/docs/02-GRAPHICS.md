# 02 — Graphics: GX on NV2A

The game talks to the GameCube GPU through GX. We implement the 110 GX entry points it uses as a state
machine (`xbox/port/gx/`) with two backends: OpenGL 2.1 + GLSL on the host (exact reference, debuggable)
and pbkit/NV2A on the Xbox.

## 1. What the game actually does with GX

1. **Frame**: `Render_before` / `Render` / `Render_done` in `main.cpp`; `main_sub.cpp` sets the render mode
   (`GXRenderModeObj Rmode`, EFB 640×448 → XFB), `GXSetDispCopy*`, `GXCopyDisp` at frame end, `VIFlush`,
   `VISetNextFrameBuffer`, `VIWaitForRetrace`. The game draws into ordering tables (`trans_ot.cpp`,
   `libgpu.cpp`: PS1-style `AddPrim/DrawOTag`), so draw order is already sorted by the game.
2. **Models**: `commonModelTrans` (trans.cpp) per `cModelInfo`: load pos/nrm matrices, set VCD/VAT, bind
   arrays, build the texture objects for the model's TPL (`GXInitTexObj` from `TEXHeader`), then per part:
   `shaderSetup` (TEV stages for material, blend textures, specular, bump, self-shadow, cast shadow,
   refraction, thermal), `GXSetBlendMode`, alpha compare, `GXCallDisplayList`.
3. **Skinning**: `calcWeightMat` builds a matrix palette; `CalcSk1_x/x2` transform s16 positions / s8 normals
   on the CPU into `pPosBuf/pNrmBuf[DblBufIdx]`. Rigid models use `vtxOrig/nrmOrig` with
   `GXMatrixIndex1u8` in the display list selecting among loaded matrices (hardware skinning by matrix index).
4. **Effects**: `esp*.cpp` immediate-mode quads/strips with `GXBegin`+`GXPosition3f32`/`GXTexCoord2f32`/
   `GXColor4u8`; `eff_sys.cpp` builds one shared 4-vertex quad display list for sprites; water
   (`Espgen42`) and sand (`Espgen43`) build their own display lists at runtime.
5. **Post-processing and render-to-texture**: `GXSetTexCopySrc/Dst` + `GXCopyTex` into textures used by
   the next pass (`filter0*`, `shadow.cpp`, refraction, thermal). `GXPeekZ` reads back depth.
6. **2D**: `mes.cpp`, `id_sys.cpp` (HUD with indirect texture warps), `eprintf.cpp`, `fade.cpp`,
   `cinesco.cpp` orthographic quads with `GXPosition2u16`/`GXPosition3s16`.
7. **Movies**: `sofdec.cpp` uploads Y/U/V planes as I8 textures and converts in TEV (swap tables, konst
   colours, 3 stages).

## 2. The GX state machine (backend-independent)

`gx_state.h` holds exactly what GX holds:

- Vertex descriptor (`GXSetVtxDesc` per attribute: none / direct / index8 / index16) and 8 vertex formats
  (`GXSetVtxAttrFmt`: component count, type s8/u8/s16/u16/f32/RGBA formats, fractional bits).
- Arrays (`GXSetArray`: base pointer + stride per attribute).
- Matrices: 10 position/normal matrices (3×4) by index 0..30 step 3, 10 texture matrices, projection
  (perspective/ortho), current matrix index.
- Colour channels: 2 channels × (ctrl, ambient, material), 8 light objects.
- Texgen: up to 8 texcoords (source, func, matrix, normalise, post-matrix).
- TEV: up to 16 stages × (order: texcoord/texmap/channel; colour in/op; alpha in/op; konst sel; swap
  mode; indirect params), 4 TEV registers, 4 konst colours, 4 swap tables, indirect stages (order,
  scale, matrices).
- Pixel: blend mode, z mode, z comp loc, alpha compare, alpha/colour update, dst alpha, dither, fog,
  cull mode, line width, scissor, viewport, pixel format, copy filter.
- 8 texture maps (texobj: image, size, format, wrap, filter, LOD, TLUT name), 20 TLUTs.
- Display-list recording (`GXBeginDisplayList`/`GXEndDisplayList`, only `dbmodule.cpp` and `eff_sys.cpp`).

Every setter stores into the state and marks a dirty bit; `GXBegin`/`GXCallDisplayList` resolve the
dirty state into backend calls. This mirrors how Dolphin's `VertexManager`/`PixelShaderGen` think about GX
and keeps backends tiny.

## 3. Display-list interpreter (`gx_dl.cpp`)

A GX display list is the raw FIFO stream: `0x80|prim<<3|vtxfmt` opcode byte, `u16 count`, then `count`
vertices, each attribute in VCD order (matrix index u8, position idx/direct, normal, colour0/1, tex0..7).
Primitive opcodes: `0x80 quads, 0x90 triangles, 0x98 strip, 0xA0 fan, 0xA8 lines, 0xB0 line strip, 0xB8
points`; `0x00` NOP; other opcodes (`0x08 CP reg`, `0x10 XF reg`, `0x61 BP reg`, `0x20..0x38` index
loads) do not appear in model data but are decoded defensively. The interpreter:

1. Reads the stream (big-endian u16 counts/indices; the converter can pre-swap these to LE so the Xbox
   reads natively — decide in `CT-03`; the host interpreter supports both via a flag).
2. For each vertex, resolves each attribute through the VCD/VAT/array into a fixed interleaved vertex
   (float3 pos, float3 nrm, RGBA8 col, float2 uv × n, u8 matrix index).
3. Emits to a CPU vertex buffer; quads → two triangles, fans/strips → triangles (or native strips on
   NV2A where allowed).

The expensive part is per-vertex dereferencing on a 733 MHz CPU. Mitigations, in order: cache decoded
vertex buffers per display list + array pointer + format hash (rigid models and room geometry never
change), decode skinned models once per frame, SSE for the s16→float conversions.

### Implementation notes (CT-08)

- **Files**: `gx_state.h/.cpp` (state, all 91 entry points of `include/gx.h` plus `GXBeginDisplayList`/
  `GXEndDisplayList`, power-on defaults from `GXInit.c`, texture cache, resolve-to-backend), `gx_dl.cpp`
  (FIFO, `GXBegin`, `GXCallDisplayList`, `gx_run_stream`, decoder, primitive expansion, `GXDrawTorus`),
  `gx_pipeline.h/.cpp` (`GxTevProgram`, `GxPipelineKey`, FNV-1a 64 hash), `gx_vertex.h`, `gx_null.cpp` +
  `re4port/gx_null_log.h`. Tests: `make -C xbox/port/tests gx && ./xbox/port/tests/gx`.
- **FIFO proxy**: `re4_xbox.h` defines the guard `__GXVERT_H__` so the game's `GXWGFifo`/inline-writer
  block in `include/gx.h` is skipped, and includes `re4port/gx_fifo.h`: `GXWGFifo` is a macro for
  `&re4_gx_fifo`, whose members `u8..f64` are proxy objects whose `operator=` appends the value (native
  order) to the primitive buffer opened by `GXBegin`. `GXBegin(prim, fmt, n)` computes the vertex size from
  VCD+VAT (direct data, 1/2-byte indices, 1-byte matrix indices) and the write that reaches `n*size`
  decodes and queues the primitive; there is no `GXEnd`. Writes with no open primitive are dropped (logged once).
- **Flush rules**: every state setter first draws the complete vertices of a partial primitive (warning,
  once per call site) and then emits the accumulated triangles/lines/points, so a draw always sees the
  state in force when its vertices arrived. Consecutive primitives of one class batch into one backend
  `draw()`; `GXCopyDisp`, `GXCopyTex`, `GXDrawDone`, `GXPixModeSync`, `GXPeekZ` and `gx_flush()` also emit.
  `GXCopyDisp` = `end_frame()`; its clear is applied at the next frame's first draw (`begin_frame` + `clear`).
- **Endian flag**: `gx_run_stream(.., big_endian)` applies to the stream's u16 counts/indices *and* the array
  data they index; `gx_set_dl_big_endian()` (default 0) is what `GXCallDisplayList` uses; immediate-mode
  vertices are always native. Byte-sequence colours (RGB8/RGBX8/RGBA6/RGBA8) have no byte order, RGB565/
  RGBA4 are u16. `GxVertex::col` is RGBA8 with bytes r,g,b,a in memory (`GX_RGBA8()` in `gx_vertex.h`).
- **Pipeline key**: hash covers the stage programs (up to `num_tev_stages`), texgens, the four channel
  controls, indirect stages, swap tables, blend/z/z-comp-loc/alpha-compare/cull/fog-type/update masks/
  dst-alpha/dither/pixel-fmt/line-width. Konst colours, TEV registers, channel colours, lights, indirect
  matrices and fog distances are uniforms: not hashed, but `set_pipeline` is re-sent when they change.
- **Deferred to backends (CT-09/CT-10)**: GX clip-space/z convention, cull-face mapping, texture format
  interpretation of raw (non-`RE4X`) images (`ConvertedImage` with `data_size 0` and `nv2a_format 0xFF`),
  EFB-copy formats, TLUT/thermal palette swaps, NBT binormal/tangent decode (unused by the game), post-
  transform texture matrices (stored, not passed), `GXSetCopyFilter`/gamma/YScale (stored only).
- **Known game-side issue**: `GXGetTexObjData()` returns the image pointer; `Espgen42.cpp`/`espgen45.cpp` add
  `0x80000000` to the SDK's physical address and need an `XBOX:` guard when those units are compiled.

## 4. Textures

| GX format | Bits | NV2A target                       | Notes                                  |
|-----------|------|-----------------------------------|----------------------------------------|
| I4        | 4    | A8R8G8B8 or L8 (expand)           | intensity → RGB=A=I                    |
| I8        | 8    | L8 (`SZ_Y8`) or A8                | shadow maps, movie planes              |
| IA4 / IA8 | 8/16 | A8L8 (`SZ_A8Y8`)                  |                                        |
| RGB565    | 16   | R5G6B5                            |                                        |
| RGB5A3    | 16   | A1R5G5B5 or A4R4G4B4 (per texel!) | expand to A8R8G8B8 unless all one kind |
| RGBA8     | 32   | A8R8G8B8                          | GC stores AR/GB in two tile planes     |
| CI4 / CI8 | 4/8  | expand through TLUT at convert    | palettes are per texture (`CLUTHeader`)|
| CI14X2    | 16   | expand                            | unused by the game as far as seen      |
| CMPR      | 4    | DXT1 (`SZ_DXT1`)                  | GC CMPR = DXT1 with 2×2 sub-block tiling and BE colours |

GC textures are stored in 4×4 (or 8×8/8×4) tiles; NV2A wants Morton-swizzled (or linear with pitch).
Conversion is done offline by the asset converter (`CT-04`) into NV2A-native swizzled images, with
mipmaps, so the Xbox `GXInitTexObj` is a pointer-and-format bind with no CPU work. The host backend
uploads the same converted images through `glTexImage2D` (so both backends read one asset format).
TLUTs: `GXInitTlutObj`/`GXLoadTlut` stay as no-ops on Xbox because CI textures are pre-expanded; the
thermal palette (`CalcTplAddrC8`, filter07) swaps palettes at runtime — handle with a small set of
pre-expanded variants or a 256-entry lookup at upload.

EFB copies: `GXCopyTex(dest, clear)` with the current `GXSetTexCopySrc/Dst` becomes "resolve the current
render target region into texture `dest`". On NV2A: render into an offscreen surface (set
`NV097_SET_SURFACE_*` to a contiguous buffer) and bind that buffer as a texture; `dest` is a handle, not a
CPU-readable image (the game never reads it back except via `GXPeekZ`). The host does FBO blits.

## 5. TEV → NV2A register combiners

GX TEV stage: `out = (d + ((1-c)·a + c·b) [+bias]) · scale`, clamped, with inputs from 4 registers, konst,
texture colour/alpha, rasterised channel colour, 0/1/0.5. NV2A general combiner stage: `AB + CD` (or
mux) with 4 inputs from texture0-3, primary/secondary colour, constant0/1, fog, spare0/1, each with
identity/invert/signed/expand/half-bias mappings, output scale ×1/×2/×4/÷2 and bias −0.5. TEV's lerp is
`a + c·(b−a)` = `(1−c)·a + c·b`, which the combiner does as `A=a,B=(1−c),C=b,D=c` → `AB+CD` exactly. The
`d +` term costs a second stage unless `d` is zero/one, which is the common case; the game's materials use
`GXSetTevOp(stage, GX_MODULATE/GX_DECAL/GX_BLEND/GX_REPLACE/GX_PASSCLR)` shortcuts plus hand-built
stages in `TextureBlend*`, `specularSetup`, `bumpSetup`, `alphaSetup`, `ShadowCastSetup`.

Resources: NV2A has 8 general stages + final, 2 constant colours **per stage** (`SAME_FACTOR_ALL` off
gives per-stage C0/C1), 4 textures. GX has up to 16 stages, 4 TEV regs (3 writable + prev), 4 konst
colours, 8 texmaps. The catalogue task (`CT-05`) enumerates the stage programs the game emits so the
mapping is done for the ~30 real cases, not for all of GX. A TEV program is compiled once per unique
configuration (hash of the TEV/texgen/channel state) into:

- host: a GLSL fragment shader generated from the state (Dolphin-style), cached;
- Xbox: a block of `NV097_SET_COMBINER_*` pushbuffer words (or an fp20 program via `fp20compiler` from a
  generated `.ps.cg`), cached in a small LRU.

Where the game exceeds NV2A (more than 8 stages, more than 4 textures in one pass, indirect stages beyond
what texture shaders can do), the compiler splits the material into two passes with additive blending or
drops the least visible stage, and logs it so the catalogue can be tuned.

Colour channels and lighting: `GXSetChanCtrl` lighting with 8 lights maps to NV2A fixed-function vertex
lighting (`NV097_SET_LIGHTING_ENABLE`, per-light diffuse/specular, attenuation) when the vertex pipeline is
fixed-function, or to a generated vertex program. Recommendation: a vertex program that does position by
matrix palette index, normal transform, up to 4 lights with the GX attenuation model, and texgen; the host
does the same in GLSL so both match.

Fog: GX `GXSetFog(type, start, end, near, far, color)` → NV2A `NV097_SET_FOG_MODE` linear/exp/exp2 with the
fog colour in the final combiner, as pbkit samples do.

## 6. Framebuffer and output

- EFB is 640×448 at 24-bit colour + 24-bit Z (`GXSetPixelFmt`), copied to 640×448 XFB; the game's
  orthographic UI assumes 640×448 (`ORTHO_T/B/L/R`, `TEST_Z` quad of 512×448 in trans.cpp).
- Xbox: render to a 640×480 (or 1280×720) surface with the GX viewport scaled; UI stays at 640×448 logical
  with a scale matrix. Duke's 480i/480p/720p detection is reused. Letterbox bars (`cinesco.cpp`) are drawn
  by the game; on 16:9 output we can later widen the projection and skip them.
- `GXSetCopyFilter`/`GXSetDispCopyYScale` (anti-aliasing/deflicker) → no-ops; NV2A multisampling optional.

## 7. Backends

```
struct GxBackend {
  void (*init)(int w, int h);         void (*begin_frame)();      void (*end_frame)();
  void (*set_render_target)(RT*);     void (*copy_to_texture)(...);
  void (*set_pipeline)(const PipelineKey*);   // compiled TEV/texgen/channel/blend/z state
  void (*draw)(const Vertex*, int n, Prim);   // interleaved vertices
  TexHandle (*create_texture)(const ConvertedImage*); void (*destroy_texture)(TexHandle);
  u32 (*peek_z)(int x, int y);
};
```

`gx_host_gl.cpp` is written first, verified against Dolphin screenshots of the same frames; `gx_nv2a.cpp`
follows Starship's `gfx_nv2a.cpp` structure (init, per-frame, pushbuffer helpers, VBO pool, texture
binds, state cache, batching).
