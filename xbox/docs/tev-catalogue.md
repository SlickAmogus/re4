# TEV / GX usage catalogue (CT-05)

Every distinct GX pipeline configuration the game builds, read from the source of each pipeline-setup
function (any function that calls `GXSetNumTevStages`, `GXSetTevOrder`, `GXSetTevColorIn` or
`GXSetTevOp`). The machine-readable form is `xbox/port/gx/tests/tev_cases.json` (schema
`tev_cases.schema.json`, checked by `python3 xbox/port/gx/tests/check_tev_cases.py`); CT-08's test
runner replays every case into the GX state machine and CT-09 renders each one.

The catalogue was produced in two halves, kept below as Part A (the model pipeline: `trans.cpp`,
`shadow.cpp`, `trans_lit.cpp`, mirror, foot shadow, weapons, cloth, sofdec, Sscrn, misc) and Part B
(effects `esp*`, water/sand `espgen*`, post-process `filter0*`, HUD `id_sys`, text, fade, letterbox).
Each part has its own conventions section and totals; the numbers below are for the whole game.

## Totals

| | |
|---|---|
| cases | 98 |
| stage-count histogram | 1: 54, 2: 14, 3: 8, 4: 10, 5: 4, 6: 1, 10: 1, runtime: 6 |
| max textures referenced by one case | 7 (`trans_shadersetup_full`, a synthetic worst-case composition; observed programs use at most 4) |
| cases using indirect texturing | 18 (always one indirect stage, ITF_8 / ITS_1, warp or bump-XYZ) |
| cases sampling an EFB copy | 41 |
| cases that need more than one NV2A pass | 1 |
| cases that do not fit 8 combiner stages | 2 |
| cases that need NV2A texture shaders | 17 |

Headline findings for the NV2A mapping (details in the part totals):

- The model pipeline (`shaderSetup`) never uses `GXSetTevOp`; it hand-builds 2–6 stages and ends with a
  `GX_CS_SCALE_4` pass on CPREV, so the whole model path runs at quarter range (precision note in Part A).
  Elsewhere the shortcuts are `GX_PASSCLR` (11 stages), `GX_MODULATE` (10), `GX_REPLACE` (4); `GX_DECAL`
  and `GX_BLEND` are never used.
- GX features never used anywhere: `GX_TEV_DIVIDE_2`, `GX_TEVREG2`, `GX_TEV_COMP_*` compare ops,
  fractional konst selects, `GX_TG_BUMP*`/`GX_TG_SRTG` texgens, texgen normalise, post matrices other
  than identity, `GX_DF_SIGN`, `GX_AF_SPEC`, `GX_BM_LOGIC`/`GX_BM_SUBTRACT` (except through runtime esp
  fields), alpha-compare `AND`/`XOR`/`XNOR`, z compares other than `LEQUAL`/`ALWAYS`. The shim can leave
  these unimplemented and log.
- Needs special treatment: `sofdec_settevprm` (per-stage swizzles turning Y/U/V I8 planes into RGB —
  a native YUY2 texture is cheaper), `shadowShaderSetup2` with more than one light (raster channel
  select through swap tables), `GXSetDstAlpha` (the shadow-map pass writes a constant alpha with colour
  writes off, so the final combiner alpha must be forced), the heat-shimmer / water warps (NV2A
  `OFFSET_TEXTURE_2D` with a DSDT-converted offset map), and the filters' lit-with-zero-lights channels
  that square the vertex colour.

---

## TEV / GX usage catalogue — part A (CT-05)

Part A of the CT-05 catalogue: `src/game/trans.cpp`, `shadow.cpp`, `trans_lit.cpp`, `mirror.cpp`, `foot_shadow.cpp`, `objWep.cpp`, `main_sub.cpp`, `gx_sub.cpp`, `libgpu.cpp`, `cloth.cpp`, `TexRender.cpp`, `examine.cpp`, `sofdec.cpp`, `src/Sscrn/ss_item_draw.cpp`, `ss_main.cpp`, `EtcModel.cpp` (and `t_prim.cpp` in the appendix). Part B (effects, filters, id/mes/eprintf/fade/cinesco/eff_sys/dbmodule) is a separate file; the two merge into `tev-catalogue.md` / `tev_cases.json`.

The machine-readable form of every case below is `xbox/port/gx/tests/tev_cases.json` (schema `tev_cases.schema.json`, checked by `check_tev_cases.py`); the tables in this file are rendered from that JSON.

### Method and conventions

- A *pipeline setup function* is one that calls `GXSetNumTevStages`, `GXSetTevOrder`, `GXSetTevColorIn` or `GXSetTevOp`. Every such function in the part-A files has at least one case; helper blocks called from `shaderSetup` are inlined into the case of the variant that uses them, and also appear as their own 'plain + block' case so each block is traceable on its own.
- Numeric arguments in the decomp were decoded with `include/dolphin/gx/GXEnum.h` (e.g. `GXSetTevColorIn(st, 0xF, 8, 0xA, 0xF)` = `ZERO, TEXC, RASC, ZERO`; channel 4 = `GX_COLOR0A0`, 5 = `GX_COLOR1A1`, 0xFF = `GX_COLOR_NULL`; mtx 0x3C = `GX_IDENTITY`, 0x1E + 3n = `GX_TEXMTXn`, post 0x7D = `GX_PTIDENTITY`). The TEV equation is `out = d + ((1-c)·a + c·b) [+bias] · scale`, so `ZERO, TEXC, RASC, ZERO` is `TEXC·RASC` and `CPREV, C0, KONST, ZERO` is `lerp(CPREV, C0, KONST)`.
- `GXSetTevOp(stage, mode)` shortcuts are recorded in the *via* column and expanded with the tables in `src/lib/GXTev.c` (`TEVCOpTableST0/ST1`, `TEVAOpTableST0/ST1`): stage 0 uses `RASC/RASA` where stages ≥ 1 use `CPREV/APREV`; all five modes write `ADD / TB_ZERO / SCALE_1 / clamp / TEVPREV`. `GXSetTevOp` does not touch konst selects or swap modes.
- `GXSetTevIndWarp(st, ind, signed, replace, mtx)` expands to `GXSetTevIndirect(st, ind, ITF_8, signed ? ITB_STU : ITB_NONE, mtx, replace ? ITW_0 : ITW_OFF ×2, FALSE, FALSE, ITBA_OFF)`; `GXSetTevIndBumpXYZ(st, ind, mtx)` = `GXSetTevIndirect(st, ind, ITF_8, ITB_STU, mtx, ITW_OFF, ITW_OFF, FALSE, FALSE, ITBA_OFF)`.
- Values only known at run time are written as strings starting with `?` and explained in the notes. The trans.cpp counters make every block's stage/coord/map numbers relative; each table shows the concrete numbers of the stated variant.
- Realistic variants are enumerated, not the cross product: the per-block cases combine in the `shaderSetup` order given at the top of the trans.cpp section, and `trans_shadersetup_full` shows the worst plausible composition.

### src/game/trans.cpp

The model pipeline. `shaderSetup` (trans.cpp:1323-1425) is the only place that composes a multi-block TEV program: it zeroes the six counters (`tev_stage`, `tev_reg`, `tev_kcolor`, `tex_map`, `tex_coord`, `ind_stage`, trans.cpp:1336-1341), then appends blocks in a fixed order: `SelfShadowSetup` **or** `ShadowCastSetup` (optional) → `materialSetup` (always; dispatches to `TextureBlend`/`TextureBlend2`/`TextureBlend3` when `ModelTexInfo.flags & 4`) → `specularSetup2` **or** (`specularSetup` + `bumpSetup`) → `GlobalIlluminationSetup` → `alphaSetup` → the final colour-scale stage, and finally `GXSetNumTevStages/TexGens/IndStages` from the counters. Every block allocates its stage/map/coord/konst/register through the `get*()` helpers (trans.cpp:137-290), which only range-check, so stage N of a block is simply 'the next free stage' — the concrete numbers in the tables below are for the stated variant; in any other composition each block shifts by the stages the previous blocks used. `ThermoShaderSetup` and `RefractShaderSetup` replace the whole program (trans.cpp:1328-1335). Surrounding state is set by `ModelRender` (trans.cpp:977-1016: alpha compare ALWAYS, z by `z_mode`, lighting channels, alpha update + `GXSetDstAlpha(1, 0)`) and `commonModelTrans` (trans.cpp:1019-1273: cull by `CullMode`, blend by `info->blend_mode`, texture objects, indirect reset after each part). `shaderReset` (trans.cpp:2447-2473) runs after each model: indirect stages off, channels COLOR0/COLOR1 disabled, alpha compare ALWAYS — it is not a pipeline setup and has no case.

#### `trans_shadersetup_plain` — shaderSetup (trans.cpp:1323)

**Purpose.** Common model part: one material texture modulated by the lit vertex colour, then the colour-scale stage.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** no shadow mng, !(ModelTexInfo.flags&4), !(part->flag & 0x93), !(be_flag & 0x01000000), !(part->flag & 4)

**Stages:** 2 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** shaderSetup (trans.cpp:1336-1341 counters reset, 1361-1368 colIn=RASC alphaIn=RASA, 1368 materialSetup, 1382-1424 final stage + GXSetNumTevStages/TexGens/IndStages). Texture map 0 = org_LoadTexObj(material_tex) (trans.cpp:316-325; ids >= 0xF8 are TexRender render-to-texture objects). GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0. Channels from LightSetModel (trans_lit.cpp:59-145) via ModelRender (trans.cpp:1000); see trans_lit_* cases for the variants. shaderSetup final stage (trans.cpp:1383-1421): CPREV scaled by TevScaleGroup; groups 0/1 use gxCsScale[] = 2 -> GX_CS_SCALE_4 (default), group 4 x1, 5 x2, 6 x4.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 2 combiner stages (tex*diffuse, x4 scale). The x4 final scale on a quarter-range pipeline costs 2 bits of the 9-bit signed combiner precision; prefer folding the x4 into the light colours in the shim.

#### `trans_shadersetup_blend0` — shaderSetup -> materialSetup -> TextureBlend (trans.cpp:1441)

**Purpose.** Material with blend type 0: a second texture lerped over the base by the info's blendRatio (konst).

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** ModelTexInfo.flags&4 && blendType==0 && 0 < blendRatio < 0xFF, one matching blend-table entry

**Stages:** 4 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVREG0 | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | CPREV, C0, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | APREV, A0, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1462-1466 (TextureBlend base) materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1493-1497 blend texture * RASC -> REG0 (getTevRegCur with tev_reg 0). **st2**: trans.cpp:1510-1514 lerp(CPREV, C0, K0) / lerp(APREV, A0, K0.a); K0 = blendRatio (trans.cpp:1504-1508). **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1487-1491 (GX_TEXMTX0 = getTexMtx() of the base slot when flags&1)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = {blendRatio, blendRatio, blendRatio, blendRatio}

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** materialSetup dispatches on ModelTexInfo.blendType when flags&4 (trans.cpp:1769-1783). The blend table (tbl[0] entries of {part tex id | 0xF7, blend tex id}) is scanned for the part's texture; one matching entry is the realistic case (the loop would emit one more texture + 2 stages per extra match). blendRatio 0 -> base stage only; 0xFF -> loadBlendTex swaps the base texture (trans.cpp:1428-1438) and no extra stage. Note getTexMtx() is read before the counters advance, so the blend stage's scroll matrix is the base slot's GX_TEXMTX0. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 4 stages, 2 textures; the lerp is one AB+CD stage with the ratio as a per-stage constant.

#### `trans_shadersetup_blend1` — shaderSetup -> materialSetup -> TextureBlend2 (trans.cpp:1523)

**Purpose.** Material with blend type 1: colour lerp as type 0, alpha = base TEXA*RASA, blend texture alpha into REG0 unused by the lerp.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** flags&4 && blendType==1 && 0 < blendRatio < 0xFF

**Stages:** 4 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVREG0 | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | CPREV, C0, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1544-1548 base TEXC*RASC, alpha TEXA*RASA. **st1**: trans.cpp:1575-1579 blend texture * RASC -> REG0, alpha TEXA -> REG0. **st2**: trans.cpp:1592-1596 lerp(CPREV, C0, K0); alpha passes APREV (A0 is not used). **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1569-1573

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = {blendRatio x4}

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** materialSetup dispatches on ModelTexInfo.blendType when flags&4 (trans.cpp:1769-1783). The blend table (tbl[0] entries of {part tex id | 0xF7, blend tex id}) is scanned for the part's texture; one matching entry is the realistic case (the loop would emit one more texture + 2 stages per extra match). blendRatio 0 -> base stage only; 0xFF -> loadBlendTex swaps the base texture (trans.cpp:1428-1438) and no extra stage. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. Same shape as blend0.

#### `trans_shadersetup_blend2` — shaderSetup -> materialSetup -> TextureBlend3 (trans.cpp:1605)

**Purpose.** Material with blend type 2: the blend texture's alpha, scaled by blendRatio, drives the lerp (per-texel blend mask).

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** flags&4 && blendType==2 && blendRatio != 0; static use_alp == 1 (the use_alp==0 path, trans.cpp:1703-1731, is dead)

**Stages:** 5 · **texgens:** 3 · **channels:** 1 · **textures bound:** 3

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVREG0 | - | - | - |
| 2 | TEXCOORD2 | TEXMAP2 | COLOR_NULL | - | ZERO, KONST, TEXA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG1 | ZERO, KONST, TEXA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG1 | KCSEL_K0 / KASEL_K0_A | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | CPREV, C0, A1, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | APREV, RASA, A1, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 4 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1621-1625 (TextureBlend3 base) materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1656-1660 blend texture * RASC -> REG0, alpha RASA -> REG0. **st2**: trans.cpp:1687-1690 (blendRatio <= 0xFF): KONST*TEXA -> REG1 in colour and alpha; the same blend texture is loaded again on a new map/coord (trans.cpp:1664-1672). blendRatio > 0xFF variant (1682-1685): colour (ZERO, ONE, TEXA, KONST) = TEXA+KONST, alpha (KONST, KONST, ZERO, TEXA) = KONST+TEXA. **st3**: trans.cpp:1696-1700 lerp(CPREV, C0, A1) / lerp(APREV, RASA, A1); alphaIn is the shaderSetup alphaIn (RASA). **st4**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1645-1649
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1668-1672

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = {blendRatio x4}

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** materialSetup dispatches on ModelTexInfo.blendType when flags&4 (trans.cpp:1769-1783). The blend table (tbl[0] entries of {part tex id | 0xF7, blend tex id}) is scanned for the part's texture; one matching entry is the realistic case (the loop would emit one more texture + 2 stages per extra match). blendRatio 0 -> base stage only; 0xFF -> loadBlendTex swaps the base texture (trans.cpp:1428-1438) and no extra stage. Three texmaps are bound but only two distinct textures (map1 and map2 hold the same blend texture). GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 5 stages; map2 duplicates map1 so NV2A needs only 2 texture units (sample once, use .a and .rgb).

#### `trans_shadersetup_specular_konst` — shaderSetup -> specularSetup (type 0) (trans.cpp:1798)

**Purpose.** Material plus a normal-mapped environment (specular) texture tinted by a konst colour and added to the base.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** (part->flag & 0x13) && !(part->flag & 0x80) && specular_type == 0 && !(part->flag & 1)

**Stages:** 4 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, KONST, C0, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / - | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1856-1867 specular TEXC*RASC -> REG0 (scale x1 since flag==0); alpha passes APREV. **st2**: trans.cpp:1871-1879 CPREV + K0*C0 (no GXSetTevKAlphaSel for this stage). **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX3x4(TG_NRM) × TEXMTX1 — trans.cpp:1854-1855 normal -> (0.5,0.5)-centred env-map lookup: trans(0.5,0.5,1) * scale(decay, -decay, 0) * specular_mat (view-space normal matrix, updateMatrices trans.cpp:2641)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = {specular_r, specular_g, specular_b, 0xFF} or info->color2

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** specularSetup (trans.cpp:1798-1896) runs when part->flag & 0x13; type = part->specular_type. The texture is Specular[part->specular_tex] from the core archive (trans.cpp:1842-1848) or, with flag&0x10, the model's own specTexOrg (1850). Colour = part->specular_rgb or info->color2 (1812-1822); decay scale halves for 8-bit normals. flag argument is 0 at every call site, so the x4 variant at 1862 never runs. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. Env-map texgen (normal through a 3x4 matrix) is a vertex-program texgen on NV2A; 4 combiner stages, 2 textures.

#### `trans_shadersetup_specular_texalpha` — shaderSetup -> specularSetup (type 1) (trans.cpp:1884)

**Purpose.** Material plus a specular texture whose alpha brightens the base: CPREV + TEXA*CPREV.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** (part->flag & 0x13) && !(part->flag & 0x80) && specular_type == 1

**Stages:** 3 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXA, CPREV, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1885-1888 CPREV + TEXA*CPREV -> PREV. **st2**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX3x4(TG_NRM) × TEXMTX1 — trans.cpp:1854-1855 normal -> (0.5,0.5)-centred env-map lookup: trans(0.5,0.5,1) * scale(decay, -decay, 0) * specular_mat (view-space normal matrix, updateMatrices trans.cpp:2641)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** specularSetup (trans.cpp:1798-1896) runs when part->flag & 0x13; type = part->specular_type. The texture is Specular[part->specular_tex] from the core archive (trans.cpp:1842-1848) or, with flag&0x10, the model's own specTexOrg (1850). Colour = part->specular_rgb or info->color2 (1812-1822); decay scale halves for 8-bit normals. flag argument is 0 at every call site, so the x4 variant at 1862 never runs. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 3 stages.

#### `trans_shadersetup_specular_bump` — shaderSetup -> specularSetup (type 0) + bumpSetup (trans.cpp:2334)

**Purpose.** Specular (konst) material whose env-map lookup is perturbed by a bump texture through an indirect stage (GXSetTevIndBumpXYZ).

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** (part->flag & 1) (bump; implies flag&0x13) && !(part->flag & 0x80), specular_type 0

**Stages:** 4 · **texgens:** 3 · **channels:** 1 · **textures bound:** 3

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | bump_xyz INDTEXSTAGE0 ITM_0 signed |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, KONST, C0, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / - | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1856-1867 as specular_konst; the indirect offset is applied to this stage's texcoord. **st2**: trans.cpp:1871-1879. **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

