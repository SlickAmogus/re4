// gx_test.cpp — headless tests of the GX state machine, FIFO and display-list interpreter against the
// null backend (CT-08). Plain C++, no framework: EXPECT* macros count failures, main() returns 1 if any.
//
//   make -C xbox/port/tests gx && ./xbox/port/tests/gx
//
// The game's include/gx.h is included on purpose: it is the interface the port implements (plain-int
// enums, inline FIFO writers redirected by re4port/gx_fifo.h through the forced re4_xbox.h include).
#include "re4port/re4_xbox.h"
#include "gx.h"
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXDispList.h>   // GXBeginDisplayList/GXEndDisplayList (dbmodule.cpp includes this one)
#include "re4port/gx_shim.h"
#include "re4port/gx_null_log.h"
#include "gx_state.h"
#include "gx_pipeline.h"
#include "gx_vertex.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#ifdef GX_TEST_HAVE_TEV_CASES
#include "gx_test_cases.h"
#endif

static int g_pass, g_fail;
static const char* g_test = "";

#define EXPECT(cond) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_test, #cond); } } while (0)
#define EXPECT_EQ(a, b) do { long long _a = (long long) (a), _b = (long long) (b); if (_a == _b) g_pass++; else { g_fail++; \
    printf("  FAIL %s:%d [%s]: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, g_test, #a, #b, _a, _b); } } while (0)
#define EXPECT_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (fabs(_a - _b) <= (eps)) g_pass++; else { g_fail++; \
    printf("  FAIL %s:%d [%s]: %s ~= %s (%g vs %g)\n", __FILE__, __LINE__, g_test, #a, #b, _a, _b); } } while (0)
#define RUN(fn) do { g_test = #fn; int before = g_fail; reset(); fn(); printf("%-40s %s\n", #fn, g_fail == before ? "ok" : "FAILED"); } while (0)

// --- helpers --------------------------------------------------------------------------------------------------

static void reset(void)
{
    gx_init();
    gx_null_log_reset();
}

// Byte-stream builder in either byte order (display lists and the arrays they index).
struct Buf {
    uint8_t data[8192];
    size_t  n;
    int     be;
    explicit Buf(int big_endian = 0) : n(0), be(big_endian) { memset(data, 0, sizeof data); }
    void u8(unsigned v) { data[n++] = (uint8_t) v; }
    void s8(int v) { data[n++] = (uint8_t) (int8_t) v; }
    void u16(unsigned v) { if (be) { u8(v >> 8); u8(v); } else { u8(v); u8(v >> 8); } }
    void s16(int v) { u16((unsigned) (uint16_t) (int16_t) v); }
    void u32(uint32_t v) { if (be) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); } else { u8(v); u8(v >> 8); u8(v >> 16); u8(v >> 24); } }
    void f32(float f) { uint32_t u; memcpy(&u, &f, 4); u32(u); }
    void prim(int op, int fmt, unsigned count) { u8(op | fmt); u16(count); }
};

static void draw_stream(const Buf& b) { gx_run_stream(b.data, b.n, b.be); gx_flush(); }

static const GxVertex* verts(size_t expect)
{
    size_t n = 0;
    const GxVertex* v = gx_null_vertices(&n);
    EXPECT_EQ(n, expect);
    return n >= expect ? v : 0;
}

static void vcd_pos_f32_direct(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
}

static void draw_quad_imm(void)
{
    vcd_pos_f32_direct();
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(0, 0, 0); GXPosition3f32(1, 0, 0); GXPosition3f32(1, 1, 0); GXPosition3f32(0, 1, 0);
    gx_flush();
}

static uint64_t last_hash(void)
{
    const GxNullRecord* r = gx_null_log_last_kind(GX_NULL_PIPELINE);
    return r ? r->hash : 0;
}

// --- tests: display lists ---------------------------------------------------------------------------------------

static const float kQuadPos[4][3] = { {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0} };
static const float kQuadUV[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
static const int kQuadIdx[4] = { 3, 2, 1, 0 };   // reversed, so indexing is actually exercised

static void quad_idx16(int be)
{
    Buf pos(be), uv(be), dl(be);
    for (int i = 0; i < 4; i++) { pos.f32(kQuadPos[i][0]); pos.f32(kQuadPos[i][1]); pos.f32(kQuadPos[i][2]); }
    for (int i = 0; i < 4; i++) { uv.f32(kQuadUV[i][0]); uv.f32(kQuadUV[i][1]); }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetArray(GX_VA_POS, pos.data, 12);
    GXSetArray(GX_VA_TEX0, uv.data, 8);
    dl.prim(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) { dl.u16(kQuadIdx[i]); dl.u16(kQuadIdx[i]); }
    while (dl.n & 31) dl.u8(0);   // 32-byte padding with NOPs like real lists
    draw_stream(dl);

    EXPECT_EQ(gx_null_triangles(), 2);
    const GxVertex* v = verts(6);
    if (!v) return;
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        const float* p = kQuadPos[kQuadIdx[tri[i]]];
        const float* t = kQuadUV[kQuadIdx[tri[i]]];
        EXPECT_NEAR(v[i].pos[0], p[0], 1e-6); EXPECT_NEAR(v[i].pos[1], p[1], 1e-6); EXPECT_NEAR(v[i].pos[2], p[2], 1e-6);
        EXPECT_NEAR(v[i].uv[0][0], t[0], 1e-6); EXPECT_NEAR(v[i].uv[0][1], t[1], 1e-6);
        EXPECT_EQ(v[i].col[0], 0xFFFFFFFFu);   // no colour in the VCD: white
        EXPECT_EQ(v[i].mtxidx, 0);
        EXPECT_EQ(v[i].texmtxidx[0], 10);     // default texgen matrix is GX_IDENTITY
    }
}

static void test_quad_idx16_le(void) { quad_idx16(0); }

static void test_quad_idx16_be(void)
{
    quad_idx16(1);
    // and through GXCallDisplayList with the global flag
    gx_null_log_reset();
    Buf pos(1), dl(1);
    for (int i = 0; i < 4; i++) { pos.f32(kQuadPos[i][0]); pos.f32(kQuadPos[i][1]); pos.f32(kQuadPos[i][2]); }
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetArray(GX_VA_POS, pos.data, 12);
    dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    dl.u8(0); dl.u8(1); dl.u8(2);
    while (dl.n & 31) dl.u8(0);
    gx_set_dl_big_endian(1);
    GXCallDisplayList(dl.data, (u32) dl.n);
    gx_flush();
    gx_set_dl_big_endian(0);
    EXPECT_EQ(gx_null_triangles(), 1);
    const GxVertex* v = verts(3);
    if (v) { EXPECT_NEAR(v[1].pos[0], 1.0f, 1e-6); EXPECT_NEAR(v[2].pos[1], 1.0f, 1e-6); }
}

