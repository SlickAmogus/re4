// gx_nv2a.cpp — NV2A (pbkit) backend for the GX shim (CT-15, first pass).
//
// Implements the GxBackend vtable (re4port/gx_shim.h) on nxdk's pbkit. Structure and every pbkit
// mechanism (init sequence, per-frame reset/flip, write-combined vertex pool, texture register block,
// passthrough vertex program, combiner words) follow Starship's proven backend
// (Starship/libultraship/src/graphic/Fast3D/backends/gfx_nv2a.cpp) — see xbox/docs/06-LESSONS-FROM-PORTS.md.
//
// Pipeline of this pass (simplest correct path; GPU transform comes later):
//   CPU : position matrix (by GxVertex::mtxidx) -> projection -> clip space, near-plane clip (z >= -w,
//         GX's near plane), texgen (MTX2x4/MTX3x4 from TEXn/POS/NRM with the per-vertex texmtx slot),
//         GX colour-channel lighting (8 lights, DF_NONE/SIGN/CLAMP, AF_NONE/SPOT; AF_SPEC approximated).
//   VP  : shaders/gx_passthrough.vp (vp20, pre-assembled into shaders/gx_passthrough.inl): clip -> screen
//         through the viewport matrix in c[0..3], divide by w, forward COL0/COL1/TEX0..3.
//   PS  : the TEV program compiled to register combiners (up to 8 general stages, per-stage constants,
//         final combiner = TEVPREV). Unmappable programs fall back to "texture x vertex colour" with one
//         log line per pipeline hash.
//
// GX conventions applied here (Dolphin's VideoCommon is the reference):
//   - GX projection yields z_ndc in [-1 (near), 0 (far)]; depth = (farz + z_ndc*(farz-nearz)) * 0xFFFFFF
//     (pbkit's depth buffer is Z24S8, pbkit.c pb_DepthFmt).
//   - The EFB (init width x height, 640x448 for RE4) is mapped onto the back buffer with a uniform scale
//     and centred: on 640x480 that is 1:1 with a 16-line border, like the VI showing a 448-line XFB.
//   - Front faces are clockwise on screen (GXSetCullMode). TODO(hw): verify NV2A FRONT_FACE on hardware.
//
// Not done in this pass (TODO, see xbox/cloud-tasks/progress/CT-15.md): GPU-side transform/lighting,
// render targets and GPU EFB copies (copy_to_texture is a slow CPU readback of the colour buffer,
// colour formats only), GXPeekZ (returns far), fog, indirect stages / texture shaders, swap tables other
// than identity, dst-alpha, zero-copy texture binding, a texture sub-allocator.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"
#include "gx_pipeline.h"
#include "gx_vertex.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <hal/video.h>
#include <hal/debug.h>
#include <pbkit/pbkit.h>
#include <pbkit/nv_regs.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

extern "C" const GxBackend gx_nv2a_backend;   // TODO(lead): declare in re4port/gx_shim.h next to gx_null_backend

// Maximum contiguous physical address for GPU-visible allocations (Starship MAXRAM).
#define NV_MAXRAM 0x03FFAFFF
#define MASK(mask, val) (((val) << (__builtin_ffs(mask) - 1)) & (mask))

// Set to 0 to disable the scissor (window clip) path during bring-up if it clips everything.
#ifndef NV_USE_SCISSOR
#define NV_USE_SCISSOR 1
#endif

// ---- raw GX enum values (include/dolphin/gx/GXEnum.h); gx_pipeline.h keeps the game's raw values ----
enum {
    CC_CPREV = 0, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2, CC_TEXC, CC_TEXA, CC_RASC, CC_RASA,
    CC_ONE, CC_HALF, CC_KONST, CC_ZERO
};
enum { CA_APREV = 0, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO };
enum { TEV_ADD = 0, TEV_SUB = 1 };
enum { TB_ZERO = 0, TB_ADDHALF = 1, TB_SUBHALF = 2 };
enum { CS_SCALE_1 = 0, CS_SCALE_2 = 1, CS_SCALE_4 = 2, CS_DIVIDE_2 = 3 };
enum { CH_COLOR0 = 0, CH_COLOR1, CH_ALPHA0, CH_ALPHA1, CH_COLOR0A0, CH_COLOR1A1, CH_COLOR_ZERO, CH_NULL = 0xFF };
enum { TG_MTX3x4 = 0, TG_MTX2x4 = 1 };
enum { TGS_POS = 0, TGS_NRM = 1, TGS_TEX0 = 4, TGS_TEX7 = 11, TGS_COLOR0 = 19, TGS_COLOR1 = 20 };
enum { SRC_REG = 0, SRC_VTX = 1 };
enum { DF_NONE = 0, DF_SIGN = 1, DF_CLAMP = 2 };
enum { AF_SPEC = 0, AF_SPOT = 1, AF_NONE = 2 };
enum { CULL_NONE = 0, CULL_FRONT, CULL_BACK, CULL_ALL };
enum { BM_NONE = 0, BM_BLEND, BM_LOGIC, BM_SUBTRACT };
enum { CMP_NEVER = 0, CMP_ALWAYS = 7 };
enum { AOP_AND = 0, AOP_OR, AOP_XOR, AOP_XNOR };

// =====================================================================================================
// TEV -> register combiner compiler (pure: no pbkit calls, so it can be unit-tested on the host later)
// =====================================================================================================
//
// A TEV stage computes out = (d +- lerp(a, b, c) + bias) * scale. lerp(a,b,c) = a*(1-c) + b*c, so each
// portion (colour / alpha) is a sum of up to four products: two from the lerp (fewer for the usual
// shapes: c = 0/1, a or b = 0), d*1, and a 0.5 bias. A general combiner stage computes A*B + C*D, so a
// portion needs 1 stage for <= 2 products and 1 more stage per extra product (accumulator chain).
// Register mapping: TEVPREV -> spare0 (the final combiner outputs spare0), TEVREG0..2 -> spare1, then
// secondary colour (when no stage rasterises COLOR1A1), then the registers of unused texture units.
// A TEV register read before any stage wrote it is its GXSetTevColor value; it and the konst colour go
// into the stage's FACTOR0/FACTOR1 (COMBINER_CONTROL FACTORx_EACH_STAGE), rgb and alpha parts separately.

enum {
    R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_PRI = 4, R_SEC = 5, R_TEX0 = 8, R_SPARE0 = 12, R_SPARE1 = 13,
    R_PENDING = 0x20,   // operand still refers to a per-TEV-stage constant need (resolved per combiner stage)
};
// Input mappings (NV097_SET_COMBINER_*_ICW_*_MAP).
enum { M_UID = 0, M_UINV = 1, M_EXPN = 2, M_EXPNEG = 3, M_HBN = 4, M_HBNEG = 5, M_SID = 6, M_SNEG = 7 };
// Output ops (NV097_SET_COMBINER_COLOR_OCW_OP / ALPHA_OCW_OP share the values).
enum { OP_NOSHIFT = 0, OP_NOSHIFT_BIAS = 1, OP_SHL1 = 2, OP_SHL1_BIAS = 3, OP_SHL2 = 4, OP_SHR1 = 6 };

struct CbOpnd { uint8_t reg, alpha, map; };
struct CbTerm { CbOpnd x, y; };
struct CbHalf { CbOpnd a, b, c, d; uint8_t dst; uint8_t op; };   // dst 0 = no-op (outputs discarded)

// A constant value source: what goes into the rgb or the alpha part of a per-stage FACTOR0/FACTOR1.
enum { CK_NONE = 0, CK_KCSEL, CK_KASEL, CK_REG_RGB, CK_REG_A };
struct CbCSrc { uint8_t kind, idx; };
struct CbNeed { uint8_t part; CbCSrc src; };    // part 0 = rgb, 1 = alpha

enum { NV_MAX_STAGES = 8, NV_MAX_UNITS = 4 };

struct NvProgram {
    uint64_t hash;
    uint8_t  valid;          // cache slot in use
    uint8_t  mapped;         // 0 = fallback program (unmapped GX pipeline)
    uint8_t  nstages;
    uint8_t  nunits;
    uint8_t  unit_map[NV_MAX_UNITS];     // GX texmap sampled by NV2A texture unit u
    uint8_t  unit_coord[NV_MAX_UNITS];   // GX texcoord feeding unit u
    uint8_t  sec_is_color1;              // secondary colour carries GX COLOR1A1
    uint32_t color_icw[NV_MAX_STAGES], color_ocw[NV_MAX_STAGES];
    uint32_t alpha_icw[NV_MAX_STAGES], alpha_ocw[NV_MAX_STAGES];
    CbCSrc   slot_rgb[NV_MAX_STAGES][2], slot_a[NV_MAX_STAGES][2];
    uint32_t final_cw0, final_cw1;
};

static inline CbOpnd op_mk(uint8_t reg, uint8_t alpha, uint8_t map) { CbOpnd o = { reg, alpha, map }; return o; }
static inline CbOpnd op_zero(void) { return op_mk(R_ZERO, 0, M_UID); }
static inline CbOpnd op_one(void)  { return op_mk(R_ZERO, 0, M_UINV); }
static inline CbOpnd op_half(void) { return op_mk(R_ZERO, 0, M_HBNEG); }   // -(0 - 0.5) = 0.5
static inline bool op_is_zero(CbOpnd o) { return o.reg == R_ZERO && (o.map == M_UID || o.map == M_SID || o.map == M_SNEG); }
static inline bool op_is_one(CbOpnd o)  { return o.reg == R_ZERO && o.map == M_UINV; }
static inline bool op_is_half(CbOpnd o) { return o.reg == R_ZERO && o.map == M_HBNEG; }
static inline bool op_eq(CbOpnd a, CbOpnd b) { return a.reg == b.reg && a.alpha == b.alpha && a.map == b.map; }

static bool op_inv(CbOpnd o, CbOpnd* out)   // 1 - o
{
    if (op_is_zero(o)) { *out = op_one(); return true; }
    if (op_is_one(o))  { *out = op_zero(); return true; }
    if (op_is_half(o)) { *out = o; return true; }
    if (o.map != M_UID) return false;
    o.map = M_UINV;
    *out = o;
    return true;
}

static bool op_neg(CbOpnd* o)   // -o in place
{
    if (op_is_zero(*o)) return true;
    if (op_is_one(*o))  { *o = op_mk(R_ZERO, 0, M_EXPN); return true; }    // 2*0 - 1 = -1
    if (op_is_half(*o)) { *o = op_mk(R_ZERO, 0, M_HBN); return true; }     // 0 - 0.5
    if (o->map == M_UID || o->map == M_SID) { o->map = M_SNEG; return true; }
    return false;
}