Indirect on stage 1: GXSetTevIndBumpXYZ(g_specular_tev_stage, IND0, GX_ITM_0) trans.cpp:2366 = GXSetTevIndirect(ITF_8, ITB_STU, ITM_0, ITW_OFF, ITW_OFF, add_prev 0, utc_lod 0, ITBA_OFF)

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX3x4(TG_NRM) × TEXMTX1 — trans.cpp:1854-1855 normal -> (0.5,0.5)-centred env-map lookup: trans(0.5,0.5,1) * scale(decay, -decay, 0) * specular_mat (view-space normal matrix, updateMatrices trans.cpp:2641)
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:2362 bump texcoord

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD2, map TEXMAP2, scale ITS_1/ITS_1, matrix GX_ITM_0 = 0.4 * rows 0,1 of the view-space inverse-transpose model matrix (row 1 y negated), scale exp 0 (updateMatrices trans.cpp:2655-2663) — bumpSetup trans.cpp:2361-2366: bump texture (part->bump_tex, or the blend table's match when flagsDC&4) as indirect stage 0

**Konst / registers / swap tables.** KCOLOR0 = {specular rgb, 0xFF}

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** specularSetup (trans.cpp:1798-1896) runs when part->flag & 0x13; type = part->specular_type. The texture is Specular[part->specular_tex] from the core archive (trans.cpp:1842-1848) or, with flag&0x10, the model's own specTexOrg (1850). Colour = part->specular_rgb or info->color2 (1812-1822); decay scale halves for 8-bit normals. flag argument is 0 at every call site, so the x4 variant at 1862 never runs. bumpSetup (trans.cpp:2334-2372) clears __GXSetIndirectMask then binds the bump map as the only indirect stage; commonModelTrans resets ind stages after each part (trans.cpp:1247-1258). GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 3 · texture shader: yes · passes: 1. Indirect bump needs an NV2A texture-shader offset stage (PS_TEXTUREMODES_BUMPENVMAP) feeding the env-map unit; 4 combiners, 3 textures. The 3x4 normal texgen must be computed in the vertex program and the bump offset added in the texture shader.

#### `trans_shadersetup_specular2` — shaderSetup -> specularSetup2 (trans.cpp:1898)

**Purpose.** Bump-mapped specular variant: bump texture sampled twice, the second through an indirect warp from the IndTex ramp, difference x base.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** (part->flag & 0x80) && (part->flag & 3)

**Stages:** 5 · **texgens:** 4 · **channels:** 1 · **textures bound:** 4

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, ONE, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | ZERO, C0, ONE, TEXC | SUB/ADDHALF/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | warp INDTEXSTAGE0 ITM_1 signed |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, CPREV, C0, ZERO | ADD/ZERO/SCALE_2/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 4 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:1917-1922 bump TEXC -> REG0 (unperturbed sample). **st2**: trans.cpp:1937-1939, 1975-1978: TEXC(perturbed bump) - C0 + 0.5 -> REG0 (op SUB, bias ADDHALF); the bump texture is bound again on map 2 with scroll matrix GX_TEXMTX2 (bp_mx/bp_my statics = 0). **st3**: trans.cpp:1982-1986 CPREV*C0 x2 -> PREV. **st4**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

Indirect on stage 2: GXSetTevIndWarp(st, IND0, TRUE, FALSE, GX_ITM_1) trans.cpp:1970

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1917
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × TEXMTX2 — trans.cpp:1937-1938 bump scroll
- TEXCOORD3 = TG_MTX3x4(TG_NRM) × TEXMTX3 — trans.cpp:1965-1966 normal env lookup (scale 0.5) for the indirect map

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD3, map TEXMAP3, scale ITS_1/ITS_1, matrix GX_ITM_1 = rotation(ang 0) * (pow*mul_x, pow*mul_y) = (-0.02, 0.02) scale exp 1 (trans.cpp:1943-1954) — trans.cpp:1957,1968-1969 IndTex[0] (core 'ind' tpl) on map 3

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** specularSetup2 (trans.cpp:1898-1991) replaces specularSetup+bumpSetup when flag&0x80. Four texmaps: material, bump (twice: maps 1 and 2 are the same texture object) and IndTex[0]. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 4 · texture shader: yes · passes: 1. Exactly 4 textures (3 distinct). Map 2 needs a texture-shader offset from IndTex[0]; 5 combiners. The SUB + ADDHALF stage maps to a signed combiner (A*B + C*D with D=-1 input mapping, bias 0.5).

#### `trans_shadersetup_gi` — shaderSetup -> GlobalIlluminationSetup (trans.cpp:1993)

**Purpose.** Material plus the normal-mapped global-illumination texture multiplied in (x2).

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** (m->be_flag & 0x01000000) && !DPF_GLB_ILM

**Stages:** 3 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_2/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:2018-2027 GlobalIlmTex[0] TEXC*CPREV x2 -> PREV. **st2**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD1 = TG_MTX3x4(TG_NRM) × TEXMTX1 — trans.cpp:2004-2022 trans(0.5,0.5,1)*scale(0.5 or 0.25 for 8-bit normals)*specular_mat

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** GlobalIlluminationSetup (trans.cpp:1993-2033) is appended after specular/bump; combines freely with the other cases. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 3 stages, 2 textures, env texgen in the vertex program.

#### `trans_shadersetup_alpha` — shaderSetup -> alphaSetup (trans.cpp:2374)

**Purpose.** Material plus a separate alpha texture (part->alpha_tex) multiplied into the output alpha, with alpha test.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** part->flag & 4

**Stages:** 3 · **texgens:** 1 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD0 | TEXMAP1 | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, APREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st1**: trans.cpp:2398-2402 colour passes CPREV, alpha = TEXA*APREV; coord = g_material_tex_coord (0) unless info->flagsDC&8 (own GX_TEXCOORDn, identity texgen, trans.cpp:2392-2394). **st2**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX0 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc after texture (FALSE) · alpha compare GREATER ?m->alpha_omit (part->alpha_omit when 0xFF) AOP_OR GREATER 255 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** alphaSetup (trans.cpp:2374-2407): GXSetZCompLoc(FALSE) (z test after texturing), GXSetAlphaCompare(GREATER ref OR GREATER 0xFF) i.e. alpha > ref. It is the last block before the final stage, so it combines with any of the other variants. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 3 stages, 2 textures; alpha test is native (NV097_SET_ALPHA_FUNC GREATER).

#### `trans_shadersetup_castshadow_kind1` — shaderSetup -> ShadowCastSetup (Kind 1) (trans.cpp:2123)

**Purpose.** Projected light texture as a shadow mask: material lit colour x (1 - tex*lightcolour).

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** g_pShdMng && w->Kind == 1 (Flag&4 variant in stage notes)

**Stages:** 4 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR_NULL | - | ZERO, TEXC, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | RASC, ZERO, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2137-2141 TEXC*K0 -> PREV (Flag&4, 2130-2134: (KONST, ZERO, TEXC, ZERO) = K0*(1-TEXC)); alpha 0. **st1**: trans.cpp:2145-2149 RASC*(1-CPREV), alpha RASA. **st2**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2101-2105 projective: mng->texMat (light view-projection, make_fix_light) * parts matrix
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX1 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = mng->pLight->Col (rgba)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** ShadowCastSetup (trans.cpp:2083-2243) runs when g_pShdMng != 0: STA_USE_SHADOW_LIGHT && be_flag 0x02000000 && !DPF_CAST_SHADOW && STA_USE_CAST_SHADOW && GetCastShadowMngPtr(m) finds a type-4 light of Kind 1..4 covering the model (trans.cpp:1030-1057, shadow.cpp:1770). Map 0 = the light's room texture RoomGetTexObj(w->Tex_no) (+TLUT, trans.cpp:2107-2111). K0 = light colour (trans.cpp:2117-2121). materialSetup then runs with colIn=CPREV, alphaIn=APREV (trans.cpp:1363-1368). w->Flag & 4 selects the inverted texture form noted per stage. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 4 stages, 2 textures; projective texgen (POS through 3x4 + q divide) is standard.

#### `trans_shadersetup_castshadow_kind2` — shaderSetup -> ShadowCastSetup (Kind 2) (trans.cpp:2151)

**Purpose.** Projected light texture added to the lit colour: RASC + tex*lightcolour, then material.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** g_pShdMng && w->Kind == 2

**Stages:** 3 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, KONST, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2162-2166 RASC + TEXC*K0 (Flag&4, 2156-2160: RASC + K0*(1-TEXC)); alpha RASA. **st1**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st2**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2101-2105 projective: mng->texMat (light view-projection, make_fix_light) * parts matrix
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX1 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Konst / registers / swap tables.** KCOLOR0 = mng->pLight->Col (rgba)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** ShadowCastSetup (trans.cpp:2083-2243) runs when g_pShdMng != 0: STA_USE_SHADOW_LIGHT && be_flag 0x02000000 && !DPF_CAST_SHADOW && STA_USE_CAST_SHADOW && GetCastShadowMngPtr(m) finds a type-4 light of Kind 1..4 covering the model (trans.cpp:1030-1057, shadow.cpp:1770). Map 0 = the light's room texture RoomGetTexObj(w->Tex_no) (+TLUT, trans.cpp:2107-2111). K0 = light colour (trans.cpp:2117-2121). materialSetup then runs with colIn=CPREV, alphaIn=APREV (trans.cpp:1363-1368). w->Flag & 4 selects the inverted texture form noted per stage. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 3 stages.

#### `trans_shadersetup_castshadow_kind3` — shaderSetup -> ShadowCastSetup (Kind 3) (trans.cpp:2169)

**Purpose.** Spot-lit projected texture (channel 1) as a shadow mask on the base lighting; two colour channels.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** g_pShdMng && w->Kind == 3

**Stages:** 5 · **texgens:** 2 · **channels:** 2 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR1A1 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, CPREV, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | RASC, ZERO, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 3 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 4 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2170-2174 TEXC*RASC(COLOR1A1 = cast spot light) -> PREV. **st1**: trans.cpp:2188-2192 CPREV*K0 (Flag&4, 2181-2185: K0*(1-CPREV)); alpha 0. **st2**: trans.cpp:2196-2200 RASC*(1-CPREV), alpha RASA. **st3**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st4**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2101-2105 projective: mng->texMat (light view-projection, make_fix_light) * parts matrix
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX1 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123
- COLOR1: enabled, amb SRC_REG, mat SRC_REG, lights `?GX_LIGHT(n) with n = model light count (trans.cpp:2068-2073)`, DF_CLAMP, AF_SPOT — SetCastShadowLight trans.cpp:2035-2081: spot light (cutoff 89, GX_SP_SHARP) at the shadow light, white x LightPow; mat1 white, amb1 {0,0,0,0xFF}
- ALPHA1: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans.cpp:2074

**Konst / registers / swap tables.** KCOLOR0 = mng->pLight->Col (rgba)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** ShadowCastSetup (trans.cpp:2083-2243) runs when g_pShdMng != 0: STA_USE_SHADOW_LIGHT && be_flag 0x02000000 && !DPF_CAST_SHADOW && STA_USE_CAST_SHADOW && GetCastShadowMngPtr(m) finds a type-4 light of Kind 1..4 covering the model (trans.cpp:1030-1057, shadow.cpp:1770). Map 0 = the light's room texture RoomGetTexObj(w->Tex_no) (+TLUT, trans.cpp:2107-2111). K0 = light colour (trans.cpp:2117-2121). materialSetup then runs with colIn=CPREV, alphaIn=APREV (trans.cpp:1363-1368). w->Flag & 4 selects the inverted texture form noted per stage. GXSetNumChans(2) + SetCastShadowLight (trans.cpp:2113-2115). GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 5 stages; the second colour channel is NV2A's secondary (specular) colour, lit by one spot light in the vertex program.

#### `trans_shadersetup_castshadow_kind4` — shaderSetup -> ShadowCastSetup (Kind 4) (trans.cpp:2203)

**Purpose.** Spot-lit projected texture (channel 1) x light colour added to the base lighting; two colour channels.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** g_pShdMng && w->Kind == 4

**Stages:** 4 · **texgens:** 2 · **channels:** 2 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR1A1 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, CPREV, KONST, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 2 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2220-2224 TEXC*RASC(COLOR1A1) (Flag&4, 2204-2208: RASC*(1-TEXC)). **st1**: trans.cpp:2230-2234 RASC + CPREV*K0, alpha RASA. **st2**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2101-2105 projective: mng->texMat (light view-projection, make_fix_light) * parts matrix
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX1 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123
- COLOR1: enabled, amb SRC_REG, mat SRC_REG, lights `?GX_LIGHT(n) with n = model light count (trans.cpp:2068-2073)`, DF_CLAMP, AF_SPOT — SetCastShadowLight trans.cpp:2035-2081: spot light (cutoff 89, GX_SP_SHARP) at the shadow light, white x LightPow; mat1 white, amb1 {0,0,0,0xFF}
- ALPHA1: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans.cpp:2074

**Konst / registers / swap tables.** KCOLOR0 = mng->pLight->Col (rgba)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** ShadowCastSetup (trans.cpp:2083-2243) runs when g_pShdMng != 0: STA_USE_SHADOW_LIGHT && be_flag 0x02000000 && !DPF_CAST_SHADOW && STA_USE_CAST_SHADOW && GetCastShadowMngPtr(m) finds a type-4 light of Kind 1..4 covering the model (trans.cpp:1030-1057, shadow.cpp:1770). Map 0 = the light's room texture RoomGetTexObj(w->Tex_no) (+TLUT, trans.cpp:2107-2111). K0 = light colour (trans.cpp:2117-2121). materialSetup then runs with colIn=CPREV, alphaIn=APREV (trans.cpp:1363-1368). w->Flag & 4 selects the inverted texture form noted per stage. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 4 stages, 2 textures, 2 colour channels.

#### `trans_shadersetup_selfshadow` — shaderSetup -> SelfShadowSetup (trans.cpp:2245)

**Purpose.** Self-shadowing: light-space depth from the IndTex ramp compared against the model's own shadow map (EFB copy), then material.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** STA_SELF_SHADOW && isSelfUse && GetSelfShadowMng(i)->pModel[0] == m (trans.cpp:1345-1357); SelfShadowLevel = 1

**Stages:** 6 · **texgens:** 3 · **channels:** 1 · **textures bound:** 3

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ONE, TEXA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR_NULL | - | ONE, ZERO, TEXA, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR_NULL | - | ZERO, CPREV, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, RASC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 4 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 5 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2287-2291 TEXA of IndTex[0] (depth ramp, s = light-space z*0.0003) -> PREV; alpha RASA. **st1**: trans.cpp:2303-2307 CPREV + (1-TEXA) with TEXA = shadow-map depth (mng->texObj, projective) -> PREV (clamped depth compare); alpha APREV. **st2**: trans.cpp:2316-2320 CPREV*CPREV, repeated w->SelfShadowLevel times (1 shown; data-driven). **st3**: trans.cpp:2324-2328 RASC*CPREV -> PREV. **st4**: trans.cpp:1786-1790 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st5**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_POS) × TEXMTX0 — trans.cpp:2263-2286 sm*trans*lookAt(light)*parts: light-space depth into s
- TEXCOORD1 = TG_MTX3x4(TG_POS) × TEXMTX1 — trans.cpp:2299-2301 mng->texMat * parts (projective shadow map)
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX2 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**EFB-copy inputs.** GX_TEXMAP1 = mng->texObj: make_shadow_texture self path, GXCopyTex GX_CTF_A8 (shadow.cpp:1136-1144) read as GX_TF_I8 (shadow.cpp:1154)

**Notes.** SelfShadowSetup (trans.cpp:2245-2332) precedes materialSetup(part, info, 0, 0) (colIn CPREV, alphaIn APREV). Stage count = 5 + SelfShadowLevel. GXSetBlendMode is set by commonModelTrans (trans.cpp:1228) from info->blend_mode: 0 BLEND SRCALPHA/INVSRCALPHA, 1 BLEND SRCALPHA/ONE, 2 BLEND ONE/ONE, 3 BLEND DSTCLR/ONE, 4 NONE. Z mode by ModelRender (trans.cpp:985-991) from m->z_mode: 0 LEQUAL+write, 1 LEQUAL no write, 2 ALWAYS no write. Cull by m->CullMode (trans.cpp:1158-1171): 0 FRONT, 1 BACK, 2 NONE (DBG_BACK_CLIP forces FRONT). Alpha compare ALWAYS (trans.cpp:984) unless alphaSetup overrides it. ModelRender also enables alpha update and GXSetDstAlpha(TRUE, 0) (trans.cpp:1003-1004): the model pass writes EFB alpha 0.

**NV2A.** fits 8 stages: yes · textures: 3 · texture shader: no · passes: 1. 6 stages, 3 textures. The depth 'ramp texture' trick is an 8-bit shadow compare; NV2A could use its native shadow-map texture format (DEPTH_Y16) instead, but the 8-bit path maps 1:1 to combiners.

#### `trans_shadersetup_full` — shaderSetup (castshadow kind1 + blend0 + specular/bump + GI + alpha) (trans.cpp:1323)

**Purpose.** Composition of every optional block in shaderSetup order: the realistic upper bound for a character part.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** all optional blocks on at once (synthetic composition of the per-block cases; each block is traced in its own case)

**Stages:** 10 · **texgens:** 6 · **channels:** 1 · **textures bound:** 7

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR_NULL | - | ZERO, TEXC, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | RASC, ZERO, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 3 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVREG0 | - | - | - |
| 4 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | CPREV, C0, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | APREV, A0, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K1 / KASEL_K1_A | - | - |
| 5 | TEXCOORD3 | TEXMAP3 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG1 | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | bump_xyz INDTEXSTAGE0 ITM_0 signed |
| 6 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, KONST, C1, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K2 / - | - | - |
| 7 | TEXCOORD5 | TEXMAP5 | COLOR0A0 | - | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_2/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 8 | TEXCOORD1 | TEXMAP6 | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, APREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 9 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: ShadowCastSetup kind1 trans.cpp:2137-2141. **st1**: trans.cpp:2145-2149. **st2**: trans.cpp:1462-1466 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st3**: TextureBlend trans.cpp:1493-1497 (colIn CPREV). **st4**: trans.cpp:1510-1514, K1 = blendRatio (tev_kcolor 1). **st5**: specularSetup trans.cpp:1856-1867 -> REG1 (tev_reg 1). **st6**: trans.cpp:1871-1879 CPREV + K2*C1. **st7**: GlobalIlluminationSetup trans.cpp:2018-2027 (coord 5: bump took coord 4). **st8**: alphaSetup trans.cpp:2398-2402 on the material coord. **st9**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2101-2105 projective: mng->texMat (light view-projection, make_fix_light) * parts matrix
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX1 loaded with (u,v) translation (trans.cpp:1758-1765)
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — blend
- TEXCOORD3 = TG_MTX3x4(TG_NRM) × TEXMTX3 — trans.cpp:1854-1855 normal -> (0.5,0.5)-centred env-map lookup: trans(0.5,0.5,1) * scale(decay, -decay, 0) * specular_mat (view-space normal matrix, updateMatrices trans.cpp:2641)
- TEXCOORD4 = TG_MTX2x4(TG_TEX0) × IDENTITY — bump
- TEXCOORD5 = TG_MTX3x4(TG_NRM) × TEXMTX5 — GI

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD4, map TEXMAP4, scale ITS_1/ITS_1, matrix GX_ITM_0 (updateMatrices) — bumpSetup trans.cpp:2361-2366

**Konst / registers / swap tables.** KCOLOR0 = cast light colour; KCOLOR1 = blendRatio; KCOLOR2 = specular colour

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc after texture (FALSE) · alpha compare GREATER ?alpha_omit AOP_OR GREATER 255 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**Notes.** Not a single observed configuration but the sum of blocks shaderSetup can chain (trans.cpp:1361-1424). 10 stages, 7 texmaps (6 distinct textures + the alpha texture), 3 konst colours, 1 indirect stage, 6 texgens. The counters error out past 8 texcoords/maps, 4 kcolors, 3 regs, 16 stages (trans.cpp:137-290). Self-shadow (+6) in place of cast shadow (+2) would give 14 stages.

**NV2A.** fits 8 stages: **no** · textures: 7 · texture shader: yes · passes: 2. Exceeds NV2A: 10 combiners (8 max) and 7 textures (4 max). Split: pass 1 = shadow+material+blend (4 textures incl. the alpha texture, 6 stages), pass 2 additive = specular(+bump)+GI (3-4 textures) with the x4 scale applied in both; or drop the blend/GI stages per the catalogue's priority.

#### `trans_thermoshadersetup` — ThermoShaderSetup (trans.cpp:1288)

**Purpose.** Thermal scope: part drawn as its lit vertex colour only (no material texture); optional alpha texture.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** StaFlagChk(STA_THERMO_GRAPH) (trans.cpp:1328); part->flag & 4 appends alphaSetup(thermo=1): stage 1 on GX_TEXCOORD1 (identity TEX0 texgen, trans.cpp:2392-2394) / GX_TEXMAP1 = alpha_tex, colour CPREV, alpha TEXA*APREV, alpha compare GREATER alpha_omit

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:1303-1307 passes RASC/RASA; the order names TEXCOORD0/TEXMAP0 but no texture or texgen is set in this path (stale state).

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare GREATER 0 AOP_OR GREATER 255 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog GX_FOG_NONE (LightMgr.setFog black fog when STA_THERMO_GRAPH, light.cpp:854-857)

**Notes.** ThermoShaderSetup (trans.cpp:1288-1317): alpha compare GREATER 0, blend SRCALPHA/INVSRCALPHA set here (1299-1300), num texgens = tex_coord (1, though GX_TEXCOORD0 has no generator call here). No final scale stage. The thermal palette is applied later by filter07 (part B). Channels from LightSetModel (trans_lit.cpp:59-145) via ModelRender (trans.cpp:1000); see trans_lit_* cases for the variants.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Trivial: diffuse pass-through (1-2 stages).

#### `trans_refractshadersetup_type2_ratio` — RefractShaderSetup (Shader_type 2, Refract_ratio != 0) (trans.cpp:2668)

**Purpose.** Refractive render-to-texture surface (TexRenderModSet: type 2, pow 0x10, ratio 0x90): warped screen copy lerped with the material.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** Shader_type == 2 && Refract_ratio not in {0, 0xFF}; no cast shadow, no GI/specular/alpha (each appends as in the shaderSetup cases: cast shadow goes before the material, GI after it, specular/bump/alpha after the lerp, trans.cpp:2800-2827)

**Stages:** 4 · **texgens:** 3 · **channels:** 1 · **textures bound:** 3

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVREG0 | KCSEL_K0 / KASEL_K0_A | - | warp INDTEXSTAGE0 ITM_1 signed |
| 1 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | C0, CPREV, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K1 / KASEL_K1_A | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2725-2731 screen TEXC*K0 -> REG0, alpha RASA -> REG0. **st1**: trans.cpp:1786-1790 via trans.cpp:2804 materialSetup(part, info, RASC, RASA) materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st2**: trans.cpp:2817-2821 lerp(C0, CPREV, K1) with K1 = Refract_ratio; alpha APREV. **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