static void test_strip_with_pnmtxidx(void)
{
    float m0[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    float m1[3][4] = { {1, 0, 0, 10}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    GXLoadPosMtxImm(m0, GX_PNMTX0);
    GXLoadPosMtxImm(m1, GX_PNMTX1);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    Buf dl;
    dl.prim(GX_TRIANGLESTRIP, GX_VTXFMT2, 4);
    const int idx[4] = { GX_PNMTX0, GX_PNMTX1, GX_PNMTX1, GX_PNMTX0 };
    for (int i = 0; i < 4; i++) { dl.u8(idx[i]); dl.f32((float) i); dl.f32(0); dl.f32(0); }
    draw_stream(dl);
    EXPECT_EQ(gx_null_triangles(), 2);
    const GxVertex* v = verts(6);
    if (!v) return;
    // strip (0,1,2) then (2,1,3) -> emitted as (0,1,2),(2,1,3) with the same winding
    EXPECT_EQ(v[0].mtxidx, 0); EXPECT_EQ(v[1].mtxidx, 1); EXPECT_EQ(v[2].mtxidx, 1);
    EXPECT_NEAR(v[3].pos[0], 2.0f, 1e-6); EXPECT_NEAR(v[4].pos[0], 1.0f, 1e-6); EXPECT_NEAR(v[5].pos[0], 3.0f, 1e-6);
    EXPECT_EQ(v[5].mtxidx, 0);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_MATRICES), 1);
    EXPECT_NEAR(gx_null_last_pos_mtx(1)[0][3], 10.0f, 1e-6);
    EXPECT_NEAR(gx_null_last_pos_mtx(0)[0][3], 0.0f, 1e-6);
}

static void test_s16_pos_frac4(void)
{
    for (int be = 0; be < 2; be++) {
        gx_null_log_reset();
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 4);
        Buf dl(be);
        dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
        dl.s16(16); dl.s16(-32); dl.s16(8);
        dl.s16(0); dl.s16(0); dl.s16(0);
        dl.s16(32767); dl.s16(-32768); dl.s16(1);
        draw_stream(dl);
        const GxVertex* v = verts(3);
        if (!v) return;
        EXPECT_NEAR(v[0].pos[0], 1.0f, 1e-6); EXPECT_NEAR(v[0].pos[1], -2.0f, 1e-6); EXPECT_NEAR(v[0].pos[2], 0.5f, 1e-6);
        EXPECT_NEAR(v[2].pos[0], 32767.0f / 16.0f, 1e-4); EXPECT_NEAR(v[2].pos[1], -2048.0f, 1e-6); EXPECT_NEAR(v[2].pos[2], 1.0f / 16.0f, 1e-7);
    }
    // the model format: GX_POS_XY s16 with GXPosition2u16 style data, z defaults to 0
    gx_null_log_reset();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    Buf dl;
    dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { dl.s16(100 + i); dl.s16(-7); }
    draw_stream(dl);
    const GxVertex* v = verts(3);
    if (v) { EXPECT_NEAR(v[2].pos[0], 102.0f, 1e-6); EXPECT_NEAR(v[2].pos[1], -7.0f, 1e-6); EXPECT_NEAR(v[2].pos[2], 0.0f, 1e-6); }
}

static void test_s8_normals(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 0);   // frac is ignored for normals: s8 is always 1.6
    Buf dl;
    dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { dl.f32(0); dl.f32(0); dl.f32(0); dl.s8(64); dl.s8(-64); dl.s8(32); }
    draw_stream(dl);
    const GxVertex* v = verts(3);
    if (!v) return;
    EXPECT_NEAR(v[0].nrm[0], 1.0f, 1e-6); EXPECT_NEAR(v[0].nrm[1], -1.0f, 1e-6); EXPECT_NEAR(v[0].nrm[2], 0.5f, 1e-6);

    // s16 normals are 2.14 (trans.cpp: GXSetVtxAttrFmt(0, 10, 0, 3, 14)), indexed
    gx_null_log_reset();
    Buf nrm;
    nrm.s16(16384); nrm.s16(-8192); nrm.s16(0);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 14);
    GXSetArray(GX_VA_NRM, nrm.data, 6);
    Buf dl2;
    dl2.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { dl2.f32(0); dl2.f32(0); dl2.f32(0); dl2.u8(0); }
    draw_stream(dl2);
    v = verts(3);
    if (v) { EXPECT_NEAR(v[1].nrm[0], 1.0f, 1e-6); EXPECT_NEAR(v[1].nrm[1], -0.5f, 1e-6); }
}

static void test_colors(void)
{
    // RGBA8 direct
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    Buf dl;
    dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { dl.f32(0); dl.f32(0); dl.f32(0); dl.u8(0x10 + i); dl.u8(0x20); dl.u8(0x30); dl.u8(0x40); }
    draw_stream(dl);
    const GxVertex* v = verts(3);
    if (v) {
        EXPECT_EQ(v[0].col[0], GX_RGBA8(0x10, 0x20, 0x30, 0x40));
        EXPECT_EQ(v[2].col[0], GX_RGBA8(0x12, 0x20, 0x30, 0x40));
        EXPECT_EQ(GX_RGBA8_R(v[0].col[0]), 0x10); EXPECT_EQ(GX_RGBA8_A(v[0].col[0]), 0x40);
        EXPECT_EQ(v[0].col[1], 0xFFFFFFFFu);
    }
    // RGB565 in both byte orders, indexed through CLR1 as well
    for (int be = 0; be < 2; be++) {
        gx_null_log_reset();
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGB, GX_RGB565, 0);
        GXSetVtxDesc(GX_VA_CLR1, GX_INDEX8);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA4, 0);
        Buf clr1(be);
        clr1.u16(0xF00F);   // r=15 a=15 -> red opaque
        clr1.u16(0x0F0F);   // g=15 a=15
        GXSetArray(GX_VA_CLR1, clr1.data, 2);
        Buf d(be);
        d.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
        d.f32(0); d.f32(0); d.f32(0); d.u16(0xF800); d.u8(0);
        d.f32(0); d.f32(0); d.f32(0); d.u16(0x07E0); d.u8(1);
        d.f32(0); d.f32(0); d.f32(0); d.u16(0x001F); d.u8(0);
        draw_stream(d);
        v = verts(3);
        if (!v) return;
        EXPECT_EQ(v[0].col[0], GX_RGBA8(255, 0, 0, 255));
        EXPECT_EQ(v[1].col[0], GX_RGBA8(0, 255, 0, 255));
        EXPECT_EQ(v[2].col[0], GX_RGBA8(0, 0, 255, 255));
        EXPECT_EQ(v[0].col[1], GX_RGBA8(255, 0, 0, 255));
        EXPECT_EQ(v[1].col[1], GX_RGBA8(0, 255, 0, 255));
    }
    // RGB8 / RGBX8 / RGBA6 (byte sequences)
    gx_null_log_reset();
    GXSetVtxDesc(GX_VA_CLR1, GX_NONE);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA6, 0);
    Buf d6;
    d6.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { d6.f32(0); d6.f32(0); d6.f32(0); d6.u8(0xFC); d6.u8(0x00); d6.u8(0x3F); }   // r=63 g=0 b=0 a=63
    draw_stream(d6);
    v = verts(3);
    if (v) EXPECT_EQ(v[0].col[0], GX_RGBA8(255, 0, 0, 255));
    gx_null_log_reset();
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGB, GX_RGB8, 0);
    Buf d8;
    d8.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { d8.f32(0); d8.f32(0); d8.f32(0); d8.u8(1); d8.u8(2); d8.u8(3); }
    draw_stream(d8);
    v = verts(3);
    if (v) EXPECT_EQ(v[0].col[0], GX_RGBA8(1, 2, 3, 255));
}

