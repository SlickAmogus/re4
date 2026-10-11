"""TPL / ConvertedImage tests on synthetic textures (no game data)."""
import os
import struct
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from re4conv import ConvertError, tpl  # noqa: E402


def make_tpl(texs):
    """texs: [(fmt, w, h, data bytes, clut (tlut_fmt, entries bytes) or None, maxlod)] -> BE TPL."""
    n = len(texs)
    dofs = 0x0C
    p = dofs + 8 * n
    th_ofs, cl_ofs = [], []
    for t in texs:
        th_ofs.append(p)
        p += 0x24
    for t in texs:
        if t[4]:
            cl_ofs.append(p)
            p += 0x0C
        else:
            cl_ofs.append(0)
    clut_data = []
    for t in texs:
        if t[4]:
            clut_data.append(p)
            p += len(t[4][1])
        else:
            clut_data.append(0)
    p = (p + 0x1F) & ~0x1F
    data_ofs = []
    for t in texs:
        data_ofs.append(p)
        p += (len(t[3]) + 0x1F) & ~0x1F
    out = bytearray(p)
    struct.pack_into('>III', out, 0, tpl.TPL_VERSION, n, dofs)
    for i, (fmt, w, h, data, clut, maxlod) in enumerate(texs):
        struct.pack_into('>II', out, dofs + 8 * i, th_ofs[i], cl_ofs[i])
        struct.pack_into('>HHIIIIIIfBBBB', out, th_ofs[i], h, w, fmt, data_ofs[i], 1, 1, 1, 1, 0.0, 0, 0, maxlod, 0)
        if clut:
            struct.pack_into('>HBBII', out, cl_ofs[i], len(clut[1]) // 2, 0, 0, clut[0], clut_data[i])
            out[clut_data[i]:clut_data[i] + len(clut[1])] = clut[1]
        out[data_ofs[i]:data_ofs[i] + len(data)] = data
    return bytes(out)


class SwizzleTest(unittest.TestCase):
    def test_square_morton(self):
        idx = tpl.swizzle_index(4, 4)
        self.assertEqual(idx[0].tolist(), [0, 1, 4, 5])
        self.assertEqual(idx[1].tolist(), [2, 3, 6, 7])
        self.assertEqual(idx[3, 3], 15)

    def test_wide(self):
        # 8x2: x0 y0 x1 x2 -> (x=2, y=0) = 4, (x=0, y=1) = 2
        idx = tpl.swizzle_index(8, 2)
        self.assertEqual(idx[0].tolist(), [0, 1, 4, 5, 8, 9, 12, 13])
        self.assertEqual(idx[1, 0], 2)
        self.assertEqual(sorted(idx.reshape(-1).tolist()), list(range(16)))

    def test_inverse(self):
        img = np.arange(16 * 8 * 4, dtype=np.uint8).reshape(8, 16, 4)
        self.assertTrue(np.array_equal(tpl.unswizzle(tpl.swizzle(img), 16, 8), img))


class DecodeTest(unittest.TestCase):
    def test_rgb5a3(self):
        v = np.array([0xFFFF, 0x7000 | 0xF00, 0x0000], np.uint32)
        px = tpl._rgb5a3(v).tolist()
        self.assertEqual(px[0], [255, 255, 255, 255])
        self.assertEqual(px[1], [255, 0, 0, 255])        # a3 = 7 -> 255, r4 = 0xF
        self.assertEqual(px[2], [0, 0, 0, 0])

    def test_rgba8_tile(self):
        # one 4x4 tile: AR pairs then GB pairs; texel k: A=k, R=16+k, G=32+k, B=48+k
        ar = bytes(b for k in range(16) for b in (k, 16 + k))
        gb = bytes(b for k in range(16) for b in (32 + k, 48 + k))
        img = tpl.decode(tpl.RGBA8, 4, 4, ar + gb)
        self.assertEqual(img[0, 1].tolist(), [17, 33, 49, 1])
        self.assertEqual(img[3, 3].tolist(), [31, 47, 63, 15])

    def test_i8_ia4_tiles(self):
        data = bytes(range(32))       # 8x4 tile
        img = tpl.decode(tpl.I8, 8, 4, data)
        self.assertEqual(img[1, 0].tolist(), [8, 8, 8, 8])
        img = tpl.decode(tpl.IA4, 8, 4, bytes([0x3C] * 32))
        self.assertEqual(img[0, 0].tolist(), [0xCC, 0xCC, 0xCC, 0x33])

    def test_ci8_clut(self):
        clut = struct.pack('>4H', 0xF800, 0x07E0, 0x001F, 0xFFFF)     # RGB565 red green blue white
        pal = tpl.tlut_rgba(1, clut)
        img = tpl.decode(tpl.CI8, 8, 4, bytes([0, 1, 2, 3] * 8), pal)
        self.assertEqual(img[0, :4, :3].tolist(), [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]])
        with self.assertRaises(ConvertError):
            tpl.decode(tpl.CI8, 8, 4, bytes([4] * 32), pal)

    def test_ci4(self):
        pal = tpl.tlut_rgba(0, struct.pack('>2H', 0xFF10, 0x8020))   # IA8: alpha hi, intensity lo
        img = tpl.decode(tpl.CI4, 8, 8, bytes([0x01] * 32), pal)
        self.assertEqual(img[0, 0].tolist(), [0x10, 0x10, 0x10, 0xFF])
        self.assertEqual(img[0, 1].tolist(), [0x20, 0x20, 0x20, 0x80])