Indirect on stage 0: GXSetTevIndWarp(st, IND0, TRUE, FALSE, GX_ITM_1) trans.cpp:2789

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2692-2699 C_MTXLightPerspective(Fovy, 4/3, 0.5, -2/3, 0.5, 0.5) * modelview: screen-space projection of the vertex
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:2784 indirect map coords = model UVs
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX2 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD1, map TEXMAP1, scale ITS_1/ITS_1, matrix GX_ITM_1 = [[0, s, -s], [s, 0, 1.35s]], s = Refract_pow/512, exp 1 (trans.cpp:2770-2776) — trans.cpp:2779-2789 map 1 = the part's material texture (or its blend-table texture when flagsDC&4) used as the offset map

**Konst / registers / swap tables.** KCOLOR0 = kv grey: 0xFF/0x80/0x40 for gxCsScale 0/1/2 (default 0x40) (trans.cpp:2701-2715); KCOLOR1 = {Refract_ratio x4} (0x90 from TexRenderModSet)

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**EFB-copy inputs.** GX_TEXMAP0 = g_Get_tex_obj: GetEfbTex (trans.cpp:2877-2894) GXCopyTex RGBA8 of the frame below line 56 at half size, taken once per model before its parts (commonModelTrans trans.cpp:1092-1095)

**Notes.** RefractShaderSetup (trans.cpp:2668-2875) replaces shaderSetup when Shader_type is 1 or 2, Refract_ratio != 0xFF and !(info->be_flag & 4) (trans.cpp:1333). The screen copy is sampled through an indirect warp (GXSetTevIndWarp signed, no replace, GX_ITM_1 scale exp 1) whose matrix scales with Refract_pow/512. Ends with the same x4 final stage (trans.cpp:2829-2872). blend/zmode are null here because they are the runtime values of commonModelTrans/ModelRender: info->blend_mode (TexRenderModSet sets 1 = SRCALPHA/ONE when blendAddFlag, TexRender.cpp:269) and m->z_mode (TexRenderModSet sets 2 = ALWAYS without write, TexRender.cpp:280).

**NV2A.** fits 8 stages: yes · textures: 3 · texture shader: yes · passes: 1. 4 combiners; the screen copy sampled with a texture-shader offset (OFFSET_TEXTURE_2D) driven by the material texture in unit 1 — exactly NV2A's dependent-texture model. Blend/z strings are schema placeholders for the runtime-selected values.

#### `trans_refractshadersetup_type1_ratio0` — RefractShaderSetup (Shader_type 1, Refract_ratio 0) (trans.cpp:2734)

**Purpose.** Pure refraction (em2d-style, ratio 0): warped screen copy x K only, normals drive the offset through IndTex[0]; no material texture.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** Shader_type == 1 && Refract_ratio == 0 (em2d.cpp:5138 sets 0; r105.cpp:148 sets type 1); specularSetup+bumpSetup are appended when part->flag & 0x13 (trans.cpp:2795-2796), materialSetup is NOT called

**Stages:** 2 · **texgens:** 2 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | warp INDTEXSTAGE0 ITM_1 signed |
| 1 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2719-2723 screen TEXC*K0 -> PREV, alpha RASA. **st1**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

Indirect on stage 0: GXSetTevIndWarp trans.cpp:2756

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2692-2699 C_MTXLightPerspective(Fovy, 4/3, 0.5, -2/3, 0.5, 0.5) * modelview: screen-space projection of the vertex
- TEXCOORD1 = TG_MTX2x4(TG_NRM) × IDENTITY — trans.cpp:2751 view-space normal xy as the offset-map coordinate

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD1, map TEXMAP1, scale ITS_1/ITS_1, matrix GX_ITM_1 = [[0, s*mul_x, 0], [s*mul_y, 0, 0]], s = Refract_pow/512, exp 1 (trans.cpp:2741-2747) — trans.cpp:2750-2756 IndTex[0] on map 1

**Konst / registers / swap tables.** KCOLOR0 = kv grey: 0xFF/0x80/0x40 for gxCsScale 0/1/2 (default 0x40) (trans.cpp:2701-2715)

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**EFB-copy inputs.** GX_TEXMAP0 = g_Get_tex_obj: GetEfbTex (trans.cpp:2877-2894) GXCopyTex RGBA8 of the frame below line 56 at half size, taken once per model before its parts (commonModelTrans trans.cpp:1092-1095)

**Notes.** RefractShaderSetup (trans.cpp:2668-2875) replaces shaderSetup when Shader_type is 1 or 2, Refract_ratio != 0xFF and !(info->be_flag & 4) (trans.cpp:1333). The screen copy is sampled through an indirect warp (GXSetTevIndWarp signed, no replace, GX_ITM_1 scale exp 1) whose matrix scales with Refract_pow/512. Ends with the same x4 final stage (trans.cpp:2829-2872). blend/zmode are null here because they are the runtime values of commonModelTrans/ModelRender: info->blend_mode (TexRenderModSet sets 1 = SRCALPHA/ONE when blendAddFlag, TexRender.cpp:269) and m->z_mode (TexRenderModSet sets 2 = ALWAYS without write, TexRender.cpp:280).

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: yes · passes: 1. 2 combiners (+2 with specular). Offset map = IndTex[0] addressed by the normal: texture-shader OFFSET_TEXTURE_2D with the normal texgen done in the vertex program.

#### `trans_refractshadersetup_type1_ratio` — RefractShaderSetup (Shader_type 1, Refract_ratio != 0) (trans.cpp:2734)

**Purpose.** Refraction blended with the material (room data r100/r10c), offset map IndTex[0] addressed by the normal.

**Callers.** ModelRender (trans.cpp:1006) -> commonModelTrans (trans.cpp:1226); make_shadow_texture (shadow.cpp:1097) -> commonModelTrans(flag=1)

**Variant / condition.** Shader_type == 1 && Refract_ratio not in {0, 0xFF} (r100.cpp:1195, r10c.cpp:490 data)