static void test_fan_and_lines(void)
{
    vcd_pos_f32_direct();
    Buf dl;
    dl.prim(GX_TRIANGLEFAN, GX_VTXFMT0, 5);
    for (int i = 0; i < 5; i++) { dl.f32((float) i); dl.f32(0); dl.f32(0); }
    draw_stream(dl);
    EXPECT_EQ(gx_null_triangles(), 3);
    const GxVertex* v = verts(9);
    if (v) {
        static const int fan[9] = { 0, 1, 2, 0, 2, 3, 0, 3, 4 };
        for (int i = 0; i < 9; i++) EXPECT_NEAR(v[i].pos[0], (float) fan[i], 1e-6);
    }
    // lines and a line strip: kept as line lists, points as points; two streams in one list
    gx_null_log_reset();
    Buf l;
    l.prim(GX_LINESTRIP, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { l.f32((float) i); l.f32(0); l.f32(0); }
    l.prim(GX_POINTS, GX_VTXFMT0, 2);
    for (int i = 0; i < 2; i++) { l.f32(7); l.f32(0); l.f32(0); }
    draw_stream(l);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_DRAW), 2);
    const GxNullRecord* r0 = gx_null_log_get(0);
    const GxNullRecord* r1 = 0;
    for (size_t i = 0; i < gx_null_log_count(); i++) if (gx_null_log_get(i)->kind == GX_NULL_DRAW) { if (!r0 || r0->kind != GX_NULL_DRAW) r0 = gx_null_log_get(i); else r1 = gx_null_log_get(i); }
    if (r0 && r1) {
        EXPECT_EQ(r0->prim, GX_PRIM_LINES); EXPECT_EQ(r0->count, 4);
        EXPECT_EQ(r1->prim, GX_PRIM_POINTS); EXPECT_EQ(r1->count, 2);
    }
    // a stream with register writes and a bad opcode stops gracefully
    gx_null_log_reset();
    Buf r;
    r.u8(0x61); r.u32(0x41000000);        // BP reg
    r.u8(0x08); r.u8(0x50); r.u32(0);     // CP reg
    r.u8(0x10); r.u16(0); r.u16(0x1018); r.u32(0);   // XF reg, 1 word
    r.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { r.f32(1); r.f32(2); r.f32(3); }
    r.u8(0x77);   // unknown
    r.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    draw_stream(r);
    EXPECT_EQ(gx_null_triangles(), 1);
}

static void test_tex_mtx_idx_and_texgen_default(void)
{
    GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_TEXMTX4);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    Buf dl;
    dl.prim(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) { dl.u8(GX_TEXMTX2); dl.f32(0); dl.f32(0); dl.f32(0); }
    draw_stream(dl);
    const GxVertex* v = verts(3);
    if (v) {
        EXPECT_EQ(v[0].texmtxidx[0], 2);    // from the vertex
        EXPECT_EQ(v[0].texmtxidx[1], 4);    // from the texgen
        EXPECT_EQ(v[0].texmtxidx[2], 10);   // GX_IDENTITY
    }
}

// --- tests: immediate mode -------------------------------------------------------------------------------------------

static void test_immediate_quad(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) {
        GXPosition3f32(kQuadPos[i][0], kQuadPos[i][1], kQuadPos[i][2]);
        GXColor4u8((u8) (10 * i), 20, 30, 255);
        GXTexCoord2f32(kQuadUV[i][0], kQuadUV[i][1]);
    }
    EXPECT_EQ(gx_state.imm.pending, 0);   // the 4th vertex completed the primitive
    gx_flush();
    EXPECT_EQ(gx_null_triangles(), 2);
    const GxVertex* v = verts(6);
    if (!v) return;
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        EXPECT_NEAR(v[i].pos[0], kQuadPos[tri[i]][0], 1e-6); EXPECT_NEAR(v[i].pos[1], kQuadPos[tri[i]][1], 1e-6);
        EXPECT_NEAR(v[i].uv[0][0], kQuadUV[tri[i]][0], 1e-6); EXPECT_NEAR(v[i].uv[0][1], kQuadUV[tri[i]][1], 1e-6);
        EXPECT_EQ(v[i].col[0], GX_RGBA8(10 * tri[i], 20, 30, 255));
    }
    // two consecutive quads batch into one draw of 12 vertices
    gx_null_log_reset();
    for (int q = 0; q < 2; q++) {
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        for (int i = 0; i < 4; i++) { GXPosition3f32(0, 0, 0); GXColor4u8(0, 0, 0, 0); GXTexCoord2f32(0, 0); }
    }
    gx_flush();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_DRAW), 1);
    EXPECT_EQ(gx_null_triangles(), 4);
}

static void test_fifo_s16_write(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GXBegin(GX_POINTS, GX_VTXFMT0, 1);
    const unsigned char* start = re4_gx_fifo_cursor.wr;
    EXPECT_EQ(gx_state.imm.vsize, 4);
    GXWGFifo->s16 = 5;                                   // the mes.cpp idiom
    EXPECT_EQ(re4_gx_fifo_cursor.wr - start, 2);        // exactly two bytes appended
    EXPECT_EQ(start[0], 5); EXPECT_EQ(start[1], 0);     // native (little-endian) order
    int y = 7;
    GXWGFifo->s16 = y;                                   // int -> short conversion compiles
    EXPECT_EQ(gx_state.imm.pending, 0);
    gx_flush();
    const GxVertex* v = verts(1);
    if (v) { EXPECT_NEAR(v[0].pos[0], 5.0f, 1e-6); EXPECT_NEAR(v[0].pos[1], 7.0f, 1e-6); }
    // other lanes: u8 appends one byte, f32 four, u16 two (GXPosition2u16)
    gx_null_log_reset();
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_POINTS, GX_VTXFMT0, 1);
    start = re4_gx_fifo_cursor.wr;
    GXPosition2u16(640, 448);
    EXPECT_EQ(re4_gx_fifo_cursor.wr - start, 4);
    GXWGFifo->u8 = 1;
    EXPECT_EQ(re4_gx_fifo_cursor.wr - start, 5);
    GXColor4u8(2, 3, 4, 5);   // 4 more bytes: one too many -> dropped with a log, primitive completed at 8
    EXPECT_EQ(gx_state.imm.pending, 0);
    gx_flush();
    v = verts(1);
    if (v) { EXPECT_NEAR(v[0].pos[0], 640.0f, 1e-6); EXPECT_EQ(v[0].col[0], GX_RGBA8(1, 2, 3, 4)); }
    // a write with nothing open is dropped, not crashed
    GXWGFifo->f32 = 1.0f;
    EXPECT_EQ(re4_gx_fifo_cursor.wr, (unsigned char*) 0);
}

