/* xbox/port/tests/ct02/ct02_test.cpp -- CT-02 tests for the C replacements of the PowerPC asm:
 * skinning (port/gx/skin.cpp), SINF/COSF/LIMIT_ANGLE/SQRTF (port/mtx/math_ppc.cpp), the fills
 * (port/os/memfill.c) and the yz2 decoder (port/data/yz2.c).
 *
 *   ct02 [yz2_stream.das reference.bin]
 *
 * The yz2 check needs a real stream (no game data in the repo): a room archive such as
 * disc1/files/St1/r120.das (body at 0x400) and the output of the Python oracle on it
 * (`py -3 tools/motion/yz2.py r120.das r120.ref.bin`).  Without arguments that check is skipped.
 * Pure functions: builds 64-bit or -m32. */
#define _CRT_SECURE_NO_WARNINGS 1
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re4port/skin.h"
#include "re4port/math_ppc.h"
#include "memfill.h"
#include "yz2.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        if (cond) {                                                  \
            g_pass++;                                                \
        } else {                                                     \
            g_fail++;                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);              \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

/* deterministic LCG so runs are reproducible */
static unsigned int g_seed = 0x12345678u;
static unsigned int rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}
static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float) (rnd() & 0xFFFFFF) / 16777216.0f;
}

/* ------------------------------------------------------------------------------------------- */
/* GQR decoding                                                                                 */

static void test_gqr(void)
{
    unsigned int v;
    /* scheduler.cpp TaskExec_hook */
    CHECK(re4_gqr_ld_type(0x00040004u) == 4 && re4_gqr_st_type(0x00040004u) == 4, "GQR2 types");
    CHECK(re4_gqr_ld_scale(0x00040004u) == 0 && re4_gqr_st_scale(0x00040004u) == 0, "GQR2 scale");
    CHECK(re4_gqr_ld_type(0x00070007u) == 7, "GQR5 type s16");
    /* trans.cpp setupGQR6 */
    v = (3u << 24) | (3u << 8) | 0x00070007u;
    CHECK(re4_gqr_ld_scale(v) == 3 && re4_gqr_st_scale(v) == 3 && re4_gqr_ld_type(v) == 7, "GQR6 shift 3");
    CHECK(re4_gqr_ld_scale(0x32073207u) == -14 && re4_gqr_st_scale(0x32073207u) == -14, "GQR6 0x32073207 scale -14");
    CHECK(re4_gqr_ld_type(0x32073207u) == 7 && re4_gqr_st_type(0x32073207u) == 7, "GQR6 0x32073207 type");
    CHECK(re4_gqr_ld_scale(0x20062006u) == -32 && re4_gqr_st_scale(0x20062006u) == -32, "GQR6 0x20062006 scale -32");
    CHECK(re4_gqr_ld_type(0x20062006u) == 6 && re4_gqr_st_type(0x20062006u) == 6, "GQR6 0x20062006 type");
    CHECK(re4_gqr_dequant_factor(3) == 0.125f && re4_gqr_quant_factor(3) == 8.0f, "scale 3 factors");
    CHECK(re4_gqr_dequant_factor(-14) == 16384.0f && re4_gqr_quant_factor(-14) == 1.0f / 16384.0f, "scale -14 factors");
    CHECK(re4_gqr_dequant_factor(-32) == 4294967296.0f, "scale -32 factor");
}

/* ------------------------------------------------------------------------------------------- */
/* skinning against a naive double implementation                                                */

/* fills palette[i] as the ROMtx of a random 3x4 matrix; returns the row-major matrices */
static void make_palette(int count, double rows[][3][4])
{
    int i, r, c;
    for (i = 0; i < count; i++) {
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                rows[i][r][c] = frand(-1.5f, 1.5f);
            }
            rows[i][r][3] = frand(-2000.0f, 2000.0f);
        }
        for (c = 0; c < 4; c++) {
            for (r = 0; r < 3; r++) {
                re4_skin_palette[i][c][r] = (float) rows[i][r][c];   /* PSMTXReorder */
            }
        }
    }
}

