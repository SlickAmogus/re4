"""BIN models (cModelData) -> little-endian, same layout, every multi-byte field byte-swapped.

Struct truth (offsets are exact, see include/model.h):
  cModelData (0x48 bytes)
    0x00 u32 pHead      -> ModelDataHead[nParts], 16 bytes each: u8 partsNo, parentNo, x2, x3; f32 center[3]
    0x04 u8  pad[8]     unknown; on the retail discs u32 0 @0x04 and a small BE u32 @0x08 (0x50 in pl00) -> both swapped as u32
    0x0C u32 pClr       vertex colours RGBA8 (4 bytes, no byte order), GX_VA_CLR0 when flags bit31
    0x10 u32 pTex       texcoords, 2 x (s16 frac 8 | u16 frac 15), stride 4
    0x14 u32 pWeight    cWeight[weight_palette_num] (8 bytes, all u8) or, when weight_ext_num > 0xFF,
                        cWeightExt[weight_ext_num] (12 bytes: u16 WeightId[3], u16 nWeight, u8 Weight[3], pad)
                        (trans.cpp:746 picks the table, trans.cpp:70-80 defines both)
    0x18 u8  weight_palette_num
    0x19 u8  nParts
    0x1A u16 displist_num   primitive parts (cPolyHeader + stream) at pParts
    0x1C u32 pParts     first cPolyHeader (0x20-aligned, the game HALTs otherwise)
    0x20 u32 flags      bit31: s16 texcoords + colour array present, bit29: s8 normals (else s16 frac 14)
    0x24 u32 nTex
    0x28 u8  shift      position fixed-point shift
    0x29 u8  pad
    0x2A u16 weight_ext_num
    0x2C u32 shapeOfs   0 or the shape table: u32 count; ShapeEntry[count] {u32 ofs (from shapeOfs+4); s32 num};
                        deltas num x {s16 vtx, dx, dy, dz} at shapeOfs+4+ofs (shape.cpp CalculateShape_new)
    0x30 u32 vtxOrig    positions s16 xyz + u16 pad, stride 8, nVtx entries
    0x34 u32 nrmOrig    normals s8 xyz + pad (stride 4) or s16 xyz + pad (stride 8), nNrm entries
    0x38 u16 nVtx
    0x3A u16 nNrm
    0x3C u32 version    0x20010801 / 0x20030817 / 0x20030818
    0x40 u32 blendTbl   (version 0x20030818) 0 or: s32 count; count x {u16 dst, a, c, percent}
    0x44 u32 flipTbl    (version 0x20030818) 0 or: u32 count; u16 remap[count]  (pXFlip = flipTbl + 4)
  Versions <= 0x20030817 have a 0x40-byte header (blendTbl/flipTbl absent; pHead is 0x40 there).
  cPolyHeader (0x20): u8 material bytes 0x00..0x17 (no byte order), u32 buff_size @0x18, u32 nPoly @0x1C;
    the GX FIFO stream follows at +0x20 and the next header at +0x20+buff_size (trans.cpp:1245).
  Stream (trans.cpp commonModelTrans:1122-1155 sets the VCD before GXCallDisplayList; shadow.cpp:1533
    and mirror.cpp use the same one): per primitive u8 opcode (GX_DRAW_* | vtxfmt, 0x80..0xB8), u16
    count, then count vertices of INDEX16 attributes in VCD order: pos, nrm, [clr0 when flags bit31],
    tex0. No matrix index (GXSetVtxDesc(0, ..) is never used for model data). 0x00 bytes are NOPs
    (padding to 32 bytes at the end). Any other opcode is an error. The port's interpreter
    (xbox/port/gx/gx_dl.cpp gx_run_stream) reads the converted stream with big_endian=0: counts and
    indices little-endian.

calcModelAddr (model.cpp:930) relocates pClr, pTex, pHead, pWeight, pParts, vtxOrig, nrmOrig (and
blendTbl / flipTbl when non-zero and version > 0x20030817) by adding the base; pClr < 0 marks an
already relocated image. Offsets stay valid after the byte swap because the layout is untouched.

Sections are located by the header offsets and may appear in any order (pl00 BIN2 has the shape table
between the normals and the colours); each array's extent is bounded by the next section start, and
the index ranges of the display lists are validated against the counts.
"""
import struct
from array import array

from . import ConvertError, NotThisFormat

HDR = struct.Struct('>IIIIIIBBHIIIBBHIIIHHIII')
HDR_LE = struct.Struct('<IIIIIIBBHIIIBBHIIIHHIII')
HDR_SIZE = 0x48
VERSIONS = (0x20010801, 0x20030817, 0x20030818)
PRIM_OPS = (0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8)
FLAG_CLR = 0x80000000
FLAG_NRM8 = 0x20000000
WEIGHT_SIZE = 8
WEIGHT_EXT_SIZE = 12


