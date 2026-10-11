/* dspadpcm_ref_enc.c — brute-force reference DSP-ADPCM encoder (see header). */
#include "dspadpcm_ref_enc.h"

#include <math.h>
#include <string.h>

/* Q11 pairs: (0,0) (1,0) (0,1) (2,-1) (1.5,-0.5) (1.75,-0.75) (1.9,-0.9) (1.96,-1.0) */
const int16_t dspadpcm_ref_test_coef[16] = {
    0,    0,
    2048, 0,
    0,    2048,
    4096, -2048,
    3072, -1024,
    3584, -1536,
    3891, -1843,
    4017, -2048
};

static inline int16_t clamp16(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t) v;
}

/* Exactly dspadpcm.c's dsp_step() for one nibble value n (-8..7). */
static inline int16_t recon_step(int n, int scale, int32_t c1, int32_t c2, int16_t h1, int16_t h2)
{
    int64_t acc = ((int64_t) n << scale) * 2048 + 1024 + (int64_t) c1 * h1 + (int64_t) c2 * h2;
    return clamp16(acc >> 11);
}

/* Encode one frame with a given pair/scale; returns the squared error, writes nibbles and the
 * resulting history. */
static double try_frame(const int16_t* pcm, int count, int pair, int scale, const int16_t coef[16],
                        int16_t h1_in, int16_t h2_in, int8_t nib[14], int16_t* h1_out, int16_t* h2_out)
{
    int32_t c1  = coef[pair * 2];
    int32_t c2  = coef[pair * 2 + 1];
    int16_t h1  = h1_in;
    int16_t h2  = h2_in;
    double  err = 0.0;
    int     i;

    for (i = 0; i < 14; i++) {
        int16_t s   = (i < count) ? pcm[i] : 0;
        int64_t num = (int64_t) s * 2048 - 1024 - (int64_t) c1 * h1 - (int64_t) c2 * h2;
        double  den = (double) (2048 << scale);
        int     n   = (int) floor((double) num / den + 0.5);
        int16_t r;
        double  d;
        if (n < -8) n = -8;
        if (n > 7) n = 7;
        r      = recon_step(n, scale, c1, c2, h1, h2);
        d      = (double) s - (double) r;
        err   += d * d;
        nib[i] = (int8_t) n;
        h2     = h1;
        h1     = r;
    }
    *h1_out = h1;
    *h2_out = h2;
    return err;
}

uint32_t dspadpcm_ref_encode(const int16_t* pcm, uint32_t samples, const int16_t coef[16], uint8_t* out, int16_t* recon)
{
    uint32_t frames = (samples + DSPADPCM_FRAME_SAMPLES - 1u) / DSPADPCM_FRAME_SAMPLES;
    uint32_t f;
    int16_t  h1 = 0, h2 = 0;

    for (f = 0; f < frames; f++) {
        uint32_t base  = f * DSPADPCM_FRAME_SAMPLES;
        int      count = (int) ((samples - base < DSPADPCM_FRAME_SAMPLES) ? samples - base : DSPADPCM_FRAME_SAMPLES);
        double   best  = -1.0;
        int      best_pair = 0, best_scale = 0;
        int8_t   best_nib[14];
        int16_t  best_h1 = h1, best_h2 = h2;
        int      pair, scale, i;
        uint8_t* frame = out + f * DSPADPCM_FRAME_BYTES;

        for (pair = 0; pair < 8; pair++) {
            for (scale = 0; scale < 16; scale++) {
                int8_t  nib[14];
                int16_t nh1, nh2;
                double  err = try_frame(pcm + base, count, pair, scale, coef, h1, h2, nib, &nh1, &nh2);
                if (best < 0.0 || err < best) {
                    best       = err;
                    best_pair  = pair;
                    best_scale = scale;
                    best_h1    = nh1;
                    best_h2    = nh2;
                    memcpy(best_nib, nib, sizeof(nib));
                }
            }
        }
        frame[0] = (uint8_t) ((best_pair << 4) | best_scale);
        for (i = 0; i < 7; i++) {
            frame[1 + i] = (uint8_t) (((best_nib[2 * i] & 0xF) << 4) | (best_nib[2 * i + 1] & 0xF));
        }
        if (recon) {
            /* replay the chosen frame to produce the reconstruction */
            int32_t c1 = coef[best_pair * 2], c2 = coef[best_pair * 2 + 1];
            int16_t r1 = h1, r2 = h2;
            for (i = 0; i < 14; i++) {
                int16_t r = recon_step(best_nib[i], best_scale, c1, c2, r1, r2);
                if ((uint32_t) i < (uint32_t) count) {
                    recon[base + i] = r;
                }
                r2 = r1;
                r1 = r;
            }
        }
        h1 = best_h1;
        h2 = best_h2;
    }
    return frames * DSPADPCM_FRAME_BYTES;
}

void dspadpcm_ref_loop_ctx(const uint8_t* frames, const int16_t* recon, uint32_t loop_start, DspAdpcmLoopCtx* ctx)
{
    ctx->pred_scale = frames[(loop_start / DSPADPCM_FRAME_SAMPLES) * DSPADPCM_FRAME_BYTES];
    ctx->yn1        = loop_start >= 1u ? recon[loop_start - 1u] : 0;
    ctx->yn2        = loop_start >= 2u ? recon[loop_start - 2u] : 0;
}
