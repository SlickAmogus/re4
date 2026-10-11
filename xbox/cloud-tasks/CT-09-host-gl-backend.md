# CT-09 — Host OpenGL backend and TEV → GLSL

**Status:** open · depends on CT-08

## Goal

The reference renderer: `xbox/port/host/gx_host_gl.cpp` implements the backend vtable on OpenGL 2.1 +
GLSL 1.20 (works in software Mesa for headless screenshots), with a TEV → fragment shader generator and a
GX-transform vertex shader generator. Milestone M3 is "title screen and r100 render on the host".

## Deliverables

1. `gx_host_gl.cpp`: window/offscreen context via SDL2 (or EGL/OSMesa headless), textures from
   `ConvertedImage` blobs (`04-DATA.md` section 5; implement A8R8G8B8/R5G6B5/A1R5G5B5/A8L8/L8/DXT1 unswizzle
   or upload swizzled→linear), render targets for EFB copies (FBO), `peek_z` via `glReadPixels`.
2. `tev_glsl.cpp`: fragment shader from `TevProgram` (Dolphin's `PixelShaderGen` is the reference
   semantics: 8-bit fixed-point TEV math with bias/scale/clamp, konst/registers, swap tables, alpha
   compare, fog, indirect stages as texture-coordinate offsets); cache by `PipelineKey`.
3. `xf_glsl.cpp`: vertex shader: matrix palette (by matrix index attribute or current matrix), normal
   matrix, projection, up to 8 lights with GX channel/attenuation semantics (`GXSetChanCtrl`,
   `GXInitLightAttn/Spot/DistAttn`), texgens (position/normal/colour/texcoord sources, texture matrices,
   post matrices, normalise).
4. Screenshot harness: `re4_host --frames 120 --screenshot out.png` once CT-03 runs the game; until then
   `xbox/port/tests/gl_cases` renders each CT-05 JSON case on a quad and dumps PNGs for review.

## Acceptance

- The gl_cases test renders every catalogue case without GL errors and the PNGs are committed as
  references under `xbox/port/tests/ref/` (small, 64×64).
- Dolphin comparison: the user supplies Dolphin screenshots of the title and r100 for M3; include a
  `compare.py` (per-pixel diff heatmap) in the harness.
