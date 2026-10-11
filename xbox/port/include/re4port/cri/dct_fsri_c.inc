/* xbox/port/include/re4port/cri/dct_fsri_c.inc -- portable C for DCT_FsriTransCore (src/lib/dct_fsri.c),
 * a literal transcription of the paired-single AAN IDCT (CT-07). Each `ps` register of the original
 * holds two lanes; here every op runs on both lanes (index L).
 *   input  blk: 64 Float32 in the interleaved order of dctfsri_Idx (row 2k col j at [16k + 2j],
 *               row 2k+1 col j at [16k + 2j + 1]);
 *   row pass:   four row pairs -> work, 8 Float32 per row, rows in natural order;
 *   column pass: two columns per step, rounded half away from zero (ps_sel +-0.5), stored as s16
 *               through GQR7 (truncate, saturate) -> tbl[i], 8 s16 per row.
 * Constants B0TableOrg: c1 = sqrt2, c2 = 2.6131, c3 = 1.0824, c4 = 0.7654, c5 = 0.5, c6 = -0.5. */
static Sint16 dctfsri_s16(Float32 v)
{
	v += (v >= 0.0f) ? B0TableOrg[8] : B0TableOrg[10];
	if (v >= 32767.0f) return 32767;
	if (v <= -32768.0f) return -32768;
	return (Sint16)(Sint32)v;
}

void DCT_FsriTransCore(DCT_PA *pa, Sint32 cbp)
{
	Sint8 *flg = pa->cbp;
	Float32 *blk = pa->blk;
	Sint16 **tbl = pa->tbl;
	Float32 *work = (Float32 *)pa->work;
	const Float32 c1 = B0TableOrg[0], c2 = B0TableOrg[2], c3 = B0TableOrg[4], c4 = B0TableOrg[6];
	Sint32 i, k, L;

	for (i = 0; i < 6; i++) {
		if (cbp < 0) {
			if (flg[0] == 0) {
				Float32 dc = blk[0];
				Sint32 n;
				Sint16 *o = tbl[0];
				if (dc < 0.0) {
					n = (Sint16)(dc - 0.5f);
				} else {
					n = (Sint16)(0.5f + dc);
				}
				for (k = 0; k < 64; k++) {
					o[k] = (Sint16)n;
				}
			} else {
				Sint16 *o = tbl[0];
				/* row pass: row pair k, lane L = row 2k + L */
				for (k = 0; k < 4; k++) {
					for (L = 0; L < 2; L++) {
						const Float32 *s = blk + 16 * k + L;
						Float32 *w = work + (2 * k + L) * 8;
						Float32 x0 = s[0], x7 = s[2], x8 = s[4], x9 = s[6];
						Float32 x10 = s[8], x11 = s[10], x12 = s[12], x13 = s[14], x31;
						x31 = x8 - x12;
						x12 = x8 + x12;
						x8 = x0 - x10;
						x31 = x31 * c1;
						x10 = x0 + x10;
						x31 = x31 - x12;
						x0 = x10 - x12;
						x12 = x10 + x12;
						x10 = x8 - x31;
						x31 = x8 + x31;
						x8 = x11 - x9;
						x9 = x11 + x9;
						x11 = x7 - x13;
						x13 = x7 + x13;
						x7 = x13 - x9;
						x9 = x13 + x9;
						x13 = x8 - x11;
						x11 = x11 * c3;
						x8 = x8 * c2;
						x13 = x13 * c4;
						x7 = x7 * c1;
						x11 = x11 - x13;
						x8 = x8 - x13;
						x13 = x12 - x9;
						x11 = x11 - x9;
						x9 = x12 + x9;
						x7 = x7 - x11;
						x12 = x31 - x11;
						x11 = x31 + x11;
						x8 = x8 - x7;
						x31 = x0 - x8;
						x8 = x0 + x8;
						x0 = x10 - x7;
						x7 = x10 + x7;
						w[0] = x9; w[1] = x11; w[2] = x7; w[3] = x8;
						w[4] = x31; w[5] = x0; w[6] = x12; w[7] = x13;
					}
				}
				/* column pass: column pair k, lane L = column 2k + L */
				for (k = 0; k < 4; k++) {
					for (L = 0; L < 2; L++) {
						const Float32 *s = work + 2 * k + L;
						Sint16 *d = o + 2 * k + L;
						Float32 x0 = s[0], x11 = s[8], x12 = s[16], x9 = s[24];
						Float32 x31 = s[32], x8 = s[40], x13 = s[48], x7 = s[56], x10;
						x10 = x12 - x13;
						x13 = x12 + x13;
						x12 = x0 - x31;
						x10 = x10 * c1;
						x31 = x0 + x31;
						x10 = x10 - x13;
						x0 = x31 - x13;
						x13 = x31 + x13;
						x31 = x12 - x10;
						x10 = x12 + x10;
						x12 = x8 - x9;
						x9 = x8 + x9;
						x8 = x11 - x7;
						x7 = x11 + x7;
						x11 = x7 - x9;
						x9 = x7 + x9;
						x7 = x12 - x8;
						x8 = x8 * c3;
						x12 = x12 * c2;
						x7 = x7 * c4;
						x11 = x11 * c1;
						x8 = x8 - x7;
						x12 = x12 - x7;
						x7 = x13 - x9;
						x8 = x8 - x9;
						x9 = x13 + x9;
						x11 = x11 - x8;
						x13 = x10 - x8;
						x8 = x10 + x8;
						x12 = x12 - x11;
						x10 = x0 - x12;
						x12 = x0 + x12;
						x0 = x31 - x11;
						x11 = x31 + x11;
						d[0] = dctfsri_s16(x9);
						d[8] = dctfsri_s16(x8);
						d[16] = dctfsri_s16(x11);
						d[24] = dctfsri_s16(x12);
						d[32] = dctfsri_s16(x10);
						d[40] = dctfsri_s16(x0);
						d[48] = dctfsri_s16(x13);
						d[56] = dctfsri_s16(x7);
					}
				}
			}
		}
		cbp <<= 1;
		blk += 64;
		tbl++;
		flg++;
	}
}

void DCT_FsriSetGqr(void)
{
}
