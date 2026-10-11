/* dspadpcm.h — GameCube DSP-ADPCM (AX "ADPCM" voice format) software decoder.
 *
 * Format (include/dolphin/ax.h AXPBADPCM, include/dolphin/syn.h WTADPCM, include/snd_drv.h SND_SHD):
 *   - 8-byte frames: 1 header byte + 7 bytes = 14 nibbles = 14 samples, high nibble first.
 *   - header: low nibble = scale exponent (scale = 1 << (h & 0xF)), high nibble = coefficient pair
 *     index 0..7 into coef[16] (a[8][2]: coef[2*i] applies to yn1, coef[2*i+1] to yn2, Q11).
 *   - sample = clamp16(((n << 11 << scale) + 1024 + coef[2i]*yn1 + coef[2i+1]*yn2) >> 11),
 *     n = sign-extended 4-bit nibble; yn2 = yn1; yn1 = sample.
 *
 * Nibble addressing (the game's convention, src/game/snd_iss3.cpp iss_ax_set_para and
 * src/game/snd_str*.cpp): nibble address N is byte N/2, nibble N&1 (high nibble = even address).
 * Frame headers occupy nibbles 16k and 16k+1 of a frame-aligned buffer; "sample addresses" skip
 * them: the first sample of a buffer is at nibble 2, sample s at s/14*16 + s%14 + 2. AX end
 * addresses are inclusive (the sample at endAddress is played, then the voice jumps to
 * loopAddress). On a loop the DSP reloads pred_scale/yn1/yn2 from AXPBADPCMLOOP.
 *
 * Portable C99, no pointer-size assumptions, no allocation. All values are host-endian: the
 * caller byte-swaps the big-endian GameCube tables (u16 coef/yn/ps) before calling.
 */
#ifndef RE4PORT_SND_DSPADPCM_H
#define RE4PORT_SND_DSPADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSPADPCM_FRAME_BYTES   8u
#define DSPADPCM_FRAME_NIBBLES 16u
#define DSPADPCM_FRAME_SAMPLES 14u

/* Decoder state == the AX voice's ADPCM context (AXPBADPCM minus gain). */
typedef struct DspAdpcmState {
    int16_t  coef[16];     /* a[8][2] flattened: coef[2*i] * yn1 + coef[2*i+1] * yn2 */
    int16_t  yn1;          /* previous sample */
    int16_t  yn2;          /* sample before that */
    uint8_t  pred_scale;   /* header byte of the frame being decoded (AXPBADPCM.pred_scale) */
} DspAdpcmState;

/* AXPBADPCMLOOP: context restored when the voice wraps from endAddress to loopAddress. */
typedef struct DspAdpcmLoopCtx {
    uint8_t  pred_scale;
    int16_t  yn1;
    int16_t  yn2;
} DspAdpcmLoopCtx;

/* A software AX voice: plays from `cur`, wraps after the (inclusive) `end` nibble to `loop` when
 * loop_flag != 0, otherwise stops (ended = 1) and dspadpcm_voice_run() pads with zeros. */
typedef struct DspAdpcmVoice {
    DspAdpcmState   st;
    const uint8_t*  base;      /* nibble address 0 */
    uint32_t        cur;       /* AXPBADDR.currentAddress (sample address) */
    uint32_t        loop;      /* AXPBADDR.loopAddress */
    uint32_t        end;       /* AXPBADDR.endAddress, inclusive */
    int             loop_flag; /* AXPBADDR.loopFlag */
    DspAdpcmLoopCtx loop_ctx;  /* AXPBADPCMLOOP */
    int             ended;
} DspAdpcmVoice;

/* ---- nibble address arithmetic (matches the game's conversions) ------------------------------ */

/* ARAM byte address -> nibble address (snd_str0: ar_buff_nbl = ar_buff_adrs * 2; iss3: pcm_adrs*2). */
static inline uint32_t dspadpcm_bytes_to_nibbles(uint32_t bytes) { return bytes * 2u; }
/* Nibble count -> bytes (snd_str0: read_end = nibbles / 2; snd_str2: (lptop_nbl >> 1)). */
static inline uint32_t dspadpcm_nibbles_to_bytes(uint32_t nibbles) { return nibbles / 2u; }
/* Sample index -> nibble offset from a frame-aligned base (iss3: cur = base + 2;
 * addr = cur + s/14*16 + s%14). The returned offset already includes the +2. */