static bool term_neg(CbTerm* t)
{
    CbOpnd x = t->x, y = t->y;
    if (op_neg(&x)) { t->x = x; return true; }
    if (op_neg(&y)) { t->y = y; return true; }
    return false;
}

// Per-compile context.
struct CbCtx {
    const GxTevProgram* t;
    int      unit_of_stage[GX_MAX_TEV_STAGES_PORT];   // -1 = stage samples nothing
    uint8_t  ras_reg[GX_MAX_TEV_STAGES_PORT];         // R_PRI / R_SEC / R_ZERO
    uint8_t  phys[4];                                 // TEV PREV/REG0..2 -> combiner register
    uint8_t  written[4][2];                           // [reg][part] written by an earlier stage
    uint8_t  scratch;                                 // free temp for multi-stage expansions (0 = none)
    CbNeed   needs[8];
    int      nneeds;
    const char* fail;
};

static bool swap_is_identity(const GxTevProgram* t, uint8_t sel)
{
    if (sel > 3) return false;
    const uint8_t* s = t->swap_table[sel];
    return s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3;
}

static CbOpnd need_const(CbCtx* c, uint8_t part, uint8_t kind, uint8_t idx)
{
    for (int i = 0; i < c->nneeds; i++) {
        if (c->needs[i].part == part && c->needs[i].src.kind == kind && c->needs[i].src.idx == idx)
            return op_mk((uint8_t) (R_PENDING + i), part, M_UID);
    }
    if (c->nneeds >= 8) { c->fail = "too many constants in one stage"; return op_zero(); }
    CbNeed& n = c->needs[c->nneeds];
    n.part = part; n.src.kind = kind; n.src.idx = idx;
    return op_mk((uint8_t) (R_PENDING + c->nneeds++), part, M_UID);
}

// TEV register read: the combiner register once a stage has written it, else its GXSetTevColor value.
static CbOpnd read_reg(CbCtx* c, int reg, int part)
{
    if (c->written[reg][part]) return op_mk(c->phys[reg], (uint8_t) part, M_UID);
    return need_const(c, (uint8_t) part, part ? CK_REG_A : CK_REG_RGB, (uint8_t) reg);
}

static bool color_arg(CbCtx* c, int s, uint8_t arg, CbOpnd* o)
{
    const GxTevStage& st = c->t->stage[s];
    switch (arg) {
    case CC_CPREV: case CC_C0: case CC_C1: case CC_C2: *o = read_reg(c, arg / 2, 0); return true;
    case CC_APREV: case CC_A0: case CC_A1: case CC_A2: *o = read_reg(c, arg / 2, 1); return true;
    case CC_TEXC: case CC_TEXA:
        if (c->unit_of_stage[s] < 0) { *o = op_zero(); return true; }
        if (!swap_is_identity(c->t, st.tex_swap)) { c->fail = "texture swap table"; return false; }
        *o = op_mk((uint8_t) (R_TEX0 + c->unit_of_stage[s]), arg == CC_TEXA, M_UID);
        return true;
    case CC_RASC: case CC_RASA:
        if (!swap_is_identity(c->t, st.ras_swap)) { c->fail = "raster swap table"; return false; }
        *o = op_mk(c->ras_reg[s], arg == CC_RASA, M_UID);
        return true;
    case CC_ONE:   *o = op_one(); return true;
    case CC_HALF:  *o = op_half(); return true;
    case CC_KONST: *o = need_const(c, 0, CK_KCSEL, st.kcsel); return true;
    case CC_ZERO:  *o = op_zero(); return true;
    default: c->fail = "colour arg TEXRRR/GGG/BBB"; return false;
    }
}

static bool alpha_arg(CbCtx* c, int s, uint8_t arg, CbOpnd* o)
{
    const GxTevStage& st = c->t->stage[s];
    switch (arg) {
    case CA_APREV: case CA_A0: case CA_A1: case CA_A2: *o = read_reg(c, arg, 1); return true;
    case CA_TEXA:
        if (c->unit_of_stage[s] < 0) { *o = op_zero(); return true; }
        if (!swap_is_identity(c->t, st.tex_swap)) { c->fail = "texture swap table"; return false; }
        *o = op_mk((uint8_t) (R_TEX0 + c->unit_of_stage[s]), 1, M_UID);
        return true;
    case CA_RASA:
        if (!swap_is_identity(c->t, st.ras_swap)) { c->fail = "raster swap table"; return false; }
        *o = op_mk(c->ras_reg[s], 1, M_UID);
        return true;
    case CA_KONST: *o = need_const(c, 1, CK_KASEL, st.kasel); return true;
    case CA_ZERO:  *o = op_zero(); return true;
    default: c->fail = "alpha arg"; return false;
    }
}

// One portion (colour or alpha) of a TEV stage as a chain of combiner halves. Returns the number of
// halves (1..3), 0 on failure.
static int compile_portion(CbCtx* c, int s, bool alpha, CbHalf out[3])
{
    const GxTevStage& st = c->t->stage[s];
    const GxTevOpParams& op = alpha ? st.alpha_op : st.color_op;
    const uint8_t* in = alpha ? st.alpha_in : st.color_in;
    CbOpnd a, b, cc, d;
    CbOpnd* args[4] = { &a, &b, &cc, &d };
    for (int i = 0; i < 4; i++) {
        if (!(alpha ? alpha_arg(c, s, in[i], args[i]) : color_arg(c, s, in[i], args[i]))) return 0;
    }
    if (c->fail) return 0;
    if (op.op > TEV_SUB) { c->fail = "TEV compare op"; return 0; }
    if (d.reg != R_ZERO && d.reg < R_PENDING) d.map = M_SID;   // TEV's d input is signed

    CbTerm t[4];
    int n = 0;
    if (op_is_zero(cc)) {
        if (!op_is_zero(a)) { t[n].x = a; t[n].y = op_one(); n++; }
    } else if (op_is_one(cc)) {
        if (!op_is_zero(b)) { t[n].x = b; t[n].y = op_one(); n++; }
    } else if (op_eq(a, b)) {
        if (!op_is_zero(a)) { t[n].x = a; t[n].y = op_one(); n++; }
    } else {
        if (!op_is_zero(a)) {
            CbOpnd ic;
            if (!op_inv(cc, &ic)) { c->fail = "lerp factor with a mapping"; return 0; }
            t[n].x = a; t[n].y = ic; n++;
        }
        if (!op_is_zero(b)) { t[n].x = b; t[n].y = cc; n++; }
    }
    if (op.op == TEV_SUB) {
        for (int i = 0; i < n; i++) if (!term_neg(&t[i])) { c->fail = "cannot negate term"; return 0; }
    }
    if (!op_is_zero(d)) { t[n].x = d; t[n].y = op_one(); n++; }

    uint8_t fop;
    switch (op.scale) {
    case CS_SCALE_2:  fop = OP_SHL1; break;
    case CS_SCALE_4:  fop = OP_SHL2; break;
    case CS_DIVIDE_2: fop = OP_SHR1; break;
    default:          fop = OP_NOSHIFT; break;
    }
    if (op.bias == TB_SUBHALF && (fop == OP_NOSHIFT || fop == OP_SHL1)) {
        fop = (fop == OP_NOSHIFT) ? OP_NOSHIFT_BIAS : OP_SHL1_BIAS;   // combiner bias precedes the shift, like TEV
    } else if (op.bias == TB_SUBHALF) {
        t[n].x = op_mk(R_ZERO, 0, M_HBN); t[n].y = op_one(); n++;   // -0.5
    } else if (op.bias == TB_ADDHALF) {
        t[n].x = op_half(); t[n].y = op_one(); n++;
    }

    uint8_t dst = c->phys[op.out_reg & 3];
    if (n <= 2) {
        CbHalf& h = out[0];
        h.a = n > 0 ? t[0].x : op_zero(); h.b = n > 0 ? t[0].y : op_zero();
        h.c = n > 1 ? t[1].x : op_zero(); h.d = n > 1 ? t[1].y : op_zero();
        h.dst = dst; h.op = fop;
        return 1;
    }
    // 3 or 4 products: acc = t0 + t1; acc += t2; [dst = acc + t3]. The accumulator may be dst itself
    // unless a later product still reads dst's part (TEV reads all inputs before writing).
    uint8_t acc = dst;
    for (int i = 2; i < n; i++) {
        const CbOpnd* ops[2] = { &t[i].x, &t[i].y };
        for (int k = 0; k < 2; k++) if (ops[k]->reg == dst && ops[k]->alpha == (alpha ? 1 : 0)) acc = c->scratch;
    }
    if (!acc) { c->fail = "no scratch register for a multi-stage TEV stage"; return 0; }
    int nh = 0;
    out[nh].a = t[0].x; out[nh].b = t[0].y; out[nh].c = t[1].x; out[nh].d = t[1].y;
    out[nh].dst = acc; out[nh].op = OP_NOSHIFT; nh++;
    for (int i = 2; i < n; i++) {
        bool last = (i == n - 1);
        out[nh].a = op_mk(acc, alpha ? 1 : 0, M_SID); out[nh].b = op_one();
        out[nh].c = t[i].x; out[nh].d = t[i].y;
        out[nh].dst = last ? dst : acc; out[nh].op = last ? fop : OP_NOSHIFT; nh++;
    }
    return nh;
}

static inline uint32_t icw_field(CbOpnd o)
{
    return (uint32_t) (o.reg & 0xF) | ((uint32_t) (o.alpha & 1) << 4) | ((uint32_t) (o.map & 7) << 5);
}
static inline uint32_t icw_word(const CbHalf& h)
{
    return (icw_field(h.a) << 24) | (icw_field(h.b) << 16) | (icw_field(h.c) << 8) | icw_field(h.d);
}

// Resolves pending constant operands of a half into FACTOR0/1 of combiner stage k.
static bool resolve_half(CbCtx* c, NvProgram* p, int k, CbHalf* h)
{
    CbOpnd* ops[4] = { &h->a, &h->b, &h->c, &h->d };
    for (int i = 0; i < 4; i++) {
        CbOpnd* o = ops[i];
        if (o->reg < R_PENDING) continue;
        const CbNeed& n = c->needs[o->reg - R_PENDING];
        bool done = false;
        for (int slot = 0; slot < 2 && !done; slot++) {
            CbCSrc* part = n.part ? &p->slot_a[k][slot] : &p->slot_rgb[k][slot];
            if (part->kind == CK_NONE) *part = n.src;
            if (part->kind == n.src.kind && part->idx == n.src.idx) {
                o->reg = (uint8_t) (R_C0 + slot); o->alpha = n.part; done = true;
            }
        }
        if (!done) { c->fail = "more than two distinct constants in one combiner stage"; return false; }
    }
    return true;
}

