/* xbox/port/mtx/math_ppc.cpp -- CT-02: scalar C port of the paired-single math asm in
 * src/game/math_sub.cpp.
 *
 * SINF (math_sub.cpp 716..751), register reading of the asm with x already wrapped:
 *   f3 = powx = {1, 1};  f4 = sum = {0, 0}
 *   ps_merge00 f2, f3, x ; ps_muls0 f2, f2, x      -> f2 = {x, x^2}
 *   for k = 0..4: f5 = {Coeff[2k], Coeff[2k+1]}
 *                 ps_muls1 f3, f2, f3              -> f3 = f2 * f3[1] = {x^(2k+1), x^(2k+2)}
 *                 ps_madd  f4, f3, f5, f4          -> f4 += f3 * f5
 *   ps_sum0 r, f4, f4, f4                          -> r = f4[0] + f4[1]
 * so sin(x) ~ (c0 x + c2 x^3 + c4 x^5 + c6 x^7 + c8 x^9) + (c1 x^2 + c3 x^4 + c5 x^6 + c7 x^8 + c9 x^10),
 * each half accumulated in order in single precision (fused on the Gekko).  COSF = SINF(x + pi/2).
 *
 * SQRTF (656): frsqrte + one Newton step, 0 for x <= 1e-5.  The port uses sqrtf: the result differs
 * from the Gekko estimate by at most a few ulp and nothing in the game depends on the exact bits.
 * LIMIT_ANGLE (790): the SDK-style wrap loop. */
#include "re4port/math_ppc.h"
#include <math.h>

extern "C" {

const float re4_ppc_sin_coeff[10] = {
    1.0000012f, 2.9073722e-06f, -0.16666685f, -5.727683e-06f, 0.008331681f,
    4.281338e-06f, -0.00019622728f, -6.4099936e-07f, 2.363633e-06f, 4.0048576e-08f,
};

float re4_ppc_sqrtf(float x)
{
    if (x <= 0.00001f) {
        return 0.0f;
    }
    return sqrtf(x);
}

float re4_ppc_limit_angle(float x)
{
    const float min = -RE4_PPC_PI;
    const float max = RE4_PPC_PI;
    const float step = RE4_PPC_PI2;

    /* fcmpu x, max; blt 1f : the subtract loop runs when !(x < max) (NaN included, once) */
    if (!(x < max)) {
        do {
            x -= step;
        } while (x >= max);
    } else if (!(x >= min)) {
        do {
            x += step;
        } while (x < min);
    }
    return x;
}

float re4_ppc_sin_series(float x)
{
    const float* c = re4_ppc_sin_coeff;
    float p0 = x;          /* f2[0] = x        */
    float p1 = x * x;      /* f2[1] = x^2      */
    float t0 = 1.0f;       /* f3 = powx {1, 1} */
    float t1 = 1.0f;
    float s0 = 0.0f;       /* f4 = sum {0, 0}  */
    float s1 = 0.0f;
    int k;

    for (k = 0; k < 5; k++) {
        /* ps_muls1 f3, f2, f3: both halves scaled by f3[1] */
        float m = t1;
        t0 = p0 * m;
        t1 = p1 * m;
        /* ps_madd f4, f3, f5, f4 */
        s0 = t0 * c[2 * k] + s0;
        s1 = t1 * c[2 * k + 1] + s1;
    }
    return s0 + s1;
}

float re4_ppc_sinf(float x)
{
    return re4_ppc_sin_series(re4_ppc_limit_angle(x));
}

float re4_ppc_cosf(float x)
{
    return re4_ppc_sin_series(re4_ppc_limit_angle(x + RE4_PPC_HALF_PI));
}

} /* extern "C" */
