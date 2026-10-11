/* dspadpcm.c — GameCube DSP-ADPCM software decoder (see dspadpcm.h for the format and the
 * nibble-address conventions).
 *
 * The decode models the DSP accelerator the way Dolphin's DSP/DSPAccelerator does it:
 *   - when the current nibble address is at a frame boundary (addr & 15 == 0) the header byte is
 *     read from memory into pred_scale and the address skips to the first sample nibble (+2);
 *   - each sample consumes one nibble (high nibble first);
 *   - the voice end test happens right after the increment: a voice ends/loops once
 *     addr == endAddress + 1, so the sample at endAddress is the last one played (inclusive end);
 *   - on a loop the address becomes loopAddress and pred_scale/yn1/yn2 are reloaded from
 *     AXPBADPCMLOOP (the game sets these from WTADPCM.loop_* / SND_SHD.lps/lyn1/lyn2, and to
 *     {first header byte, 0, 0} for the stream ring wrap, src/game/snd_str2.cpp:277-290).
 *
 * Game ground truth for the addresses (all nibble addresses, see the header):
 *   src/game/snd_iss3.cpp:270-284  cur = pcm_adrs*2 + offset + 2; end = cur + n/14*16 + n%14
 *   src/game/snd_str3.cpp:17-60    start = ring*2 + 2; loop = ring*2 + lptop_nbl; end = ring*2 + lpend_nbl
 *   src/game/snd_str2.cpp:277-287  ring wrap: loop = ring*2 + 2, end = ring*2 + 0x3FFFF (last nibble)
 *
 * The sample formula uses a 64-bit intermediate: coef*yn1 + coef*yn2 can exceed 2^31 for extreme
 * (but legal) coefficient tables, and the DSP accumulates in 40 bits.
 */
#include "dspadpcm.h"

#include <string.h>

/* ---- primitives ----------------------------------------------------------------------------- */

static inline int16_t dsp_clamp16(int64_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t) v;
}

/* Nibble value (0..15) at nibble address `addr`: even addresses are the high nibble. */
static inline int dsp_nibble_at(const uint8_t* base, uint32_t addr)
{
    uint8_t b = base[addr >> 1];
    return (addr & 1u) ? (b & 0xF) : (b >> 4);
}

/* Decode one nibble with the current frame header and history, advancing the history. */
static inline int16_t dsp_step(DspAdpcmState* s, int nibble)
{
    int      n     = (nibble >= 8) ? nibble - 16 : nibble;      /* sign-extend 4 bits */
    int      scale = s->pred_scale & 0xF;                        /* low nibble: scale exponent */
    int      idx   = (s->pred_scale >> 4) & 0x7;                 /* high nibble: coef pair 0..7 */
    int64_t  acc   = ((int64_t) n << scale) * 2048;              /* (n << scale) << 11 */
    int16_t  out;

    acc += 1024;
    acc += (int64_t) s->coef[idx * 2] * s->yn1;
    acc += (int64_t) s->coef[idx * 2 + 1] * s->yn2;
    out = dsp_clamp16(acc >> 11);
    s->yn2 = s->yn1;
    s->yn1 = out;
    return out;
}

/* Header byte of the frame containing nibble address `addr`. */
static inline uint8_t dsp_header_at(const uint8_t* base, uint32_t addr)
{
    return base[(addr / DSPADPCM_FRAME_NIBBLES) * DSPADPCM_FRAME_BYTES];
}

/* ---- public API ----------------------------------------------------------------------------- */

void dspadpcm_init(DspAdpcmState* s, const uint16_t coef[16], uint16_t pred_scale, uint16_t yn1, uint16_t yn2)
{
    int i;
    for (i = 0; i < 16; i++) {
        s->coef[i] = (int16_t) coef[i];
    }
    s->pred_scale = (uint8_t) pred_scale;
    s->yn1        = (int16_t) yn1;
    s->yn2        = (int16_t) yn2;
}

void dspadpcm_decode_frame(DspAdpcmState* s, const uint8_t frame[8], int16_t out[14])
{
    int i;
    s->pred_scale = frame[0];
    for (i = 0; i < (int) DSPADPCM_FRAME_SAMPLES; i++) {
        uint8_t b = frame[1 + (i >> 1)];
        out[i] = dsp_step(s, (i & 1) ? (b & 0xF) : (b >> 4));
    }
}

void dspadpcm_seek(DspAdpcmState* s, const uint8_t* base, uint32_t nibble_addr)
{
    s->pred_scale = dsp_header_at(base, nibble_addr);
}

uint32_t dspadpcm_decode(DspAdpcmState* s, const uint8_t* base, uint32_t nibble_addr, uint32_t count, int16_t* out)
{
    uint32_t addr = nibble_addr;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if ((addr % DSPADPCM_FRAME_NIBBLES) < 2u) {
            /* frame boundary: load the header, skip to the first sample nibble */
            addr = addr - addr % DSPADPCM_FRAME_NIBBLES;
            s->pred_scale = dsp_header_at(base, addr);
            addr += 2u;
        }
        out[i] = dsp_step(s, dsp_nibble_at(base, addr));
        addr++;
    }
    /* Not consuming a trailing header on purpose: when the last frame was finished, `addr` is the
     * next frame's header nibble (16k), which may lie past the end of the caller's buffer. Passing
     * it back to dspadpcm_decode() loads that header first, so chunked decoding stays exact. */
    return addr;
}

void dspadpcm_voice_init(DspAdpcmVoice* v, const uint8_t* base, const uint16_t coef[16],
                         uint16_t pred_scale, uint16_t yn1, uint16_t yn2,
                         uint32_t cur, uint32_t loop, uint32_t end, int loop_flag,
                         const DspAdpcmLoopCtx* loop_ctx)
{
    memset(v, 0, sizeof(*v));
    dspadpcm_init(&v->st, coef, pred_scale, yn1, yn2);
    v->base      = base;
    v->cur       = cur;
    v->loop      = loop;
    v->end       = end;
    v->loop_flag = loop_flag;
    if (loop_ctx != NULL) {
        v->loop_ctx = *loop_ctx;
    } else {
        /* no context supplied: a wrap re-reads the loop frame's header, history continues */
        v->loop_ctx.pred_scale = dsp_header_at(base, loop);
        v->loop_ctx.yn1        = 0;
        v->loop_ctx.yn2        = 0;
    }
    v->ended = 0;
}

uint32_t dspadpcm_voice_run(DspAdpcmVoice* v, uint32_t count, int16_t* out)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (v->ended) {
            break;
        }
        if ((v->cur % DSPADPCM_FRAME_NIBBLES) < 2u) {
            v->cur = v->cur - v->cur % DSPADPCM_FRAME_NIBBLES;
            v->st.pred_scale = dsp_header_at(v->base, v->cur);
            v->cur += 2u;
        }
        out[i] = dsp_step(&v->st, dsp_nibble_at(v->base, v->cur));
        v->cur++;
        /* inclusive end: the sample at `end` was just played */
        if (v->cur == v->end + 1u) {
            if (v->loop_flag) {
                v->cur           = v->loop;
                v->st.pred_scale = v->loop_ctx.pred_scale;
                v->st.yn1        = v->loop_ctx.yn1;
                v->st.yn2        = v->loop_ctx.yn2;
            } else {
                v->ended = 1;
            }
        }
    }
    if (i < count) {
        memset(out + i, 0, (count - i) * sizeof(int16_t));
    }
    return i;
}