static int clamp_trunc(double v, int lo, int hi)
{
    if (v >= hi) return hi;
    if (v <= lo) return lo;
    return (int) v;   /* toward zero like psq_st */
}

static void test_skin_positions(void)
{
    enum { N = 4000, NPAL = 64 };
    static double rows[NPAL][3][4];
    static short src[N * 4];
    static short dst[N * 3];
    int i, shift;
    int maxdiff = 0;

    for (shift = 0; shift < 8; shift += 3) {   /* 0, 3, 6 */
        unsigned int gqr = ((unsigned) shift << 24) | ((unsigned) shift << 8) | 0x00070007u;
        make_palette(NPAL, rows);
        for (i = 0; i < N; i++) {
            src[i * 4 + 0] = (short) (rnd() & 0xFFFF);
            src[i * 4 + 1] = (short) (rnd() & 0xFFFF);
            src[i * 4 + 2] = (short) (rnd() & 0xFFFF);
            src[i * 4 + 3] = (short) (rnd() % NPAL);
        }
        memset(dst, 0x55, sizeof(dst));
        re4_calcsk1_s16(dst, src, N, gqr);
        for (i = 0; i < N; i++) {
            double k = ldexp(1.0, -shift);
            double x = src[i * 4] * k, y = src[i * 4 + 1] * k, z = src[i * 4 + 2] * k;
            const double (*m)[4] = rows[src[i * 4 + 3]];
            int r;
            for (r = 0; r < 3; r++) {
                double o = (m[r][0] * x + m[r][1] * y + m[r][2] * z + m[r][3]) * ldexp(1.0, shift);
                int q = clamp_trunc(o, -32768, 32767);
                int d = abs(q - dst[i * 3 + r]);
                if (d > maxdiff) maxdiff = d;
                /* truncation can flip by one unit when the float product lands on an integer boundary */
                CHECK(d <= 1, "s16 skin shift %d vtx %d axis %d: got %d want %d (%.3f)", shift, i, r, dst[i * 3 + r], q, o);
            }
        }
    }
    printf("  s16 skinning: max |diff| vs double = %d\n", maxdiff);
}

static void test_skin_normals_s16(void)
{
    enum { N = 2000, NPAL = 32 };
    static double rows[NPAL][3][4];
    static short src[N * 4];
    static short dst[N * 3];
    int i;
    int maxdiff = 0;

    make_palette(NPAL, rows);
    for (i = 0; i < N; i++) {
        src[i * 4 + 0] = (short) (rnd() & 0xFFFF);
        src[i * 4 + 1] = (short) (rnd() & 0xFFFF);
        src[i * 4 + 2] = (short) (rnd() & 0xFFFF);
        src[i * 4 + 3] = (short) (rnd() % NPAL);
    }
    re4_calcsk1_s16(dst, src, N, 0x32073207u);
    for (i = 0; i < N; i++) {
        const double (*m)[4] = rows[src[i * 4 + 3]];
        int r;
        for (r = 0; r < 3; r++) {
            /* a direction: no translation (the scale -14 trick makes it negligible: |t| / 16384 < 0.13) */
            double o = m[r][0] * src[i * 4] + m[r][1] * src[i * 4 + 1] + m[r][2] * src[i * 4 + 2];
            int q = clamp_trunc(o, -32768, 32767);
            int d = abs(q - dst[i * 3 + r]);
            if (d > maxdiff) maxdiff = d;
            CHECK(d <= 1, "s16 normal %d axis %d: got %d want %d", i, r, dst[i * 3 + r], q);
        }
    }
    printf("  s16 normals (0x32073207): max |diff| vs double = %d\n", maxdiff);
}

