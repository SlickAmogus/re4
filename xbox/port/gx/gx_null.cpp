// gx_null.cpp — backend that draws nothing and records what it was asked (tests, early bring-up).
// Every vtable call appends a GxNullRecord (re4port/gx_null_log.h); draw() also keeps the vertices so
// tests can check what the interpreter decoded. The first draw is still logged through re4_log_once.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"
#include "re4port/gx_null_log.h"
#include "gx_vertex.h"
#include <string.h>

const GxBackend* gx_backend = 0;

enum { NULL_LOG_RING = 4096 };

static GxNullRecord  log_ring[NULL_LOG_RING];
static size_t        log_count;        // total records since reset (may exceed the ring)
static GxVertexBuffer log_vertices;
static size_t        log_tri_vertices;
static int           last_proj_type = -1;
static float         last_proj[4][4];
static float         last_pos[GX_PNMTX_COUNT][3][4];
static float         last_tex[GX_TEXMTX_COUNT][3][4];
static GxPipelineKey last_key;
static int           have_key;

static GxNullRecord* rec(int kind)
{
    GxNullRecord* r = &log_ring[log_count % NULL_LOG_RING];
    log_count++;
    memset(r, 0, sizeof *r);
    r->kind = (uint8_t) kind;
    return r;
}

extern "C" {

void gx_null_log_reset(void)
{
    log_count = 0;
    gx_vb_clear(&log_vertices);
    log_tri_vertices = 0;
    last_proj_type = -1;
    have_key = 0;
}

size_t gx_null_log_count(void) { return log_count < (size_t) NULL_LOG_RING ? log_count : (size_t) NULL_LOG_RING; }

const GxNullRecord* gx_null_log_get(size_t i)
{
    if (i >= gx_null_log_count()) return 0;
    size_t first = log_count > NULL_LOG_RING ? log_count - NULL_LOG_RING : 0;
    return &log_ring[(first + i) % NULL_LOG_RING];
}

size_t gx_null_log_count_kind(int kind)
{
    size_t n = 0;
    for (size_t i = 0; i < gx_null_log_count(); i++) if (gx_null_log_get(i)->kind == kind) n++;
    return n;
}

const GxNullRecord* gx_null_log_last_kind(int kind)
{
    for (size_t i = gx_null_log_count(); i > 0; i--) {
        const GxNullRecord* r = gx_null_log_get(i - 1);
        if (r->kind == kind) return r;
    }
    return 0;
}

const GxVertex* gx_null_vertices(size_t* count) { *count = log_vertices.count; return log_vertices.data; }
size_t gx_null_triangles(void) { return log_tri_vertices / 3; }
int gx_null_last_proj_type(void) { return last_proj_type; }
const float (*gx_null_last_proj(void))[4] { return last_proj; }
const float (*gx_null_last_pos_mtx(int slot))[4] { return last_pos[slot < 0 ? 0 : slot % GX_PNMTX_COUNT]; }
const float (*gx_null_last_tex_mtx(int slot))[4] { return last_tex[slot < 0 ? 0 : slot % GX_TEXMTX_COUNT]; }
const GxPipelineKey* gx_null_last_pipeline(void) { return have_key ? &last_key : 0; }

} // extern "C"

static int null_init(int w, int h)
{
    re4_log("gx_null: init %dx%d\n", w, h);
    GxNullRecord* r = rec(GX_NULL_INIT);
    r->f[0] = (float) w; r->f[1] = (float) h;
    return 0;
}
static void null_shutdown(void) { rec(GX_NULL_SHUTDOWN); }
static void null_begin_frame(void) { rec(GX_NULL_BEGIN_FRAME); }
static void null_end_frame(void) { rec(GX_NULL_END_FRAME); }

static void null_set_viewport(float l, float t, float w, float h, float nz, float fz)
{
    GxNullRecord* r = rec(GX_NULL_VIEWPORT);
    r->f[0] = l; r->f[1] = t; r->f[2] = w; r->f[3] = h; r->f[4] = nz; r->f[5] = fz;
}

static void null_set_scissor(uint32_t l, uint32_t t, uint32_t w, uint32_t h)
{
    GxNullRecord* r = rec(GX_NULL_SCISSOR);
    r->f[0] = (float) l; r->f[1] = (float) t; r->f[2] = (float) w; r->f[3] = (float) h;
}

