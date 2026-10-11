"""Simple fixed-layout formats: ESL enemy lists, etc/SizeTbl.dat, bgm/bgmtbl.dat.

ESL  (etc/*.esl, read raw into pG->Em_list, include/global.h:67 EM_LIST, 0x20 bytes per entry, 255 or
     256 entries): u8 be_flag, id, type, set; u32 flag@4; u16 hp@8; u8 emset_no@A, Character@B;
     s16 pos[3]@C; s16 rot[3]@12; u16 room@18; s16 Guard_r@1A; u8 pad[4]@1C.
SizeTbl.dat (dvd.cpp:1159 SizeTableRead -> pSizeTbl, never consumed by game code): u32 count;
     count x {char name[8]; u32 size; u32 x} (16 bytes) -> count and both words swapped.
bgmtbl.dat (snd.cpp:112 SndMem.bgm_tbl, include/snd.h:80-92): SndBgmTbl {u32 room_ofs, u32 list_ofs};
     at room_ofs u32 ofs[nRoom] relative to that array -> SndBgmRoom {u32 num; SndBgmEnt[num] (u32 id;
     u32 bgm[6]; u32 str[6]) = 0x34 each}; at list_ofs u16 room ids terminated by 0xFFFF (SndBgmTblInit
     snd.cpp:162 walks list and offsets in step). Everything before list_ofs is u32 words (validated by
     walking every room record inside that region), the list is u16.

Each format: convert(bytes) -> bytes, check(converted, original) -> [problems].
"""
import re
import struct
from array import array

from . import ConvertError


def _swap(d, code, start, end):
    a = array(code)
    a.frombytes(bytes(d[start:end]))
    a.byteswap()
    return a.tobytes()


# ---- ESL --------------------------------------------------------------------------------------------

ESL = struct.Struct('>4BIH2B3h3hHh4s')
ESL_LE = struct.Struct('<4BIH2B3h3hHh4s')


def esl_convert(d: bytes) -> bytes:
    if len(d) % 0x20 or not d:
        raise ConvertError(f'ESL: size {len(d):#x} is not a multiple of 0x20')
    return b''.join(ESL_LE.pack(*ESL.unpack_from(d, o)) for o in range(0, len(d), 0x20))


def esl_check(d, original=None):
    if len(d) % 0x20:
        return ['ESL: size']
    if original is not None:
        if [ESL_LE.unpack_from(d, o) for o in range(0, len(d), 0x20)] != \
                [ESL.unpack_from(original, o) for o in range(0, len(original), 0x20)]:
            return ['ESL: entries differ']
    return []


# ---- SizeTbl ----------------------------------------------------------------------------------------

def _sizetbl_parse(d, e):
    n, = struct.unpack_from(e + 'I', d, 0)
    if 4 + 16 * n != len(d):
        raise ConvertError(f'SizeTbl: {n} records do not match size {len(d):#x}')
    return n, [struct.unpack_from(e + '8sII', d, 4 + 16 * i) for i in range(n)]


def sizetbl_convert(d: bytes) -> bytes:
    n, recs = _sizetbl_parse(d, '>')
    return struct.pack('<I', n) + b''.join(struct.pack('<8sII', *r) for r in recs)


def sizetbl_check(d, original=None):
    try:
        p = _sizetbl_parse(d, '<')
    except ConvertError as e:
        return [str(e)]
    if original is not None and _sizetbl_parse(original, '>') != p:
        return ['SizeTbl: records differ']
    return []


# ---- bgmtbl -----------------------------------------------------------------------------------------

def _bgm_parse(d, e):
    room_ofs, list_ofs = struct.unpack_from(e + 'II', d, 0)
    if not 8 <= room_ofs < list_ofs < len(d) or room_ofs & 3 or list_ofs & 1:
        raise ConvertError(f'bgmtbl: room_ofs {room_ofs:#x} list_ofs {list_ofs:#x} size {len(d):#x}')
    ids = []
    p = list_ofs
    while True:
        if p + 2 > len(d):
            raise ConvertError('bgmtbl: room list not terminated')
        v, = struct.unpack_from(e + 'H', d, p)
        p += 2
        if v == 0xFFFF:
            break
        ids.append(v)
    if any(d[p:]):
        raise ConvertError('bgmtbl: non-zero bytes after the room list')
    rooms = []
    for i in range(len(ids)):
        ofs, = struct.unpack_from(e + 'I', d, room_ofs + 4 * i)
        r = room_ofs + ofs
        num, = struct.unpack_from(e + 'I', d, r)
        if r & 3 or r + 4 + 0x34 * num > list_ofs or num > 0x100:
            raise ConvertError(f'bgmtbl: room {i} record {r:#x} ({num} entries) outside the u32 region')
        rooms.append((ofs, struct.unpack_from(f'{e}{13 * num}I', d, r + 4)))
    return room_ofs, list_ofs, tuple(ids), tuple(rooms)


def bgmtbl_convert(d: bytes) -> bytes:
    room_ofs, list_ofs, _, _ = _bgm_parse(d, '>')
    out = _swap(d, 'I', 0, list_ofs & ~3) + d[list_ofs & ~3:list_ofs] + _swap(d, 'H', list_ofs, len(d) & ~1)
    out += d[len(out):]
    return out


def bgmtbl_check(d, original=None):
    try:
        p = _bgm_parse(d, '<')
    except ConvertError as e:
        return [str(e)]
    if original is not None and _bgm_parse(original, '>') != p:
        return ['bgmtbl: decoded table differs']
    return []


# path regex (lower case, relative to files/) -> (name, convert, check)
HANDLERS = [
    (re.compile(r'^etc/[^/]+\.esl$'), 'ESL', esl_convert, esl_check),
    (re.compile(r'^etc/sizetbl\.dat$'), 'SizeTbl', sizetbl_convert, sizetbl_check),
    (re.compile(r'^bgm/bgmtbl\.dat$'), 'bgmtbl', bgmtbl_convert, bgmtbl_check),
]


def handler(relpath: str):
    rp = relpath.replace('\\', '/').lower()
    for rx, name, conv, chk in HANDLERS:
        if rx.match(rp):
            return name, conv, chk
    return None