**Stages:** 4 · **texgens:** 3 · **channels:** 1 · **textures bound:** 3

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVREG0 | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVREG0 | KCSEL_K0 / KASEL_K0_A | - | warp INDTEXSTAGE0 ITM_1 signed |
| 1 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |
| 2 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | C0, CPREV, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K1 / KASEL_K1_A | - | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, CPREV | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, ZERO, ZERO, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2725-2731. **st1**: trans.cpp:1786-1790 via trans.cpp:2804 materialSetup: TEXC*colIn, alpha = alphaIn (part->material_tex, or the animation frame's texture when flags&2). **st2**: trans.cpp:2817-2821. **st3**: trans.cpp:1384-1385,1419-1421 final colour-scale stage, x4 by default (TevScaleGroup 0/1).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — trans.cpp:2692-2699 C_MTXLightPerspective(Fovy, 4/3, 0.5, -2/3, 0.5, 0.5) * modelview: screen-space projection of the vertex
- TEXCOORD1 = TG_MTX2x4(TG_NRM) × IDENTITY — trans.cpp:2751
- TEXCOORD2 = TG_MTX2x4(TG_TEX0) × IDENTITY — trans.cpp:1767; when ModelTexInfo.flags&1 (uv scroll) GX_TEXMTX2 loaded with (u,v) translation (trans.cpp:1758-1765)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Indirect stages.**
- INDTEXSTAGE0: coord TEXCOORD1, map TEXMAP1, scale ITS_1/ITS_1, matrix GX_ITM_1 (trans.cpp:2741-2747) — IndTex[0] trans.cpp:2750

**Konst / registers / swap tables.** KCOLOR0 = kv grey: 0xFF/0x80/0x40 for gxCsScale 0/1/2 (default 0x40) (trans.cpp:2701-2715); KCOLOR1 = {Refract_ratio x4}

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull ?GX_CULL_FRONT|GX_CULL_BACK|GX_CULL_NONE by m->CullMode (trans.cpp:1158) · fog ?LightMgr.setFog(): GXSetFog(LightEnv.Fog.Type, Start, End, ZNEAR, ZFAR, Color) (light.cpp:859); GX_FOG_NONE when DPF_FOG or thermo

**EFB-copy inputs.** GX_TEXMAP0 = g_Get_tex_obj: GetEfbTex (trans.cpp:2877-2894) GXCopyTex RGBA8 of the frame below line 56 at half size, taken once per model before its parts (commonModelTrans trans.cpp:1092-1095)

**Notes.** RefractShaderSetup (trans.cpp:2668-2875) replaces shaderSetup when Shader_type is 1 or 2, Refract_ratio != 0xFF and !(info->be_flag & 4) (trans.cpp:1333). The screen copy is sampled through an indirect warp (GXSetTevIndWarp signed, no replace, GX_ITM_1 scale exp 1) whose matrix scales with Refract_pow/512. Ends with the same x4 final stage (trans.cpp:2829-2872). blend/zmode are null here because they are the runtime values of commonModelTrans/ModelRender: info->blend_mode (TexRenderModSet sets 1 = SRCALPHA/ONE when blendAddFlag, TexRender.cpp:269) and m->z_mode (TexRenderModSet sets 2 = ALWAYS without write, TexRender.cpp:280). Identical to type2_ratio except the offset map/texgen.

**NV2A.** fits 8 stages: yes · textures: 3 · texture shader: yes · passes: 1. As type2_ratio.

#### `trans_clearzbuf` — ClearZbuf (trans.cpp:2896)

**Purpose.** Full-screen quad at z = -0.99999 with colour writes off: clears the Z buffer between the sub-screen and effect passes.

**Callers.** Render (trans.cpp:944)

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR_NULL | - | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: trans.cpp:2918-2922 all-zero output (colour update is off anyway).

**Channels / lights.**
- COLOR0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_ONE LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** trans.cpp:2904-2938: GXSetColorUpdate(0)/AlphaUpdate(0), ortho 512x448, mat colour {8,8,0x80,0x1C} unused by the TEV. ss_main clearZbuffer is the same program.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Replace by a depth-only clear (NV097_CLEAR_SURFACE with Z only) in the shim.

#### `trans_modelrender_unlit` — ModelRender (trans.cpp:993)

**Purpose.** Models with be_flag 0x00020000: no lighting, channel off, ambient 0x40 grey, material = model colour (register colour only).

**Callers.** ModelRender (trans.cpp:993-998)

**Variant / condition.** m->be_flag & 0x00020000

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans.cpp:994-998. With the channel disabled GX outputs the material colour register.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Diffuse = constant material colour.

### src/game/trans_lit.cpp

Lighting only; no TEV calls. The channel configurations are recorded as channel-only cases (`num_tev_stages` = `?unchanged`) so the merged catalogue can pair them with the TEV cases above. All eight light types are distance/spot variants programmed through `GXInitLightAttn`/`GXInitLightSpot`/`GXInitLightDistAttn` (trans_lit.cpp:300-470); the light colour is `DispCol.rgb * DispCol.a / 128` with alpha 0x80 (trans_lit.cpp:497-527).

#### `trans_lit_lightsetmodel_default` — LightSetModel (trans_lit.cpp:59)

**Purpose.** Lighting channels of a normal model draw: up to 8 GX lights by type, clamped diffuse, spot attenuation; alpha channel unlit.

**Callers.** ModelRender (trans.cpp:1000)

**Variant / condition.** !(data->flags & 0x40000000), !(LightInfo.Flag & 4)

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?mask of the model's lights (bit i = list[i]), GX_LIGHT0 when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:122 LightSetModel; amb = max(env ambient, local ambients) (+AddAmb when be_flag 8), mat = info->color
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:123

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans_lit.cpp:85-109 builds one GXLightObj per non-null light (types 0-6 via funcLightParam, type 7 raises the ambient), be_flag 0x8000 disables all lights (n = 0); mask 0 -> the black light in slot 0 (trans_lit.cpp:110-113). Ambient: AmbientScr/Esp/Em by EnableMask bits 0x10/0x8 (trans_lit.cpp:124-132, +AddAmb 138-142) + AddAmb (be_flag 8). Material = info->color. Light colour = DispCol.rgb * DispCol.a / 128, alpha 0x80 (lightSetColor trans_lit.cpp:497-527); attenuation models per type at trans_lit.cpp:300-470.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Vertex-program lighting: 8 lights with GX attenuation (a0+a1*cos+a2*cos^2)/(k0+k1*d+k2*d^2), CLAMP diffuse, SPOT attenuation. NV2A fixed-function has 8 lights too but a different attenuation model; use a vertex program (02-GRAPHICS.md section 5).

#### `trans_lit_lightsetmodel_vtxamb` — LightSetModel (trans_lit.cpp:115)

**Purpose.** Model data flagged 0x40000000: vertex colour supplies the ambient (and the alpha material).

**Callers.** ModelRender (trans.cpp:1000)

**Variant / condition.** data->flags & 0x40000000

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: enabled, amb SRC_VTX, mat SRC_REG, lights `?model light mask`, DF_CLAMP, AF_SPOT — trans_lit.cpp:116 amb_src VTX
- ALPHA0: disabled, amb SRC_VTX, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:117 amb VTX, mat VTX

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans_lit.cpp:115-120; material colour = info->color, returns before the ambient computation.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Vertex colour as ambient term in the vertex program.

#### `trans_lit_lightdisable` — LightDisable (trans_lit.cpp:535)

**Purpose.** Unlit model (LightInfo.Flag & 4): black light, black material and ambient -> black diffuse.

**Callers.** LightSetModel (trans_lit.cpp:75)

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT0`, DF_CLAMP, AF_SPOT — trans_lit.cpp:539 lit by lightObjBlack (colour 0)
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:540

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans_lit.cpp:535-544 mat = amb = {0,0,0,0xFF}.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Constant black diffuse.

#### `trans_lit_commonclothlightset` — commonClothLightSet / commonWaterLightSet (trans_lit.cpp:149)

**Purpose.** Lighting for cloth and water surfaces: the chain's lights, scenery ambient, white material.

**Callers.** clothTrans (cloth.cpp:384); Espgen43.cpp:274; commonWaterLightSet: Espgen42.cpp:754, espgen45.cpp:480

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?lights of the chain (bit i), GX_LIGHT0 black when none`, DF_CLAMP, AF_SPOT — trans_lit.cpp:191 / 248
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:192, 199 / 249, 256

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans_lit.cpp:149-200 and 204-257 are identical apart from the water variant scaling each light's alpha by pow/256 (trans_lit.cpp:231-234). obj_flag = 0 (no distance fade).

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Same vertex-program lighting path.

#### `trans_lit_commonesplightset` — commonEspLightSet (trans_lit.cpp:260)

**Purpose.** Lighting for effects: lights with DF_NONE (no clamp of N.L) and the effect ambient.

**Callers.** esp_sub.cpp:983 (part B)

**Stages:** ?unchanged (channel-only record) · **texgens:** ?unchanged · **channels:** 1 · **textures bound:** 0

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?effect lights (bit i), GX_LIGHT0 black when none`, DF_NONE, AF_SPOT — trans_lit.cpp:293 GX_DF_NONE
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:294

**Blend / Z / alpha compare / cull / fog.** blend runtime / unchanged (see notes) · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** trans_lit.cpp:260-296; ambient = AmbientEsp.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. DF_NONE = add the light colour without the N.L term.

### src/game/shadow.cpp

Shadow maps. `make_shadow_texture` renders the casters from the light into the EFB with colour writes off and copies the alpha out as `GX_CTF_A8` (read back as I8); `MakeSoftShadow` blurs it by copy/draw ping-pong; `shadowShaderSetup2` projects up to four of these onto receivers per part; `TransLightTexture` multiplies a light map into the shadow alpha. `shadowShaderSetup` (shadow.cpp:1356) is dead-stripped in the DOL. The cast-shadow (`ShadowCastSetup`) and self-shadow (`SelfShadowSetup`) receive paths live in trans.cpp and are catalogued there.

#### `shadow_softshadowgxdraw` — SoftShadowGXDraw (shadow.cpp:859)

**Purpose.** One blur tap of the soft shadow: the A8 shadow copy drawn as a screen quad (texture colour/alpha only).

**Callers.** MakeSoftShadow (shadow.cpp:943-958) x2-5 per shadow texture

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: shadow.cpp:890-894 TEXC / TEXA.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — shadow.cpp:885

**Channels / lights.**
- COLOR0: enabled, amb SRC_VTX, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_SPOT — shadow.cpp:886 lit, no lights: colour = vertex colour (white, alpha a); amb register left uninitialised (local `amb`)
- ALPHA0: enabled, amb SRC_VTX, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_SPOT — shadow.cpp:887

**Blend / Z / alpha compare / cull / fog.** blend BM_NONE BL_ONE/BL_ZERO LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull GX_CULL_NONE · fog unchanged

**EFB-copy inputs.** GX_TEXMAP0 = mng->pTex as I8 (size/div): SoftShadowGetEFB GXCopyTex GX_CTF_A8 (shadow.cpp:850-852)

**Notes.** shadow.cpp:870-894. Ortho projection over Screen; alpha update is on from make_shadow_texture, colour update off; each tap re-copies the EFB (A8) at a different scale, i.e. a ping-pong box blur.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner. The blur is copy/draw ping-pong on the Xbox too (render-to-texture + quad).

#### `shadow_make_shadow_texture_noself` — make_shadow_texture (SHD_NO_SELF) (shadow.cpp:971)

**Purpose.** Shadow-map pass for cast shadows: the casters drawn from the light with colour writes off and a constant destination alpha.

**Callers.** shadowModelRender (shadow.cpp:657) from OT 8

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | PASSCLR | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: shadow.cpp:1004-1005.

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — shadow.cpp:997; amb 0, mat = light Col.r grey (998-1001)

**Blend / Z / alpha compare / cull / fog.** blend BM_NONE BL_ONE/BL_ZERO LO_CLEAR · z compare off, ALWAYS, no write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** shadow.cpp:982-1005. This TEV program is immediately replaced: commonModelTrans(p, info, lookAt, 1) (shadow.cpp:1097) runs shaderSetup per part (alpha compare forced ALWAYS by flag&1, trans.cpp:1229-1231). What matters is GXSetColorUpdate(0) + GXSetAlphaUpdate(1) + GXSetDstAlpha(TRUE, a) per model (shadow.cpp:1084): the silhouette is written as constant alpha a = Col.r*(256-Shd_color)/256 (x distance rate), then TransLightTexture may multiply a light map in, MakeSoftShadow blurs, and GXCopyTex GX_CTF_A8 (shadow.cpp:1136-1144) produces mng->texObj (I8). Viewport/scissor = g_Shd_render_size, projection C_MTXPerspective(mng->fov, 1, ZNEAR, ZFAR).

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. NV2A has no destination-alpha-replace; the shim must implement GXSetDstAlpha by forcing the final combiner alpha to the constant whenever it is enabled (colour mask off, alpha write on). The material TEV program still runs (only its alpha test matters).

#### `shadow_make_shadow_texture_self` — make_shadow_texture (self shadow) (shadow.cpp:1007)

**Purpose.** Self-shadow depth pass: casters drawn from the light with the IndTex ramp mapping light-space depth to alpha.

**Callers.** shadowModelRender (shadow.cpp:657) when isSelfUse && mng->self & 1

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: shadow.cpp:1047-1051 TEXC / TEXA of IndTex[shd_tex_no=0].

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_POS) × TEXMTX0 — shadow.cpp:1018-1038 sm*trans*lookAt(light, target)*parts: z*0.0003 (+shd_ofs) into s

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — shadow.cpp:1040-1044 amb 0, mat 0x80 grey

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_ONE/BL_ZERO LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull GX_CULL_FRONT · fog unchanged

**Notes.** shadow.cpp:1007-1054 then shadowModelTrans2 (shadow.cpp:1099, 1586-1647) which only sets matrices/arrays/cull FRONT, so this TEV stands. Colour update off, alpha on: alpha = ramp(depth) -> GXCopyTex GX_CTF_A8 -> mng->texObj, compared in SelfShadowSetup (trans_shadersetup_selfshadow).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner, 1 texture; the ramp texgen is a POS-through-2x4 texgen in the vertex program.

#### `shadow_shadowshadersetup2_1` — shadowShaderSetup2 (num = 1) (shadow.cpp:1377)

**Purpose.** Receiver pass: one projected shadow texture (I8) x spot light 0 (red channel) as alpha, colour black, alpha-blended.

**Callers.** shadowModelTrans (shadow.cpp:1571) per part <- ProcShadowScrModel (shadow.cpp:1273) <- shadowScrModelRender (OT 0xE)

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | TEXC, ZERO, ONE, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP0/SWAP0 | - |

Stage sources: **st0**: shadow.cpp:1414-1421 colour = (1-ONE)*TEXC + ONE*ZERO = 0 (black); alpha = TEXA*RASA; swap table 0 = (R,R,R,R) so RASA = light 0's channel (red).

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — shadow.cpp:1408-1411 mng->texMat * parts (projective)

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT0`, DF_CLAMP, AF_SPOT — shadow.cpp:1491 spot light i: colour col_tbl[i] (one channel), cutoff fov*0.9 GX_SP_FLAT, no distance attn (1391-1403); amb 0, mat 0xFF
- ALPHA0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT0`, DF_CLAMP, AF_SPOT — shadow.cpp:1492 alpha channel lit by the same lights

**Konst / registers / swap tables.** swap table SWAP0 = (C,C,C,C)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, no write · z-comp-loc unchanged · alpha compare GREATER 0 AOP_OR GREATER 255 · cull GX_CULL_FRONT · fog unchanged

**EFB-copy inputs.** GX_TEXMAP0 = mng->texObj (make_shadow_texture GXCopyTex GX_CTF_A8 read as I8)

**Notes.** shadow.cpp:1377-1497. Blend/alpha-compare from ProcShadowScrModel (shadow.cpp:1265-1266), cull FRONT from shadowModelTrans (1566), z from 1405. shadowScrModelRender restores swap tables 0-3 to identity afterwards (shadow.cpp:1329-1335), which differs from the SDK default tables (see sofdec_restoretevprm). The DBG_SHADOW_LIGHT block (1455-1489) is debug only.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Per-stage ras swizzle (R->A) has no direct NV2A equivalent; for one light use the diffuse red channel through a dot-product combiner (RGB dot (1,0,0)) into the alpha pipe, or light the spot into alpha directly in the vertex program.

#### `shadow_shadowshadersetup2_4_eqcol` — shadowShaderSetup2 (num = 4, STA_SHADOW_EQCOL) (shadow.cpp:1377)

**Purpose.** Receiver pass with four shadow textures, each masked by its spot light through the swap tables, alpha summed, then equalised to the light colour.

**Callers.** shadowModelTrans <- ProcShadowScrModel (up to 4 managers per call, shadow.cpp:1267-1274)

**Stages:** 5 · **texgens:** 4 · **channels:** 1 · **textures bound:** 4

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | TEXC, ZERO, ONE, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP0/SWAP0 | - |
| 1 | TEXCOORD1 | TEXMAP1 | COLOR0A0 | - | TEXC, ZERO, ONE, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP1/SWAP1 | - |
| 2 | TEXCOORD2 | TEXMAP2 | COLOR0A0 | - | TEXC, ZERO, ONE, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP2/SWAP2 | - |
| 3 | TEXCOORD3 | TEXMAP3 | COLOR0A0 | - | TEXC, ZERO, ONE, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, APREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP3/SWAP3 | - |
| 4 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | CPREV, ZERO, KONST, ZERO | COMP_RGB8_GT/ZERO/SCALE_1/clamp → TEVPREV | APREV, ZERO, KONST, ZERO | COMP_RGB8_GT/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |

Stage sources: **st0**: shadow.cpp:1418-1421. **st1**: shadow.cpp:1423-1426 CPREV (0) / APREV + TEXA*RASA with swap 1 = green. **st2**: swap 2 = blue. **st3**: swap 3 = alpha. **st4**: shadow.cpp:1443-1451 GX_TEV_COMP_RGB8_GT / COMP_A8_GT: out = ZERO + (prev > 0 ? K0 : 0): any shadow -> K0 (light Col.r grey) so overlapping shadows are not darker.

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — shadow.cpp:1410-1411 GX_TEXMTX0
- TEXCOORD1 = TG_MTX3x4(TG_POS) × TEXMTX1 — shadow.cpp:1410-1411 GX_TEXMTX1
- TEXCOORD2 = TG_MTX3x4(TG_POS) × TEXMTX2 — shadow.cpp:1410-1411 GX_TEXMTX2
- TEXCOORD3 = TG_MTX3x4(TG_POS) × TEXMTX3 — shadow.cpp:1410-1411 GX_TEXMTX3

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT0|GX_LIGHT1|GX_LIGHT2|GX_LIGHT3`, DF_CLAMP, AF_SPOT — shadow.cpp:1491; light i colour = col_tbl[i] (R,G,B,A unit vectors)
- ALPHA0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT0|GX_LIGHT1|GX_LIGHT2|GX_LIGHT3`, DF_CLAMP, AF_SPOT — shadow.cpp:1492

**Konst / registers / swap tables.** KCOLOR0 = {Col.r, Col.r, Col.r, Col.r} of tbl[0]->pLight (shadow.cpp:1436-1443); swap table SWAP0 = (C,C,C,C); swap table SWAP1 = (C,C,C,C); swap table SWAP2 = (C,C,C,C); swap table SWAP3 = (C,C,C,C)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, no write · z-comp-loc unchanged · alpha compare GREATER 0 AOP_OR GREATER 255 · cull GX_CULL_FRONT · fog unchanged

**EFB-copy inputs.** GX_TEXMAP0..3 = shadow textures (A8 copies read as I8)

**Notes.** shadow.cpp:1377-1497 with num = 4 and STA_SHADOW_EQCOL (set by make_shadow_texture when w->bMultiShadow, shadow.cpp:1146-1148). Max 5 stages, 4 textures, 4 texgens, 4 lights.

**NV2A.** fits 8 stages: yes · textures: 4 · texture shader: no · passes: 1. Fits 8 combiners and 4 textures only if the channel select is done as RGB dot products: per light i, alpha_i = dot(diffuse.rgb, e_i) * tex_i.a -> 4 stages, sum in the alpha pipe, and the COMP_GT stage as a mux (NV2A combiner mux on spare0.a) -> 6 stages. Alternatively one pass per light (4 passes, 1 texture each).

#### `shadow_translighttexture` — TransLightTexture (shadow.cpp:1649)

**Purpose.** Multiplies a light-map texture into the shadow map's alpha (blend DSTALPHA x src): alpha = TEXA (or 1-TEXA), x4 through a TLUT.

**Callers.** make_shadow_texture (shadow.cpp:1121 flag1=1 room light texture + TLUT; 1128 flag1=0 room texture 0)

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | PASSCLR | ZERO, ONE, TEXC, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, KONST, TEXA, ZERO | ADD/ZERO/SCALE_4/clamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | - | - |

Stage sources: **st0**: shadow.cpp:1676 GXSetTevOp PASSCLR then overridden 1687-1690: colour TEXC, alpha K0*TEXA (K0 = white) scale x4 when flag1 (tlut); flag2 (w->Flag & 4, 1682-1685): alpha (KONST, ZERO, TEXA, ZERO) = 1-TEXA.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — shadow.cpp:1668-1669 identity matrix

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — shadow.cpp:1655; amb/mat white written to GX_COLOR0 (1657-1658)

**Konst / registers / swap tables.** KCOLOR0 = {0xFF,0xFF,0xFF,0xFF}

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_DSTALPHA/BL_ZERO LO_CLEAR · z compare off, ALWAYS, no write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** shadow.cpp:1649-1715. Ortho quad of g_Shd_render_size; colour update still off from make_shadow_texture so only alpha is written: dstA = srcA * dstA.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner; blend factor DST_ALPHA/ZERO is native. CI textures need the TLUT expanded to a palettised NV2A texture (the shim's CI path).

#### `shadow_drawtexture2` — drawTexture2 (debug_mode 0xE / DBG_DRAW_SH_TEX) (shadow.cpp:1717)

**Purpose.** Debug display of a shadow texture: REPLACE with the texture's alpha swizzled into every channel.

**Callers.** shadowScrModelRender (shadow.cpp:1345) only when pG->debug_mode == 0xE or DBG_DRAW_SH_TEX

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | REPLACE | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | SWAP0/SWAP1 | - |

Stage sources: **st0**: shadow.cpp:1736; swap 1737-1739: ras table 0, tex table 1 = (A,A,A,A).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — shadow.cpp:1731-1732

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — shadow.cpp:1723 white

**Konst / registers / swap tables.** swap table SWAP0 = (C,C,C,C); swap table SWAP1 = (C,C,C,C)

**Blend / Z / alpha compare / cull / fog.** blend BM_NONE BL_ONE/BL_ZERO LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull GX_CULL_NONE · fog unchanged

**Notes.** shadow.cpp:1717-1765; the stage's swap is (GX_TEV_SWAP0, GX_TEV_SWAP1) and tables 0/1 are restored to identity at 1762-1764. Debug-only at run time (compiled in).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Texture alpha broadcast = combiner input mapping (tex0.a into RGB) — trivial.

### src/game/mirror.cpp

#### `mirror_mirrormodeltrans2` — mirrorModelTrans2 (mirror.cpp:28)

**Purpose.** Mirror surface: TexRender texture 0 (the reflected scene) projected through the camera onto the model, x2, alpha from the material register.

**Callers.** MirrorDraw2 (mirror.cpp:22) <- TexRenderModAddOtMirror (TexRender.cpp:329, OT 0x10) <- st1/r120.cpp:569

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, ONE, ZERO | ADD/ZERO/SCALE_2/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: mirror.cpp:92-96 TEXC x2, alpha RASA.

**Texgens.**
- TEXCOORD0 = TG_MTX3x4(TG_POS) × TEXMTX0 — mirror.cpp:88-91 C_MTXLightPerspective(Fovy, 4/3, 0.5, -0.5, 0.5, 0.5) * modelview

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — mirror.cpp:45-48 white amb/mat on GX_COLOR0; per part GXSetChanMatColor(COLOR0A0, info->color * invisible_factor) when < 1 (105-109)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare off, ALWAYS, write · z-comp-loc unchanged · alpha compare GREATER 0 AOP_OR GREATER 255 · cull GX_CULL_FRONT · fog unchanged

**EFB-copy inputs.** GX_TEXMAP0 = GetTexRenderMgrAddr(0)->GetTexObj(): CopyTexRenderMgr GXCopyTex RGBA8 (TexRender.cpp:131-133)

**Notes.** mirror.cpp:38-101 uses pG->gxStage counters from 0. Z compare is off (GXSetZMode(0, ALWAYS, 1)).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner, projective texgen; the render texture is an NV2A render target.

### src/game/foot_shadow.cpp

#### `foot_shadow_drawshadowparts` — drawShadowParts (foot_shadow.cpp:225)

**Purpose.** Foot-shadow blob: textured quad on the floor, colour black, alpha = texture alpha x channel alpha (grey a).

**Callers.** DrawFootShadow (foot_shadow.cpp:176,187,208) <- commonModelTrans (trans.cpp:1264)

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: foot_shadow.cpp:248-252 colour 0, alpha TEXA*RASA.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — foot_shadow.cpp:244-245 identity (GXSetTexCoordGen, no post matrix)

**Channels / lights.**
- COLOR0A0: enabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — foot_shadow.cpp:233 lit with no lights: colour = mat*amb, both {a,a,a,a} (239-240)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, no write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull GX_CULL_NONE · fog unchanged

**Notes.** foot_shadow.cpp:230-275; texture = EspGetTexObj(0x13) (effect texture 0x13). Camera view matrix, perspective projection.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner.

### src/game/objWep.cpp

#### `objwep_draw_line3d_local_222` — Draw_line3d_local_222 (objWep.cpp:472)

**Purpose.** Laser-sight line: vertex-colour pass-through, opaque or additive.

**Callers.** Draw_line3d_222 (objWep.cpp:277,433; em39.cpp:6507); esp19.cpp:122

**Variant / condition.** blend == 0 -> BLEND ONE/ZERO (objWep.cpp:482); blend != 0 -> BLEND ONE/ONE (484). (col >> 24) == 0xFE -> z compare off (489)

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | PASSCLR | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: objWep.cpp:497-498.

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — objWep.cpp:494 material from vertex colour

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_ONE/BL_ZERO LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** objWep.cpp:481-498. GX_LINES with per-vertex colour fading along the length.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Diffuse pass-through; blend ONE/ONE additive.

### src/game/main_sub.cpp

#### `main_sub_drawtexture` — DrawTexture (main_sub.cpp:496)

**Purpose.** 2D texture blit (loading / sub-screen TPL images): REPLACE, alpha test > 1, no blend.

**Callers.** DrawTpl (main_sub.cpp:492) <- game.cpp:991, 1761; ss_Draw_tpl_local (ss_item_draw.cpp:126)

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | REPLACE | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: main_sub.cpp:515-516.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — main_sub.cpp:511-512 identity

**Channels / lights.**
- COLOR0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — main_sub.cpp:503 white amb/mat on GX_COLOR0 (505-506)

**Blend / Z / alpha compare / cull / fog.** blend BM_NONE BL_ONE/BL_ZERO LO_CLEAR · z compare on, ALWAYS, no write · z-comp-loc unchanged · alpha compare GREATER 1 AOP_OR GREATER 1 · cull GX_CULL_NONE · fog unchanged

**Notes.** main_sub.cpp:502-543, ortho 448 x fbWidth; the texture may be CI8/CI4 with a TLUT (main_sub.cpp:482-488, ss_item_draw.cpp:113-122).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Trivial.

### src/game/libgpu.cpp

#### `libgpu_gpusetup` — gpuSetup (make_g3/g4/f3/f4/tile/lg*/lf*) (libgpu.cpp:97)

**Purpose.** PlayStation-style 2D primitive list (DrawOTag): colour-only vertices, pass-through, z off.

**Callers.** DrawOTag (libgpu.cpp:59-93) <- main.cpp:189 (MainOt[4])

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | PASSCLR | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: libgpu.cpp:106-107.

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — libgpu.cpp:100 vertex colour

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_ONE/BL_ZERO LO_CLEAR · z compare off, LEQUAL, no write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** libgpu.cpp:97-114, ortho 512x448. Compiled into the port (xbox/units.mk) though the comment calls it debug line drawing.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Trivial.

### src/game/cloth.cpp

#### `cloth_clothtrans_1tex` — clothTrans (cloth.cpp:346)

**Purpose.** Cloth sheet: one lit texture, colour and alpha x4, alpha test > 4.

**Callers.** ClothDraw (cloth.cpp:339) OT 0xD

**Variant / condition.** pCL->pTobjA == 0; blendMode 0 -> BLEND SRCALPHA/INVSRCALPHA + z write (cloth.cpp:394), else BLEND ONE/ONE without z write (396-397)

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_4/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: cloth.cpp:364-368 TEXC*RASC x4, TEXA*RASA x4.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — cloth.cpp:391-392 identity

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?cloth lights (commonClothLightSet)`, DF_CLAMP, AF_SPOT — trans_lit.cpp:191; mat = pCL->color (cloth.cpp:385), amb scenery
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:192

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc unchanged · alpha compare GREATER 4 AOP_OR GREATER 255 · cull GX_CULL_NONE · fog unchanged

**Notes.** cloth.cpp:361-409. Texture may be CI with TLUT (cloth.cpp:183-189). Lighting via a temporary cModel and commonClothLightSet (cloth.cpp:370-384).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. 1 combiner with x4 scale.

#### `cloth_clothtrans_2tex` — clothTrans (pTobjA) (cloth.cpp:400)

**Purpose.** Cloth sheet with a second texture modulated in (same UVs), z-compare after texturing.

**Callers.** ClothDraw (cloth.cpp:339)

**Variant / condition.** pCL->pTobjA != 0 (cloth.cpp:401-408): ZCompLoc FALSE, blend forced SRCALPHA/INVSRCALPHA

**Stages:** 2 · **texgens:** 1 · **channels:** 1 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, TEXC, RASC, ZERO | ADD/ZERO/SCALE_4/clamp → TEVPREV | ZERO, TEXA, RASA, ZERO | ADD/ZERO/SCALE_4/clamp → TEVPREV | - | - | - |
| 1 | TEXCOORD0 | TEXMAP1 | COLOR0A0 | MODULATE | ZERO, TEXC, CPREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, APREV, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: cloth.cpp:364-368. **st1**: cloth.cpp:406-407 GXSetTevOp(1, GX_MODULATE): stage-1 table (CPREV).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × TEXMTX0 — cloth.cpp:392

**Channels / lights.**
- COLOR0: enabled, amb SRC_REG, mat SRC_REG, lights `?cloth lights (commonClothLightSet)`, DF_CLAMP, AF_SPOT — trans_lit.cpp:191; mat = pCL->color (cloth.cpp:385), amb scenery
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — trans_lit.cpp:192

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, write · z-comp-loc after texture (FALSE) · alpha compare GREATER 4 AOP_OR GREATER 255 · cull GX_CULL_NONE · fog unchanged

**Notes.** cloth.cpp:400-409.

**NV2A.** fits 8 stages: yes · textures: 2 · texture shader: no · passes: 1. 2 combiners, 2 textures sharing one coordinate set.

### src/game/examine.cpp

#### `examine_drawbuffer` — drawBuffer (examine.cpp:326)

**Purpose.** Item-examine background: the stored half-size frame copy drawn back full screen, alpha from the vertex colour.

**Callers.** render (examine.cpp:390) OT callback <- ItemExamine::move

**Stages:** 1 · **texgens:** 1 · **channels:** 1 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR0A0 | - | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: examine.cpp:353-356 TEXC, alpha RASA (operation_flag static 1; else branch GXSetTevOp REPLACE at 358 is dead).

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — examine.cpp:347 (GXSetTexCoordGen)

**Channels / lights.**
- COLOR0: enabled, amb SRC_VTX, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_SPOT — examine.cpp:348 lit, no lights, vertex colour (white, alpha)
- ALPHA0: enabled, amb SRC_VTX, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_SPOT — examine.cpp:349

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare ALWAYS 0 AOP_OR ALWAYS 0 · cull GX_CULL_NONE · fog GX_FOG_NONE (examine.cpp:384 render())

**EFB-copy inputs.** GX_TEXMAP0 = local_buff: getEFB GXCopyTex RGBA8 SCR_W/2 x SCR_H/2 with a 2-tap vfilter (examine.cpp:286-289)

**Notes.** examine.cpp:334-364: GXSetNumChans(0) then ChanCtrl then GXSetNumChans(1) (345-350), so the effective channel count is 1. Ortho SCR_W x SCR_H at z 65530; z mode ALWAYS+write during the quad, restored to LEQUAL after (360-362).

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Trivial; the EFB copy is a render-target resolve.

### src/game/sofdec.cpp

#### `sofdec_settevprm` — setTevPrm (sofdec.cpp:68)

**Purpose.** Sofdec movie YUV -> RGB conversion in the TEV: Y plane (I8) and UV plane (IA8) combined through konst matrices and swap tables.

**Callers.** cSofdec::drawTex mode 0 (sofdec.cpp:176)

**Stages:** 4 · **texgens:** 2 · **channels:** 0 · **textures bound:** 2

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP1 | COLOR_NULL | - | ZERO, TEXC, KONST, C0 | ADD/ZERO/SCALE_1/noclamp → TEVPREV | ZERO, TEXA, KONST, A0 | SUB/ZERO/SCALE_1/noclamp → TEVPREV | KCSEL_K0 / KASEL_K0_A | SWAP0/SWAP1 | - |
| 1 | TEXCOORD1 | TEXMAP0 | COLOR_NULL | - | ZERO, TEXC, KONST, CPREV | ADD/ZERO/SCALE_2/noclamp → TEVPREV | ZERO, TEXA, KONST, APREV | ADD/ZERO/SCALE_2/noclamp → TEVPREV | KCSEL_K1 / KASEL_K1_A | SWAP0/SWAP0 | - |
| 2 | TEXCOORD0 | TEXMAP1 | COLOR_NULL | - | ZERO, TEXC, KONST, CPREV | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, TEXA, KONST, APREV | SUB/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K2 / KASEL_K2_A | SWAP0/SWAP2 | - |
| 3 | TEXCOORD_NULL | TEXMAP_NULL | COLOR_NULL | - | CPREV, APREV, KONST, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | KCSEL_K3 / - | - | - |

Stage sources: **st0**: sofdec.cpp:80-87 UV map (mapUV=1), swap table 1 = (R,A,A,A): colour = C0 + TEX*K0 unclamped, alpha = A0 - TEXA*K0.a unclamped. **st1**: sofdec.cpp:89-96 Y map (mapY=0): (CPREV + TEX*K1) x2 unclamped; alpha (APREV + TEXA*K1.a) x2. **st2**: sofdec.cpp:98-105 UV map again, swap table 2 = (R,R,A,R): CPREV + TEX*K2 clamped; alpha APREV - TEXA*K2.a clamped. **st3**: sofdec.cpp:107-113 lerp(CPREV, APREV, K3) with K3 = (0, 0xFF, 0, 0): the green channel is taken from the alpha pipe; alpha 0.

**Texgens.**
- TEXCOORD0 = TG_MTX2x4(TG_TEX0) × IDENTITY — sofdec.cpp:76
- TEXCOORD1 = TG_MTX2x4(TG_TEX0) × IDENTITY — sofdec.cpp:77

**Konst / registers / swap tables.** KCOLOR0 = {0x66, 0x00, 0xFF, 0x32}; KCOLOR1 = {0x94, 0x00, 0x94, 0x94}; KCOLOR2 = {0xCB, 0x00, 0x05, 0xCF}; KCOLOR3 = {0x00, 0xFF, 0x00, 0x00}; TEVREG0 = GXSetTevColorS10 {-111, 0, -138, 68} (sofdec.cpp:116-117); swap table SWAP0 = (C,C,C,C); swap table SWAP1 = (C,C,C,C); swap table SWAP2 = (C,C,C,C)

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_ONE/BL_ZERO LO_CLEAR · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** sofdec.cpp:68-132: GXSetNumChans(0), GXSetNumIndStages(0); blend BLEND ONE/ZERO from drawTex (177). Textures: ytobj GX_TF_I8 (map 0) and ctobj GX_TF_IA8 (map 1) written by the decoder (sofdec.cpp:310-311), not EFB copies. Uses signed/unclamped intermediates, GXSetTevColorS10 and three swap tables: the only such program in part A.

**NV2A.** fits 8 stages: **no** · textures: 2 · texture shader: no · passes: 1. The arithmetic fits 4 combiners (signed 9-bit intermediates, x2 scale) but the per-stage texture swizzles (R,A,A,A)/(R,R,A,R) and the alpha->green move cannot be expressed with NV2A input mappings (only alpha/blue selection). Do not emulate: decode the movie into an NV2A YUY2 (CR8YB8CB8YA8) texture or convert on the CPU, and draw with REPLACE.

#### `sofdec_restoretevprm` — restoreTevPrm (sofdec.cpp:135)

**Purpose.** State restored after a movie frame: one REPLACE stage, no channels, SDK-default swap tables, blend NONE with logic op SET.

**Callers.** cSofdec::drawTex (sofdec.cpp:201)

**Stages:** 1 · **texgens:** 1 · **channels:** 0 · **textures bound:** 1

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD0 | TEXMAP0 | COLOR_NULL | REPLACE | ZERO, ZERO, ZERO, TEXC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, TEXA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: sofdec.cpp:142-143; swap (SWAP0, SWAP0) 144.

**Konst / registers / swap tables.** swap table SWAP0 = (C,C,C,C); swap table SWAP1 = (C,C,C,C); swap table SWAP2 = (C,C,C,C); swap table SWAP3 = (C,C,C,C)

**Blend / Z / alpha compare / cull / fog.** blend BM_NONE BL_ONE/BL_ZERO LO_SET · z compare on, ALWAYS, no write · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** sofdec.cpp:135-152. GXSetNumTexGens(1) without a GXSetTexCoordGen call (stale generator). These swap tables are the GXInit defaults; shadow.cpp instead resets tables 1-3 to identity (shadow.cpp:1332-1335, 1763-1764) — the shim should track tables explicitly rather than assume either.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Trivial.

#### `sofdec_drawtex_mode1` — cSofdec::drawTex (m_draw_mode 1) (sofdec.cpp:187)

**Purpose.** Debug/test movie draw: RGBA8 frame texture on a torus with whatever TEV stage 0 is current.

**Callers.** cSofdec::drawTex (sofdec.cpp:187-200)

**Stages:** 1 · **texgens:** 1 · **channels:** ?unchanged · **textures bound:** 0

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z runtime / unchanged (see notes) · z-comp-loc unchanged · alpha compare unchanged · cull unchanged · fog unchanged

**Notes.** sofdec.cpp:188-190 only sets GXSetNumTevStages(1), loads tobj (GX_TF_RGBA8, sofdec.cpp:325) on GX_TEXMAP0 and blend SRCALPHA/INVSRCALPHA; stage 0's inputs/order/texgen are inherited from the previous draw. m_draw_mode 1 looks like a test path (GXDrawTorus); treat as unused unless the retail data sets it.

**NV2A.** fits 8 stages: yes · textures: 1 · texture shader: no · passes: 1. Inherits whatever is current; nothing to compile.

### src/Sscrn/ss_item_draw.cpp

#### `ss_item_draw_line3d_local` — ss_Draw_line3d_local (ss_item_draw.cpp:157)

**Purpose.** Sub-screen 3D line (puzzle case): vertex colour pass-through, selectable blend, z test without write.

**Callers.** ss_Draw_line3d_trans (ss_item_draw.cpp:151) OT callback of ss_Draw_line3d

**Variant / condition.** blend 0 -> BLEND ONE/ZERO, 1 -> SRCALPHA/INVSRCALPHA, 2 -> SRCALPHA/ONE, 3 -> ONE/ONE (ss_item_draw.cpp:161-174); zupd -> z compare on (LEQUAL, no write) else off (177-181)

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | PASSCLR | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: ss_item_draw.cpp:190-191.

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — ss_item_draw.cpp:187

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare on, LEQUAL, no write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** ss_item_draw.cpp:161-203. GX_LINES, camera projection and view matrix.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Trivial.

#### `ss_item_draw_tile3d_local` — ss_Draw_tile3d_local (ss_item_draw.cpp:231)

**Purpose.** Sub-screen 3D quad (puzzle grid cells): vertex colour pass-through, selectable blend, no z write.

**Callers.** ss_Draw_tile3d_trans (ss_item_draw.cpp:227) <- ss_pzzl

**Variant / condition.** blend 0 -> BLEND ONE/ZERO, 1 -> SRCALPHA/INVSRCALPHA, 2 -> SRCALPHA/ONE, 3 -> ONE/ONE (ss_item_draw.cpp:235-248); the OT callback always passes zupd = 0 (227)

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | PASSCLR | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: ss_item_draw.cpp:264-265.

**Channels / lights.**
- COLOR0A0: disabled, amb SRC_REG, mat SRC_VTX, lights `GX_LIGHT_NULL`, DF_NONE, AF_NONE — ss_item_draw.cpp:252

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_INVSRCALPHA LO_CLEAR · z compare off, LEQUAL, no write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** ss_item_draw.cpp:235-281. GX_QUADS.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Trivial.

### src/Sscrn/ss_main.cpp

#### `ss_main_clearzbuffer` — clearZbuffer (ss_main.cpp:741)

**Purpose.** Sub-screen Z clear quad (shop / puzzle screens): identical program to trans ClearZbuf.

**Callers.** ss_shop.cpp:303-304 (OT 0xF, 0x14); ss_pzzl.cpp:186 (OT 0xF)

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR_NULL | - | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, ZERO | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: ss_main.cpp:758-762.

**Channels / lights.**
- COLOR0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_SRCALPHA/BL_ONE LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog unchanged

**Notes.** ss_main.cpp:744-777: colour/alpha update off, ortho 512x448, mat {8,8,0x80,0x1C}.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Depth-only clear in the shim.

### src/game/gx_sub.cpp

#### `gx_sub_addbgcolor` — bio4_AddBgColor (gx_sub.cpp:30)

**Purpose.** Background colour quad at the far plane, added (ONE/ONE) over the black-cleared frame after the model pass.

**Callers.** Render (trans.cpp:939) after OT_TYPE_SHADOW_DRAW

**Variant / condition.** only when bg != 0 (gx_sub.cpp:70): bg = g_sysBgColor override (STA_SET_BG_COLOR) or LightEnv.Fog.Color (black when Fog.Type == 0), alpha 0

**Stages:** 1 · **texgens:** 0 · **channels:** 1 · **textures bound:** 0

| st | coord | map | chan | via | colour a,b,c,d | op/bias/scale/clamp → out | alpha a,b,c,d | op/bias/scale/clamp → out | kcsel / kasel | swap ras/tex | indirect |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | TEXCOORD_NULL | TEXMAP_NULL | COLOR0A0 | - | ZERO, ZERO, ZERO, RASC | ADD/ZERO/SCALE_1/clamp → TEVPREV | ZERO, ZERO, ZERO, RASA | ADD/ZERO/SCALE_1/clamp → TEVPREV | - | - | - |

Stage sources: **st0**: gx_sub.cpp:74-78 RASC / RASA (material register = bg colour).

**Channels / lights.**
- COLOR0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — gx_sub.cpp:72; mat = amb = bg (67-68)
- ALPHA0: disabled, amb SRC_REG, mat SRC_REG, lights `GX_LIGHT_NULL`, DF_CLAMP, AF_NONE — gx_sub.cpp:73

**Blend / Z / alpha compare / cull / fog.** blend BM_BLEND BL_ONE/BL_ONE LO_CLEAR · z compare on, ALWAYS, write · z-comp-loc unchanged · alpha compare unchanged · cull GX_CULL_NONE · fog GX_FOG_NONE during the quad (gx_sub.cpp:38), LightMgr.setFog() restored after (90)

**Notes.** gx_sub.cpp:38-89: GXSetNumTevStages(1)/NumChans(1)/NumTexGens(0) are set before the condition (48-50); colour and alpha update on during the quad, alpha update off after. The real clear colour is black (bio4_GXSetCopyClear, gx_sub.cpp:19-24): the background is painted additively where the depth is still at the far plane.

**NV2A.** fits 8 stages: yes · textures: 0 · texture shader: no · passes: 1. Constant colour quad with additive blend and z test ALWAYS + write; or clear to the colour directly when nothing relies on the EFB alpha being 0.

### Files in part A with no TEV configuration

- `src/game/TexRender.cpp`: render-to-texture management only — `RenderTexRenderMgr` resizes the EFB/scissor (TexRender.cpp:78-103), `CopyTexRenderMgr` does `GXSetTexCopySrc/Dst(RGBA8) + GXCopyTex` into the target buffer and builds the `GXTexObj` with wrap by repeat type (TexRender.cpp:105-165). Its textures are consumed by `org_LoadTexObj` ids ≥ 0xF8 (trans.cpp:316-325) and by `mirrorModelTrans2`. It also sets `Shader_type 2 / Refract_pow 0x10 / Refract_ratio 0x90 / z_mode 2 / blend_mode 1` on the models it renders (TexRender.cpp:244-284) — the selectors of the refraction cases.
- `src/game/EtcModel.cpp`: no GX calls at all (model table management).
- `src/game/trans_lit.cpp`: lighting only, recorded as channel-only cases above.
- `GetEfbTex` (trans.cpp:2877-2894), `getEFB` (examine.cpp:281-293), `SoftShadowGetEFB` (shadow.cpp:844-856), `CopyTexRenderMgr` and the A8 copy in `make_shadow_texture` (shadow.cpp:1133-1148) are the EFB→texture producers in part A; they are listed in the `efb_copy_inputs` of the cases that sample them.

### Totals (part A)

- Cases: 49 (42 with a TEV program, 6 channel-only lighting records, 1 stale-stage record `sofdec_drawtex_mode1`).
- Stage-count distribution (TEV cases): 1 stage: 21, 2 stages: 3, 3 stages: 4, 4 stages: 9, 5 stages: 4, 6 stages: 1, 10 stages: 1.
- Max stages: 10 (`trans_shadersetup_full`, synthetic composition); the largest single observed program is 6 (`trans_shadersetup_selfshadow`, 5 + SelfShadowLevel), then 5 (`trans_shadersetup_blend2`, `trans_shadersetup_specular2`, `trans_shadersetup_castshadow_kind3`, `shadow_shadowshadersetup2_4_eqcol`). Materials with every block enabled can reach 10-14 stages (see the `full` case notes).
- Max textures per pass: 7 texmaps (`trans_shadersetup_full`); 4 in `trans_shadersetup_specular2` (3 distinct) and `shadow_shadowshadersetup2_4_eqcol`; everything else ≤ 3.
- Max texgens 6, max konst colours 3 (4 in sofdec), max TEV registers 2 (REG0/REG1 in blend2), max indirect stages 1 everywhere (the counter allows 4, `IND_STAGE_ID` trans.cpp:272).
- Indirect texturing: 5 cases (`specular_bump` BumpXYZ with `GX_ITM_0`; `specular2`, the three refraction cases with `GXSetTevIndWarp` signed/no-replace `GX_ITM_1`). Always `GX_ITF_8`, coord scale `GX_ITS_1`, one indirect stage, indirect matrices `GX_ITM_0` (updateMatrices) and `GX_ITM_1` only.
- `GXSetTevOp` shortcuts: `GX_PASSCLR` dominates (libgpu, objWep, ss_item_draw ×2, make_shadow_texture no-self, TransLightTexture (overridden), t_prim); `GX_REPLACE` in DrawTexture, drawTexture2, restoreTevPrm (and the dead examine branch); `GX_MODULATE` once, on stage 1 of clothTrans. `GX_DECAL` and `GX_BLEND` are never used in part A. The model pipeline never uses the shortcut — every stage is hand-built with `GXSetTevColorIn/Op` + `GXSetTevAlphaIn/Op`.
- Hand-built stage shapes that recur: `TEXC·RASC` (material), `d + TEXC·KONST`, `lerp(prev, reg, KONST)`, `RASC·(1-CPREV)` (shadow mask), `CPREV + (1-TEXA)` (depth compare), `CPREV²`, pass-through `CPREV` with the final `×4` scale.
- TEV features used: `GX_TEV_ADD`, `GX_TEV_SUB` (specular2, sofdec), `GX_TEV_COMP_RGB8_GT/A8_GT` (shadow EQCOL stage only), bias `GX_TB_ADDHALF` (specular2 only), scales ×1/×2/×4 (never `DIVIDE_2`), clamp off only in sofdec, output registers `TEVPREV/REG0/REG1` (never REG2), konst selects `K0..K3` and `K0_A..K3_A` only (no fractional konst), `GXSetTevColorS10` once (sofdec), `GXSetTevColor` never (part A), swap modes/tables only in shadow.cpp and sofdec.cpp (`GX_TEV_SWAP0..3`), channels `GX_COLOR0A0`, `GX_COLOR1A1` (cast shadow kinds 3/4), `GX_COLOR_NULL`.
- Texgens: `GX_TG_MTX2x4` from `GX_TG_TEX0` (identity or a translation matrix), `GX_TG_MTX3x4` from `GX_TG_NRM` (env maps) and from `GX_TG_POS` (projective shadow/screen), `GX_TG_MTX2x4` from `GX_TG_POS` (depth ramp) and from `GX_TG_NRM` (refraction type 1 offset map). Never used: `GX_TG_BUMP*`, `GX_TG_SRTG`, texgen `normalize`, post-transform matrices other than `GX_PTIDENTITY`, sources TEX1..7/COLOR.
- Channels: at most 2 (`GXSetNumChans(2)` only in `SetCastShadowLight`); 0 in sofdec. `GX_DF_SIGN` and `GX_AF_SPEC` never used; `GX_DF_NONE` only in effects lighting, foot shadows and the 2D helpers. Up to 8 lights (bit i = list slot i), spot lights `GX_SP_SHARP` (cast light, `GXInitLightSpot(89, 4)`), `GX_SP_FLAT` (shadow receivers), `GX_SP_COS` (light types 3/6); `GXInitLightDistAttn` only in light type 3 and the cast light (`GX_DA_OFF`).
- Blend: `BM_BLEND` with SRCALPHA/INVSRCALPHA, SRCALPHA/ONE, ONE/ONE, ONE/ZERO, DSTCLR/ONE (model blend_mode 3), DSTALPHA/ZERO (TransLightTexture); `BM_NONE`; `GX_BM_LOGIC` and `GX_BM_SUBTRACT` never; logic op SET appears only as the unused op argument in sofdec. Z: LEQUAL and ALWAYS only; `GXSetZCompLoc(FALSE)` in alphaSetup, cloth and at init (main_sub.cpp:116). Alpha compare: ALWAYS, `GREATER ref OR GREATER 0xFF`, `GREATER 1 OR GREATER 1`, `GREATER 4 OR GREATER 0xFF` — `GX_AOP_AND/XOR/XNOR` and the other compares never. Fog: data-driven `LightEnv.Fog.Type` via `LightMgr.setFog`, `GX_FOG_NONE` in thermo/examine/background quads. `GXSetDstAlpha` is used (model pass writes alpha 0; shadow-map pass writes a constant).
- EFB copies consumed by part-A programs: RGBA8 half-size screen (refraction `g_Get_tex_obj`, examine `local_buff`), RGBA8 render-to-texture (TexRender → material ids ≥ 0xF8, mirror), `GX_CTF_A8` shadow maps read as I8 (shadow receivers, self shadow, soft-shadow taps).
- NV2A fit: every observed single program fits 8 combiner stages and 4 textures except `sofdec_settevprm` (needs native YUV texture instead) and the swizzle-dependent multi-light shadow receiver (needs the dot-product formulation or one pass per light). Texture shaders are needed for the bump/indirect cases (5 cases). The synthetic `full` composition needs 2 passes.

### Appendix: debug-only / not compiled

- `src/game/t_prim.cpp` (**not compiled** into the port, absent from `xbox/units.mk`): `set_attr_common` (t_prim.cpp:76-92) — 1 stage `GXSetTevOp(0, GX_PASSCLR)`, order NULL/NULL/COLOR0A0, `GXSetChanCtrl(COLOR0, off, amb REG, mat VTX, LIGHT0, DF_NONE, AF_NONE)`, no texgens, cull NONE, z LEQUAL by `FlipMode`; `TprimSetBlend` (t_prim.cpp:60-72): NONE / ONE+ONE / ZERO+SRCCLR. `tprim_default_env` is never called.
- `shadowShaderSetup` (shadow.cpp:1356-1375): dead-stripped (STRIP_UNUSED), lights only.
- `primBuffDebugDisp` (trans.cpp:2475-2483): dead-stripped, viewport only.
- `drawTexture2` (shadow.cpp:1717) and the `DBG_SHADOW_LIGHT` block of `shadowShaderSetup2` (shadow.cpp:1455-1489): compiled, but only reachable through `pG->debug_mode == 0xE` / debug flags; `drawTexture2` has a case, the DBG block is described in that case's notes.
- `examine.cpp:358` `GXSetTevOp(0, GX_REPLACE)` and `TextureBlend3`'s `use_alp == 0` path (trans.cpp:1703-1731): dead (static flags).
- `sofdec` `m_draw_mode 1` (torus test draw): recorded as `sofdec_drawtex_mode1`, likely unused by retail data.


---

## TEV / GX usage catalogue — part B (CT-05)

Part B of the CT-05 catalogue: effects (`esp_sub.cpp`, `esp*.cpp`, `espgen45.cpp`, `Espgen42/43.cpp`), the
post-process filters (`filter00..0b.cpp`), the HUD/ID system (`id_sys.cpp`, `id_tex.cpp`), text (`mes.cpp`,
`eprintf.cpp`), `fade.cpp`, `cinesco.cpp`, `eff_sys.cpp`, and a short appendix for the debug unit `dbmodule.cpp`.
Machine-readable twin: `xbox/port/gx/tests/tev_cases.json` (49 cases, validates against
`tev_cases.schema.json`). Part A (trans.cpp, shadow.cpp, …) is a separate document; the two are merged later.

All line numbers refer to `src/game/<file>` on branch `xbox` at the time of writing. The decomp passes GX enums
as raw integers; they are decoded with `include/dolphin/gx/GXEnum.h` (e.g. `GXSetTevOrder(0, 0xFF, 0xFF, 4)` =
`GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0`; `0x1E` = `GX_TEXMTX0`, `0x21` = `GX_TEXMTX1`, `0x3C` =
`GX_IDENTITY`, `0x7D` = `GX_PTIDENTITY`; copy formats `6` = `GX_TF_RGBA8`, `1` = `GX_TF_I8`, `9` = `GX_TF_C8`,
`0x11` = `GX_TF_Z8`, `0x27` = `GX_CTF_A8`, `0x29` = `GX_CTF_G8`).

### 0. Conventions

#### 0.1 `GXSetTevOp` expansion (src/lib/GXTev.c)

`GXSetTevOp(stage, mode)` overwrites the whole low 24 bits of the colour and alpha TEV registers (inputs, op,
bias, scale, clamp, dest) with the table entry, so anything set earlier on that stage is lost; stage 0 reads the
rasterised colour where later stages read the previous stage. Every entry has op ADD, bias ZERO, scale 1, clamp
TRUE, out TEVPREV.

| mode | stage 0 colour a,b,c,d | stage 0 alpha | stage ≥1 colour | stage ≥1 alpha |
|---|---|---|---|---|
| GX_MODULATE | ZERO, TEXC, RASC, ZERO | ZERO, TEXA, RASA, ZERO | ZERO, TEXC, CPREV, ZERO | ZERO, TEXA, APREV, ZERO |
| GX_DECAL | RASC, TEXC, TEXA, ZERO | ZERO, ZERO, ZERO, RASA | CPREV, TEXC, TEXA, ZERO | ZERO, ZERO, ZERO, APREV |
| GX_BLEND | RASC, ONE, TEXC, ZERO | ZERO, TEXA, RASA, ZERO | CPREV, ONE, TEXC, ZERO | ZERO, TEXA, APREV, ZERO |
| GX_REPLACE | ZERO, ZERO, ZERO, TEXC | ZERO, ZERO, ZERO, TEXA | same | same |
| GX_PASSCLR | ZERO, ZERO, ZERO, RASC | ZERO, ZERO, ZERO, RASA | ZERO, ZERO, ZERO, CPREV | ZERO, ZERO, ZERO, APREV |

Part B uses only MODULATE (9 stages), PASSCLR (5) and REPLACE (1) as shortcuts; DECAL and BLEND never appear.

#### 0.2 Other shortcuts

- `GXSetTevIndWarp(st, ind, signed, replace, mtx)` (src/lib/GXBump.c:186) = `GXSetTevIndirect(st, ind,
  GX_ITF_8, signed ? GX_ITB_STU : GX_ITB_NONE, mtx, wrap_s = wrap_t = (replace ? GX_ITW_0 : GX_ITW_OFF),
  add_prev FALSE, utc_lod FALSE, GX_ITBA_OFF)`. Every indirect use in part B is this call with `GX_ITM_1`, and
  every indirect matrix is loaded with `GXSetIndTexMtx(GX_ITM_1, m, scale_exp = 1)`, every indirect stage with
  `GXSetIndTexOrder(GX_INDTEXSTAGE0, …)`, `GXSetIndTexCoordScale(GX_INDTEXSTAGE0, GX_ITS_1, GX_ITS_1)` and
  `GXSetNumIndStages(1)`.
- "Half-size EFB copy" below means `GXSetTexCopySrc(0, ofs, Screen.width, Screen.height-ofs);
  GXSetTexCopyDst(Screen.width/2, Screen.height/2 - ofs, GX_TF_RGBA8, mipmap TRUE); GXCopyTex(buf, clear FALSE)`
  with `ofs = 56` (0 while `STA_TEX_RENDER`), followed by `GXPixModeSync(); GXInvalidateTexAll()`. The filters
  use the `SCR_W/SCR_H` variants listed per case (always with the copy filter turned off around the copy).
- Channel notation: `ChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)`.
- Z notation: `ZMode(compare, func, update)`; alpha compare `AC(comp0, ref0, op, comp1, ref1)`.
- "Mask stage" = the pattern shared by 8 cases: `GXSetTevColorIn(1, ZERO, ZERO, ZERO, CPREV)`,
  `GXSetTevAlphaIn(1, ZERO, TEXA, RASA, ZERO)` — colour passes, alpha = mask alpha × raster alpha. Note that the
  stage-0 texture alpha is **discarded**, not multiplied.

#### 0.3 Helpers inlined into the cases (no case of their own)

| helper | file:lines | effect |
|---|---|---|
| `EspTexSet(id, ptn)` | eff_sys.cpp:454-467 | `GXLoadTexObj(pattern, GX_TEXMAP0)`, `GXLoadTlut(.., GX_TLUT0)` for CI, `GXLoadTexMtxImm(w->_Mtx, GX_TEXMTX0, 2x4)` |
| `cEsp::CommonStateSet()` | esp_sub.cpp:1071-1080 | `GXSetCullMode(NONE); AC(GREATER,1,OR,GREATER,1); ZMode(TRUE,LEQUAL,FALSE); GXSetNumTevStages(1); GXSetTevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0); GXSetNumTexGens(1); GXSetTexCoordGen(0, MTX2x4, TEX0, TEXMTX0)` |
| `cEsp::ChannelSet()` | esp_sub.cpp:975-1050 | `Tool_flg & 0x40`: `GXSetTevOp(0, MODULATE); ColorIn(0, ZERO,TEXC,RASC,ZERO); ColorOp(0, ADD,ZERO,SCALE_4,TRUE,PREV); commonEspLightSet(...)` (trans_lit.cpp, part A). Else: `GXSetTevOp(0, MODULATE)`; `0x80` → `ColorOp scale 4` (987); `0x20000` → `AlphaOp scale 4` (990); `GXSetNumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, REG, 0, NONE, NONE)`. Always `GXSetChanMatColor(COLOR0A0, col)` (1043) with the effect colour × final colour × distance fade. Returns 0 when alpha is 0 (caller skips the draw). |
| `EspChannelSet09` | esp09.cpp:168-178 | `GXSetTevOp(0, MODULATE)`; `0x80` → `ColorIn(0, ZERO,TEXC,RASC,ZERO) + scale 4`; `GXSetNumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX, 0, NONE, NONE)` |
| `IdTexSet(id, no)` | id_tex.cpp:85-111 | `GXLoadTexObj(tex, TEXMAP0)` (+`GXLoadTlut(.., TLUT0)`), `ZMode(FALSE, LEQUAL, FALSE)`, `GXSetNumTevStages(1)`, `GXSetTevOp(0, MODULATE)`, `GXSetTevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0)`, `GXSetNumTexGens(1)`, identity `GX_TEXMTX0`, `GXSetTexCoordGen(0, MTX2x4, TEX0, TEXMTX0)` |
| `IdChannelSet(u)` | id_tex.cpp:120-132 | `GXSetTevOp(0, MODULATE); GXSetNumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, REG, 0, NONE, NONE); GXSetChanMatColor(COLOR0A0, u->col)` |
| `IdGeneralTrans(u)` | id_sys.cpp:1059-1089 | dispatcher: `GXSetFog(GX_FOG_NONE, …)`; `trans_type` 0/default → `IdCommonTrans`, 1 → `IdNegativeTrans(u, pow<=1 ? pow : 2)`, 2/3 → `IdShimmerTrans(u, pow, trans_type)`; `LightMgr.setFog()` after |
| `Filter0xGetEFB` | each filter | EFB copy helpers, described per case |
| `Esp1b_SpTrans` | esp_sub.cpp:1503-1590 | vertex-format/geometry only, no pipeline state |

`GXSetNumTevStages` callers that are helpers (`cEsp::CommonStateSet`, `IdTexSet`, `Esp09_Trans_Setup`,
`filter0a_mask_tex`) are therefore covered through the cases that include them.

---

### 1. esp_sub.cpp

#### 1.1 `EspCommonTrans` (74-345) — generic sprite, 4 cases

Default draw of every sprite effect without its own Trans (EspTransTbl, esp_app.cpp:124…; also `esp0a`, `esp0b`,
`esp0e`, `esp47` call it directly). Dispatches to `EspCommonTransShimmer` (87, `m_Shimmer_type != 0`) or
`EspCommonTransNega(esp, 2)` (91, `Tool_flg & 0x2000`). Otherwise: `GXSetBlendMode(m_Blend_mode, m_Src_factor,
m_Dst_factor, m_Logic_op)` (94, runtime), `EspTexSet` (110) + `CommonStateSet()` (113) on the first sprite of a
batch (`OtGetPrevKind() != 8`), `ChannelSet()` (148) per sprite, then the display list `g_EspCommonDisplayList`.

| case | variant | stage 0 (coord0 / map0 / COLOR0A0) | extra |
|---|---|---|---|
| `esp_sub_esp_common_trans_base` | no 0x40/0x80/0x4000/0x20000 | `GX_MODULATE` → C: ZERO,TEXC,RASC,ZERO; A: ZERO,TEXA,RASA,ZERO; ×1 | — |
| `esp_sub_esp_common_trans_scale4` | 0x80 and/or 0x20000 (985-990) | MODULATE, colour scale 4 (0x80) / alpha scale 4 (0x20000) | — |
| `esp_sub_esp_common_trans_lit` | 0x40 (979-983) | ColorIn ZERO,TEXC,RASC,ZERO ×4; alpha MODULATE | channel from `commonEspLightSet` (part A) |
| `esp_sub_esp_common_trans_mask` | 0x4000 (274-308) | any of the above | stage 1 coord1/map1/COLOR0A0 mask stage, alpha ×4 when 0x20000 (302-306) |

Common state: texgen0 `MTX2x4 TEX0 TEXMTX0`; channel `ChanCtrl(COLOR0A0, FALSE, REG, REG, 0, NONE, NONE)`,
material = effect colour; `ZMode(TRUE, LEQUAL, FALSE)`, `AC(GREATER,1,OR,GREATER,1)`, cull NONE. Mask: GX_TEXMAP1 =
`m_MaskTex_id`/`m_MaskPtn_no` (GX_TLUT1 for CI, 283-293), `GXLoadTexMtxImm(tw->_Mtx, TEXMTX1)`, texgen1
`MTX2x4 TEX0 TEXMTX1` (295), `GXSetNumTevStages(2); GXSetNumTexGens(2)` (296-297), restored to 1/1 at 330-331.
Fixed-function extras collapsed into the cases: `0x200000` → `ZMode(TRUE,LEQUAL,TRUE)` + `GXSetDstAlpha(TRUE,0)`
(318-319), `0x100000` → `AC(GREATER,0x80,OR,GREATER,0x80)` (322), `0x8000`/`0x800000` → `GXSetAlphaUpdate(TRUE)`
(314), all undone after the draw (334-342). Textures: 1 (2 with mask), none from the EFB.

NV2A: 1 combiner stage (tex0·col0); mask = second texture, alpha half `tex1.a·col0.a` fits the same stage. The
lit variant needs the vertex-lighting path. Scale ×4 is native.

#### 1.2 `EspCommonTransShimmer` (347-640) — heat shimmer, 3 cases

Called from `EspCommonTrans` for `m_Shimmer_type` (= `Blur_type`) 1/2/3. `ChannelSet()` first (383; its stage-0
TEV inputs are overwritten at 579-582, its channel stays). Fixed state: cull NONE (390), `AC(GREATER,1,…)` (391,
426), `ZMode(TRUE,LEQUAL,FALSE)` (392), blend runtime (427), `GXSetFog(GX_FOG_NONE,…)` (456) restored by
`LightMgr.setFog()` (639).

Textures:
- GX_TEXMAP0 = the sprite's own texture (`EspTexSet`, 425) — used **only** as the indirect map.
- GX_TEXMAP1 = half-size EFB copy (474-481); `buf = GetDrawTmpBufAddr(Tool_flg & 0x1000 ? 2 : 1)`, the copy is
  skipped (`copyOk = 0`) when `0x1000` and `GetDrawTmpBufType() == 2`, i.e. the previous copy is reused.
- GX_TEXMAP2 = mask texture when `0x4000` (587-602, GX_TLUT1 for CI).

Texgens: 0 = screen parts: `MTX2x4 TEX0 TEXMTX0` with `Matrix1|Matrix2 × m_Mat` (490-492); world parts:
`MTX3x4 POS TEXMTX0` with `C_MTXLightPerspective(Fovy, 4/3, 0.5, -0.667, 0.5, 0.5) × m_Mat` (494-497)
(projective screen lookup). 1 = `MTX2x4 TEX0 IDENTITY` (501) for the indirect lookup. 2 = `MTX2x4 TEX0 TEXMTX1`
(604) for the mask.

Indirect: `GXSetNumIndStages(1)` (500); `GXSetIndTexOrder(0, TEXCOORD1, TEXMAP0)` (503); scale 1,1 (504);
`GXSetIndTexMtx(GX_ITM_1, m, 1)` (551) with `m = diag(k,k)`, `k = m_Col_a/255 · 0.04 · 1000/dot · (1+u_pow/32)`,
`dot = max(1500, view depth)` (2500 for screen parts) — or the static `{0.5, 0, 0 / 0, H·0.5/W, 0}` for type 3
(532-549). `GXSetTevIndWarp(0, 0, signed, replace, GX_ITM_1)` (576): type 1 → (0,0), type 2 → (1,0), type 3 and the
error default → (0,1).

| stage | order | colour a,b,c,d → op | alpha | lines |
|---|---|---|---|---|
| 0 | coord0 / **map1** / COLOR0A0 | ZERO,TEXC,RASC,ZERO → ADD ×1 PREV (indirect warp) | ZERO,ZERO,ZERO,RASA | 578-582 |
| 1 (mask) | coord2 / map2 / COLOR0A0 | ZERO,ZERO,ZERO,CPREV | ZERO,TEXA,RASA,ZERO, ×4 if 0x20000 | 605-612 |

`GXSetNumTevStages(1|2); GXSetNumTexGens(2|3)` (618-619); teardown `NumTevStages(1), NumTexGens(0),
NumIndStages(0), TevDirect(0), TevDirect(1)` (633-637).

Cases: `esp_sub_esp_common_trans_shimmer` (type 1/2), `esp_sub_esp_common_trans_shimmer_replace` (type 3),
`esp_sub_esp_common_trans_shimmer_mask` (0x4000). NV2A: the indirect warp is a dependent read →
`OFFSET_TEXTURE_2D` texture shader on the EFB-copy texture with the sprite texture as the offset map (must be
uploaded/converted to a signed DS/DT format); replace mode = offset with zero base coordinate. 2-3 textures, 1-2
combiner stages, one pass.

#### 1.3 `EspCommonTransNega` (643-810) — "negative" sprite, 1 case `esp_sub_esp_common_trans_nega`

Called only with `type == 2` (esp_sub.cpp:91). `ChannelSet()` (669), `CommonStateSet()` (710), blend runtime
(709), fog NONE (734). GX_TEXMAP0 = sprite texture (708); GX_TEXMAP1 = half-size EFB copy into
`GetDrawTmpBufAddr(3)` (741-747). Texgen0 projective/screen as in 1.2 (749-762); texgen1 `MTX2x4 TEX0 IDENTITY`
(784). `GXSetNumTexGens(2)` (783), `GXSetNumTevStages(3)` (791).

| stage | order | colour → op | alpha | lines |
|---|---|---|---|---|
| 0 | coord0 / map1 / COLOR0A0 | ZERO,ZERO,ZERO,TEXC → ADD scale ×1/×2/×4 (type 0/1/2) | ZERO,ZERO,ZERO,RASA | 763-777 |
| 1 | NULL / NULL / COLOR0A0 | RASC,ZERO,CPREV,ZERO = (1−CPREV)·RASC | ZERO,ZERO,ZERO,RASA | 778-782 |
| 2 | coord1 / map0 / COLOR0A0 | ZERO,ZERO,ZERO,CPREV | ZERO,APREV,TEXA,ZERO = mat.a·tex.a | 786-790 |

Result: material colour × inverted (4× screen), alpha from the sprite texture. Z `(TRUE, LEQUAL, FALSE)`,
AC GREATER 1, cull NONE. Teardown 805-809. NV2A: 3 combiner stages, 2 textures, projective texgen is a vertex
matrix; no texture shader.

---

### 2. esp*.cpp

#### 2.1 `Esp03_Trans` (esp03.cpp:78-115) — `esp03_esp03_trans`

Untextured polygon debris. `ZMode(TRUE,LEQUAL,FALSE)` (86), cull NONE (87), `GXSetNumTexGens(0)` (88),
`GXSetNumTevStages(1)` (89), `GXSetTevOrder(0, NULL, NULL, COLOR0A0)` (90), `ChannelSet()` (108), then
`GXSetTevOp(0, GX_PASSCLR)` (109) which overwrites ChannelSet's MODULATE/scale (0x80/0x20000 have no effect).
Stage 0: C ZERO,ZERO,ZERO,RASC; A ZERO,ZERO,ZERO,RASA. Channel `COLOR0A0 FALSE REG REG` (mat = effect colour);
the `GX_VA_CLR0` attribute sent at 113-116 is ignored unless `0x40` (lit). AC GREATER 1 (110), blend runtime
(111). 0 textures. NV2A: constant colour.

#### 2.2 `Esp08_Trans` (esp08.cpp:413-533) — tiled sprite, `esp08_esp08_trans_mask`

Without a mask this is exactly `esp_sub_esp_common_trans_base`/`_scale4`/`_lit` (`EspTexSet` 499, `ChannelSet()`
500, blend 501, `CommonStateSet()` 502) drawn as the `ESP08_TILES` grid (102-…). With `Tool_flg & 0x4000` the
`ESP08_MASK_SET(1, 1, …)` macro (297-346) adds: GX_TEXMAP1 = mask (GX_TLUT1 for CI, 311-319),
`GXLoadTexMtxImm(tw->_Mtx, TEXMTX1)` (320), texgen1 `GXSetTexCoordGen2(1, MTX2x4, Mask_type==1 ? TEX1 : TEX0,
TEXMTX1, FALSE, PTIDENTITY)` (321-325) — Mask_type 1 uses a second UV attribute (`GX_VA_TEX1`, 350-353) spanning
the whole tiled sprite —, `GXSetNumTevStages(2); GXSetNumTexGens(2)` (326-327), stage 1 coord1/map1/COLOR0A0 mask
stage, alpha ×1 only (328-332). Restored 525-526, `GXSetAlphaUpdate(FALSE)` 529.

#### 2.3 `Esp08_TransShimmer` (esp08.cpp:535-768) — `esp08_esp08_trans_shimmer`, `_shimmer_mask`

Same program as 1.2 with these differences: `ChannelSet()`+`CommonStateSet()` first (630-632, overwritten),
`buf = GetDrawTmpBufAddr(1)` always, copy always taken (667-669), explicit `GXSetNumTevStages(1)` (695),
`dot = 2500` fixed (703), projective matrix offset `-0.667` or `-0.5` by `SYS_SCISSOR_ON` (684-690),
`m_Shimmer_type` 1/2/3 → `GXSetTevIndWarp(0,0, 0/1/0, 0/0/1, ITM_1)` (723-745), mask via `ESP08_MASK_SET(2, 2)`
(752: GX_TEXMAP2, texgen2 `TEX0|TEX1 TEXMTX1 PTIDENTITY`, alpha ×1). Tiled (`ESP08_TILES` 753). Teardown 761-765.

#### 2.4 `Esp09_Trans_Setup` (esp09.cpp:181-220) + `EspChannelSet09` (168-178) — `esp09_esp09_trans_setup`

Trails. `ZMode(TRUE,LEQUAL,FALSE)` (185), cull NONE, `NumTexGens(0)`, `NumTevStages(1)`, `TevOrder(0, NULL,
NULL, COLOR0A0)` (187-189), `EspChannelSet09` (204: MODULATE, optional ×4, `ChanCtrl(COLOR0A0, FALSE, REG, VTX,
0, NONE, NONE)`), `GXSetTevOp(0, GX_PASSCLR)` (205) — again overriding the MODULATE/scale, so `0x80` is dead.
AC GREATER 1 (206), blend runtime (207). When `m_Tex_id != 0xFF` `EspTexSet` binds GX_TEXMAP0 and a texcoord
attribute is sent (214-216) but no texgen/stage reads it: the texture is unused (decomp-faithful). Vertex colour.

#### 2.5 `Esp0f_Trans` (esp0f.cpp:47-215) — `esp0f_esp0f_trans`

Identical TEV program to 1.3 (`EspCommonTransNega`), stage 0 scale by `w->Pow` 0/1/2 (168-177). Differences:
copy of the whole screen `GXSetTexCopySrc(0,0,W,H); GXSetTexCopyDst(W/2, H/2, RGBA8, TRUE)` into
`GetDrawTmpBufAddr(3)` (143-150, GX_TEXMAP1), LightPerspective offset `-0.5` (160), `GXSetAlphaUpdate(FALSE)` at
the end (213). GX_TEXMAP0 = sprite texture (110). `ChannelSet()`/`CommonStateSet()` 111-113. Lines: stages
166-193, `NumTexGens(2)` 186, `NumTevStages(3)` 194, teardown 208-212.

#### 2.6 `Esp18_Trans` (esp18.cpp:74-395) — radial blur layers, `esp18_esp18_trans`

`ChannelSet()` (101), cull NONE / AC GREATER 1 / `ZMode(TRUE,LEQUAL,FALSE)` (113-115, 149), blend runtime (150).
Loop `esp18_lp = 8` layers (212): fog NONE (221), half-size EFB copy at `i == 0` (246-250; buffer 2 when
`0x1000`, skipped if `GetDrawTmpBufType()==2`) → GX_TEXMAP1 (252-253). Texgen0 screen `MTX2x4 TEX0 TEXMTX0`
(zoomed about the centre by `m1·m2·m3` when `blur_rate != 0`, 288-298) or projective `MTX3x4 POS TEXMTX0` with a
rotating per-layer offset `(rx, ry)` (300-304). `GXSetNumIndStages(1)` (307), texgen1 `MTX2x4 TEX0 IDENTITY`
(308), `GXSetIndTexOrder(0, TEXCOORD1, TEXMAP0)` (310), scale 1,1 (311) — **but no `GXSetIndTexMtx` and no
`GXSetTevIndWarp`/`GXSetTevIndirect` anywhere in this function**; every shimmer draw ends with
`GXSetTevDirect(0)`, so stage 0 is a direct lookup and the configured indirect stage is dead. Per layer
`GXSetChanMatColor(COLOR0A0, {r,g,b, f(a)/(i+2)})` (328).

| stage | order | colour | alpha | lines |
|---|---|---|---|---|
| 0 | coord0 / map1 / COLOR0A0 | ZERO,TEXC,RASC,ZERO | ZERO,ZERO,ZERO,RASA | 329-333 |
| 1 | coord2 / map2 / COLOR0A0 | ZERO,ZERO,ZERO,CPREV | ZERO,TEXA,RASA,ZERO, ×4 if 0x20000 | 361-369 |

GX_TEXMAP2 = the sprite's **own** texture again (`TEXGet(tw->Tpl_addr, m_Ptn_no)`, 345-358, GX_TLUT1 for CI) with
`tw->_Mtx` in TEXMTX1, texgen2 `MTX2x4 TEX0 TEXMTX1` (360). `GXSetNumTevStages(2); GXSetNumTexGens(3)` (374-375)
(1/2 if the texture work is missing). Teardown per layer 389-393. NV2A: 2 live textures, 2 combiner stages, 8
draws; a faithful shim must notice that the indirect stage is never bound to a TEV stage.

#### 2.7 `Draw_line3d_local_222` (esp19.cpp:59-110) — `esp19_draw_line3d_local_222`

Laser line. Blend runtime (67), cull NONE (69), `ZMode(FALSE|TRUE, LEQUAL, TRUE)` by alpha byte `0xFE` (70-74),
`GXSetNumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX, 0, NONE, NONE)` (75-76), `NumTexGens(0)`,
`NumTevStages(1)`, `TevOrder(0, NULL, NULL, COLOR0A0)`, `GXSetTevOp(0, GX_PASSCLR)` (77-80). Vertex colour; AC
inherited.

#### 2.8 Other esp files

`esp01/02/04/12/16.cpp` call `EspTexSet` + `ChannelSet()` + `CommonStateSet()` and set the blend only (they are
callers of `esp_sub_esp_common_trans_base`). `esp0e/esp45/espgen01` only `GXPeekZ`. No other `esp*.cpp` touches
the pipeline.

---

### 3. Water and sand (espgen)

#### 3.1 `Espgen45_TransSub` (espgen45.cpp:436-731) and `Espgen42_TransSub` (Espgen42.cpp:724-887)

Two copies of the same water renderer (42 = room water, 45 = camera-following outdoor water with the
`Estgen45Set*` overrides; 45 adds the mask stage). Uses the shared counters `pG->gxStage.{tevStage, texMap,
texCoord}` reset to 0 (450-452 / 736-738). Cull NONE, `ZMode(TRUE, LEQUAL, TRUE)` (454-455). Lights:
`LightMgr.setCloth(&model, 5)` + `commonWaterLightSet(pLight, 5, amb.a)` (trans_lit.cpp, part A) issue the channel
control; here only `GXSetChanMatColor(COLOR0A0, white); GXSetChanAmbColor(COLOR0A0, amb)` (483-484). Blend
`(BLEND, SRCALPHA, INVSRCALPHA)` (497).

Textures and texgens (espgen45 line numbers; Espgen42 = same order at 779-865):
- GX_TEXMAP0 = half-size EFB copy into `GetDrawTmpBufAddr(0xE)` (505-515); texgen0 `MTX3x4 POS TEXMTX0`
  projective (`C_MTXLightPerspective(Fovy, 4/3, 0.5, -0.667, 0.5, 0.5) × modelview`, 516-519).
- GX_TEXMAP1 = `p->pTexBuf`, a `Width×Height` **GX_TF_I8** bump/height map from the water simulation (546-547),
  bound only as the indirect map; texgen1 `MTX2x4 TEX0 IDENTITY` (549); `GXSetIndTexOrder(0, TEXCOORD1,
  TEXMAP1)` (550); scale 1,1 (551); `SetIndMtx` → `GXSetIndTexMtx(ITM_1, {{p1·0.001+0.01, 0, 0}, {0,
  p2·0.007+0.07, 0}}, 1)` (389-412); `GXSetTevIndWarp(stage0, 0, signed=1, replace=0, ITM_1)` (553).
- Specular sphere map `EspGetTexObj(p->Spec_Tex)` or the global `Specular` → GX_TEXMAP(n) (577-583); texgen
  `MTX3x4 NRM TEXMTX1` with `trans(0.5,0.5,1)·scale(1,-0.5,0)·inverse-transpose(view)` (585-598).
- Mask (45 only, `flag & 2`, 606-642): GX_TEXMAP(n+1), `tw->_Mtx` reloaded into **TEXMTX1** (633 — this also
  changes the matrix the already-issued specular texgen reads, since GX latches matrices at vertex time; likely a
  latent bug in the original), texgen `MTX2x4 TEX0 TEXMTX1`.
- `GXSetTevColor(GX_TEVREG0, p->Color)` (532), possibly overridden/multiplied by `g_r..g_a` (520-531).

| stage | order | colour → op | alpha | lines (45) |
|---|---|---|---|---|
| 0 | coord0 / map0 / COLOR0A0 | ZERO,C0,TEXC,ZERO → ×4 if `Refrect_type > 0` else ×1, indirect warp | ZERO,ZERO,ZERO,RASA | 533-541, 553 |
| 1 (`Refrect_type > 1`) | coord2 / map2 | ZERO,CPREV,CPREV,ZERO ×4 = 4·CPREV² | RASA | 556-565 |
| 2 (`Refrect_type > 2`) | coord3 / map3 | same | RASA | 566-575 |
| n (specular) | coord(n) / map(n) | ZERO,RASC,TEXA,CPREV = CPREV + lit·spec.a | RASA | 599-603 |
| n+1 (mask, 45) | coord / map | ZERO,ZERO,ZERO,CPREV | ZERO,TEXA,RASA,ZERO | 635-639 |

The `Refrect_type > 1` stages name texcoords/texmaps that have **no texgen and no texture** (the counters are
bumped at 562-564 without `GXSetTexCoordGen`/`GXLoadTexObj`); harmless since TEXC is not an input, but
`GXSetNumTexGens(st->texCoord)` (646) then counts the hole. `GXSetNumTevStages(st->tevStage)` (645). Grid display
list, then teardown (724-728).

Cases: `espgen42_espgen42_transsub` (type ≤1), `espgen45_espgen45_transsub` (type ≤1, no mask),
`espgen45_espgen45_transsub_refract3` (4 stages, 5 referenced maps of which 3 live),
`espgen45_espgen45_transsub_mask` (3 stages, 4 textures). NV2A: `OFFSET_TEXTURE_2D` on the EFB copy with the I8
bump map converted to a signed DSDT texture; sphere-map texgen in the vertex program; ≤4 textures and ≤4
combiner stages per pass, so one pass. `textures_bound` in the JSON counts referenced `GX_TEXMAPn` (holes
included); `nv2a.textures` is the live count.

#### 3.2 `Espgen43_TransSub` (Espgen43.cpp:235-317) — `espgen43_espgen43_transsub`

Sand grid. Cull NONE, `ZMode(TRUE,LEQUAL,TRUE)` (255-256), 5 lights via `commonClothLightSet` (part A),
`GXSetChanMatColor(COLOR0A0, p->Color)` (275), blend SRCALPHA/INVSRCALPHA (285). GX_TEXMAP0 =
`EspGetTexObj(p->TexNo)` (fallback `Specular`), `GXLoadTlut(.., TLUT0)` if present (286-295); texgen0
`GXSetTexCoordGen2(0, MTX2x4, TEX0, IDENTITY, FALSE, PTIDENTITY)` (291). Stage 0 coord0/map0/COLOR0A0: C
ZERO,RASC,TEXC,ZERO ×4; A ZERO,ZERO,ZERO,RASA (296-300). `NumTevStages(1); NumTexGens(1)` (304-305). No
teardown. NV2A: trivial + lit vertex path.

---

### 4. Filters (filter00..0b.cpp)

All filters are OT callbacks queued by `FilterTrans` (filter.cpp:45-61) on the 512×448 frame buffer when
`Render_checkBlurPermission()`; `Filter09` runs instead when the game is paused. Common fixed state unless noted:
ortho `C_MTXOrtho(0, SCR_H, 0, SCR_W, 0, -65536)`, `AC(ALWAYS,0,OR,ALWAYS,0)`, `GXSetColorUpdate(TRUE)`,
`GXSetAlphaUpdate(FALSE)`, cull NONE, `GXSetFog(GX_FOG_NONE,…)` in the Render wrapper (restored by
`LightMgr.setFog()`), texgen0 `GXSetTexCoordGen(0, MTX2x4, TEX0, IDENTITY)`, and the restore `ZMode(TRUE,
LEQUAL, TRUE); GXSetAlphaUpdate(TRUE)` at the end. Retail enablement: 00, 01, 03, 06, 07, 08, 09, 0b always
compiled and reachable; `filter02` (`use_filter2 = 0`, filter02.cpp:52) and `filter04` (`use_filter4 = 0`,
filter04.cpp:64) are compiled but statically disabled; `filter0a` is switched on by `cam_extra.cpp:111` (scope /
binoculars); `filter05` has no GX calls.

The common filter TEV stage, "copy × vertex alpha": stage 0 coord0/map0/COLOR0A0, C ZERO,ZERO,ZERO,TEXC, A
ZERO,ZERO,ZERO,RASA (op ADD, ×1, clamp, PREV). The filters differ in how the raster channel is set up:

| channel setup | used by | effective raster colour |
|---|---|---|
| `ChanCtrl(COLOR0, TRUE, VTX, VTX, 0, CLAMP, SPOT)` + same for `ALPHA0`; `GXSetNumChans(0)` then `(1)` | filter01, 02, 0a | lighting on with no lights → `mat·clamp(amb)` = vtx·vtx per channel; quads use (255,255,255,α) so RGB = 1, **A = α²/255**. `GXSetChanAmbColor(amb)` is dead (amb_src VTX; `amb` is even uninitialised in 01/02/0a) |
| `ChanCtrl(COLOR0/ALPHA0, TRUE, REG, VTX, 0, CLAMP, SPOT)` + `GXSetChanAmbColor(COLOR0A0, amb)` | filter03 (amb 0xFF), 04 (flt04.amb), 07 ({0,0,0,0xFF}), 08 ({0,0,0,0xFF} then `mat` per draw) | vtx·clamp(amb register) |
| `ChanCtrl(COLOR0/ALPHA0, FALSE, VTX, VTX, 0, CLAMP, SPOT)` | filter09, 0b | plain vertex colour |
| `GXSetNumChans(0)`, order `GX_COLOR_NULL` | filter00 | none; alpha from `GX_TEVREG0` |

#### 4.1 `Filter00Render` (filter00.cpp:89-257) — motion blur / spread, 3 cases

GX_TEXMAP0 = `filter00_buff`, the half-size RGBA8 copy taken at the **end** of this function (250-252, copy
filter off), i.e. the previous frame (feedback); re-copied between spread quads (232-234). `ZMode(FALSE, ALWAYS,
FALSE)` (113), cull NONE, ortho 512×448 (114-115), `GXSetNumTevStages(1); GXSetNumChans(0); GXSetNumTexGens(1)`
(132-134), texgen0 identity (135).

| case | condition | colour | alpha | blend | lines |
|---|---|---|---|---|---|
| `filter00_filter00_render_blur` | `blur_type != 2` | ZERO,ZERO,ZERO,TEXC | ZERO,ZERO,ZERO,**A0** (= `blur_rate`, TEVREG0 109) | type 3: `(SUBTRACT, SRCALPHA, INVSRCALPHA)`; else `(BLEND, SRCALPHA, INVSRCALPHA)` (118-130) | 138-142 |
| `filter00_filter00_render_blur_sub` | `blur_type == 2` | C0,ZERO,ZERO,TEXC → **SUB**, scale `blur_scale` (static 0 = ×1) = TEXC − blur_power | A0 | `(BLEND, SRCALPHA, ONE)` | 144-151 |
| `filter00_filter00_render_spread` | `is_eff_spread_on` | ZERO,TEXC,C0,ZERO (C0 = eff_blur rgb, 196-200) | A0 = eff_blur_rate | `bl[eff_blur_type]` (97-99): {SRCALPHA,INVSRCALPHA}, {SRCALPHA,ONE}, {ONE,ONE}, {SRCCLR,ONE}, {SRCCLR,ZERO}, {SRCALPHA,INVSRCCLR} | 188-202 |

Order is coord0/map0/**COLOR_NULL** in all three. `blur_type 1` only changes the quad UVs (zoom). NV2A: one
combiner stage each; `GX_BM_SUBTRACT` = reverse-subtract blend equation.

#### 4.2 `Filter00RenderContrast` (filter00.cpp:308-350) — `filter00_filter00_render_contrast`

Called at 254 after a fresh copy into `filter00_buff` (GX_TEXMAP0, 317-318). `GXSetTevColor(TEVREG0, {bias×3,
pow})` (321-324). `ZMode(FALSE,ALWAYS,FALSE)` (328), blend `(BLEND, SRCALPHA, ONE)` (334), `NumTevStages(1);
NumChans(1); NumTexGens(1)` (335-337; the channel is unused — order is COLOR_NULL, 339). Stage 0: C
ZERO,C0,ONE,TEXC → **SUB**, scale ×1/×2/×4 by `g_cont_level` 1/2/3 (340-346) = (TEXC − bias)·scale; A
ZERO,ZERO,ZERO,A0 (348-349). NV2A: one stage with output scale.

#### 4.3 `Filter01Render` (filter01.cpp:98-305) — depth of field, `filter01_filter01_render`

`filter01_buff = GetDrawTmpBufAddr(6)`; copy `Filter01CopyEFB` (82-95: `(0,0,SCR_W,SCR_H)` → `(SCR_W/2,
SCR_H/2, RGBA8, TRUE)`) → GX_TEXMAP0 (123-124, 142). Blend SRCALPHA/INVSRCALPHA (133), `NumTevStages(1)`,
channels VTX/VTX lit (135-141), stage 0 copy × vertex alpha (143-147). `ZMode(TRUE, GEQUAL, FALSE)` for `Mode 0`
(near, 161) or `(TRUE, LEQUAL, FALSE)` for `Mode 1` (far, 165); quads at depth `w->z`. NV2A: trivial, but the
α² raster quirk must be reproduced for identical fades.

#### 4.4 `Filter02DrawBuffer` / `Filter02DrawBuffer2` (filter02.cpp:148-343 / 346-485) — disabled DOF, 2 cases

`filter02_filter02_drawbuffer`: same stage/channel as filter01 (184-188), blend SRCALPHA/INVSRCALPHA (175),
`ZMode(FALSE, GEQUAL, FALSE)` (197); GX_TEXMAP0 = `filter02_buff`/`filter02_buff3` via `Filter02GetEFB(scale,
div, buf, mip)` (63-75: `(0,0,SCR_W/div,SCR_H/div)` → `((SCR_W/2)/scale, (SCR_H/2)/scale, RGBA8, mip)`), many
quads (`Filter02GXDraw` 124-146).

`filter02_filter02_drawbuffer2`: stage 0 as above (448-452), GX_TEXMAP0 = blurred `filter02_buff` (420-422); then
an EFB **Z** copy: `GXSetTexCopyDst(SCR_W/2, SCR_H/2, 0x11 = GX_TF_Z8, TRUE)` → `filter02_buff2` (401-405), read
back as **GX_TF_C8** (`GXInitTexObjCI(.., 9, ..)`, 441) through TLUT0 = `CLUTdata` (GX_TL_IA8, 256 entries: 0
below `f02_start_no`, then `+f02_add_num` per step, 423-443) → GX_TEXMAP1 (444); `NumTexGens(2)` with texgen1
identity (445-447); stage 1 coord1/map1/COLOR0A0: C ZERO,CPREV,ZERO,CPREV (= CPREV), A ZERO,ZERO,ZERO,TEXA
(455-460, `f02_set_alpha` static 1); the `f02_set_alpha2` debug variant (static 0, 463-468) shows the mask (C
…,TEXC). `NumTevStages(2)`. NV2A: needs a depth copy quantised to 8 bits and remapped by a LUT — a dependent
read or a conversion pass; low priority since the filter is off.

#### 4.5 `Filter03DrawBuffer` (filter03.cpp:163-293) — alpha glow, 2 cases

GX_TEXMAP0 = `filter03_buff` via `Filter03GetEFB(div, div2)` (102-115): `GXSetTexCopyDst((SCR_W/2)/div2,
(SCR_H/2)/div2, 0x27 = GX_CTF_A8, TRUE)` — the **alpha plane only** — read back as GX_TF_I8 (`Filter03GXDraw`
fmt 1, 143), so TEXC = TEXA = frame alpha. Channels REG/VTX lit with amb 0xFF (195-200) → vertex colour. Stage 0
(201-205): C ZERO,RASC,TEXC,ZERO **×2**; A ZERO,RASA,TEXA,ZERO **×2**. `ZMode(FALSE, GEQUAL, FALSE)` (214).

- `filter03_filter03_drawbuffer_feedback`: `GXSetColorUpdate(FALSE); GXSetAlphaUpdate(TRUE)` (182-183), blend
  `(BLEND, ONE, ZERO)` (190) — the blurred alpha is written back into the EFB alpha plane only, at 1/2, 1/4 and
  `flt03.loop × n` offset quads (223-283, vertex alpha 0xFF/0x80).
- `filter03_filter03_drawbuffer_final`: blend `(BLEND, ONE, ONE)` (284), `ColorUpdate(TRUE); AlphaUpdate(FALSE)`
  (286-287), one quad in `flt03.rgb` (289).

NV2A: colour-mask alpha-only writes (`NV097_SET_COLOR_MASK`), a copy of the alpha plane to an A8/L8 texture, one
combiner stage with ×2.

#### 4.6 `Filter04DrawBuffer` (filter04.cpp:156-295) — radial glow (disabled), `filter04_filter04_drawbuffer`

Same stage as filter03 (195-199), amb = `flt04.amb` (192-193), copies `GX_CTF_A8` into `filter04_buff` /
`filter04_buff2` (75-88). Blend: `(ONE, ZERO)` for the downsample (185), `(SRCALPHA, INVSRCALPHA)` for the
feedback quads (253, 280; vertex alpha `flt04.alpha`), `(ONE, ONE)` for the final tinted quad (287). Colour/alpha
masks as filter03 (177-178, 289-290). `ZMode(FALSE,GEQUAL,FALSE)` (208). One case, phases in notes.

#### 4.7 `Filter06Render` (filter06.cpp:259-292) — particles, `filter06_filter06_render`

Lines in vertex colour: blend SRCALPHA/INVSRCALPHA (265), cull NONE, `ZMode(TRUE,LEQUAL,FALSE)` (269-270),
`NumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX, 0, NONE, NONE)` (271-272), `NumTexGens(0); NumTevStages(1);
TevOrder(0, NULL, NULL, COLOR0A0)` (273-275), stage 0 C ZERO,ONE,RASC,ZERO (= RASC), A ZERO,ZERO,ZERO,RASA
(276-279) — hand-written PASSCLR. `GXEnableTexOffsets(0, FALSE, TRUE)` around the lines (283, 291).

#### 4.8 `Filter07DrawBuffer` (filter07.cpp:141-270) — thermal vision, `filter07_filter07_drawbuffer`

Copy of the **green channel**: `GXSetTexCopyDst(.., 0x29 = GX_CTF_G8, TRUE)` → `filter07_buff` (77-90). Blur
passes read it as GX_TF_I8 (`Filter07GXDraw` 119), the final pass as **GX_TF_C8 with `ThermoTlut`** (GX_TLUT0,
116-117; skipped while `Joy[0].on & 0x400`, a debug key). Channels REG/VTX lit with amb {0,0,0,0xFF} (176-181;
RGB raster = 0, unused). Stage 0 copy × vertex alpha (182-186). Blend SRCALPHA/INVSRCALPHA (171). **`ZMode(FALSE,
ALWAYS, TRUE)`** (195): Z written unconditionally at `z = 65530`. NV2A: palette = C8 texture expanded on upload
(the TLUT is a texture-format feature, not TEV).

#### 4.9 `Filter08DrawBuffer` (filter08.cpp:235-320) — aiming zoom blur, 2 cases

Channels REG/VTX lit, amb {0,0,0,0xFF} (269-272), then `Filter08GXDraw` sets `GXSetChanMatColor` and
`GXSetChanAmbColor` to the same `mat` (175-176: white, or `sr/sg/sb[ptn]` in mode 1) → raster = mat². `ZMode(FALSE,
ALWAYS, TRUE)` (286). 6-vertex fan scaled from the weapon marker with vertex alphas `alpha/alpha2/alpha3 ×
filter08_ratio`.

- `filter08_filter08_drawbuffer_zoom`: stage 0 copy × vertex alpha (274-278); GX_TEXMAP0 = `filter08_buff` RGBA8
  (`Filter08GetEFB(.., mode 0)`, 92-108); blend SRCALPHA/INVSRCALPHA (290, 294, 309) switching to `(SRCALPHA, ONE)`
  for the last `flt08_blend_add` feedback passes (296, 303); `GXSetDither` toggled (291, 299).
- `filter08_filter08_drawbuffer_mono`: `GXSetTevColorIn(0, ZERO, RASC, TEXC, ZERO); GXSetTevColorOp(0, ADD, ZERO,
  SCALE_2, TRUE, PREV)` (312-313) = 2·tint·luminance; GX_TEXMAP0 = **GX_TF_I8** copy (`Filter08GetEFB(1,1,1,
  mode 1)`: `GXSetTexCopyDst(.., 1 = GX_TF_I8, TRUE)`, 99-100); blend `(SRCALPHA, ONE)` (315).

#### 4.10 `Filter09DrawBuffer` (filter09.cpp:119-176) / `Filter0bDrawBuffer` (filter0b.cpp:134-181)

Identical programs: channels `ChanCtrl(COLOR0/ALPHA0, FALSE, VTX, VTX, 0, CLAMP, SPOT)` (144-147 / 157-160) →
vertex colour; stage 0 copy × vertex alpha (148-152 / 161-165); blend SRCALPHA/INVSRCALPHA; `ZMode(FALSE, GEQUAL,
FALSE)` (161 / 174); GX_TEXMAP0 = `filter09_buff` / `filter0b_buff` RGBA8 half-size copies (`Filter09GetEFB`
54-67, `Filter0bGetEFB` 84-97 via `Filter0bCapture`). Scissor `(0, 56, SCR_W, SCR_H-111)` in the Render wrappers
(88 / 104). filter09 re-copies after the spreading quad (`g_bSpred`, 168-170). Cases
`filter09_filter09_drawbuffer`, `filter0b_filter0b_drawbuffer`.

#### 4.11 `Filter0aGXDraw` (filter0a.cpp:140-190) + `filter0a_mask_tex` (34-75) — scope vignette, 2 cases

Unusually the TEV/channel state is set inside the per-quad draw: blend SRCALPHA/INVSRCALPHA (152),
`NumTevStages(1)`, channels VTX/VTX lit (154-160), stage 0 copy × vertex alpha (161-165); GX_TEXMAP0 =
`filter0a_buff` (`Filter0aGetEFB` 109-122, RGBA8). `Filter0aDrawBuffer` (193-294) supplies AC ALWAYS, colour mask,
cull, ortho, `ZMode(FALSE, GEQUAL, FALSE)` (220) and the quads. Case `filter0a_filter0a_gxdraw` (`mask == 0`).

`filter0a_filter0a_gxdraw_mask` (`mask != 0 && filter0a_mask_flag`, last quad at 291): GX_TEXMAP1 = ID-system
texture `filter0a_mask_id` (`IdGetTexWk`, GX_TLUT1 for CI, 47-63), identity TEXMTX1, texgen1 `MTX2x4 TEX0
TEXMTX1` (65-66), `NumTevStages(2); NumTexGens(2)` (67-68); stage 1 coord1/map1/COLOR0A0: C ZERO,ZERO,ZERO,CPREV;
**A TEXA,ZERO,ZERO,RASA = mask.a + vertex alpha** (an add, unlike the multiply used by the esp/id masks) (69-73).

---

### 5. HUD / ID system (id_sys.cpp, id_tex.cpp)

Dispatcher `IdGeneralTrans` (1059-1089, see 0.3). All three draws share: cull NONE, `IdTexSet` (GX_TEXMAP0 = id
texture, `ZMode(FALSE, LEQUAL, FALSE)`, MODULATE, texgen0 `MTX2x4 TEX0 TEXMTX0` identity), `IdChannelSet`
(`ChanCtrl(COLOR0A0, FALSE, REG, REG, …)`, mat = `u->col`), `AC(GREATER,1,OR,GREATER,1)`, blend from
`blend[u->blend_type]` = {SRCALPHA,INVSRCALPHA}, {SRCALPHA,ONE}, {ONE,ONE}, {SRCCLR,ONE}, {SRCCLR,ZERO} (all
GX_BM_BLEND / LO_CLEAR, 1107-1109), fog NONE, quad `u->ver` with matrix index 0 + normal.

#### 5.1 `IdCommonTrans` (1105-1177) — `id_sys_idcommontrans`, `id_sys_idcommontrans_mask`

Stage 0 MODULATE (coord0/map0/COLOR0A0). Mask (`tex_flag & 1`, 1125-1152): GX_TEXMAP1 = `u->maskId`/`maskNo`
(GX_TLUT1 for CI), identity TEXMTX1, texgen1 `MTX2x4 TEX0 TEXMTX1` (1143), `NumTevStages(2); NumTexGens(2)`
(1144-1145, not restored here — the next `IdTexSet` resets), stage 1 mask stage (1146-1150).
`GXSetAlphaUpdate(FALSE)` after the quad (1174).

#### 5.2 `IdNegativeTrans` (1179-1277) — `id_sys_idnegativetrans`

Same 3-stage program as `EspCommonTransNega` (1.3): copy of the whole screen `(0,0,W,H)` → `(W/2, H/2, RGBA8,
TRUE)` into `IdGetBufferAddr(1)` (1205-1207) → GX_TEXMAP1 (1211-1212); texgen0 always projective `MTX3x4 POS
TEXMTX0` with `C_MTXLightPerspective(Fovy, 4/3, 0.5, -0.5, 0.5, 0.5) × m_scrn_mat × u->mat` (1216-1220); stage 0
scale ×1/×2/×4 by `pow` (1223-1232; `pow = u->pow <= 1 ? u->pow : 2`); stage 1 (1236-1240), texgen1 identity
(1241-1242), stage 2 (1243-1247), `NumTevStages(3)` (1248). Teardown 1269-1273, `GXSetAlphaUpdate(FALSE)` (1275).

#### 5.3 `IdShimmerTrans` (1280-1447) — `id_sys_idshimmertrans`, `id_sys_idshimmertrans_mask`

Same program as `EspCommonTransShimmer` (1.2). GX_TEXMAP0 = id texture (indirect map); GX_TEXMAP1 = whole-screen
copy into `IdGetBufferAddr(2)` (1314-1321, `GXInitTexObjLOD` linear); texgen0 projective (1326-1330), texgen1
identity (1332), `GXSetIndTexOrder(0, TEXCOORD1, TEXMAP0)` (1333), scale 1,1 (1334), `GXSetIndTexMtx(ITM_1,
diag(k), 1)` (1355) with `k = col.a/255 · 0.04 · 1000/dot · (1+u_pow/32)` and `dot = max(1500, …)` — the `…` is
read from the **uninitialised** `indMtx` stack array (1340-1352, decomp-faithful; effectively 1500 unless garbage
is larger). `GXSetTevIndWarp(0, 0, signed, replace, ITM_1)` (1376): `Refract_type` 2 → (0,0), 3 → (1,0), other →
(0,1) + error log. Stage 0 (1378-1382) as in 1.2. Mask (`tex_flag & 1`, 1383-1417): GX_TEXMAP2 (GX_TLUT1 for CI),
identity TEXMTX1, texgen2 `MTX2x4 TEX0 TEXMTX1` (1409), stage 1 coord2/map2 mask stage (1410-1415).
`NumTevStages(nStage); NumTexGens(nGen)` (1418-1419); teardown 1440-1444.

---

### 6. Text, fade, letterbox

#### 6.1 `RomFont::RomFont` (mes.cpp:79-104) + `RomFont::setup` (106-118) — `mes_romfont_romfont`

IPL ROM font used by the disc-error screen (dvd.cpp:1719, retail). Ortho 544×408, `ZMode(TRUE, ALWAYS, TRUE)`
(89), `GXSetNumChans(0)` (90), `NumTevStages(1)` (91), `GXSetTevOp(0, GX_REPLACE)` (92: C …,TEXC; A …,TEXA),
`TevOrder(0, TEXCOORD0, TEXMAP0, COLOR_NULL)` (93), blend `(BLEND, ONE, ONE)` (94). `setup`: GX_TEXMAP0 = font
sheet (111-113), `GX_TEXMTX0 = scale(1/sheetW, 1/sheetH)` (114-115), `GXSetTexCoordGen2(0, MTX2x4, TEX0,
TEXMTX0, FALSE, PTIDENTITY)` (116-117).

#### 6.2 `setAttribute(FONT_TEX*)` (mes.cpp:982-1008) — `mes_setattribute`

Game text. Cull NONE (984), `ZMode(FALSE, LEQUAL, TRUE)` (985), `NumTevStages(1); NumChans(1)` (986-987),
`GXSetTevOp(0, GX_MODULATE)` (988), `TevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0)` (989), `ChanCtrl(COLOR0A0, FALSE,
VTX, VTX, 0, NONE, NONE)` (990) → vertex colour; GX_TEXMAP0 = `t->tex` (`GXLoadTlut(&t->tlut, TLUT0)` for CI,
991-994); `GX_TEXMTX0 = t->mtx`, `GXSetTexCoordGen2(0, MTX2x4, TEX0, TEXMTX0, FALSE, PTIDENTITY)` (995-997);
blend SRCALPHA/INVSRCALPHA (998). Fog NONE in `draw()` (1045).

#### 6.3 `EprintfDrawing` (eprintf.cpp:242-312) — `eprintf_eprintfdrawing`

Debug on-screen text; compiled but returns at 265 when `pG->debug_mode == 0`. Ortho 512×448, **`GXSetCullMode(
GX_CULL_BACK)`** (261, the only back-face cull in part B), `ZMode(TRUE, LEQUAL, TRUE)` (262), `NumTevStages(1);
NumChans(1)` (263-264), MODULATE (268), `TevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0)` (269), `ChanCtrl(COLOR0A0,
FALSE, VTX, VTX, 0, NONE, NONE)` (270), `NumTexGens(1)` (271), GX_TEXMAP0 = `fontTexObj` (272), `GX_TEXMTX0 =
fontTMtx` (273), texgen0 `MTX2x4 TEX0 TEXMTX0` (274), blend SRCALPHA/INVSRCALPHA (275).

#### 6.4 `fadeDraw` (fade.cpp:111-140) — `fade_fadedraw`; `Draw_cinesco` (cinesco.cpp:78-128) — `cinesco_draw_cinesco`

Both: blend SRCALPHA/INVSRCALPHA, `GXSetColorUpdate(TRUE)`, ortho, cull NONE, `NumTexGens(0); NumTevStages(1);
GXSetTevOp(0, GX_PASSCLR); TevOrder(0, NULL, NULL, COLOR0A0); NumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX,
GX_LIGHT0, NONE, NONE)` (fade 124-131, cinesco 97-104; the light mask is meaningless with lighting off). fade:
`ZMode(TRUE, ALWAYS, FALSE)` (125), quad between y 56 and `xfbHeight-56` at `pF->z`. cinesco: `ZMode(FALSE,
ALWAYS, TRUE)` (98, writes Z = 10 without testing), fog NONE (96), two bars rows 0-56 and 393-449.

---

### 7. Totals (part B)

- **Cases:** 49 (JSON), from 27 pipeline-setup functions (+ 6 helpers inlined). Compiled-but-statically-disabled
  in retail: `filter02_*` (2), `filter04_*` (1); runtime-disabled in retail: `eprintf_eprintfdrawing`
  (`debug_mode`).
- **Stage count distribution:** 1 stage: 33 cases; 2 stages: 11; 3 stages: 4 (the three "nega" draws and the
  water+mask); 4 stages: 1 (`espgen45 … refract3`). **Max 4 TEV stages.**
- **Textures referenced per pass:** 0: 6 cases; 1: 23; 2: 12; 3: 6; 4: 1; 5: 1 (the water refract3 case, where 2
  of the 5 `GX_TEXMAPn` are unloaded holes — **max 4 live textures**, water+mask). **Max texgens: 5** (same case,
  with holes). Nothing in part B exceeds NV2A's 8 combiner stages or 4 textures, so **every part-B case is one
  pass**.
- **EFB copies as texture inputs:** 32 of 49 cases. Formats copied: `GX_TF_RGBA8` (half size, everywhere),
  `GX_TF_I8` (filter08 mono), `GX_CTF_A8` (filter03/04 alpha glow), `GX_CTF_G8` (filter07 thermal), `GX_TF_Z8`
  (filter02, disabled) — the last read back as `GX_TF_C8` through a CLUT, and the `GX_CTF_*`/`I8` ones read back
  as `GX_TF_I8` or `GX_TF_C8` + `ThermoTlut`.
- **Indirect texturing:** 11 cases use exactly one indirect stage, always `GXSetTevIndWarp(stage0,
  GX_INDTEXSTAGE0, signed, replace, GX_ITM_1)` with `GX_ITF_8`, scale `GX_ITS_1`, no bias alpha, no `add_prev`:
  heat shimmer (esp_sub ×3, esp08 ×2, id_sys ×2) and the water bump (espgen42, espgen45 ×3). Replace mode
  (`GX_ITW_0`) only in the shimmer "type 3". `Esp18_Trans` configures an indirect stage it never binds (dead).
  Never used: `GXSetTevIndBumpXYZ/ST`, `GXSetTevIndTile`, `GXSetTevIndRepeat`, more than one indirect stage,
  `GX_ITM_0/2`, `GX_ITM_S*/T*`, indirect scale ≠ 1, `GXSetIndTexCoordScale` ≠ 1.
- **`GXSetTevOp` shortcuts:** 15 shortcut stages: `GX_MODULATE` 9 (all sprites, HUD, text), `GX_PASSCLR` 5
  (esp03/09, esp19, fade, cinesco; filter06 writes the same by hand), `GX_REPLACE` 1 (ROM font).
  `GX_DECAL`/`GX_BLEND` never. Of 71 stage programs in the JSON, 56 are hand-built; the dominant hand-built
  patterns are "TEXC passthrough + RASA" (filters, 12×), the mask stage `CPREV / TEXA·RASA` (8×), `TEXC·RASC`
  (shimmer/esp18, 8×) and `TEXC·C0` / `TEXC·RASC ×4` (water/sand).
- **TEV features used:** inputs CPREV, APREV, C0/A0 (TEVREG0 only), TEXC, TEXA, RASC, RASA, ONE, ZERO; ops ADD and
  SUB (filter00 only); bias always ZERO; scales ×1, ×2, ×4; clamp always TRUE; output register always
  `GX_TEVPREV`. **Never used in part B:** `GX_TEVREG1/2` as output or input, `GX_CC_C1/C2/A1/A2`, `GX_CC_HALF`,
  `GX_CC_KONST`/`GX_CA_KONST`, `GXSetTevKColor*`, `GXSetTevSwapMode*`, `GXSetTevColorS10`, compare ops
  (`GX_TEV_COMP_*`), `GX_TB_ADDHALF/SUBHALF`, `GX_CS_DIVIDE_2`, `GX_TEXRRR/GGG/BBB`, `GX_ALPHA_BUMP*`, more than
  one colour channel (`GX_COLOR1A1`), `GXSetZCompLoc`, `GXSetTexCoordGen` with `GX_TG_BUMP*`/`GX_TG_SRTG`/
  `GX_TG_COLOR*`, texgen `normalize = TRUE`, post-transform matrices other than `GX_PTIDENTITY`.
- **Texgens:** `MTX2x4 TEX0` with `GX_IDENTITY`/`GX_TEXMTX0`/`GX_TEXMTX1`; `MTX2x4 TEX1` (esp08 Mask_type 1);
  `MTX3x4 POS` projective (screen copies, 15 cases counting the screen/world runtime split); `MTX3x4 NRM` sphere map (water). Max texture matrices live:
  TEXMTX0 + TEXMTX1.
- **Channels:** `GX_COLOR0A0` only; lit channels come from `commonEspLightSet`/`commonWaterLightSet`/
  `commonClothLightSet` (part A). The filters' "lighting on, zero lights, VTX/VTX" pattern yields vtx², which the
  port must reproduce (or decide to drop) — see §4.
- **Blend modes:** `GX_BM_BLEND` with SRCALPHA/INVSRCALPHA (default), SRCALPHA/ONE, ONE/ONE, ONE/ZERO, SRCCLR/ONE,
  SRCCLR/ZERO, SRCALPHA/INVSRCCLR; `GX_BM_SUBTRACT` SRCALPHA/INVSRCALPHA (filter00 type 3). `GX_BM_LOGIC` and
  `GXSetDstAlpha` only via the esp runtime fields (`GXSetDstAlpha(TRUE, 0)` for `Tool_flg & 0x200000`).
- **Z modes:** `(TRUE, LEQUAL, FALSE|TRUE)`, `(FALSE, LEQUAL, FALSE|TRUE)`, `(TRUE|FALSE, GEQUAL, FALSE)`,
  `(FALSE|TRUE, ALWAYS, FALSE|TRUE)`. **Alpha compare:** `GREATER 1 OR GREATER 1` (sprites/HUD), `GREATER 0x80`
  (esp 0x100000), `ALWAYS 0` (filters). **Cull:** NONE everywhere except `GX_CULL_BACK` in `EprintfDrawing`.
  **Fog:** every effect/filter that samples a screen copy forces `GX_FOG_NONE` and restores via
  `LightMgr.setFog()`.
- **Colour/alpha write masks:** filter03/04 write alpha only (`GXSetColorUpdate(FALSE)`), most filters write
  colour only; `GXSetDither` toggled in filter08.
- **NV2A summary:** 11 cases need a texture shader (`OFFSET_TEXTURE_2D` dependent read) for the indirect warp;
  filter02's Z8-through-CLUT and filter07's C8 palette are texture-format conversions, not combiner work. All 49
  cases fit one pass with ≤4 combiner stages and ≤4 textures.

### Appendix A — debug-only unit `dbmodule.cpp` (not compiled into the port)

All are 1 TEV stage, 0 texgens, `GXSetTevOrder(0, NULL, NULL, COLOR0A0)`, `GXSetTevOp(0, GX_PASSCLR)`, one
colour channel; listed for completeness only.

| function | lines | channel | blend / Z |
|---|---|---|---|
| `Render_tile` | 67-112 | `ChanCtrl(COLOR0A0, FALSE, REG, VTX, …)` | `(NONE, ONE, ZERO)`, `Z(TRUE,LEQUAL,TRUE)` |
| `Draw_line` | 127-170 | REG/VTX | `(BLEND, SRCALPHA, INVSRCALPHA)`, `Z(TRUE,LEQUAL,TRUE)` |
| `Draw_quad` | 171-218 | REG/VTX | `(BLEND, SRCALPHA, INVSRCALPHA)`, `Z(TRUE,LEQUAL,TRUE)` |
| `Draw_line3d_local` | 226-265 | REG/VTX | `(BLEND, ONE, ZERO)` or `(BLEND, ONE, ONE)` by `blend`; Z compare by alpha byte |
| `Draw_line3d_init` | 266-295 | REG/VTX | `(NONE, ONE, ZERO)`, `Z(FALSE,LEQUAL,TRUE)` |
| `Draw_poly_local` | 308-351 | `ChanCtrl(COLOR0A0, FALSE, VTX, VTX, …)` | `(BLEND, SRCALPHA, INVSRCALPHA)`, Z write by `zmode` |
| `Draw_sphere` | 352-391 | `ChanCtrl(COLOR0, FALSE, REG, REG, …)` | `(NONE, SRCALPHA, INVSRCALPHA)`, Z by `zmode`/`zcheck` |
| `Draw_cylinder` / `Draw_cylinderMtx` | 392-451 | COLOR0 REG/REG | `(NONE, …)`, `Z(TRUE,LEQUAL,TRUE)` |
| `Draw_corn` | 471-504 | COLOR0 REG/REG | `(NONE, …)`, `Z(TRUE,LEQUAL,TRUE)` |

Also runtime-debug-only though compiled: `EprintfDrawing` (§6.3), `Filter07` palette bypass (`Joy & 0x400`),
`filter02 f02_set_alpha2`, `Filter04` joystick preview (`Joy & 0x200`).

### Appendix B — open points / uncertainties

1. Channel control for the lit variants (`esp_sub_esp_common_trans_lit`, both waters, sand) is issued in
   `trans_lit.cpp` (`commonEspLightSet`, `commonWaterLightSet`, `commonClothLightSet`), outside part B; the JSON
   marks `num_chans` and the channel fields with `?`. Part A should fill them in at merge time.
2. `Esp18_Trans`: I could not find any `GXSetTevIndirect`/`GXSetTevIndWarp` for the configured indirect stage;
   the catalogue records stage 0 as direct on the assumption that the previous draw left `GXSetTevDirect(0)` (all
   part-B indirect users and, per the comments, `trans.cpp` reset to direct). If part A finds a path that leaves
   stage 0 indirect, this case changes.
3. `IdShimmerTrans` reads `dot` from the uninitialised `indMtx` array (decomp-faithful); the JSON describes the
   effective matrix as `k/max(1500, garbage)`.
4. The water mask (`espgen45 … _mask`) reloads `GX_TEXMTX1` after the specular texgen was issued; I recorded the
   effect (both texgens use the mask matrix) as "likely a latent bug in the original" — unverified on hardware.
5. `textures_bound` for the water cases counts `GX_TEXMAPn` holes referenced by stages that have no texture
   loaded; `nv2a.textures` gives the live count.
6. The filter "lit with zero lights" channels (vtx² alpha) are recorded as GX semantics; whether the retail disc
   relies on the squared alpha visually has not been checked.

