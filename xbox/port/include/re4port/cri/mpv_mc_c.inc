/* xbox/port/include/re4port/cri/mpv_mc_c.inc -- portable C motion-compensation kernels replacing the
 * big-endian SWAR / PPC-asm "TuneC" kernels of src/lib/mpv_mc.c (8x8) and src/lib/mpv_mcy.c (16x16)
 * (CT-07). The originals pack four pixels MSB-first into each Uint32 of mc->dst, i.e. the bytes land in
 * pixel order in memory on the big-endian Gekko; these versions write the same memory image byte by byte.
 *   8x8:   dst = 64 bytes, row r at r*8.
 *   16x16: dst = four 8x8 blocks, pixel (r, c) at (r >> 3) * 128 + (c >> 3) * 64 + (r & 7) * 8 + (c & 7).
 * src2 is whatever mpvumc_OneReadMb set (src + stride, + 1 in the mcflag approximation). */
#ifndef RE4_CRI_MPV_MC_C_INC
#define RE4_CRI_MPV_MC_C_INC

#define RE4_MC_KERNEL(NAME, N, EXPR)                                                            \
	void NAME(MPVMC *mc)                                                                        \
	{                                                                                           \
		const Uint8 *s = mc->src;                                                               \
		const Uint8 *t = mc->src2;                                                              \
		Uint8 *d = (Uint8 *)mc->dst;                                                            \
		Sint32 stride = mc->stride;                                                             \
		Sint32 r, c;                                                                            \
		for (r = 0; r < (N); r++) {                                                             \
			Uint8 *o = (N) == 8 ? d + r * 8 : d + (r >> 3) * 128 + (r & 7) * 8;                 \
			for (c = 0; c < (N); c++) {                                                         \
				o[(N) == 8 ? c : (c >> 3) * 64 + (c & 7)] = (Uint8)(EXPR);                      \
			}                                                                                   \
			s += stride;                                                                        \
			t += stride;                                                                        \
		}                                                                                       \
		(void)t;                                                                                \
	}

#define RE4_MC_1P s[c]
#define RE4_MC_H2 (((Uint32)s[c] + (Uint32)s[c + 1] + 1) >> 1)
#define RE4_MC_V2 (((Uint32)s[c] + (Uint32)t[c] + 1) >> 1)
#define RE4_MC_4P (((Uint32)s[c] + (Uint32)s[c + 1] + (Uint32)t[c] + (Uint32)t[c + 1] + 2) >> 2)

#endif

#ifdef RE4_CRI_MC08
RE4_MC_KERNEL(MPVMC08_OneRef1p_TuneC, 8, RE4_MC_1P)
RE4_MC_KERNEL(MPVMC08_OneRefH2_TuneC, 8, RE4_MC_H2)
RE4_MC_KERNEL(MPVMC08_OneRefV2_TuneC, 8, RE4_MC_V2)
RE4_MC_KERNEL(MPVMC08_OneRef4p_TuneC, 8, RE4_MC_4P)
#undef RE4_CRI_MC08
#endif

#ifdef RE4_CRI_MC16
RE4_MC_KERNEL(MPVMC16_OneRef1p_TuneC, 16, RE4_MC_1P)
RE4_MC_KERNEL(MPVMC16_OneRefH2_TuneC, 16, RE4_MC_H2)
RE4_MC_KERNEL(MPVMC16_OneRefV2_TuneC, 16, RE4_MC_V2)
RE4_MC_KERNEL(MPVMC16_OneRef4p_TuneC, 16, RE4_MC_4P)
#undef RE4_CRI_MC16
#endif
