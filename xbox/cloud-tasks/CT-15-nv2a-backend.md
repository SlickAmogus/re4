# CT-15 — NV2A backend for the GX shim (local: needs Xbox hardware)

**Status:** partial (first pass: xbox/port/gx/gx_nv2a.cpp; remaining list in progress/CT-15.md) · LOCAL ONLY (hardware; xemu cannot validate pbkit register combiner output reliably)

## Goal

`xbox/port/gx/gx_nv2a.cpp` implements the `GxBackend` vtable (`port/include/re4port/gx_shim.h`) on pbkit,
so the game draws on a real Xbox. The backend-independent half exists (CT-08: `gx_state.cpp`, `gx_dl.cpp`,
`gx_pipeline.cpp`), and the TEV catalogue says every pipeline the game builds fits one NV2A pass
(`docs/tev-catalogue.md`). Follow `xbox/docs/02-GRAPHICS.md` and Starship's `gfx_nv2a.cpp`
(`C:\Claude\starfox\Starship\libultraship\src\graphic\Fast3D\backends\gfx_nv2a.cpp`) for the pbkit
mechanics summarised in `docs/06-LESSONS-FROM-PORTS.md`.

## Order

1. Init/frame: `pb_init`, specular enable, clip range, `pb_reset/target_back_buffer/busy/finished`;
   clear to the game's `GXSetCopyClear` colour; present on `GXCopyDisp` (main_sub.cpp `Render_swap`).
2. Vertex path: a vertex program (vp20) taking the interleaved `GxVertex`, applying the position matrix
   by matrix index, projection, colour passthrough, texcoords; batch draws into a write-combined pool.
3. Textures: `ConvertedImage` blobs bind directly (they are already NV2A-swizzled A8R8G8B8 / DXT1, see
   `re4conv/tpl.py`); texture objects in GPU-visible memory (the arena is contiguous memory).
4. Combiners: map `GxTevProgram` stages to `NV097_SET_COMBINER_*` (≤8 stages, final combiner, konst
   colours per stage); start with the catalogue's top 10 cases by frequency.
5. EFB copies → render targets (shadow maps, filters), `GXPeekZ` → read the depth surface.
6. Lighting: GX channels in the vertex program (up to 8 lights, GX attenuation).

## Acceptance

Title screen recognisable on hardware; a log line per unique pipeline the backend could not map.
