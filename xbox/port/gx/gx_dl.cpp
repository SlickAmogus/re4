// gx_dl.cpp — the GX geometry front end (CT-08, xbox/docs/02-GRAPHICS.md section 3):
//   - the immediate-mode FIFO (GXBegin + the GXWGFifo proxy writes of re4port/gx_fifo.h),
//   - GXCallDisplayList and gx_run_stream(), the FIFO-stream interpreter,
//   - the one vertex decoder both share (VCD + VAT + arrays -> GxVertex),
//   - primitive expansion (quads/strips/fans -> triangle lists) and the draw accumulator,
//   - display-list recording (GXBeginDisplayList/GXEndDisplayList, dbmodule.cpp only),
//   - GXDrawTorus (GXDraw.c).
//
// Byte order: a stream is decoded with `big_endian` for its u16 counts/indices and for the array data
// those indices dereference. Immediate-mode primitives are always native (the CPU wrote them);
// GXCallDisplayList uses the global set by gx_set_dl_big_endian (default native: converted assets).
// Byte-sequence colour formats (RGB8, RGBX8, RGBA6, RGBA8) have no byte order; RGB565/RGBA4 are u16.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"
#include "gx_state.h"
#include "gx.h"
#include <dolphin/gx/GXEnum.h>
#include <string.h>
#include <math.h>

// --- globals of the FIFO proxy (re4port/gx_fifo.h) ---------------------------------------------------------

Re4GxFifoCursor re4_gx_fifo_cursor = { 0, 0 };   // C linkage from the declaration in gx_fifo.h
Re4GxFifo re4_gx_fifo;

static GxByteBuffer   imm_buf;        // immediate-mode primitive bytes
static GxVertexBuffer decoded;        // one primitive's vertices before expansion
static GxVertexBuffer acc;            // expanded vertices waiting for a backend draw
static int            acc_prim = GX_PRIM_TRIANGLES;
static int            dl_big_endian = 0;
static GxState*       dl_saved_state = 0;   // GXBeginDisplayList context save (allocated on first use)

extern "C" void gx_set_dl_big_endian(int big_endian) { dl_big_endian = big_endian ? 1 : 0; }
extern "C" int  gx_get_dl_big_endian(void) { return dl_big_endian; }

static inline void cursor_reset(void) { re4_gx_fifo_cursor.wr = 0; re4_gx_fifo_cursor.end = 0; }

void gx_imm_reset(void)
{
    cursor_reset();
    gx_state.imm.pending = 0;
    acc.count = 0;
    decoded.count = 0;
}

// --- byte readers ----------------------------------------------------------------------------------------------

static inline uint16_t rd16(const uint8_t* p, int be)
{
    return be ? (uint16_t) ((p[0] << 8) | p[1]) : (uint16_t) (p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t* p, int be)
{
    return be ? ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]
              : ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) | ((uint32_t) p[1] << 8) | p[0];
}