static uint32_t color_ocw_word(const CbHalf& h)
{
    return MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, (uint32_t) h.dst) | MASK(NV097_SET_COMBINER_COLOR_OCW_OP, (uint32_t) h.op);
}
static uint32_t alpha_ocw_word(const CbHalf& h)
{
    return MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, (uint32_t) h.dst) | MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, (uint32_t) h.op);
}

// Compiles `t` into `p`. Returns false (with *why) when the program does not fit this mapping.
static bool nv_compile(const GxTevProgram* t, NvProgram* p, const char** why)
{
    static CbCtx c;
    memset(&c, 0, sizeof c);
    c.t = t;
    memset(p->slot_rgb, 0, sizeof p->slot_rgb);
    memset(p->slot_a, 0, sizeof p->slot_a);
    memset(p->color_icw, 0, sizeof p->color_icw); memset(p->color_ocw, 0, sizeof p->color_ocw);
    memset(p->alpha_icw, 0, sizeof p->alpha_icw); memset(p->alpha_ocw, 0, sizeof p->alpha_ocw);
    p->nunits = 0;
    p->sec_is_color1 = 0;
    p->mapped = 1;
    int nst = t->num_stages ? t->num_stages : 1;
    if (nst > GX_MAX_TEV_STAGES_PORT) nst = GX_MAX_TEV_STAGES_PORT;
    if (t->num_ind_stages) { *why = "indirect texture stages"; return false; }

    // 1. texture units: one per distinct (texmap, texcoord) pair a stage samples
    for (int s = 0; s < nst; s++) {
        const GxTevStage& st = t->stage[s];
        c.unit_of_stage[s] = -1;
        bool samples = false;
        for (int i = 0; i < 4; i++) {
            if (st.color_in[i] == CC_TEXC || st.color_in[i] == CC_TEXA) samples = true;
            if (st.alpha_in[i] == CA_TEXA) samples = true;
        }
        if (!samples || (st.map & 0x100) || (st.map & 0xFF) == 0xFF) continue;
        uint8_t map = (uint8_t) (st.map & 7), coord = st.coord == 0xFF ? 0 : (uint8_t) (st.coord & 7);
        int u;
        for (u = 0; u < p->nunits; u++) if (p->unit_map[u] == map && p->unit_coord[u] == coord) break;
        if (u == p->nunits) {
            if (p->nunits >= NV_MAX_UNITS) { *why = "more than 4 texture units"; return false; }
            p->unit_map[u] = map; p->unit_coord[u] = coord; p->nunits++;
        }
        c.unit_of_stage[s] = u;
    }
    // 2. rasterised colour per stage: COLOR0A0 -> primary, COLOR1A1 -> secondary
    for (int s = 0; s < nst; s++) {
        uint8_t ch = t->stage[s].chan;
        if (ch == CH_COLOR0A0 || ch == CH_COLOR0 || ch == CH_ALPHA0) c.ras_reg[s] = R_PRI;
        else if (ch == CH_COLOR1A1 || ch == CH_COLOR1 || ch == CH_ALPHA1) { c.ras_reg[s] = R_SEC; p->sec_is_color1 = 1; }
        else if (ch == CH_COLOR_ZERO || ch == CH_NULL) c.ras_reg[s] = R_ZERO;
        else { *why = "bump alpha channel"; return false; }
    }
    // 3. TEV registers: PREV -> spare0, REG0..2 from a pool of writable combiner registers
    uint8_t pool[8];
    int npool = 0;
    pool[npool++] = R_SPARE1;
    if (!p->sec_is_color1) pool[npool++] = R_SEC;
    for (int u = p->nunits; u < NV_MAX_UNITS; u++) pool[npool++] = (uint8_t) (R_TEX0 + u);
    int next = 0;
    c.phys[0] = R_SPARE0;
    for (int s = 0; s < nst; s++) {
        int regs[2] = { t->stage[s].color_op.out_reg & 3, t->stage[s].alpha_op.out_reg & 3 };
        for (int k = 0; k < 2; k++) {
            int r = regs[k];
            if (r == 0 || c.phys[r]) continue;
            if (next >= npool) { *why = "out of combiner temporaries"; return false; }
            c.phys[r] = pool[next++];
        }
    }
    c.scratch = next < npool ? pool[next] : 0;

    // 4. stages
    int k = 0;
    for (int s = 0; s < nst; s++) {
        CbHalf hc[3], ha[3];
        c.nneeds = 0;
        c.fail = 0;
        int nc = compile_portion(&c, s, false, hc);
        int na = nc ? compile_portion(&c, s, true, ha) : 0;
        if (!nc || !na) { *why = c.fail ? c.fail : "stage"; return false; }
        int n = nc > na ? nc : na;
        if (k + n > NV_MAX_STAGES) { *why = "more than 8 combiner stages"; return false; }
        for (int j = 0; j < n; j++) {
            // The shorter portion runs in the group's last stage(s): colour halves only write rgb parts and
            // alpha halves only alpha parts, so each portion still reads the stage's original inputs.
            // (Both portions multi-stage AND cross-reading each other's partial results is not handled.)
            CbHalf noop;
            memset(&noop, 0, sizeof noop);
            CbHalf col = (j >= n - nc) ? hc[j - (n - nc)] : noop;
            CbHalf alp = (j >= n - na) ? ha[j - (n - na)] : noop;
            if (!resolve_half(&c, p, k + j, &col) || !resolve_half(&c, p, k + j, &alp)) { *why = c.fail; return false; }
            p->color_icw[k + j] = icw_word(col);
            p->color_ocw[k + j] = col.dst ? color_ocw_word(col) : 0;
            p->alpha_icw[k + j] = icw_word(alp);
            p->alpha_ocw[k + j] = alp.dst ? alpha_ocw_word(alp) : 0;
        }
        k += n;
        const GxTevStage& st = t->stage[s];
        c.written[st.color_op.out_reg & 3][0] = 1;
        c.written[st.alpha_op.out_reg & 3][1] = 1;
    }
    p->nstages = (uint8_t) k;

    // 5. final combiner: out.rgb = A*B + (1-A)*C + D with A=B=C=0, D=spare0; out.a = G = spare0.a.
    // TODO: GX fog belongs here (A = fog factor, B = spare0, C = fog colour).
    p->final_cw0 = MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, (uint32_t) R_SPARE0);
    p->final_cw1 = MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, (uint32_t) R_SPARE0) | NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA;
    return true;
}

// "texture x vertex colour" (or vertex colour alone) on the first textured stage's texmap/texcoord.
static void nv_compile_fallback(const GxTevProgram* t, NvProgram* p)
{
    static GxTevProgram f;   // static: keep ~1.5 KB off the (small) stack
    memcpy(&f, t, sizeof f);
    f.num_stages = 1;
    f.num_ind_stages = 0;
    int src = -1;
    int nst = t->num_stages ? t->num_stages : 1;
    for (int s = 0; s < nst && s < GX_MAX_TEV_STAGES_PORT; s++) {
        uint16_t m = t->stage[s].map;
        if (!(m & 0x100) && (m & 0xFF) != 0xFF) { src = s; break; }
    }
    GxTevStage& st = f.stage[0];
    memset(&st, 0, sizeof st);
    st.chan = CH_COLOR0A0;
    st.coord = src >= 0 ? t->stage[src].coord : 0xFF;
    st.map = src >= 0 ? t->stage[src].map : 0xFF;
    static const uint8_t ci_tex[4] = { CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO }, ai_tex[4] = { CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO };
    static const uint8_t ci_ras[4] = { CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC }, ai_ras[4] = { CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA };
    memcpy(st.color_in, src >= 0 ? ci_tex : ci_ras, 4);
    memcpy(st.alpha_in, src >= 0 ? ai_tex : ai_ras, 4);
    st.color_op.clamp = st.alpha_op.clamp = 1;   // ADD, no bias, scale 1, -> PREV (zeroed)
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) f.swap_table[i][j] = (uint8_t) j;
    const char* why = 0;
    nv_compile(&f, p, &why);   // always fits: one stage, at most one unit, no constants
    p->mapped = 0;
}

// Per-stage constants from the uniform part of the program (refreshed on every set_pipeline).
static uint8_t clamp_u8(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }
static uint8_t frac_sel(uint8_t sel) { return clamp_u8((8 - sel) * 32); }   // KCSEL_1 .. KCSEL_1_8
static uint8_t kcomp(const GxTevProgram* t, uint8_t sel)                    // K0_R .. K3_A (0x10..0x1F)
{
    int k = (sel - 0x10) & 3, comp = (sel - 0x10) >> 2;
    return (uint8_t) (t->kcolor[k] >> (8 * comp));
}
static void csrc_rgb(const GxTevProgram* t, CbCSrc s, uint8_t rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (s.kind == CK_KCSEL) {
        if (s.idx < 8) rgb[0] = rgb[1] = rgb[2] = frac_sel(s.idx);
        else if (s.idx >= 0x0C && s.idx <= 0x0F) {
            uint32_t k = t->kcolor[s.idx - 0x0C];
            rgb[0] = (uint8_t) k; rgb[1] = (uint8_t) (k >> 8); rgb[2] = (uint8_t) (k >> 16);
        } else if (s.idx >= 0x10 && s.idx <= 0x1F) rgb[0] = rgb[1] = rgb[2] = kcomp(t, s.idx);
    } else if (s.kind == CK_REG_RGB) {
        for (int i = 0; i < 3; i++) rgb[i] = clamp_u8(t->tevreg[s.idx & 3][i]);
    }
}
static uint8_t csrc_a(const GxTevProgram* t, CbCSrc s)
{
    if (s.kind == CK_KASEL) {
        if (s.idx < 8) return frac_sel(s.idx);
        if (s.idx >= 0x10 && s.idx <= 0x1F) return kcomp(t, s.idx);
        return 0xFF;
    }
    if (s.kind == CK_REG_A) return clamp_u8(t->tevreg[s.idx & 3][3]);
    return 0;
}
static uint32_t factor_argb(const GxTevProgram* t, CbCSrc rgbsrc, CbCSrc asrc)
{
    uint8_t rgb[3];
    csrc_rgb(t, rgbsrc, rgb);
    return ((uint32_t) csrc_a(t, asrc) << 24) | ((uint32_t) rgb[0] << 16) | ((uint32_t) rgb[1] << 8) | rgb[2];
}

