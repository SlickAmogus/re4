# CT-08 — GX state machine and display-list interpreter

**Status:** done (host, null backend) — 2026-10-11. All 91 `include/gx.h` entry points (+ display-list
recording) store state; FIFO/display-list decoding, primitive expansion, pipeline key/hash, texture cache
and the null backend log are tested (`xbox/port/tests/gx`: 2522 checks, the CT-05 catalogue's 98 cases
replay with 0 hash collisions). Remaining: nothing backend-independent; the model-`.bin` decode check
(deliverable 6, last item) needs a converted sample and the GL/NV2A backends are CT-09/CT-10. See
`02-GRAPHICS.md` "Implementation notes (CT-08)".

## Goal

The backend-independent half of the renderer (`02-GRAPHICS.md` sections 2–3): all 110 GX functions store
state; `GXBegin`/vertex FIFO writes/`GXCallDisplayList` produce interleaved vertices and a resolved
pipeline key for a backend. Tested headless with a null backend that records what it was asked to draw.

## Deliverables

1. `xbox/port/gx/gx_state.h/.cpp`: the state described in `02-GRAPHICS.md` section 2; every setter from
   `include/gx.h` implemented (enum values from `include/dolphin/gx/GXEnum.h`), dirty bits.
2. `xbox/port/gx/gx_vertex.h`: the interleaved `Vertex` struct (pos f32×3, nrm f32×3, col0/col1 RGBA8,
   uv f32×2 ×8, matrix index u8) and a growable CPU vertex buffer.
3. `xbox/port/gx/gx_dl.cpp`: the FIFO stream interpreter: `GXBegin(prim, fmt, n)` + the inline
   `GXPosition3f32`/`GXPosition3s16`/`GXPosition2u16`/`GXColor4u8`/`GXNormal3f32`/`GXNormal3s8`/
   `GXTexCoord2f32`/`GXTexCoord2s16`/`GXMatrixIndex1u8` writers (the game's `include/gx.h` writes into
   `GXWGFifo`; the port redefines that as a small inline byte writer into the current primitive buffer,
   which the interpreter then decodes using the VCD/VAT for format 0..7 — direct attributes are byte-packed
   exactly like a display list, so one decoder serves both), and `GXCallDisplayList(ptr, bytes)` with a
   `big_endian` flag. Quads → triangles, strips/fans expanded or passed through per backend capability.
4. `xbox/port/gx/gx_pipeline.h`: `PipelineKey` = hash of TEV/texgen/channel/light/blend/z/alpha/cull/fog
   state, built at draw time; a `TevProgram` struct describing stages in a backend-neutral way (this is
   what CT-09 compiles to GLSL and the Xbox backend to combiners).
5. `xbox/port/gx/gx_backend.h`: the backend vtable from `02-GRAPHICS.md` section 7; `gx_null.cpp`
   records calls to a log.
6. Tests: hand-built display lists (a quad with pos idx16 + tex idx16, a strip with matrix indices, s16
   positions with shift 4, s8 normals) decode to the expected floats; the TEV cases from CT-05 produce
   stable `PipelineKey`s; if the user attaches a model `.bin` (from CT-04's converter or raw BE with the
   flag), every part decodes to a vertex count consistent with `cPolyHeader::nPoly`.

## Acceptance

`make -C xbox/port/tests gx && ./xbox/port/tests/gx` passes; no backend code in this task beyond the
null backend.
