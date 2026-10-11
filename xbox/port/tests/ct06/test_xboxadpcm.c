/* test_xboxadpcm.c — CT-06: Xbox ADPCM (0x0069) encoder/decoder round trip and block layout. */
#include "ct06_test.h"
#include "../../snd/xboxadpcm.h"

#include <string.h>

#define RATE    32000.0
#define SAMPLES 32000u

static int16_t pcm[SAMPLES * 2];
static int16_t recon[(SAMPLES + 64) * 2];
static int16_t dec[(SAMPLES + 64) * 2];
static uint8_t enc[(SAMPLES / 64 + 1) * 72];

static void test_sizes(void)
{
    CHECK_EQ_U32(XBOXADPCM_FORMAT_TAG, 0x69u);
    CHECK_EQ_U32(xboxadpcm_block_bytes(1), 36u);
    CHECK_EQ_U32(xboxadpcm_block_bytes(2), 72u);
    CHECK_EQ_U32(xboxadpcm_blocks_for_samples(0), 0u);
    CHECK_EQ_U32(xboxadpcm_blocks_for_samples(1), 1u);
    CHECK_EQ_U32(xboxadpcm_blocks_for_samples(64), 1u);
    CHECK_EQ_U32(xboxadpcm_blocks_for_samples(65), 2u);
    CHECK_EQ_U32(xboxadpcm_encoded_size(32000, 1), 500u * 36u);
    CHECK_EQ_U32(xboxadpcm_encoded_size(32000, 2), 500u * 72u);
    CHECK(xboxadpcm_step_table[0] == 7);
    CHECK(xboxadpcm_step_table[88] == 32767);
    CHECK(xboxadpcm_index_table[7] == 8 && xboxadpcm_index_table[15] == 8 && xboxadpcm_index_table[0] == -1);
}

/* Hand-built stereo block: checks header fields, verbatim sample 0, nibble positions and order. */
static void test_layout(void)
{
    uint8_t        blk[72];
    int16_t        out[128];
    XboxAdpcmState s;
    int            i;

    memset(blk, 0, sizeof(blk));
    /* L header: predictor 0x1234, step index 5 ; R header: predictor -2 (0xFFFE), step index 88 */
    blk[0] = 0x34; blk[1] = 0x12; blk[2] = 5;  blk[3] = 0;
    blk[4] = 0xFE; blk[5] = 0xFF; blk[6] = 88; blk[7] = 0;
    /* L nibble 0 (byte 8, low nibble) = 7 (+ step 12: 12>>3 + 12 + 6 + 3 = 22) */
    blk[8] |= 0x07;
    /* L nibble 1 (byte 8, high nibble) = 8 (-(step>>3)); step index after nibble 7 is 5+8 = 13 -> step 25 -> diff 3 */
    blk[8] |= 0x80;
    /* R nibble 8 = word 1 (bytes 16..23), R half = bytes 20..23, low nibble of byte 20 = 0xF */
    blk[20] |= 0x0F;

    xboxadpcm_init(&s, 2);
    xboxadpcm_decode_block(&s, blk, out);
    CHECK(out[0] == 0x1234);                    /* L sample 0 = header predictor verbatim */
    CHECK(out[1] == -2);                        /* R sample 0 */
    CHECK(out[2] == 0x1234 + 22);               /* L sample 1 */
    CHECK(out[4] == 0x1234 + 22 - 3);           /* L sample 2 */
    /* R: steps 1..8 decode nibble 0 (step 32767 at idx 88: diff = 4095, index -> 87) ... all zeros
     * until nibble 8 = 0xF at sample 9. Just check the sample-9 jump is negative and large. */
    CHECK(out[2 * 9 + 1] < out[2 * 8 + 1] - 10000);
    for (i = 3; i < 64; i++) {
        /* L nibbles 2..62 are 0: each adds step>>3 (positive, small) */
        CHECK(out[2 * i] >= out[2 * (i - 1)]);
    }
    /* mono layout: nibble k at byte 4 + k/2, low nibble first */
    memset(blk, 0, sizeof(blk));
    blk[0] = 100; blk[1] = 0; blk[2] = 0;
    blk[4] = 0x07;              /* nibble 0 = 7: step 7 -> 0 + 7 + 3 + 1 = 11 */
    xboxadpcm_init(&s, 1);
    xboxadpcm_decode_block(&s, blk, out);
    CHECK(out[0] == 100);
    CHECK(out[1] == 111);
    /* nibble 63 (byte 35 high nibble) must be ignored: setting it changes nothing */
    {
        int16_t out2[64];
        blk[35] |= 0xF0;
        xboxadpcm_decode_block(&s, blk, out2);
        CHECK(memcmp(out, out2, sizeof(out2)) == 0);
    }
}