static void test_skin_normals_s8(void)
{
    enum { N = 2000, NPAL = 256 };
    static double rows[NPAL][3][4];
    static signed char src[N * 4];
    static signed char dst[N * 3];
    int i;
    int maxdiff = 0;

    make_palette(NPAL, rows);
    for (i = 0; i < N; i++) {
        src[i * 4 + 0] = (signed char) (rnd() & 0xFF);
        src[i * 4 + 1] = (signed char) (rnd() & 0xFF);
        src[i * 4 + 2] = (signed char) (rnd() & 0xFF);
        src[i * 4 + 3] = (signed char) (rnd() & 0xFF);   /* u8 index: all 256 entries */
    }
    re4_calcsk1_s8(dst, src, N, 0x20062006u);
    for (i = 0; i < N; i++) {
        const double (*m)[4] = rows[(unsigned char) src[i * 4 + 3]];
        int r;
        for (r = 0; r < 3; r++) {
            double o = m[r][0] * src[i * 4] + m[r][1] * src[i * 4 + 1] + m[r][2] * src[i * 4 + 2];
            int q = clamp_trunc(o, -128, 127);
            int d = abs(q - dst[i * 3 + r]);
            if (d > maxdiff) maxdiff = d;
            CHECK(d <= 1, "s8 normal %d axis %d: got %d want %d", i, r, dst[i * 3 + r], q);
        }
    }
    printf("  s8 normals (0x20062006): max |diff| vs double = %d\n", maxdiff);
}

static void test_skin_layout(void)
{
    /* one vertex, identity + translation, shift 0: output = input + t; n = 0 touches nothing */
    short src[4] = {100, -200, 300, 5};
    short dst[3] = {7, 7, 7};
    memset(re4_skin_palette[5], 0, sizeof(re4_skin_palette[5]));
    re4_skin_palette[5][0][0] = 1.0f;
    re4_skin_palette[5][1][1] = 1.0f;
    re4_skin_palette[5][2][2] = 1.0f;
    re4_skin_palette[5][3][0] = 10.0f;
    re4_skin_palette[5][3][1] = 20.0f;
    re4_skin_palette[5][3][2] = 30.0f;
    re4_calcsk1_s16(dst, src, 0, 0x00070007u);
    CHECK(dst[0] == 7 && dst[1] == 7 && dst[2] == 7, "n = 0 writes nothing");
    re4_calcsk1_s16(dst, src, 1, 0x00070007u);
    CHECK(dst[0] == 110 && dst[1] == -180 && dst[2] == 330, "identity+t: %d %d %d", dst[0], dst[1], dst[2]);
    /* saturation */
    src[0] = 32767;
    re4_calcsk1_s16(dst, src, 1, 0x00070007u);
    CHECK(dst[0] == 32767, "saturate +: %d", dst[0]);
    src[0] = -32768;
    re4_skin_palette[5][3][0] = -10.0f;
    re4_calcsk1_s16(dst, src, 1, 0x00070007u);
    CHECK(dst[0] == -32768, "saturate -: %d", dst[0]);
    /* the game-facing global */
    re4_gqr6 = 0x32073207u;
    CHECK(re4_gqr6 == 0x32073207u, "re4_gqr6 global");
}

/* ------------------------------------------------------------------------------------------- */
/* math                                                                                          */

static void test_limit_angle(void)
{
    const float PI = RE4_PPC_PI;
    float r;
    CHECK(re4_ppc_limit_angle(0.0f) == 0.0f, "wrap 0");
    CHECK(re4_ppc_limit_angle(1.0f) == 1.0f, "wrap 1");
    CHECK(re4_ppc_limit_angle(-PI) == -PI, "wrap -PI stays");
    r = re4_ppc_limit_angle(PI);
    CHECK(r == PI - RE4_PPC_PI2, "wrap PI -> %g", r);
    r = re4_ppc_limit_angle(3.0f * PI);
    CHECK(r >= -PI && r < PI && fabsf(r - (3.0f * PI - RE4_PPC_PI2)) < 1e-5f, "wrap 3PI -> %g", r);
    r = re4_ppc_limit_angle(-4.0f * PI);
    CHECK(r >= -PI && r < PI && fabsf(r) < 1e-5f, "wrap -4PI -> %g", r);
    r = re4_ppc_limit_angle(100.0f);
    CHECK(r >= -PI && r < PI && fabsf(r - (100.0f - 16.0f * RE4_PPC_PI2)) < 1e-4f, "wrap 100 -> %g", r);
    r = re4_ppc_limit_angle(-100.0f);
    CHECK(r >= -PI && r < PI, "wrap -100 -> %g", r);
}