// =====================================================================================================
// Hardware backend
// =====================================================================================================

static const uint32_t nv_vp_program[] = {
#include "shaders/gx_passthrough.inl"
};

// GPU vertex: clip position, COLOR0, COLOR1, 4 texcoords (s, t, 0, q). Attribute slots 0, 3, 4, 9..12.
struct NvVtx {
    float pos[4];
    float c0[4];
    float c1[4];
    float tc[NV_MAX_UNITS][4];
};
static_assert(sizeof(NvVtx) == 112, "NvVtx layout");

enum {
    NV_VBO_POOL_SIZE   = 4 * 1024 * 1024,   // per frame; wraps with a GPU-idle wait (Starship pattern)
    NV_MAX_TEX         = 4096,
    NV_PROG_CACHE      = 256,
    NV_PB_RESET_DWORDS = 96 * 1024,         // pbkit's push buffer is 512 KB = 128K dwords
    NV_LOGGED_HASHES   = 512,
    NV_DUMMY_TEX       = 1,                 // handle of the 4x4 white texture
};

struct NvTex {
    void*    mem;
    uint32_t bytes;
    uint32_t pitch;      // linear textures (bytes per row, 64-byte aligned)
    uint16_t w, h;
    uint8_t  fmt;        // NV097_SET_TEXTURE_FORMAT_COLOR_*
    uint8_t  levels;
    uint8_t  linear;     // LU_IMAGE: unnormalised texel coordinates, no mips, no wrap
    uint8_t  used;
    uint8_t  log2w, log2h;
    uint8_t  is_rt;      // EFB copy destination (copy_to_texture writes it)
};

struct NvMapBind {
    GxTexHandle h;
    uint8_t wrap_s, wrap_t, min_f, mag_f, aniso;
    float   lod_bias;
};

static struct {
    int      inited;
    int      bb_w, bb_h, efb_w, efb_h;
    float    scale, ox, oy;             // EFB -> back buffer
    uint8_t* pool;
    size_t   pool_off;
    uint32_t pb_dwords;
    uint32_t frame, frame_tris, frame_draws;
    // viewport / scissor (EFB coordinates)
    float    vp[6];
    uint32_t sc[4];
    int      vp_dirty, sc_dirty;
    // matrices
    float    proj[4][4];
    float    pos[GX_PNMTX_COUNT][3][4];
    float    nrm[GX_PNMTX_COUNT][3][4];
    float    tex[GX_TEXMTX_COUNT][3][4];
    // pipeline
    GxPipelineKey key;
    GxTevProgram  tev;
    int      have_key, pipe_dirty;
    const NvProgram* prog;
    const NvProgram* bound_prog;
    uint64_t bound_hash;
    NvMapBind maps[GX_MAX_TEXMAPS_PORT];
    uint32_t unit_words[NV_MAX_UNITS][7];
    uint8_t  unit_valid[NV_MAX_UNITS];  // unit_words holds what the GPU has
    float    unit_su[NV_MAX_UNITS], unit_sv[NV_MAX_UNITS];
    // clear restricted to the last copy rectangle (GXCopyTex with clear)
    int      clear_rect_valid;
    int      clear_rect[4];
} nv;

static NvTex     nv_tex[NV_MAX_TEX];
static NvProgram nv_progs[NV_PROG_CACHE];
static uint32_t  nv_prog_evict;
static uint64_t  nv_logged[NV_LOGGED_HASHES];
static uint32_t  nv_nlogged;

static inline void nv_sfence(void) { __asm__ volatile("sfence" ::: "memory"); }

// pb_begin/pb_end with a running count of the dwords pushed since the last pb_reset.
static inline void nv_end(uint32_t* start, uint32_t* p)
{
    nv.pb_dwords += (uint32_t) (p - start);
    pb_end(p);
}

static void nv_invalidate_caches(void)
{
    nv.vp_dirty = nv.sc_dirty = 1;
    nv.pipe_dirty = 1;
    nv.bound_prog = 0;
    nv.bound_hash = 0;
    memset(nv.unit_valid, 0, sizeof nv.unit_valid);
}

static void nv_wait_idle(void) { while (pb_busy()) { } }

// Push buffer overflow protection (Starship FlushBatch): wait for the GPU before moving the write
// pointer back to the head, then re-target the back buffer.
static void nv_check_pushbuffer(void)
{
    if (nv.pb_dwords < NV_PB_RESET_DWORDS) return;
    nv_wait_idle();
    pb_reset();
    pb_target_back_buffer();
    nv.pb_dwords = 0;
    nv.pool_off = 0;   // GPU idle: the vertex pool can be reused too
    nv_invalidate_caches();
}

// ---- textures --------------------------------------------------------------------------------------------

static inline int is_pot(unsigned v) { return v && !(v & (v - 1)); }
static inline uint8_t log2_u(unsigned v) { uint8_t r = 0; while (v > 1) { v >>= 1; r++; } return r; }

static void* nv_alloc(uint32_t bytes)
{
    return MmAllocateContiguousMemoryEx(bytes, 0, NV_MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static NvTex* nv_tex_get(GxTexHandle h)
{
    if (h == 0 || h >= NV_MAX_TEX || !nv_tex[h].used) return &nv_tex[NV_DUMMY_TEX];
    return &nv_tex[h];
}

static GxTexHandle nv_tex_slot(void)
{
    for (uint32_t i = NV_DUMMY_TEX + 1; i < NV_MAX_TEX; i++) if (!nv_tex[i].used) return i;
    re4_log_once("nv2a_tex_full", "NV2A: texture table full (%d)\n", NV_MAX_TEX);
    return 0;
}

static void nv_make_dummy(void)
{
    NvTex& t = nv_tex[NV_DUMMY_TEX];
    t.bytes = 4 * 4 * 4;
    t.mem = nv_alloc(t.bytes);
    if (t.mem) memset(t.mem, 0xFF, t.bytes);
    t.w = t.h = 4; t.log2w = t.log2h = 2;
    t.fmt = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8;
    t.levels = 1; t.linear = 0; t.used = 1; t.pitch = 16;
}

// A linear A8R8G8B8 texture (EFB copy destination), rows 64-byte aligned.
static GxTexHandle nv_make_linear(uint16_t w, uint16_t h, const uint8_t* src, uint32_t src_pitch, int is_rt)
{
    GxTexHandle id = nv_tex_slot();
    if (!id || !w || !h) return NV_DUMMY_TEX;
    NvTex& t = nv_tex[id];
    memset(&t, 0, sizeof t);
    t.pitch = ((uint32_t) w * 4 + 63) & ~63u;
    t.bytes = t.pitch * h;
    t.mem = nv_alloc(t.bytes);
    if (!t.mem) {
        re4_log("NV2A: texture alloc failed (%ux%u linear, %u KB)\n", w, h, t.bytes / 1024);
        return NV_DUMMY_TEX;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = (uint8_t*) t.mem + y * t.pitch;
        if (src) memcpy(row, src + y * src_pitch, (size_t) w * 4);
        else memset(row, 0, t.pitch);
    }
    nv_sfence();
    t.w = w; t.h = h;
    t.fmt = NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8;
    t.levels = 1; t.linear = 1; t.used = 1; t.is_rt = (uint8_t) is_rt;
    return id;
}

static GxTexHandle nv_create_texture(const ConvertedImage* img)
{
    if (!nv.inited || !img) return NV_DUMMY_TEX;
    if (img->data_size == 0) {
        if (img->magic == RE4_IMG_MAGIC) return nv_make_linear(img->width, img->height, 0, 0, 1);   // GXCopyTex dest
        re4_log_once("nv2a_raw_tex", "NV2A: raw (non-RE4X) textures are not decoded yet; using white\n");
        return NV_DUMMY_TEX;
    }
    const uint8_t* data = (const uint8_t*) (img + 1);
    uint8_t fmt = img->nv2a_format;
    int compressed = (fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ||
                      fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8 ||
                      fmt == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8);
    if (fmt == NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 && !img->swizzled)
        return nv_make_linear(img->width, img->height, data, (uint32_t) img->width * 4, 0);   // single level
    if ((img->swizzled || compressed) && !(is_pot(img->width) && is_pot(img->height))) {
        re4_log_once("nv2a_npot_tex", "NV2A: NPOT swizzled/DXT texture %ux%u fmt 0x%02x not supported; using white\n",
                     img->width, img->height, fmt);
        return NV_DUMMY_TEX;
    }
    if (!img->swizzled && !compressed) {
        re4_log_once("nv2a_fmt_tex", "NV2A: unhandled ConvertedImage format 0x%02x; using white\n", fmt);
        return NV_DUMMY_TEX;
    }
    GxTexHandle id = nv_tex_slot();
    if (!id) return NV_DUMMY_TEX;
    NvTex& t = nv_tex[id];
    memset(&t, 0, sizeof t);
    t.bytes = img->data_size;
    t.mem = nv_alloc(t.bytes);   // TODO: bind the blob in place when tpl.py 128-aligns texel data (arena is contiguous)
    if (!t.mem) {
        re4_log("NV2A: texture alloc failed (%ux%u fmt 0x%02x, %u KB)\n", img->width, img->height, fmt, t.bytes / 1024);
        return NV_DUMMY_TEX;
    }
    memcpy(t.mem, data, t.bytes);
    nv_sfence();
    t.w = img->width; t.h = img->height;
    t.log2w = log2_u(t.w); t.log2h = log2_u(t.h);
    t.fmt = fmt;
    t.levels = img->mip_count ? img->mip_count : 1;
    t.linear = 0; t.used = 1; t.pitch = (uint32_t) t.w * 4;
    return id;
}

static void nv_destroy_texture(GxTexHandle h)
{
    if (h <= NV_DUMMY_TEX || h >= NV_MAX_TEX || !nv_tex[h].used) return;
    nv_wait_idle();   // the GPU may still sample it
    if (nv_tex[h].mem) MmFreeContiguousMemory(nv_tex[h].mem);
    memset(&nv_tex[h], 0, sizeof nv_tex[h]);
    for (int u = 0; u < NV_MAX_UNITS; u++) nv.unit_valid[u] = 0;
}

static void nv_bind_texture(int map, GxTexHandle h, int wrap_s, int wrap_t, int min_filt, int mag_filt, float lod_bias, int aniso)
{
    NvMapBind& b = nv.maps[map & 7];
    b.h = h;
    b.wrap_s = (uint8_t) wrap_s; b.wrap_t = (uint8_t) wrap_t;
    b.min_f = (uint8_t) min_filt; b.mag_f = (uint8_t) mag_filt;
    b.lod_bias = lod_bias; b.aniso = (uint8_t) aniso;
}

// GX wrap (CLAMP 0, REPEAT 1, MIRROR 2) -> NV2A address mode (1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE).
static inline uint32_t nv_wrap(uint8_t gx) { return gx == 1 ? 1u : gx == 2 ? 2u : 3u; }

// The 7 texture registers of a unit (OFFSET, FORMAT, ADDRESS, CONTROL0, CONTROL1, FILTER, IMAGE_RECT).
static void nv_unit_words(const NvTex* t, const NvMapBind& b, uint32_t w[7])
{
    w[0] = (uint32_t) (uintptr_t) t->mem & 0x03FFFFFF;
    w[1] = MASK(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2u)
         | MASK(NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE, 1u)
         | MASK(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2u)
         | MASK(NV097_SET_TEXTURE_FORMAT_COLOR, (uint32_t) t->fmt)
         | MASK(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, (uint32_t) (t->linear ? 1 : t->levels));
    if (!t->linear) {
        w[1] |= MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, (uint32_t) t->log2w)
              | MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, (uint32_t) t->log2h);
    }
    uint32_t wu = t->linear ? 3u : nv_wrap(b.wrap_s), wv = t->linear ? 3u : nv_wrap(b.wrap_t);
    w[2] = (3u << 16) | (wv << 8) | wu;
    uint32_t aniso = b.aniso > 2 ? 2u : b.aniso;   // GX_ANISO_1/2/4 -> 1x/2x/4x
    w[3] = NV097_SET_TEXTURE_CONTROL0_ENABLE | MASK(NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP, 0xFFFu)
         | MASK(NV097_SET_TEXTURE_CONTROL0_ANISOTROPY, aniso);
    w[4] = t->pitch << 16;
    // GX filters (NEAR, LINEAR, NEAR_MIP_NEAR, LIN_MIP_NEAR, NEAR_MIP_LIN, LIN_MIP_LIN) = NV2A value - 1.
    uint32_t minf = (uint32_t) (b.min_f > 5 ? 1 : b.min_f) + 1, magf = b.mag_f ? 2u : 1u;
    int levels = t->linear ? 1 : t->levels;
    if (levels <= 1 && minf > 2) minf = (minf == 3 || minf == 5) ? 1u : 2u;   // no mips: drop the mip part
    uint32_t bias = levels > 1 ? ((uint32_t) (int) (b.lod_bias * 256.0f) & 0x1FFF) : 0;
    w[5] = MASK(NV097_SET_TEXTURE_FILTER_MIN, minf) | MASK(NV097_SET_TEXTURE_FILTER_MAG, magf) | 0x2000u | bias;   // 0x2000 as Starship
    w[6] = ((uint32_t) t->w << 16) | t->h;
}