static void test_partial_primitive_flush(void)
{
    vcd_pos_f32_direct();
    GXBegin(GX_QUADS, GX_VTXFMT0, 8);
    for (int i = 0; i < 4; i++) GXPosition3f32((float) i, 0, 0);
    GXPosition3f32(99, 0, 0);   // one vertex into the second quad
    EXPECT_EQ(gx_state.imm.pending, 1);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);   // state change: flush
    EXPECT_EQ(gx_state.imm.pending, 0);
    EXPECT_EQ(gx_null_triangles(), 2);     // the complete quad was drawn, the partial one dropped
    const GxVertex* v = verts(6);
    if (v) EXPECT_NEAR(v[5].pos[0], 3.0f, 1e-6);
    // the flushed draw used the blend mode in force before the change
    const GxPipelineKey* k = gx_null_last_pipeline();
    EXPECT(k && k->blend_type == GX_BM_NONE);
    GXPosition3f32(1, 2, 3);   // stray write after the flush: dropped
    gx_flush();
    EXPECT_EQ(gx_null_triangles(), 2);
    // GXBegin while pending flushes too
    gx_null_log_reset();
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
    for (int i = 0; i < 3; i++) GXPosition3f32(0, 0, 0);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (int i = 0; i < 3; i++) GXPosition3f32(1, 1, 1);
    gx_flush();
    EXPECT_EQ(gx_null_triangles(), 2);
}

// --- tests: TEV / pipeline -----------------------------------------------------------------------------------------

static int in4(const uint8_t* a, int x, int y, int z, int w) { return a[0] == x && a[1] == y && a[2] == z && a[3] == w; }

static void test_tevop_expansion(void)
{
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP1, GX_TEV_SWAP2);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_4, 0, GX_TEVREG1);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetTevOp(GX_TEVSTAGE1, GX_MODULATE);
    const GxTevStage* s = gx_state.tev.stage;
    EXPECT(in4(s[0].color_in, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO));
    EXPECT(in4(s[0].alpha_in, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO));
    EXPECT(in4(s[1].color_in, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO));
    EXPECT(in4(s[1].alpha_in, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO));
    // op/bias/scale/clamp/out reset to ADD/ZERO/1/TRUE/PREV (GXTev.c tables: dest 0, shift 0, clamp 1, sub 0, bias 0)
    EXPECT_EQ(s[0].color_op.op, GX_TEV_ADD); EXPECT_EQ(s[0].color_op.bias, GX_TB_ZERO);
    EXPECT_EQ(s[0].color_op.scale, GX_CS_SCALE_1); EXPECT_EQ(s[0].color_op.clamp, 1); EXPECT_EQ(s[0].color_op.out_reg, GX_TEVPREV);
    EXPECT_EQ(s[0].alpha_op.op, GX_TEV_ADD); EXPECT_EQ(s[0].alpha_op.clamp, 1); EXPECT_EQ(s[0].alpha_op.out_reg, GX_TEVPREV);
    // the swap selectors live in the low bits of the alpha register and survive GXSetTevOp
    EXPECT_EQ(s[0].ras_swap, GX_TEV_SWAP1); EXPECT_EQ(s[0].tex_swap, GX_TEV_SWAP2);

    GXSetTevOp(GX_TEVSTAGE0, GX_DECAL);   GXSetTevOp(GX_TEVSTAGE1, GX_DECAL);
    EXPECT(in4(s[0].color_in, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO));
    EXPECT(in4(s[0].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA));
    EXPECT(in4(s[1].color_in, GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO));
    EXPECT(in4(s[1].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV));
    GXSetTevOp(GX_TEVSTAGE0, GX_BLEND);   GXSetTevOp(GX_TEVSTAGE1, GX_BLEND);
    EXPECT(in4(s[0].color_in, GX_CC_RASC, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO));
    EXPECT(in4(s[0].alpha_in, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO));
    EXPECT(in4(s[1].color_in, GX_CC_CPREV, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO));
    EXPECT(in4(s[1].alpha_in, GX_CA_ZERO, GX_CA_TEXA, GX_CA_APREV, GX_CA_ZERO));
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE); GXSetTevOp(GX_TEVSTAGE1, GX_REPLACE);
    EXPECT(in4(s[0].color_in, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC));
    EXPECT(in4(s[0].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA));
    EXPECT(in4(s[1].color_in, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC));
    EXPECT(in4(s[1].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA));
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR); GXSetTevOp(GX_TEVSTAGE1, GX_PASSCLR);
    EXPECT(in4(s[0].color_in, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC));
    EXPECT(in4(s[0].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA));
    EXPECT(in4(s[1].color_in, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV));
    EXPECT(in4(s[1].alpha_in, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV));
}

static void setup_material_a(void)
{
    GXSetNumTevStages(2);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP3, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_KONST);
    GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, 1, GX_TEVPREV);
    GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
    GXSetNumTexGens(2);
    GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, 1, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0 | GX_LIGHT2, GX_DF_CLAMP, GX_AF_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetZMode(1, GX_LEQUAL, 0);
    GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXColor fog = { 10, 20, 30, 0 };
    GXSetFog(GX_FOG_PERSP_LIN, 10.0f, 100.0f, 1.0f, 1000.0f, fog);
}

