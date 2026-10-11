/* test_dspadpcm.c — CT-06: DSP-ADPCM decoder, nibble arithmetic, voice loop model.
 *
 * Ground truth for the address tests (nibble addresses):
 *   src/game/snd_iss3.cpp:270-284   cur = pcm_adrs*2 + offset + 2; addr(s) = cur + s/14*16 + s%14
 *   src/game/snd_str0.cpp:80-95     file_size = nibbles/2; buff_size_nbl = buff_size*2
 *   src/game/snd_str0.cpp:295       ar_buff_nbl = ar_buff_adrs*2
 *   src/game/snd_str2.cpp:34        blks = (lptop_nbl >> 1) / buff_size
 *   src/game/snd_str2.cpp:277-287   ring wrap: loop = ring_nbl + 2, end = ring_nbl + 0x3FFFF/0x7FFFF
 */
#include "ct06_test.h"
#include "dspadpcm_ref_enc.h"

#include <string.h>

#define RATE    32000.0
#define SAMPLES 32000u   /* 1 second */

/* ---- 1. nibble address arithmetic ---------------------------------------------------------- */

static void test_addresses(void)
{
    uint32_t s, k;

    /* ARAM byte address <-> nibble address (snd_str0:295, snd_iss3:270) */
    CHECK_EQ_U32(dspadpcm_bytes_to_nibbles(0x700000u), 0xE00000u);
    CHECK_EQ_U32(dspadpcm_nibbles_to_bytes(0xE00000u), 0x700000u);
    /* read_end = nibbles / 2 (snd_str0:84), odd nibble counts round down like the game's u32 divide */
    CHECK_EQ_U32(dspadpcm_nibbles_to_bytes(12345u), 12345u / 2u);
    /* blks = (lptop_nbl >> 1) / buff_size (snd_str2:34) */
    CHECK_EQ_U32(dspadpcm_nibbles_to_bytes(0x12340u) / 0x4000u, (0x12340u >> 1) / 0x4000u);

    /* iss_ax_set_para: addr of sample s relative to the frame-aligned sample base */
    for (s = 0; s < 2000u; s++) {
        uint32_t game = 2u + s / 14u * 16u + s % 14u;
        CHECK_EQ_U32(dspadpcm_sample_to_nibble(s), game);
        CHECK_EQ_U32(dspadpcm_nibble_to_sample(game), s);
        CHECK((game % 16u) >= 2u); /* never a header nibble */
    }
    /* header nibbles map to the frame's first sample; skip_header lands on +2 */
    for (k = 0; k < 10u; k++) {
        CHECK_EQ_U32(dspadpcm_nibble_to_sample(16u * k), 14u * k);
        CHECK_EQ_U32(dspadpcm_nibble_to_sample(16u * k + 1u), 14u * k);
        CHECK_EQ_U32(dspadpcm_skip_header(16u * k), 16u * k + 2u);
        CHECK_EQ_U32(dspadpcm_skip_header(16u * k + 1u), 16u * k + 2u);
        CHECK_EQ_U32(dspadpcm_skip_header(16u * k + 5u), 16u * k + 5u);
        CHECK_EQ_U32(dspadpcm_skip_header(16u * k + 15u), 16u * k + 15u);
    }
    /* frame <-> sample counts */
    CHECK_EQ_U32(dspadpcm_samples_to_bytes(0u), 0u);
    CHECK_EQ_U32(dspadpcm_samples_to_bytes(1u), 8u);
    CHECK_EQ_U32(dspadpcm_samples_to_bytes(14u), 8u);
    CHECK_EQ_U32(dspadpcm_samples_to_bytes(15u), 16u);
    CHECK_EQ_U32(dspadpcm_bytes_to_samples(8u), 14u);
    CHECK_EQ_U32(dspadpcm_bytes_to_samples(0x20000u), 0x20000u / 8u * 14u);
    /* inclusive end address of the last sample */
    CHECK_EQ_U32(dspadpcm_end_nibble(0u), 2u);
    CHECK_EQ_U32(dspadpcm_end_nibble(1u), 2u);
    CHECK_EQ_U32(dspadpcm_end_nibble(14u), 15u);
    CHECK_EQ_U32(dspadpcm_end_nibble(15u), 18u);
    /* stream ring: 128 KB per channel (stereo) -> last nibble 0x3FFFF, 256 KB (mono) -> 0x7FFFF;
     * both are sample nibbles (position 15), consistent with an inclusive end */
    CHECK_EQ_U32(dspadpcm_bytes_to_nibbles(0x20000u) - 1u, 0x3FFFFu);
    CHECK_EQ_U32(dspadpcm_bytes_to_nibbles(0x40000u) - 1u, 0x7FFFFu);
    CHECK_EQ_U32(0x3FFFFu % 16u, 15u);
    /* the game's one-shot end (nibble of sample index `length`) is one sample past end_nibble() */
    CHECK_EQ_U32(2u + 100u / 14u * 16u + 100u % 14u, dspadpcm_end_nibble(100u) + 1u);
}

