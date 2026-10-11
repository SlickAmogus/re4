# TEV / GX usage catalogue — part B (CT-05)

Part B of the CT-05 catalogue: effects (`esp_sub.cpp`, `esp*.cpp`, `espgen45.cpp`, `Espgen42/43.cpp`), the
post-process filters (`filter00..0b.cpp`), the HUD/ID system (`id_sys.cpp`, `id_tex.cpp`), text (`mes.cpp`,
`eprintf.cpp`), `fade.cpp`, `cinesco.cpp`, `eff_sys.cpp`, and a short appendix for the debug unit `dbmodule.cpp`.
Machine-readable twin: `xbox/port/gx/tests/tev_cases_partB.json` (49 cases, validates against
`tev_cases.schema.json`). Part A (trans.cpp, shadow.cpp, …) is a separate document; the two are merged later.

All line numbers refer to `src/game/<file>` on branch `xbox` at the time of writing. The decomp passes GX enums
as raw integers; they are decoded with `include/dolphin/gx/GXEnum.h` (e.g. `GXSetTevOrder(0, 0xFF, 0xFF, 4)` =
`GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0`; `0x1E` = `GX_TEXMTX0`, `0x21` = `GX_TEXMTX1`, `0x3C` =
`GX_IDENTITY`, `0x7D` = `GX_PTIDENTITY`; copy formats `6` = `GX_TF_RGBA8`, `1` = `GX_TF_I8`, `9` = `GX_TF_C8`,
`0x11` = `GX_TF_Z8`, `0x27` = `GX_CTF_A8`, `0x29` = `GX_CTF_G8`).

## 0. Conventions

### 0.1 `GXSetTevOp` expansion (src/lib/GXTev.c)

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

### 0.2 Other shortcuts

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

### 0.3 Helpers inlined into the cases (no case of their own)

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

## 1. esp_sub.cpp

### 1.1 `EspCommonTrans` (74-345) — generic sprite, 4 cases

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

### 1.2 `EspCommonTransShimmer` (347-640) — heat shimmer, 3 cases

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

### 1.3 `EspCommonTransNega` (643-810) — "negative" sprite, 1 case `esp_sub_esp_common_trans_nega`

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

## 2. esp*.cpp

### 2.1 `Esp03_Trans` (esp03.cpp:78-115) — `esp03_esp03_trans`

Untextured polygon debris. `ZMode(TRUE,LEQUAL,FALSE)` (86), cull NONE (87), `GXSetNumTexGens(0)` (88),
`GXSetNumTevStages(1)` (89), `GXSetTevOrder(0, NULL, NULL, COLOR0A0)` (90), `ChannelSet()` (108), then
`GXSetTevOp(0, GX_PASSCLR)` (109) which overwrites ChannelSet's MODULATE/scale (0x80/0x20000 have no effect).
Stage 0: C ZERO,ZERO,ZERO,RASC; A ZERO,ZERO,ZERO,RASA. Channel `COLOR0A0 FALSE REG REG` (mat = effect colour);
the `GX_VA_CLR0` attribute sent at 113-116 is ignored unless `0x40` (lit). AC GREATER 1 (110), blend runtime
(111). 0 textures. NV2A: constant colour.

### 2.2 `Esp08_Trans` (esp08.cpp:413-533) — tiled sprite, `esp08_esp08_trans_mask`

Without a mask this is exactly `esp_sub_esp_common_trans_base`/`_scale4`/`_lit` (`EspTexSet` 499, `ChannelSet()`
500, blend 501, `CommonStateSet()` 502) drawn as the `ESP08_TILES` grid (102-…). With `Tool_flg & 0x4000` the
`ESP08_MASK_SET(1, 1, …)` macro (297-346) adds: GX_TEXMAP1 = mask (GX_TLUT1 for CI, 311-319),
`GXLoadTexMtxImm(tw->_Mtx, TEXMTX1)` (320), texgen1 `GXSetTexCoordGen2(1, MTX2x4, Mask_type==1 ? TEX1 : TEX0,
TEXMTX1, FALSE, PTIDENTITY)` (321-325) — Mask_type 1 uses a second UV attribute (`GX_VA_TEX1`, 350-353) spanning
the whole tiled sprite —, `GXSetNumTevStages(2); GXSetNumTexGens(2)` (326-327), stage 1 coord1/map1/COLOR0A0 mask
stage, alpha ×1 only (328-332). Restored 525-526, `GXSetAlphaUpdate(FALSE)` 529.

