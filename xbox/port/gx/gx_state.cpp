// gx_state.cpp — the GX state machine: every entry point of include/gx.h as a setter into gx_state,
// the SDK's power-on defaults (src/lib/GXInit.c __GXInitGX), and the resolve step that pushes dirty
// state to the backend (xbox/docs/02-GRAPHICS.md section 2, CT-08).
//
// Semantics follow the SDK sources in src/lib/GX*.c (referenced per function), not OpenGL. Enum
// parameters arrive as plain ints (the game's include/gx.h); the names come from dolphin/gx/GXEnum.h.
// Geometry (GXBegin, GXCallDisplayList, GXDrawTorus, the FIFO) lives in gx_dl.cpp; the pipeline key
// builder in gx_pipeline.cpp.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"
#include "gx_state.h"
#include "gx.h"                     // the game's prototypes (plain-int enums) and GXColor/GXTexObj types
#include <dolphin/gx/GXEnum.h>
#include <string.h>
#include <math.h>

static_assert(sizeof(GxTexObjInt) <= sizeof(GXTexObj), "GxTexObjInt must fit the game's 32-byte GXTexObj");
static_assert(sizeof(GxTlutObjInt) <= sizeof(GXTlutObj), "GxTlutObjInt must fit the game's 12-byte GXTlutObj");
static_assert(sizeof(GxLightObjInt) <= sizeof(GXLightObj), "GxLightObjInt must fit the game's 64-byte GXLightObj");

GxState gx_state;

static int gx_initialised = 0;

static inline uint32_t gxcolor_u32(GXColor c) { return GX_RGBA8(c.r, c.g, c.b, c.a); }

// --- attribute index helpers ------------------------------------------------------------------------

int gx_attr_vat_index(int attr)
{
    if (attr == GX_VA_NBT) return GX_VA_NRM - GX_VA_POS;
    if (attr >= GX_VA_POS && attr <= GX_VA_TEX7) return attr - GX_VA_POS;
    return -1;
}

int gx_attr_array_index(int attr)
{
    if (attr == GX_VA_NBT) return GX_VA_NRM - GX_VA_POS;
    if (attr >= GX_VA_POS && attr <= GX_LIGHT_ARRAY) return attr - GX_VA_POS;
    return -1;
}

// Called at the top of every state setter: GX forbids state changes between GXBegin and the last
// vertex (CHECK_GXBEGIN in the SDK). The port draws the complete vertices and warns.
void gx_check_pending(const char* who)
{
    if (gx_state.imm.pending) gx_flush_pending(who);
    gx_emit_flush();   // vertices decoded under the old state must be drawn before it changes
}

// --- texture cache -----------------------------------------------------------------------------------
// Backend textures are created lazily per unique image pointer. A ConvertedImage (RE4_IMG_MAGIC) is
// immutable (it came from disc), a raw pointer is game-written memory (movie planes, noise textures) and
// is re-uploaded after GXInvalidateTexAll; an EFB-copy destination (GXCopyTex) is a render target.

enum { TEXCACHE_SIZE = 2048 };
enum { TC_RAW = 1, TC_RT = 2, TC_CONVERTED = 3 };

struct TexCacheEntry {
    const void* image;
    GxTexHandle handle;
    uint32_t    gen;
    uint16_t    width, height;
    uint8_t     kind;
    uint8_t     gx_format;
};

static TexCacheEntry gx_texcache[TEXCACHE_SIZE];
static int gx_texcache_used = 0;

// 4x4 GX_TF_IA8 white, what __GXInitGX loads into all eight maps.
static const uint8_t gx_default_tex_image[32] __attribute__((aligned(32))) = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static TexCacheEntry* texcache_find(const void* image, int create)
{
    uint32_t h = ((uint32_t) (uintptr_t) image >> 5) * 2654435761u;
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        TexCacheEntry* e = &gx_texcache[(h + i) & (TEXCACHE_SIZE - 1)];
        if (e->image == image) return e;
        if (!e->image) {
            if (!create) return 0;
            if (gx_texcache_used >= TEXCACHE_SIZE - 1) {
                re4_log_once("texcache_full", "GX: texture cache full (%d images)\n", TEXCACHE_SIZE);
                return 0;
            }
            gx_texcache_used++;
            memset(e, 0, sizeof *e);
            e->image = image;
            return e;
        }
    }
    return 0;
}

static void texcache_release(TexCacheEntry* e)
{
    if (e->handle && gx_backend && gx_backend->destroy_texture) gx_backend->destroy_texture(e->handle);
    e->handle = 0;
}

static void texcache_reset(void)
{
    for (int i = 0; i < TEXCACHE_SIZE; i++) {
        if (gx_texcache[i].image) texcache_release(&gx_texcache[i]);
    }
    memset(gx_texcache, 0, sizeof gx_texcache);
    gx_texcache_used = 0;
}

// Creates (or re-creates) the backend texture for a texobj's image.
static GxTexHandle texcache_resolve(const GxTexObjInt* t)
{
    if (!gx_backend || !t->image) return 0;
    TexCacheEntry* e = texcache_find(t->image, 1);
    if (!e) return 0;

    if (e->kind == TC_RT) return e->handle;   // EFB copy destination, owned by the backend

    const ConvertedImage* img = (const ConvertedImage*) t->image;
    int converted = (img->magic == RE4_IMG_MAGIC);
    int stale = e->handle && e->kind == TC_RAW && e->gen != gx_state.tex_generation;
    if (e->handle && !stale) return e->handle;
    if (stale) texcache_release(e);

    if (converted) {
        e->kind = TC_CONVERTED;
        e->handle = gx_backend->create_texture(img);
    } else {
        // Raw GameCube-format texels (or a CPU-written buffer). The backend gets a synthesized header
        // with the GX dimensions and data_size 0; it decides whether it can interpret the GX format
        // (gx_format is in the entry) — CT-09/CT-10 territory. Logged once per distinct image.
        if (t->image != gx_default_tex_image) {
            re4_log_once("texcache_raw", "GX: raw (non-RE4X) image %p %ux%u fmt %u passed to the backend\n",
                         t->image, t->width, t->height, t->format);
        }
        ConvertedImage hdr;
        memset(&hdr, 0, sizeof hdr);
        hdr.magic = 0;
        hdr.width = t->width;
        hdr.height = t->height;
        hdr.nv2a_format = 0xFF;          // "unknown, GX format in gx_format": backend policy
        hdr.mip_count = t->mipmap ? 0 : 1;
        hdr.data_size = 0;
        e->kind = TC_RAW;
        e->handle = gx_backend->create_texture(&hdr);
    }
    e->gen = gx_state.tex_generation;
    e->width = t->width;
    e->height = t->height;
    e->gx_format = t->format;
    return e->handle;
}

// GXCopyTex destination: a render-target texture keyed by the destination pointer the game will later
// pass to GXInitTexObj.
static GxTexHandle texcache_render_target(const void* dest, uint16_t w, uint16_t h, uint8_t gx_format)
{
    if (!gx_backend) return 0;
    TexCacheEntry* e = texcache_find(dest, 1);
    if (!e) return 0;
    if (e->handle && (e->kind != TC_RT || e->width != w || e->height != h || e->gx_format != gx_format)) {
        texcache_release(e);
    }
    if (!e->handle) {
        ConvertedImage hdr;
        memset(&hdr, 0, sizeof hdr);
        hdr.magic = RE4_IMG_MAGIC;
        hdr.width = w;
        hdr.height = h;
        hdr.nv2a_format = 0xFF;   // backend picks the surface format for this GX copy format
        hdr.mip_count = 1;
        hdr.data_size = 0;        // no pixels: allocate a writable texture
        e->handle = gx_backend->create_texture(&hdr);
        e->kind = TC_RT;
        e->width = w;
        e->height = h;
        e->gx_format = gx_format;
    }
    e->gen = gx_state.tex_generation;
    return e->handle;
}

