# CT-05 — TEV / GX usage catalogue

**Status:** done — 2026-10-11 (98 cases, 47 setup functions; `xbox/docs/tev-catalogue.md`, `xbox/port/gx/tests/tev_cases.json` + schema + `check_tev_cases.py`). Open points are listed in the two "Appendix" sections of the doc (channel control of lit effects comes from trans_lit.cpp; a few runtime-selected values are `?`-marked).

## Goal

A complete, machine-readable catalogue of every distinct GX pipeline configuration the game builds, so
the register-combiner mapping (`02-GRAPHICS.md` section 5) is designed for the real cases and the host GLSL
generator can be tested against each.

## Method

Read every GX call site (list per file in `01-ANALYSIS.md`; ~4,000 sites). For each "pipeline setup
function" (a function that calls `GXSetNumTevStages` or `GXSetTevOrder`/`GXSetTevColorIn`), record the
sequence of TEV/texgen/channel/indirect/blend/z/alpha-compare calls it emits, including the counters it
uses (`tev_stage`, `tev_reg`, `tex_coord`, `tex_map`, `ind_stage`, `getTexCoord()` etc. in `trans.cpp`),
and the conditions that select variants (model flags, `Shader_type`, `info->blend_mode`, debug flags).

Primary sources: `src/game/trans.cpp` (`shaderSetup`, `materialSetup`, `TextureBlend/2/3`,
`specularSetup/2`, `GlobalIlluminationSetup`, `ShadowCastSetup`, `SelfShadowSetup`, `bumpSetup`,
`alphaSetup`, `RefractShaderSetup`, `ThermoShaderSetup`, `shaderReset`), `shadow.cpp`, `trans_lit.cpp`
(lights/channels), `esp_sub.cpp` + `esp*.cpp` + `espgen*.cpp` (effects), `filter00..0b.cpp`,
`id_sys.cpp`, `id_tex.cpp`, `mes.cpp`, `eprintf.cpp`, `fade.cpp`, `cinesco.cpp`, `cloth.cpp`, `mirror.cpp`,
`foot_shadow.cpp`, `objWep.cpp`, `TexRender.cpp`, `sofdec.cpp`, `main_sub.cpp`, `gx_sub.cpp`,
`src/Sscrn/ss_item_draw.cpp`, `ss_main.cpp`, `Espgen42/43.cpp`, `examine.cpp`.

## Deliverables

1. `xbox/docs/tev-catalogue.md`: one section per setup function: purpose, stage table (stage → order
   (coord/map/channel), colour in a,b,c,d → op/bias/scale/clamp/out, alpha the same, konst selects,
   swap modes), texgens, channel control/lights, indirect stages, blend/z/alpha-compare, how many
   textures bound, and an NV2A feasibility note: fits in 8 combiner stages / 4 textures? needs texture
   shaders (indirect)? needs two passes?
2. `xbox/port/gx/tests/tev_cases.json`: the same data in JSON (an array of pipeline states using the
   enum names from `include/dolphin/gx/GXEnum.h`), used by CT-08/CT-09 tests.
3. Totals: distinct stage counts, max stages, max textures per pass, which GX features are never used
   (so the shim can leave them unimplemented), and which `GXSetTevOp` shortcuts dominate.

## Acceptance

- Every function that calls `GXSetNumTevStages` in the non-debug units appears in the catalogue.
- JSON validates against a schema included in the same directory; a 20-line Python check loads it.