/* The game's Coeff[] polynomial evaluated in double on an already wrapped angle. */
static double poly_ref(double x)
{
    double s = 0.0, p = x;
    int k;
    for (k = 0; k < 10; k++) {
        s += (double) re4_ppc_sin_coeff[k] * p;
        p *= x;
    }
    return s;
}

static void test_sincos(void)
{
    /* NOTE: the GameCube polynomial itself is only accurate to 2.585e-3 at |x| = PI (3.4e-5 at PI/2,
     * 2e-6 on [-1, 1]); a faithful port cannot be within 1e-5 of sinf everywhere.  So: (a) the C port vs
     * the same polynomial in double (implementation error) and (b) vs libm with the polynomial's bound. */
    const double lo = -4.0 * M_PI, hi = 4.0 * M_PI;
    const int steps = 200000;
    double max_s = 0, max_c = 0, at_s = 0, at_c = 0, max_ps = 0, max_pc = 0, max_s1 = 0;
    int i;
    for (i = 0; i <= steps; i++) {
        float x = (float) (lo + (hi - lo) * i / steps);
        float ws = re4_ppc_limit_angle(x);
        float wc = re4_ppc_limit_angle(x + RE4_PPC_HALF_PI);
        double s = re4_ppc_sinf(x), c = re4_ppc_cosf(x);
        double es = fabs(s - sin((double) x));
        double ec = fabs(c - cos((double) x));
        double ps = fabs(s - poly_ref(ws));
        double pc = fabs(c - poly_ref(wc));
        if (es > max_s) { max_s = es; at_s = x; }
        if (ec > max_c) { max_c = ec; at_c = x; }
        if (ps > max_ps) max_ps = ps;
        if (pc > max_pc) max_pc = pc;
        if (x >= -1.0f && x <= 1.0f && es > max_s1) max_s1 = es;
    }
    printf("  SINF max err vs sin %.3g at %.6f, COSF vs cos %.3g at %.6f over [-4pi, 4pi]\n", max_s, at_s, max_c, at_c);
    printf("  SINF/COSF vs the polynomial in double: %.3g / %.3g; SINF vs sin on [-1,1]: %.3g\n", max_ps, max_pc,
           max_s1);
    CHECK(max_ps < 1e-5 && max_pc < 1e-5, "SINF/COSF within 1e-5 of the game polynomial (%.3g %.3g)", max_ps, max_pc);
    CHECK(max_s < 2.7e-3 && max_c < 2.7e-3, "SINF/COSF within the polynomial's 2.7e-3 of sin/cos (%.3g %.3g)", max_s,
          max_c);
    CHECK(max_s1 < 1e-5, "SINF within 1e-5 of sin on [-1, 1] (%.3g)", max_s1);
    CHECK(re4_ppc_sinf(0.0f) == 0.0f, "SINF(0) == 0 exactly");
    CHECK(fabs(re4_ppc_cosf(0.0f) - poly_ref(RE4_PPC_HALF_PI)) < 1e-6, "COSF(0) == P(pi/2): %.8g",
          re4_ppc_cosf(0.0f));
    CHECK(re4_ppc_sin_coeff[0] == 1.0000012f && re4_ppc_sin_coeff[9] == 4.0048576e-08f, "Coeff table");
}