/* ---- 2. hand-computed vector (shared with test_adpcm.py) ------------------------------------ */

/* Frame A: pair 0 = (0,0), scale 3: out = n << 3.   Frame B: pair 1 = (2048,0), scale 0: out = n + yn1. */
static const uint8_t  hand_frames[16] = { 0x03, 0x12, 0x3F, 0, 0, 0, 0, 0,
                                          0x10, 0x11, 0x1F, 0, 0, 0, 0, 0 };
static const int16_t  hand_expect[28] = { 8, 16, 24, -8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                          1, 2, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };

static void test_hand_vector(void)
{
    DspAdpcmState s;
    uint16_t      coef[16];
    int16_t       out[28];
    int           i;

    for (i = 0; i < 16; i++) coef[i] = (uint16_t) dspadpcm_ref_test_coef[i];
    dspadpcm_init(&s, coef, hand_frames[0], 0, 0);
    CHECK_EQ_U32(dspadpcm_decode(&s, hand_frames, 2u, 28u, out), 32u); /* header of frame 2, unread */
    for (i = 0; i < 28; i++) CHECK(out[i] == hand_expect[i]);
    /* frame API gives the same */
    dspadpcm_init(&s, coef, 0, 0, 0);
    dspadpcm_decode_frame(&s, hand_frames, out);
    dspadpcm_decode_frame(&s, hand_frames + 8, out + 14);
    for (i = 0; i < 28; i++) CHECK(out[i] == hand_expect[i]);
    /* starting on the header nibble (address 0) is the same as starting at 2 */
    dspadpcm_init(&s, coef, 0, 0, 0);
    dspadpcm_decode(&s, hand_frames, 0u, 28u, out);
    for (i = 0; i < 28; i++) CHECK(out[i] == hand_expect[i]);
    /* the state's pred_scale is the last header read (frame B; frame 2 past the buffer is not read) */
    CHECK(s.pred_scale == 0x10);
}

/* ---- 3. sine through the reference encoder ------------------------------------------------- */

static int16_t  pcm[SAMPLES];
static int16_t  recon[SAMPLES];
static int16_t  dec[SAMPLES + 256];
static uint8_t  frames[SAMPLES / 14 * 8 + 16];
static uint16_t coef_u16[16];

