# CT-06 — DSP-ADPCM decoder and Xbox ADPCM encoder

**Status:** open

## Goal

Portable C for the two sample codecs the audio pipeline needs (`03-AUDIO.md` section 4), with a Python
wrapper for the asset converter and tests.

## Deliverables

1. `xbox/port/snd/dspadpcm.c/.h`: GameCube DSP-ADPCM decoder. Frame = 1 header byte (high nibble = scale
   exponent, low nibble = coefficient pair index 0..7) + 7 bytes = 14 samples (nibbles, high first).
   `sample = clamp((nibble_signed << scale) * 2048 + coef[2i] * hist1 + coef[2i+1] * hist2 + 1024) >> 11`
   with 16-bit history (verify against Dolphin's `DSPHLE`/libogc `dsp` docs and the field comments in
   `include/snd_drv.h`: `coef[16]`, `nibbles`, `lptop_nbl`/`lpend_nbl`, loop predictor/scale/hist).
   API: decode `n` nibbles from a nibble address with state (hist1, hist2), loop-aware.
2. `xbox/port/snd/xboxadpcm.c/.h`: Xbox ADPCM encoder (IMA ADPCM, 4 bits/sample, blocks of 64 samples:
   4-byte header (predictor s16 + step index u8 + reserved) + 32 bytes of nibbles per channel, stereo
   interleaved per 4-byte word) and decoder for tests. This is the `WAVE_FORMAT_XBOX_ADPCM` (0x0069)
   layout used by the XDK's `xbadpcm` tools.
3. `xbox/tools/re4conv/adpcm.py`: ctypes wrapper (or a pure-Python port) exposing
   `dsp_to_xbox(dsp_bytes, coefs, loop) -> (xbox_bytes, loop_block_start, loop_block_end, warnings)`;
   loops are padded to 64-sample block boundaries, with the padding method documented.
4. Tests (`xbox/port/tests`): a synthetic sine encoded with a reference DSP-ADPCM encoder (write one:
   brute-force the best coefficient pair/scale per frame, it need not be fast) decodes back within SNR
   > 30 dB; Xbox ADPCM round-trip; nibble address arithmetic (`nibbles → bytes`, frame boundaries) matches
   the game's conversions in `snd_str0.cpp` (`read_end = nibbles / 2`).

## Acceptance

Tests pass on Linux; the Python wrapper converts a 1-second test tone in under a second; no game data
needed.