def _swap16(o: bytearray, start: int, nbytes: int):
    """Byte-swaps nbytes (even) of u16 at o[start:] in place."""
    a = array('H')
    a.frombytes(bytes(o[start:start + nbytes]))
    a.byteswap()
    o[start:start + nbytes] = a.tobytes()


def _swap32(o: bytearray, start: int, nbytes: int):
    a = array('I')
    a.frombytes(bytes(o[start:start + nbytes]))
    a.byteswap()
    o[start:start + nbytes] = a.tobytes()


class Header:
    __slots__ = ('pHead', 'x4', 'x8', 'pClr', 'pTex', 'pWeight', 'weight_palette_num', 'nParts', 'displist_num',
                 'pParts', 'flags', 'nTex', 'shift', 'pad29', 'weight_ext_num', 'shapeOfs', 'vtxOrig', 'nrmOrig',
                 'nVtx', 'nNrm', 'version', 'blendTbl', 'flipTbl')

    def __init__(self, d, endian='>'):
        st = HDR if endian == '>' else HDR_LE
        if len(d) < HDR_SIZE:
            raise ConvertError(f'BIN: {len(d)} bytes, smaller than the header')
        for k, v in zip(self.__slots__, st.unpack_from(d, 0)):
            setattr(self, k, v)

    @property
    def has_tables(self):
        return self.version > 0x20030817

    @property
    def nidx(self):
        return 4 if self.flags & FLAG_CLR else 3

    @property
    def nrm_stride(self):
        return 4 if self.flags & FLAG_NRM8 else 8

    def as_tuple(self):
        t = tuple(getattr(self, k) for k in self.__slots__)
        return t if self.has_tables else t[:-2]   # 0x40.. is pHead's data in old versions


def _validate_header(h: Header, size: int):
    if h.version not in VERSIONS:
        raise NotThisFormat(f'BIN: unknown version {h.version:#x} (not a cModelData)')
    if h.x4 != 0 or h.x8 > size:
        raise ConvertError(f'BIN: header words 0x04/0x08 = {h.x4:#x}/{h.x8:#x} (expected 0 and a small u32)')
    if h.pClr & 0x80000000:
        raise ConvertError('BIN: header is relocated (memory image)')
    for name in ('pHead', 'pClr', 'pTex', 'pWeight', 'pParts', 'vtxOrig', 'nrmOrig'):
        v = getattr(h, name)
        if v == 0 or v >= size:
            raise ConvertError(f'BIN: {name} {v:#x} outside the entry ({size:#x})')
    if h.pParts & 0x1F:
        raise ConvertError(f'BIN: pParts {h.pParts:#x} not 32-byte aligned (the game HALTs)')
    for name in ('shapeOfs',) + (('blendTbl', 'flipTbl') if h.has_tables else ()):
        v = getattr(h, name)
        if v and v >= size:
            raise ConvertError(f'BIN: {name} {v:#x} outside the entry ({size:#x})')
    if h.nParts == 0 or h.displist_num == 0:
        raise ConvertError(f'BIN: nParts {h.nParts}, displist_num {h.displist_num}')


def _sections(h: Header, size: int):
    """Sorted section starts (header offsets that point at data) plus the end."""
    s = {HDR_SIZE if h.has_tables else 0x40, h.pHead, h.pClr, h.pTex, h.pWeight, h.pParts, h.vtxOrig, h.nrmOrig, size}
    if h.shapeOfs:
        s.add(h.shapeOfs)
    if h.has_tables:
        if h.blendTbl:
            s.add(h.blendTbl)
        if h.flipTbl:
            s.add(h.flipTbl)
    return sorted(s)


def _span(starts, ofs):
    """Bytes from `ofs` to the next section start."""
    i = starts.index(ofs)
    return starts[i + 1] - ofs