// Binds the textures of the current program; texture units beyond it are disabled.
static void nv_apply_textures(const NvProgram* pr)
{
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    for (int u = 0; u < NV_MAX_UNITS; u++) {
        if (u < pr->nunits) {
            const NvMapBind& b = nv.maps[pr->unit_map[u]];
            const NvTex* t = nv_tex_get(b.h);
            if (!t->mem) t = &nv_tex[NV_DUMMY_TEX];
            uint32_t w[7];
            nv_unit_words(t, b, w);
            nv.unit_su[u] = t->linear ? (float) t->w : 1.0f;
            nv.unit_sv[u] = t->linear ? (float) t->h : 1.0f;
            if (nv.unit_valid[u] == 1 && !memcmp(w, nv.unit_words[u], sizeof w)) continue;
            p = pb_push2(p, NV097_SET_TEXTURE_OFFSET + u * 64, w[0], w[1]);
            p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + u * 64, w[2]);
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + u * 64, w[3]);
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1 + u * 64, w[4]);
            p = pb_push1(p, NV097_SET_TEXTURE_FILTER + u * 64, w[5]);
            p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT + u * 64, w[6]);
            memcpy(nv.unit_words[u], w, sizeof w);
            nv.unit_valid[u] = 1;
        } else if (nv.unit_valid[u] != 2) {
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + u * 64, 0);
            nv.unit_valid[u] = 2;   // known disabled
        }
    }
    nv_end(p0, p);
}

// ---- pipeline state --------------------------------------------------------------------------------------

static const NvProgram* nv_get_program(const GxPipelineKey* key)
{
    uint64_t h = key->hash;
    uint32_t base = (uint32_t) (h ^ (h >> 32)) % NV_PROG_CACHE;
    NvProgram* slot = 0;
    for (int i = 0; i < 8; i++) {
        NvProgram* p = &nv_progs[(base + i) % NV_PROG_CACHE];
        if (p->valid && p->hash == h) return p;
        if (!p->valid && !slot) slot = p;
    }
    if (!slot) slot = &nv_progs[(base + (nv_prog_evict++ & 7)) % NV_PROG_CACHE];
    memset(slot, 0, sizeof *slot);
    const char* why = 0;
    if (!key->tev || !nv_compile(key->tev, slot, &why)) {
        static GxTevProgram empty;   // all zero: one stage of zeros -> vertex colour fallback
        nv_compile_fallback(key->tev ? key->tev : &empty, slot);
        uint32_t i;
        for (i = 0; i < nv_nlogged && i < NV_LOGGED_HASHES; i++) if (nv_logged[i] == h) break;
        if (i == nv_nlogged || i == NV_LOGGED_HASHES) {
            nv_logged[nv_nlogged++ % NV_LOGGED_HASHES] = h;
            re4_log("NV2A: unmapped GX pipeline %08x%08x (%u TEV stages, %u texgens, %u ind): %s -> texture x vertex colour\n",
                    (unsigned) (h >> 32), (unsigned) h, key->tev ? key->tev->num_stages : 0,
                    key->tev ? key->tev->num_texgens : 0, key->tev ? key->tev->num_ind_stages : 0, why ? why : "no program");
        }
    }
    slot->hash = h;
    slot->valid = 1;
    return slot;
}

static uint32_t nv_blend_src(uint8_t f)   // GXBlendFactor as a source factor (2/3 = DSTCLR/INVDSTCLR)
{
    static const uint32_t t[8] = { 0x0000, 0x0001, 0x0306, 0x0307, 0x0302, 0x0303, 0x0304, 0x0305 };
    return t[f & 7];
}
static uint32_t nv_blend_dst(uint8_t f)   // as a destination factor (2/3 = SRCCLR/INVSRCCLR)
{
    static const uint32_t t[8] = { 0x0000, 0x0001, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305 };
    return t[f & 7];
}

// GX's two-comparison alpha test reduced to NV2A's single compare (the catalogue only uses reducible forms).
static void nv_alpha_test(const GxPipelineKey& k, uint32_t* func, uint32_t* ref)
{
    int c0 = k.alpha_comp0 & 7, c1 = k.alpha_comp1 & 7, c = c0, r = k.alpha_ref0;
    switch (k.alpha_op) {
    case AOP_AND:
        if (c1 == CMP_ALWAYS) { c = c0; r = k.alpha_ref0; }
        else if (c0 == CMP_ALWAYS) { c = c1; r = k.alpha_ref1; }
        else if (c0 == CMP_NEVER || c1 == CMP_NEVER) { c = CMP_NEVER; r = 0; }
        else re4_log_once("nv2a_atest", "NV2A: alpha compare (%d AND %d) not reducible; using the first\n", c0, c1);
        break;
    case AOP_OR:
        if (c1 == CMP_NEVER) { c = c0; r = k.alpha_ref0; }
        else if (c0 == CMP_NEVER) { c = c1; r = k.alpha_ref1; }
        else if (c0 == CMP_ALWAYS || c1 == CMP_ALWAYS) { c = CMP_ALWAYS; r = 0; }
        else re4_log_once("nv2a_atest", "NV2A: alpha compare (%d OR %d) not reducible; using the first\n", c0, c1);
        break;
    default:   // XOR / XNOR
        if (c0 == CMP_ALWAYS && c1 == CMP_ALWAYS) c = (k.alpha_op == AOP_XOR) ? CMP_NEVER : CMP_ALWAYS;
        else re4_log_once("nv2a_atest", "NV2A: alpha compare XOR/XNOR not supported; using the first\n");
        break;
    }
    *func = 0x0200u + (uint32_t) c;
    *ref = (uint32_t) r;
}