### 2.3 `Esp08_TransShimmer` (esp08.cpp:535-768) — `esp08_esp08_trans_shimmer`, `_shimmer_mask`

Same program as 1.2 with these differences: `ChannelSet()`+`CommonStateSet()` first (630-632, overwritten),
`buf = GetDrawTmpBufAddr(1)` always, copy always taken (667-669), explicit `GXSetNumTevStages(1)` (695),
`dot = 2500` fixed (703), projective matrix offset `-0.667` or `-0.5` by `SYS_SCISSOR_ON` (684-690),
`m_Shimmer_type` 1/2/3 → `GXSetTevIndWarp(0,0, 0/1/0, 0/0/1, ITM_1)` (723-745), mask via `ESP08_MASK_SET(2, 2)`
(752: GX_TEXMAP2, texgen2 `TEX0|TEX1 TEXMTX1 PTIDENTITY`, alpha ×1). Tiled (`ESP08_TILES` 753). Teardown 761-765.

### 2.4 `Esp09_Trans_Setup` (esp09.cpp:181-220) + `EspChannelSet09` (168-178) — `esp09_esp09_trans_setup`

Trails. `ZMode(TRUE,LEQUAL,FALSE)` (185), cull NONE, `NumTexGens(0)`, `NumTevStages(1)`, `TevOrder(0, NULL,
NULL, COLOR0A0)` (187-189), `EspChannelSet09` (204: MODULATE, optional ×4, `ChanCtrl(COLOR0A0, FALSE, REG, VTX,
0, NONE, NONE)`), `GXSetTevOp(0, GX_PASSCLR)` (205) — again overriding the MODULATE/scale, so `0x80` is dead.
AC GREATER 1 (206), blend runtime (207). When `m_Tex_id != 0xFF` `EspTexSet` binds GX_TEXMAP0 and a texcoord
attribute is sent (214-216) but no texgen/stage reads it: the texture is unused (decomp-faithful). Vertex colour.

### 2.5 `Esp0f_Trans` (esp0f.cpp:47-215) — `esp0f_esp0f_trans`

Identical TEV program to 1.3 (`EspCommonTransNega`), stage 0 scale by `w->Pow` 0/1/2 (168-177). Differences:
copy of the whole screen `GXSetTexCopySrc(0,0,W,H); GXSetTexCopyDst(W/2, H/2, RGBA8, TRUE)` into
`GetDrawTmpBufAddr(3)` (143-150, GX_TEXMAP1), LightPerspective offset `-0.5` (160), `GXSetAlphaUpdate(FALSE)` at
the end (213). GX_TEXMAP0 = sprite texture (110). `ChannelSet()`/`CommonStateSet()` 111-113. Lines: stages
166-193, `NumTexGens(2)` 186, `NumTevStages(3)` 194, teardown 208-212.

### 2.6 `Esp18_Trans` (esp18.cpp:74-395) — radial blur layers, `esp18_esp18_trans`

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

### 2.7 `Draw_line3d_local_222` (esp19.cpp:59-110) — `esp19_draw_line3d_local_222`

Laser line. Blend runtime (67), cull NONE (69), `ZMode(FALSE|TRUE, LEQUAL, TRUE)` by alpha byte `0xFE` (70-74),
`GXSetNumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX, 0, NONE, NONE)` (75-76), `NumTexGens(0)`,
`NumTevStages(1)`, `TevOrder(0, NULL, NULL, COLOR0A0)`, `GXSetTevOp(0, GX_PASSCLR)` (77-80). Vertex colour; AC
inherited.

