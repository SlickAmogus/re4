"""Synthetic BIN (cModelData) round trips: big-endian built with struct.pack('>'), converted, compared
with the same structure packed '<'. Run: cd xbox/tools && py -3 -m unittest discover re4conv/tests"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import ConvertError, NotThisFormat, model  # noqa: E402


def _pad(b, a=0x20, fill=b'\0'):
    return b + fill * ((-len(b)) % a)


def build(e='>', version=0x20030818, clr=True, bad_index=False, shape=True, tables=True):
    """A one-part model: 3 vertices, 1 normal (s16), 3 colours, 3 texcoords, one triangle."""
    hsize = 0x48 if version > 0x20030817 else 0x40
    nidx = 4 if clr else 3
    heads = struct.pack(f'{e}4B3f', 0, 0xFF, 0, 0, 1.0, 2.0, 3.0)
    vtx = struct.pack(f'{e}12h', 1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0)
    nrm = struct.pack(f'{e}4h', 0, 0x4000, 0, 0)
    clrs = bytes(range(12))
    tex = struct.pack(f'{e}6H', 10, 20, 30, 40, 50, 60)
    weights = bytes(8)
    idx = []
    for v in range(3):
        row = [v, 0] + ([v] if clr else []) + [9 if bad_index and v == 2 else v]   # texcoord count = section span / 4 = 8
        idx += row
    stream = bytes([0x90]) + struct.pack(f'{e}H', 3) + struct.pack(f'{e}{len(idx)}H', *idx)
    stream = _pad(stream)
    poly = bytes(range(0x18)) + struct.pack(f'{e}II', len(stream), 1) + stream
    shp = struct.pack(f'{e}I', 1) + struct.pack(f'{e}Ii', 8, 1) + struct.pack(f'{e}4h', 2, -1, 1, -1)
    blend = struct.pack(f'{e}i', 1) + struct.pack(f'{e}4H', 1, 2, 3, 50)
    flip = struct.pack(f'{e}I', 3) + struct.pack(f'{e}3H', 2, 1, 0)
    # layout
    secs = [('head', heads), ('vtx', vtx), ('nrm', nrm), ('clr', clrs), ('tex', tex), ('w', weights)]
    if shape:
        secs.append(('shape', shp))
    if tables and version > 0x20030817:
        secs += [('blend', blend), ('flip', flip)]
    ofs = {}
    body = b''
    p = (hsize + 0x1F) & ~0x1F
    for name, b in secs:
        b = _pad(b, 0x20)
        ofs[name] = p
        body += b
        p += len(b)
    ofs['parts'] = p
    body += poly
    hdr = struct.pack(f'{e}IIIIIIBBHIIIBBHIIIHHI', ofs['head'], 0, 0x50, ofs['clr'], ofs['tex'], ofs['w'], 1, 1, 1,
                      ofs['parts'], 0x80000000 if clr else 0, 1, 8, 0, 0, ofs.get('shape', 0), ofs['vtx'],
                      ofs['nrm'], 3, 1, version)
    if hsize == 0x48:
        hdr += struct.pack(f'{e}II', ofs.get('blend', 0), ofs.get('flip', 0))
    return _pad(hdr) + body


class ModelTest(unittest.TestCase):
    def test_roundtrip_new_version(self):
        be, le = build('>'), build('<')
        out = model.convert(be)
        self.assertEqual(len(out), len(be))
        # every field is a pure byte swap of the BE build except the u8/byte data, so LE build == output
        self.assertEqual(out, le)
        self.assertEqual(model.check(out, be), [])

    def test_roundtrip_old_version_0x40_header(self):
        be, le = build('>', version=0x20010801, tables=False), build('<', version=0x20010801, tables=False)
        self.assertEqual(model.convert(be), le)

    def test_no_colour(self):
        be, le = build('>', clr=False, shape=False), build('<', clr=False, shape=False)
        self.assertEqual(model.convert(be), le)

    def test_index_out_of_range(self):
        with self.assertRaises(ConvertError):
            model.convert(build('>', bad_index=True))

    def test_bad_opcode(self):
        be = bytearray(build('>'))
        h = model.Header(be)
        be[h.pParts + 0x20] = 0x61   # BP load: not model data
        with self.assertRaises(ConvertError):
            model.convert(bytes(be))

    def test_not_a_model(self):
        with self.assertRaises(NotThisFormat):
            model.convert(b'\x0b' + bytes(0x7F))

    def test_check_detects_change(self):
        be = build('>')
        out = bytearray(model.convert(be))
        h = model.Header(be)
        out[h.vtxOrig] ^= 1
        self.assertNotEqual(model.check(bytes(out), be), [])


if __name__ == '__main__':
    unittest.main()