static inline float rdf32(const uint8_t* p, int be)
{
    uint32_t u = rd32(p, be);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// --- vertex layout from VCD + VAT ------------------------------------------------------------------------------

struct AttrDesc {
    uint8_t  attr;      // GX_VA_*
    uint8_t  vcd;       // GX_DIRECT / GX_INDEX8 / GX_INDEX16
    uint8_t  cnt, type, frac;
    uint8_t  ncomp;     // components in the array/direct data (pos 2/3, nrm 3/9, tex 1/2, colour 1)
    uint8_t  nidx;      // indices per vertex (1, or 3 for GX_NRM_NBT3)
    uint8_t  csize;     // bytes per component (colour: bytes per colour)
    uint16_t size;      // bytes this attribute occupies in the stream
};

struct VtxLayout {
    AttrDesc a[GX_VCD_ATTRS];
    int      n;
    uint32_t vsize;
};

static inline uint8_t comp_size(int type)
{
    switch (type) {
    case GX_U8: case GX_S8: return 1;
    case GX_U16: case GX_S16: return 2;
    default: return 4;   // GX_F32
    }
}

static inline uint8_t color_size(int type)
{
    switch (type) {
    case GX_RGB565: return 2;
    case GX_RGB8: return 3;
    case GX_RGBX8: return 4;
    case GX_RGBA4: return 2;
    case GX_RGBA6: return 3;
    default: return 4;   // GX_RGBA8
    }
}

// Normals have a fixed fractional format per component type (GXAttr.c GetFracForNrm).
static inline uint8_t nrm_frac(int type) { return type == GX_S8 ? 6 : type == GX_S16 ? 14 : 0; }

static void build_layout(VtxLayout* L, int vtxfmt)
{
    const GxState& s = gx_state;
    L->n = 0;
    L->vsize = 0;
    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; attr++) {
        int vcd = s.vcd[attr];
        if (vcd == GX_NONE) continue;
        AttrDesc& d = L->a[L->n++];
        memset(&d, 0, sizeof d);
        d.attr = (uint8_t) attr;
        d.vcd = (uint8_t) vcd;
        d.nidx = 1;
        if (attr <= GX_VA_TEX7MTXIDX) {          // matrix indices: always one direct u8
            d.vcd = GX_DIRECT;
            d.ncomp = 1; d.csize = 1; d.size = 1;
            L->vsize += 1;
            continue;
        }
        const GxVatAttr& f = s.vat[vtxfmt & 7][attr - GX_VA_POS];
        d.cnt = f.cnt; d.type = f.type; d.frac = f.frac;
        switch (attr) {
        case GX_VA_POS:
            d.ncomp = (f.cnt == GX_POS_XY) ? 2 : 3;
            d.csize = comp_size(f.type);
            break;
        case GX_VA_NRM:
            d.ncomp = (f.cnt == GX_NRM_XYZ) ? 3 : 9;
            d.csize = comp_size(f.type);
            d.frac = nrm_frac(f.type);
            if (f.cnt == GX_NRM_NBT3 && vcd != GX_DIRECT) d.nidx = 3;
            break;
        case GX_VA_CLR0: case GX_VA_CLR1:
            d.ncomp = 1;
            d.csize = color_size(f.type);
            break;
        default:   // GX_VA_TEX0..7
            d.ncomp = (f.cnt == GX_TEX_S) ? 1 : 2;
            d.csize = comp_size(f.type);
            break;
        }
        if (vcd == GX_DIRECT) d.size = (uint16_t) (d.ncomp * d.csize);
        else d.size = (uint16_t) ((vcd == GX_INDEX8 ? 1 : 2) * d.nidx);
        L->vsize += d.size;
    }
}

// --- component decoding ----------------------------------------------------------------------------------------

static inline float fixed_scale(int frac) { return 1.0f / (float) (1u << (frac & 31)); }

// Reads one numeric component of `type` at p (pos/nrm/tex).
static inline float rd_comp(const uint8_t* p, int type, float scale, int be)
{
    switch (type) {
    case GX_U8:  return (float) p[0] * scale;
    case GX_S8:  return (float) (int8_t) p[0] * scale;
    case GX_U16: return (float) rd16(p, be) * scale;
    case GX_S16: return (float) (int16_t) rd16(p, be) * scale;
    default:     return rdf32(p, be);   // GX_F32: frac is ignored
    }
}

