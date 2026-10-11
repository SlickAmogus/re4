/* xbox/port/mtx/math_ppc.h -- CT-02: scalar C versions of the PowerPC/paired-single math helpers of
 * src/game/math_sub.cpp (SQRTF, SINF, COSF, LIMIT_ANGLE).
 *
 * The game keeps its own names (include/math_sub_decl.h); xbox/tools/prep.py currently rewrites the asm
 * bodies in math_sub.cpp to __builtin_sqrtf/sinf/cosf and a C wrap loop.  These functions are the exact
 * polynomial the GameCube build evaluates, for when bit-closer behaviour is wanted (point prep.py's
 * replacement at re4_ppc_sinf / re4_ppc_cosf) and for the tests. */
#ifndef RE4PORT_MATH_PPC_H
#define RE4PORT_MATH_PPC_H

#define RE4_PPC_PI 3.1415927f
#define RE4_PPC_PI2 6.2831855f
#define RE4_PPC_HALF_PI 1.5707964f

#ifdef __cplusplus
extern "C" {
#endif

/* The Taylor coefficients of math_sub.cpp (Coeff[10]): the odd-power (sin) terms at even indices,
 * small even-power correction terms at odd indices. */
extern const float re4_ppc_sin_coeff[10];

float re4_ppc_sqrtf(float x);        /* SQRTF: 0 for x <= 1e-5, else sqrt */
float re4_ppc_sinf(float x);         /* SINF: LIMIT_ANGLE then the degree-10 series */
float re4_ppc_cosf(float x);         /* COSF: the same series on LIMIT_ANGLE(x + PI/2) */
float re4_ppc_limit_angle(float x);  /* LIMIT_ANGLE: wrap into [-PI, PI) by repeated +-2PI */

/* The series alone (no wrap), for x in [-PI, PI). */
float re4_ppc_sin_series(float x);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_MATH_PPC_H */
