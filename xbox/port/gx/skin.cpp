/* xbox/port/gx/skin.cpp -- CT-02: C replacement for the paired-single skinning loops of
 * src/game/trans.cpp (CalcSk1_x, CalcSk1_x2) and the GQR6 store of setupGQR6.
 *
 * Decoded from the asm (trans.cpp 2485..2560).  Per vertex, with M = palette[idx] as ROMtx[4][3]
 * (M[c][r] = Mtx[r][c], see PSMTXReorder in xbox/port/mtx/mtx_extra.c):
 *   f0 = {M[0][0], M[0][1]}  f1 = {M[0][2], 1}     (psq_l 0(r9) / 8(r9), GQR0 = float)
 *   f2 = {M[1][0], M[1][1]}  f3 = {M[1][2], 1}     (0xc / 0x14)
 *   f4 = {M[2][0], M[2][1]}  f5 = {M[2][2], 1}     (0x18 / 0x20)
 *   f6 = {M[3][0], M[3][1]}  f7 = {M[3][2], 1}     (0x24 / 0x2c)
 *   f8 = {x, y}, f9 = {z, 1}   dequantised through GQR6
 *   ps_madds0 12,0,8,6 : f12 = f0*x + f6            ps_madds0 13,1,8,7 : f13 = f1*x + f7
 *   ps_madds1 12,2,8,12: f12 = f2*y + f12           ps_madds1 13,3,8,13: f13 = f3*y + f13
 *   ps_madds0 12,4,9,12: f12 = f4*z + f12           ps_madds0 13,5,9,13: f13 = f5*z + f13
 *   psq_stu f12 (x', y'), psq_st f13 (z') quantised through GQR6.
 * i.e. out = ((col0*x + col3) + col1*y) + col2*z, evaluated in that order in single precision.
 * The Gekko fuses each multiply-add; x86 does not, so results may differ by an ulp before quantisation.
 *
 * GQR6 values seen (trans.cpp 750/771/776):
 *   (shift<<24)|(shift<<8)|0x00070007  positions: s16, scale `shift` both ways (fixed point)
 *   0x32073207                         s16 normals: scale 50 = -14 -> load *2^14, store *2^-14, which
 *                                      makes the translation column negligible (normals are directions)
 *   0x20062006                         s8 normals: scale 32 = -32 -> load *2^32, store *2^-32, same trick
 */
#include "re4port/skin.h"
#include <math.h>

extern "C" {

float re4_skin_palette[RE4_SKIN_PALETTE_MAX][4][3];
unsigned int re4_gqr6;

unsigned int re4_gqr_ld_type(unsigned int gqr) { return (gqr >> 16) & 7u; }
unsigned int re4_gqr_st_type(unsigned int gqr) { return gqr & 7u; }

static int sext6(unsigned int v)
{
    v &= 0x3Fu;
    return (v & 0x20u) ? (int) v - 64 : (int) v;
}
int re4_gqr_ld_scale(unsigned int gqr) { return sext6(gqr >> 24); }
int re4_gqr_st_scale(unsigned int gqr) { return sext6(gqr >> 8); }

/* 2^-scale for loads, 2^scale for stores (Gekko table semantics: field 32..63 = -32..-1). */
float re4_gqr_dequant_factor(int scale) { return ldexpf(1.0f, -scale); }
float re4_gqr_quant_factor(int scale) { return ldexpf(1.0f, scale); }

/* psq_st integer conversion: scale, truncate toward zero, saturate. */
static inline int quant_s16(float f, float k)
{
    f *= k;
    if (f >= 32767.0f) return 32767;
    if (f <= -32768.0f) return -32768;
    return (int) f;
}
static inline int quant_s8(float f, float k)
{
    f *= k;
    if (f >= 127.0f) return 127;
    if (f <= -128.0f) return -128;
    return (int) f;
}

static inline void skin_one(const float (*M)[3], float x, float y, float z, float* ox, float* oy, float* oz)
{
    float rx, ry, rz;
    rx = M[0][0] * x + M[3][0];
    ry = M[0][1] * x + M[3][1];
    rz = M[0][2] * x + M[3][2];
    rx = M[1][0] * y + rx;
    ry = M[1][1] * y + ry;
    rz = M[1][2] * y + rz;
    rx = M[2][0] * z + rx;
    ry = M[2][1] * z + ry;
    rz = M[2][2] * z + rz;
    *ox = rx;
    *oy = ry;
    *oz = rz;
}

void re4_calcsk1_s16(void* dst, const void* src, unsigned int n, unsigned int gqr)
{
    const float kl = re4_gqr_dequant_factor(re4_gqr_ld_scale(gqr));
    const float ks = re4_gqr_quant_factor(re4_gqr_st_scale(gqr));
    const short* s = (const short*) src;
    short* d = (short*) dst;
    unsigned int i;

    for (i = 0; i < n; i++, s += 4, d += 3) {
        const float (*M)[3] = re4_skin_palette[(unsigned short) s[3] % RE4_SKIN_PALETTE_MAX];
        float x = (float) s[0] * kl, y = (float) s[1] * kl, z = (float) s[2] * kl;
        float ox, oy, oz;
        skin_one(M, x, y, z, &ox, &oy, &oz);
        d[0] = (short) quant_s16(ox, ks);
        d[1] = (short) quant_s16(oy, ks);
        d[2] = (short) quant_s16(oz, ks);
    }
}

void re4_calcsk1_s8(void* dst, const void* src, unsigned int n, unsigned int gqr)
{
    const float kl = re4_gqr_dequant_factor(re4_gqr_ld_scale(gqr));
    const float ks = re4_gqr_quant_factor(re4_gqr_st_scale(gqr));
    const signed char* s = (const signed char*) src;
    signed char* d = (signed char*) dst;
    unsigned int i;

    for (i = 0; i < n; i++, s += 4, d += 3) {
        const float (*M)[3] = re4_skin_palette[(unsigned char) s[3]];
        float x = (float) s[0] * kl, y = (float) s[1] * kl, z = (float) s[2] * kl;
        float ox, oy, oz;
        skin_one(M, x, y, z, &ox, &oy, &oz);
        d[0] = (signed char) quant_s8(ox, ks);
        d[1] = (signed char) quant_s8(oy, ks);
        d[2] = (signed char) quant_s8(oz, ks);
    }
}

} /* extern "C" */