def parse(d: bytes, endian='>'):
    """Structural decode of a model: header tuple, parts hierarchy, arrays, display-list indices,
    shape / blend / flip tables. Used by `check` to compare the little-endian output with the
    big-endian original and by `convert` for validation. Returns a dict of tuples/ints/bytes only."""
    h = Header(d, endian)
    _validate_header(h, len(d))
    starts = _sections(h, len(d))
    e = endian
    out = {'header': h.as_tuple()}
    n = h.nParts
    if h.pHead + 16 * n > len(d):
        raise ConvertError('BIN: ModelDataHead array runs past the end')
    out['heads'] = tuple(struct.unpack_from(f'{e}4B3f', d, h.pHead + 16 * i) for i in range(n))
    if h.vtxOrig + 8 * h.nVtx > len(d) or 8 * h.nVtx > _span(starts, h.vtxOrig):
        raise ConvertError(f'BIN: {h.nVtx} vertices do not fit at {h.vtxOrig:#x}')
    out['vtx'] = struct.unpack_from(f'{e}{4 * h.nVtx}h', d, h.vtxOrig)
    ns = h.nrm_stride
    if h.nrmOrig + ns * h.nNrm > len(d) or ns * h.nNrm > _span(starts, h.nrmOrig):
        raise ConvertError(f'BIN: {h.nNrm} normals do not fit at {h.nrmOrig:#x}')
    if ns == 4:
        out['nrm'] = bytes(d[h.nrmOrig:h.nrmOrig + 4 * h.nNrm])
    else:
        out['nrm'] = struct.unpack_from(f'{e}{4 * h.nNrm}h', d, h.nrmOrig)
    n_tex = _span(starts, h.pTex) // 4
    out['tex'] = struct.unpack_from(f'{e}{2 * n_tex}H', d, h.pTex)
    n_clr = _span(starts, h.pClr) // 4
    out['clr'] = bytes(d[h.pClr:h.pClr + 4 * n_clr])
    if h.weight_ext_num > 0xFF:
        nw = h.weight_ext_num
        if nw * WEIGHT_EXT_SIZE > _span(starts, h.pWeight):
            raise ConvertError(f'BIN: {nw} cWeightExt do not fit at {h.pWeight:#x}')
        out['weights'] = tuple(struct.unpack_from(f'{e}4H4B', d, h.pWeight + WEIGHT_EXT_SIZE * i) for i in range(nw))
    else:
        nw = h.weight_palette_num
        if nw * WEIGHT_SIZE > _span(starts, h.pWeight):
            raise ConvertError(f'BIN: {nw} cWeight do not fit at {h.pWeight:#x}')
        out['weights'] = bytes(d[h.pWeight:h.pWeight + WEIGHT_SIZE * nw])
    # parts: cPolyHeader + display list each
    parts = []
    p = h.pParts
    nidx = h.nidx
    limits = (h.nVtx, h.nNrm, n_clr, n_tex) if nidx == 4 else (h.nVtx, h.nNrm, n_tex)
    names = ('pos', 'nrm', 'clr', 'tex') if nidx == 4 else ('pos', 'nrm', 'tex')
    parts_end = h.pParts + _span(starts, h.pParts)
    for i in range(h.displist_num):
        if p + 0x20 > len(d):
            raise ConvertError(f'BIN: part {i} header at {p:#x} past the end')
        buff_size, npoly = struct.unpack_from(f'{e}II', d, p + 0x18)
        s0 = p + 0x20
        s1 = s0 + buff_size
        if s1 > parts_end or buff_size & 0x1F:
            raise ConvertError(f'BIN: part {i} stream {s0:#x}+{buff_size:#x} (section end {parts_end:#x}; '
                               f'buff_size must be a multiple of 32)')
        prims = []
        q = s0
        while q < s1:
            op = d[q]
            if op == 0:
                q += 1
                continue
            if (op & 0xF8) not in PRIM_OPS:
                raise ConvertError(f'BIN: part {i} opcode {op:#x} at stream offset {q - s0:#x} is not a GX_DRAW primitive')
            if op & 7:
                raise ConvertError(f'BIN: part {i} uses vertex format {op & 7} (commonModelTrans only sets GX_VTXFMT0)')
            if q + 3 > s1:
                raise ConvertError(f'BIN: part {i} primitive header truncated')
            cnt, = struct.unpack_from(f'{e}H', d, q + 1)
            q += 3
            nb = cnt * nidx * 2
            if q + nb > s1:
                raise ConvertError(f'BIN: part {i} primitive of {cnt} vertices runs past buff_size')
            idx = struct.unpack_from(f'{e}{cnt * nidx}H', d, q)
            for k in range(nidx):
                col = idx[k::nidx]
                if col and max(col) >= limits[k]:
                    raise ConvertError(f'BIN: part {i} {names[k]} index {max(col)} >= {limits[k]}')
            prims.append((op, idx))
            q += nb
        parts.append((bytes(d[p:p + 0x18]), buff_size, npoly, tuple(prims)))
        p = s1
    out['parts'] = tuple(parts)
    if h.shapeOfs:
        so = h.shapeOfs
        count, = struct.unpack_from(f'{e}I', d, so)
        tbl = so + 4
        span = _span(starts, so)
        if count > 0x1000 or 4 + 8 * count > span:
            raise ConvertError(f'BIN: shape table count {count} does not fit')
        ents = [struct.unpack_from(f'{e}Ii', d, tbl + 8 * k) for k in range(count)]
        deltas = []
        for ofs, num in ents:
            if num < 0 or ofs + 8 * num > span - 4:
                raise ConvertError(f'BIN: shape entry {ofs:#x}+{num} outside the shape table ({span:#x})')
            deltas.append(struct.unpack_from(f'{e}{4 * num}h', d, tbl + ofs))
        out['shape'] = (count, tuple(ents), tuple(deltas))
    if h.has_tables:
        if h.blendTbl:
            count, = struct.unpack_from(f'{e}i', d, h.blendTbl)
            if count < 0 or 4 + 8 * count > _span(starts, h.blendTbl):
                raise ConvertError(f'BIN: blend table count {count} does not fit')
            out['blend'] = struct.unpack_from(f'{e}{4 * count}H', d, h.blendTbl + 4)
        if h.flipTbl:
            count, = struct.unpack_from(f'{e}I', d, h.flipTbl)
            if 4 + 2 * count > _span(starts, h.flipTbl):
                raise ConvertError(f'BIN: flip table count {count} does not fit')
            out['flip'] = struct.unpack_from(f'{e}{count}H', d, h.flipTbl + 4)
    return out


