/* xbox/port/include/re4port/cri/cftyp422_c.inc -- portable C for the PPC colour converters of
 * src/lib/cftyp422_ppc.c (CT-07). Output is the GameCube texture memory image, byte for byte:
 *   CFT_Ycc420plnToY84C44: Y as 8x4 I8 tiles (32 bytes: four 8-byte rows), CbCr as 4x4 IA8 tiles
 *     (Cb, Cr byte pairs) -- a transcription of the original pointer walk (exact).
 *   cnv{Dynamic,Static}Ycc420plnToArgb8888: 4x4 RGBA8 tiles (32 bytes AR pairs, then 32 bytes GB).
 *     Written from the table semantics, not from the asm: the dynamic form sums the A/R/G/B
 *     contributions of tbl (Y at +0, Cb at +0x1000, Cr at +0x2000, four Float32 per value) and
 *     truncates/saturates like psq_st u8 (exact by construction); the static form uses
 *     y/cb_g/cb_b/cr_r/cr_g with A = 255 and +0.5 rounding (may differ by 1 LSB from the original).
 *     The game only uses the Y84C44 path (cSofdec::m_draw_mode is never set). */
#ifdef RE4_CFT_Y84C44
#undef RE4_CFT_Y84C44
void CFT_Ycc420plnToY84C44(const CFT_YCC420PLN *src, void *ybuf, void *cbuf, Sint32 width, Sint32 height)
{
	Sint32 ywidth = src->ywidth;
	Sint32 yskip = ywidth * 3 / 8 * 8;
	Sint32 cnt = ywidth / 8;
	Sint32 hblk = height / 4;
	Sint32 dskip = (width - ywidth) / 8 * 32;
	const Uint8 *y0 = src->y;
	Uint8 *d = (Uint8 *)ybuf;
	Sint32 i, n, r;

	for (i = 0; i < hblk; i++) {
		for (n = 0; n < cnt; n++) {
			for (r = 0; r < 4; r++) {
				__builtin_memcpy(d, y0 + r * ywidth, 8);
				d += 8;
			}
			y0 += 8;
		}
		d += dskip;
		y0 += yskip;
	}

	{
		Sint32 ccnt = src->ywidth / 2 / 4;
		Sint32 cskip = (src->cbwidth - src->ywidth / 2) / 4;       /* in words */
		Sint32 chblk = height / 2 / 4;
		Sint32 cw = src->cbwidth;                                  /* row pitch in bytes */
		const Uint8 *cb0 = src->cb;
		const Uint8 *cr0 = src->cr;
		Uint8 *c = (Uint8 *)cbuf;

		for (i = 0; i < chblk; i++) {
			for (n = 0; n < ccnt; n++) {
				for (r = 0; r < 4; r++) {
					const Uint8 *b = cb0 + r * cw;
					const Uint8 *q = cr0 + r * cw;
					c[0] = b[0]; c[1] = q[0]; c[2] = b[1]; c[3] = q[1];
					c[4] = b[2]; c[5] = q[2]; c[6] = b[3]; c[7] = q[3];
					c += 8;
				}
				cb0 += 4;
				cr0 += 4;
			}
			c += cskip * 8 * 4;
			cb0 += (3 * (cw / 4) + cskip) * 4;
			cr0 += (3 * (cw / 4) + cskip) * 4;
		}
	}
}

#endif /* Y84C44 */

#ifdef RE4_CFT_ARGB
#undef RE4_CFT_ARGB
static Uint8 cftyp_sat8(Float32 v)
{
	if (v <= 0.0f) return 0;
	if (v >= 255.0f) return 255;
	return (Uint8)(Sint32)v;
}

static void cftyp_argb_tiles(const CFT_YCC420PLN *src, const CFT_ARGBDST *dst, const Float32 *tbl)
{
	Sint32 w = dst->width, h = dst->height, pitch = dst->pitch;
	Sint32 ty, tx, py, px;

	for (ty = 0; ty < h / 4; ty++) {
		for (tx = 0; tx < w / 4; tx++) {
			Uint8 *t = dst->buf + ty * 4 * pitch + tx * 64;
			for (py = 0; py < 4; py++) {
				Sint32 row = ty * 4 + py;
				for (px = 0; px < 4; px++) {
					Sint32 col = tx * 4 + px;
					Sint32 k = (py * 4 + px) * 2;
					Uint32 Y = src->y[row * src->ywidth + col];
					Uint32 U = src->cb[(row >> 1) * src->cbwidth + (col >> 1)];
					Uint32 V = src->cr[(row >> 1) * src->crwidth + (col >> 1)];
					Float32 a, r, g, b;
					if (tbl != NULL) {
						const Float32 *ty_ = tbl + Y * 4;
						const Float32 *tu = (const Float32 *)((const Uint8 *)tbl + 0x1000) + U * 4;
						const Float32 *tv = (const Float32 *)((const Uint8 *)tbl + 0x2000) + V * 4;
						a = ty_[0] + tu[0] + tv[0];
						r = ty_[1] + tu[1] + tv[1];
						g = ty_[2] + tu[2] + tv[2];
						b = ty_[3] + tu[3] + tv[3];
					} else {
						a = 255.0f;
						r = y[Y] + cr_r[V] + 0.5f;
						g = y[Y] + cb_g[U] + cr_g[V] + 0.5f;
						b = y[Y] + cb_b[U] + 0.5f;
					}
					t[k] = cftyp_sat8(a);
					t[k + 1] = cftyp_sat8(r);
					t[32 + k] = cftyp_sat8(g);
					t[33 + k] = cftyp_sat8(b);
				}
			}
		}
	}
}

void cnvDynamicYcc420plnToArgb8888(CFT_YCC420PLN *src, CFT_ARGBDST *dst, Float32 *tbl)
{
	cftyp_argb_tiles(src, dst, tbl);
}

void cnvStaticYcc420plnToArgb8888(CFT_YCC420PLN *src, CFT_ARGBDST *dst)
{
	cftyp_argb_tiles(src, dst, NULL);
}
#endif /* ARGB */
