"""CT-06: tests for re4conv.adpcm (no game data). Run from xbox/tools:  py -3 -m unittest re4conv.tests.test_adpcm"""
import math
import time
import unittest

from re4conv import adpcm

RATE = 32000
# Test coefficient table (same as xbox/port/tests/ct06/dspadpcm_ref_enc.c); pair 7 suits a 1 kHz tone.
COEF = [0, 0, 2048, 0, 0, 2048, 4096, -2048, 3072, -1024, 3584, -1536, 3891, -1843, 4017, -2048]


def sine(n, freq, amp=20000.0):
    return [int(math.floor(amp * math.sin(2 * math.pi * freq * i / RATE) + 0.5)) for i in range(n)]


def snr_db(ref, got):
    sig = sum(v * v for v in ref)
    err = sum((a - b) ** 2 for a, b in zip(ref, got))
    return 99.0 if err == 0 else 10 * math.log10(sig / err)


def ref_dsp_encode(pcm, pairs=(7, 0)):
    """Small reference DSP-ADPCM encoder: per frame, try the given coef pairs and every scale,
    greedy nearest nibble with exact decoder reconstruction; keep the lowest-error choice."""
    out = bytearray()
    h1 = h2 = 0
    for f in range(0, len(pcm), 14):
        chunk = pcm[f:f + 14]
        chunk = chunk + [chunk[-1]] * (14 - len(chunk))
        best = None
        for p in pairs:
            c1, c2 = COEF[2 * p], COEF[2 * p + 1]
            for sc in range(13):
                a1, a2, err, nibs = h1, h2, 0, []
                for s in chunk:
                    pred = c1 * a1 + c2 * a2 + 1024
                    n = int(round(((s << 11) - pred) / float((1 << sc) << 11)))
                    n = max(-8, min(7, n))
                    v = max(-32768, min(32767, (((n << sc) << 11) + pred) >> 11))
                    err += (s - v) ** 2
                    a2, a1 = a1, v
                    nibs.append(n & 0xF)
                if best is None or err < best[0]:
                    best = (err, (p << 4) | sc, nibs, a1, a2)
        _, ps, nibs, h1, h2 = best
        out.append(ps)
        for k in range(0, 14, 2):
            out.append((nibs[k] << 4) | nibs[k + 1])
    return bytes(out)


def play_loop(decoded, start_blk, end_blk, total):
    """Simulates APU playback of a looping buffer: play to the end block, wrap to the start block."""
    s, e = start_blk * 64, end_blk * 64
    out = list(decoded[:e])
    while len(out) < total:
        out += decoded[s:e]
    return out[:total]