static void nv_apply_pipeline(void)
{
    const GxPipelineKey& k = nv.key;
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;

    // blend / logic op
    uint32_t be = 0, sf = 1, df = 0, eq = NV097_SET_BLEND_EQUATION_V_FUNC_ADD, lo_en = 0, lo = 0x1503;
    switch (k.blend_type) {
    case BM_BLEND:    be = 1; sf = nv_blend_src(k.blend_src); df = nv_blend_dst(k.blend_dst); break;
    case BM_SUBTRACT: be = 1; sf = 1; df = 1; eq = NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT; break;
    case BM_LOGIC:    lo_en = 1; lo = 0x1500u | (k.blend_op & 15); break;   // GX logic ops are in GL order
    default: break;
    }
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, be);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, sf);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, df);
    p = pb_push1(p, NV097_SET_BLEND_EQUATION, eq);
    p = pb_push1(p, NV097_SET_LOGIC_OP_ENABLE, lo_en);
    p = pb_push1(p, NV097_SET_LOGIC_OP, lo);

    // depth (GX: compare disabled also disables the update)
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, k.z_enable ? 1 : 0);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, 0x0200u + (k.z_func & 7));
    p = pb_push1(p, NV097_SET_DEPTH_MASK, (k.z_enable && k.z_update) ? 1 : 0);

    // alpha test
    uint32_t afunc, aref;
    nv_alpha_test(k, &afunc, &aref);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, afunc != NV097_SET_ALPHA_FUNC_V_ALWAYS ? 1 : 0);
    p = pb_push1(p, NV097_SET_ALPHA_FUNC, afunc);
    p = pb_push1(p, NV097_SET_ALPHA_REF, aref);

    // cull (front face = CW set at init)
    if (k.cull_mode == CULL_NONE) {
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    } else {
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 1);
        p = pb_push1(p, NV097_SET_CULL_FACE, k.cull_mode == CULL_FRONT ? NV097_SET_CULL_FACE_V_FRONT :
                                             k.cull_mode == CULL_BACK ? NV097_SET_CULL_FACE_V_BACK :
                                                                        NV097_SET_CULL_FACE_V_FRONT_AND_BACK);
    }

    // write masks, dither, line width (GX 1/6 px -> NV2A 1/8 px)
    uint32_t cm = (k.color_update ? (NV097_SET_COLOR_MASK_RED_WRITE_ENABLE | NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE |
                                     NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE) : 0) |
                  (k.alpha_update ? NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE : 0);
    p = pb_push1(p, NV097_SET_COLOR_MASK, cm);
    p = pb_push1(p, NV097_SET_DITHER_ENABLE, k.dither ? 1 : 0);
    uint32_t lw = (uint32_t) k.line_width * 8 / 6;
    if (lw < 8) lw = 8;
    p = pb_push1(p, NV097_SET_LINE_WIDTH, lw & NV097_SET_LINE_WIDTH_MASK);
    nv_end(p0, p);

    if (k.dst_alpha_enable) re4_log_once("nv2a_dstalpha", "NV2A: GXSetDstAlpha ignored (TODO)\n");
    if (k.fog_type) re4_log_once("nv2a_fog", "NV2A: GX fog not implemented yet (TODO)\n");

    // register combiners (program words only when the program changed; constants every time)
    const NvProgram* pr = nv.prog;
    int n = pr->nstages ? pr->nstages : 1;
    p0 = pb_begin();
    p = p0;
    if (pr != nv.bound_prog || pr->hash != nv.bound_hash) {
        pb_push(p++, NV097_SET_COMBINER_COLOR_ICW, n); for (int i = 0; i < n; i++) *(p++) = pr->color_icw[i];
        pb_push(p++, NV097_SET_COMBINER_COLOR_OCW, n); for (int i = 0; i < n; i++) *(p++) = pr->color_ocw[i];
        pb_push(p++, NV097_SET_COMBINER_ALPHA_ICW, n); for (int i = 0; i < n; i++) *(p++) = pr->alpha_icw[i];
        pb_push(p++, NV097_SET_COMBINER_ALPHA_OCW, n); for (int i = 0; i < n; i++) *(p++) = pr->alpha_ocw[i];
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
                     MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, (uint32_t) NV097_SET_COMBINER_CONTROL_FACTOR0_EACH_STAGE) |
                     MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, (uint32_t) NV097_SET_COMBINER_CONTROL_FACTOR1_EACH_STAGE) |
                     MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, (uint32_t) n));
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, pr->final_cw0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, pr->final_cw1);
        uint32_t stages = 0;
        for (int u = 0; u < pr->nunits; u++) stages |= (uint32_t) NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE << (5 * u);
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, stages);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
        nv.bound_prog = pr;
        nv.bound_hash = pr->hash;
    }
    pb_push(p++, NV097_SET_COMBINER_FACTOR0, n);
    for (int i = 0; i < n; i++) *(p++) = factor_argb(&nv.tev, pr->slot_rgb[i][0], pr->slot_a[i][0]);
    pb_push(p++, NV097_SET_COMBINER_FACTOR1, n);
    for (int i = 0; i < n; i++) *(p++) = factor_argb(&nv.tev, pr->slot_rgb[i][1], pr->slot_a[i][1]);
    nv_end(p0, p);
}

// Viewport matrix in VP constants c[0..3]: rows multiply clip x, y, z, w (see gx_passthrough.vp).
static void nv_apply_viewport(void)
{
    float l = nv.vp[0], t = nv.vp[1], w = nv.vp[2], h = nv.vp[3], nz = nv.vp[4], fz = nv.vp[5];
    const float zmax = 16777215.0f;   // Z24
    float m[16];
    memset(m, 0, sizeof m);
    m[0]  = nv.scale * w * 0.5f;
    m[5]  = -nv.scale * h * 0.5f;
    m[10] = (fz - nz) * zmax;
    m[12] = nv.ox + nv.scale * (l + w * 0.5f);
    m[13] = nv.oy + nv.scale * (t + h * 0.5f);
    m[14] = fz * zmax;
    m[15] = 1.0f;
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);   // c[0]
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
    memcpy(p, m, sizeof m);
    p += 16;
    nv_end(p0, p);
}

static void nv_screen_rect(int l, int t, int w, int h, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = (int) (nv.ox + l * nv.scale);
    *y0 = (int) (nv.oy + t * nv.scale);
    *x1 = (int) (nv.ox + (l + w) * nv.scale) - 1;
    *y1 = (int) (nv.oy + (t + h) * nv.scale) - 1;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > nv.bb_w - 1) *x1 = nv.bb_w - 1;
    if (*y1 > nv.bb_h - 1) *y1 = nv.bb_h - 1;
}

static void nv_apply_scissor(void)
{
#if NV_USE_SCISSOR
    int x0, y0, x1, y1;
    nv_screen_rect((int) nv.sc[0], (int) nv.sc[1], (int) nv.sc[2], (int) nv.sc[3], &x0, &y0, &x1, &y1);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_TYPE, 0);   // inclusive
    // All 8 window-clip rectangles get the scissor so their union is the scissor either way.
    pb_push(p++, NV097_SET_WINDOW_CLIP_HORIZONTAL, 8);
    for (int i = 0; i < 8; i++) *(p++) = ((uint32_t) x1 << 16) | (uint32_t) x0;
    pb_push(p++, NV097_SET_WINDOW_CLIP_VERTICAL, 8);
    for (int i = 0; i < 8; i++) *(p++) = ((uint32_t) y1 << 16) | (uint32_t) y0;
    nv_end(p0, p);
#endif
}

// ---- CPU vertex path -------------------------------------------------------------------------------------

static inline float u8f(uint32_t c, int i) { return (float) ((c >> (8 * i)) & 0xFF) * (1.0f / 255.0f); }
static inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Factor of one light for one channel control (Dolphin LightingShaderGen semantics).
static float nv_light_factor(const GxChanCtrl& cc, const GxLightObjInt& L, const float ep[3], const float en[3])
{
    float ld[3] = { L.pos[0] - ep[0], L.pos[1] - ep[1], L.pos[2] - ep[2] };
    float dist2 = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2];
    float dist = sqrtf(dist2);
    if (dist > 0.0f) { float inv = 1.0f / dist; ld[0] *= inv; ld[1] *= inv; ld[2] *= inv; }
    else { ld[0] = en[0]; ld[1] = en[1]; ld[2] = en[2]; }
    float attn = 1.0f;
    int diff_fn = cc.diff_fn;
    if (cc.attn_fn == AF_SPOT) {
        float ca = ld[0] * L.dir[0] + ld[1] * L.dir[1] + ld[2] * L.dir[2];
        if (ca < 0.0f) ca = 0.0f;
        float num = L.a[0] + L.a[1] * ca + L.a[2] * ca * ca;
        float den = L.k[0] + L.k[1] * dist + L.k[2] * dist2;
        attn = (num > 0.0f && den > 0.0f) ? num / den : 0.0f;
    } else if (cc.attn_fn == AF_SPEC) {
        re4_log_once("nv2a_afspec", "NV2A: GX_AF_SPEC lighting approximated as AF_NONE (TODO)\n");
        diff_fn = DF_NONE;
    }
    float nd = ld[0] * en[0] + ld[1] * en[1] + ld[2] * en[2];
    float diff = diff_fn == DF_SIGN ? nd : diff_fn == DF_CLAMP ? (nd > 0.0f ? nd : 0.0f) : 1.0f;
    return attn * diff;
}

// GX colour channel ch (0/1): colour from chan[ch], alpha from chan[2 + ch].
static void nv_channel(const GxVertex& v, const float ep[3], const float en[3], int ch, float out[4])
{
    const GxTevProgram& T = nv.tev;
    uint32_t vc = v.col[ch];
    if (ch >= T.num_chans) {   // no channel: the vertex colour (Dolphin)
        for (int i = 0; i < 4; i++) out[i] = u8f(vc, i);
        return;
    }
    for (int part = 0; part < 2; part++) {
        const GxChanCtrl& cc = T.chan[part ? 2 + ch : ch];
        uint32_t mat = cc.mat_src == SRC_VTX ? vc : T.mat_color[ch];
        int i0 = part ? 3 : 0, i1 = part ? 4 : 3;
        if (!cc.enable) {
            for (int i = i0; i < i1; i++) out[i] = u8f(mat, i);
            continue;
        }
        uint32_t amb = cc.amb_src == SRC_VTX ? vc : T.amb_color[ch];
        float lit[4];
        for (int i = i0; i < i1; i++) lit[i] = u8f(amb, i);
        for (int l = 0; l < GX_MAX_LIGHTS_PORT; l++) {
            if (!(cc.light_mask & (1 << l))) continue;
            const GxLightObjInt& L = T.light[l];
            float f = nv_light_factor(cc, L, ep, en);
            for (int i = i0; i < i1; i++) lit[i] += f * u8f(L.color, i);
        }
        for (int i = i0; i < i1; i++) out[i] = u8f(mat, i) * clamp01(lit[i]);
    }
}