### 2.8 Other esp files

`esp01/02/04/12/16.cpp` call `EspTexSet` + `ChannelSet()` + `CommonStateSet()` and set the blend only (they are
callers of `esp_sub_esp_common_trans_base`). `esp0e/esp45/espgen01` only `GXPeekZ`. No other `esp*.cpp` touches
the pipeline.

---

## 3. Water and sand (espgen)

### 3.1 `Espgen45_TransSub` (espgen45.cpp:436-731) and `Espgen42_TransSub` (Espgen42.cpp:724-887)

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

### 3.2 `Espgen43_TransSub` (Espgen43.cpp:235-317) — `espgen43_espgen43_transsub`

Sand grid. Cull NONE, `ZMode(TRUE,LEQUAL,TRUE)` (255-256), 5 lights via `commonClothLightSet` (part A),
`GXSetChanMatColor(COLOR0A0, p->Color)` (275), blend SRCALPHA/INVSRCALPHA (285). GX_TEXMAP0 =
`EspGetTexObj(p->TexNo)` (fallback `Specular`), `GXLoadTlut(.., TLUT0)` if present (286-295); texgen0
`GXSetTexCoordGen2(0, MTX2x4, TEX0, IDENTITY, FALSE, PTIDENTITY)` (291). Stage 0 coord0/map0/COLOR0A0: C
ZERO,RASC,TEXC,ZERO ×4; A ZERO,ZERO,ZERO,RASA (296-300). `NumTevStages(1); NumTexGens(1)` (304-305). No
teardown. NV2A: trivial + lit vertex path.

---

## 4. Filters (filter00..0b.cpp)

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

### 4.1 `Filter00Render` (filter00.cpp:89-257) — motion blur / spread, 3 cases

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

### 4.2 `Filter00RenderContrast` (filter00.cpp:308-350) — `filter00_filter00_render_contrast`

Called at 254 after a fresh copy into `filter00_buff` (GX_TEXMAP0, 317-318). `GXSetTevColor(TEVREG0, {bias×3,
pow})` (321-324). `ZMode(FALSE,ALWAYS,FALSE)` (328), blend `(BLEND, SRCALPHA, ONE)` (334), `NumTevStages(1);
NumChans(1); NumTexGens(1)` (335-337; the channel is unused — order is COLOR_NULL, 339). Stage 0: C
ZERO,C0,ONE,TEXC → **SUB**, scale ×1/×2/×4 by `g_cont_level` 1/2/3 (340-346) = (TEXC − bias)·scale; A
ZERO,ZERO,ZERO,A0 (348-349). NV2A: one stage with output scale.

### 4.3 `Filter01Render` (filter01.cpp:98-305) — depth of field, `filter01_filter01_render`

`filter01_buff = GetDrawTmpBufAddr(6)`; copy `Filter01CopyEFB` (82-95: `(0,0,SCR_W,SCR_H)` → `(SCR_W/2,
SCR_H/2, RGBA8, TRUE)`) → GX_TEXMAP0 (123-124, 142). Blend SRCALPHA/INVSRCALPHA (133), `NumTevStages(1)`,
channels VTX/VTX lit (135-141), stage 0 copy × vertex alpha (143-147). `ZMode(TRUE, GEQUAL, FALSE)` for `Mode 0`
(near, 161) or `(TRUE, LEQUAL, FALSE)` for `Mode 1` (far, 165); quads at depth `w->z`. NV2A: trivial, but the
α² raster quirk must be reproduced for identical fades.

### 4.4 `Filter02DrawBuffer` / `Filter02DrawBuffer2` (filter02.cpp:148-343 / 346-485) — disabled DOF, 2 cases

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

### 4.5 `Filter03DrawBuffer` (filter03.cpp:163-293) — alpha glow, 2 cases

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

