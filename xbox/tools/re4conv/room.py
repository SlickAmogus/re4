"""Room block entries (CT-14): fixed-layout tables of St*/rNNN.das bodies (and core.das), converted field by
field to little-endian with the span engine (spans.py). Entries holding nested BIN/TPL (SMD, SHD, TEX,
ITM, EFF) live in room_nested.py. Each builder takes (bytes, byte order) and returns (spans, info); see
FORMATS.md section 2 for the per-tag notes. Unknown non-zero bytes outside every known span are refused.

Shared: AREA_HIT_DATA (include/area.h:47) 0x30 = u8 be_flag, type; u16 pad; 11 x f32 (every union body
is f32) -> AREA.
"""
import struct

from . import ConvertError
from .spans import _L, apply, values

AREA = 'BBH11f'                     # 0x30


def _fmt_size(f):
    return struct.calcsize('<' + f)


# ---- BLK (include/block.h, block.cpp) ---------------------------------------------------------------
# BlockHeader '4sHBBHH3I' 0x18: char tag[4] "BLK", u16 version 0x100, u8 x6, u8 nBlock, u16 nArea,
# u16 nConnect, u32 ofsLink, ofsArea, ofsConnect (relative to the file). BlockLink 0xC {u8 flags; pad 3;
# s8 link[8]}; BlockArea 0x38 {u32 tag (OT link, runtime); AREA_HIT_DATA; u8 flags, x35; s8 areaNo; u8 pri};
# BlockConnect 0x14 {u8 flags, blockNo; pad 2; s8 mram[8], aram[8]}.

BLK_HDR = '4sHBBHH3I'
BLK_LINK = 'B3s8b'
BLK_AREA = 'I' + AREA + 'BBbB'
BLK_CONNECT = 'BB2s16b'


def blk_layout(d, e='>'):
    L = _L(d, e)
    tag, ver, _, n_block, n_area, n_conn, o_link, o_area, o_conn = L.get(BLK_HDR, 0)
    if tag[:3] != b'BLK' or ver != 0x100:
        raise ConvertError(f'BLK: tag {tag!r} version {ver:#x}')
    L.add(0, 0x18, BLK_HDR)
    L.arr(o_link, BLK_LINK, n_block)
    L.arr(o_area, BLK_AREA, n_area)
    L.arr(o_conn, BLK_CONNECT, n_conn)
    return L.finish(), {'blocks': n_block, 'areas': n_area, 'connects': n_conn}


# ---- LIT (include/light.h:291 cLit, light.cpp) ------------------------------------------------------
# u16 CutNum, u8 Version, u8 nMaxLight, u32 ofs[CutNum] (relative to the file, 0 = none) -> cLightEnv
# 0x104 (light.h:255) followed by nLight cLightWork 0x12C (light.h:116). cLit::versionUp (light.cpp:1521)
# patches bytes/colours in place after load (fine on the converted data).

LIT_ENV = ('4sI'            # AmbientScr, nLight
           'I2f4s'          # Fog {GXFogType Type; f32 Start, End; GXColor}
           'I2f4s'          # MirrorFog
           'i'              # FocusZ
           '4s'             # FocusFlag, FocusLevel, FocusMode, blur_rate
           '4s'             # tuneOn + pad
           '12s'            # Tune[3]
           '4s'             # tev_scale[4]
           'f'              # far_play_ratio
           '164s'           # Hokan @0x48 + pad_49..0xEC (unknown, copied)
           '4s'             # wind (3 bytes) + pad_EF
           '8s'             # blur_type, blur_power, min_lod, max_lod, aniso, contrast[3]
           'f'              # lod_bias
           '4s4s')          # AmbientEm, AmbientEsp
LIT_WORK_HEAD = '4s3ff4sf4sIHHI'   # 0x2C: BeFlag..EnableMask, Pos, Radius, Col, Intensity, ParentType..Priority,
                                   # ParentNo, HitRadius, Dummy82, Dummy9
LIT_SPOT = '16I'                   # 0x40 @0x2C: union by Type (LIT_TYPE03/04/05_FREE), every field 4 bytes
# work/sub @0x6C (0x80) by Id (LightFuncTbl, game.cpp:173): LIT01..LIT08_MOVE_FREE (light.h), Light10Work
LIT_SUB = {
    2: '4f112s',                   # LIT02 {f32 Center, Range, Speed, Radian}
    3: '3f116s',                   # light03: Vec Rot
    4: 'HBBhh4s3fB103s',           # LIT04 {u16 Flag; u8 Kind, Tex_no; s16 Ang_x, Ang_y; u8 x4; Vec Lit_pos; u8 Fovy_sub}
    5: '2I120s',                   # cLightPath {u8* pBase, pPtr (runtime); u8 Flag} + LIT05 u8 Id@C, Flag@D
    6: '3f116s',                   # LIT06 {f32 m_Start, m_Speed, m_Fade}
    7: '6f104s',                   # LIT07 {Vec Rot, Speed}
}
LIT_SUB_RAW = '128s'               # Id 0, 1 (GXColor + s8), 8 (bytes), 9..15, 16 (u8 wait)
LIT_PATH = '64s'                   # LightPath @0xEC: pad_0[0x40], copied
assert _fmt_size(LIT_ENV) == 0x104 and _fmt_size(LIT_WORK_HEAD) == 0x2C
assert all(_fmt_size(f) == 0x80 for f in LIT_SUB.values())


def lit_layout(d, e='>'):
    L = _L(d, e)
    n_cut, ver, n_max = L.get('HBB', 0)
    L.add(0, 2, 'H')
    L.add(2, 4, None)
    L.arr(4, 'I', n_cut)
    lights = 0
    for o in sorted({L.get('I', 4 + 4 * i) for i in range(n_cut)} - {0}):
        L.add(o, o + 0x104, LIT_ENV)
        n = L.get('I', o + 4)
        w = o + 0x104
        for _ in range(n):
            lid = L.get('B', w + 2)
            L.add(w, w + 0x2C, LIT_WORK_HEAD)
            L.add(w + 0x2C, w + 0x6C, LIT_SPOT)
            L.add(w + 0x6C, w + 0xEC, LIT_SUB.get(lid, LIT_SUB_RAW))
            L.add(w + 0xEC, w + 0x12C, LIT_PATH)
            w += 0x12C
        lights += n
    return L.finish(), {'version': ver, 'cuts': n_cut, 'lights': lights}


# ---- registry -----------------------------------------------------------------------------------------

BUILDERS = {
    'BLK': blk_layout,
    'LIT': lit_layout,
}


def convert(tag, d: bytes) -> bytes:
    spans, _ = BUILDERS[tag](d, '>')
    return apply(d, spans)


def check(tag, out: bytes, original=None) -> list:
    b = BUILDERS[tag]
    try:
        spans, _ = b(out, '<')
    except (ConvertError, struct.error) as ex:
        return [f'{tag}: {ex}']
    if original is not None:
        if len(out) != len(original):
            return [f'{tag}: size changed']
        spans_in, _ = b(original, '>')
        if spans_in != spans:
            return [f'{tag}: layout differs from the original']
        if values(out, spans, '<') != values(original, spans_in, '>'):
            return [f'{tag}: field values differ from the original']
    return []