static inline uint32_t dspadpcm_sample_to_nibble(uint32_t sample)
{
    return sample / DSPADPCM_FRAME_SAMPLES * DSPADPCM_FRAME_NIBBLES + sample % DSPADPCM_FRAME_SAMPLES + 2u;
}
/* Inverse of dspadpcm_sample_to_nibble; a header nibble maps to its frame's first sample. */
static inline uint32_t dspadpcm_nibble_to_sample(uint32_t nibble)
{
    uint32_t in_frame = nibble % DSPADPCM_FRAME_NIBBLES;
    return nibble / DSPADPCM_FRAME_NIBBLES * DSPADPCM_FRAME_SAMPLES + (in_frame < 2u ? 0u : in_frame - 2u);
}
/* Nibble address -> address of the first sample nibble at or after it (skips a frame header). */
static inline uint32_t dspadpcm_skip_header(uint32_t nibble)
{
    return (nibble % DSPADPCM_FRAME_NIBBLES) < 2u ? (nibble - nibble % DSPADPCM_FRAME_NIBBLES) + 2u : nibble;
}
/* Number of bytes holding `samples` samples (whole frames). */
static inline uint32_t dspadpcm_samples_to_bytes(uint32_t samples)
{
    return (samples + DSPADPCM_FRAME_SAMPLES - 1u) / DSPADPCM_FRAME_SAMPLES * DSPADPCM_FRAME_BYTES;
}
/* Number of samples in `bytes` bytes of whole frames. */
static inline uint32_t dspadpcm_bytes_to_samples(uint32_t bytes)
{
    return bytes / DSPADPCM_FRAME_BYTES * DSPADPCM_FRAME_SAMPLES;
}
/* AX inclusive end address of the last of `samples` samples from a frame-aligned base (0 samples -> 2). */
static inline uint32_t dspadpcm_end_nibble(uint32_t samples)
{
    return samples == 0u ? 2u : dspadpcm_sample_to_nibble(samples - 1u);
}

/* ---- decoding ------------------------------------------------------------------------------- */

/* Initialise from a table (WTADPCM / SND_SHD / AXPBADPCM fields, already host-endian). */
void dspadpcm_init(DspAdpcmState* s, const uint16_t coef[16], uint16_t pred_scale, uint16_t yn1, uint16_t yn2);

/* Decode one 8-byte frame (reads its header) into 14 samples. */
void dspadpcm_decode_frame(DspAdpcmState* s, const uint8_t frame[8], int16_t out[14]);

/* Load the frame header for an arbitrary start address (use before dspadpcm_decode() when the
 * start is mid-frame and the state's pred_scale is not the one for that frame). */
void dspadpcm_seek(DspAdpcmState* s, const uint8_t* base, uint32_t nibble_addr);

/* Decode `count` samples starting at nibble address `nibble_addr` (relative to `base`). Header
 * nibbles are skipped; whenever a frame's first sample is reached the header is reloaded from
 * memory, exactly like the DSP. A mid-frame start uses s->pred_scale. Returns the sample address of
 * the next sample (never a header nibble). */
uint32_t dspadpcm_decode(DspAdpcmState* s, const uint8_t* base, uint32_t nibble_addr, uint32_t count, int16_t* out);

/* Program a software voice the way iss_ax_set_para / str_ax_voice_para_set program AX. */
void dspadpcm_voice_init(DspAdpcmVoice* v, const uint8_t* base, const uint16_t coef[16],
                         uint16_t pred_scale, uint16_t yn1, uint16_t yn2,
                         uint32_t cur, uint32_t loop, uint32_t end, int loop_flag,
                         const DspAdpcmLoopCtx* loop_ctx);

/* Produce `count` samples; returns how many were real (the rest are zero when a one-shot ended). */
uint32_t dspadpcm_voice_run(DspAdpcmVoice* v, uint32_t count, int16_t* out);

#ifdef __cplusplus
}
#endif

#endif /* RE4PORT_SND_DSPADPCM_H */