static void test_pipeline_hash(void)
{
    setup_material_a();
    draw_quad_imm();
    uint64_t h1 = last_hash();
    EXPECT(h1 != 0);
    const GxPipelineKey* k = gx_null_last_pipeline();
    EXPECT(k != 0);
    if (k) {
        EXPECT_EQ(k->num_tev_stages, 2); EXPECT_EQ(k->num_texgens, 2); EXPECT_EQ(k->num_chans, 1);
        EXPECT_EQ(k->blend_type, GX_BM_BLEND); EXPECT_EQ(k->z_update, 0); EXPECT_EQ(k->alpha_ref0, 128);
        EXPECT_EQ(k->fog_type, GX_FOG_PERSP_LIN); EXPECT_NEAR(k->fog_end, 100.0f, 1e-6);
        EXPECT_EQ(k->fog_color, GX_RGBA8(10, 20, 30, 0));
        EXPECT(k->tev != 0);
        if (k->tev) {
            EXPECT_EQ(k->tev->tex_enable_mask, (1 << 0) | (1 << 3));
            EXPECT_EQ(k->tev->chan[GX_COLOR0].light_mask, GX_LIGHT0 | GX_LIGHT2);
            EXPECT_EQ(k->tev->chan[GX_ALPHA0].enable, 1);
            EXPECT_EQ(k->tev->chan[GX_COLOR1].enable, 0);
        }
    }
    // maps 0 and 3 were bound (default texture), nothing else
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_BIND_TEX), 2);

    // identical set-up from scratch -> identical hash
    reset();
    setup_material_a();
    draw_quad_imm();
    EXPECT(last_hash() == h1);

    // uniforms do not change the hash but do re-send the pipeline
    GXColor kc = { 1, 2, 3, 4 };
    GXSetTevKColor(GX_KCOLOR1, kc);
    GXColor amb = { 9, 9, 9, 9 };
    GXSetChanAmbColor(GX_COLOR0A0, amb);
    size_t pipes = gx_null_log_count_kind(GX_NULL_PIPELINE);
    draw_quad_imm();
    EXPECT(last_hash() == h1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_PIPELINE), pipes + 1);
    // no change at all -> no new set_pipeline
    draw_quad_imm();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_PIPELINE), pipes + 1);

    // each single change -> a different hash
    uint64_t prev = h1;
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K2); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetZMode(1, GX_LESS, 0); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetCullMode(GX_CULL_BACK); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetAlphaCompare(GX_GREATER, 129, GX_AOP_AND, GX_ALWAYS, 0); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_TEXMTX1); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetChanCtrl(GX_COLOR0A0, 1, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP1); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetTevIndWarp(GX_TEVSTAGE1, GX_INDTEXSTAGE0, 1, 0, GX_ITM_0); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetNumIndStages(1); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXColor fog = { 10, 20, 30, 0 };
    GXSetFog(GX_FOG_NONE, 10.0f, 100.0f, 1.0f, 1000.0f, fog); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetColorUpdate(0); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    GXSetDstAlpha(1, 77); draw_quad_imm(); EXPECT(last_hash() != prev); prev = last_hash();
    // a stage beyond num_tev_stages does not matter
    GXSetTevColorIn(GX_TEVSTAGE5, GX_CC_ONE, GX_CC_ONE, GX_CC_ONE, GX_CC_ONE); draw_quad_imm(); EXPECT(last_hash() == prev);
}

static void test_defaults(void)
{
    const GxState& s = gx_state;
    EXPECT_EQ(s.cull_mode, GX_CULL_BACK);
    EXPECT_EQ(s.blend_type, GX_BM_NONE); EXPECT_EQ(s.blend_src, GX_BL_SRCALPHA); EXPECT_EQ(s.blend_dst, GX_BL_INVSRCALPHA);
    EXPECT_EQ(s.z_enable, 1); EXPECT_EQ(s.z_func, GX_LEQUAL); EXPECT_EQ(s.z_update, 1); EXPECT_EQ(s.z_comp_loc, 1);
    EXPECT_EQ(s.alpha_comp0, GX_ALWAYS); EXPECT_EQ(s.alpha_op, GX_AOP_AND);
    EXPECT_EQ(s.dither, 1); EXPECT_EQ(s.color_update, 1); EXPECT_EQ(s.alpha_update, 1);
    EXPECT_EQ(s.pixel_fmt, GX_PF_RGB8_Z24); EXPECT_EQ(s.z_fmt, GX_ZC_LINEAR);
    EXPECT_EQ(s.fog_type, GX_FOG_NONE);
    EXPECT_EQ(s.line_width, 6);
    EXPECT_EQ(s.tev.num_stages, 1); EXPECT_EQ(s.tev.num_texgens, 1); EXPECT_EQ(s.tev.num_chans, 0); EXPECT_EQ(s.tev.num_ind_stages, 0);
    EXPECT(in4(s.tev.stage[0].color_in, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC));   // GX_REPLACE
    EXPECT_EQ(s.tev.stage[0].coord, GX_TEXCOORD0); EXPECT_EQ(s.tev.stage[0].map, GX_TEXMAP0); EXPECT_EQ(s.tev.stage[0].chan, GX_COLOR0A0);
    EXPECT_EQ(s.tev.stage[8].coord, GX_TEXCOORD_NULL); EXPECT_EQ(s.tev.stage[8].map, GX_TEXMAP_NULL); EXPECT_EQ(s.tev.stage[8].chan, GX_COLOR_NULL);
    EXPECT_EQ(s.tev.stage[3].kcsel, GX_TEV_KCSEL_1_4); EXPECT_EQ(s.tev.stage[3].kasel, GX_TEV_KASEL_1);
    EXPECT_EQ(s.tev.stage[3].ind.mtx_sel, GX_ITM_OFF);
    EXPECT_EQ(s.tev.swap_table[2][0], GX_CH_GREEN); EXPECT_EQ(s.tev.swap_table[3][3], GX_CH_ALPHA);
    EXPECT_EQ(s.tev.texgen[5].func, GX_TG_MTX2x4); EXPECT_EQ(s.tev.texgen[5].src, GX_TG_TEX5); EXPECT_EQ(s.tev.texgen[5].mtx, GX_IDENTITY);
    EXPECT_EQ(s.tev.texgen[5].postmtx, GX_PTIDENTITY);
    EXPECT_EQ(s.tev.chan[GX_COLOR0].mat_src, GX_SRC_VTX); EXPECT_EQ(s.tev.chan[GX_ALPHA1].amb_src, GX_SRC_REG);
    EXPECT_EQ(s.tev.mat_color[0], 0xFFFFFFFFu); EXPECT_EQ(s.tev.amb_color[1], 0u);
    EXPECT_EQ(s.vcd[GX_VA_POS], GX_DIRECT); EXPECT_EQ(s.vcd[GX_VA_NRM], GX_NONE); EXPECT_EQ(s.vcd[GX_VA_TEX0], GX_NONE);
    EXPECT_EQ(s.vat[7][GX_VA_CLR0 - GX_VA_POS].type, GX_RGBA8); EXPECT_EQ(s.vat[3][GX_VA_TEX7 - GX_VA_POS].cnt, GX_TEX_ST);
    EXPECT_EQ(s.proj_type, GX_ORTHOGRAPHIC);
    EXPECT_NEAR(s.vp_wd, 640.0f, 0); EXPECT_NEAR(s.vp_ht, 480.0f, 0);
    EXPECT_EQ(s.scissor_wd, 640); EXPECT_EQ(s.scissor_ht, 480);
    EXPECT_EQ(s.copy_clear_color, GX_RGBA8(64, 64, 64, 255)); EXPECT_EQ(s.copy_clear_z, 0xFFFFFF);
    EXPECT_EQ(s.disp_copy_src.wd, 640); EXPECT_EQ(s.disp_copy_dst_ht, 480);
    EXPECT_EQ(s.texmap_valid[7], 1); EXPECT_EQ(s.texmap[7].width, 4); EXPECT_EQ(s.texmap[7].format, GX_TF_IA8);
    EXPECT_NEAR(s.pos_mtx[0][1][1], 1.0f, 0); EXPECT_NEAR(s.tex_mtx[10][2][2], 1.0f, 0); EXPECT_NEAR(s.pt_mtx[20][0][0], 1.0f, 0);
    f32 p[7], vp[6];
    GXGetProjectionv(p);
    EXPECT_NEAR(p[0], 1.0f, 0); EXPECT_NEAR(p[2], 1.0f, 0); EXPECT_NEAR(p[4], -1.0f, 0); EXPECT_NEAR(p[5], -2.0f, 0); EXPECT_NEAR(p[6], 0.0f, 0);   // GXDefaultProjData {1,0,1,0,-1,-2,0}
    GXGetViewportv(vp);
    EXPECT_NEAR(vp[2], 640.0f, 0); EXPECT_NEAR(vp[5], 1.0f, 0);

    // the first draw after init: begin_frame, one pipeline, matrices, one texture bind (map 0), viewport/scissor were sent at init
    draw_quad_imm();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_BEGIN_FRAME), 1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_PIPELINE), 1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_MATRICES), 1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_BIND_TEX), 1);
    const GxNullRecord* b = gx_null_log_last_kind(GX_NULL_BIND_TEX);
    if (b) { EXPECT_EQ(b->map, 0); EXPECT(b->count != 0); EXPECT_NEAR(b->f[0], GX_CLAMP, 0); EXPECT_NEAR(b->f[2], GX_LINEAR, 0); }
    EXPECT_EQ(gx_null_last_proj_type(), GX_ORTHOGRAPHIC);
    EXPECT_NEAR(gx_null_last_tex_mtx(10)[0][0], 1.0f, 0);
}

