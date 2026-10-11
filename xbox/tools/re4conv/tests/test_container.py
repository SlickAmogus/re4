"""Containers and simple tables on synthetic data, plus an optional real-data smoke test
(skipped unless C:/Claude/RE4-Xbox/gamedata/disc1/files or $RE4_DISC1 exists)."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import container, simple  # noqa: E402
import drs  # noqa: E402
import relfile  # noqa: E402

from re4conv.tests.test_model import build as build_model  # noqa: E402


def _pad(b, fill=b'\0'):
    return b + fill * ((-len(b)) % 0x20)


def seq(e='>'):
    return _pad(struct.pack(f'{e}HBB', 2, 1, 0) + struct.pack(f'{e}HBBHBB', 10, 3, 0, 20, 4, 0), b'\xcd')


def body(entries, rel=None):
    count = len(entries)
    hdr_end = 16 + 8 * count
    off = (hdr_end + 0x1F) & ~0x1F
    ofs = []
    for _, d in entries:
        ofs.append(off)
        off += len(d)
    out = struct.pack('>4I', count, off if rel else 0, 0, 0) + struct.pack(f'>{count}I', *ofs)
    out += b''.join(t for t, _ in entries)
    out = _pad(out) + b''.join(d for _, d in entries)
    if rel:
        out = _pad(out + rel, b'\xcd')
    return out


def tiny_rel():
    """Smallest thing drs.rel_size accepts: imp table at 0x40 with one entry -> reloc list at 0x48 ending at once."""
    r = bytearray(0x50)
    struct.pack_into('>2I', r, 0x28, 0x40, 8)
    struct.pack_into('>2I', r, 0x40, 0, 0x48)
    r[0x48 + 2] = relfile.R_DOLPHIN_END
    return bytes(r)


def hakase(b):
    sig = 'ﾊｶｾ '.encode('shift_jis') * 8
    recs = [(0, len(b), 0, 0x400, 0, 0, 0, 0), (drs.EMPTY, 0, 0, 0x400 + len(b), 0, 0, 0, 0)]
    return bytes(drs.write_records(sig, recs)) + b


class ContainerTest(unittest.TestCase):
    def test_hakase_roundtrip(self):
        ents = [(b'SEQ\0', seq()), (b'BIN\0', build_model('>')), (b'XYZ\0', _pad(b'\x12\x34raw')), (b'\0' * 4, b'')]
        d = hakase(body(ents, rel=tiny_rel()))
        out, info = container.convert_hakase(d)
        self.assertEqual(info['rel_dropped'], 0x50)
        self.assertEqual([r['status'] for r in info['entries']], ['converted', 'converted', 'passthrough', 'empty'])
        _, recs, got, _ = container.parse_hakase_le(out)
        self.assertEqual(recs[0][:4], (0, recs[0][1], 0, 0x400))
        self.assertEqual(struct.unpack_from('<I', out, 0x400 + 4)[0], 0, 'rel_offset must be 0')
        self.assertEqual(got[0][1], seq('<'))
        self.assertEqual(got[1][1], build_model('<'))
        self.assertEqual(got[2][1], _pad(b'\x12\x34raw'))
        self.assertEqual(container.check_hakase(out, d, info), [])

    def test_failed_entry_is_copied(self):
        bad = build_model('>', bad_index=True)
        notseq = _pad(b'\0\x02\x01\x07' + b'\xff' * 8)     # "SEQ" that is not a SEQUENCE_FILE (op/*.das)
        d = hakase(body([(b'BIN\0', bad), (b'SEQ\0', notseq)]))
        out, info = container.convert_hakase(d)
        self.assertEqual([r['status'] for r in info['entries']], ['failed', 'passthrough'])
        got = container.parse_hakase_le(out)[2]
        self.assertEqual((got[0][1], got[1][1]), (bad, notseq))

    def test_bare_body(self):
        d = body([(b'SEQ\0', seq())])
        out, info = container.convert_body(d)
        self.assertEqual(container.parse_body_le(out)[0][1], seq('<'))
        self.assertEqual(container.check_body(out, d, info), [])

    def test_event(self):
        bins = [('event/x/a.seq', seq()), ('event/x/b.dat', b'\1\2\3\4')]
        n = len(bins)
        hdr = bytearray(0x50)
        hdr[0:0x20] = b'event/evd/r100s00.evd'.ljust(0x20, b'\0')
        hdr[0x20:0x24] = b'r100'
        hdr[0x28:0x2B] = b's00'
        struct.pack_into('>iIIIIiI', hdr, 0x34, -0x80000000, 0, 0, 0x50, 0x10, n, 0x60)
        d = bytearray(hdr) + b'\xAA' * 0x10   # packets
        d = _pad(bytes(d))
        tbl = 0x60
        d = bytearray(d) + bytes(0x40 * n)
        for i, (name, b) in enumerate(bins):
            d = bytearray(_pad(bytes(d)))
            e = tbl + 0x40 * i
            d[e:e + len(name)] = name.encode()
            struct.pack_into('>II', d, e + 0x30, len(d), len(b))
            d += b
        d = bytes(d)
        out, info = container.convert_event(d)
        got = dict(container.parse_event_le(out))
        self.assertEqual(got['event/x/a.seq'], seq('<'))
        self.assertEqual(got['event/x/b.dat'], b'\1\2\3\4')
        self.assertEqual(struct.unpack_from('<i', out, 0x34)[0], -0x80000000)
        self.assertEqual(out[0x50:0x60], b'\xAA' * 0x10)
        self.assertEqual(container.check_event(out, d, info), [])

    def test_etm(self):
        files = [('pl00017.seq', seq()), ('x.raw', _pad(b'abc'))]
        d = bytearray(struct.pack('>I', len(files)) + bytes(0x1C))
        for name, b in files:
            rec = bytearray(0x40)
            struct.pack_into('>I', rec, 0, 0x40 + len(b))
            rec[0x20:0x20 + len(name)] = name.encode()
            d += rec + b
        out, info = container.convert_etm(bytes(d))
        got = dict(container.parse_etm_le(out))
        self.assertEqual(got['pl00017.seq'], seq('<'))
        self.assertEqual(info['status'], 'partial')


class SimpleTest(unittest.TestCase):
    def test_esl(self):
        rec = ('>', (1, 2, 3, 4, 0x11223344, 500, 0, 7, -1, 2, 3, 100, -200, 300, 0x0105, 1000, b'\0' * 4))
        be = simple.ESL.pack(*rec[1]) * 2
        self.assertEqual(simple.esl_convert(be), simple.ESL_LE.pack(*rec[1]) * 2)

    def test_sizetbl(self):
        be = struct.pack('>I', 1) + struct.pack('>8sII', b'em06', 0x6ED80, 0)
        out = simple.sizetbl_convert(be)
        self.assertEqual(out, struct.pack('<I', 1) + struct.pack('<8sII', b'em06', 0x6ED80, 0))
        self.assertEqual(simple.sizetbl_check(out, be), [])

    def test_bgmtbl(self):
        def mk(e):
            room = struct.pack(f'{e}I', 1) + struct.pack(f'{e}13I', *range(13))
            ofs = struct.pack(f'{e}2I', 8, 8 + len(room))
            rooms = ofs + room + room
            list_ofs = 0x20 + len(rooms)
            return (struct.pack(f'{e}II', 0x20, list_ofs) + bytes(0x18) + rooms +
                    struct.pack(f'{e}3H', 0x100, 0x101, 0xFFFF) + bytes(2))
        out = simple.bgmtbl_convert(mk('>'))
        self.assertEqual(out, mk('<'))
        self.assertEqual(simple.bgmtbl_check(out, mk('>')), [])


DISC1 = os.environ.get('RE4_DISC1', r'C:/Claude/RE4-Xbox/gamedata/disc1/files')


@unittest.skipUnless(os.path.isfile(os.path.join(DISC1, 'em', 'pl00.drs')), 'retail disc data not present')
class RealDataTest(unittest.TestCase):
    def test_pl00(self):
        with open(os.path.join(DISC1, 'em', 'pl00.drs'), 'rb') as f:
            d = f.read()
        out, info = container.convert_hakase(d)
        st = [r['status'] for r in info['entries']]
        self.assertNotIn('failed', st)
        self.assertEqual(container.check_hakase(out, d, info), [])


if __name__ == '__main__':
    unittest.main()
