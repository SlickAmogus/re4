"""re4conv.room_nested on synthetic SMD / SHD / TEX / ITM / EFF / EVS entries built from the BIN and TPL
test builders (test_model.build, test_tpl.make_tpl): the head must come out little-endian, every nested
file converted by its own converter, the tables re-pointed, and check() must pass."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import ConvertError, model, room_nested as rn, tpl  # noqa: E402
from re4conv.tests.test_model import build as build_model  # noqa: E402
from re4conv.tests.test_tpl import make_tpl  # noqa: E402


def pad(b, a=0x20):
    return b + b'\0' * ((-len(b)) % a)


TPL = pad(make_tpl([(tpl.I8, 8, 4, bytes(range(32)), None, 0)]))
BIN = pad(build_model('>'))


def slot_table(files, nslots=8):
    """u32 slots relative to the table (gap = nslots * 4, 0x20-aligned), files 0x20-aligned after it."""
    gap = max(0x20, (nslots * 4 + 0x1F) & ~0x1F)
    slots, body = [], b''
    for f in files:
        slots.append(gap + len(body))
        body += pad(f)
    return pad(struct.pack(f'>{nslots}I', *(slots + [0] * (nslots - len(slots)))), gap) + body


def counted_table(files):
    n = len(files)
    gap = (4 + 4 * n + 0x1F) & ~0x1F
    slots, body = [], b''
    for f in files:
        slots.append(gap + len(body))
        body += pad(f)
    return pad(struct.pack(f'>I{n}I', n, *slots), gap) + body


def smd():
    work = struct.pack('>9f4BfI20sI', 1, 2, 3, 0, 0, 0, 1, 1, 1, 0, 0, 0xFF, 1, 0, 0, bytes(20), 0)
    head = pad(struct.pack('>BBHIII', 0x40, 0, 1, 0, 0, 0) + work)
    bt = slot_table([BIN])
    tt = slot_table([TPL])
    d = bytearray(head + bt + tt)
    struct.pack_into('>III', d, 4, len(head), len(head) + len(bt), len(d))    # no motion table
    return bytes(d)


class NestedTest(unittest.TestCase):
    def roundtrip(self, tag, d):
        out, info = rn.convert(tag, d)
        self.assertEqual(info['status'], 'converted', info)
        self.assertEqual(rn.check(tag, out, d), [])
        return out, info

    def test_smd(self):
        d = smd()
        out, info = self.roundtrip('SMD', d)
        self.assertEqual([r['tag'] for r in info['entries']], ['BIN', 'TPL'])
        ver, flag, n, bt, tt, mt = struct.unpack_from('<BBHIII', out)
        self.assertEqual((ver, n), (0x40, 1))
        self.assertEqual(mt, len(out))                      # "no motion table" follows the new size
        self.assertEqual(struct.unpack_from('<9f', out, 0x10)[:3], (1.0, 2.0, 3.0))
        b0 = bt + struct.unpack_from('<I', out, bt)[0]
        t0 = tt + struct.unpack_from('<I', out, tt)[0]
        self.assertEqual(model.check(out[b0:tt], BIN), [])
        self.assertEqual(tpl.check(out[t0:], TPL), [])
        self.assertEqual(b0 % 0x20, 0)
        self.assertEqual(t0 % 0x20, 0)

    def test_shd(self):
        ent = struct.pack('>9fBBB33s', 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0x80, bytes(33))
        head = pad(struct.pack('>BBHI', 0x41, 0, 1, 0) + bytes(8) + ent)
        d = bytearray(head + slot_table([BIN]))
        struct.pack_into('>I', d, 4, len(head))
        self.roundtrip('SHD', bytes(d))

    def test_tex_and_itm(self):
        anm = struct.pack('>HHhhHBBB3s', 8, 4, 4, 2, 2, 1, 0, 0, bytes(3)) + bytes([0, 1, 10, 10])
        idt = struct.pack('>I', 1) + struct.pack('>HHI', 7, 0, 0)
        head = pad(struct.pack('>4I', 3, 0x10, 0, 0) + idt)
        tt = counted_table([TPL])
        d = bytearray(head + tt + counted_table([pad(anm)]))
        struct.pack_into('>II', d, 8, len(head), len(head) + len(tt))
        out, _ = self.roundtrip('TEX', bytes(d))
        a = struct.unpack_from('<I', out, 0xC)[0]
        a += struct.unpack_from('<I', out, a + 4)[0]
        self.assertEqual(struct.unpack_from('<HHhh', out, a), (8, 4, 4, 2))
        # ITM: id table + BIN and TPL tables
        bt = counted_table([BIN])
        d = bytearray(head + bt + counted_table([TPL]))
        struct.pack_into('>4I', d, 0, 0, 0x10, len(head), len(head) + len(bt))
        self.roundtrip('ITM', bytes(d))
        # TEX with ofsTpl = ofsAnm = 0 (St2 r20f): id table only
        self.roundtrip('TEX', struct.pack('>4I', 3, 0x20, 0, 0) + bytes(0x10) + struct.pack('>I', 0) + bytes(0x1C))

    def test_eff(self):
        rec = bytearray(0x12C)
        struct.pack_into('>4BH2BI3f', rec, 0, 1, 2, 0, 3, 30, 0, 0, 0, 1.0, 2.0, 3.0)
        struct.pack_into('>II', rec, 0xCC, 1000, 0xFFFFFFFF)
        esq = struct.pack('>4HHBB3f3fBBHII', 1, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0) + bytes(rec)
        path = struct.pack('>H2s', 1, b'\0\0') + struct.pack('>3f3ff3BB3B5s', 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                                             0, 0, 0, bytes(5))
        efm_parts = [pad(BIN), pad(TPL)]
        efm = bytearray(pad(struct.pack('>5I', 0, 0x20, 0x20 + len(efm_parts[0]), 0, 0)))
        efm += b''.join(efm_parts)
        idt = struct.pack('>I', 1) + struct.pack('>HHI', 3, 0, 0)
        head = bytearray(pad(struct.pack('>12I', 0xB, *([0] * 11))))
        tables = [('TPL', 0x18, counted_table([TPL])), ('EST', 0x20, counted_table([pad(esq)])),
                  ('PTH', 0x28, counted_table([pad(path)])), ('EFM', 0x2C, counted_table([bytes(efm)]))]
        d = head + pad(idt)
        struct.pack_into('>I', d, 4, len(head))           # tpl id table
        struct.pack_into('>I', d, 8, len(head))           # est id table (shared)
        for _, field, t in tables:
            struct.pack_into('>I', d, field, len(d))
            d += t
        out, info = self.roundtrip('EFF', bytes(d))
        self.assertEqual(sorted(r['tag'] for r in info['entries']), ['EFM', 'ESQ', 'PTH', 'TPL'])
        efm_rec = [r for r in info['entries'] if r['tag'] == 'EFM'][0]
        self.assertEqual([f['tag'] for f in efm_rec['files']], ['BIN', 'TPL'])
        e = struct.unpack_from('<I', out, 0x20)[0]
        e += struct.unpack_from('<I', out, e + 4)[0]
        self.assertEqual(struct.unpack_from('<4H', out, e), (1, 0, 1, 2))
        self.assertEqual(struct.unpack_from('<II', out, e + 0x30 + 0xCC), (1000, 0xFFFFFFFF))

    def test_refusals(self):
        with self.assertRaises(ConvertError):
            rn.convert('SMD', struct.pack('>BBHIII', 0x30, 0, 0, 0x10, 0x10, 0x10))
        with self.assertRaises(ConvertError):
            rn.convert('EFF', struct.pack('>12I', 0xA, *([0] * 11)))

    def test_fnt(self):
        from re4conv import fonts
        width = bytes([1, 7, 0, 8] * 4)
        d = struct.pack('>2I', 0x20, 0x20 + len(TPL)) + bytes(0x18) + TPL + width
        out = fonts.convert(d)
        t, w = struct.unpack_from('<2I', out)
        self.assertEqual((t, w % 0x20, out[w:]), (0x20, 0, width))
        self.assertEqual(fonts.check(out, d), [])
        with self.assertRaises(ConvertError):
            fonts.convert(struct.pack('>2I', 0x40, 0x20) + bytes(0x18))

    def test_evs(self):
        d = struct.pack('>iI', 0, 0x10) + b'\xcd' * 0x18
        out, info = rn.convert_evs(d)
        self.assertEqual(out[:8], struct.pack('<iI', 0, 0x10))
        self.assertEqual(rn.check_evs(out, d), [])


if __name__ == '__main__':
    unittest.main()