// Colour formats (GXCompType for GX_VA_CLRn) -> RGBA8, bytes r,g,b,a (GX_RGBA8()).
static inline uint32_t rd_color(const uint8_t* p, int type, int be)
{
    switch (type) {
    case GX_RGB565: {
        uint16_t v = rd16(p, be);
        unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return GX_RGBA8((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
    }
    case GX_RGB8:  return GX_RGBA8(p[0], p[1], p[2], 255);
    case GX_RGBX8: return GX_RGBA8(p[0], p[1], p[2], 255);
    case GX_RGBA4: {
        uint16_t v = rd16(p, be);
        unsigned r = (v >> 12) & 15, g = (v >> 8) & 15, b = (v >> 4) & 15, a = v & 15;
        return GX_RGBA8(r * 17, g * 17, b * 17, a * 17);
    }
    case GX_RGBA6: {
        uint32_t v = ((uint32_t) p[0] << 16) | ((uint32_t) p[1] << 8) | p[2];   // 24 bits, big-endian packed
        unsigned r = (v >> 18) & 63, g = (v >> 12) & 63, b = (v >> 6) & 63, a = v & 63;
        return GX_RGBA8((r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4), (a << 2) | (a >> 4));
    }
    default: return GX_RGBA8(p[0], p[1], p[2], p[3]);   // GX_RGBA8
    }
}

// GX_TEXMTX0..9 (30..57) -> 0..9, GX_IDENTITY (60) and anything else -> 10.
static inline uint8_t texmtx_slot(unsigned id)
{
    return (id >= GX_TEXMTX0 && id < GX_IDENTITY) ? (uint8_t) ((id - GX_TEXMTX0) / 3) : (uint8_t) 10;
}

static void vertex_defaults(GxVertex* v)
{
    const GxState& s = gx_state;
    memset(v, 0, sizeof *v);
    v->col[0] = v->col[1] = 0xFFFFFFFFu;
    v->mtxidx = (uint8_t) ((s.current_mtx / 3) % GX_PNMTX_COUNT);
    for (int i = 0; i < 8; i++) v->texmtxidx[i] = texmtx_slot(s.tev.texgen[i].mtx);
}

// Decodes one vertex at *pp (advanced past it) into v. Returns 0 when an indexed attribute has no array.
static int decode_vertex(const VtxLayout& L, const uint8_t** pp, int be, GxVertex* v)
{
    const GxState& s = gx_state;
    const uint8_t* p = *pp;
    vertex_defaults(v);
    for (int i = 0; i < L.n; i++) {
        const AttrDesc& d = L.a[i];
        const uint8_t* src[3];
        if (d.vcd == GX_DIRECT) {
            src[0] = p;
            p += d.size;
        } else {
            const GxArray& arr = s.array[d.attr - GX_VA_POS];
            if (!arr.base) {
                re4_log_once("dl_noarray", "GX: attribute %d is indexed but GXSetArray was never called\n", d.attr);
                return 0;
            }
            for (int k = 0; k < d.nidx; k++) {
                unsigned idx = (d.vcd == GX_INDEX8) ? p[0] : rd16(p, be);
                p += (d.vcd == GX_INDEX8) ? 1 : 2;
                src[k] = arr.base + idx * arr.stride;
            }
        }
        switch (d.attr) {
        case GX_VA_PNMTXIDX:
            v->mtxidx = (uint8_t) ((src[0][0] / 3) % GX_PNMTX_COUNT);   // the vertex holds GX_PNMTXn (0..27)
            break;
        case GX_VA_POS: {
            float sc = fixed_scale(d.frac);
            for (int c = 0; c < d.ncomp; c++) v->pos[c] = rd_comp(src[0] + c * d.csize, d.type, sc, be);
            break;
        }
        case GX_VA_NRM: {
            // Only the normal is kept; GX_NRM_NBT/NBT3 binormal and tangent are skipped (the game never
            // uses them). For NBT3 with indices src[1]/src[2] would hold B and T.
            float sc = fixed_scale(d.frac);
            for (int c = 0; c < 3; c++) v->nrm[c] = rd_comp(src[0] + c * d.csize, d.type, sc, be);
            break;
        }
        case GX_VA_CLR0: case GX_VA_CLR1:
            v->col[d.attr - GX_VA_CLR0] = rd_color(src[0], d.type, be);
            break;
        default:
            if (d.attr >= GX_VA_TEX0MTXIDX && d.attr <= GX_VA_TEX7MTXIDX) {
                v->texmtxidx[d.attr - GX_VA_TEX0MTXIDX] = texmtx_slot(src[0][0]);
            } else {   // GX_VA_TEX0..7
                float sc = fixed_scale(d.frac);
                int t = d.attr - GX_VA_TEX0;
                for (int c = 0; c < d.ncomp; c++) v->uv[t][c] = rd_comp(src[0] + c * d.csize, d.type, sc, be);
            }
            break;
        }
    }
    *pp = p;
    return 1;
}

// --- primitive expansion and the draw accumulator ------------------------------------------------------------

static inline int prim_class(int prim)
{
    switch (prim) {
    case GX_PRIM_LINES: case GX_PRIM_LINESTRIP: return GX_PRIM_LINES;
    case GX_PRIM_POINTS: return GX_PRIM_POINTS;
    default: return GX_PRIM_TRIANGLES;
    }
}

void gx_emit_flush(void)
{
    if (acc.count == 0) return;
    if (gx_backend) {
        gx_resolve_dirty();
        gx_backend->draw(acc_prim, acc.data, acc.count);
        gx_state.draws++;
    }
    acc.count = 0;
}

static inline void acc_push3(const GxVertex* v, int a, int b, int c)
{
    gx_vb_push(&acc, &v[a]); gx_vb_push(&acc, &v[b]); gx_vb_push(&acc, &v[c]);
}

// Quads -> two triangles, strips and fans -> triangle lists (same winding), line strips -> line pairs.
static void emit_prim(int prim, const GxVertex* v, int n)
{
    int cls = prim_class(prim);
    if (acc.count && acc_prim != cls) gx_emit_flush();
    acc_prim = cls;
    switch (prim) {
    case GX_PRIM_QUADS:
        for (int i = 0; i + 3 < n; i += 4) { acc_push3(v, i, i + 1, i + 2); acc_push3(v, i, i + 2, i + 3); }
        break;
    case GX_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) acc_push3(v, i, i + 1, i + 2);
        break;
    case GX_PRIM_TRISTRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) acc_push3(v, i + 1, i, i + 2);
            else acc_push3(v, i, i + 1, i + 2);
        }
        break;
    case GX_PRIM_TRIFAN:
        for (int i = 1; i + 1 < n; i++) acc_push3(v, 0, i, i + 1);
        break;
    case GX_PRIM_LINES:
        for (int i = 0; i + 1 < n; i += 2) { gx_vb_push(&acc, &v[i]); gx_vb_push(&acc, &v[i + 1]); }
        break;
    case GX_PRIM_LINESTRIP:
        for (int i = 0; i + 1 < n; i++) { gx_vb_push(&acc, &v[i]); gx_vb_push(&acc, &v[i + 1]); }
        break;
    case GX_PRIM_POINTS:
        for (int i = 0; i < n; i++) gx_vb_push(&acc, &v[i]);
        break;
    default:
        re4_log_once("emit_prim", "GX: unknown primitive opcode 0x%02x\n", prim);
        break;
    }
}

