// gx_pipeline.cpp — builds the backend-neutral GxTevProgram and GxPipelineKey from the GX state at draw
// time and hashes the pipeline-relevant part with FNV-1a 64 (CT-08, xbox/docs/02-GRAPHICS.md section 5).
//
// Hash coverage: everything a backend would compile (TEV stage programs, texgens, channel controls,
// indirect stages, swap tables, counts, blend/z/alpha-compare/cull/fog-type/update masks/dst-alpha/
// dither/pixel format/line width). Excluded: uniforms (konst colours, TEV registers, channel colours,
// lights, indirect matrices, fog distances and colour) which change without recompiling anything.
#include "re4port/re4_xbox.h"
#include "gx_state.h"
#include <dolphin/gx/GXEnum.h>
#include <string.h>

// Serialising hasher: fields are appended explicitly so struct padding never reaches the hash.
struct Fnv1a64 {
    uint64_t h;
    Fnv1a64() : h(0xcbf29ce484222325ull) { }
    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = (const uint8_t*) p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) { bytes(&v, 2); }
    void u32(uint32_t v) { bytes(&v, 4); }
};

static void hash_op(Fnv1a64& f, const GxTevOpParams& p)
{
    f.u8(p.op); f.u8(p.bias); f.u8(p.scale); f.u8(p.clamp); f.u8(p.out_reg);
}

uint64_t gx_pipeline_hash(const GxTevProgram* prog, const GxPipelineKey* key)
{
    Fnv1a64 f;
    f.u8(prog->num_stages); f.u8(prog->num_texgens); f.u8(prog->num_chans); f.u8(prog->num_ind_stages);
    for (int i = 0; i < prog->num_stages && i < GX_MAX_TEV_STAGES_PORT; i++) {
        const GxTevStage& s = prog->stage[i];
        f.u8(s.coord); f.u8(s.chan); f.u16(s.map);
        f.bytes(s.color_in, 4); f.bytes(s.alpha_in, 4);
        hash_op(f, s.color_op); hash_op(f, s.alpha_op);
        f.u8(s.kcsel); f.u8(s.kasel); f.u8(s.ras_swap); f.u8(s.tex_swap);
        const GxTevIndirect& n = s.ind;
        f.u8(n.ind_stage); f.u8(n.format); f.u8(n.bias_sel); f.u8(n.mtx_sel); f.u8(n.wrap_s); f.u8(n.wrap_t);
        f.u8(n.add_prev); f.u8(n.utc_lod); f.u8(n.alpha_sel);
    }
    for (int i = 0; i < prog->num_texgens && i < GX_MAX_TEXGENS_PORT; i++) {
        const GxTexGen& g = prog->texgen[i];
        f.u8(g.func); f.u8(g.src); f.u8(g.normalize); f.u8(g.mtx); f.u8(g.postmtx);
    }
    // All four channel controls matter even with num_chans == 0 (GX_COLOR_ZERO stages still work), so
    // hash them whole; the rasteriser only looks at the first num_chans colour/alpha pairs, which the
    // backend decides.
    for (int i = 0; i < GX_MAX_CHANS_PORT; i++) {
        const GxChanCtrl& c = prog->chan[i];
        f.u8(c.enable); f.u8(c.amb_src); f.u8(c.mat_src); f.u8(c.diff_fn); f.u8(c.attn_fn); f.u8(c.light_mask);
    }
    for (int i = 0; i < prog->num_ind_stages && i < GX_MAX_IND_STAGES_PORT; i++) {
        const GxIndStage& s = prog->ind[i];
        f.u8(s.coord); f.u8(s.map); f.u8(s.scale_s); f.u8(s.scale_t);
    }
    f.bytes(prog->swap_table, sizeof prog->swap_table);

    f.u8(key->cull_mode);
    f.u8(key->blend_type); f.u8(key->blend_src); f.u8(key->blend_dst); f.u8(key->blend_op);
    f.u8(key->z_enable); f.u8(key->z_func); f.u8(key->z_update); f.u8(key->z_comp_loc);
    f.u8(key->alpha_comp0); f.u8(key->alpha_ref0); f.u8(key->alpha_op); f.u8(key->alpha_comp1); f.u8(key->alpha_ref1);
    f.u8(key->color_update); f.u8(key->alpha_update); f.u8(key->dst_alpha_enable); f.u8(key->dst_alpha);
    f.u8(key->dither); f.u8(key->fog_type); f.u8(key->line_width); f.u8(key->pixel_fmt); f.u8(key->z_fmt);
    return f.h;
}

void gx_pipeline_build(const GxState* st, GxTevProgram* prog, GxPipelineKey* key)
{
    *prog = st->tev;

    // Derived: which texture maps the active stages sample. GXSetTevOrder's map may be GX_TEXMAP_NULL
    // (0xFF) or carry GX_TEX_DISABLE (0x100): neither samples a texture (GXTev.c). Indirect stages
    // sample their own maps.
    uint8_t mask = 0;
    for (int i = 0; i < prog->num_stages; i++) {
        unsigned map = prog->stage[i].map;
        if (map != GX_TEXMAP_NULL && !(map & GX_TEX_DISABLE) && (map & ~GX_TEX_DISABLE) < GX_MAX_TEXMAP) {
            mask |= (uint8_t) (1u << (map & 7));
        }
    }
    for (int i = 0; i < prog->num_ind_stages; i++) mask |= (uint8_t) (1u << (prog->ind[i].map & 7));
    prog->tex_enable_mask = mask;

    memset(key, 0, sizeof *key);
    key->num_tev_stages = prog->num_stages;
    key->num_texgens = prog->num_texgens;
    key->num_chans = prog->num_chans;
    key->num_ind_stages = prog->num_ind_stages;
    key->cull_mode = st->cull_mode;
    key->blend_type = st->blend_type; key->blend_src = st->blend_src; key->blend_dst = st->blend_dst; key->blend_op = st->blend_op;
    key->z_enable = st->z_enable; key->z_func = st->z_func; key->z_update = st->z_update; key->z_comp_loc = st->z_comp_loc;
    key->alpha_comp0 = st->alpha_comp0; key->alpha_ref0 = st->alpha_ref0; key->alpha_op = st->alpha_op;
    key->alpha_comp1 = st->alpha_comp1; key->alpha_ref1 = st->alpha_ref1;
    key->color_update = st->color_update; key->alpha_update = st->alpha_update;
    key->dst_alpha_enable = st->dst_alpha_enable; key->dst_alpha = st->dst_alpha;
    key->dither = st->dither;
    key->fog_type = st->fog_type;
    key->line_width = st->line_width;
    key->pixel_fmt = st->pixel_fmt; key->z_fmt = st->z_fmt;
    key->fog_start = st->fog_start; key->fog_end = st->fog_end; key->fog_near = st->fog_near; key->fog_far = st->fog_far;
    key->fog_color = st->fog_color;
    key->tev = prog;
    key->hash = gx_pipeline_hash(prog, key);
}
