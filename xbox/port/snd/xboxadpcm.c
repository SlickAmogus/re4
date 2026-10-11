/* xboxadpcm.c — Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x0069) encoder/decoder. Format in xboxadpcm.h. */
#include "xboxadpcm.h"

#include <string.h>

const int16_t xboxadpcm_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

const int8_t xboxadpcm_index_table[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int32_t ima_clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

static inline int32_t ima_clamp_index(int32_t i)
{
    if (i < 0) return 0;
    if (i > 88) return 88;
    return i;
}

/* Reference bitwise IMA expansion of nibble `n` (0..15). */
static inline int16_t ima_expand(XboxAdpcmChannel* c, int n)
{
    int32_t step = xboxadpcm_step_table[c->step_index];
    int32_t diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    if (n & 8) {
        c->predictor -= diff;
    } else {
        c->predictor += diff;
    }
    c->predictor  = ima_clamp16(c->predictor);
    c->step_index = ima_clamp_index(c->step_index + xboxadpcm_index_table[n]);
    return (int16_t) c->predictor;
}

/* Reference IMA quantisation of `sample` against the channel state; advances the state exactly as
 * ima_expand() would for the returned nibble. */
static inline int ima_compress(XboxAdpcmChannel* c, int32_t sample)
{
    int32_t step  = xboxadpcm_step_table[c->step_index];
    int32_t delta = sample - c->predictor;
    int32_t diff  = step >> 3;
    int32_t tmp   = step;
    int     n     = 0;
    int     mask  = 4;
    int     i;

    if (delta < 0) {
        n     = 8;
        delta = -delta;
    }
    for (i = 0; i < 3; i++) {
        if (delta >= tmp) {
            n     |= mask;
            delta -= tmp;
            diff  += tmp;
        }
        tmp  >>= 1;
        mask >>= 1;
    }
    if (n & 8) {
        c->predictor -= diff;
    } else {
        c->predictor += diff;
    }
    c->predictor  = ima_clamp16(c->predictor);
    c->step_index = ima_clamp_index(c->step_index + xboxadpcm_index_table[n]);
    return n;
}

/* Byte offset of nibble k (0..63) of channel `ch` inside a block of `channels` channels. */
static inline uint32_t ima_nibble_byte(uint32_t channels, uint32_t ch, uint32_t k)
{
    uint32_t word = k / 8u;          /* 8 nibbles per 4-byte word */
    uint32_t byte = (k % 8u) / 2u;
    return XBOXADPCM_HEADER_BYTES_CH * channels + (word * channels + ch) * 4u + byte;
}

void xboxadpcm_init(XboxAdpcmState* s, uint32_t channels)
{
    memset(s, 0, sizeof(*s));
    s->channels = (channels == 2u) ? 2u : 1u;
}

/* Squared reconstruction error of block `pcm` when encoded from step index `idx` (channel `ch`). */
static uint64_t ima_block_error(const int16_t* pcm, uint32_t nch, uint32_t ch, int32_t idx)
{
    XboxAdpcmChannel c;
    uint64_t         err = 0;
    uint32_t         k;

    c.predictor  = pcm[ch];
    c.step_index = idx;
    for (k = 1; k < XBOXADPCM_SAMPLES_PER_BLOCK; k++) {
        int32_t d;
        (void) ima_compress(&c, pcm[k * nch + ch]);
        d = pcm[k * nch + ch] - c.predictor;
        err += (uint64_t) ((int64_t) d * d);
    }
    return err;
}

void xboxadpcm_choose_step_index(XboxAdpcmState* s, const int16_t* pcm)
{
    uint32_t ch;
    for (ch = 0; ch < s->channels; ch++) {
        int32_t  best = 0, i;
        uint64_t best_err = UINT64_MAX;
        for (i = 0; i <= 88; i++) {
            uint64_t e = ima_block_error(pcm, s->channels, ch, i);
            if (e < best_err) {
                best_err = e;
                best     = i;
            }
        }
        s->ch[ch].step_index = best;
    }
}

void xboxadpcm_encode_block(XboxAdpcmState* s, const int16_t* pcm, uint8_t* out, int16_t* recon)
{
    uint32_t nch = s->channels;
    uint32_t ch, k;

    memset(out, 0, xboxadpcm_block_bytes(nch));
    for (ch = 0; ch < nch; ch++) {
        XboxAdpcmChannel* c = &s->ch[ch];
        int32_t first = pcm[ch];
        c->predictor = first;
        out[ch * 4 + 0] = (uint8_t) (first & 0xFF);
        out[ch * 4 + 1] = (uint8_t) ((first >> 8) & 0xFF);
        out[ch * 4 + 2] = (uint8_t) c->step_index;
        out[ch * 4 + 3] = 0;
        if (recon) {
            recon[ch] = (int16_t) first;
        }
        for (k = 0; k < XBOXADPCM_SAMPLES_PER_BLOCK - 1u; k++) {
            int      n   = ima_compress(c, pcm[(k + 1u) * nch + ch]);
            uint32_t off = ima_nibble_byte(nch, ch, k);
            out[off] |= (uint8_t) ((k & 1u) ? (n << 4) : n);   /* low nibble first */
            if (recon) {
                recon[(k + 1u) * nch + ch] = (int16_t) c->predictor;
            }
        }
        /* nibble 63 stays 0 (padding) */
    }
}

void xboxadpcm_decode_block(XboxAdpcmState* s, const uint8_t* in, int16_t* pcm)
{
    uint32_t nch = s->channels;
    uint32_t ch, k;

    for (ch = 0; ch < nch; ch++) {
        XboxAdpcmChannel* c = &s->ch[ch];
        c->predictor  = (int16_t) (in[ch * 4 + 0] | (in[ch * 4 + 1] << 8));
        c->step_index = ima_clamp_index(in[ch * 4 + 2]);
        pcm[ch] = (int16_t) c->predictor;
        for (k = 0; k < XBOXADPCM_SAMPLES_PER_BLOCK - 1u; k++) {
            uint8_t b = in[ima_nibble_byte(nch, ch, k)];
            int     n = (k & 1u) ? (b >> 4) : (b & 0xF);
            pcm[(k + 1u) * nch + ch] = ima_expand(c, n);
        }
    }
}

uint32_t xboxadpcm_encode(const int16_t* pcm, uint32_t samples, uint32_t channels, uint8_t* out, int16_t* recon)
{
    XboxAdpcmState s;
    uint32_t       blocks = xboxadpcm_blocks_for_samples(samples);
    uint32_t       nch    = (channels == 2u) ? 2u : 1u;
    uint32_t       bsz    = xboxadpcm_block_bytes(nch);
    uint32_t       b, i, ch;
    int16_t        tmp[XBOXADPCM_SAMPLES_PER_BLOCK * XBOXADPCM_MAX_CHANNELS];

    xboxadpcm_init(&s, nch);
    for (b = 0; b < blocks; b++) {
        uint32_t       base = b * XBOXADPCM_SAMPLES_PER_BLOCK;
        const int16_t* src  = pcm + (size_t) base * nch;
        if (base + XBOXADPCM_SAMPLES_PER_BLOCK > samples) {
            /* partial last block: pad by holding the last sample */
            uint32_t have = samples - base;
            for (i = 0; i < XBOXADPCM_SAMPLES_PER_BLOCK; i++) {
                uint32_t j = (i < have) ? i : (have ? have - 1u : 0u);
                for (ch = 0; ch < nch; ch++) {
                    tmp[i * nch + ch] = have ? src[j * nch + ch] : 0;
                }
            }
            src = tmp;
        }
        if (b == 0u) {
            /* No history yet: pick the start step index instead of ramping up from 0 (the header
             * stores it, so any index is legal). Later blocks carry the adapted index over. */
            xboxadpcm_choose_step_index(&s, src);
        }
        xboxadpcm_encode_block(&s, src, out + (size_t) b * bsz,
                               recon ? recon + (size_t) base * nch : NULL);
    }
    return blocks * bsz;
}

uint32_t xboxadpcm_decode(const uint8_t* in, uint32_t blocks, uint32_t channels, int16_t* pcm)
{
    XboxAdpcmState s;
    uint32_t       nch = (channels == 2u) ? 2u : 1u;
    uint32_t       bsz = xboxadpcm_block_bytes(nch);
    uint32_t       b;

    xboxadpcm_init(&s, nch);
    for (b = 0; b < blocks; b++) {
        xboxadpcm_decode_block(&s, in + (size_t) b * bsz, pcm + (size_t) b * XBOXADPCM_SAMPLES_PER_BLOCK * nch);
    }
    return blocks * XBOXADPCM_SAMPLES_PER_BLOCK;
}