// Decodes `count` vertices of `prim`/`vtxfmt` from data and queues the expanded primitive.
// Returns the number of stream bytes consumed (count * vsize) or 0 on error.
static size_t decode_primitive(int prim, int vtxfmt, unsigned count, const uint8_t* data, size_t avail, int be)
{
    VtxLayout L;
    build_layout(&L, vtxfmt);
    size_t need = (size_t) count * L.vsize;
    if (need > avail) {
        re4_log_once("dl_truncated", "GX: primitive 0x%02x x%u needs %u bytes, %u left in the stream\n",
                     prim, count, (unsigned) need, (unsigned) avail);
        return 0;
    }
    decoded.count = 0;
    if (!gx_vb_reserve(&decoded, count)) return 0;
    const uint8_t* p = data;
    for (unsigned i = 0; i < count; i++) {
        if (!decode_vertex(L, &p, be, &decoded.data[i])) return 0;
    }
    decoded.count = count;
    emit_prim(prim, decoded.data, (int) count);
    return need;
}

// --- the stream interpreter ------------------------------------------------------------------------------------

static inline int is_prim_opcode(unsigned op)
{
    switch (op & 0xF8) {
    case GX_PRIM_QUADS: case GX_PRIM_TRIANGLES: case GX_PRIM_TRISTRIP: case GX_PRIM_TRIFAN:
    case GX_PRIM_LINES: case GX_PRIM_LINESTRIP: case GX_PRIM_POINTS:
        return 1;
    default:
        return 0;
    }
}