// --- resolve: dirty state -> backend ------------------------------------------------------------------

void gx_ensure_frame_open(void)
{
    GxState& s = gx_state;
    if (!gx_backend || s.frame_open) return;
    s.frame_open = 1;
    gx_backend->begin_frame();
    if (s.pending_clear) {
        s.pending_clear = 0;
        gx_backend->clear(s.copy_clear_color, s.copy_clear_z, 1, 1);
    }
}

static void bind_texture_maps(void)
{
    GxState& s = gx_state;
    uint8_t mask = s.resolved_tev.tex_enable_mask;
    for (int map = 0; map < GX_MAX_TEXMAPS_PORT; map++) {
        if (!(mask & (1 << map))) continue;
        const GxTexObjInt* t = &s.texmap[map];
        if (!s.texmap_valid[map]) {
            re4_log_once("texmap_unset", "GX: stage samples GX_TEXMAP%d with no texobj loaded\n", map);
            continue;
        }
        GxTexHandle h = texcache_resolve(t);
        gx_backend->bind_texture(map, h, t->wrap_s, t->wrap_t, t->min_filt, t->mag_filt, t->lod_bias, t->max_aniso);
    }
}

void gx_resolve_dirty(void)
{
    GxState& s = gx_state;
    if (!gx_backend) { s.dirty = 0; return; }
    gx_ensure_frame_open();
    if (s.dirty & GX_DIRTY_PIPELINE) {
        gx_pipeline_build(&s, &s.resolved_tev, &s.key);
        gx_backend->set_pipeline(&s.key);
        s.dirty |= GX_DIRTY_TEX;   // the set of referenced maps may have changed
    }
    if (s.dirty & GX_DIRTY_MTX) {
        gx_backend->set_matrices(s.proj, s.proj_type, s.pos_mtx, s.nrm_mtx, s.tex_mtx);
    }
    if (s.dirty & GX_DIRTY_TEX) bind_texture_maps();
    s.dirty = 0;
}

extern "C" void gx_flush(void)
{
    gx_check_pending("gx_flush");
    gx_emit_flush();
    if (gx_state.dirty) gx_resolve_dirty();
}

// --- power-on defaults (src/lib/GXInit.c __GXInitGX) -------------------------------------------------

static const float gx_identity34[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };

static void load_mtx34(float dst[3][4], const f32 src[][4], int rows)
{
    for (int r = 0; r < rows; r++) for (int c = 0; c < 4; c++) dst[r][c] = src[r][c];
    if (rows == 2) { dst[2][0] = 0; dst[2][1] = 0; dst[2][2] = 1; dst[2][3] = 0; }
}

// GXSetProjectionv (used for the default projection): rebuilds the 4x4 from the SDK's 6-float form.
static void set_projection_v(const f32* ptr)
{
    f32 m[4][4];
    memset(m, 0, sizeof m);
    int ortho = ptr[0] != 0.0f;
    m[0][0] = ptr[1];
    m[1][1] = ptr[3];
    m[2][2] = ptr[5];
    m[2][3] = ptr[6];
    if (ortho) { m[0][3] = ptr[2]; m[1][3] = ptr[4]; m[3][3] = 1.0f; }
    else { m[0][2] = ptr[2]; m[1][2] = ptr[4]; m[3][2] = -1.0f; }
    GXSetProjection(m, ortho ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE);
}

