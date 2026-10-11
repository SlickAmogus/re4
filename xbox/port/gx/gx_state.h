// gx_state.h — everything GX holds (xbox/docs/02-GRAPHICS.md section 2), as one POD struct (CT-08).
//
// Every GX entry point of include/gx.h is a setter into gx_state that marks a dirty bit; GXBegin /
// GXCallDisplayList (gx_dl.cpp) resolve the dirty state into backend calls before drawing. Internal to
// xbox/port/gx/ and its tests; backends only see re4port/gx_shim.h.
#ifndef RE4PORT_GX_STATE_H
#define RE4PORT_GX_STATE_H

#include <stdint.h>
#include <stddef.h>
#include "re4port/gx_shim.h"
#include "gx_pipeline.h"
#include "gx_vertex.h"

enum {
    GX_VCD_ATTRS   = 21,   // GX_VA_PNMTXIDX .. GX_VA_TEX7
    GX_VAT_ATTRS   = 12,   // GX_VA_POS .. GX_VA_TEX7 (index = attr - GX_VA_POS); GX_VA_NBT shares GX_VA_NRM
    GX_ARRAY_ATTRS = 16,   // GX_VA_POS .. GX_LIGHT_ARRAY (index = attr - GX_VA_POS)
    GX_NUM_VTXFMT  = 8,
    GX_NUM_PTTEXMTX_PORT = 21,  // GX_PTTEXMTX0..19 + GX_PTIDENTITY (slot 20)
    GX_NUM_TLUTS_PORT    = 20,
};

// Dirty bits. The decoder reads VCD/VAT/arrays directly, so they need none.
enum {
    GX_DIRTY_TEV      = 1u << 0,   // stage programs, counts, orders, swap modes, indirect params
    GX_DIRTY_TEXGEN   = 1u << 1,
    GX_DIRTY_CHAN     = 1u << 2,   // channel controls, num chans
    GX_DIRTY_PIXEL    = 1u << 3,   // blend / z / alpha compare / cull / fog type / updates / dst alpha ...
    GX_DIRTY_IND      = 1u << 4,   // indirect stage orders/scales/count
    GX_DIRTY_UNIFORMS = 1u << 5,   // konst, tev regs, chan colours, lights, ind matrices, fog params
    GX_DIRTY_MTX      = 1u << 6,   // pos/nrm/tex palettes, projection
    GX_DIRTY_TEX      = 1u << 7,   // a texture map was (re)loaded
    GX_DIRTY_PIPELINE = GX_DIRTY_TEV | GX_DIRTY_TEXGEN | GX_DIRTY_CHAN | GX_DIRTY_PIXEL | GX_DIRTY_IND | GX_DIRTY_UNIFORMS,
    GX_DIRTY_ALL      = 0xFFu,
};

struct GxVatAttr { uint8_t cnt, type, frac; };     // GXCompCnt, GXCompType, fractional bits

struct GxArray { const uint8_t* base; uint8_t stride; };

// Port-internal layout of the game's opaque objects (include/gx.h: GXTexObj u32[8], GXTlutObj u32[3],
// GXLightObj u32[16]). Sizes are static_asserted in gx_state.cpp.
#define GX_TEXOBJ_MAGIC 0x584F5854u   // 'TXOX'
struct GxTexObjInt {
    const void* image;
    uint16_t width, height;
    uint8_t  format;        // GXTexFmt / GXCITexFmt
    uint8_t  wrap_s, wrap_t;
    uint8_t  mipmap;
    uint8_t  min_filt, mag_filt;   // GXTexFilter
    uint8_t  max_aniso;            // GXAnisotropy
    uint8_t  is_ci;
    float    lod_bias;             // -4.0 .. 3.99
    uint8_t  min_lod16, max_lod16; // LOD * 16, as the hardware's mode1 register holds them (0..160)
    uint8_t  bias_clamp, edge_lod;
    uint32_t tlut_name;
    uint32_t magic;         // GX_TEXOBJ_MAGIC once GXInitTexObj ran
};

struct GxTlutObjInt {
    const void* lut;
    uint16_t n_entries;
    uint8_t  fmt;           // GXTlutFmt
    uint8_t  valid;
    uint32_t pad;
};

struct GxImmediate {
    uint8_t  pending;       // a GXBegin is open and not all nverts have arrived
    uint8_t  prim;          // GX_PRIM_*
    uint8_t  vtxfmt;
    uint8_t  pad;
    uint16_t nverts;
    uint16_t pad2;
    uint32_t vsize;         // bytes per vertex (direct attributes, indices, matrix indices)
    uint32_t expected;      // nverts * vsize
};

struct GxDlRecord {
    uint8_t  active;        // GXBeginDisplayList in progress (dbmodule.cpp only)
    uint8_t* base;
    uint32_t size;
    uint32_t count;         // bytes written so far
    uint8_t  overflow;
};

struct GxCopyRect { uint16_t left, top, wd, ht; };

struct GxState {
    uint32_t dirty;

