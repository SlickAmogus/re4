// gx_pipeline.h — backend-neutral description of the active GX fragment/vertex pipeline (CT-08).
//
// GxTevProgram is what the host backend (CT-09) compiles to GLSL and the Xbox backend to register
// combiners / a vertex program. It has two parts:
//   - the PROGRAM part (hashed into GxPipelineKey::hash): stage programs, texgens, channel controls,
//     indirect stages, swap tables, counts;
//   - the UNIFORM part (not hashed): konst colours, TEV registers, channel ambient/material colours,
//     the 8 loaded lights, the 3 indirect matrices, the indirect mask.
// Enum fields hold the raw GX values from include/dolphin/gx/GXEnum.h (GXTevColorArg, GXTexGenType ...),
// exactly as the game passed them; derived register encodings are left to the backends.
#ifndef RE4PORT_GX_PIPELINE_H
#define RE4PORT_GX_PIPELINE_H

#include <stdint.h>
#include "re4port/gx_shim.h"

enum {
    GX_MAX_TEV_STAGES_PORT = 16,
    GX_MAX_TEXGENS_PORT    = 8,
    GX_MAX_CHANS_PORT      = 4,     // GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1
    GX_MAX_LIGHTS_PORT     = 8,
    GX_MAX_IND_STAGES_PORT = 4,
    GX_MAX_IND_MTX_PORT    = 3,
    GX_MAX_TEXMAPS_PORT    = 8,
};

// GXSetTevColorOp / GXSetTevAlphaOp parameters.
struct GxTevOpParams {
    uint8_t op;        // GXTevOp
    uint8_t bias;      // GXTevBias
    uint8_t scale;     // GXTevScale
    uint8_t clamp;     // GXBool
    uint8_t out_reg;   // GXTevRegID
};

// GXSetTevIndirect parameters (GXSetTevDirect/IndWarp/IndBumpXYZ expand to these like src/lib/GXBump.c).
struct GxTevIndirect {
    uint8_t ind_stage;  // GXIndTexStageID
    uint8_t format;     // GXIndTexFormat
    uint8_t bias_sel;   // GXIndTexBiasSel
    uint8_t mtx_sel;    // GXIndTexMtxID
    uint8_t wrap_s;     // GXIndTexWrap
    uint8_t wrap_t;
    uint8_t add_prev;   // GXBool
    uint8_t utc_lod;    // GXBool
    uint8_t alpha_sel;  // GXIndTexAlphaSel
};

struct GxTevStage {
    uint8_t  coord;          // GXTexCoordID: 0..7 or GX_TEXCOORD_NULL (0xFF)
    uint8_t  chan;           // GXChannelID as passed to GXSetTevOrder (GX_COLOR0A0 .. GX_COLOR_NULL)
    uint16_t map;            // GXTexMapID: 0..7, GX_TEXMAP_NULL (0xFF), may carry GX_TEX_DISABLE (0x100)
    uint8_t  color_in[4];    // GXTevColorArg a, b, c, d
    uint8_t  alpha_in[4];    // GXTevAlphaArg a, b, c, d
    GxTevOpParams color_op;
    GxTevOpParams alpha_op;
    uint8_t  kcsel;          // GXTevKColorSel
    uint8_t  kasel;          // GXTevKAlphaSel
    uint8_t  ras_swap;       // GXTevSwapSel
    uint8_t  tex_swap;
    GxTevIndirect ind;
};

struct GxTexGen {
    uint8_t  func;        // GXTexGenType
    uint8_t  src;         // GXTexGenSrc
    uint8_t  normalize;   // GXBool
    uint8_t  mtx;         // GXTexMtx id: 30..57 step 3 or GX_IDENTITY (60)
    uint8_t  postmtx;     // GXPTTexMtx id: 64..121 step 3 or GX_PTIDENTITY (125)
};

// GXSetChanCtrl parameters for one of GX_COLOR0, GX_COLOR1, GX_ALPHA0, GX_ALPHA1 (GX_COLOR0A0 sets two).
struct GxChanCtrl {
    uint8_t enable;       // GXBool
    uint8_t amb_src;      // GXColorSrc
    uint8_t mat_src;      // GXColorSrc
    uint8_t diff_fn;      // GXDiffuseFn (the SDK forces GX_DF_NONE when attn_fn == GX_AF_SPEC; backends do too)
    uint8_t attn_fn;      // GXAttnFn
    uint8_t light_mask;   // GX_LIGHT0..7 bits
};

struct GxIndStage {
    uint8_t coord;     // GXTexCoordID (GX_TEXCOORD_NULL already mapped to 0 like the SDK)
    uint8_t map;       // GXTexMapID (GX_TEXMAP_NULL mapped to 0)
    uint8_t scale_s;   // GXIndTexScale
    uint8_t scale_t;
};

// What GXInitLight* build inside the game's 64-byte GXLightObj and GXLoadLightObjImm copies here.
// dir is stored negated like the SDK does (GXInitLightDir stores -n, the hardware half-angle convention).
struct GxLightObjInt {
    uint32_t color;     // RGBA8, bytes r,g,b,a
    float    a[3];      // angular attenuation a0 a1 a2
    float    k[3];      // distance attenuation k0 k1 k2
    float    pos[3];
    float    dir[3];
};

struct GxTevProgram {
    // ---- program part (hashed) ----
    uint8_t     num_stages;        // GXSetNumTevStages (1..16)
    uint8_t     num_texgens;       // GXSetNumTexGens (0..8)
    uint8_t     num_chans;         // GXSetNumChans (0..2)
    uint8_t     num_ind_stages;    // GXSetNumIndStages (0..4)
    GxTevStage  stage[GX_MAX_TEV_STAGES_PORT];
    GxTexGen    texgen[GX_MAX_TEXGENS_PORT];
    GxChanCtrl  chan[GX_MAX_CHANS_PORT];
    GxIndStage  ind[GX_MAX_IND_STAGES_PORT];
    uint8_t     swap_table[4][4];  // [GXTevSwapSel][r g b a] = GXTevColorChan
    uint8_t     tex_enable_mask;   // derived: bit n set when an active stage or ind stage samples GX_TEXMAPn
    // ---- uniform part (not hashed) ----
    uint32_t    kcolor[4];         // GXSetTevKColor, RGBA8 bytes r,g,b,a
    int16_t     tevreg[4][4];      // GXSetTevColor(S10): [GXTevRegID][r g b a], -1024..1023
    uint32_t    amb_color[2];      // GXSetChanAmbColor per colour channel, RGBA8
    uint32_t    mat_color[2];      // GXSetChanMatColor
    GxLightObjInt light[GX_MAX_LIGHTS_PORT];
    float       ind_mtx[GX_MAX_IND_MTX_PORT][2][3];
    int8_t      ind_mtx_scale[GX_MAX_IND_MTX_PORT];
    uint32_t    ind_mask;          // __GXSetIndirectMask
};

struct GxState;

// Builds the resolved program and key from the state (gx_pipeline.cpp). `prog` receives a copy of the
// state's TEV/texgen/channel data with the derived fields filled in; `key` points at it.
void gx_pipeline_build(const GxState* st, GxTevProgram* prog, GxPipelineKey* key);

// FNV-1a 64 over the program part of `prog` and the pipeline fields of `key` (everything except
// hash, fog distances/colour and the tev pointer). Exposed for tests.
uint64_t gx_pipeline_hash(const GxTevProgram* prog, const GxPipelineKey* key);

#endif // RE4PORT_GX_PIPELINE_H