extern "C" void gx_run_stream(const uint8_t* stream, size_t bytes, int big_endian)
{
    const uint8_t* p = stream;
    const uint8_t* end = stream + bytes;
    while (p < end) {
        unsigned op = *p++;
        if (op == 0x00) continue;                       // NOP (32-byte padding at the end of lists)
        if (is_prim_opcode(op)) {
            if (end - p < 2) { re4_log_once("dl_short", "GX: stream ends inside a primitive header\n"); return; }
            unsigned count = rd16(p, big_endian);
            p += 2;
            size_t used = decode_primitive(op & 0xF8, op & 7, count, p, (size_t) (end - p), big_endian);
            if (used == 0 && count != 0) return;
            p += used;
            continue;
        }
        // Register writes do not appear in model data; skip them by size (GXCommandList.h).
        size_t skip;
        switch (op) {
        case 0x08: skip = 1 + 4; break;                 // CP register: u8 addr, u32 value
        case 0x10: {                                    // XF registers: u16 (count-1), u16 addr, count*u32
            if (end - p < 4) return;
            unsigned n = rd16(p, big_endian) + 1;
            skip = 4 + (size_t) n * 4;
            break;
        }
        case 0x61: skip = 4; break;                     // BP register
        case 0x20: case 0x28: case 0x30: case 0x38: skip = 4; break;   // indexed XF loads (u16 idx, u16 size|addr)
        case 0x40: skip = 8; break;                     // call display list: u32 addr, u32 size (not followed)
        case 0x48: skip = 0; break;                     // invalidate vertex cache
        default:
            re4_log_once("dl_opcode", "GX: unknown FIFO opcode 0x%02x at offset %u; stopping\n", op, (unsigned) (p - 1 - stream));
            return;
        }
        re4_log_once("dl_regwrite", "GX: register write opcode 0x%02x in a display list (skipped)\n", op);
        if ((size_t) (end - p) < skip) return;
        p += skip;
    }
}

// --- immediate mode -------------------------------------------------------------------------------------------------

// Opens a primitive: the FIFO proxy appends vertex bytes until nverts * vsize have arrived.
extern "C" void GXBegin(int type, int vtxfmt, u16 nverts)
{
    GxState& s = gx_state;
    if (s.imm.pending) gx_flush_pending("GXBegin");
    if (!is_prim_opcode((unsigned) type) || (type & 7)) {
        re4_log_once("gxbegin_prim", "GXBegin: bad primitive 0x%02x\n", type);
        return;
    }
    VtxLayout L;
    build_layout(&L, vtxfmt);
    s.imm.prim = (uint8_t) type;
    s.imm.vtxfmt = (uint8_t) (vtxfmt & 7);
    s.imm.nverts = nverts;
    s.imm.vsize = L.vsize;
    s.imm.expected = (uint32_t) nverts * L.vsize;

    if (s.dl.active) {
        // Recording: the primitive goes into the caller's buffer as a native-order FIFO stream.
        uint32_t need = 3 + s.imm.expected;
        if (s.dl.count + need > s.dl.size) {
            if (!s.dl.overflow) re4_log("GXBeginDisplayList: list of %u bytes overflowed\n", (unsigned) s.dl.size);
            s.dl.overflow = 1;
            if (!gx_bb_reserve(&imm_buf, s.imm.expected ? s.imm.expected : 1)) return;
            re4_gx_fifo_cursor.wr = imm_buf.data;             // scratch: bytes are dropped
            re4_gx_fifo_cursor.end = imm_buf.data + s.imm.expected;
        } else {
            uint8_t* hdr = s.dl.base + s.dl.count;
            hdr[0] = (uint8_t) (type | (vtxfmt & 7));
            memcpy(hdr + 1, &nverts, 2);
            re4_gx_fifo_cursor.wr = hdr + 3;
            re4_gx_fifo_cursor.end = hdr + 3 + s.imm.expected;
        }
        s.imm.pending = s.imm.expected != 0;
        if (!s.imm.pending && !s.dl.overflow) s.dl.count += need;   // zero-size primitive: header only
        return;
    }

    if (s.imm.expected == 0) {
        s.imm.pending = 0;
        cursor_reset();
        if (nverts) decode_primitive(type, vtxfmt & 7, nverts, imm_buf.data, 0, 0);   // no direct data at all
        return;
    }
    if (!gx_bb_reserve(&imm_buf, s.imm.expected)) {
        re4_log_once("imm_oom", "GXBegin: out of memory for %u bytes\n", (unsigned) s.imm.expected);
        return;
    }
    re4_gx_fifo_cursor.wr = imm_buf.data;
    re4_gx_fifo_cursor.end = imm_buf.data + s.imm.expected;
    s.imm.pending = 1;
}

