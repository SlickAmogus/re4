/* re4port/skin.h -- CT-02: C replacement for the paired-single skinning asm in src/game/trans.cpp
 * (CalcSk1_x / CalcSk1_x2 / setupGQR6).
 *
 * The GameCube build keeps the blended matrix palette in the locked L1 cache at 0xE0000000, one ROMtx
 * (4x3 f32, column-major, 0x30 bytes) per entry, written by MakeWeightPalette / MakeWeightPaletteExt
 * (trans.cpp lines 847 / 899: PSMTXReorder(m, (f32(*)[3]) (0xE0000000 + i * 0x30))).  The port keeps it in
 * a normal array; xbox/tools/prep.py is expected to redirect those two writes to RE4_SKIN_PALETTE(i).
 *
 * GQR6 (SPR 918) is the quantisation register the psq_l / psq_st in the skinning loops use; the port
 * stores the raw register value in re4_gqr6 (setupGQR6 is rewritten by prep.py to do that) and the C
 * loops decode LD_SCALE / ST_SCALE from it.  Layout (IBM bit numbers, 0 = MSB):
 *   2-7 LD_SCALE (6-bit two's complement), 13-15 LD_TYPE, 18-23 ST_SCALE, 29-31 ST_TYPE
 *   types: 0 f32, 4 u8, 5 u16, 6 s8, 7 s16.  load: int * 2^-LD_SCALE; store: trunc(f * 2^ST_SCALE), clamped.
 */
#ifndef RE4PORT_SKIN_H
#define RE4PORT_SKIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* 16 KB locked cache / 0x30 = 341 entries. CalcSk1_x indexes with an s16, CalcSk1_x2 with a u8. */
#define RE4_SKIN_PALETTE_MAX 341

extern float re4_skin_palette[RE4_SKIN_PALETTE_MAX][4][3];
#define RE4_SKIN_PALETTE(i) ((float(*)[3]) re4_skin_palette[(i)])

extern unsigned int re4_gqr6; /* raw GQR6 value last passed to setupGQR6 */

/* GQR field decoding (shared with the tests). */
unsigned int re4_gqr_ld_type(unsigned int gqr);   /* 0..7 */
unsigned int re4_gqr_st_type(unsigned int gqr);   /* 0..7 */
int re4_gqr_ld_scale(unsigned int gqr);           /* -32..31 */
int re4_gqr_st_scale(unsigned int gqr);           /* -32..31 */
float re4_gqr_dequant_factor(int scale);          /* 2^-scale */
float re4_gqr_quant_factor(int scale);            /* 2^scale */

/* CalcSk1_x: n vertices of {s16 x, y, z, s16 matrix index} (8 bytes) -> {s16 x, y, z} (6 bytes),
 * through re4_skin_palette[index] with GQR6 = `gqr` (LD_TYPE/ST_TYPE must be 7). */
void re4_calcsk1_s16(void* dst, const void* src, unsigned int n, unsigned int gqr);

/* CalcSk1_x2: n normals of {s8 x, y, z, u8 matrix index} (4 bytes) -> {s8 x, y, z} (3 bytes),
 * GQR6 LD_TYPE/ST_TYPE must be 6. */
void re4_calcsk1_s8(void* dst, const void* src, unsigned int n, unsigned int gqr);

#ifdef __cplusplus
}
#endif
#endif /* RE4PORT_SKIN_H */