// Texgen for texcoord `coord`: s, t, q.
static void nv_texgen(const GxVertex& v, int coord, float out[3])
{
    const GxTevProgram& T = nv.tev;
    out[0] = out[1] = 0.0f; out[2] = 1.0f;
    if (coord >= T.num_texgens || coord >= GX_MAX_TEXGENS_PORT) return;
    const GxTexGen& g = T.texgen[coord];
    float in[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    if (g.src == TGS_POS) { in[0] = v.pos[0]; in[1] = v.pos[1]; in[2] = v.pos[2]; }
    else if (g.src == TGS_NRM) { in[0] = v.nrm[0]; in[1] = v.nrm[1]; in[2] = v.nrm[2]; }
    else if (g.src >= TGS_TEX0 && g.src <= TGS_TEX7) { in[0] = v.uv[g.src - TGS_TEX0][0]; in[1] = v.uv[g.src - TGS_TEX0][1]; }
    else if (g.src == TGS_COLOR0 || g.src == TGS_COLOR1) { uint32_t c = v.col[g.src - TGS_COLOR0]; in[0] = u8f(c, 0); in[1] = u8f(c, 1); }
    else { in[0] = v.uv[0][0]; in[1] = v.uv[0][1]; re4_log_once("nv2a_tgsrc", "NV2A: texgen source %d not supported\n", g.src); }
    if (g.func == TG_MTX2x4 || g.func == TG_MTX3x4) {
        const float (*M)[4] = nv.tex[v.texmtxidx[coord] % GX_TEXMTX_COUNT];
        out[0] = M[0][0] * in[0] + M[0][1] * in[1] + M[0][2] * in[2] + M[0][3] * in[3];
        out[1] = M[1][0] * in[0] + M[1][1] * in[1] + M[1][2] * in[2] + M[1][3] * in[3];
        if (g.func == TG_MTX3x4) {
            out[2] = M[2][0] * in[0] + M[2][1] * in[1] + M[2][2] * in[2] + M[2][3] * in[3];
            if (out[2] == 0.0f) out[2] = 1.0f;
        }
    } else {
        re4_log_once("nv2a_tgfunc", "NV2A: texgen func %d (bump/SRTG) passes the source through (TODO)\n", g.func);
        out[0] = in[0]; out[1] = in[1];
    }
}

static void nv_xform(const GxVertex& v, const NvProgram* pr, NvVtx* o)
{
    int m = v.mtxidx % GX_PNMTX_COUNT;
    const float (*P)[4] = nv.pos[m];
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = P[r][0] * v.pos[0] + P[r][1] * v.pos[1] + P[r][2] * v.pos[2] + P[r][3];
    for (int r = 0; r < 4; r++) o->pos[r] = nv.proj[r][0] * e[0] + nv.proj[r][1] * e[1] + nv.proj[r][2] * e[2] + nv.proj[r][3];

    const GxTevProgram& T = nv.tev;
    bool lit = (T.num_chans > 0 && (T.chan[0].enable || T.chan[2].enable)) ||
               (T.num_chans > 1 && (T.chan[1].enable || T.chan[3].enable));
    float n[3] = { 0.0f, 0.0f, 1.0f };
    if (lit) {
        const float (*N)[4] = nv.nrm[m];
        for (int r = 0; r < 3; r++) n[r] = N[r][0] * v.nrm[0] + N[r][1] * v.nrm[1] + N[r][2] * v.nrm[2];
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f) { float inv = 1.0f / len; n[0] *= inv; n[1] *= inv; n[2] *= inv; }
    }
    nv_channel(v, e, n, 0, o->c0);
    if (pr->sec_is_color1) nv_channel(v, e, n, 1, o->c1);
    else o->c1[0] = o->c1[1] = o->c1[2] = o->c1[3] = 0.0f;

    for (int u = 0; u < NV_MAX_UNITS; u++) {
        float* tc = o->tc[u];
        if (u >= pr->nunits) { tc[0] = tc[1] = tc[2] = 0.0f; tc[3] = 1.0f; continue; }
        float stq[3];
        nv_texgen(v, pr->unit_coord[u], stq);
        tc[0] = stq[0] * nv.unit_su[u];   // linear textures take texel coordinates
        tc[1] = stq[1] * nv.unit_sv[u];
        tc[2] = 0.0f;
        tc[3] = stq[2];
    }
}

static inline float nv_clipdist(const NvVtx& v) { return v.pos[2] + v.pos[3]; }   // GX near plane: z >= -w

static void nv_lerp(const NvVtx& a, const NvVtx& b, float t, NvVtx* o)
{
    const float* fa = (const float*) &a;
    const float* fb = (const float*) &b;
    float* fo = (float*) o;
    for (size_t i = 0; i < sizeof(NvVtx) / sizeof(float); i++) fo[i] = fa[i] + (fb[i] - fa[i]) * t;
}

// ---- draw ------------------------------------------------------------------------------------------------

struct NvBatch { size_t start; size_t count; };

static void nv_submit(int prim, NvBatch* b)
{
    if (!b->count) return;
    nv_sfence();   // flush write-combined vertex data before the GPU reads it
    uint32_t base = ((uint32_t) (uintptr_t) nv.pool & 0x03FFFFFF) + (uint32_t) b->start;
    uint32_t op = prim == GX_PRIM_LINES ? NV097_SET_BEGIN_END_OP_LINES :
                  prim == GX_PRIM_POINTS ? NV097_SET_BEGIN_END_OP_POINTS : NV097_SET_BEGIN_END_OP_TRIANGLES;
    size_t chunk = prim == GX_PRIM_LINES ? 254 : 255;   // DRAW_ARRAYS count field is 8 bits (count - 1)

    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, base + 0);
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, base + 16);
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * 4, base + 32);
    for (int u = 0; u < NV_MAX_UNITS; u++)
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + (9 + u) * 4, base + 48 + 16 * (uint32_t) u);
    p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
    p = pb_push1(p, NV097_SET_BEGIN_END, op);
    size_t start = 0, left = b->count;
    int in_block = 0;
    while (left) {
        size_t n = left > chunk ? chunk : left;
        p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                     MASK(NV097_DRAW_ARRAYS_COUNT, (uint32_t) (n - 1)) | MASK(NV097_DRAW_ARRAYS_START_INDEX, (uint32_t) start));
        start += n;
        left -= n;
        if (++in_block == 64 && left) {   // keep each pb_begin/pb_end block small
            nv_end(p0, p);
            p0 = pb_begin();
            p = p0;
            in_block = 0;
        }
    }
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    nv_end(p0, p);
    nv.pool_off = (nv.pool_off + 15) & ~(size_t) 15;
    b->start = nv.pool_off;
    b->count = 0;
}

static inline void nv_put(NvBatch* b, const NvVtx& v)
{
    memcpy(nv.pool + nv.pool_off, &v, sizeof v);
    nv.pool_off += sizeof v;
    b->count++;
}

static void nv_apply_state(void)
{
    if (nv.vp_dirty) { nv_apply_viewport(); nv.vp_dirty = 0; }
    if (nv.sc_dirty) { nv_apply_scissor(); nv.sc_dirty = 0; }
    if (nv.pipe_dirty) { nv_apply_pipeline(); nv.pipe_dirty = 0; }
    nv_apply_textures(nv.prog);   // cheap: compares the cached unit words
}

static void nv_draw(int prim, const GxVertex* v, size_t count)
{
    if (!nv.inited || !count || !v) return;
    if (!nv.have_key) {
        re4_log_once("nv2a_nokey", "NV2A: draw before set_pipeline dropped\n");
        return;
    }
    nv_check_pushbuffer();
    nv_apply_state();
    const NvProgram* pr = nv.prog;
    int per = prim == GX_PRIM_LINES ? 2 : prim == GX_PRIM_POINTS ? 1 : 3;
    NvBatch b = { nv.pool_off, 0 };
    nv.frame_draws++;

    for (size_t i = 0; i + (size_t) per <= count; i += (size_t) per) {
        if (nv.pool_off + 8 * sizeof(NvVtx) > NV_VBO_POOL_SIZE) {   // pool full: draw, wait, wrap
            nv_submit(prim, &b);
            nv_wait_idle();
            nv.pool_off = 0;
            b.start = 0;
        }
        NvVtx t[3];
        float d[3];
        for (int j = 0; j < per; j++) { nv_xform(v[i + j], pr, &t[j]); d[j] = nv_clipdist(t[j]); }
        if (per == 1) {
            if (d[0] >= 0.0f) nv_put(&b, t[0]);
        } else if (per == 2) {
            if (d[0] < 0.0f && d[1] < 0.0f) continue;
            NvVtx a = t[0], c = t[1];
            if (d[0] < 0.0f) nv_lerp(t[0], t[1], d[0] / (d[0] - d[1]), &a);
            if (d[1] < 0.0f) nv_lerp(t[0], t[1], d[0] / (d[0] - d[1]), &c);
            nv_put(&b, a); nv_put(&b, c);
        } else {
            if (d[0] >= 0.0f && d[1] >= 0.0f && d[2] >= 0.0f) {
                nv_put(&b, t[0]); nv_put(&b, t[1]); nv_put(&b, t[2]);
                nv.frame_tris++;
                continue;
            }
            if (d[0] < 0.0f && d[1] < 0.0f && d[2] < 0.0f) continue;
            NvVtx poly[4];   // Sutherland-Hodgman against the near plane: 3 -> at most 4 vertices
            int np = 0;
            for (int j = 0; j < 3; j++) {
                int k = (j + 1) % 3;
                if (d[j] >= 0.0f) poly[np++] = t[j];
                if ((d[j] >= 0.0f) != (d[k] >= 0.0f)) nv_lerp(t[j], t[k], d[j] / (d[j] - d[k]), &poly[np++]);
            }
            for (int j = 1; j + 1 < np; j++) {
                nv_put(&b, poly[0]); nv_put(&b, poly[j]); nv_put(&b, poly[j + 1]);
                nv.frame_tris++;
            }
        }
    }
    nv_submit(prim, &b);
}

// ---- vtable ----------------------------------------------------------------------------------------------

static void nv_upload_vertex_program(void)
{
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, (uint32_t) NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, (uint32_t) NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    nv_end(p0, p);
    for (size_t i = 0; i < sizeof nv_vp_program / 16; i++) {   // 16 bytes per instruction
        p0 = pb_begin();
        p = p0;
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &nv_vp_program[i * 4], 16);
        p += 4;
        nv_end(p0, p);
    }
    // Vertex attributes: all float; 0 = position, 3 = diffuse, 4 = specular, 9..12 = texcoord 0..3.
    p0 = pb_begin();
    p = p0;
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (int i = 0; i < 16; i++) {
        bool used = (i == 0 || i == 3 || i == 4 || (i >= 9 && i <= 12));
        *(p++) = MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, (uint32_t) NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) |
                 (used ? (MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 4u) |
                          MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, (uint32_t) sizeof(NvVtx))) : 0);
    }
    nv_end(p0, p);
}