class DxtTest(unittest.TestCase):
    def test_block_bytes(self):
        # 8x8 CMPR = 4 blocks TL TR BL BR; 8x8 DXT1 raster = TL TR BL BR as well, check one block's bytes
        blk = bytes([0xF8, 0x00, 0x00, 0x1F, 0b00011011, 0, 0xFF, 0x55])
        data = blk + bytes(24)
        out = tpl.cmpr_to_dxt1(8, 8, data)
        self.assertEqual(out[:8], bytes([0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0xFF, 0x55]))

    def test_raster_order(self):
        # 16x8 = two tiles; tile 1's TL block must land at raster block 2 (row 0, x 2)
        blocks = [bytes([i] * 8) for i in range(8)]
        out = tpl.cmpr_to_dxt1(16, 8, b''.join(blocks))
        order = [out[8 * k] for k in range(8)]
        self.assertEqual(order, [0, 1, 4, 5, 2, 3, 6, 7])


class TplTest(unittest.TestCase):
    def _rt(self, texs):
        d = make_tpl(texs)
        out = tpl.convert(d)
        self.assertEqual(tpl.check(out, d), [])
        return d, out

    def test_formats(self):
        rng = np.random.default_rng(1)
        texs = []
        for fmt, w, h in ((tpl.I4, 8, 8), (tpl.I8, 16, 4), (tpl.IA4, 8, 8), (tpl.IA8, 4, 4), (tpl.RGB565, 8, 8),
                          (tpl.RGB5A3, 8, 4), (tpl.RGBA8, 4, 8), (tpl.CMPR, 16, 16), (tpl.I8, 24, 4)):
            texs.append((fmt, w, h, rng.integers(0, 256, tpl.level_size(fmt, w, h), dtype=np.uint8).tobytes(), None, 0))
        clut = struct.pack('>16H', *range(0x8000, 0x8010))
        texs.append((tpl.CI4, 8, 8, rng.integers(0, 256, 32, dtype=np.uint8).tobytes(), (2, clut), 0))
        d, out = self._rt(texs)
        _, ts = tpl.parse(out, '<')
        fmts = [tpl.IMG_HDR.unpack_from(out, t['data'])[3] for t in ts]
        self.assertEqual(fmts[7], tpl.L_DXT1)
        self.assertEqual(fmts[8], tpl.LU_A8R8G8B8)     # 24 wide: not a power of two -> linear
        self.assertEqual(fmts[0], tpl.SZ_A8R8G8B8)
        # CLUT data kept little-endian at its offset
        self.assertEqual(struct.unpack_from('<H', out, ts[-1]['clut_hdr'][4])[0], 0x8000)

    def test_mips(self):
        lv = [tpl.level_size(tpl.RGB565, 8 >> k, 8 >> k) for k in range(4)]
        data = bytes(range(256)) * ((sum(lv) // 256) + 1)
        d, out = self._rt([(tpl.RGB565, 8, 8, data[:sum(lv)], None, 3)])
        _, ts = tpl.parse(out, '<')
        nv, w, h, mips, imgs = tpl.decode_image(out[ts[0]['data']:])
        self.assertEqual((w, h, mips), (8, 8, 4))
        self.assertEqual([i.shape[:2] for i in imgs], [(8, 8), (4, 4), (2, 2), (1, 1)])

    def test_bad_version(self):
        with self.assertRaises(ConvertError):
            tpl.convert(b'\0' * 0x40)


if __name__ == '__main__':
    unittest.main()
