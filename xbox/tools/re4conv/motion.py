"""FCV motions, SEQ sequence tables and face ShapeData -> little-endian (same layout).

Source of truth: tools/motion/fcv.py (parse / serialise with an endian flag), include/model.h
MotionData / SEQUENCE_FILE, game/motion.cpp MotionSetCore. `serialise(parse(d, '>'), '<')` is the
exact pattern from 04-DATA.md section 3: every u16/s16/f32 field byte-swapped, bytes untouched,
layout and padding identical, so the game's byte-wise readers see the GameCube values.

ShapeData (face/*.fcv bins of the events, shape.cpp CalculateShape_new) has the same header layout
as a motion: u16 nFrame, u8 num, u16 flags[num] (= kind), u8 idx[num] (= partsNo), 4-aligned s32
marker (= the size word), s32 table[num] (= keyOfs) and Hermite key blocks typed by flags >> 12,
so it goes through the same code.

Not converted: the PS2 disc is already little-endian; nothing to do with it here.
"""
import fcv  # tools/motion (sys.path set by re4conv/__init__)

from . import ConvertError, NotThisFormat


def convert(d: bytes) -> bytes:
    """FCV entry (big-endian) -> little-endian image of the same layout."""
    try:
        m = fcv.parse(d, '>')
    except (ValueError, IndexError, KeyError) as e:
        raise ConvertError(f'FCV: {e}') from e
    out = fcv.serialise(m, '<')
    be = fcv.serialise(m, '>')
    n = len(d)
    if n % 0x20 and len(out) == (n + 0x1F) & ~0x1F:
        # standalone file without the archive's 0x20 padding (ss/<lang>/tel*.fcv): same bytes, unpadded
        out, be = out[:n], be[:n]
    if len(out) != len(d):
        raise ConvertError(f'FCV: converted size {len(out):#x} != {len(d):#x}')
    if be != d:
        raise ConvertError('FCV: big-endian re-serialisation is not byte-identical (unknown layout variant)')
    return out


def check(d: bytes, original: bytes = None) -> list:
    """Problems with a converted (little-endian) FCV; [] when it parses and, given the original,
    decodes to the same motion."""
    try:
        m = fcv.parse(d, '<')
    except (ValueError, IndexError, KeyError) as e:
        return [f'FCV (LE): {e}']
    if original is not None and fcv.parse(original, '>') != m:
        return ['FCV: little-endian parse differs from the big-endian original']
    return []


def convert_seq(d: bytes) -> bytes:
    try:
        s = fcv.parse_seq(d, '>')
    except (ValueError, IndexError) as e:
        # op/*.das "SEQ" entries are the terminal's operation scripts (ss_term.cpp), not SEQUENCE_FILE
        raise NotThisFormat(f'SEQ: not a SEQUENCE_FILE ({e})') from e
    if fcv.serialise_seq(s, '>') != d:
        raise ConvertError('SEQ: big-endian re-serialisation is not byte-identical')
    out = fcv.serialise_seq(s, '<')
    if len(out) != len(d):
        raise ConvertError('SEQ: size changed')
    return out


def check_seq(d: bytes, original: bytes = None) -> list:
    try:
        s = fcv.parse_seq(d, '<')
    except (ValueError, IndexError) as e:
        return [f'SEQ (LE): {e}']
    if original is not None and fcv.parse_seq(original, '>') != s:
        return ['SEQ: little-endian parse differs from the big-endian original']
    return []