static void test_frame_and_copies(void)
{
    draw_quad_imm();
    GXCopyDisp((void*) 0x1000, 1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_END_FRAME), 1);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_CLEAR), 0);   // deferred to the next frame
    draw_quad_imm();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_BEGIN_FRAME), 2);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_CLEAR), 1);
    const GxNullRecord* c = gx_null_log_last_kind(GX_NULL_CLEAR);
    if (c) { EXPECT_EQ(c->count, GX_RGBA8(64, 64, 64, 255)); EXPECT_EQ(c->hash, 0xFFFFFF); EXPECT_EQ(c->map, 3); }

    // GXCopyTex into a buffer that is then bound as a texture
    static uint8_t copybuf[64] __attribute__((aligned(32)));
    GXSetTexCopySrc(0, 0, 320, 224);
    GXSetTexCopyDst(160, 112, GX_TF_RGBA8, 0);
    size_t creates = gx_null_log_count_kind(GX_NULL_CREATE_TEX);
    GXCopyTex(copybuf, 0);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_CREATE_TEX), creates + 1);
    const GxNullRecord* cr = gx_null_log_last_kind(GX_NULL_CREATE_TEX);
    if (cr) { EXPECT_NEAR(cr->f[0], 160, 0); EXPECT_NEAR(cr->f[1], 112, 0); EXPECT_EQ(cr->map, 0); }   // data_size 0: render target
    const GxNullRecord* cp = gx_null_log_last_kind(GX_NULL_COPY_TO_TEX);
    EXPECT(cp != 0);
    if (cp && cr) { EXPECT_EQ(cp->count, cr->count); EXPECT_NEAR(cp->f[2], 320, 0); EXPECT_NEAR(cp->f[3], 224, 0); }
    GXTexObj obj;
    GXInitTexObj(&obj, copybuf, 160, 112, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, 0);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, 0, 0, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    draw_quad_imm();
    const GxNullRecord* b = gx_null_log_last_kind(GX_NULL_BIND_TEX);
    if (b && cr) { EXPECT_EQ(b->count, cr->count); EXPECT_EQ(b->map, 0); EXPECT_NEAR(b->f[2], GX_NEAR, 0); }

    // raw images are re-uploaded after GXInvalidateTexAll, converted ones are not
    static uint8_t raw[32] __attribute__((aligned(32)));
    static struct { ConvertedImage hdr; uint8_t px[64]; } conv __attribute__((aligned(32)));
    conv.hdr.magic = RE4_IMG_MAGIC; conv.hdr.width = 4; conv.hdr.height = 4; conv.hdr.data_size = 64;
    GXTexObj t1, t2;
    GXInitTexObj(&t1, raw, 4, 4, GX_TF_I8, GX_REPEAT, GX_REPEAT, 0);
    GXInitTexObj(&t2, &conv, 4, 4, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, 0);
    GXLoadTexObj(&t1, GX_TEXMAP0);
    GXLoadTexObj(&t2, GX_TEXMAP1);
    GXSetNumTevStages(2);
    GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP1, GX_COLOR_NULL);
    draw_quad_imm();
    creates = gx_null_log_count_kind(GX_NULL_CREATE_TEX);
    draw_quad_imm();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_CREATE_TEX), creates);       // cached
    GXInvalidateTexAll();
    draw_quad_imm();
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_CREATE_TEX), creates + 1);   // only the raw one
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_DESTROY_TEX), 1);
    EXPECT(GXGetTexObjData(&t2) == (void*) &conv);
    u32 z = 0;
    GXPeekZ(10, 20, &z);
    EXPECT_EQ(z, 0xFFFFFF);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_PEEK_Z), 1);
}