static void test_sine_roundtrip(void)
{
    DspAdpcmState s;
    uint32_t      nbytes, i, addr, pos;
    double        snr;
    static const uint32_t chunks[] = { 1, 3, 7, 13, 14, 15, 100, 160, 161, 1000 };

    ct06_sine(pcm, SAMPLES, 1000.0, RATE, 20000.0);
    nbytes = dspadpcm_ref_encode(pcm, SAMPLES, dspadpcm_ref_test_coef, frames, recon);
    CHECK_EQ_U32(nbytes, dspadpcm_samples_to_bytes(SAMPLES));
    for (i = 0; i < 16; i++) coef_u16[i] = (uint16_t) dspadpcm_ref_test_coef[i];

    /* the encoder's own reconstruction must already be > 30 dB */
    snr = ct06_snr_db(pcm, recon, SAMPLES);
    printf("  ref encoder reconstruction SNR: %.1f dB\n", snr);
    CHECK(snr > 30.0);

    /* one linear decode from the first sample address */
    dspadpcm_init(&s, coef_u16, frames[0], 0, 0);
    addr = dspadpcm_decode(&s, frames, 2u, SAMPLES, dec);
    CHECK_EQ_U32(dspadpcm_skip_header(addr), dspadpcm_sample_to_nibble(SAMPLES));
    snr = ct06_snr_db(pcm, dec, SAMPLES);
    printf("  dspadpcm_decode SNR vs sine: %.1f dB\n", snr);
    CHECK(snr > 30.0);
    CHECK(memcmp(dec, recon, SAMPLES * sizeof(int16_t)) == 0);

    /* frame by frame */
    dspadpcm_init(&s, coef_u16, 0, 0, 0);
    for (i = 0; i < SAMPLES / 14u; i++) {
        dspadpcm_decode_frame(&s, frames + i * 8u, dec + i * 14u);
    }
    CHECK(memcmp(dec, recon, (SAMPLES / 14u) * 14u * sizeof(int16_t)) == 0);

    /* chunked decode continuing from the returned address */
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        memset(dec, 0x55, sizeof(dec));
        dspadpcm_init(&s, coef_u16, frames[0], 0, 0);
        addr = 2u;
        pos  = 0;
        while (pos < SAMPLES) {
            uint32_t n = chunks[i];
            if (pos + n > SAMPLES) n = SAMPLES - pos;
            addr = dspadpcm_decode(&s, frames, addr, n, dec + pos);
            CHECK_EQ_U32(dspadpcm_skip_header(addr), dspadpcm_sample_to_nibble(pos + n));
            pos += n;
        }
        CHECK(memcmp(dec, recon, SAMPLES * sizeof(int16_t)) == 0);
    }

    /* seek: start mid-frame with the right history and header */
    for (pos = 1; pos < 200u; pos += 7u) {
        dspadpcm_init(&s, coef_u16, 0, (uint16_t) recon[pos - 1u], pos >= 2u ? (uint16_t) recon[pos - 2u] : 0u);
        dspadpcm_seek(&s, frames, dspadpcm_sample_to_nibble(pos));
        dspadpcm_decode(&s, frames, dspadpcm_sample_to_nibble(pos), 300u, dec);
        CHECK(memcmp(dec, recon + pos, 300u * sizeof(int16_t)) == 0);
    }
}

/* ---- 4. software voice: loops and one-shots ----------------------------------------------- */

static void run_voice_check(DspAdpcmVoice* v, uint32_t total, uint32_t ls, uint32_t L, uint32_t play_end)
{
    /* expected: recon[i] for i < play_end, then recon[ls + (i - play_end) % L] */
    uint32_t pos = 0;
    int      ok  = 1;
    while (pos < total) {
        uint32_t n = 160u; /* one AX audio frame */
        uint32_t i;
        if (pos + n > total) n = total - pos;
        dspadpcm_voice_run(v, n, dec);
        for (i = 0; i < n; i++) {
            uint32_t idx = pos + i;
            int16_t  exp = idx < play_end ? recon[idx] : recon[ls + (idx - play_end) % L];
            if (dec[i] != exp) {
                ok = 0;
            }
        }
        pos += n;
    }
    CHECK(ok);
    CHECK(!v->ended);
}

