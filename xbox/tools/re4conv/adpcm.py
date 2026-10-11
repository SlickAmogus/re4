"""re4conv.adpcm -- GameCube DSP-ADPCM -> Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM, 0x0069), CT-06.

Pure-Python port of xbox/port/snd/dspadpcm.c (decoder) and xbox/port/snd/xboxadpcm.c (encoder);
the C files document the formats and the game's nibble-address conventions. Outputs are
bit-identical to the C code (same arithmetic, same first-block step-index search).

Main entry point::

    dsp_to_xbox(dsp_bytes, coefs, loop=None, num_samples=None, pred_scale=None, yn1=0, yn2=0)
        -> (xbox_bytes, loop_block_start, loop_block_end, warnings)

* ``dsp_bytes``: raw mono DSP-ADPCM frames (8 bytes = header + 14 nibbles), frame-aligned.
* ``coefs``: 16 ints in AX ``a[8][2]`` order flattened (``coefs[2i]`` * yn1 + ``coefs[2i+1]`` * yn2),
  host values (already byte-swapped; signed or unsigned 16-bit both accepted). SND_SHD stores
  ``coef[i]``/``coef[i+8]`` as the pair (snd_str3.cpp:108-111): use ``shd_coefs_to_ax()`` first.
* ``loop``: ``None``/``False`` for a one-shot, or ``(start, end)`` in samples, ``end`` EXCLUSIVE.
  ``loop_from_nibbles()`` converts AX nibble addresses (inclusive end) to this form.
* Returns mono Xbox ADPCM blocks (36 bytes / 64 samples). For a loop, ``loop_block_start`` and
  ``loop_block_end`` are block indices, end EXCLUSIVE: the loop region is blocks
  [start, end), i.e. bytes [start*36, end*36) / samples [start*64, end*64). For a one-shot both
  are ``None``.

Loop padding method (the APU loops on whole 64-sample blocks):

1. **Start alignment by rotation (lossless).** The loop start is moved forward to the next block
   boundary ``s' = ceil(s/64)*64`` and the end by the same amount; the samples between the old and
   the new end are the first ``s'-s`` samples of the loop body. Because the loop is periodic, the
   played sound is identical sample for sample; nothing is delayed.
2. **Length to a multiple of 64.** If the loop length L is not a multiple of 64, the body is
   unrolled k = 64/gcd(L, 64) times (k*L is then a multiple of 64; exact, no pitch change) when
   k*L <= ``max_unroll`` samples (default 65536). Otherwise the body is resampled (linear
   interpolation, periodic) to the nearest non-zero multiple of 64, which changes the loop's pitch
   by |L'/L - 1| (at most 32/L); a warning reports it.
3. Anything after the loop end is dropped (the loop never exits). The last block is padded by
   holding the last sample (one-shots) -- for loops the padded data always lies inside the loop.

Every block header is self-contained (predictor = first sample, step index), so the wrap from
the loop end to the loop start block is clean without a separate loop context.
"""
import math

FRAME_BYTES = 8
FRAME_NIBBLES = 16
FRAME_SAMPLES = 14
XBOX_SAMPLES_PER_BLOCK = 64
XBOX_BLOCK_BYTES_CH = 36
XBOX_FORMAT_TAG = 0x0069

STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28,
    31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def _s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def _clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


# ---- nibble address arithmetic (dspadpcm.h; game: snd_iss3.cpp, snd_str0.cpp) -----------------

def sample_to_nibble(sample):
    """Sample index -> nibble address from a frame-aligned base (includes the +2 header skip)."""
    return sample // FRAME_SAMPLES * FRAME_NIBBLES + sample % FRAME_SAMPLES + 2


def nibble_to_sample(nibble):
    """Nibble address -> sample index (a header nibble maps to its frame's first sample)."""
    in_frame = nibble % FRAME_NIBBLES
    return nibble // FRAME_NIBBLES * FRAME_SAMPLES + (0 if in_frame < 2 else in_frame - 2)


def nibbles_to_bytes(nibbles):
    """snd_str0.cpp: read_end = nibbles / 2."""
    return nibbles // 2


def loop_from_nibbles(loop_nibble, end_nibble):
    """AX loopAddress / inclusive endAddress (relative to the sample's frame-aligned start) ->
    (start, end_exclusive) samples for dsp_to_xbox(). Note the iss3 quirk (xbox/docs/03-AUDIO.md,
    "ADPCM conventions (CT-06)"): snd_iss3 computes end from loopStart+loopLength, i.e. one past
    the last sample, so for those pass (loopStart, loopStart+loopLength) directly instead."""
    return nibble_to_sample(loop_nibble), nibble_to_sample(end_nibble) + 1


def shd_coefs_to_ax(coef16):
    """SND_SHD coef[16] (pair i = coef[i], coef[i+8]) -> AX a[8][2] flattened order."""
    out = []
    for i in range(8):
        out += [coef16[i], coef16[i + 8]]
    return out