static void test_texbuffersize_and_sizes(void)
{
    EXPECT_EQ(GXGetTexBufferSize(64, 64, GX_TF_RGBA8, 0, 0), 64 * 64 * 4);
    EXPECT_EQ(GXGetTexBufferSize(64, 64, GX_TF_CMPR, 0, 0), 64 * 64 / 2);
    EXPECT_EQ(GXGetTexBufferSize(32, 32, GX_TF_I4, 0, 0), 32 * 32 / 2);
    EXPECT_EQ(GXGetTexBufferSize(640, 448, GX_TF_RGB565, 0, 0), 640 * 448 * 2);
    EXPECT_EQ(GXGetTexBufferSize(100, 100, GX_TF_I8, 0, 0), 13 * 25 * 32);      // 8x4 tiles, rounded up
    EXPECT_EQ(GXGetTexBufferSize(32, 32, GX_TF_RGB565, 1, 6), 2048 + 512 + 128 + 32 + 32 + 32);
    EXPECT_EQ(GXGetTexBufferSize(256, 256, GX_TF_C8, 1, 9), 65536 + 16384 + 4096 + 1024 + 256 + 64 + 32 + 32 + 32);
    EXPECT_EQ(GXGetTexBufferSize(8, 8, GX_TF_RGBA8, 1, 2), 256 + 64);
    EXPECT_EQ(sizeof(GXTexObj), 32); EXPECT(sizeof(GxTexObjInt) <= 32);
    EXPECT_EQ(sizeof(GXTlutObj), 12); EXPECT(sizeof(GxTlutObjInt) <= 12);
    EXPECT_EQ(sizeof(GXLightObj), 64); EXPECT(sizeof(GxLightObjInt) <= 64);

    GXTexObj obj;
    GXInitTexObjCI(&obj, (void*) 0x2000, 128, 64, GX_TF_C8, GX_REPEAT, GX_MIRROR, 1, GX_TLUT3);
    const GxTexObjInt* t = (const GxTexObjInt*) &obj;
    EXPECT_EQ(t->is_ci, 1); EXPECT_EQ(t->tlut_name, GX_TLUT3); EXPECT_EQ(t->min_filt, GX_LIN_MIP_NEAR);
    EXPECT_EQ(t->max_lod16, 16 * 7); EXPECT_EQ(t->wrap_t, GX_MIRROR); EXPECT_EQ(t->mag_filt, GX_LINEAR);
    GXInitTexObjLOD(&obj, GX_LIN_MIP_LIN, GX_LINEAR, 1.0f, 20.0f, -8.0f, 1, 0, GX_ANISO_4);
    EXPECT_EQ(t->max_lod16, 160); EXPECT_EQ(t->min_lod16, 16); EXPECT_NEAR(t->lod_bias, -4.0f, 0); EXPECT_EQ(t->edge_lod, 0);

    // lights
    GXLightObj lo;
    memset(&lo, 0, sizeof lo);
    GXInitLightPos(&lo, 1, 2, 3);
    GXInitLightDir(&lo, 0, 0, 1);
    GXColor lc = { 200, 100, 50, 255 };
    GXInitLightColor(&lo, lc);
    GXInitLightSpot(&lo, 45.0f, GX_SP_COS);
    GXInitLightDistAttn(&lo, 100.0f, 0.5f, GX_DA_GENTLE);
    GXLoadLightObjImm(&lo, GX_LIGHT2);
    const GxLightObjInt& L = gx_state.tev.light[2];
    EXPECT_NEAR(L.pos[2], 3.0f, 0); EXPECT_NEAR(L.dir[2], -1.0f, 0); EXPECT_EQ(L.color, GX_RGBA8(200, 100, 50, 255));
    EXPECT_NEAR(L.k[0], 1.0f, 0); EXPECT_NEAR(L.k[1], 0.5f / (0.5f * 100.0f), 1e-6); EXPECT_NEAR(L.k[2], 0.0f, 0);
    float cr = cosf(3.1415927f * 45.0f / 180.0f);
    EXPECT_NEAR(L.a[1], 1.0f / (1.0f - cr), 1e-5); EXPECT_NEAR(L.a[0], -cr / (1.0f - cr), 1e-5);
}

static void test_project_and_matrices(void)
{
    f32 ortho[4][4] = { {2.0f / 640, 0, 0, -1}, {0, -2.0f / 480, 0, 1}, {0, 0, -1, 0}, {0, 0, 0, 1} };
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    f32 pm[7], vp[6], sx, sy, sz;
    GXGetProjectionv(pm);
    GXGetViewportv(vp);
    EXPECT_NEAR(pm[0], 1.0f, 0); EXPECT_NEAR(pm[2], -1.0f, 0); EXPECT_NEAR(pm[4], 1.0f, 0);
    f32 ident[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    GXProject(320, 240, 0, ident, pm, vp, &sx, &sy, &sz);
    EXPECT_NEAR(sx, 320.0f, 1e-3); EXPECT_NEAR(sy, 240.0f, 1e-3);
    GXProject(0, 0, 0, ident, pm, vp, &sx, &sy, &sz);
    EXPECT_NEAR(sx, 0.0f, 1e-3); EXPECT_NEAR(sy, 0.0f, 1e-3);
    // perspective form picks [0][2]/[1][2]
    f32 persp[4][4] = { {1.5f, 0, 0.1f, 0}, {0, 2.0f, 0.2f, 0}, {0, 0, -1.001f, -2.002f}, {0, 0, -1, 0} };
    GXSetProjection(persp, GX_PERSPECTIVE);
    GXGetProjectionv(pm);
    EXPECT_NEAR(pm[0], 0.0f, 0); EXPECT_NEAR(pm[1], 1.5f, 0); EXPECT_NEAR(pm[2], 0.1f, 0); EXPECT_NEAR(pm[6], -2.002f, 0);
    // 2x4 texture matrices get a 0 0 1 0 third row; the identity slots stay identity
    f32 tm[2][4] = { {2, 0, 0, 0.5f}, {0, 2, 0, 0.25f} };
    GXLoadTexMtxImm(tm, GX_TEXMTX3, GX_MTX2x4);
    EXPECT_NEAR(gx_state.tex_mtx[3][0][3], 0.5f, 0); EXPECT_NEAR(gx_state.tex_mtx[3][2][2], 1.0f, 0);
    EXPECT_EQ(gx_state.tex_mtx_type[3], GX_MTX2x4);
    draw_quad_imm();
    EXPECT_NEAR(gx_null_last_tex_mtx(3)[1][3], 0.25f, 0);
    EXPECT_EQ(gx_null_last_proj_type(), GX_PERSPECTIVE);
    EXPECT_NEAR(gx_null_last_proj()[2][3], -2.002f, 0);
    // GXSetCurrentMtx feeds the default matrix index
    GXSetCurrentMtx(GX_PNMTX4);
    gx_null_log_reset();
    draw_quad_imm();
    const GxVertex* v = verts(6);
    if (v) EXPECT_EQ(v[0].mtxidx, 4);
}

static void test_display_list_recording(void)
{
    static uint8_t list[256] __attribute__((aligned(32)));
    vcd_pos_f32_direct();
    GXBeginDisplayList(list, sizeof list);
    GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
    GXPosition3f32(1, 2, 3); GXPosition3f32(4, 5, 6); GXPosition3f32(7, 8, 9);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) GXPosition3f32((float) i, 0, 0);
    u32 n = GXEndDisplayList();
    EXPECT_EQ(n, 96);                       // 3+36 + 3+48 = 90, padded to 32 bytes
    EXPECT_EQ(list[0], GX_TRIANGLES); EXPECT_EQ(list[1], 3); EXPECT_EQ(list[2], 0);
    EXPECT_EQ(list[39], GX_QUADS);
    EXPECT_EQ(gx_null_log_count_kind(GX_NULL_DRAW), 0);   // recording draws nothing
    GXCallDisplayList(list, n);
    gx_flush();
    EXPECT_EQ(gx_null_triangles(), 3);
    const GxVertex* v = verts(9);
    if (v) { EXPECT_NEAR(v[2].pos[2], 9.0f, 0); EXPECT_NEAR(v[8].pos[0], 3.0f, 0); }
}