### 4.6 `Filter04DrawBuffer` (filter04.cpp:156-295) — radial glow (disabled), `filter04_filter04_drawbuffer`

Same stage as filter03 (195-199), amb = `flt04.amb` (192-193), copies `GX_CTF_A8` into `filter04_buff` /
`filter04_buff2` (75-88). Blend: `(ONE, ZERO)` for the downsample (185), `(SRCALPHA, INVSRCALPHA)` for the
feedback quads (253, 280; vertex alpha `flt04.alpha`), `(ONE, ONE)` for the final tinted quad (287). Colour/alpha
masks as filter03 (177-178, 289-290). `ZMode(FALSE,GEQUAL,FALSE)` (208). One case, phases in notes.

### 4.7 `Filter06Render` (filter06.cpp:259-292) — particles, `filter06_filter06_render`

Lines in vertex colour: blend SRCALPHA/INVSRCALPHA (265), cull NONE, `ZMode(TRUE,LEQUAL,FALSE)` (269-270),
`NumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX, 0, NONE, NONE)` (271-272), `NumTexGens(0); NumTevStages(1);
TevOrder(0, NULL, NULL, COLOR0A0)` (273-275), stage 0 C ZERO,ONE,RASC,ZERO (= RASC), A ZERO,ZERO,ZERO,RASA
(276-279) — hand-written PASSCLR. `GXEnableTexOffsets(0, FALSE, TRUE)` around the lines (283, 291).

### 4.8 `Filter07DrawBuffer` (filter07.cpp:141-270) — thermal vision, `filter07_filter07_drawbuffer`

Copy of the **green channel**: `GXSetTexCopyDst(.., 0x29 = GX_CTF_G8, TRUE)` → `filter07_buff` (77-90). Blur
passes read it as GX_TF_I8 (`Filter07GXDraw` 119), the final pass as **GX_TF_C8 with `ThermoTlut`** (GX_TLUT0,
116-117; skipped while `Joy[0].on & 0x400`, a debug key). Channels REG/VTX lit with amb {0,0,0,0xFF} (176-181;
RGB raster = 0, unused). Stage 0 copy × vertex alpha (182-186). Blend SRCALPHA/INVSRCALPHA (171). **`ZMode(FALSE,
ALWAYS, TRUE)`** (195): Z written unconditionally at `z = 65530`. NV2A: palette = C8 texture expanded on upload
(the TLUT is a texture-format feature, not TEV).

### 4.9 `Filter08DrawBuffer` (filter08.cpp:235-320) — aiming zoom blur, 2 cases

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

### 4.10 `Filter09DrawBuffer` (filter09.cpp:119-176) / `Filter0bDrawBuffer` (filter0b.cpp:134-181)

Identical programs: channels `ChanCtrl(COLOR0/ALPHA0, FALSE, VTX, VTX, 0, CLAMP, SPOT)` (144-147 / 157-160) →
vertex colour; stage 0 copy × vertex alpha (148-152 / 161-165); blend SRCALPHA/INVSRCALPHA; `ZMode(FALSE, GEQUAL,
FALSE)` (161 / 174); GX_TEXMAP0 = `filter09_buff` / `filter0b_buff` RGBA8 half-size copies (`Filter09GetEFB`
54-67, `Filter0bGetEFB` 84-97 via `Filter0bCapture`). Scissor `(0, 56, SCR_W, SCR_H-111)` in the Render wrappers
(88 / 104). filter09 re-copies after the spreading quad (`g_bSpred`, 168-170). Cases
`filter09_filter09_drawbuffer`, `filter0b_filter0b_drawbuffer`.

### 4.11 `Filter0aGXDraw` (filter0a.cpp:140-190) + `filter0a_mask_tex` (34-75) — scope vignette, 2 cases