static void test_sqrt(void)
{
    CHECK(re4_ppc_sqrtf(0.0f) == 0.0f, "SQRTF(0)");
    CHECK(re4_ppc_sqrtf(-4.0f) == 0.0f, "SQRTF(-4) == 0");
    CHECK(re4_ppc_sqrtf(0.00001f) == 0.0f, "SQRTF(1e-5) == 0 (<= threshold)");
    CHECK(re4_ppc_sqrtf(16.0f) == 4.0f, "SQRTF(16)");
    CHECK(fabsf(re4_ppc_sqrtf(2.0f) - 1.41421356f) < 1e-6f, "SQRTF(2)");
}

/* ------------------------------------------------------------------------------------------- */
/* fills                                                                                         */

static void test_memfill(void)
{
    unsigned char buf[128];
    static const unsigned int sizes[] = {0, 1, 3, 4, 15, 16, 17, 31, 32, 33, 64};
    unsigned int si, off;

    for (off = 0; off < 4; off++) {
        for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            unsigned int n = sizes[si], i;
            int ok = 1;
            memset(buf, 0xEE, sizeof(buf));
            memset_asm(buf + 8 + off, 0x1AB, n);   /* c truncated to 0xAB */
            for (i = 0; i < sizeof(buf); i++) {
                unsigned char want = (i >= 8 + off && i < 8 + off + n) ? 0xAB : 0xEE;
                if (buf[i] != want) ok = 0;
            }
            CHECK(ok, "memset_asm off %u n %u", off, n);

            memset(buf, 0xEE, sizeof(buf));
            memclr_asm(buf + 8 + off, n);
            ok = 1;
            for (i = 0; i < sizeof(buf); i++) {
                unsigned char want = (i >= 8 + off && i < 8 + off + n) ? 0x00 : 0xEE;
                if (buf[i] != want) ok = 0;
            }
            CHECK(ok, "memclr_asm off %u n %u", off, n);
        }
    }
    {
        float m[3][3][4];
        int i, ok = 1;
        memset(m, 0xEE, sizeof(m));
        mtxclr_asm(m, 0);
        for (i = 0; i < (int) sizeof(m); i++) if (((unsigned char*) m)[i] != 0xEE) ok = 0;
        CHECK(ok, "mtxclr_asm count 0 is a no-op");
        mtxclr_asm(m, 2);
        ok = 1;
        for (i = 0; i < 2 * 48; i++) if (((unsigned char*) m)[i] != 0) ok = 0;
        for (i = 2 * 48; i < 3 * 48; i++) if (((unsigned char*) m)[i] != 0xEE) ok = 0;
        CHECK(ok, "mtxclr_asm count 2 zeroes 96 bytes exactly");
    }
}

/* ------------------------------------------------------------------------------------------- */
/* yz2                                                                                           */

static unsigned char* read_file(const char* path, unsigned int* len)
{
    FILE* f = fopen(path, "rb");
    unsigned char* p;
    long n;
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (unsigned char*) malloc((size_t) n + 64);
    if (fread(p, 1, (size_t) n, f) != (size_t) n) {
        fclose(f);
        free(p);
        return 0;
    }
    fclose(f);
    memset(p + n, 0, 64);
    *len = (unsigned int) n;
    return p;
}