static void test_voice(void)
{
    DspAdpcmVoice   v;
    DspAdpcmLoopCtx ctx;
    uint32_t        ls, le, got;
    static const uint32_t loop_starts[] = { 0, 1, 2, 13, 14, 15, 47, 700 };
    uint32_t        i;

    /* (a) wavetable-style loop [ls, le): end = inclusive nibble of sample le-1 (SDK convention) */
    for (i = 0; i < sizeof(loop_starts) / sizeof(loop_starts[0]); i++) {
        ls = loop_starts[i];
        le = 1000u;
        dspadpcm_ref_loop_ctx(frames, recon, ls, &ctx);
        dspadpcm_voice_init(&v, frames, coef_u16, frames[0], 0, 0,
                            2u, dspadpcm_sample_to_nibble(ls), dspadpcm_sample_to_nibble(le - 1u), 1, &ctx);
        run_voice_check(&v, 6000u, ls, le - ls, le);
    }

    /* (b) the game's iss_ax_set_para form: end = nibble of sample (loopStart + loopLength), which
     *     with an inclusive end plays loopLength + 1 samples per pass (recon[ls..le] inclusive). */
    ls = 47u;
    le = 1000u;
    dspadpcm_ref_loop_ctx(frames, recon, ls, &ctx);
    dspadpcm_voice_init(&v, frames, coef_u16, frames[0], 0, 0,
                        2u, 2u + ls / 14u * 16u + ls % 14u, 2u + le / 14u * 16u + le % 14u, 1, &ctx);
    run_voice_check(&v, 6000u, ls, le - ls + 1u, le + 1u);

    /* (c) stream ring wrap (snd_str2:277-290): loop = base + 2, end = last nibble of the buffer,
     *     loop ctx = {first header byte, 0, 0}: output after the wrap equals a fresh decode. */
    {
        uint32_t ring_samples = 14u * 64u; /* 64 frames = 512 bytes */
        ctx.pred_scale = frames[0];
        ctx.yn1 = 0;
        ctx.yn2 = 0;
        dspadpcm_voice_init(&v, frames, coef_u16, frames[0], 0, 0,
                            2u, 2u, dspadpcm_bytes_to_nibbles(64u * 8u) - 1u, 1, &ctx);
        run_voice_check(&v, 4u * ring_samples + 37u, 0u, ring_samples, ring_samples);
    }

    /* (d) one-shot: plays exactly `n` samples, then zeros; AX end is inclusive */
    for (i = 0; i < 5u; i++) {
        uint32_t n = 14u * 20u + i * 3u + 1u;
        memset(dec, 0x55, sizeof(dec));
        dspadpcm_voice_init(&v, frames, coef_u16, frames[0], 0, 0,
                            2u, 2u, dspadpcm_end_nibble(n), 0, NULL);
        got = dspadpcm_voice_run(&v, n + 100u, dec);
        CHECK_EQ_U32(got, n);
        CHECK(v.ended);
        CHECK(memcmp(dec, recon, n * sizeof(int16_t)) == 0);
        {
            uint32_t j;
            int zeros = 1;
            for (j = n; j < n + 100u; j++) if (dec[j] != 0) zeros = 0;
            CHECK(zeros);
        }
        /* a further run yields nothing */
        CHECK_EQ_U32(dspadpcm_voice_run(&v, 10u, dec), 0u);
    }

    /* (e) voice starting mid-buffer at a non-frame-aligned sample with the game's cur formula */
    {
        uint32_t start = 333u;
        dspadpcm_init(&v.st, coef_u16, 0, 0, 0);
        dspadpcm_voice_init(&v, frames, coef_u16,
                            frames[(start / 14u) * 8u], (uint16_t) recon[start - 1u], (uint16_t) recon[start - 2u],
                            dspadpcm_sample_to_nibble(start), 2u, dspadpcm_end_nibble(SAMPLES), 0, NULL);
        got = dspadpcm_voice_run(&v, 500u, dec);
        CHECK_EQ_U32(got, 500u);
        CHECK(memcmp(dec, recon + start, 500u * sizeof(int16_t)) == 0);
    }
}

int main(void)
{
    printf("ct06_dsp: DSP-ADPCM decoder tests\n");
    test_addresses();
    test_hand_vector();
    test_sine_roundtrip();
    test_voice();
    return ct06_finish("ct06_dsp");
}
