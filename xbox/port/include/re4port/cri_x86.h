/* xbox/port/include/re4port/cri_x86.h -- included first by every CRI Middleware unit prepared by
 * xbox/tools/prep_cri.py (CT-07). Maps the CodeWarrior/Gekko intrinsics the ADX/Sofdec sources use to
 * x86 equivalents. Cache hints and line clears are no-ops (no locked cache, coherent DMA on Xbox);
 * the rotate/insert intrinsics keep the PowerPC (MSB = bit 0) numbering. */
#ifndef RE4PORT_CRI_X86_H
#define RE4PORT_CRI_X86_H




/* cache block ops */
#define __dcbt(p, o)   ((void)(p), (void)(o))
#define __dcbtst(p, o) ((void)(p), (void)(o))
#define __dcbz(p, o)   ((void)(p), (void)(o))
#define __dcbf(p, o)   ((void)(p), (void)(o))
#define __dcbst(p, o)  ((void)(p), (void)(o))
#define __dcbi(p, o)   ((void)(p), (void)(o))
#define __sync()       ((void)0)
#define __isync()      ((void)0)
#define __eieio()      ((void)0)

static inline unsigned int re4_ppc_mask(unsigned int mb, unsigned int me)
{
	unsigned int a = 0xFFFFFFFFu >> mb;
	unsigned int b = 0xFFFFFFFFu << (31 - me);
	return mb <= me ? (a & b) : (a | b);
}

static inline unsigned int re4_ppc_rotl(unsigned int v, unsigned int sh)
{
	sh &= 31;
	return sh ? (v << sh) | (v >> (32 - sh)) : v;
}

/* rlwimi: insert rotl(s, sh) into a under mask(mb, me); rlwinm: rotl(s, sh) & mask(mb, me) */
#define __rlwimi(a, s, sh, mb, me) \
	((((unsigned int)(a)) & ~re4_ppc_mask((mb), (me))) | (re4_ppc_rotl((unsigned int)(s), (sh)) & re4_ppc_mask((mb), (me))))
#define __rlwinm(s, sh, mb, me) (re4_ppc_rotl((unsigned int)(s), (sh)) & re4_ppc_mask((mb), (me)))
#define __rlwnm(s, sh, mb, me) __rlwinm((s), (sh), (mb), (me))

static inline unsigned int __re4_lwbrx(const void *p, int o)
{
	const unsigned char *b = (const unsigned char *)p + o;
	return (unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16) | ((unsigned int)b[3] << 24);
}
static inline unsigned short __re4_lhbrx(const void *p, int o)
{
	const unsigned char *b = (const unsigned char *)p + o;
	return (unsigned short)(b[0] | (b[1] << 8));
}
#define __lwbrx(p, o) __re4_lwbrx((p), (o))
#define __lhbrx(p, o) __re4_lhbrx((p), (o))

static inline int __re4_cntlzw(unsigned int v) { return v ? __builtin_clz(v) : 32; }
#define __cntlzw(v) __re4_cntlzw((unsigned int)(v))

/* reciprocal square root estimate; the callers refine with Newton steps */
#define __frsqrte(x) (1.0 / __builtin_sqrt((double)(x)))
#define __fres(x) (1.0f / (float)(x))
#define __fabs(x) __builtin_fabs(x)

/* big-endian loads for the stream parsers (MPEG bitstream words, SFD/ADX headers) */
static inline unsigned int re4_be32(const void *p)
{
	const unsigned char *b = (const unsigned char *)p;
	return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | (unsigned int)b[3];
}
static inline unsigned short re4_be16(const void *p)
{
	const unsigned char *b = (const unsigned char *)p;
	return (unsigned short)((b[0] << 8) | b[1]);
}

/* an MWCC asm block prep_cri.py could not port: trap if it is ever reached */
#ifdef __cplusplus
extern "C"
#endif
void re4_cri_asm_unported(const char *file, int line);
#define RE4_CRI_ASM_UNPORTED() re4_cri_asm_unported(__FILE__, __LINE__)

#endif