// The FIFO proxy reached `end`: all vertices of the open primitive are in.
extern "C" void re4_gx_fifo_complete(void)
{
    GxState& s = gx_state;
    if (!s.imm.pending) { cursor_reset(); return; }
    s.imm.pending = 0;
    cursor_reset();
    if (s.dl.active) {
        if (!s.dl.overflow) s.dl.count += 3 + s.imm.expected;
        return;
    }
    decode_primitive(s.imm.prim, s.imm.vtxfmt, s.imm.nverts, imm_buf.data, s.imm.expected, 0);
}

extern "C" void re4_gx_fifo_overflow(const void* p, unsigned n)
{
    (void) p;
    re4_log_once("fifo_overflow", "GX: %u-byte FIFO write with no open primitive (or past its %u vertices); dropped\n",
                 n, (unsigned) gx_state.imm.nverts);
}

// A state change, GXBegin, GXCallDisplayList or sync arrived while a primitive was still short of
// vertices: draw the complete ones, drop the partial tail.
void gx_flush_pending(const char* who)
{
    GxState& s = gx_state;
    if (!s.imm.pending) return;
    uint8_t* start = s.dl.active ? (s.dl.overflow ? imm_buf.data : s.dl.base + s.dl.count + 3) : imm_buf.data;
    uint32_t have = (uint32_t) (re4_gx_fifo_cursor.wr - start);
    uint32_t complete = s.imm.vsize ? have / s.imm.vsize : 0;
    re4_log_once(who, "GX: %s while GXBegin(0x%02x, %u verts) had %u complete vertices; partial primitive flushed\n",
                 who, s.imm.prim, s.imm.nverts, complete);
    s.imm.pending = 0;
    cursor_reset();
    if (s.dl.active) {
        if (!s.dl.overflow) {
            uint8_t* hdr = s.dl.base + s.dl.count;
            uint16_t c = (uint16_t) complete;
            memcpy(hdr + 1, &c, 2);
            s.dl.count += 3 + complete * s.imm.vsize;
        }
        return;
    }
    if (complete) decode_primitive(s.imm.prim, s.imm.vtxfmt, complete, imm_buf.data, complete * s.imm.vsize, 0);
}

extern "C" void GXCallDisplayList(void* list, u32 nbytes)
{
    GxState& s = gx_state;
    if (s.imm.pending) gx_flush_pending("GXCallDisplayList");
    if (s.dl.active) {
        re4_log_once("dl_nested", "GXCallDisplayList inside GXBeginDisplayList is not supported; ignored\n");
        return;
    }
    if (!list || !nbytes) return;
    gx_run_stream((const uint8_t*) list, nbytes, dl_big_endian);
}

// --- display-list recording (GXDisplayList.c) -----------------------------------------------------------------