    // --- vertex descriptor / formats / arrays (GXAttr.c) ---
    uint8_t   vcd[GX_VCD_ATTRS];      // GXAttrType per attribute (GX_VA_NRM slot shared with GX_VA_NBT)
    uint8_t   has_nrm, has_binrm;     // SDK's hasNrms/hasBiNrms
    uint8_t   nrm_type;
    GxVatAttr vat[GX_NUM_VTXFMT][GX_VAT_ATTRS];
    GxArray   array[GX_ARRAY_ATTRS];

    // --- transform (GXTransform.c) ---
    float    pos_mtx[GX_PNMTX_COUNT][3][4];
    float    nrm_mtx[GX_PNMTX_COUNT][3][4];
    float    tex_mtx[GX_TEXMTX_COUNT][3][4];      // slot 10 = GX_IDENTITY
    uint8_t  tex_mtx_type[GX_TEXMTX_COUNT];       // GXTexMtxType of the last load
    float    pt_mtx[GX_NUM_PTTEXMTX_PORT][3][4];  // post-transform matrices, slot 20 = GX_PTIDENTITY
    float    proj[4][4];
    float    proj6[6];       // the SDK's 6-float form (GXGetProjectionv)
    uint8_t  proj_type;      // GXProjectionType
    uint32_t current_mtx;    // GXSetCurrentMtx: 0..27 step 3

    // --- TEV / texgen / channels / lights / indirect: lives in the program struct, copied at draw ---
    GxTevProgram tev;

    // --- pixel pipeline (GXPixel.c, GXTev.c, GXGeometry.c) ---
    uint8_t  blend_type, blend_src, blend_dst, blend_op;
    uint8_t  color_update, alpha_update;
    uint8_t  z_enable, z_func, z_update, z_comp_loc;
    uint8_t  alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    uint8_t  dither;
    uint8_t  dst_alpha_enable, dst_alpha;
    uint8_t  cull_mode;
    uint8_t  pixel_fmt, z_fmt;
    uint8_t  line_width, line_tex_offsets;
    uint8_t  tex_offset_line[GX_MAX_TEXGENS_PORT], tex_offset_point[GX_MAX_TEXGENS_PORT];
    uint8_t  fog_type;
    float    fog_start, fog_end, fog_near, fog_far;
    uint32_t fog_color;
    uint32_t scissor_left, scissor_top, scissor_wd, scissor_ht;
    float    vp_left, vp_top, vp_wd, vp_ht, vp_nearz, vp_farz;

    // --- EFB copies (GXFrameBuf.c) ---
    uint32_t   copy_clear_color;   // RGBA8
    uint32_t   copy_clear_z;
    GxCopyRect tex_copy_src;
    uint16_t   tex_copy_dst_wd, tex_copy_dst_ht;
    uint8_t    tex_copy_dst_fmt, tex_copy_dst_mipmap;
    GxCopyRect disp_copy_src;
    uint16_t   disp_copy_dst_wd, disp_copy_dst_ht;
    float      disp_copy_yscale;
    uint8_t    disp_copy_gamma;
    uint8_t    copy_filter_aa, copy_filter_vf;
    uint8_t    copy_sample_pattern[12][2];
    uint8_t    copy_vfilter[7];

    // --- textures ---
    GxTexObjInt  texmap[GX_MAX_TEXMAPS_PORT];
    uint8_t      texmap_valid[GX_MAX_TEXMAPS_PORT];
    GxTlutObjInt tlut[GX_NUM_TLUTS_PORT];
    uint32_t     tex_generation;   // bumped by GXInvalidateTexAll (raw images are re-uploaded)

    // --- geometry front end ---
    GxImmediate imm;
    GxDlRecord  dl;
    uint8_t     frame_open;        // begin_frame() issued since the last GXCopyDisp
    uint8_t     pending_clear;     // GXCopyDisp(clear=1): clear at the start of the next frame

    // --- resolved at draw ---
    GxTevProgram  resolved_tev;
    GxPipelineKey key;
    uint32_t      draws;           // statistics
};

extern GxState gx_state;

// gx_state.cpp
void gx_resolve_dirty(void);                 // push dirty state to the backend (pipeline, matrices, textures)
void gx_ensure_frame_open(void);             // begin_frame() (+ deferred clear) if not yet done this frame
void gx_check_pending(const char* who);      // flush a partial immediate-mode primitive, with a warning
int  gx_attr_vat_index(int attr);            // GX_VA_POS..TEX7 / GX_VA_NBT -> 0..11, else -1
int  gx_attr_array_index(int attr);          // GX_VA_POS..GX_LIGHT_ARRAY / GX_VA_NBT -> 0..15, else -1

// gx_dl.cpp
void gx_flush_pending(const char* who);      // decode + draw the complete vertices of a partial primitive
void gx_imm_reset(void);                     // drop the immediate-mode buffer (gx_init)
void gx_emit_flush(void);                    // draw whatever the decoder accumulated

#endif // RE4PORT_GX_STATE_H
