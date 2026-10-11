/* xboxadpcm.h — Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM, 0x0069) encoder and decoder.
 *
 * This is the IMA ADPCM variant the Xbox APU (MCPX) decodes in hardware and the XDK's xbadpcm tools
 * produce (CImaAdpcmCodec, wSamplesPerBlock = 64, nBlockAlign = 36 * nChannels).
 *
 * Block layout, per channel 36 bytes carrying 64 samples:
 *   - 4-byte header: int16 LE predictor = sample 0 of the block, stored verbatim (clamped PCM);
 *                    uint8 step index (0..88) in effect for the first nibble; uint8 reserved = 0.
 *   - 32 bytes = 64 nibbles, LOW nibble first within a byte. Nibble k encodes sample k+1, so
 *     nibbles 0..62 carry samples 1..63 and nibble 63 is padding (written as 0, ignored on decode).
 *   Mono: [hdr][32 bytes]. Stereo (72 bytes/block): [hdr L][hdr R] then 4-byte words alternating
 *   L (8 nibbles), R (8 nibbles), ... i.e. byte 8 + 8*w + 4*ch + b, w = word 0..7, b = byte 0..3.
 *   This is the layout vgmstream's decode_xbox_ima reads and the XDK encoder writes; the hardware
 *   reads blocks whole, so every block is self-contained (predictor + step index) and a loop region
 *   may start on any block boundary (SetLoopRegion in whole blocks).
 *
 * IMA arithmetic: the reference bitwise form (Windows ACM imaadpcm.c / Intel-DVI):
 *   diff = step >> 3; if (n & 4) diff += step; if (n & 2) diff += step >> 1; if (n & 1) diff += step >> 2;
 *   predictor +/-= diff (clamped to int16); index += index_table[n] (clamped to 0..88).
 * The encoder reconstructs with exactly the decoder's arithmetic, so a decode of its output is
 * bit-identical to the reconstruction it tracked internally.
 *
 * Portable C99, host-endian ints in, little-endian bytes out (the file/hardware format).
 */
#ifndef RE4PORT_SND_XBOXADPCM_H
#define RE4PORT_SND_XBOXADPCM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOXADPCM_FORMAT_TAG        0x0069u
#define XBOXADPCM_SAMPLES_PER_BLOCK 64u
#define XBOXADPCM_BLOCK_BYTES_CH    36u   /* per channel; nBlockAlign = 36 * channels */
#define XBOXADPCM_HEADER_BYTES_CH   4u
#define XBOXADPCM_MAX_CHANNELS      2u

/* Per-channel IMA state. */
typedef struct XboxAdpcmChannel {
    int32_t predictor;   /* last reconstructed sample */
    int32_t step_index;  /* 0..88 */
} XboxAdpcmChannel;

typedef struct XboxAdpcmState {
    uint32_t         channels;   /* 1 or 2 */
    XboxAdpcmChannel ch[XBOXADPCM_MAX_CHANNELS];
} XboxAdpcmState;

/* IMA tables (89 steps, 16 index deltas). */
extern const int16_t xboxadpcm_step_table[89];
extern const int8_t  xboxadpcm_index_table[16];

/* Block size for `channels` channels; number of blocks needed for `samples` samples per channel. */
static inline uint32_t xboxadpcm_block_bytes(uint32_t channels) { return XBOXADPCM_BLOCK_BYTES_CH * channels; }
static inline uint32_t xboxadpcm_blocks_for_samples(uint32_t samples)
{
    return (samples + XBOXADPCM_SAMPLES_PER_BLOCK - 1u) / XBOXADPCM_SAMPLES_PER_BLOCK;
}
static inline uint32_t xboxadpcm_encoded_size(uint32_t samples, uint32_t channels)
{
    return xboxadpcm_blocks_for_samples(samples) * xboxadpcm_block_bytes(channels);
}

/* Reset the state (step index 0, predictor 0). `channels` must be 1 or 2. */
void xboxadpcm_init(XboxAdpcmState* s, uint32_t channels);

/* Encode one block: `pcm` holds 64 interleaved frames (64 * channels int16). `out` receives
 * 36 * channels bytes. If `recon` is not NULL it receives the 64 * channels reconstructed samples
 * (what a decoder will produce). The step index carries over between blocks; the predictor is
 * always the block's first sample. */
void xboxadpcm_encode_block(XboxAdpcmState* s, const int16_t* pcm, uint8_t* out, int16_t* recon);

/* Decode one block (36 * channels bytes) into 64 interleaved frames. Blocks are self-contained:
 * the state is loaded from the header; `s` is only updated for inspection. */
void xboxadpcm_decode_block(XboxAdpcmState* s, const uint8_t* in, int16_t* pcm);

/* Encode `samples` frames of interleaved PCM into whole blocks (the last block is padded by
 * repeating the last sample value, which costs nothing in IMA and avoids a click). Returns the
 * number of bytes written (= xboxadpcm_encoded_size). `out` must hold that many bytes; `recon`
 * (optional) must hold blocks*64*channels samples. */
uint32_t xboxadpcm_encode(const int16_t* pcm, uint32_t samples, uint32_t channels, uint8_t* out, int16_t* recon);

/* Decode `blocks` whole blocks into blocks*64*channels interleaved samples. Returns samples per channel. */
uint32_t xboxadpcm_decode(const uint8_t* in, uint32_t blocks, uint32_t channels, int16_t* pcm);

#ifdef __cplusplus
}
#endif

#endif /* RE4PORT_SND_XBOXADPCM_H */