static void test_yz2_layout(void)
{
    if (sizeof(void*) == 4) {
        CHECK(sizeof(re4_yz2_in_ev) == 0x14, "Yz2InEv size %u", (unsigned) sizeof(re4_yz2_in_ev));
        CHECK(sizeof(re4_yz2_freq) == 0x1C, "Yz2Freq size %u", (unsigned) sizeof(re4_yz2_freq));
        CHECK(sizeof(re4_yz2_model) == 0x24, "Yz2Model size %u", (unsigned) sizeof(re4_yz2_model));
        CHECK(sizeof(re4_yz2_dec) == 0x50, "Yz2Dec size %u", (unsigned) sizeof(re4_yz2_dec));
        CHECK(sizeof(re4_yz2_ctx) == 0x5C, "Yz2Ctx size %u", (unsigned) sizeof(re4_yz2_ctx));
    }
    CHECK(sizeof(re4_yz2_dic_ent) == 0x1004, "Yz2DicEnt size %u", (unsigned) sizeof(re4_yz2_dic_ent));
    CHECK(re4_yz2_work_size() == 0x142800u, "work size %#x", re4_yz2_work_size());
    {
        const unsigned char h[] = "17ec2b\t5ce020\n";
        unsigned int packed, unpacked, stream;
        CHECK(re4_yz2_is_yz2(h, sizeof(h)), "is_yz2 header");
        CHECK(re4_yz2_parse_header(h, sizeof(h), &packed, &unpacked, &stream) == 0 && packed == 0x17ec2b &&
                  unpacked == 0x5ce020 && stream == 0x20,
              "parse_header: %#x %#x %#x", packed, unpacked, stream);
        CHECK(!re4_yz2_is_yz2((const unsigned char*) "\0\0\0\x02\0\0\0\0", 8), "raw container is not yz2");
    }
}

static void test_yz2_file(const char* das, const char* ref)
{
    unsigned int dlen, rlen, base = 0;
    unsigned char* d = read_file(das, &dlen);
    unsigned char* r = read_file(ref, &rlen);
    unsigned char* out;
    void* work;
    long n;
    unsigned int i;

    if (!d || !r) {
        printf("FAIL cannot read %s / %s\n", das, ref);
        g_fail++;
        return;
    }
    if (!re4_yz2_is_yz2(d, dlen)) {
        base = 0x400;   /* a .das container: body at 0x400 (yz2.py main) */
    }
    out = (unsigned char*) malloc(rlen + 16);
    memset(out, 0xCD, rlen + 16);
    work = malloc(re4_yz2_work_size());
    n = re4_yz2_decode(d + base, dlen - base, out, rlen, work, re4_yz2_work_size());
    CHECK(n == (long) rlen, "yz2 decode of %s: returned %ld, reference %u bytes (status %d at %#x)", das, n, rlen,
          re4_yz2_last_status, re4_yz2_last_written);
    if (n == (long) rlen) {
        for (i = 0; i < rlen; i++) {
            if (out[i] != r[i]) {
                break;
            }
        }
        CHECK(i == rlen, "yz2 output differs from the oracle at %#x (got %02x want %02x)", i, out[i], r[i]);
        if (i == rlen) {
            printf("  yz2: %s -> %u bytes, identical to %s\n", das, rlen, ref);
        }
    }
    CHECK(out[rlen] == 0xCD, "yz2 did not write past the end");

    /* a truncated stream must stop with an error rather than read past the end */
    memset(out, 0, rlen);
    n = re4_yz2_decode(d + base, (dlen - base) / 2, out, rlen, work, re4_yz2_work_size());
    CHECK(n == RE4_YZ2_E_STREAM, "truncated stream -> error (%ld)", n);
    /* too small a destination */
    n = re4_yz2_decode(d + base, dlen - base, out, rlen / 2, work, re4_yz2_work_size());
    CHECK(n == RE4_YZ2_E_SIZE, "small dst -> E_SIZE (%ld)", n);

    free(work);
    free(out);
    free(d);
    free(r);
}

int main(int argc, char** argv)
{
    printf("ct02: %u-bit build\n", (unsigned) (sizeof(void*) * 8));
    test_gqr();
    test_skin_layout();
    test_skin_positions();
    test_skin_normals_s16();
    test_skin_normals_s8();
    test_limit_angle();
    test_sincos();
    test_sqrt();
    test_memfill();
    test_yz2_layout();
    if (argc >= 3) {
        test_yz2_file(argv[1], argv[2]);
    } else {
        printf("  yz2 file check SKIPPED (usage: ct02 <stream.das> <reference.bin>)\n");
    }
    printf("ct02: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