static void null_set_pipeline(const GxPipelineKey* key)
{
    GxNullRecord* r = rec(GX_NULL_PIPELINE);
    r->hash = key->hash;
    r->count = key->num_tev_stages;
    last_key = *key;
    have_key = 1;
}

static void null_set_matrices(const float proj[4][4], int proj_type, const float (*pos)[3][4], const float (*nrm)[3][4], const float (*tex)[3][4])
{
    (void) nrm;
    rec(GX_NULL_MATRICES);
    last_proj_type = proj_type;
    memcpy(last_proj, proj, sizeof last_proj);
    memcpy(last_pos, pos, sizeof last_pos);
    memcpy(last_tex, tex, sizeof last_tex);
}

static void null_draw(int prim, const GxVertex* v, size_t n)
{
    re4_log_once("null_draw", "gx_null: draw prim 0x%02x x%u (first only)\n", prim, (unsigned) n);
    GxNullRecord* r = rec(GX_NULL_DRAW);
    r->prim = (uint8_t) prim;
    r->count = (uint32_t) n;
    if (prim == GX_PRIM_TRIANGLES) log_tri_vertices += n;
    if (gx_vb_reserve(&log_vertices, log_vertices.count + n)) {
        memcpy(log_vertices.data + log_vertices.count, v, n * sizeof(GxVertex));
        log_vertices.count += n;
    }
}

static GxTexHandle null_create_texture(const ConvertedImage* img)
{
    static GxTexHandle next = 1;
    GxNullRecord* r = rec(GX_NULL_CREATE_TEX);
    r->count = next;
    r->f[0] = img ? (float) img->width : 0.0f;
    r->f[1] = img ? (float) img->height : 0.0f;
    r->map = img ? (int) img->data_size : -1;
    return next++;
}

static void null_destroy_texture(GxTexHandle h) { rec(GX_NULL_DESTROY_TEX)->count = h; }

static void null_bind_texture(int map, GxTexHandle h, int wrap_s, int wrap_t, int min_filt, int mag_filt, float lod_bias, int aniso)
{
    GxNullRecord* r = rec(GX_NULL_BIND_TEX);
    r->map = map;
    r->count = h;
    r->f[0] = (float) wrap_s; r->f[1] = (float) wrap_t; r->f[2] = (float) min_filt; r->f[3] = (float) mag_filt;
    r->f[4] = lod_bias; r->f[5] = (float) aniso;
}

static GxRtHandle null_create_rt(int w, int h, int with_depth)
{
    static GxRtHandle next = 1;
    GxNullRecord* r = rec(GX_NULL_CREATE_RT);
    r->count = next; r->f[0] = (float) w; r->f[1] = (float) h; r->map = with_depth;
    return next++;
}

static void null_set_rt(GxRtHandle rt) { rec(GX_NULL_SET_RT)->count = rt; }

static void null_copy_to_texture(GxTexHandle dst, int left, int top, int w, int h, int clear)
{
    GxNullRecord* r = rec(GX_NULL_COPY_TO_TEX);
    r->count = dst; r->map = clear;
    r->f[0] = (float) left; r->f[1] = (float) top; r->f[2] = (float) w; r->f[3] = (float) h;
}

static void null_clear(uint32_t rgba, uint32_t z, int color, int depth)
{
    GxNullRecord* r = rec(GX_NULL_CLEAR);
    r->count = rgba; r->hash = z; r->map = (color ? 1 : 0) | (depth ? 2 : 0);
}

static uint32_t null_peek_z(int x, int y)
{
    GxNullRecord* r = rec(GX_NULL_PEEK_Z);
    r->map = x; r->count = (uint32_t) y;
    return 0xFFFFFF;
}

const GxBackend gx_null_backend = {
    "null",
    null_init, null_shutdown, null_begin_frame, null_end_frame,
    null_set_viewport, null_set_scissor, null_set_pipeline, null_set_matrices, null_draw,
    null_create_texture, null_destroy_texture, null_bind_texture,
    null_create_rt, null_set_rt, null_copy_to_texture, null_clear, null_peek_z,
};