// The SDK redirects the CPU FIFO into `list` and saves/restores GXData around it; the port records the
// primitives GXBegin + FIFO writes produce and restores the state at GXEndDisplayList. State setters
// called while recording are applied live (and undone at the end) but not recorded.
extern "C" void GXBeginDisplayList(void* list, u32 size)
{
    GxState& s = gx_state;
    if (s.imm.pending) gx_flush_pending("GXBeginDisplayList");
    gx_emit_flush();
    if (s.dl.active) { re4_log_once("dl_reentry", "GXBeginDisplayList: display list already in progress\n"); return; }
    if (!dl_saved_state) dl_saved_state = (GxState*) malloc(sizeof(GxState));
    if (dl_saved_state) memcpy(dl_saved_state, &s, sizeof(GxState));
    s.dl.active = 1;
    s.dl.base = (uint8_t*) list;
    s.dl.size = size;
    s.dl.count = 0;
    s.dl.overflow = 0;
}

extern "C" u32 GXEndDisplayList(void)
{
    GxState& s = gx_state;
    if (!s.dl.active) { re4_log_once("dl_noend", "GXEndDisplayList: no display list in progress\n"); return 0; }
    if (s.imm.pending) gx_flush_pending("GXEndDisplayList");
    uint32_t count = s.dl.overflow ? 0 : s.dl.count;
    // pad to 32 bytes with NOPs like the hardware FIFO does
    while (!s.dl.overflow && (s.dl.count & 31) && s.dl.count < s.dl.size) s.dl.base[s.dl.count++] = 0;
    if (dl_saved_state) {
        GxDlRecord dl = s.dl;
        memcpy(&s, dl_saved_state, sizeof(GxState));
        s.dl = dl;
        s.dirty = GX_DIRTY_ALL;
    }
    s.dl.active = 0;
    return count ? s.dl.count : 0;
}

// --- GXDrawTorus (GXDraw.c): the only GXDraw* primitive the game uses (sofdec.cpp) -----------------------------

extern "C" void GXDrawTorus(f32 rc, u8 numc, u8 numt)
{
    GxState& s = gx_state;
    if (numc == 0 || numt == 0) return;
    // GetVertState: save the VCD and VTXFMT3, then pos/nrm direct f32 (+ tex0 direct f32 if present)
    uint8_t saved_vcd[GX_VCD_ATTRS];
    uint8_t saved_has_nrm = s.has_nrm, saved_has_binrm = s.has_binrm, saved_nrm_type = s.nrm_type;
    GxVatAttr saved_vat[GX_VAT_ATTRS];
    memcpy(saved_vcd, s.vcd, sizeof saved_vcd);
    memcpy(saved_vat, s.vat[GX_VTXFMT3], sizeof saved_vat);
    int ttype = s.vcd[GX_VA_TEX0];

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    if (ttype != GX_NONE) {
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }

    const f32 twopi = 6.2831855f;
    f32 rt = 1.0f - rc;
    for (int i = 0; i < numc; i++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16) ((numt + 1) * 2));
        for (int j = 0; j <= numt; j++) {
            for (int k = 1; k >= 0; k--) {
                f32 sa = (f32) ((i + k) % numc);
                f32 t = (f32) (j % numt);
                f32 x = (rt - rc * cosf(sa * twopi / numc)) * cosf(t * twopi / numt);
                f32 y = (rt - rc * cosf(sa * twopi / numc)) * sinf(t * twopi / numt);
                f32 z = rc * sinf(sa * twopi / numc);
                GXPosition3f32(x, y, z);
                x = -cosf(t * twopi / numt) * cosf(sa * twopi / numc);
                y = -sinf(t * twopi / numt) * cosf(sa * twopi / numc);
                z = sinf(sa * twopi / numc);
                GXNormal3f32(x, y, z);
                if (ttype != GX_NONE) GXTexCoord2f32((i + k) / (f32) numc, j / (f32) numt);
            }
        }
    }

    // RestoreVertState
    memcpy(s.vcd, saved_vcd, sizeof saved_vcd);
    s.has_nrm = saved_has_nrm; s.has_binrm = saved_has_binrm; s.nrm_type = saved_nrm_type;
    memcpy(s.vat[GX_VTXFMT3], saved_vat, sizeof saved_vat);
}
