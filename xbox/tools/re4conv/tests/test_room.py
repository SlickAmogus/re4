"""re4conv.room / events on synthetic structures: each builder writes the same records in either byte
order, and convert(big-endian) must equal the little-endian build (plus check() must pass and catch a
corrupted field). A real-data smoke test runs when C:/Claude/RE4-Xbox/gamedata/disc1/files exists."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import ConvertError, events, room  # noqa: E402

DISC1 = os.environ.get('RE4_DISC1', 'C:/Claude/RE4-Xbox/gamedata/disc1/files')


def P(e, fmt, *v):
    return struct.pack(e + fmt, *v)


def pad(b, a=0x20, fill=b'\0'):
    return b + fill * ((-len(b)) % a)


def area(e):
    return P(e, 'BBH11f', 1, 2, 0, *[float(i) for i in range(11)])


def blk(e):
    hdr = P(e, '4sHBBHH3I', b'BLK\0', 0x100, 0, 2, 1, 1, 0x18, 0x30, 0x68)
    link = P(e, 'B3s8b', 1, b'\0' * 3, 1, -1, -1, -1, -1, -1, -1, -1) * 2
    ar = P(e, 'I', 0) + area(e) + P(e, 'BBbB', 1, 0, 3, 7)
    conn = P(e, 'BB2s16b', 1, 0, b'\0\0', *([0, 1] + [-1] * 14))
    return hdr + link + ar + conn


def lit(e):
    env = (P(e, '4sI', b'\x10\x20\x30\xff', 2) + P(e, 'I2f4s', 2, 100.0, 5000.0, b'\x01\x02\x03\x04') * 2 +
           P(e, 'i', 0x1234) + bytes(4) + bytes(4) + bytes(12) + bytes([2, 2, 0, 0]) + P(e, 'f', 0.5) +
           bytes(164) + bytes(4) + bytes(8) + P(e, 'f', -0.25) + bytes(8))
    assert len(env) == 0x104

    def work(lid, sub):
        return (bytes([1, 3, lid, 0x41]) + P(e, '3ff', 1.0, 2.0, 3.0, 400.0) + b'\xff\x80\x40\xff' + P(e, 'f', 1.5) +
                bytes([1, 0, 0, 3]) + P(e, 'IHHI', 0x10002, 50, 0, 0) + P(e, '16I', *range(16)) + sub + bytes(0x40))
    w2 = work(2, P(e, '4f', 1.0, 2.0, 3.0, 4.0) + bytes(112))
    w4 = work(4, P(e, 'HBBhh', 7, 1, 0xFF, -30, 45) + bytes(4) + P(e, '3f', 1.0, 2.0, 3.0) + bytes([5]) + bytes(103))
    return P(e, 'HBB', 1, 0x2C, 8) + P(e, 'I', 8) + env + w2 + w4


def cam(e):
    hdr = b'B404' + bytes([1, 1, 1]) + bytes(9)
    cut = bytes([3]) + bytes(7) + P(e, 'II', 0x20, 0x50)
    ar = P(e, 'BbbBfBB22sffiI', 1, 0, 0, 8, 1.5, 1, 0xFF, bytes(22), 100.0, 0.0, 2, 0x94)
    cd = P(e, 'BbbB3fIf8siIIII', 1, 1, 6, 1, 1.0, 2.0, 3.0, 0xAC, 0.5, bytes(8), 2, 0xB0, 0xC8, 0xE0, 0xE8)
    lerp = P(e, 'B4bB2si4s', 1, 0, 0, 1, 0, 0, b'\0\0', 30, bytes(4))
    ver = P(e, '6f', *[float(i) for i in range(6)])
    frames = P(e, '2H', 0, 30)
    pos = P(e, '6f', *[float(i) for i in range(6)])
    tgt = P(e, '6f', *[float(-i) for i in range(6)])
    roll = P(e, '2f', 0.0, 0.1)
    fovy = P(e, '2f', 45.0, 50.0)
    out = hdr + cut + ar + cd + lerp + ver + frames + pos + tgt + roll + fovy
    assert len(out) == 0xF0, hex(len(out))
    return out


def fse(e):
    return (P(e, '4sHHBBHI', b'FSE\0', 0x103, 2, 1, 0, 0, 0) +
            bytes([1, 0, 0, 0, 15]) + bytes(15) + area(e) + bytes([1, 2, 3, 4]) + bytes(60) +
            bytes([1, 2, 1, 0, 15]) + bytes(15) + area(e) + P(e, 'BBbb2iHHI44s', 1, 1, 100, 50, 30, 60, 0, 7, 90,
                                                              bytes(44)))


def sce(e, kind=b'AEV'):
    def unit(rid):
        return (P(e, 'I', 0) + area(e) + bytes([1, rid, 2, 0, 1, 1, 0, 0]) + P(e, 'II', 0, 0) + bytes([3, 0, 0, 0]) +
                bytes([10, 20, 0, 0]) + P(e, 'Ih', 0, -1) + bytes([0, 0]) + bytes(8))
    if kind == b'ITA':
        return (P(e, '4sHH8s', b'ITA\0', 0x105, 1, bytes(8)) + unit(3) +
                P(e, '3fI3f4H6Bh4f2B2s', 1.0, 2.0, 3.0, 0, 0.0, 10.0, 0.0, 0x2E, 5, 1, 0, 1, 0, 0, 0, 0, 0, -1,
                  100.0, 0.0, 1.0, 0.0, 3, 4, b'\0\0'))
    return (P(e, '4sHH8s', b'AEV\0', 0x104, 2, bytes(8)) +
            unit(1) + P(e, '3ffBBBBIbBBBI32s', 1.0, 2.0, 3.0, 1.57, 1, 0x20, 0, 0, 0, -1, 2, 0, 0, 0, bytes(32)) +
            unit(5) + P(e, 'hhBBHB3s52s', 0, 12, 1, 0, 0x100, 4, bytes(3), bytes(52)))


def stb(e):
    rev = P(e, '4H6f', 1, 2, 3, 4, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6)
    tbl = [0] * 128
    tbl[0] = 0x240           # combine table
    tbl[32] = 0x260          # vol curve
    tbl[64] = 0x260          # pitch shares it
    body = bytes([1, 2, 3, 4, 5, 6, 7, 0]) + bytes(24) + P(e, 'If', 2, 1.0) + P(e, 'fHH', 0.0, 0, 100) * 2
    return rev + rev + P(e, '128I', *tbl) + body


def sat(e):
    poly = P(e, '8HI', 0, 1, 2, 0, 0, 1, 2, 0, 0x12345678)
    child = P(e, '6f4HI', 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0) + P(e, 'H', 0)
    root = P(e, '6f4HI', 0, 0, 0, 2, 2, 2, 0, 0, 0, 1, 0) + child
    f = P(e, 'BB9H', 0x20, 0, 3, 1, 3, 0, 1, 1, 0, 0, 2) + P(e, '21f', *[float(i) for i in range(21)]) + poly + root
    f = pad(f, 4)
    return bytes([0x80, 1, 0x12, 0x34]) + P(e, 'I', 8) + f


def mdt(e):
    def block(msgs):
        n = len(msgs)
        ofs, p = [], 8 + 4 * n
        for m in msgs:
            ofs.append(p)
            p += 2 * len(m)
        return P(e, 'II', 0xABCD, n) + P(e, f'{n}I', *ofs) + b''.join(P(e, f'{len(m)}H', *m) for m in msgs)
    b0 = pad(block([[0, 0xC1, 0x80, 0x0008, 1, 0], [3, 0x41]]), 4)
    b1 = pad(block([[0x10, 0x11]]), 4)
    return P(e, '3I', 2, 12, 12 + len(b0)) + b0 + b1


def uwf(e):
    unit = (bytes(0x14) + P(e, '3f12fff', *[float(i) for i in range(17)]) + b'\x01\x02\x03\x04' + b'\xff\xee\xdd\xcc' +
            P(e, '3f', 0.0, 1.0, 0.0) + bytes([1, 2, 3, 4, 5]) + bytes(3) + P(e, '6I', 0x94, 0, 0xB4, 0, 0, 0))
    path = P(e, 'b6sb', 3, bytes(6), 2) + P(e, '6f', *[1.0] * 6)
    curve = P(e, 'i', 1) + P(e, '4f', 0.0, 1.0, 0.5, 0.5)
    return b'2.00' + bytes([1, 1, 0, 0]) + unit + path + curve


def vib(e):
    return P(e, '3I', 2, 12, 0) + P(e, 'I', 2) + P(e, '3H2B', 1, 0, 10, 255, 0) * 2


BUILDERS = {
    'BLK': blk, 'LIT': lit, 'CAM': cam, 'FSE': fse, 'AEV': sce, 'ITA': lambda e: sce(e, b'ITA'), 'STB': stb,
    'SAT': sat, 'MDT': mdt, 'UWF': uwf, 'VIB': vib,
    'SAR': lambda e: P(e, 'I', 1) + bytes(12) + bytes([0, 0, 1, 2]) + area(e) + P(e, 'IbB158s', 0, 50, 3, bytes(158)),
    'EAR': lambda e: P(e, 'II', 1, 3) + bytes(8) + bytes([0, 1, 2, 0]) + area(e) + P(e, 'I96s', 1, bytes(96)),
    'ETS': lambda e: P(e, 'H', 1) + bytes(14) + P(e, 'HH12s6f', 0x12, 0x0103, bytes(12), 0, 1, 0, 100, 0, -50),
    'ESE': lambda e: P(e, '4sHH8s', b'ESE\0', 0x100, 1, bytes(8)) + P(e, 'BBH3f7HhHH8s', 1, 0, 1, 1.0, 2.0, 3.0,
                                                                     0, 6, 0, 30, 60, 0, 0, -1, 30, 10, bytes(8)),
    'DSE': lambda e: P(e, 'I', 1) + P(e, '6H', 0x101, 1, 2, 3, 4, 5),
    'CNS': lambda e: P(e, '4I', 2, 0x3, 100, 200),
    'EMI': lambda e: P(e, 'i', 1) + bytes(4) + bytes([6, 0, 0, 0]) + P(e, '3ff', 1.0, 2.0, 3.0, 1.57) + bytes(44),
    'RTP': lambda e: P(e, '4s4H3I', b'2RTP', 1, 2, 1, 4, 0x18, 0x38, 0x3C) + P(e, '3fHH', 0, 0, 0, 0, 1) +
    P(e, '3fHH', 1, 0, 1, 1, 0) + P(e, 'hH', 1, 50) + bytes([0, 1, 0, 0]),
    'SMX': lambda e: bytes([0, 1]) + bytes(14) + bytes([0, 1, 2, 0]) + P(e, 'II', 0xFFFF, 4) + b'\x80\x80\x80\xff' +
    P(e, '3fB3s25I', 0.0, 0.01, 0.0, 1, bytes(3), *range(25)) + b'\x10\x20\x30\x40' + P(e, 'ff', 0.5, 0.0),
    'FCS': lambda e: P(e, '516I', *range(516)) + bytes(0x10),
}


class RoomFormatsTest(unittest.TestCase):
    def test_every_builder(self):
        for tag, b in BUILDERS.items():
            with self.subTest(tag=tag):
                be, le = b('>'), b('<')
                out = room.convert(tag, be)
                self.assertEqual(out, le)
                self.assertEqual(room.check(tag, out, be), [])

    def test_check_catches_corruption(self):
        be = lit('>')
        out = bytearray(room.convert('LIT', be))
        out[0x08 + 0x104 + 0x04] ^= 0x01        # first light's Pos.x
        self.assertNotEqual(room.check('LIT', bytes(out), be), [])

    def test_unknown_bytes_and_versions_refused(self):
        b = bytearray(blk('>'))
        b[0x16] = 0                 # move ofsConnect: the connect record now overlaps nothing known
        b[0x17] = 0x80
        with self.assertRaises(ConvertError):
            room.convert('BLK', bytes(b) + bytes(0x40) + b'\x01')
        with self.assertRaises(ConvertError):
            room.convert('CAM', b'B399' + bytes(12))
        with self.assertRaises(ConvertError):
            room.convert('FSE', P('>', '4sHH', b'FSE\0', 0x102, 0) + bytes(8))

    def test_empty_entries_copied(self):
        for d in (bytes(0x20), b'\xcd' * 0x20):
            self.assertEqual(room.convert('FSE', d), d)


def packets(e):
    def pkt(pid, body, cut=0, frame=0):
        size = 0x10 + len(body)
        return P(e, 'iIhhhh', pid, 0x80000000 if pid == 9 else 0, cut, frame, size, 0) + body
    return (pkt(0, b'') + pkt(6, b'cam000\0\0\0\0\0\0' + bytes(0x64)) +
            pkt(9, b'pl0000\0\0\0\0\0\0' + b'oya0000\0\0\0\0\0' + P(e, '7i', 100, -200, 300, 0, 90, 0, -1)) +
            pkt(13, b'esp\0' + bytes(8) + P(e, 'i', 5) + bytes([0, 0, 0, 3]), 1, 20) +
            pkt(15, P(e, 'iii', 0, 12, 0)) + pkt(8, P(e, 'iii', 120, 0, 0)) + pkt(27, b''))


class EventPacketTest(unittest.TestCase):
    def test_packets(self):
        be, le = packets('>'), packets('<')
        sp, n = events.packet_layout(be, 0, len(be))
        self.assertEqual(n, 7)
        self.assertEqual(events.convert_packets(be, 0, len(be)), le)

    def test_bad_stream(self):
        be = bytearray(packets('>'))
        struct.pack_into('>h', be, 0x0C, 4)         # size < 0x10
        with self.assertRaises(ConvertError):
            events.packet_layout(bytes(be), 0, len(be))


@unittest.skipUnless(os.path.isdir(DISC1), 'no game data')
class RealRoomTest(unittest.TestCase):
    def test_core_tables(self):
        from re4conv import container
        with open(os.path.join(DISC1, 'etc', 'core.das'), 'rb') as f:
            d = f.read()
        a, _ = container.open_hakase(d)
        import drs
        n = 0
        for t4, b in a.entries:
            tag = drs.tag_name(t4)
            if tag in room.BUILDERS and b and tag != 'SAT':
                self.assertEqual(room.check(tag, room.convert(tag, b), b), [], tag)
                n += 1
        self.assertGreater(n, 5)


if __name__ == '__main__':
    unittest.main()
