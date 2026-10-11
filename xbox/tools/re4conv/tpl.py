"""TPL texture palettes -> little-endian headers + ConvertedImage blobs (xbox/docs/04-DATA.md section 5).

Source layout (include/tpl.h, all big-endian, offsets relative to the palette, relocated by
calcTplAddr / CalcTplAddrC8 trans.cpp:2563):
  TEXPalette    {u32 version 0x0020AF30; u32 numDescriptors; u32 descriptorArray}
  TEXDescriptor {u32 textureHeader; u32 CLUTHeader (0 = none)}
  TEXHeader     0x24: u16 height, width; u32 format, data, wrapS, wrapT, minFilter, magFilter; f32 LODBias;
                u8 edgeLODEnable, minLOD, maxLOD, unpacked
  CLUTHeader    0x0C: u16 numEntries; u8 unpacked, pad; u32 format (0 IA8, 1 RGB565, 2 RGB5A3); u32 data

Output layout: the palette header, descriptors, TEXHeaders, CLUTHeaders and CLUT data stay at their
offsets (multi-byte fields swapped, CLUT entries swapped as u16 so the port's GXInitTlutObj on the thermal
palette, SpecularInit trans.cpp:2621, still sees the right colours). The file is cut at the first
texel-data offset and one `ConvertedImage` per texture is appended, 0x20-aligned; TEXHeader.data is
rewritten to point at that blob (the shim recognises the 'RE4X' magic in GXInitTexObj). The GX format
field is kept, so game code that inspects it is unaffected.

ConvertedImage (gx_shim.h): u32 magic 'RE4X' (0x58344552 LE), u16 width, height, u8 nv2a_format,
mip_count, swizzled, pad, u32 data_size, then the levels back to back.
  CMPR, w and h multiples of 4 -> NV2A L_DXT1_A1R5G5B5 (0x0C), not swizzled: GC 8x8 tiles of 2x2
      blocks re-ordered into a block raster, colour words little-endian, 2-bit index order reversed in
      every row byte (GC: left texel in the high bits; DXT1: in the low bits). Note the GC decodes the
      4-colour mode with 5/8-3/8 weights, DXT1 hardware with 2/3-1/3 (sub-LSB difference, accepted).
  everything else -> A8R8G8B8 (bytes B, G, R, A): power-of-two sizes SZ_A8R8G8B8 (0x06) Morton-swizzled
      (NV2A order: bits interleaved x first while both dimensions have bits, as xemu's
      generate_swizzle_masks), other sizes LU_IMAGE_A8R8G8B8 (0x12) linear, swizzled = 0.
  CI4 / CI8 / CI14X2 are expanded through the descriptor's CLUT.
Mip levels: maxLOD + 1 when maxLOD > minLOD (none on the sampled discs), each level converted alike.

GX texel layouts (Dolphin TextureDecoder; tiles row-major, texels row-major within a tile):
  I4 8x8 4bpp (high nibble left), I8 8x4, IA4 8x4 (high nibble alpha), IA8 4x4 (byte A, byte I),
  RGB565 4x4, RGB5A3 4x4 (bit15: RGB555 opaque, else A3 RGB444), RGBA8 4x4 with 64-byte tiles
  (32 bytes A,R pairs then 32 bytes G,B pairs), CI4 8x8, CI8 8x4, CI14X2 4x4 (u16 & 0x3FFF), CMPR as gxtex.py.
"""
import struct

import numpy as np

import gxtex  # tools/motion (sys.path set by re4conv/__init__)

from . import ConvertError, NotThisFormat

TPL_VERSION = 0x0020AF30
MAGIC = 0x58344552
IMG_HDR = struct.Struct('<IHHBBBBI')
TEXHDR = struct.Struct('>HHIIIIIIfBBBB')
TEXHDR_LE = struct.Struct('<HHIIIIIIfBBBB')
CLUTHDR = struct.Struct('>HBBII')
CLUTHDR_LE = struct.Struct('<HBBII')

SZ_A8R8G8B8 = 0x06
L_DXT1 = 0x0C
LU_A8R8G8B8 = 0x12

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2, CMPR = 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14
FMT_NAMES = {I4: 'I4', I8: 'I8', IA4: 'IA4', IA8: 'IA8', RGB565: 'RGB565', RGB5A3: 'RGB5A3', RGBA8: 'RGBA8',
             CI4: 'CI4', CI8: 'CI8', CI14X2: 'CI14X2', CMPR: 'CMPR'}