def convert(d: bytes) -> bytes:
    """Big-endian model -> little-endian image of the same size and layout."""
    parsed = parse(d, '>')   # validates everything first
    h = Header(d, '>')
    starts = _sections(h, len(d))
    o = bytearray(d)
    for ofs in (0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x1C, 0x20, 0x24, 0x2C, 0x30, 0x34, 0x3C) + ((0x40, 0x44) if h.has_tables else ()):
        _swap32(o, ofs, 4)
    for ofs in (0x1A, 0x2A, 0x38, 0x3A):
        _swap16(o, ofs, 2)
    for i in range(h.nParts):
        _swap32(o, h.pHead + 16 * i + 4, 12)       # ModelDataHead.center
    _swap16(o, h.vtxOrig, 8 * h.nVtx)
    if h.nrm_stride == 8:
        _swap16(o, h.nrmOrig, 8 * h.nNrm)
    _swap16(o, h.pTex, _span(starts, h.pTex) & ~1)
    if h.weight_ext_num > 0xFF:
        for i in range(h.weight_ext_num):
            _swap16(o, h.pWeight + WEIGHT_EXT_SIZE * i, 8)
    p = h.pParts
    nidx = h.nidx
    for _, buff_size, _, prims in parsed['parts']:
        _swap32(o, p + 0x18, 8)
        q = p + 0x20
        for op, idx in prims:
            while d[q] == 0:
                q += 1
            _swap16(o, q + 1, 2 + 2 * len(idx))
            q += 3 + 2 * len(idx)
        p += 0x20 + buff_size
    if h.shapeOfs:
        count, ents, _ = parsed['shape']
        _swap32(o, h.shapeOfs, 4 + 8 * count)
        for ofs, num in ents:
            _swap16(o, h.shapeOfs + 4 + ofs, 8 * num)
    if h.has_tables:
        if h.blendTbl:
            _swap32(o, h.blendTbl, 4)
            _swap16(o, h.blendTbl + 4, 2 * len(parsed['blend']))
        if h.flipTbl:
            _swap32(o, h.flipTbl, 4)
            _swap16(o, h.flipTbl + 4, 2 * len(parsed['flip']))
    out = bytes(o)
    back = parse(out, '<')
    if back != parsed:
        bad = [k for k in parsed if back.get(k) != parsed[k]]
        raise ConvertError(f'BIN: little-endian image does not decode to the original in {bad} (converter bug)')
    return out


def check(d: bytes, original: bytes = None) -> list:
    """Problems with a converted (little-endian) model; [] when it parses and, given the original,
    decodes to the same structure."""
    try:
        p = parse(d, '<')
    except ConvertError as e:
        return [str(e)]
    if original is not None:
        try:
            if parse(original, '>') != p:
                return ['BIN: little-endian parse differs from the big-endian original']
        except ConvertError as e:
            return [f'BIN: original does not parse: {e}']
    return []


def summary(d: bytes, endian='>') -> dict:
    """Small per-model statistics for the manifest."""
    h = Header(d, endian)
    return {'version': f'{h.version:#x}', 'nParts': h.nParts, 'displist_num': h.displist_num, 'nVtx': h.nVtx,
            'nNrm': h.nNrm, 'nTex': h.nTex, 'flags': f'{h.flags:#x}', 'shape': bool(h.shapeOfs)}