static void test_draw_torus(void)
{
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_S16, 8);   // GXDrawTorus must override this to f32 and restore it
    GXDrawTorus(0.4f, 16, 12);
    gx_flush();
    EXPECT_EQ(gx_null_triangles(), 16 * ((12 + 1) * 2 - 2));
    EXPECT_EQ(gx_state.vcd[GX_VA_NRM], GX_NONE);
    EXPECT_EQ(gx_state.vcd[GX_VA_TEX0], GX_DIRECT);
    EXPECT_EQ(gx_state.vat[3][GX_VA_TEX0 - GX_VA_POS].type, GX_S16);
    size_t n = 0;
    const GxVertex* v = gx_null_vertices(&n);
    double maxr = 0;
    for (size_t i = 0; i < n; i++) {
        double r = sqrt(v[i].pos[0] * v[i].pos[0] + v[i].pos[1] * v[i].pos[1] + v[i].pos[2] * v[i].pos[2]);
        if (r > maxr) maxr = r;
        EXPECT(v[i].uv[0][0] >= 0.0f && v[i].uv[0][0] <= 1.0f);
    }
    EXPECT_NEAR(maxr, 1.0, 1e-5);
}

// --- tests: the CT-05 catalogue ------------------------------------------------------------------------------------------

#ifdef GX_TEST_HAVE_TEV_CASES
// Field-by-field comparison of the hashed (program) part, independent of the hasher.
static int program_equal(const GxTevProgram* a, const GxTevProgram* b, const GxPipelineKey* ka, const GxPipelineKey* kb)
{
    if (a->num_stages != b->num_stages || a->num_texgens != b->num_texgens || a->num_chans != b->num_chans || a->num_ind_stages != b->num_ind_stages) return 0;
    for (int i = 0; i < a->num_stages; i++) {
        const GxTevStage& x = a->stage[i];
        const GxTevStage& y = b->stage[i];
        if (x.coord != y.coord || x.chan != y.chan || x.map != y.map) return 0;
        if (memcmp(x.color_in, y.color_in, 4) || memcmp(x.alpha_in, y.alpha_in, 4)) return 0;
        if (x.color_op.op != y.color_op.op || x.color_op.bias != y.color_op.bias || x.color_op.scale != y.color_op.scale || x.color_op.clamp != y.color_op.clamp || x.color_op.out_reg != y.color_op.out_reg) return 0;
        if (x.alpha_op.op != y.alpha_op.op || x.alpha_op.bias != y.alpha_op.bias || x.alpha_op.scale != y.alpha_op.scale || x.alpha_op.clamp != y.alpha_op.clamp || x.alpha_op.out_reg != y.alpha_op.out_reg) return 0;
        if (x.kcsel != y.kcsel || x.kasel != y.kasel || x.ras_swap != y.ras_swap || x.tex_swap != y.tex_swap) return 0;
        if (memcmp(&x.ind, &y.ind, sizeof x.ind)) return 0;
    }
    for (int i = 0; i < a->num_texgens; i++) if (memcmp(&a->texgen[i], &b->texgen[i], sizeof(GxTexGen))) return 0;
    for (int i = 0; i < GX_MAX_CHANS_PORT; i++) if (memcmp(&a->chan[i], &b->chan[i], sizeof(GxChanCtrl))) return 0;
    for (int i = 0; i < a->num_ind_stages; i++) if (memcmp(&a->ind[i], &b->ind[i], sizeof(GxIndStage))) return 0;
    if (memcmp(a->swap_table, b->swap_table, sizeof a->swap_table)) return 0;
    const uint8_t* pa = &ka->num_tev_stages;   // the contiguous u8 block of the key
    const uint8_t* pb = &kb->num_tev_stages;
    return memcmp(pa, pb, (size_t) (&ka->pixel_fmt - pa) + 2) == 0;
}

static void test_tev_catalogue(void)
{
    printf("  %d catalogue cases\n", gx_tev_case_count);
    GxTevProgram* progs = (GxTevProgram*) calloc(gx_tev_case_count, sizeof(GxTevProgram));
    GxPipelineKey* keys = (GxPipelineKey*) calloc(gx_tev_case_count, sizeof(GxPipelineKey));
    int approx = 0;
    for (int i = 0; i < gx_tev_case_count; i++) {
        const GxTevCase& c = gx_tev_cases[i];
        reset();
        int a = c.replay();
        EXPECT_EQ(a, c.approximate);
        approx += a;
        draw_quad_imm();
        const GxPipelineKey* k = gx_null_last_pipeline();
        EXPECT(k != 0);
        if (!k) continue;
        EXPECT(k->hash != 0);
        EXPECT(k->tev != 0);
        EXPECT_EQ(k->num_tev_stages, gx_state.tev.num_stages);
        EXPECT(k->num_tev_stages >= 1 && k->num_tev_stages <= 16);
        EXPECT(k->num_texgens <= 8);
        EXPECT(k->num_chans <= 2);
        EXPECT_EQ(gx_null_log_count_kind(GX_NULL_DRAW), 1);
        if (c.textures) {
            int bound = 0;
            for (int m = 0; m < 8; m++) bound += (k->tev->tex_enable_mask >> m) & 1;
            if (!c.approximate) EXPECT_EQ(bound, c.textures);
        }
        progs[i] = *k->tev;
        keys[i] = *k;
    }
    int collisions = 0, distinct = 0;
    for (int i = 0; i < gx_tev_case_count; i++) {
        for (int j = i + 1; j < gx_tev_case_count; j++) {
            int same = program_equal(&progs[i], &progs[j], &keys[i], &keys[j]);
            if (!same) distinct++;
            if (!same && keys[i].hash == keys[j].hash) {
                collisions++;
                printf("  HASH COLLISION: %s vs %s (%016llx)\n", gx_tev_cases[i].id, gx_tev_cases[j].id, (unsigned long long) keys[i].hash);
            }
            if (same && keys[i].hash != keys[j].hash) {
                g_fail++;
                printf("  FAIL: identical programs %s / %s hash differently\n", gx_tev_cases[i].id, gx_tev_cases[j].id);
            }
        }
    }
    printf("  %d distinct pairs, %d approximate cases, %d collisions\n", distinct, approx, collisions);
    EXPECT_EQ(collisions, 0);
    free(progs);
    free(keys);
}
#endif

// --- main ------------------------------------------------------------------------------------------------------------------

#undef main   // re4_xbox.h renames the game's main(); this is the real process entry point

int main(void)
{
    gx_select_backend(&gx_null_backend);
    gx_null_backend.init(640, 480);
    RUN(test_defaults);
    RUN(test_quad_idx16_le);
    RUN(test_quad_idx16_be);
    RUN(test_strip_with_pnmtxidx);
    RUN(test_s16_pos_frac4);
    RUN(test_s8_normals);
    RUN(test_colors);
    RUN(test_fan_and_lines);
    RUN(test_tex_mtx_idx_and_texgen_default);
    RUN(test_immediate_quad);
    RUN(test_fifo_s16_write);
    RUN(test_partial_primitive_flush);
    RUN(test_tevop_expansion);
    RUN(test_pipeline_hash);
    RUN(test_frame_and_copies);
    RUN(test_texbuffersize_and_sizes);
    RUN(test_project_and_matrices);
    RUN(test_display_list_recording);
    RUN(test_draw_torus);
#ifdef GX_TEST_HAVE_TEV_CASES
    RUN(test_tev_catalogue);
#else
    printf("test_tev_catalogue                       skipped (no generated cases)\n");
#endif
    printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
