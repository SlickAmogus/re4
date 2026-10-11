/* xbox/port/include/re4port/cri/mpv_umc_c.inc -- portable C for the paired-single merge kernels of
 * src/lib/mpv_umc.c (CT-07). Included by prep_cri.py in place of the asm bodies of mpvumc_BiMakeMb,
 * mpvumc_OneMakeMb and mpvumc_OutputIntra6blk. Byte layouts are the GameCube's (memory order):
 *   prediction work (wk->work / wk->work2): six 8x8 u8 blocks of 64 bytes (Cb, Cr, Y0..Y3)
 *   residual (wk->buf0, or `blk` for intra): six 8x8 s16 blocks of 128 bytes
 *   output: ob->rt[i].p with pitch ob->rt[i].n
 * `cbp` carries the coded flag of block i in its sign bit, shifted left once per block.
 * GQRs (MPVUMC_SetGqr): GQR3 s16 load / u8 store, GQR4 u8 load/store, GQR5 s16 load x2, u8 store x0.5;
 * psq_st to u8 truncates toward zero and saturates to 0..255. dcbz (line clear before the stores) is
 * dropped: every byte it cleared is written by a later block in decode order. */
#define MPVUMC_SAT8(v) ((Uint8)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))

void mpvumc_BiMakeMb(MPVUMC_WORK *wk, MPVUMC_OUTBLK *ob, Sint32 cbp)
{
	const Sint16 *res = (const Sint16 *)wk->buf0;
	const Uint8 *p0 = wk->work;
	const Uint8 *p1 = wk->work2;
	Sint32 i, y, x;

	for (i = 0; i < 6; i++) {
		Uint8 *d = (Uint8 *)ob->rt[i].p;
		Sint32 pitch = ob->rt[i].n;
		if (cbp < 0) {
			for (y = 0; y < 8; y++) {
				for (x = 0; x < 8; x++) {
					/* trunc(((a + b + 1) + 2r) * 0.5) == ((a + b + 1) >> 1) + r for non-negative sums */
					Sint32 v = (Sint32)p0[x] + (Sint32)p1[x] + 1 + 2 * (Sint32)res[x];
					d[x] = MPVUMC_SAT8(v < 0 ? 0 : v >> 1);
				}
				p0 += 8; p1 += 8; res += 8; d += pitch;
			}
		} else {
			for (y = 0; y < 8; y++) {
				for (x = 0; x < 8; x++) {
					d[x] = (Uint8)(((Sint32)p0[x] + (Sint32)p1[x] + 1) >> 1);
				}
				p0 += 8; p1 += 8; d += pitch;
			}
			res += 64;
		}
		cbp <<= 1;
	}
}

void mpvumc_OneMakeMb(MPVUMC_WORK *wk, MPVUMC_OUTBLK *ob, Sint32 cbp)
{
	const Sint16 *res = (const Sint16 *)wk->buf0;
	const Uint8 *p0 = wk->work;
	Sint32 i, y, x;

	for (i = 0; i < 6; i++) {
		Uint8 *d = (Uint8 *)ob->rt[i].p;
		Sint32 pitch = ob->rt[i].n;
		if (cbp < 0) {
			for (y = 0; y < 8; y++) {
				for (x = 0; x < 8; x++) {
					Sint32 v = (Sint32)p0[x] + (Sint32)res[x];
					d[x] = MPVUMC_SAT8(v);
				}
				p0 += 8; res += 8; d += pitch;
			}
		} else {
			for (y = 0; y < 8; y++) {
				memcpy(d, p0, 8);
				p0 += 8; d += pitch;
			}
			res += 64;
		}
		cbp <<= 1;
	}
}

void mpvumc_OutputIntra6blk(Uint8 *blk, MPVUMC_OUTBLK *ob, Uint8 *clip)
{
	const Sint16 *res = (const Sint16 *)blk;
	Sint32 i, y, x;

	(void)clip;
	for (i = 0; i < 6; i++) {
		Uint8 *d = (Uint8 *)ob->rt[i].p;
		Sint32 pitch = ob->rt[i].n;
		for (y = 0; y < 8; y++) {
			for (x = 0; x < 8; x++) {
				d[x] = MPVUMC_SAT8((Sint32)res[x]);
			}
			res += 8; d += pitch;
		}
	}
}

void MPVUMC_SetGqr(void)
{
}