Unusually the TEV/channel state is set inside the per-quad draw: blend SRCALPHA/INVSRCALPHA (152),
`NumTevStages(1)`, channels VTX/VTX lit (154-160), stage 0 copy × vertex alpha (161-165); GX_TEXMAP0 =
`filter0a_buff` (`Filter0aGetEFB` 109-122, RGBA8). `Filter0aDrawBuffer` (193-294) supplies AC ALWAYS, colour mask,
cull, ortho, `ZMode(FALSE, GEQUAL, FALSE)` (220) and the quads. Case `filter0a_filter0a_gxdraw` (`mask == 0`).

`filter0a_filter0a_gxdraw_mask` (`mask != 0 && filter0a_mask_flag`, last quad at 291): GX_TEXMAP1 = ID-system
texture `filter0a_mask_id` (`IdGetTexWk`, GX_TLUT1 for CI, 47-63), identity TEXMTX1, texgen1 `MTX2x4 TEX0
TEXMTX1` (65-66), `NumTevStages(2); NumTexGens(2)` (67-68); stage 1 coord1/map1/COLOR0A0: C ZERO,ZERO,ZERO,CPREV;
**A TEXA,ZERO,ZERO,RASA = mask.a + vertex alpha** (an add, unlike the multiply used by the esp/id masks) (69-73).

---

## 5. HUD / ID system (id_sys.cpp, id_tex.cpp)

Dispatcher `IdGeneralTrans` (1059-1089, see 0.3). All three draws share: cull NONE, `IdTexSet` (GX_TEXMAP0 = id
texture, `ZMode(FALSE, LEQUAL, FALSE)`, MODULATE, texgen0 `MTX2x4 TEX0 TEXMTX0` identity), `IdChannelSet`
(`ChanCtrl(COLOR0A0, FALSE, REG, REG, …)`, mat = `u->col`), `AC(GREATER,1,OR,GREATER,1)`, blend from
`blend[u->blend_type]` = {SRCALPHA,INVSRCALPHA}, {SRCALPHA,ONE}, {ONE,ONE}, {SRCCLR,ONE}, {SRCCLR,ZERO} (all
GX_BM_BLEND / LO_CLEAR, 1107-1109), fog NONE, quad `u->ver` with matrix index 0 + normal.

### 5.1 `IdCommonTrans` (1105-1177) — `id_sys_idcommontrans`, `id_sys_idcommontrans_mask`

Stage 0 MODULATE (coord0/map0/COLOR0A0). Mask (`tex_flag & 1`, 1125-1152): GX_TEXMAP1 = `u->maskId`/`maskNo`
(GX_TLUT1 for CI), identity TEXMTX1, texgen1 `MTX2x4 TEX0 TEXMTX1` (1143), `NumTevStages(2); NumTexGens(2)`
(1144-1145, not restored here — the next `IdTexSet` resets), stage 1 mask stage (1146-1150).
`GXSetAlphaUpdate(FALSE)` after the quad (1174).

### 5.2 `IdNegativeTrans` (1179-1277) — `id_sys_idnegativetrans`

Same 3-stage program as `EspCommonTransNega` (1.3): copy of the whole screen `(0,0,W,H)` → `(W/2, H/2, RGBA8,
TRUE)` into `IdGetBufferAddr(1)` (1205-1207) → GX_TEXMAP1 (1211-1212); texgen0 always projective `MTX3x4 POS
TEXMTX0` with `C_MTXLightPerspective(Fovy, 4/3, 0.5, -0.5, 0.5, 0.5) × m_scrn_mat × u->mat` (1216-1220); stage 0
scale ×1/×2/×4 by `pow` (1223-1232; `pow = u->pow <= 1 ? u->pow : 2`); stage 1 (1236-1240), texgen1 identity
(1241-1242), stage 2 (1243-1247), `NumTevStages(3)` (1248). Teardown 1269-1273, `GXSetAlphaUpdate(FALSE)` (1275).

### 5.3 `IdShimmerTrans` (1280-1447) — `id_sys_idshimmertrans`, `id_sys_idshimmertrans_mask`

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

## 6. Text, fade, letterbox

### 6.1 `RomFont::RomFont` (mes.cpp:79-104) + `RomFont::setup` (106-118) — `mes_romfont_romfont`