class AddressTests(unittest.TestCase):
    def test_game_formulas(self):
        # snd_iss3.cpp: cur = base + 2; addr(n) = cur + n/14*16 + n%14
        for n in range(0, 2000, 3):
            self.assertEqual(adpcm.sample_to_nibble(n), 2 + n // 14 * 16 + n % 14)
            self.assertEqual(adpcm.nibble_to_sample(adpcm.sample_to_nibble(n)), n)
        # snd_str0.cpp: read_end = nibbles / 2; frame headers at nibbles 16k, 16k+1
        self.assertEqual(adpcm.nibbles_to_bytes(0x40000), 0x20000)
        self.assertEqual(adpcm.nibble_to_sample(16), 14)
        self.assertEqual(adpcm.nibble_to_sample(17), 14)
        self.assertEqual(adpcm.loop_from_nibbles(2, adpcm.sample_to_nibble(99)), (0, 100))

    def test_shd_coefs(self):
        shd = list(range(16))
        self.assertEqual(adpcm.shd_coefs_to_ax(shd)[:4], [0, 8, 1, 9])


class DspDecodeTests(unittest.TestCase):
    def test_hand_vector(self):
        frames = bytes([0x03, 0x12, 0x3F, 0, 0, 0, 0, 0, 0x10, 0x11, 0x1F, 0, 0, 0, 0, 0])
        expect = [8, 16, 24, -8] + [0] * 10 + [1, 2, 3] + [2] * 11
        self.assertEqual(adpcm.decode_dsp(frames, COEF), expect)
        self.assertEqual(adpcm.decode_dsp(frames, [v & 0xFFFF for v in COEF], num_samples=5), expect[:5])


class XboxTests(unittest.TestCase):
    def test_roundtrip(self):
        pcm = sine(6400, 1000)
        enc = adpcm.encode_xbox(pcm)
        self.assertEqual(len(enc), 100 * 36)
        self.assertGreater(snr_db(pcm, adpcm.decode_xbox(enc)), 30)

    def test_block_layout(self):
        enc = adpcm.encode_xbox([0x1234] + [0x1234] * 63)
        self.assertEqual(enc[0:2], b'\x34\x12')     # predictor LE = sample 0
        self.assertEqual(enc[3], 0)
        self.assertEqual(enc[35] >> 4, 0)            # nibble 63 is padding

    def test_partial_block(self):
        pcm = sine(100, 1000)
        enc = adpcm.encode_xbox(pcm)
        self.assertEqual(len(enc), 72)
        self.assertGreater(snr_db(pcm, adpcm.decode_xbox(enc)[:100]), 25)


class DspToXboxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pcm = sine(RATE, 1000)
        cls.dsp = ref_dsp_encode(cls.pcm)
        cls.dec = adpcm.decode_dsp(cls.dsp, COEF, num_samples=RATE)

    def test_reference_encoder(self):
        self.assertGreater(snr_db(self.pcm, self.dec), 30)

    def test_one_shot_one_second(self):
        t0 = time.perf_counter()
        data, ls, le, warns = adpcm.dsp_to_xbox(self.dsp, COEF, None, num_samples=RATE)
        dt = time.perf_counter() - t0
        self.assertLess(dt, 1.0, 'conversion took %.2f s' % dt)
        self.assertEqual((ls, le, warns), (None, None, []))
        self.assertEqual(len(data), 500 * 36)
        self.assertGreater(snr_db(self.pcm, adpcm.decode_xbox(data)[:RATE]), 30)

    def _check_loop(self, loop, total=20000, **kw):
        data, ls, le, warns = adpcm.dsp_to_xbox(self.dsp, COEF, loop, num_samples=RATE, **kw)
        self.assertLess(ls, le)
        self.assertEqual(len(data), le * 36)
        played = play_loop(adpcm.decode_xbox(data), ls, le, total)
        s, e = loop
        ideal = list(self.dec[:e])
        while len(ideal) < total:
            ideal += self.dec[s:e]
        return snr_db(ideal[:total], played), warns

    def test_loop_aligned(self):
        snr, warns = self._check_loop((640, 640 + 3200))
        self.assertGreater(snr, 30)
        self.assertEqual(warns, ['%d samples after the loop end dropped' % (RATE - 3840)])

    def test_loop_unaligned_unrolled(self):
        # start not on a block, length 224 = 7 periods of 1 kHz -> unrolled x2 (exact)
        snr, warns = self._check_loop((101, 101 + 224))
        self.assertGreater(snr, 30)
        self.assertTrue(any('unrolled x2' in w for w in warns), warns)

    def test_loop_resampled(self):
        # 1000 samples (= 31.25 periods, not periodic in 64): forbid unrolling -> resample to 1024
        data, ls, le, warns = adpcm.dsp_to_xbox(self.dsp, COEF, (37, 1037), num_samples=RATE, max_unroll=0)
        self.assertEqual((le - ls) * 64, 1024)
        self.assertEqual(ls, 1)
        self.assertTrue(any('resampled to 1024' in w for w in warns), warns)

    def test_bad_loop(self):
        with self.assertRaises(ValueError):
            adpcm.dsp_to_xbox(self.dsp, COEF, (500, 400), num_samples=RATE)


if __name__ == '__main__':
    unittest.main()