extern "C" void gx_init(void)
{
    GxState& s = gx_state;
    gx_imm_reset();
    texcache_reset();
    memset(&s, 0, sizeof s);
    gx_initialised = 1;

    static const f32 default_proj[7] = { 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, -2.0f, 0.0f };   // GXDefaultProjData
    const GXColor clear = { 64, 64, 64, 255 };
    const GXColor black = { 0, 0, 0, 0 };
    const GXColor white = { 255, 255, 255, 255 };
    const u16 fbWidth = 640, efbHeight = 480, xfbHeight = 480;   // GXNtsc480IntDf

    GXSetCopyClear(clear, 0xFFFFFF);
    for (int i = 0; i < 8; i++) GXSetTexCoordGen2(GX_TEXCOORD0 + i, GX_TG_MTX2x4, GX_TG_TEX0 + i, GX_IDENTITY, 0, GX_PTIDENTITY);
    GXSetNumTexGens(1);
    GXClearVtxDesc();
    GXInvalidateVtxCache();
    for (int i = GX_VA_POS; i <= GX_LIGHT_ARRAY; i++) GXSetArray(i, 0, 0);
    for (int fmt = 0; fmt < GX_NUM_VTXFMT; fmt++) {   // GXDefaultVATList
        GXSetVtxAttrFmt(fmt, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(fmt, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(fmt, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXSetVtxAttrFmt(fmt, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
        for (int t = 0; t < 8; t++) GXSetVtxAttrFmt(fmt, GX_VA_TEX0 + t, GX_TEX_ST, GX_F32, 0);
    }
    GXSetLineWidth(6, GX_TO_ZERO);
    for (int i = 0; i < 8; i++) GXEnableTexOffsets(GX_TEXCOORD0 + i, 0, 0);

    for (int i = 0; i < GX_PNMTX_COUNT; i++) {        // the hardware leaves these undefined; identity is kinder
        load_mtx34(s.pos_mtx[i], gx_identity34, 3);
        load_mtx34(s.nrm_mtx[i], gx_identity34, 3);
    }
    for (int i = 0; i < GX_TEXMTX_COUNT; i++) { load_mtx34(s.tex_mtx[i], gx_identity34, 3); s.tex_mtx_type[i] = GX_MTX3x4; }
    for (int i = 0; i < GX_NUM_PTTEXMTX_PORT; i++) load_mtx34(s.pt_mtx[i], gx_identity34, 3);
    GXLoadPosMtxImm(gx_identity34, GX_PNMTX0);
    GXLoadNrmMtxImm(gx_identity34, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXLoadTexMtxImm(gx_identity34, GX_IDENTITY, GX_MTX3x4);
    GXLoadTexMtxImm(gx_identity34, GX_PTIDENTITY, GX_MTX3x4);
    GXSetViewport(0.0f, 0.0f, fbWidth, xfbHeight, 0.0f, 1.0f);
    set_projection_v(default_proj);
    GXSetCullMode(GX_CULL_BACK);
    GXSetScissor(0, 0, fbWidth, efbHeight);
    GXSetNumChans(0);
    GXSetChanCtrl(GX_COLOR0A0, 0, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR0A0, black);
    GXSetChanMatColor(GX_COLOR0A0, white);
    GXSetChanCtrl(GX_COLOR1A1, 0, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanAmbColor(GX_COLOR1A1, black);
    GXSetChanMatColor(GX_COLOR1A1, white);
    GXInvalidateTexAll();

    GXTexObj tex_obj;
    GXInitTexObj(&tex_obj, (void*) gx_default_tex_image, 4, 4, GX_TF_IA8, GX_CLAMP, GX_CLAMP, 0);
    for (int i = 0; i < 8; i++) GXLoadTexObj(&tex_obj, GX_TEXMAP0 + i);

    for (int i = 0; i < 8; i++) GXSetTevOrder(GX_TEVSTAGE0 + i, GX_TEXCOORD0 + i, GX_TEXMAP0 + i, GX_COLOR0A0);
    for (int i = 8; i < 16; i++) GXSetTevOrder(GX_TEVSTAGE0 + i, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    for (int i = 0; i < 16; i++) {
        GXSetTevKColorSel(i, GX_TEV_KCSEL_1_4);
        GXSetTevKAlphaSel(i, GX_TEV_KASEL_1);
        GXSetTevSwapMode(i, GX_TEV_SWAP0, GX_TEV_SWAP0);
    }
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    for (int i = 0; i < 16; i++) GXSetTevDirect(i);
    GXSetNumIndStages(0);
    for (int i = 0; i < 4; i++) GXSetIndTexCoordScale(i, GX_ITS_1, GX_ITS_1);
    GXSetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, black);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetColorUpdate(1);
    GXSetAlphaUpdate(1);
    GXSetZMode(1, GX_LEQUAL, 1);
    GXSetZCompLoc(1);
    GXSetDither(1);
    GXSetDstAlpha(0, 0);
    GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GXSetDispCopySrc(0, 0, fbWidth, efbHeight);
    GXSetDispCopyDst(fbWidth, efbHeight);
    GXSetDispCopyYScale((f32) xfbHeight / (f32) efbHeight);
    GXSetDispCopyGamma(GX_GM_1_0);
    GXClearBoundingBox();
    __GXSetIndirectMask(0);

    s.dirty = GX_DIRTY_ALL;
    s.draws = 0;
}

extern "C" void gx_select_backend(const GxBackend* b)
{
    gx_backend = b;
    re4_log("GX backend: %s\n", b ? b->name : "(none)");
    if (!gx_initialised) gx_init();
    gx_state.dirty = GX_DIRTY_ALL;
    gx_state.frame_open = 0;
}

// =====================================================================================================
// GX entry points. All extern "C", signatures from include/gx.h.
// =====================================================================================================

extern "C" {

// --- vertex descriptor and formats (GXAttr.c) --------------------------------------------------------

void GXClearVtxDesc(void)
{
    gx_check_pending("GXClearVtxDesc");
    GxState& s = gx_state;
    memset(s.vcd, GX_NONE, sizeof s.vcd);
    s.vcd[GX_VA_POS] = GX_DIRECT;     // the SDK sets the POS field of vcdLo to 1 (GX_DIRECT)
    s.has_nrm = s.has_binrm = 0;
    s.nrm_type = GX_NONE;
}

void GXSetVtxDesc(int attr, int type)
{
    gx_check_pending("GXSetVtxDesc");
    GxState& s = gx_state;
    if (type < GX_NONE || type > GX_INDEX16) { re4_log_once("vtxdesc_type", "GXSetVtxDesc: bad type %d\n", type); return; }
    if (attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX) {
        // matrix indices are GX_NONE or GX_DIRECT only (one u8 in the vertex)
        s.vcd[attr] = type != GX_NONE ? GX_DIRECT : GX_NONE;
        return;
    }
    if (attr == GX_VA_NRM) {
        if (type != GX_NONE) { s.has_nrm = 1; s.has_binrm = 0; s.nrm_type = (uint8_t) type; }
        else s.has_nrm = 0;
    } else if (attr == GX_VA_NBT) {
        if (type != GX_NONE) { s.has_binrm = 1; s.has_nrm = 0; s.nrm_type = (uint8_t) type; }
        else s.has_binrm = 0;
    } else if (attr >= GX_VA_POS && attr <= GX_VA_TEX7) {
        s.vcd[attr] = (uint8_t) type;
        return;
    } else {
        re4_log_once("vtxdesc_attr", "GXSetVtxDesc: bad attribute %d\n", attr);
        return;
    }
    s.vcd[GX_VA_NRM] = (s.has_nrm || s.has_binrm) ? s.nrm_type : (uint8_t) GX_NONE;
}

void GXSetVtxAttrFmt(int vtxfmt, int attr, int cnt, int type, u8 frac)
{
    gx_check_pending("GXSetVtxAttrFmt");
    int ai = gx_attr_vat_index(attr);
    if ((unsigned) vtxfmt >= GX_NUM_VTXFMT || ai < 0) {
        re4_log_once("vtxattrfmt_bad", "GXSetVtxAttrFmt: bad vtxfmt %d / attr %d\n", vtxfmt, attr);
        return;
    }
    GxVatAttr& a = gx_state.vat[vtxfmt][ai];
    a.cnt = (uint8_t) cnt;
    a.type = (uint8_t) type;
    a.frac = (uint8_t) (frac & 31);   // the SDK asserts frac < 32; normals ignore it (fixed per type)
}

void GXSetArray(int attr, void* base_ptr, u8 stride)
{
    gx_check_pending("GXSetArray");
    int ai = gx_attr_array_index(attr);
    if (ai < 0) { re4_log_once("setarray_bad", "GXSetArray: bad attribute %d\n", attr); return; }
    gx_state.array[ai].base = (const uint8_t*) base_ptr;
    gx_state.array[ai].stride = stride;
}

void GXInvalidateVtxCache(void) { }   // no vertex cache to invalidate

// --- transform (GXTransform.c) -------------------------------------------------------------------------

static int pn_slot(u32 id, const char* who)
{
    if (id > 27 || (id % 3) != 0) {
        re4_log_once(who, "%s: matrix id %u is not GX_PNMTX0..9\n", who, (unsigned) id);
        return (int) ((id / 3) % GX_PNMTX_COUNT);
    }
    return (int) (id / 3);
}

void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id)
{
    gx_check_pending("GXLoadPosMtxImm");
    load_mtx34(gx_state.pos_mtx[pn_slot(id, "GXLoadPosMtxImm")], mtx, 3);
    gx_state.dirty |= GX_DIRTY_MTX;
}

void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id)
{
    gx_check_pending("GXLoadNrmMtxImm");
    load_mtx34(gx_state.nrm_mtx[pn_slot(id, "GXLoadNrmMtxImm")], mtx, 3);
    gx_state.dirty |= GX_DIRTY_MTX;
}

void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, int type)
{
    gx_check_pending("GXLoadTexMtxImm");
    GxState& s = gx_state;
    int rows = (type == GX_MTX2x4) ? 2 : 3;
    if (id >= GX_PTTEXMTX0) {
        // post-transform matrices: XF 0x500.., GX_PTIDENTITY (125) is slot 20
        u32 slot = (id - GX_PTTEXMTX0) / 3;
        if (slot >= GX_NUM_PTTEXMTX_PORT) slot = GX_NUM_PTTEXMTX_PORT - 1;
        load_mtx34(s.pt_mtx[slot], mtx, 3);   // post matrices are always 3x4 in the SDK
    } else if (id >= GX_TEXMTX0 && id <= GX_IDENTITY) {
        u32 slot = (id - GX_TEXMTX0) / 3;
        load_mtx34(s.tex_mtx[slot], mtx, rows);
        s.tex_mtx_type[slot] = (uint8_t) type;
    } else {
        re4_log_once("GXLoadTexMtxImm", "GXLoadTexMtxImm: bad matrix id %u\n", (unsigned) id);
        return;
    }
    s.dirty |= GX_DIRTY_MTX;
}

void GXSetCurrentMtx(u32 id)
{
    gx_check_pending("GXSetCurrentMtx");
    if (id > 27 || (id % 3)) re4_log_once("GXSetCurrentMtx", "GXSetCurrentMtx: bad id %u\n", (unsigned) id);
    gx_state.current_mtx = id;
}

// The SDK keeps the projection as 6 floats + type (GXSetProjection / GXGetProjectionv); the backend
// gets the full 4x4 the game built.
void GXSetProjection(const f32 mtx[4][4], int type)
{
    gx_check_pending("GXSetProjection");
    GxState& s = gx_state;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) s.proj[r][c] = mtx[r][c];
    s.proj_type = (uint8_t) type;
    s.proj6[0] = mtx[0][0];
    s.proj6[2] = mtx[1][1];
    s.proj6[4] = mtx[2][2];
    s.proj6[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) { s.proj6[1] = mtx[0][3]; s.proj6[3] = mtx[1][3]; }
    else { s.proj6[1] = mtx[0][2]; s.proj6[3] = mtx[1][2]; }
    s.dirty |= GX_DIRTY_MTX;
}

void GXGetProjectionv(f32* p)
{
    const GxState& s = gx_state;
    p[0] = (s.proj_type != GX_PERSPECTIVE) ? 1.0f : 0.0f;
    for (int i = 0; i < 6; i++) p[1 + i] = s.proj6[i];
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field)
{
    gx_check_pending("GXSetViewport");
    GxState& s = gx_state;
    if (field == 0) top -= 0.5f;   // odd field: shift half a line (SDK)
    s.vp_left = left; s.vp_top = top; s.vp_wd = wd; s.vp_ht = ht; s.vp_nearz = nearz; s.vp_farz = farz;
    if (gx_backend) gx_backend->set_viewport(left, top, wd, ht, nearz, farz);
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)
{
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1);
}

void GXGetViewportv(f32* vp)
{
    const GxState& s = gx_state;
    vp[0] = s.vp_left; vp[1] = s.vp_top; vp[2] = s.vp_wd; vp[3] = s.vp_ht; vp[4] = s.vp_nearz; vp[5] = s.vp_farz;
}

// Straight port of GXProject (GXTransform.c): pm/vp are the GXGetProjectionv/GXGetViewportv arrays.
void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz)
{
    f32 ex = mtx[0][3] + ((mtx[0][2] * z) + ((mtx[0][0] * x) + (mtx[0][1] * y)));
    f32 ey = mtx[1][3] + ((mtx[1][2] * z) + ((mtx[1][0] * x) + (mtx[1][1] * y)));
    f32 ez = mtx[2][3] + ((mtx[2][2] * z) + ((mtx[2][0] * x) + (mtx[2][1] * y)));
    f32 xc, yc, zc, wc;
    if (pm[0] == 0.0f) {   // perspective
        xc = (ex * pm[1]) + (ez * pm[2]);
        yc = (ey * pm[3]) + (ez * pm[4]);
        zc = pm[6] + (ez * pm[5]);
        wc = 1.0f / -ez;
    } else {               // orthographic
        xc = pm[2] + (ex * pm[1]);
        yc = pm[4] + (ey * pm[3]);
        zc = pm[6] + (ez * pm[5]);
        wc = 1.0f;
    }
    *sx = (vp[2] / 2.0f) + (vp[0] + (wc * (xc * vp[2] / 2.0f)));
    *sy = (vp[3] / 2.0f) + (vp[1] + (wc * (-yc * vp[3] / 2.0f)));
    *sz = vp[5] + (wc * (zc * (vp[5] - vp[4])));
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    gx_check_pending("GXSetScissor");
    GxState& s = gx_state;
    s.scissor_left = left; s.scissor_top = top; s.scissor_wd = wd; s.scissor_ht = ht;
    if (gx_backend) gx_backend->set_scissor(left, top, wd, ht);
}

// --- texgen, channels, lights (GXAttr.c, GXLight.c) ----------------------------------------------------

void GXSetTexCoordGen2(int dst_coord, int func, int src_param, u32 mtx, u8 normalize, u32 pt_texmtx)
{
    gx_check_pending("GXSetTexCoordGen2");
    if ((unsigned) dst_coord >= GX_MAX_TEXGENS_PORT) { re4_log_once("texgen_bad", "GXSetTexCoordGen: bad coord %d\n", dst_coord); return; }
    GxTexGen& g = gx_state.tev.texgen[dst_coord];
    g.func = (uint8_t) func;
    g.src = (uint8_t) src_param;
    g.normalize = normalize ? 1 : 0;
    g.mtx = (uint8_t) mtx;
    g.postmtx = (uint8_t) pt_texmtx;
    gx_state.dirty |= GX_DIRTY_TEXGEN;
}

void GXSetNumTexGens(u8 n)
{
    gx_check_pending("GXSetNumTexGens");
    gx_state.tev.num_texgens = n > 8 ? 8 : n;
    gx_state.dirty |= GX_DIRTY_TEXGEN;
}

void GXSetNumChans(u8 n)
{
    gx_check_pending("GXSetNumChans");
    gx_state.tev.num_chans = n > 2 ? 2 : n;
    gx_state.dirty |= GX_DIRTY_CHAN;
}

// GX_COLOR0A0 / GX_COLOR1A1 program both the colour and the alpha channel control.
void GXSetChanCtrl(int chan, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn)
{
    gx_check_pending("GXSetChanCtrl");
    int first, count;
    switch (chan) {
    case GX_COLOR0: case GX_COLOR1: case GX_ALPHA0: case GX_ALPHA1: first = chan; count = 1; break;
    case GX_COLOR0A0: first = GX_COLOR0; count = 2; break;
    case GX_COLOR1A1: first = GX_COLOR1; count = 2; break;
    default: re4_log_once("chanctrl_bad", "GXSetChanCtrl: bad channel %d\n", chan); return;
    }
    for (int i = 0; i < count; i++) {
        GxChanCtrl& c = gx_state.tev.chan[first + i * 2];
        c.enable = enable ? 1 : 0;
        c.amb_src = (uint8_t) amb_src;
        c.mat_src = (uint8_t) mat_src;
        c.diff_fn = (uint8_t) diff_fn;
        c.attn_fn = (uint8_t) attn_fn;
        c.light_mask = (uint8_t) (light_mask & 0xFF);
    }
    gx_state.dirty |= GX_DIRTY_CHAN;
}

// GX_COLORn replaces rgb only, GX_ALPHAn alpha only, GX_COLORnAn all four (GXLight.c).
static void set_chan_color(uint32_t* dst2, int chan, GXColor color, const char* who)
{
    uint32_t v = gxcolor_u32(color);
    switch (chan) {
    case GX_COLOR0: case GX_COLOR1: dst2[chan] = (dst2[chan] & 0xFF000000u) | (v & 0x00FFFFFFu); break;
    case GX_ALPHA0: case GX_ALPHA1: dst2[chan - GX_ALPHA0] = (dst2[chan - GX_ALPHA0] & 0x00FFFFFFu) | (v & 0xFF000000u); break;
    case GX_COLOR0A0: dst2[0] = v; break;
    case GX_COLOR1A1: dst2[1] = v; break;
    default: re4_log_once(who, "%s: bad channel %d\n", who, chan); return;
    }
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

void GXSetChanAmbColor(int chan, GXColor color)
{
    gx_check_pending("GXSetChanAmbColor");
    set_chan_color(gx_state.tev.amb_color, chan, color, "GXSetChanAmbColor");
}

void GXSetChanMatColor(int chan, GXColor color)
{
    gx_check_pending("GXSetChanMatColor");
    set_chan_color(gx_state.tev.mat_color, chan, color, "GXSetChanMatColor");
}

static inline GxLightObjInt* lobj(GXLightObj* o) { return (GxLightObjInt*) o; }

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
    GxLightObjInt* l = lobj(lt_obj);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2)
{
    GxLightObjInt* l = lobj(lt_obj);
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

// Angular attenuation polynomial for the spot function (GXLight.c GXInitLightSpot).
void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, int spot_func)
{
    GxLightObjInt* l = lobj(lt_obj);
    f32 a0, a1, a2, d;
    if (cutoff <= 0.0f || cutoff > 90.0f) spot_func = GX_SP_OFF;
    f32 r = (3.1415927f * cutoff) / 180.0f;
    f32 cr = cosf(r);
    switch (spot_func) {
    case GX_SP_FLAT:  a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;
    case GX_SP_COS:   a1 = 1.0f / (1.0f - cr); a0 = -cr * a1; a2 = 0.0f; break;
    case GX_SP_COS2:  a2 = 1.0f / (1.0f - cr); a0 = 0.0f; a1 = -cr * a2; break;
    case GX_SP_SHARP: d = 1.0f / ((1.0f - cr) * (1.0f - cr)); a0 = (cr * (cr - 2.0f)) * d; a1 = 2.0f * d; a2 = -d; break;
    case GX_SP_RING1: d = 1.0f / ((1.0f - cr) * (1.0f - cr)); a2 = -4.0f * d; a0 = a2 * cr; a1 = (4.0f * (1.0f + cr)) * d; break;
    case GX_SP_RING2: d = 1.0f / ((1.0f - cr) * (1.0f - cr)); a0 = 1.0f - ((2.0f * cr * cr) * d); a1 = (4.0f * cr) * d; a2 = -2.0f * d; break;
    case GX_SP_OFF: default: a0 = 1.0f; a1 = 0.0f; a2 = 0.0f; break;
    }
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_distance, f32 ref_brightness, int dist_func)
{
    GxLightObjInt* l = lobj(lt_obj);
    f32 k0, k1, k2;
    if (ref_distance < 0.0f) dist_func = GX_DA_OFF;
    if (ref_brightness <= 0.0f || ref_brightness >= 1.0f) dist_func = GX_DA_OFF;
    switch (dist_func) {
    case GX_DA_GENTLE: k0 = 1.0f; k1 = (1.0f - ref_brightness) / (ref_brightness * ref_distance); k2 = 0.0f; break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = (0.5f * (1.0f - ref_brightness)) / (ref_brightness * ref_distance);
        k2 = (0.5f * (1.0f - ref_brightness)) / (ref_brightness * ref_distance * ref_distance);
        break;
    case GX_DA_STEEP: k0 = 1.0f; k1 = 0.0f; k2 = (1.0f - ref_brightness) / (ref_brightness * ref_distance * ref_distance); break;
    case GX_DA_OFF: default: k0 = 1.0f; k1 = 0.0f; k2 = 0.0f; break;
    }
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z)
{
    GxLightObjInt* l = lobj(lt_obj);
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz)
{
    GxLightObjInt* l = lobj(lt_obj);
    l->dir[0] = -nx; l->dir[1] = -ny; l->dir[2] = -nz;   // the SDK stores the negated direction
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color)
{
    lobj(lt_obj)->color = gxcolor_u32(color);
}

void GXLoadLightObjImm(GXLightObj* lt_obj, u32 light)
{
    gx_check_pending("GXLoadLightObjImm");
    if (light == 0 || light > GX_LIGHT7) { re4_log_once("light_id", "GXLoadLightObjImm: bad light id 0x%x\n", (unsigned) light); return; }
    int idx = 0;
    while ((1u << idx) < light) idx++;        // GX_LIGHTn is a bit mask; the SDK takes the highest bit
    gx_state.tev.light[idx & 7] = *lobj(lt_obj);
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

// --- TEV (GXTev.c) ----------------------------------------------------------------------------------------

static inline int stage_ok(int stage, const char* who)
{
    if ((unsigned) stage < GX_MAX_TEV_STAGES_PORT) return 1;
    re4_log_once(who, "%s: bad stage %d\n", who, stage);
    return 0;
}

void GXSetNumTevStages(u8 n)
{
    gx_check_pending("GXSetNumTevStages");
    gx_state.tev.num_stages = n < 1 ? 1 : (n > 16 ? 16 : n);
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevOrder(int stage, int coord, int map, int color)
{
    gx_check_pending("GXSetTevOrder");
    if (!stage_ok(stage, "GXSetTevOrder")) return;
    GxTevStage& st = gx_state.tev.stage[stage];
    st.coord = (uint8_t) coord;
    st.map = (uint16_t) map;
    st.chan = (uint8_t) color;
    gx_state.dirty |= GX_DIRTY_TEV;
}

// GXSetTevOp: the SDK's TEVCOpTableST0/ST1 and TEVAOpTableST0/ST1 (GXTev.c). Stage 0 uses the
// rasterised colour where later stages use the previous stage's output. The op/bias/scale/clamp/out
// fields are reset to ADD/ZERO/1/clamp/PREV; the swap mode (low bits of the alpha register) survives.
void GXSetTevOp(int id, int mode)
{
    gx_check_pending("GXSetTevOp");
    if (!stage_ok(id, "GXSetTevOp")) return;
    if (mode < GX_MODULATE || mode > GX_PASSCLR) { re4_log_once("tevop_mode", "GXSetTevOp: bad mode %d\n", mode); return; }
    static const uint8_t c0[5][4] = {   // a, b, c, d
        { GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO },   // modulate
        { GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO },   // decal
        { GX_CC_RASC, GX_CC_ONE,  GX_CC_TEXC, GX_CC_ZERO },   // blend
        { GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC },   // replace
        { GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC },   // passclr
    };
    static const uint8_t c1[5][4] = {
        { GX_CC_ZERO,  GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO },
        { GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA,  GX_CC_ZERO },
        { GX_CC_CPREV, GX_CC_ONE,  GX_CC_TEXC,  GX_CC_ZERO },
        { GX_CC_ZERO,  GX_CC_ZERO, GX_CC_ZERO,  GX_CC_TEXC },
        { GX_CC_ZERO,  GX_CC_ZERO, GX_CC_ZERO,  GX_CC_CPREV },
    };
    static const uint8_t a0[5][4] = {
        { GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA },
        { GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA },
    };
    static const uint8_t a1[5][4] = {
        { GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,  GX_CA_APREV },
        { GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,  GX_CA_TEXA },
        { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,  GX_CA_APREV },
    };
    GxTevStage& st = gx_state.tev.stage[id];
    const uint8_t* ci = (id == GX_TEVSTAGE0) ? c0[mode] : c1[mode];
    const uint8_t* ai = (id == GX_TEVSTAGE0) ? a0[mode] : a1[mode];
    for (int i = 0; i < 4; i++) { st.color_in[i] = ci[i]; st.alpha_in[i] = ai[i]; }
    GxTevOpParams def = { GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, 1, GX_TEVPREV };
    st.color_op = def;
    st.alpha_op = def;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevColorIn(int stage, int a, int b, int c, int d)
{
    gx_check_pending("GXSetTevColorIn");
    if (!stage_ok(stage, "GXSetTevColorIn")) return;
    GxTevStage& st = gx_state.tev.stage[stage];
    st.color_in[0] = (uint8_t) a; st.color_in[1] = (uint8_t) b; st.color_in[2] = (uint8_t) c; st.color_in[3] = (uint8_t) d;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevAlphaIn(int stage, int a, int b, int c, int d)
{
    gx_check_pending("GXSetTevAlphaIn");
    if (!stage_ok(stage, "GXSetTevAlphaIn")) return;
    GxTevStage& st = gx_state.tev.stage[stage];
    st.alpha_in[0] = (uint8_t) a; st.alpha_in[1] = (uint8_t) b; st.alpha_in[2] = (uint8_t) c; st.alpha_in[3] = (uint8_t) d;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevColorOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg)
{
    gx_check_pending("GXSetTevColorOp");
    if (!stage_ok(stage, "GXSetTevColorOp")) return;
    GxTevOpParams& p = gx_state.tev.stage[stage].color_op;
    p.op = (uint8_t) op; p.bias = (uint8_t) bias; p.scale = (uint8_t) scale; p.clamp = clamp ? 1 : 0; p.out_reg = (uint8_t) out_reg;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevAlphaOp(int stage, int op, int bias, int scale, u8 clamp, int out_reg)
{
    gx_check_pending("GXSetTevAlphaOp");
    if (!stage_ok(stage, "GXSetTevAlphaOp")) return;
    GxTevOpParams& p = gx_state.tev.stage[stage].alpha_op;
    p.op = (uint8_t) op; p.bias = (uint8_t) bias; p.scale = (uint8_t) scale; p.clamp = clamp ? 1 : 0; p.out_reg = (uint8_t) out_reg;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevColor(int id, GXColor color)
{
    gx_check_pending("GXSetTevColor");
    if ((unsigned) id >= 4) { re4_log_once("tevcolor_id", "GXSetTevColor: bad reg %d\n", id); return; }
    int16_t* r = gx_state.tev.tevreg[id];
    r[0] = color.r; r[1] = color.g; r[2] = color.b; r[3] = color.a;
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

void GXSetTevColorS10(int id, GXColorS10 color)
{
    gx_check_pending("GXSetTevColorS10");
    if ((unsigned) id >= 4) { re4_log_once("tevcolors10_id", "GXSetTevColorS10: bad reg %d\n", id); return; }
    int16_t* r = gx_state.tev.tevreg[id];
    r[0] = color.r; r[1] = color.g; r[2] = color.b; r[3] = color.a;   // -1024..1023, as the hardware's 11-bit regs
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

void GXSetTevKColor(int id, GXColor color)
{
    gx_check_pending("GXSetTevKColor");
    if ((unsigned) id >= 4) { re4_log_once("kcolor_id", "GXSetTevKColor: bad id %d\n", id); return; }
    gx_state.tev.kcolor[id] = gxcolor_u32(color);
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

void GXSetTevKColorSel(int stage, int sel)
{
    gx_check_pending("GXSetTevKColorSel");
    if (!stage_ok(stage, "GXSetTevKColorSel")) return;
    gx_state.tev.stage[stage].kcsel = (uint8_t) sel;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevKAlphaSel(int stage, int sel)
{
    gx_check_pending("GXSetTevKAlphaSel");
    if (!stage_ok(stage, "GXSetTevKAlphaSel")) return;
    gx_state.tev.stage[stage].kasel = (uint8_t) sel;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevSwapMode(int stage, int ras_sel, int tex_sel)
{
    gx_check_pending("GXSetTevSwapMode");
    if (!stage_ok(stage, "GXSetTevSwapMode")) return;
    gx_state.tev.stage[stage].ras_swap = (uint8_t) ras_sel;
    gx_state.tev.stage[stage].tex_swap = (uint8_t) tex_sel;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevSwapModeTable(int table, int red, int green, int blue, int alpha)
{
    gx_check_pending("GXSetTevSwapModeTable");
    if ((unsigned) table >= 4) { re4_log_once("swaptable_id", "GXSetTevSwapModeTable: bad table %d\n", table); return; }
    uint8_t* t = gx_state.tev.swap_table[table];
    t[0] = (uint8_t) red; t[1] = (uint8_t) green; t[2] = (uint8_t) blue; t[3] = (uint8_t) alpha;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetAlphaCompare(int comp0, u8 ref0, int op, int comp1, u8 ref1)
{
    gx_check_pending("GXSetAlphaCompare");
    GxState& s = gx_state;
    s.alpha_comp0 = (uint8_t) comp0; s.alpha_ref0 = ref0; s.alpha_op = (uint8_t) op; s.alpha_comp1 = (uint8_t) comp1; s.alpha_ref1 = ref1;
    s.dirty |= GX_DIRTY_PIXEL;
}

// --- indirect texturing (GXBump.c) ----------------------------------------------------------------------

static void set_tev_indirect(int tev_stage, int ind_stage, int format, int bias_sel, int matrix_sel,
                             int wrap_s, int wrap_t, int add_prev, int utc_lod, int alpha_sel)
{
    if (!stage_ok(tev_stage, "GXSetTevIndirect")) return;
    GxTevIndirect& i = gx_state.tev.stage[tev_stage].ind;
    i.ind_stage = (uint8_t) ind_stage; i.format = (uint8_t) format; i.bias_sel = (uint8_t) bias_sel;
    i.mtx_sel = (uint8_t) matrix_sel; i.wrap_s = (uint8_t) wrap_s; i.wrap_t = (uint8_t) wrap_t;
    i.add_prev = add_prev ? 1 : 0; i.utc_lod = utc_lod ? 1 : 0; i.alpha_sel = (uint8_t) alpha_sel;
    gx_state.dirty |= GX_DIRTY_TEV;
}

void GXSetTevDirect(int tev_stage)
{
    gx_check_pending("GXSetTevDirect");
    set_tev_indirect(tev_stage, GX_INDTEXSTAGE0, GX_ITF_8, GX_ITB_NONE, GX_ITM_OFF, GX_ITW_OFF, GX_ITW_OFF, 0, 0, GX_ITBA_OFF);
}

void GXSetTevIndWarp(int tev_stage, int ind_stage, u8 signed_offset, u8 replace_mode, int matrix_sel)
{
    gx_check_pending("GXSetTevIndWarp");
    int wrap = replace_mode ? GX_ITW_0 : GX_ITW_OFF;
    set_tev_indirect(tev_stage, ind_stage, GX_ITF_8, signed_offset ? GX_ITB_STU : GX_ITB_NONE, matrix_sel, wrap, wrap, 0, 0, GX_ITBA_OFF);
}

void GXSetTevIndBumpXYZ(int tev_stage, int ind_stage, int matrix_sel)
{
    gx_check_pending("GXSetTevIndBumpXYZ");
    set_tev_indirect(tev_stage, ind_stage, GX_ITF_8, GX_ITB_STU, matrix_sel, GX_ITW_OFF, GX_ITW_OFF, 0, 0, GX_ITBA_OFF);
}

void GXSetNumIndStages(u8 nstages)
{
    gx_check_pending("GXSetNumIndStages");
    gx_state.tev.num_ind_stages = nstages > 4 ? 4 : nstages;
    gx_state.dirty |= GX_DIRTY_IND;
}

void GXSetIndTexOrder(int ind_stage, int tex_coord, int tex_map)
{
    gx_check_pending("GXSetIndTexOrder");
    if ((unsigned) ind_stage >= 4) { re4_log_once("indorder_id", "GXSetIndTexOrder: bad stage %d\n", ind_stage); return; }
    if (tex_map == GX_TEXMAP_NULL) tex_map = GX_TEXMAP0;       // SDK
    if (tex_coord == GX_TEXCOORD_NULL) tex_coord = GX_TEXCOORD0;
    gx_state.tev.ind[ind_stage].coord = (uint8_t) (tex_coord & 7);
    gx_state.tev.ind[ind_stage].map = (uint8_t) (tex_map & 7);
    gx_state.dirty |= GX_DIRTY_IND;
}

void GXSetIndTexCoordScale(int ind_stage, int scale_s, int scale_t)
{
    gx_check_pending("GXSetIndTexCoordScale");
    if ((unsigned) ind_stage >= 4) { re4_log_once("indscale_id", "GXSetIndTexCoordScale: bad stage %d\n", ind_stage); return; }
    gx_state.tev.ind[ind_stage].scale_s = (uint8_t) scale_s;
    gx_state.tev.ind[ind_stage].scale_t = (uint8_t) scale_t;
    gx_state.dirty |= GX_DIRTY_IND;
}

// GX_ITM_0..2, GX_ITM_S0..S2 and GX_ITM_T0..T2 all address the same three matrices (GXBump.c).
void GXSetIndTexMtx(int mtx_id, const f32 offset[2][3], s8 scale_exp)
{
    gx_check_pending("GXSetIndTexMtx");
    int id;
    switch (mtx_id) {
    case GX_ITM_0: case GX_ITM_1: case GX_ITM_2:    id = mtx_id - GX_ITM_0; break;
    case GX_ITM_S0: case GX_ITM_S1: case GX_ITM_S2: id = mtx_id - GX_ITM_S0; break;
    case GX_ITM_T0: case GX_ITM_T1: case GX_ITM_T2: id = mtx_id - GX_ITM_T0; break;
    default: id = 0; break;
    }
    for (int r = 0; r < 2; r++) for (int c = 0; c < 3; c++) gx_state.tev.ind_mtx[id][r][c] = offset[r][c];
    gx_state.tev.ind_mtx_scale[id] = scale_exp;
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

void __GXSetIndirectMask(u32 mask)
{
    gx_state.tev.ind_mask = mask;
    gx_state.dirty |= GX_DIRTY_UNIFORMS;
}

// --- pixel pipeline (GXPixel.c, GXGeometry.c) -----------------------------------------------------------

void GXSetBlendMode(int type, int src_factor, int dst_factor, int op)
{
    gx_check_pending("GXSetBlendMode");
    GxState& s = gx_state;
    s.blend_type = (uint8_t) type; s.blend_src = (uint8_t) src_factor; s.blend_dst = (uint8_t) dst_factor; s.blend_op = (uint8_t) op;
    s.dirty |= GX_DIRTY_PIXEL;
}

void GXSetColorUpdate(u8 update_enable) { gx_check_pending("GXSetColorUpdate"); gx_state.color_update = update_enable ? 1 : 0; gx_state.dirty |= GX_DIRTY_PIXEL; }
void GXSetAlphaUpdate(u8 update_enable) { gx_check_pending("GXSetAlphaUpdate"); gx_state.alpha_update = update_enable ? 1 : 0; gx_state.dirty |= GX_DIRTY_PIXEL; }
void GXSetCullMode(int mode) { gx_check_pending("GXSetCullMode"); gx_state.cull_mode = (uint8_t) mode; gx_state.dirty |= GX_DIRTY_PIXEL; }

void GXSetZMode(u8 compare_enable, int func, u8 update_enable)
{
    gx_check_pending("GXSetZMode");
    GxState& s = gx_state;
    s.z_enable = compare_enable ? 1 : 0; s.z_func = (uint8_t) func; s.z_update = update_enable ? 1 : 0;
    s.dirty |= GX_DIRTY_PIXEL;
}

void GXSetZCompLoc(u8 before_tex) { gx_check_pending("GXSetZCompLoc"); gx_state.z_comp_loc = before_tex ? 1 : 0; gx_state.dirty |= GX_DIRTY_PIXEL; }
void GXSetDither(u8 dither) { gx_check_pending("GXSetDither"); gx_state.dither = dither ? 1 : 0; gx_state.dirty |= GX_DIRTY_PIXEL; }

void GXSetDstAlpha(u8 enable, u8 alpha)
{
    gx_check_pending("GXSetDstAlpha");
    gx_state.dst_alpha_enable = enable ? 1 : 0; gx_state.dst_alpha = alpha;
    gx_state.dirty |= GX_DIRTY_PIXEL;
}

void GXSetPixelFmt(int pix_fmt, int z_fmt)
{
    gx_check_pending("GXSetPixelFmt");
    gx_state.pixel_fmt = (uint8_t) pix_fmt; gx_state.z_fmt = (uint8_t) z_fmt;
    gx_state.dirty |= GX_DIRTY_PIXEL;
}

void GXSetFog(int type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color)
{
    gx_check_pending("GXSetFog");
    GxState& s = gx_state;
    s.fog_type = (uint8_t) type; s.fog_start = startz; s.fog_end = endz; s.fog_near = nearz; s.fog_far = farz;
    s.fog_color = gxcolor_u32(color);
    s.dirty |= GX_DIRTY_PIXEL | GX_DIRTY_UNIFORMS;
}

void GXSetLineWidth(u8 width, int tex_offsets)
{
    gx_check_pending("GXSetLineWidth");
    gx_state.line_width = width; gx_state.line_tex_offsets = (uint8_t) tex_offsets;
    gx_state.dirty |= GX_DIRTY_PIXEL;
}

void GXEnableTexOffsets(int coord, u8 line_enable, u8 point_enable)
{
    gx_check_pending("GXEnableTexOffsets");
    if ((unsigned) coord >= 8) return;
    gx_state.tex_offset_line[coord] = line_enable ? 1 : 0;
    gx_state.tex_offset_point[coord] = point_enable ? 1 : 0;
}

// --- EFB copies and sync (GXFrameBuf.c, GXMisc.c) --------------------------------------------------------

void GXSetCopyClear(GXColor clear_clr, u32 clear_z)
{
    gx_check_pending("GXSetCopyClear");
    gx_state.copy_clear_color = gxcolor_u32(clear_clr);
    gx_state.copy_clear_z = clear_z & 0xFFFFFF;
}

void GXSetCopyFilter(u8 aa, const u8 sample_pattern[12][2], u8 vf, const u8 vfilter[7])
{
    gx_check_pending("GXSetCopyFilter");
    GxState& s = gx_state;
    s.copy_filter_aa = aa; s.copy_filter_vf = vf;
    if (sample_pattern) memcpy(s.copy_sample_pattern, sample_pattern, sizeof s.copy_sample_pattern);
    if (vfilter) memcpy(s.copy_vfilter, vfilter, sizeof s.copy_vfilter);
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    gx_check_pending("GXSetTexCopySrc");
    GxCopyRect& r = gx_state.tex_copy_src;
    r.left = left; r.top = top; r.wd = wd; r.ht = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, int fmt, u8 mipmap)
{
    gx_check_pending("GXSetTexCopyDst");
    gx_state.tex_copy_dst_wd = wd; gx_state.tex_copy_dst_ht = ht;
    gx_state.tex_copy_dst_fmt = (uint8_t) fmt; gx_state.tex_copy_dst_mipmap = mipmap ? 1 : 0;
}

// Resolve the EFB rectangle into the texture the game will later bind through GXInitTexObj(dest ...).
void GXCopyTex(void* dest, u8 clear)
{
    gx_check_pending("GXCopyTex");
    gx_emit_flush();
    GxState& s = gx_state;
    if (!gx_backend) return;
    gx_ensure_frame_open();
    GxTexHandle h = texcache_render_target(dest, s.tex_copy_dst_wd, s.tex_copy_dst_ht, s.tex_copy_dst_fmt);
    // clear != 0: GX clears the copied EFB region with the copy-clear colour/Z during the copy. The
    // flag tells the backend the source is about to be cleared; the clear itself follows as a clear()
    // with the GXSetCopyClear values (a backend may restrict it to the copied rectangle).
    gx_backend->copy_to_texture(h, s.tex_copy_src.left, s.tex_copy_src.top, s.tex_copy_src.wd, s.tex_copy_src.ht, clear);
    if (clear) gx_backend->clear(s.copy_clear_color, s.copy_clear_z, 1, 1);
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    gx_check_pending("GXSetDispCopySrc");
    GxCopyRect& r = gx_state.disp_copy_src;
    r.left = left; r.top = top; r.wd = wd; r.ht = ht;
}

void GXSetDispCopyDst(u16 wd, u16 ht)
{
    gx_check_pending("GXSetDispCopyDst");
    gx_state.disp_copy_dst_wd = wd; gx_state.disp_copy_dst_ht = ht;
}

// Returns the number of XFB lines the copy produces (__GXGetNumXfbLines).
u32 GXSetDispCopyYScale(f32 yscale)
{
    gx_check_pending("GXSetDispCopyYScale");
    if (yscale < 1.0f) yscale = 1.0f;
    gx_state.disp_copy_yscale = yscale;
    u32 iScale = (u32) (256.0f / yscale) & 0x1FF;
    u32 efbHt = gx_state.disp_copy_src.ht;
    if (efbHt == 0) return 0;
    u32 realHt = ((efbHt - 1) * 0x100) / iScale + 1;
    u32 iScaleD = iScale;
    if (iScaleD > 0x80 && iScaleD < 0x100) {
        while (iScaleD % 2 == 0) iScaleD /= 2;
        if (efbHt % iScaleD == 0) realHt++;
    }
    if (realHt > 0x400) realHt = 0x400;
    return realHt;
}

void GXSetDispCopyGamma(int gamma) { gx_check_pending("GXSetDispCopyGamma"); gx_state.disp_copy_gamma = (uint8_t) gamma; }

// EFB -> XFB copy: the end of the frame. The optional clear applies to the next frame's first draw.
void GXCopyDisp(void* dest, u8 clear)
{
    gx_check_pending("GXCopyDisp");
    gx_emit_flush();
    GxState& s = gx_state;
    (void) dest;   // the XFB address: the backend presents its own back buffer
    if (!gx_backend) return;
    gx_ensure_frame_open();
    gx_backend->end_frame();
    s.frame_open = 0;
    s.pending_clear = clear ? 1 : 0;
}

void GXDrawDone(void)
{
    gx_check_pending("GXDrawDone");
    gx_emit_flush();
}

void GXPixModeSync(void)
{
    // Orders pixel-engine state changes after pending draws; the port draws synchronously.
    gx_check_pending("GXPixModeSync");
    gx_emit_flush();
}

void GXPeekZ(u16 x, u16 y, u32* z)
{
    gx_check_pending("GXPeekZ");
    gx_emit_flush();
    *z = gx_backend ? gx_backend->peek_z(x, y) : 0xFFFFFF;
}

void GXClearBoundingBox(void) { }   // bounding-box readback is never used by the game

// --- textures (GXTexture.c) ----------------------------------------------------------------------------------

static inline GxTexObjInt* tobj(GXTexObj* o) { return (GxTexObjInt*) o; }

static int floor_log2(unsigned v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }

void GXInitTexObj(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap)
{
    GxTexObjInt* t = tobj(obj);
    memset(t, 0, sizeof *t);
    t->image = image;
    t->width = width;
    t->height = height;
    t->format = (uint8_t) format;
    t->wrap_s = (uint8_t) wrap_s;
    t->wrap_t = (uint8_t) wrap_t;
    t->mipmap = mipmap ? 1 : 0;
    t->mag_filt = GX_LINEAR;              // mode0 bit 4 = 1
    int fmt4 = format & 0xF;
    int ci = (fmt4 == GX_TF_C4 || fmt4 == GX_TF_C8 || fmt4 == GX_TF_C14X2);
    if (mipmap) {
        t->min_filt = ci ? GX_LIN_MIP_NEAR : GX_LIN_MIP_LIN;   // SDK: hw filter 5 for CI, 6 otherwise
        t->max_lod16 = (uint8_t) (16 * floor_log2(width > height ? width : height));
    } else {
        t->min_filt = GX_LINEAR;                                 // hw filter 4
        t->max_lod16 = 0;
    }
    t->min_lod16 = 0;
    t->lod_bias = 0.0f;
    t->max_aniso = GX_ANISO_1;
    t->bias_clamp = 0;
    t->edge_lod = 1;        // mode0 bit 8 == 0 means "edge LOD enabled"
    t->magic = GX_TEXOBJ_MAGIC;
}

void GXInitTexObjCI(GXTexObj* obj, void* image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap, u32 tlut_name)
{
    GXInitTexObj(obj, image, width, height, format, wrap_s, wrap_t, mipmap);
    tobj(obj)->is_ci = 1;
    tobj(obj)->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, int min_filt, int mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, u8 bias_clamp, u8 do_edge_lod, int max_aniso)
{
    GxTexObjInt* t = tobj(obj);
    if (t->magic != GX_TEXOBJ_MAGIC) re4_log_once("texobjlod_uninit", "GXInitTexObjLOD on an uninitialised GXTexObj\n");
    if (lod_bias < -4.0f) lod_bias = -4.0f; else if (lod_bias >= 4.0f) lod_bias = 3.99f;
    if (min_lod < 0.0f) min_lod = 0.0f; else if (min_lod > 10.0f) min_lod = 10.0f;
    if (max_lod < 0.0f) max_lod = 0.0f; else if (max_lod > 10.0f) max_lod = 10.0f;
    t->lod_bias = lod_bias;
    t->min_filt = (uint8_t) min_filt;
    t->mag_filt = (uint8_t) (mag_filt == GX_LINEAR ? GX_LINEAR : GX_NEAR);
    t->min_lod16 = (uint8_t) (16.0f * min_lod);
    t->max_lod16 = (uint8_t) (16.0f * max_lod);
    t->bias_clamp = bias_clamp ? 1 : 0;
    t->edge_lod = do_edge_lod ? 1 : 0;
    t->max_aniso = (uint8_t) max_aniso;
}

void* GXGetTexObjData(GXTexObj* obj)
{
    // NOTE: the SDK returns the physical address and the game adds 0x80000000 (Espgen42.cpp,
    // espgen45.cpp); the port returns the pointer itself, so those two call sites need an XBOX: edit.
    return (void*) tobj(obj)->image;
}

void GXLoadTexObj(GXTexObj* obj, int id)
{
    gx_check_pending("GXLoadTexObj");
    if ((unsigned) id >= GX_MAX_TEXMAPS_PORT) { re4_log_once("loadtexobj_id", "GXLoadTexObj: bad map %d\n", id); return; }
    GxTexObjInt* t = tobj(obj);
    if (t->magic != GX_TEXOBJ_MAGIC) re4_log_once("loadtexobj_uninit", "GXLoadTexObj: GXTexObj not initialised (map %d)\n", id);
    gx_state.texmap[id] = *t;
    gx_state.texmap_valid[id] = 1;
    gx_state.dirty |= GX_DIRTY_TEX;
}

void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, int fmt, u16 n_entries)
{
    GxTlutObjInt* t = (GxTlutObjInt*) tlut_obj;
    memset(t, 0, sizeof *t);
    t->lut = lut;
    t->fmt = (uint8_t) fmt;
    t->n_entries = n_entries;
    t->valid = 1;
}

void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name)
{
    gx_check_pending("GXLoadTlut");
    if (tlut_name >= GX_NUM_TLUTS_PORT) { re4_log_once("loadtlut_id", "GXLoadTlut: bad tlut %u\n", (unsigned) tlut_name); return; }
    gx_state.tlut[tlut_name] = *(GxTlutObjInt*) tlut_obj;
    gx_state.dirty |= GX_DIRTY_TEX;   // CI textures pre-expanded offline; the thermal palette swap is a backend concern
}

void GXInvalidateTexAll(void)
{
    gx_check_pending("GXInvalidateTexAll");
    gx_state.tex_generation++;         // raw (game-written) images are re-uploaded at their next bind
    gx_state.dirty |= GX_DIRTY_TEX;
}

// Size in bytes of a GX texture image (GXTexture.c): tiles of 32 or 64 bytes, per mip level.
u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod)
{
    u32 shx, shy;
    switch (format) {
    case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
        shx = 3; shy = 3; break;
    case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: case GX_TF_Z8: case GX_CTF_RA4: case GX_TF_A8:
    case GX_CTF_R8: case GX_CTF_G8: case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        shx = 3; shy = 2; break;
    case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case GX_TF_C14X2:
    case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_RA8: case GX_CTF_RG8: case GX_CTF_GB8: case GX_CTF_Z16L:
        shx = 2; shy = 2; break;
    default:
        re4_log_once("texbufsize_fmt", "GXGetTexBufferSize: invalid format %u\n", (unsigned) format);
        shx = shy = 0; break;
    }
    u32 tileBytes = (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) ? 64 : 32;
    u32 size = 0;
    if (mipmap) {
        u32 w = width, h = height;
        for (u32 level = 0; level < max_lod; level++) {
            u32 nx = (w + (1u << shx) - 1) >> shx;
            u32 ny = (h + (1u << shy) - 1) >> shy;
            size += tileBytes * nx * ny;
            if (w == 1 && h == 1) break;
            w = w > 1 ? w >> 1 : 1;
            h = h > 1 ? h >> 1 : 1;
        }
    } else {
        u32 nx = (width + (1u << shx) - 1) >> shx;
        u32 ny = (height + (1u << shy) - 1) >> shy;
        size = nx * ny * tileBytes;
    }
    return size;
}

} // extern "C"