static int nv_init(int width, int height)
{
    if (nv.inited) return 0;
    memset(&nv, 0, sizeof nv);
    nv.efb_w = width > 0 ? width : 640;
    nv.efb_h = height > 0 ? height : 448;

    // Pool before pb_init, assigned only after it is valid (Starship's malloc/memset lesson).
    uint8_t* pool = (uint8_t*) nv_alloc(NV_VBO_POOL_SIZE);
    if (!pool) { re4_log("NV2A: vertex pool alloc failed\n"); return -1; }
    nv.pool = pool;

    int status = pb_init();
    if (status != 0) {
        re4_log("NV2A: pb_init failed: %d\n", status);
        debugPrint("pb_init failed: %d\n", status);
        return -1;
    }
    pb_show_front_screen();
    nv.bb_w = (int) pb_back_buffer_width();
    nv.bb_h = (int) pb_back_buffer_height();
    float sx = (float) nv.bb_w / (float) nv.efb_w, sy = (float) nv.bb_h / (float) nv.efb_h;
    nv.scale = sx < sy ? sx : sy;
    nv.ox = ((float) nv.bb_w - nv.efb_w * nv.scale) * 0.5f;
    nv.oy = ((float) nv.bb_h - nv.efb_h * nv.scale) * 0.5f;
    re4_log("NV2A: back buffer %dx%d, EFB %dx%d at scale %.3f offset (%.0f, %.0f)\n",
            nv.bb_w, nv.bb_h, nv.efb_w, nv.efb_h, nv.scale, nv.ox, nv.oy);

    nv_upload_vertex_program();

    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);   // required, or the final combiner is bypassed (Starship)
    p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SHADE_MODEL, NV097_SET_SHADE_MODEL_SMOOTH);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CW);   // GX: clockwise = front. TODO(hw): verify
    p = pb_push1f(p, NV097_SET_CLIP_MIN, 0.0f);
    p = pb_push1f(p, NV097_SET_CLIP_MAX, 16777215.0f);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 1);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_ALWAYS);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_COLOR_MASK, NV097_SET_COLOR_MASK_BLUE_WRITE_ENABLE | NV097_SET_COLOR_MASK_GREEN_WRITE_ENABLE |
                                          NV097_SET_COLOR_MASK_RED_WRITE_ENABLE | NV097_SET_COLOR_MASK_ALPHA_WRITE_ENABLE);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
    for (int i = 0; i < NV_MAX_UNITS; i++) p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + i * 64, 0);
    nv_end(p0, p);
    nv_wait_idle();

    nv_make_dummy();
    for (int i = 0; i < GX_MAX_TEXMAPS_PORT; i++) nv.maps[i].h = NV_DUMMY_TEX;
    nv.vp[0] = 0; nv.vp[1] = 0; nv.vp[2] = (float) nv.efb_w; nv.vp[3] = (float) nv.efb_h; nv.vp[4] = 0; nv.vp[5] = 1;
    nv.sc[0] = 0; nv.sc[1] = 0; nv.sc[2] = (uint32_t) nv.efb_w; nv.sc[3] = (uint32_t) nv.efb_h;
    for (int i = 0; i < 4; i++) nv.proj[i][i] = 1.0f;
    for (int m = 0; m < GX_PNMTX_COUNT; m++) for (int i = 0; i < 3; i++) nv.pos[m][i][i] = nv.nrm[m][i][i] = 1.0f;
    for (int m = 0; m < GX_TEXMTX_COUNT; m++) for (int i = 0; i < 3; i++) nv.tex[m][i][i] = 1.0f;
    nv_invalidate_caches();
    nv.inited = 1;
    re4_log("NV2A: init complete\n");
    return 0;
}

static void nv_shutdown(void)
{
    if (!nv.inited) return;
    nv_wait_idle();
    pb_kill();
    nv.inited = 0;
}

static void nv_clear_rect(uint32_t rgba, uint32_t z, int color, int depth, int x0, int y0, int x1, int y1)
{
    uint32_t flags = (depth ? (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL) : 0) | (color ? NV097_CLEAR_SURFACE_COLOR : 0);
    if (!flags) return;
    uint32_t argb = (rgba & 0xFF000000u) | ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, ((uint32_t) x1 << 16) | (uint32_t) x0);
    p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, ((uint32_t) y1 << 16) | (uint32_t) y0);
    p = pb_push1(p, NV097_SET_ZSTENCIL_CLEAR_VALUE, (z & 0xFFFFFF) << 8);
    p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, argb);
    p = pb_push1(p, NV097_CLEAR_SURFACE, flags);
    nv_end(p0, p);
}

static void nv_begin_frame(void)
{
    if (!nv.inited) return;
    pb_reset();
    pb_target_back_buffer();
    nv_wait_idle();
    uint32_t* p0 = pb_begin();
    uint32_t* p = p0;
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, (uint32_t) NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, (uint32_t) NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    nv.pb_dwords = 0;
    nv_end(p0, p);
    nv.pool_off = 0;
    nv.frame_tris = nv.frame_draws = 0;
    nv.clear_rect_valid = 0;
    nv_invalidate_caches();
    // The border outside the EFB stays black; clear() then paints the EFB area with the copy-clear colour.
    nv_clear_rect(0xFF000000u, 0xFFFFFF, 1, 1, 0, 0, nv.bb_w - 1, nv.bb_h - 1);
}

static void nv_end_frame(void)
{
    if (!nv.inited) return;
    DWORD t0 = KeTickCount;
    while (pb_busy()) { if (KeTickCount - t0 > 500) break; }   // 500 ms hang guard (Starship)
    DWORD busy = KeTickCount - t0;
    DWORD t1 = KeTickCount;
    while (pb_finished()) { if (KeTickCount - t1 > 500) break; }   // flip at vblank
    nv.frame++;
    if (nv.frame <= 5 || nv.frame % 600 == 0) {
        re4_log("NV2A: frame %u: %u draws, %u tris, gpu wait %u ms, pb %u dwords\n",
                nv.frame, nv.frame_draws, nv.frame_tris, (unsigned) busy, nv.pb_dwords);
    }
}

static void nv_set_viewport(float l, float t, float w, float h, float nz, float fz)
{
    nv.vp[0] = l; nv.vp[1] = t; nv.vp[2] = w; nv.vp[3] = h; nv.vp[4] = nz; nv.vp[5] = fz;
    nv.vp_dirty = 1;
}

static void nv_set_scissor(uint32_t l, uint32_t t, uint32_t w, uint32_t h)
{
    nv.sc[0] = l; nv.sc[1] = t; nv.sc[2] = w; nv.sc[3] = h;
    nv.sc_dirty = 1;
}

static void nv_set_pipeline(const GxPipelineKey* key)
{
    if (!key) return;
    nv.key = *key;
    if (key->tev) memcpy(&nv.tev, key->tev, sizeof nv.tev);
    else memset(&nv.tev, 0, sizeof nv.tev);
    nv.key.tev = &nv.tev;
    nv.prog = nv_get_program(key);
    nv.have_key = 1;
    nv.pipe_dirty = 1;
}

static void nv_set_matrices(const float proj[4][4], int proj_type, const float (*pos)[3][4], const float (*nrm)[3][4], const float (*tex)[3][4])
{
    (void) proj_type;   // the 4x4 already encodes perspective vs orthographic
    if (proj) memcpy(nv.proj, proj, sizeof nv.proj);
    if (pos) memcpy(nv.pos, pos, sizeof nv.pos);
    if (nrm) memcpy(nv.nrm, nrm, sizeof nv.nrm);
    if (tex) memcpy(nv.tex, tex, sizeof nv.tex);
}

static GxRtHandle nv_create_render_target(int w, int h, int with_depth)
{
    (void) w; (void) h; (void) with_depth;
    re4_log_once("nv2a_rt", "NV2A: create_render_target not implemented (TODO)\n");
    return 0;
}

static void nv_set_render_target(GxRtHandle rt)
{
    if (rt) re4_log_once("nv2a_setrt", "NV2A: set_render_target(%u) not implemented; drawing to the back buffer\n", rt);
}

// EFB copy as a CPU readback of the colour buffer (slow; colour formats only, nearest sampling when
// the destination is smaller). TODO: GPU path (render into the texture's surface / NV062 blit), Z copies,
// GX copy formats (I8, RGB565, Z24 ...), texture cache invalidation after CPU writes.
static void nv_copy_to_texture(GxTexHandle dst, int left, int top, int w, int h, int clear)
{
    if (!nv.inited || w <= 0 || h <= 0) return;
    if (clear) {
        nv_screen_rect(left, top, w, h, &nv.clear_rect[0], &nv.clear_rect[1], &nv.clear_rect[2], &nv.clear_rect[3]);
        nv.clear_rect_valid = 1;
    }
    if (dst <= NV_DUMMY_TEX || dst >= NV_MAX_TEX || !nv_tex[dst].used || !nv_tex[dst].is_rt) return;
    NvTex& t = nv_tex[dst];
    nv_wait_idle();
    const uint8_t* fb = (const uint8_t*) pb_back_buffer();
    uint32_t pitch = pb_back_buffer_pitch();
    if (!fb) return;
    for (int y = 0; y < t.h; y++) {
        int sy = (int) (nv.oy + (top + (float) y * h / t.h) * nv.scale);
        if (sy < 0 || sy >= nv.bb_h) continue;
        const uint32_t* srow = (const uint32_t*) (fb + (size_t) sy * pitch);
        uint32_t* drow = (uint32_t*) ((uint8_t*) t.mem + (size_t) y * t.pitch);
        for (int x = 0; x < t.w; x++) {
            int sx = (int) (nv.ox + (left + (float) x * w / t.w) * nv.scale);
            drow[x] = (sx >= 0 && sx < nv.bb_w) ? srow[sx] : 0;
        }
    }
    nv_sfence();
    re4_log_once("nv2a_copytex", "NV2A: GXCopyTex served by CPU readback (slow path)\n");
}

static void nv_clear(uint32_t rgba, uint32_t z, int color, int depth)
{
    if (!nv.inited) return;
    nv_check_pushbuffer();
    int x0, y0, x1, y1;
    if (nv.clear_rect_valid) {
        x0 = nv.clear_rect[0]; y0 = nv.clear_rect[1]; x1 = nv.clear_rect[2]; y1 = nv.clear_rect[3];
        nv.clear_rect_valid = 0;
    } else {
        nv_screen_rect(0, 0, nv.efb_w, nv.efb_h, &x0, &y0, &x1, &y1);
    }
    nv_clear_rect(rgba, z, color, depth, x0, y0, x1, y1);
}

static uint32_t nv_peek_z(int x, int y)
{
    (void) x; (void) y;
    re4_log_once("nv2a_peekz", "NV2A: GXPeekZ returns far (TODO: read the Z24S8 surface)\n");
    return 0xFFFFFF;
}

extern "C" const GxBackend gx_nv2a_backend = {
    "nv2a",
    nv_init,
    nv_shutdown,
    nv_begin_frame,
    nv_end_frame,
    nv_set_viewport,
    nv_set_scissor,
    nv_set_pipeline,
    nv_set_matrices,
    nv_draw,
    nv_create_texture,
    nv_destroy_texture,
    nv_bind_texture,
    nv_create_render_target,
    nv_set_render_target,
    nv_copy_to_texture,
    nv_clear,
    nv_peek_z,
};
