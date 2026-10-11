"""Event packet streams (CT-14): the part of an event block (.evd, EVS) the first pass left big-endian.

EvtPacket (include/event.h:160): a 0x10 header {int id (EvpTp); u32 flag; s16 cut, frame; s16 size (offset
of the next packet); s16 pad} followed by a body whose shape depends on the id; the stream runs from
EvtHeader.pacOfs for pacSize bytes and ends with id 0x1B (EndPac). Bodies, from the handlers
(src/game/event.cpp:804-1480, CalMaxCut/CalMaxFrame :1689-1751):

  names only (copied): 1 SetPl, 3 SetOm, 6 Cam, 11 Mot, 12 Shp, 14 Lit, 28 SetEff, 30 Fog, 31 Focus,
                       32 SetMdt (mod {char name[0xC], bin[0x30], tpl[0x30]}); 4 SetParts (parts {name, oya,
                       bin, tpl})
  val  {s32 no, arg, time}: 8 CamDammy (no = frames), 15 Str, 16 Se, 17 Mes, 18 Func, 29 Fade
  pos  {char name[0xC], oya[0xC]; s32 pos[3], rot[3], partsNo}: 9 Pos
  esp  {char name[0xC]; s32 type; u8 pad[3], parts}: 13 Esp
  unused handlers (body copied): 0 BeginEvt, 2 SetEm, 5 SetList, 7 CamPos, 10 PosPl, 19 ParentOn,
                       20 ParentOff, 21..27 End*
Bytes after the shape (up to `size`) are copied.
"""
import struct

from . import ConvertError
from .spans import _L

HEAD = 'iIhhhh'
END_PAC = 0x1B
BODY = {}
for _i in (8, 15, 16, 17, 18, 29):
    BODY[_i] = 'iii'
BODY[9] = '24s7i'
BODY[13] = '12si4s'


def packet_spans(L, pac_ofs, pac_size):
    """Adds the packet stream's spans to L; returns the packet count."""
    p, end, n = pac_ofs, pac_ofs + pac_size, 0
    if end > len(L.d):
        raise ConvertError(f'event packets {pac_ofs:#x}+{pac_size:#x} past the block')
    while p < end:
        pid, _, _, _, size, _ = L.get(HEAD, p)
        if not 0 <= pid <= 0x20:
            raise ConvertError(f'event packet at {p:#x}: id {pid}')
        if pid == END_PAC:
            size = max(size, 0x10)
        if size < 0x10 or p + size > end:
            raise ConvertError(f'event packet at {p:#x}: size {size:#x}')
        L.add(p, p + 0x10, HEAD)
        f = BODY.get(pid)
        if f:
            k = struct.calcsize('<' + f)
            if 0x10 + k > size:
                raise ConvertError(f'event packet at {p:#x} (id {pid}): body {size:#x} < {0x10 + k:#x}')
            L.add(p + 0x10, p + 0x10 + k, f)
        n += 1
        p += size
        if pid == END_PAC:
            break
    return n


def packet_layout(d, pac_ofs, pac_size, e='>'):
    """Spans of the packet region only (start-relative to d)."""
    L = _L(d, e)
    n = packet_spans(L, pac_ofs, pac_size)
    return sorted(set(L.spans)), n


def convert_packets(d: bytes, pac_ofs, pac_size, src='>', dst='<') -> bytes:
    """d with the packet stream rewritten in the other byte order (everything else unchanged)."""
    from .spans import apply
    sp, _ = packet_layout(d, pac_ofs, pac_size, src)
    out = bytearray(d)
    for span in sp:
        out[span[0]:span[1]] = apply(d, [span], src, dst)
    return bytes(out)