IPL ROM font used by the disc-error screen (dvd.cpp:1719, retail). Ortho 544×408, `ZMode(TRUE, ALWAYS, TRUE)`
(89), `GXSetNumChans(0)` (90), `NumTevStages(1)` (91), `GXSetTevOp(0, GX_REPLACE)` (92: C …,TEXC; A …,TEXA),
`TevOrder(0, TEXCOORD0, TEXMAP0, COLOR_NULL)` (93), blend `(BLEND, ONE, ONE)` (94). `setup`: GX_TEXMAP0 = font
sheet (111-113), `GX_TEXMTX0 = scale(1/sheetW, 1/sheetH)` (114-115), `GXSetTexCoordGen2(0, MTX2x4, TEX0,
TEXMTX0, FALSE, PTIDENTITY)` (116-117).

### 6.2 `setAttribute(FONT_TEX*)` (mes.cpp:982-1008) — `mes_setattribute`

Game text. Cull NONE (984), `ZMode(FALSE, LEQUAL, TRUE)` (985), `NumTevStages(1); NumChans(1)` (986-987),
`GXSetTevOp(0, GX_MODULATE)` (988), `TevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0)` (989), `ChanCtrl(COLOR0A0, FALSE,
VTX, VTX, 0, NONE, NONE)` (990) → vertex colour; GX_TEXMAP0 = `t->tex` (`GXLoadTlut(&t->tlut, TLUT0)` for CI,
991-994); `GX_TEXMTX0 = t->mtx`, `GXSetTexCoordGen2(0, MTX2x4, TEX0, TEXMTX0, FALSE, PTIDENTITY)` (995-997);
blend SRCALPHA/INVSRCALPHA (998). Fog NONE in `draw()` (1045).

### 6.3 `EprintfDrawing` (eprintf.cpp:242-312) — `eprintf_eprintfdrawing`

Debug on-screen text; compiled but returns at 265 when `pG->debug_mode == 0`. Ortho 512×448, **`GXSetCullMode(
GX_CULL_BACK)`** (261, the only back-face cull in part B), `ZMode(TRUE, LEQUAL, TRUE)` (262), `NumTevStages(1);
NumChans(1)` (263-264), MODULATE (268), `TevOrder(0, TEXCOORD0, TEXMAP0, COLOR0A0)` (269), `ChanCtrl(COLOR0A0,
FALSE, VTX, VTX, 0, NONE, NONE)` (270), `NumTexGens(1)` (271), GX_TEXMAP0 = `fontTexObj` (272), `GX_TEXMTX0 =
fontTMtx` (273), texgen0 `MTX2x4 TEX0 TEXMTX0` (274), blend SRCALPHA/INVSRCALPHA (275).

### 6.4 `fadeDraw` (fade.cpp:111-140) — `fade_fadedraw`; `Draw_cinesco` (cinesco.cpp:78-128) — `cinesco_draw_cinesco`

Both: blend SRCALPHA/INVSRCALPHA, `GXSetColorUpdate(TRUE)`, ortho, cull NONE, `NumTexGens(0); NumTevStages(1);
GXSetTevOp(0, GX_PASSCLR); TevOrder(0, NULL, NULL, COLOR0A0); NumChans(1); ChanCtrl(COLOR0A0, FALSE, REG, VTX,
GX_LIGHT0, NONE, NONE)` (fade 124-131, cinesco 97-104; the light mask is meaningless with lighting off). fade:
`ZMode(TRUE, ALWAYS, FALSE)` (125), quad between y 56 and `xfbHeight-56` at `pF->z`. cinesco: `ZMode(FALSE,
ALWAYS, TRUE)` (98, writes Z = 10 without testing), fog NONE (96), two bars rows 0-56 and 393-449.

---

## 7. Totals (part B)

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

## Appendix A — debug-only unit `dbmodule.cpp` (not compiled into the port)

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

## Appendix B — open points / uncertainties

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