# ---- DSP-ADPCM decoder ------------------------------------------------------------------------

def decode_dsp(dsp_bytes, coefs, num_samples=None, pred_scale=None, yn1=0, yn2=0):
    """Decodes mono DSP-ADPCM frames to a list of int16 samples (linear, from sample 0).
    ``pred_scale`` is accepted for API symmetry; every frame's header is read from the data."""
    if len(coefs) != 16:
        raise ValueError('coefs must have 16 entries')
    c = [_s16(v) for v in coefs]
    nframes = len(dsp_bytes) // FRAME_BYTES
    total = nframes * FRAME_SAMPLES
    if num_samples is None:
        num_samples = total
    if num_samples > total:
        raise ValueError('num_samples %d > %d samples in %d frames' % (num_samples, total, nframes))
    h1, h2 = _s16(yn1), _s16(yn2)
    out = [0] * num_samples
    data = bytes(dsp_bytes)
    i = 0
    for f in range((num_samples + FRAME_SAMPLES - 1) // FRAME_SAMPLES):
        base = f * FRAME_BYTES
        ps = data[base]
        scale = 1 << (ps & 0xF)
        idx = (ps >> 4) & 7
        c1, c2 = c[idx * 2], c[idx * 2 + 1]
        for k in range(FRAME_SAMPLES):
            if i >= num_samples:
                break
            b = data[base + 1 + (k >> 1)]
            n = (b & 0xF) if (k & 1) else (b >> 4)
            if n >= 8:
                n -= 16
            v = ((n * scale) << 11) + 1024 + c1 * h1 + c2 * h2
            v = _clamp16(v >> 11)
            h2, h1 = h1, v
            out[i] = v
            i += 1
    return out


# ---- Xbox ADPCM (IMA, 64 samples per block) ---------------------------------------------------

def _ima_compress(pred, idx, sample):
    """One reference IMA step; returns (nibble, new_pred, new_idx). Same as ima_compress() in C."""
    step = STEP_TABLE[idx]
    delta = sample - pred
    diff = step >> 3
    n = 0
    if delta < 0:
        n = 8
        delta = -delta
    if delta >= step:
        n |= 4
        delta -= step
        diff += step
    step >>= 1
    if delta >= step:
        n |= 2
        delta -= step
        diff += step
    step >>= 1
    if delta >= step:
        n |= 1
        diff += step
    pred = pred - diff if n & 8 else pred + diff
    pred = _clamp16(pred)
    idx += INDEX_TABLE[n]
    idx = 0 if idx < 0 else (88 if idx > 88 else idx)
    return n, pred, idx


def _ima_expand(pred, idx, n):
    step = STEP_TABLE[idx]
    diff = step >> 3
    if n & 4:
        diff += step
    if n & 2:
        diff += step >> 1
    if n & 1:
        diff += step >> 2
    pred = pred - diff if n & 8 else pred + diff
    pred = _clamp16(pred)
    idx += INDEX_TABLE[n]
    idx = 0 if idx < 0 else (88 if idx > 88 else idx)
    return pred, idx


def _block_error(blk, idx):
    pred = blk[0]
    err = 0
    for k in range(1, XBOX_SAMPLES_PER_BLOCK):
        s = blk[k]
        _, pred, idx = _ima_compress(pred, idx, s)
        d = s - pred
        err += d * d
    return err


def _choose_step_index(blk):
    best, best_err = 0, None
    for i in range(89):
        e = _block_error(blk, i)
        if best_err is None or e < best_err:
            best, best_err = i, e
    return best


def encode_xbox(pcm):
    """Encodes mono int16 samples into Xbox ADPCM blocks (bytes). The last block is padded by
    holding the last sample; the first block's step index is searched (as xboxadpcm_encode())."""
    n = len(pcm)
    nblocks = (n + XBOX_SAMPLES_PER_BLOCK - 1) // XBOX_SAMPLES_PER_BLOCK
    out = bytearray(nblocks * XBOX_BLOCK_BYTES_CH)
    idx = 0
    for b in range(nblocks):
        base = b * XBOX_SAMPLES_PER_BLOCK
        blk = [int(v) for v in pcm[base:base + XBOX_SAMPLES_PER_BLOCK]]
        if len(blk) < XBOX_SAMPLES_PER_BLOCK:
            blk += [blk[-1]] * (XBOX_SAMPLES_PER_BLOCK - len(blk))
        if b == 0:
            idx = _choose_step_index(blk)
        o = b * XBOX_BLOCK_BYTES_CH
        pred = _clamp16(blk[0])
        out[o] = pred & 0xFF
        out[o + 1] = (pred >> 8) & 0xFF
        out[o + 2] = idx
        out[o + 3] = 0
        for k in range(XBOX_SAMPLES_PER_BLOCK - 1):
            nib, pred, idx = _ima_compress(pred, idx, blk[k + 1])
            out[o + 4 + (k >> 1)] |= (nib << 4) if (k & 1) else nib     # low nibble first
        # nibble 63 stays 0 (padding)
    return bytes(out)


def decode_xbox(data, channels=1):
    """Decodes Xbox ADPCM blocks (mono or stereo-interleaved) to a list of interleaved int16."""
    bsz = XBOX_BLOCK_BYTES_CH * channels
    nblocks = len(data) // bsz
    out = [0] * (nblocks * XBOX_SAMPLES_PER_BLOCK * channels)
    for b in range(nblocks):
        o = b * bsz
        for ch in range(channels):
            pred = _s16(data[o + ch * 4] | (data[o + ch * 4 + 1] << 8))
            idx = min(data[o + ch * 4 + 2], 88)
            p = b * XBOX_SAMPLES_PER_BLOCK * channels + ch
            out[p] = pred
            for k in range(XBOX_SAMPLES_PER_BLOCK - 1):
                word, byte = k // 8, (k % 8) // 2
                v = data[o + 4 * channels + (word * channels + ch) * 4 + byte]
                nib = (v >> 4) if (k & 1) else (v & 0xF)
                pred, idx = _ima_expand(pred, idx, nib)
                out[p + (k + 1) * channels] = pred
    return out


# ---- loop padding -----------------------------------------------------------------------------

def _resample_periodic(body, new_len):
    """Linear interpolation of one period `body` to `new_len` samples (wraps at the end)."""
    L = len(body)
    out = []
    for i in range(new_len):
        x = i * L / new_len
        j = int(x)
        t = x - j
        a, b = body[j % L], body[(j + 1) % L]
        out.append(_clamp16(int(math.floor(a + (b - a) * t + 0.5))))
    return out


def pad_loop(pcm, loop_start, loop_end, max_unroll=65536):
    """Applies the loop padding method (module docstring) to PCM. Returns
    (pcm', loop_start', loop_end', warnings) with both loop points multiples of 64 (end exclusive)."""
    warnings = []
    n = len(pcm)
    if not (0 <= loop_start < loop_end <= n):
        raise ValueError('bad loop (%d, %d) for %d samples' % (loop_start, loop_end, n))
    body = list(pcm[loop_start:loop_end])
    L = len(body)
    if L % XBOX_SAMPLES_PER_BLOCK:
        k = XBOX_SAMPLES_PER_BLOCK // math.gcd(L, XBOX_SAMPLES_PER_BLOCK)
        if k * L <= max_unroll:
            body = body * k
            warnings.append('loop length %d unrolled x%d to %d samples' % (L, k, k * L))
        else:
            new_len = max(XBOX_SAMPLES_PER_BLOCK,
                          int(round(L / XBOX_SAMPLES_PER_BLOCK)) * XBOX_SAMPLES_PER_BLOCK)
            body = _resample_periodic(body, new_len)
            warnings.append('loop length %d resampled to %d samples (pitch %+.3f%%)'
                            % (L, new_len, (L / new_len - 1.0) * 100.0))
    # rotate the start forward to a block boundary (lossless: the loop is periodic)
    shift = (-loop_start) % XBOX_SAMPLES_PER_BLOCK
    pre = list(pcm[:loop_start])
    BL = len(body)
    new_start = loop_start + shift
    rotated = [body[(shift + i) % BL] for i in range(BL)]
    pre += body[:shift] if shift <= BL else [body[i % BL] for i in range(shift)]
    out = pre + rotated
    if n > loop_end:
        warnings.append('%d samples after the loop end dropped' % (n - loop_end))
    return out, new_start, new_start + BL, warnings


# ---- main entry point -------------------------------------------------------------------------

def dsp_to_xbox(dsp_bytes, coefs, loop=None, num_samples=None, pred_scale=None, yn1=0, yn2=0,
                max_unroll=65536):
    """GameCube DSP-ADPCM (mono) -> Xbox ADPCM (mono). See the module docstring."""
    warnings = []
    if len(dsp_bytes) % FRAME_BYTES:
        warnings.append('%d trailing bytes ignored (not a whole frame)' % (len(dsp_bytes) % FRAME_BYTES))
    if pred_scale is not None and len(dsp_bytes) and (pred_scale & 0xFF) != bytes(dsp_bytes[:1])[0]:
        warnings.append('pred_scale 0x%02X != first frame header 0x%02X'
                        % (pred_scale & 0xFF, bytes(dsp_bytes[:1])[0]))
    pcm = decode_dsp(dsp_bytes, coefs, num_samples, pred_scale, yn1, yn2)
    if not loop:
        return encode_xbox(pcm) if pcm else b'', None, None, warnings
    start, end = loop
    if end > len(pcm):
        warnings.append('loop end %d clamped to %d samples' % (end, len(pcm)))
        end = len(pcm)
    pcm, s2, e2, w = pad_loop(pcm, start, end, max_unroll)
    warnings += w
    data = encode_xbox(pcm)
    return data, s2 // XBOX_SAMPLES_PER_BLOCK, e2 // XBOX_SAMPLES_PER_BLOCK, warnings
