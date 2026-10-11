"""re4conv.sound on synthetic big-endian banks / headers, plus a real-data smoke test (skipped unless
C:/Claude/RE4-Xbox/gamedata/disc1/files or $RE4_DISC1 exists)."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import ConvertError, sound  # noqa: E402

DISC1 = os.environ.get('RE4_DISC1', 'C:/Claude/RE4-Xbox/gamedata/disc1/files')


def _read(p):
    with open(p, 'rb') as f:
        return f.read()


def _pad(b, a=0x20):
    return b + b'\0' * ((-len(b)) % a)


def wt(e='>'):
    """WT with 1 instrument of 2 keys, 2 regions, 1 articulation, 2 samples, 2 ADPCM contexts."""
    inst = struct.pack(f'{e}2H', 0, 1)
    regions = (struct.pack(f'{e}BBhiIIII', 60, 0, -5, -0x10000, 0x10, 0x200, 0, 0) +
               struct.pack(f'{e}BBhiIIII', 61, 1, 7, 0, 0, 0, 0, 1))
    art = struct.pack(f'{e}20i', *range(-10, 10))
    samples = (struct.pack(f'{e}HHIIH2s', 0, 32000, 2, 0xC98A, 0, b'\0\0') +
               struct.pack(f'{e}HHIIH2s', 0, 22050, 0xE660, 0xEFC9, 1, b'\0\0'))
    adpcm = struct.pack(f'{e}23H', *range(0x100, 0x117)) + struct.pack(f'{e}23H', *range(0xF000, 0xF017))
    perc = 0x18
    reg = perc + len(inst)
    a = reg + len(regions)
    s = a + len(art)
    ad = s + len(samples)
    return struct.pack(f'{e}6I', perc, perc, reg, a, s, ad) + inst + regions + art + samples + adpcm


def iss(e='>', seq=False):
    w = _pad(wt(e), 0x10)
    sit = struct.pack(f'{e}H8bHH8BH', 0x0001, 0, 1, 50, -1, 100, -1, -1, 3, 0x100, 0x200, 80, 0, 1, 0, 0,
                      2, 0x20, 50, 0x4004)
    wt_ofs, sit_ofs = 0x10, 0x10 + len(w)
    blob = w + sit
    seq_ofs = 0
    if seq:
        seq_ofs = _align(0x10 + len(blob), 0x10)
        blob = blob + b'\0' * (seq_ofs - 0x10 - len(blob))
        midi = bytes([0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0x90, 0x3C, 0x7F])
        blob += struct.pack(f'{e}3I', 2, 12, 12 + len(midi)) + midi + midi
    return struct.pack(f'{e}4I', 1, wt_ofs, sit_ofs, seq_ofs) + blob


def _align(n, a):
    return (n + a - 1) & ~(a - 1)


def mram(e='>'):
    i = _pad(iss(e))
    rnd_ofs = 0x20 + len(i)
    tbl = [0] * 32
    tbl[3] = 0x80
    sbl = struct.pack(f'{e}5H', 3, 0, 4, 5, 6)
    return struct.pack(f'{e}2I', 0x20, rnd_ofs) + b'\0' * 0x18 + i + struct.pack(f'{e}32I', *tbl) + sbl


PCM = bytes(range(0x30, 0x30 + 0x61))   # odd size, as the real ARAM records


def bank(blocks, e='>'):
    """blocks: [(sndType, mram bytes)] -> bank with records 1/2 per block."""
    recs, data, off = [], b'', 0x400
    for st, m in blocks:
        for ty, blob in ((1, m), (2, PCM)):
            recs.append((ty, len(blob), 0, off, st, 0, 0, 0))
            data += _pad(blob)
            off += len(_pad(blob))
    t = sound.SIG + b''.join(struct.pack(f'{e}8I', *r) for r in recs) + struct.pack(f'{e}8I', sound.END, *[0] * 7)
    return t + b'\0' * (0x400 - len(t)) + data


def str_hed(e='>'):
    shd = struct.pack(f'{e}8I46H4s', 1, 0x43509, 0x4CEE7, 32000, 2, 2, 0x4CEE6, 0, *range(1, 47), b'\0' * 4)
    rit = struct.pack(f'{e}h4b2s3bsH2b', 0, 0, 1, 100, 64, b'\0\0', -1, -1, 0, b'\0', 1, -1, -1)
    # block: hdr (12) pad to 0x10, sbh table of 2 ofs (+ pad) at 0x10, 2 SHDs, then RIT
    sbh = struct.pack(f'{e}4I', 0x10, 0x90, 0, 0)
    blk = struct.pack(f'{e}3I', 2, 0x10, 0x10 + 0x10 + 0x100) + b'\0' * 4 + sbh + shd + shd + rit + rit
    return struct.pack(f'{e}2I', 0x20, 0x20 + len(blk)) + b'\0' * 0x18 + blk + blk


def door_hed(e='>'):
    return (struct.pack(f'{e}2I', 0x20, 0x30) + b'\0' * 0x18 + struct.pack(f'{e}3I', 0, 0x10240, 0x1F200) +
            b'\0' * 4 + struct.pack(f'{e}4H', 3, 1, 2, 0x10) + b'\0' * 8)


class SoundTest(unittest.TestCase):
    def roundtrip(self, conv, chk, d, le_ref=None):
        out = conv(d)
        self.assertEqual(len(out), len(d))
        self.assertEqual(chk(out, d), [])
        if le_ref is not None:
            self.assertEqual(out, le_ref)
        return out

    def test_bank_matches_le_build(self):
        d = bank([(6, mram('>')), (5, mram('>'))])
        le = bank([(6, mram('<')), (5, mram('<'))], '<')
        # the signature is bytes and stays; everything else must equal the little-endian build
        self.roundtrip(sound.convert_bank, sound.check_bank, d, le)

    def test_bgm_block_with_sequence(self):
        d = bank([(3, iss('>', seq=True))])
        out = self.roundtrip(sound.convert_bank, sound.check_bank, d, bank([(3, iss('<', seq=True))], '<'))
        self.assertIn(bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20]), out)    # MIDI bytes kept

    def test_samples_unchanged(self):
        out = sound.convert_bank(bank([(8, mram())]))
        self.assertIn(PCM, out)

    def test_concatenated_banks(self):
        d = bank([(7, mram())]) + bank([(7, mram())])
        le = bank([(7, mram('<'))], '<') + bank([(7, mram('<'))], '<')
        self.roundtrip(sound.convert_banks, sound.check_banks, d, le)

    def test_str_hed(self):
        self.roundtrip(sound.convert_str_hed, sound.check_str_hed, str_hed(), str_hed('<'))

    def test_door_and_midi_hed(self):
        self.roundtrip(sound.convert_door_hed, sound.check_door_hed, door_hed(), door_hed('<'))
        h = struct.pack('>24I', 0, 0x69000, *([0] * 22))
        self.roundtrip(sound.convert_midi_hed, sound.check_midi_hed, h, struct.pack('<24I', 0, 0x69000, *([0] * 22)))

    def test_unknown_bytes_refused(self):
        d = bytearray(bank([(6, mram())]))
        d[0x400 + 0x10] = 1       # inside the zero gap between the MRAM header and the ISS block
        with self.assertRaises(ConvertError):
            sound.convert_bank(bytes(d))

    def test_bad_signature_and_offsets(self):
        d = bytearray(bank([(6, mram())]))
        d[0] = 0
        with self.assertRaises(ConvertError):
            sound.convert_bank(bytes(d))
        d = bytearray(bank([(6, mram())]))
        struct.pack_into('>I', d, 0x400 + 4, 0x7FFFF)      # rnd_ofs past the block
        with self.assertRaises(ConvertError):
            sound.convert_bank(bytes(d))

    def test_check_detects_corruption(self):
        d = bank([(6, mram())])
        out = bytearray(sound.convert_bank(d))
        # SIT pitch_l of the first SIT: flip a byte -> values differ
        sit = 0x400 + 0x20 + 0x10 + len(_pad(wt(), 0x10)) + 0xA
        out[sit] ^= 0xFF
        self.assertNotEqual(sound.check_bank(bytes(out), d), [])


@unittest.skipUnless(os.path.isdir(DISC1), 'no game data')
class RealSoundTest(unittest.TestCase):
    def test_bgm_files(self):
        for f in ('bio4str.hed', 'bio4midi.hed', 'doorse.hed', 'doorse.dat', 'bio4midi.dat'):
            d = _read(os.path.join(DISC1, 'bgm', f))
            _, conv, chk = sound.handler('bgm/' + f)
            out = conv(d)
            self.assertEqual(chk(out, d), [], f)

    def test_nested_bank(self):
        import drs
        d = _read(os.path.join(DISC1, 'em', 'pl00.drs'))
        snd = drs.Drs(d).snd.pack()
        self.assertEqual(sound.check_bank(sound.convert_bank(snd), snd), [])


if __name__ == '__main__':
    unittest.main()