# format -> (tile w, tile h, bytes per tile)
TILE = {I4: (8, 8, 32), I8: (8, 4, 32), IA4: (8, 4, 32), IA8: (4, 4, 32), RGB565: (4, 4, 32), RGB5A3: (4, 4, 32),
        RGBA8: (4, 4, 64), CI4: (8, 8, 32), CI8: (8, 4, 32), CI14X2: (4, 4, 32), CMPR: (8, 8, 32)}
CI_FORMATS = (CI4, CI8, CI14X2)


def level_size(fmt, w, h):
    if fmt not in TILE:
        raise ConvertError(f'TPL: unknown texture format {fmt}')
    tw, th, tb = TILE[fmt]
    return ((w + tw - 1) // tw) * ((h + th - 1) // th) * tb


def _levels(w, h, n):
    out = []
    for _ in range(n):
        out.append((w, h))
        w, h = max(1, w // 2), max(1, h // 2)
    return out


# ---- GX decoding to RGBA8 ---------------------------------------------------------------------------

def _tiles(px, fmt, w, h):
    tw, th, _ = TILE[fmt]
    return gxtex._tiles(px, w, h, tw, th, True)


def _u16(data):
    return np.frombuffer(data, '>u2').astype(np.uint32)


def _rgb5a3(v):
    v = v.astype(np.uint32)
    op = (v & 0x8000) != 0
    r = np.where(op, gxtex._c5((v >> 10) & 0x1F), ((v >> 8) & 0xF) * 0x11)
    g = np.where(op, gxtex._c5((v >> 5) & 0x1F), ((v >> 4) & 0xF) * 0x11)
    b = np.where(op, gxtex._c5(v & 0x1F), (v & 0xF) * 0x11)
    a3 = (v >> 12) & 7
    a = np.where(op, 255, (a3 << 5) | (a3 << 2) | (a3 >> 1))
    return np.stack([r, g, b, a], axis=-1).astype(np.uint8)


def tlut_rgba(tlut_fmt, data):
    """CLUT entries (big-endian u16) -> (n, 4) RGBA8."""
    v = _u16(data)
    if tlut_fmt == 0:     # IA8: high byte alpha, low byte intensity
        i, a = v & 0xFF, v >> 8
        return np.stack([i, i, i, a], axis=-1).astype(np.uint8)
    if tlut_fmt == 1:
        rgb = gxtex._rgb565(v)
        return np.concatenate([rgb, np.full((len(v), 1), 255, np.uint8)], axis=1)
    if tlut_fmt == 2:
        return _rgb5a3(v)
    raise ConvertError(f'TPL: unknown TLUT format {tlut_fmt}')


def decode(fmt, w, h, data: bytes, pal=None):
    """GX texture level -> RGBA8 (h, w, 4). pal: (n, 4) RGBA8 for the CI formats."""
    data = bytes(data[:level_size(fmt, w, h)])
    if fmt in (I4, IA8, CMPR):
        return gxtex.decode(fmt, w, h, data)
    b = np.frombuffer(data, np.uint8)
    if fmt == I8:
        return _tiles(np.repeat(b[:, None], 4, axis=1), fmt, w, h)
    if fmt == IA4:
        i, a = (b & 0xF) * 0x11, (b >> 4) * 0x11
        return _tiles(np.stack([i, i, i, a], axis=-1).astype(np.uint8), fmt, w, h)
    if fmt == RGB565:
        rgb = gxtex._rgb565(_u16(data))
        return _tiles(np.concatenate([rgb, np.full((len(rgb), 1), 255, np.uint8)], axis=1), fmt, w, h)
    if fmt == RGB5A3:
        return _tiles(_rgb5a3(_u16(data)), fmt, w, h)
    if fmt == RGBA8:
        t = b.reshape(-1, 2, 16, 2)       # tile, (AR | GB), texel, byte
        px = np.stack([t[:, 0, :, 1], t[:, 1, :, 0], t[:, 1, :, 1], t[:, 0, :, 0]], axis=-1).reshape(-1, 4)
        return _tiles(px, fmt, w, h)
    if fmt in CI_FORMATS:
        if pal is None:
            raise ConvertError(f'TPL: {FMT_NAMES[fmt]} texture without a CLUT')
        if fmt == CI4:
            idx = np.stack([b >> 4, b & 0xF], axis=-1).reshape(-1)
        elif fmt == CI8:
            idx = b
        else:
            idx = _u16(data) & 0x3FFF
        if idx.size and int(idx.max()) >= len(pal):
            raise ConvertError(f'TPL: CLUT index {int(idx.max())} >= {len(pal)} entries')
        return _tiles(pal[idx.astype(np.intp)], fmt, w, h)
    raise ConvertError(f'TPL: unknown texture format {fmt}')


# ---- NV2A encoding ----------------------------------------------------------------------------------

def is_pot(n):
    return n > 0 and n & (n - 1) == 0


def _swizzle_masks(w, h):
    mx = my = 0
    bit = mbit = 1
    while True:
        done = True
        if bit < w:
            mx |= mbit
            mbit <<= 1
            done = False
        if bit < h:
            my |= mbit
            mbit <<= 1
            done = False
        bit <<= 1
        if done:
            return mx, my


def _deposit(v, mask):
    """Scatter the low bits of v (array) into the set bits of mask (pdep)."""
    out = np.zeros_like(v)
    src = 0
    for i in range(mask.bit_length()):
        if mask >> i & 1:
            out |= ((v >> src) & 1) << i
            src += 1
    return out


def swizzle_index(w, h):
    """(h, w) array: the swizzled texel index of every (y, x)."""
    mx, my = _swizzle_masks(w, h)
    x = _deposit(np.arange(w, dtype=np.int64), mx)
    y = _deposit(np.arange(h, dtype=np.int64), my)
    return y[:, None] | x[None, :]


def swizzle(img):
    """(h, w, c) power-of-two image -> swizzled flat (h*w, c)."""
    h, w = img.shape[:2]
    out = np.empty((h * w,) + img.shape[2:], img.dtype)
    out[swizzle_index(w, h).reshape(-1)] = img.reshape((h * w,) + img.shape[2:])
    return out


def unswizzle(flat, w, h):
    return flat[swizzle_index(w, h).reshape(-1)].reshape((h, w) + flat.shape[1:])


def _rev2(b):
    """Reverses the four 2-bit fields of each byte."""
    return ((b & 0x03) << 6) | ((b & 0x0C) << 2) | ((b & 0x30) >> 2) | ((b & 0xC0) >> 6)


def cmpr_to_dxt1(w, h, data):
    tiles_x, tiles_y = (w + 7) // 8, (h + 7) // 8
    blk = np.frombuffer(bytes(data[:level_size(CMPR, w, h)]), np.uint8).reshape(tiles_y, tiles_x, 2, 2, 8)
    blk = blk.transpose(0, 2, 1, 3, 4).reshape(tiles_y * 2, tiles_x * 2, 8)[:h // 4, :w // 4].reshape(-1, 8)
    out = np.empty_like(blk)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = blk[:, 1], blk[:, 0], blk[:, 3], blk[:, 2]
    out[:, 4:] = _rev2(blk[:, 4:])
    return out.tobytes()


def dxt1_decode(w, h, data):
    """NV2A DXT1 level -> RGBA8 with the GameCube's blend (exact inverse check of cmpr_to_dxt1)."""
    b = np.frombuffer(data, np.uint8).reshape(-1, 8)
    gc = np.empty_like(b)
    gc[:, 0], gc[:, 1], gc[:, 2], gc[:, 3] = b[:, 1], b[:, 0], b[:, 3], b[:, 2]
    gc[:, 4:] = _rev2(b[:, 4:])
    bw, bh = w // 4, h // 4
    # raster blocks -> GC 8x8 tiles (pad to whole tiles) and reuse gxtex's decoder
    tx, ty = (bw + 1) // 2, (bh + 1) // 2
    grid = np.zeros((ty * 2, tx * 2, 8), np.uint8)
    grid[:bh, :bw] = gc.reshape(bh, bw, 8)
    tiles = grid.reshape(ty, 2, tx, 2, 8).transpose(0, 2, 1, 3, 4)
    return gxtex.decode(CMPR, w, h, tiles.tobytes())


def _bgra(img):
    return np.ascontiguousarray(img[..., [2, 1, 0, 3]])


def encode_image(fmt, w, h, levels_data, pal=None):
    """levels_data: GX bytes per level -> ConvertedImage blob."""
    lv = _levels(w, h, len(levels_data))
    if fmt == CMPR and w % 4 == 0 and h % 4 == 0 and all(a % 4 == 0 and b % 4 == 0 for a, b in lv):
        nv, sw = L_DXT1, 0
        body = b''.join(cmpr_to_dxt1(a, b, d) for (a, b), d in zip(lv, levels_data))
    else:
        pot = is_pot(w) and is_pot(h)
        nv, sw = (SZ_A8R8G8B8, 1) if pot else (LU_A8R8G8B8, 0)
        parts = []
        for (a, b), d in zip(lv, levels_data):
            img = _bgra(decode(fmt, a, b, d, pal))
            parts.append((swizzle(img) if pot else img).tobytes())
        body = b''.join(parts)
    return IMG_HDR.pack(MAGIC, w, h, nv, len(levels_data), sw, 0, len(body)) + body


def decode_image(blob):
    """ConvertedImage -> (fmt, w, h, mips, [RGBA8 (h, w, 4) per level])."""
    magic, w, h, nv, mips, sw, _, size = IMG_HDR.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ConvertError('ConvertedImage: bad magic')
    body = blob[IMG_HDR.size:IMG_HDR.size + size]
    if len(body) != size:
        raise ConvertError('ConvertedImage: truncated')
    out = []
    p = 0
    for a, b in _levels(w, h, mips):
        if nv == L_DXT1:
            n = (a // 4) * (b // 4) * 8
            out.append(dxt1_decode(a, b, body[p:p + n]))
        elif nv in (SZ_A8R8G8B8, LU_A8R8G8B8):
            n = a * b * 4
            flat = np.frombuffer(body[p:p + n], np.uint8).reshape(-1, 4)
            img = unswizzle(flat, a, b) if sw else flat.reshape(b, a, 4)
            out.append(img[..., [2, 1, 0, 3]])
        else:
            raise ConvertError(f'ConvertedImage: unknown NV2A format {nv:#x}')
        p += n
    if p != size:
        raise ConvertError(f'ConvertedImage: {size - p} trailing bytes')
    return nv, w, h, mips, out


# ---- TPL --------------------------------------------------------------------------------------------

def _align(n, a=0x20):
    return (n + a - 1) & ~(a - 1)


def parse(d: bytes, endian='>'):
    """Header-level decode: list of dicts per texture (header fields, clut header/data range)."""
    e = endian
    if len(d) < 12:
        raise ConvertError('TPL: too small')
    version, n, dofs = struct.unpack_from(f'{e}III', d, 0)
    if version != TPL_VERSION:
        raise NotThisFormat(f'TPL: version {version:#x} (not a texture palette, e.g. a 32-byte blank placeholder)')
    if n == 0 or n > 0x400 or dofs + 8 * n > len(d):
        raise ConvertError(f'TPL: {n} descriptors at {dofs:#x} do not fit')
    th_st = TEXHDR if e == '>' else TEXHDR_LE
    cl_st = CLUTHDR if e == '>' else CLUTHDR_LE
    texs = []
    for i in range(n):
        tho, clo = struct.unpack_from(f'{e}II', d, dofs + 8 * i)
        if tho + 0x24 > len(d):
            raise ConvertError(f'TPL: texture {i} header {tho:#x} outside')
        f = th_st.unpack_from(d, tho)
        if f[12]:
            raise ConvertError(f'TPL: texture {i} header is relocated (memory image)')
        t = {'th': tho, 'clut': clo, 'hdr': f, 'h': f[0], 'w': f[1], 'fmt': f[2], 'data': f[3],
             'minlod': f[10], 'maxlod': f[11]}
        if t['fmt'] not in TILE:
            raise ConvertError(f'TPL: texture {i} format {t["fmt"]}')
        if clo:
            if clo + 0x0C > len(d):
                raise ConvertError(f'TPL: texture {i} CLUT header {clo:#x} outside')
            c = cl_st.unpack_from(d, clo)
            if c[1]:
                raise ConvertError(f'TPL: texture {i} CLUT header is relocated')
            if c[4] + 2 * c[0] > len(d):
                raise ConvertError(f'TPL: texture {i} CLUT data outside')
            t['clut_hdr'] = c
        elif t['fmt'] in CI_FORMATS:
            raise ConvertError(f'TPL: texture {i} is {FMT_NAMES[t["fmt"]]} without a CLUT')
        texs.append(t)
    return dofs, texs


def _mips(t):
    return t['maxlod'] + 1 if t['maxlod'] > t['minlod'] else 1


def convert(d: bytes) -> bytes:
    dofs, texs = parse(d, '>')
    n = len(texs)
    lv_data = []
    for i, t in enumerate(texs):
        p = t['data']
        lvs = []
        for a, b in _levels(t['w'], t['h'], _mips(t)):
            sz = level_size(t['fmt'], a, b)
            if p + sz > len(d):
                raise ConvertError(f'TPL: texture {i} level data {p:#x}+{sz:#x} past the end ({len(d):#x})')
            lvs.append(d[p:p + sz])
            p += sz
        lv_data.append(lvs)
    cut = min(t['data'] for t in texs)
    meta_end = max([dofs + 8 * n] + [t['th'] + 0x24 for t in texs] +
                   [t['clut'] + 0x0C for t in texs if t['clut']] +
                   [t['clut_hdr'][4] + 2 * t['clut_hdr'][0] for t in texs if t['clut']])
    if meta_end > cut:
        raise ConvertError(f'TPL: headers/CLUTs ({meta_end:#x}) after the first texel data ({cut:#x})')
    o = bytearray(d[:cut])
    o[0:12] = struct.pack('<III', TPL_VERSION, n, dofs)
    for i, t in enumerate(texs):
        o[dofs + 8 * i:dofs + 8 * i + 8] = struct.pack('<II', t['th'], t['clut'])
        if t['clut']:
            c = t['clut_hdr']
            o[t['clut']:t['clut'] + 0x0C] = CLUTHDR_LE.pack(*c)
            v = np.frombuffer(d, '>u2', c[0], c[4]).astype('<u2')
            o[c[4]:c[4] + 2 * c[0]] = v.tobytes()
    blobs = {}
    for i, t in enumerate(texs):
        key = (t['data'], t['fmt'], t['w'], t['h'], t['clut'])
        if key not in blobs:   # shared texel data (same offset) -> one blob
            pal = None
            if t['clut']:
                c = t['clut_hdr']
                pal = tlut_rgba(c[3], d[c[4]:c[4] + 2 * c[0]])
            blob = encode_image(t['fmt'], t['w'], t['h'], lv_data[i], pal)
            o += b'\0' * (_align(len(o)) - len(o))
            blobs[key] = len(o)
            o += blob
        f = list(t['hdr'])
        f[3] = blobs[key]
        o[t['th']:t['th'] + 0x24] = TEXHDR_LE.pack(*f)
    o += b'\0' * (_align(len(o)) - len(o))
    out = bytes(o)
    probs = check(out, d)
    if probs:
        raise ConvertError(f'TPL: converted image fails its check: {probs[0]}')
    return out


def check(d: bytes, original: bytes = None) -> list:
    """Converted TPL: headers parse little-endian, every blob decodes; with the original, every level's
    pixels equal the GX decode of the original."""
    try:
        dofs, texs = parse(d, '<')
    except ConvertError as e:
        return [str(e)]
    probs = []
    orig = None
    if original is not None:
        try:
            odofs, orig = parse(original, '>')
        except ConvertError as e:
            return [f'TPL original: {e}']
        if len(orig) != len(texs) or odofs != dofs:
            return ['TPL: descriptor table differs from the original']
    for i, t in enumerate(texs):
        try:
            nv, w, h, mips, imgs = decode_image(d[t['data']:])
        except (ConvertError, ValueError) as e:
            probs.append(f'TPL texture {i}: {e}')
            continue
        if (w, h, mips) != (t['w'], t['h'], _mips(t)):
            probs.append(f'TPL texture {i}: blob {w}x{h}/{mips} vs header {t["w"]}x{t["h"]}/{_mips(t)}')
            continue
        if orig is None:
            continue
        ot = orig[i]
        if ot['hdr'][:3] != t['hdr'][:3] or ot['hdr'][4:] != t['hdr'][4:] or ot['clut'] != t['clut']:
            probs.append(f'TPL texture {i}: header fields differ from the original')
            continue
        pal = None
        if ot['clut']:
            c = ot['clut_hdr']
            pal = tlut_rgba(c[3], original[c[4]:c[4] + 2 * c[0]])
            if np.frombuffer(d, '<u2', c[0], c[4]).tolist() != np.frombuffer(original, '>u2', c[0], c[4]).tolist():
                probs.append(f'TPL texture {i}: CLUT data differs')
        p = ot['data']
        for (a, b), img in zip(_levels(w, h, mips), imgs):
            sz = level_size(ot['fmt'], a, b)
            ref = decode(ot['fmt'], a, b, original[p:p + sz], pal)
            if not np.array_equal(ref, img):
                probs.append(f'TPL texture {i} level {a}x{b}: pixels differ from the GX decode')
                break
            p += sz
    return probs


def summary(d: bytes) -> dict:
    _, texs = parse(d, '>')
    return {'textures': [f'{FMT_NAMES[t["fmt"]]} {t["w"]}x{t["h"]}' + (' clut' if t['clut'] else '') +
                         (f' mips{_mips(t)}' if _mips(t) > 1 else '') for t in texs]}
