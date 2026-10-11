"""Fonts (.fnt; FNT entries of ss/*.dat bodies), CT-14.

MessageControl::setupFont (src/game/mes.cpp:338): u32 tplOfs@0, u32 widthOfs@4 (relative to the file), zero to
0x20; a TPL at tplOfs (MessageFont::create binds up to two sheets), the glyph width table at widthOfs (s8
left/right pairs, bytes, copied). The TPL is converted by tpl.py (its size changes), so the width table moves
to the next 0x20 boundary after it and widthOfs is rewritten.
"""
import struct

from . import ConvertError, tpl


def _parse(d, e):
    if len(d) < 0x20:
        raise ConvertError('FNT: short')
    t, w = struct.unpack_from(e + '2I', d, 0)
    if t != 0x20 or not t < w <= len(d) or any(d[8:0x20]):
        raise ConvertError(f'FNT: tplOfs {t:#x} widthOfs {w:#x}')
    return t, w


def convert(d: bytes) -> bytes:
    t, w = _parse(d, '>')
    tp = tpl.convert(d[t:w])
    tp += b'\0' * ((-len(tp)) % 0x20)
    return struct.pack('<2I', t, t + len(tp)) + bytes(0x18) + tp + d[w:]


def check(out: bytes, original: bytes = None) -> list:
    try:
        t, w = _parse(out, '<')
    except ConvertError as e:
        return [str(e)]
    if original is None:
        return tpl.check(out[t:w])
    t0, w0 = _parse(original, '>')
    probs = [f'FNT/TPL: {p}' for p in tpl.check(out[t:w], original[t0:w0])]
    if out[w:] != original[w0:]:
        probs.append('FNT: width table differs')
    return probs
