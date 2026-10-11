/* dspadpcm_ref_enc.h — brute-force reference DSP-ADPCM encoder for the CT-06 tests.
 *
 * Not for production: it tries every coefficient pair (8) and every scale (16) per 14-sample frame,
 * quantising greedily and reconstructing with exactly the decoder's arithmetic (dspadpcm.c), and
 * keeps the (pair, scale) with the least squared error. Coefficients are supplied by the caller
 * (AX a[8][2] order, Q11); no LPC analysis is done.
 */
#ifndef CT06_DSPADPCM_REF_ENC_H
#define CT06_DSPADPCM_REF_ENC_H

#include <stdint.h>
#include "../../snd/dspadpcm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encode `samples` samples into whole frames at `out` (dspadpcm_samples_to_bytes(samples) bytes;
 * a partial last frame is padded with zeros). `recon` (optional, `samples` entries) receives the
 * decoder's reconstruction. Returns the number of bytes written. */
uint32_t dspadpcm_ref_encode(const int16_t* pcm, uint32_t samples, const int16_t coef[16], uint8_t* out, int16_t* recon);

/* AXPBADPCMLOOP for a loop starting at sample `loop_start` of a buffer encoded by
 * dspadpcm_ref_encode(): pred_scale = header of the loop frame, yn1/yn2 = the two reconstructed
 * samples before the loop start (0 when there are none). This is what the SDK's DSPADPCM tool
 * stores in WTADPCM.loop_* / SND_SHD.lps/lyn1/lyn2. */
void dspadpcm_ref_loop_ctx(const uint8_t* frames, const int16_t* recon, uint32_t loop_start, DspAdpcmLoopCtx* ctx);

/* A coefficient table that suits low-frequency test tones (pair 3 = 2cos(w) ~ 2.0, -1.0). */
extern const int16_t dspadpcm_ref_test_coef[16];

#ifdef __cplusplus
}
#endif

#endif /* CT06_DSPADPCM_REF_ENC_H */