static void test_mono_roundtrip(void)
{
    uint32_t nbytes, blocks;
    double   snr;

    ct06_sine(pcm, SAMPLES, 1000.0, RATE, 20000.0);
    nbytes = xboxadpcm_encode(pcm, SAMPLES, 1, enc, recon);
    blocks = nbytes / 36u;
    CHECK_EQ_U32(nbytes, xboxadpcm_encoded_size(SAMPLES, 1));
    CHECK_EQ_U32(xboxadpcm_decode(enc, blocks, 1, dec), blocks * 64u);
    CHECK(memcmp(dec, recon, SAMPLES * sizeof(int16_t)) == 0);     /* decoder == encoder's model */
    snr = ct06_snr_db(pcm, dec, SAMPLES);
    printf("  mono 1 kHz @20000: SNR %.1f dB\n", snr);
    CHECK(snr > 30.0);
    /* every block header carries the first sample verbatim and a padding nibble of 0 */
    {
        uint32_t b, ok = 1;
        for (b = 0; b < blocks; b++) {
            int16_t hdr = (int16_t) (enc[b * 36u] | (enc[b * 36u + 1] << 8));
            if (b * 64u < SAMPLES && hdr != pcm[b * 64u]) ok = 0;
            if (enc[b * 36u + 3] != 0) ok = 0;
            if ((enc[b * 36u + 35] & 0xF0) != 0) ok = 0;
        }
        CHECK(ok);
    }
    /* full scale and a low tone */
    ct06_sine(pcm, SAMPLES, 1000.0, RATE, 32767.0);
    nbytes = xboxadpcm_encode(pcm, SAMPLES, 1, enc, NULL);
    xboxadpcm_decode(enc, nbytes / 36u, 1, dec);
    snr = ct06_snr_db(pcm, dec, SAMPLES);
    printf("  mono 1 kHz @32767: SNR %.1f dB\n", snr);
    CHECK(snr > 25.0);
    ct06_sine(pcm, SAMPLES, 110.0, RATE, 20000.0);
    nbytes = xboxadpcm_encode(pcm, SAMPLES, 1, enc, NULL);
    xboxadpcm_decode(enc, nbytes / 36u, 1, dec);
    snr = ct06_snr_db(pcm, dec, SAMPLES);
    printf("  mono 110 Hz @20000: SNR %.1f dB\n", snr);
    CHECK(snr > 30.0);
}

static void test_stereo_roundtrip(void)
{
    static int16_t left[SAMPLES], right[SAMPLES], monoL[SAMPLES + 64], monoR[SAMPLES + 64];
    static int16_t decL[SAMPLES], decR[SAMPLES];
    uint32_t nbytes, blocks, i;
    double   snr;

    ct06_sine(left, SAMPLES, 1000.0, RATE, 20000.0);
    ct06_sine(right, SAMPLES, 440.0, RATE, 12000.0);
    for (i = 0; i < SAMPLES; i++) {
        pcm[2 * i]     = left[i];
        pcm[2 * i + 1] = right[i];
    }
    nbytes = xboxadpcm_encode(pcm, SAMPLES, 2, enc, recon);
    blocks = nbytes / 72u;
    CHECK_EQ_U32(nbytes, xboxadpcm_encoded_size(SAMPLES, 2));
    xboxadpcm_decode(enc, blocks, 2, dec);
    CHECK(memcmp(dec, recon, SAMPLES * 2u * sizeof(int16_t)) == 0);
    for (i = 0; i < SAMPLES; i++) {
        decL[i] = dec[2 * i];
        decR[i] = dec[2 * i + 1];
    }
    snr = ct06_snr_db(left, decL, SAMPLES);
    printf("  stereo L: SNR %.1f dB\n", snr);
    CHECK(snr > 30.0);
    snr = ct06_snr_db(right, decR, SAMPLES);
    printf("  stereo R: SNR %.1f dB\n", snr);
    CHECK(snr > 30.0);
    /* channels are independent: a mono encode of each channel is bit-identical */
    {
        static uint8_t encL[(SAMPLES / 64 + 1) * 36];
        uint32_t nb = xboxadpcm_encode(left, SAMPLES, 1, encL, NULL);
        xboxadpcm_decode(encL, nb / 36u, 1, monoL);
        CHECK(memcmp(monoL, decL, SAMPLES * sizeof(int16_t)) == 0);
        nb = xboxadpcm_encode(right, SAMPLES, 1, encL, NULL);
        xboxadpcm_decode(encL, nb / 36u, 1, monoR);
        CHECK(memcmp(monoR, decR, SAMPLES * sizeof(int16_t)) == 0);
        /* and the stereo stream's L header/nibbles are the mono stream's, at the interleaved spots */
        {
            uint32_t b, w, ok = 1;
            for (b = 0; b < 8u; b++) {
                if (memcmp(enc + b * 72u + 4u, encL + b * 36u, 4u) != 0) ok = 0; /* R headers */
                for (w = 0; w < 8u; w++) {
                    if (memcmp(enc + b * 72u + 8u + w * 8u + 4u, encL + b * 36u + 4u + w * 4u, 4u) != 0) ok = 0;
                }
            }
            CHECK(ok);
        }
    }
}

static void test_partial_block(void)
{
    uint32_t nbytes;
    ct06_sine(pcm, 100, 1000.0, RATE, 20000.0);
    nbytes = xboxadpcm_encode(pcm, 100, 1, enc, NULL);
    CHECK_EQ_U32(nbytes, 72u);
    xboxadpcm_decode(enc, 2, 1, dec);
    CHECK(ct06_snr_db(pcm, dec, 100) > 25.0);
    /* the padding holds the last sample: the tail converges to it */
    CHECK(abs((int) dec[127] - (int) pcm[99]) < 1000);
    /* zero samples: nothing written */
    CHECK_EQ_U32(xboxadpcm_encode(pcm, 0, 1, enc, NULL), 0u);
}

int main(void)
{
    printf("ct06_xbox: Xbox ADPCM tests\n");
    test_sizes();
    test_layout();
    test_mono_roundtrip();
    test_stereo_roundtrip();
    test_partial_block();
    return ct06_finish("ct06_xbox");
}
