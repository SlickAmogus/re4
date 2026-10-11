"""Room entries holding nested BIN / TPL / FCV data (CT-14): SMD, SHD, TEX, ITM.

Each format is a head (fixed fields, converted with the span engine) followed by offset tables whose
slots point at nested files. Nested files are converted with container.convert_entry (TPLs change size
when re-encoded for the NV2A), so the entry is re-laid out: the head is copied (converted) up to the first
table, then every table in its original order at a 0x20-aligned position, keeping the original gap between
the table start and its first entry, and the nested files after it, each 0x20-aligned. Slots stay offsets
relative to their table, the head's table-offset fields are rewritten. Shared slots (same offset) are
converted once.

Formats (decomp references):
  SMD  include/scroll.h:25 cSmd, scroll.cpp:445-530: u8 Version, Flag (bit0: group table), u16 nModel,
       u32 BinTblOfs, TplTblOfs, MotTblOfs (relative to the SMD); works cSmdWork 0x48 at 0x10 {Vec Pos, Ang,
       Scale; u8 BinId, TplId, MotId, WorkNo; f32 Radius; u32 Flag; pad 0x14 (copied); u32 flags@44}, or with
       Flag bit0 u32 nGroup@10, u32 num[nGroup]@14 and the works at 0x14 + 4 * nGroup; nModel works in both
       cases on the retail discs. Tables: u32 slot[] relative to the table (0 = none), slot count = first entry / 4. A table
       offset equal to the SMD size means "no table".
  SHD  include/shadow.h:41 ShdHeader: u8 version (<= 0x41), x1; u16 num; u32 tblOfs (model table, u32 slots
       relative to the table); pad to 0x10; ShdEntry 0x48 {Vec pos, rot, scale; u8 model, x25, shdCol; pad}.
  TEX  include/texture.h:32 TexData: u32 version 3, ofsId -> TexIdTbl {u32 num; {u16 id, x2; u32 x4}[num]},
       ofsTpl -> TexOfsTbl {u32 num; u32 ofs[num] relative to the table} of TPLs, ofsAnm -> TexOfsTbl of cAnm
       (esp.h:121 {u16 Width, Height; s16 Cx, Cy; u16 Frames; u8 Xn, Loop, Data_num; pad 3; u8 Frame_cnt[]}).
  ITM  item_model.cpp:11 ItemModelPack: u32 x0, ofsId -> {u32 num; {u16 id, pad; u32 pad}[num]}, ofsBin,
       ofsTpl -> {u32 num; u32 ofs[num] relative to the table} of BIN / TPL.
"""
import struct

from . import ConvertError
from .spans import _L, apply, values

ALIGN = 0x20
ANM_HEAD = 'HHhhHBBB3s'


def _align(n, a=ALIGN):
    return (n + a - 1) & ~(a - 1)


class Table:
    """An offset table at `ofs` (absolute) whose slots are relative to it.
    counted: u32 num precedes the slots; else the slot count is the first entry offset / 4."""

    def __init__(self, field, ofs, slots, tag, counted, gap, single=False):
        self.field, self.ofs, self.slots, self.tag, self.counted, self.gap = field, ofs, slots, tag, counted, gap
        self.single = single      # the field points at one nested file directly (EffEfmEnt parts)


def _single(field, ofs, tag):
    return Table(field, ofs, [0], tag, False, 0, single=True)


def _slot_table(L, field, t, tag, end):
    """u32 slots relative to the table, count = first non-zero / 4 (or up to `end`)."""
    if t >= end:
        return None
    slots = []
    o = t
    while o + 4 <= end:
        v = L.u32(o)
        slots.append(v)
        o += 4
        nz = [s for s in slots if s]
        if nz and o >= t + min(nz):
            break
    nz = [s for s in slots if s]
    if not nz:
        raise ConvertError(f'table at {t:#x}: no entries')
    gap = min(nz)
    return Table(field, t, slots[:gap // 4], tag, False, gap)


def _counted_table(L, field, t, tag):
    n = L.u32(t)
    slots = [L.u32(t + 4 + 4 * i) for i in range(n)]
    nz = [s for s in slots if s]
    gap = min(nz) if nz else 4 + 4 * n
    if gap < 4 + 4 * n:
        raise ConvertError(f'table at {t:#x}: entry inside the table')
    return Table(field, t, slots, tag, True, gap)


def _entries(d, tables, end):
    """{absolute start: (end, tag)} of every nested file; a file runs to the next start (any table) or the
    next table / `end`."""
    starts = {}
    for tb in tables:
        if tb.single:
            starts[tb.ofs] = tb.tag
        for s in tb.slots:
            if s:
                starts[tb.ofs + s] = tb.tag
    bounds = sorted(set(starts) | {tb.ofs for tb in tables} | {end})
    out = {}
    for a, tag in starts.items():
        b = next(x for x in bounds if x > a)
        if b > len(d):
            raise ConvertError(f'nested file at {a:#x} past the entry')
        out[a] = (b, tag)
    return out


# ---- per-format parsers: (d, e) -> (L with head spans, head_end, [Table], end of nested data) -----------------

def _smd(d, e):
    L = _L(d, e)
    ver, flag, n_model, bt, tt, mt = L.get('BBHIII', 0)
    if ver != 0x40:
        raise ConvertError(f'SMD: version {ver:#x}')
    L.add(0, 0x10, 'BBHIII')
    if flag & 1:
        n_grp = L.u32(0x10)
        L.arr(0x10, 'I', 1 + n_grp)
        # the retail files hold nModel works after the group table (r100 #4: 13 works, num[] = 11, 99, ...
        # are not work counts), whatever getWorkNum computes
        n = n_model
        w = 0x14 + 4 * n_grp
    else:
        n, w = n_model, 0x10
    L.arr(w, '9f4BfI20sI', n)
    tables, absent = [], []
    for field, t, tag in ((4, bt, 'BIN'), (8, tt, 'TPL'), (0xC, mt, 'FCV')):
        if t >= len(d):
            absent.append(field)       # "no table": offset == SMD size, rewritten to the new size
        else:
            later = [x for x in (bt, tt, mt) if x > t] + [len(d)]
            tb = _slot_table(L, field, t, tag, min(later))
            if tb:
                tables.append(tb)
    head_end = min([tb.ofs for tb in tables] + [len(d)])
    return L, head_end, tables, len(d), absent


def _shd(d, e):
    L = _L(d, e)
    ver, _, n, t = L.get('BBHI', 0)
    if ver > 0x41:
        raise ConvertError(f'SHD: version {ver:#x}')
    L.add(0, 8, 'BBHI')
    L.add(8, 0x10, None)
    L.arr(0x10, '9fBBB33s', n)
    tables = [_slot_table(L, 4, t, 'BIN', len(d))] if t < len(d) else []
    return L, (t if tables else len(d)), [tb for tb in tables if tb], len(d), ([] if tables else [4])


def _counted_tables(L, d, spec):
    """Counted tables at the given head fields; offset 0 = no table (TEX of r20f: id table only)."""
    tables = [_counted_table(L, f, t, tag) for f, t, tag in spec if t]
    head_end = min([tb.ofs for tb in tables] + [len(d)])
    return L, head_end, tables, len(d), []


def _idtbl(L, t):
    n = L.u32(t)
    L.add(t, t + 4, 'I')
    L.arr(t + 4, 'HHI', n)
    return t + 4 + 8 * n


def _tex(d, e):
    L = _L(d, e)
    ver, o_id, o_tpl, o_anm = L.get('4I', 0)
    if ver != 3:
        raise ConvertError(f'TEX: version {ver}')
    L.add(0, 0x10, 'I')
    _idtbl(L, o_id)
    return _counted_tables(L, d, ((8, o_tpl, 'TPL'), (0xC, o_anm, 'ANM')))


def _itm(d, e):
    L = _L(d, e)
    x0, o_id, o_bin, o_tpl = L.get('4I', 0)
    L.add(0, 0x10, 'I')
    _idtbl(L, o_id)
    return _counted_tables(L, d, ((8, o_bin, 'BIN'), (0xC, o_tpl, 'TPL')))


# ---- EFF (eff_sys.cpp:40 ESP_EFF_HEADER, :329 EspDataLoad) and its nested records ----------------------------------
# u32 Num 0xB; u32 Tpl/Est/Sst/Path/Obj id table offsets @4..0x14 (EffIdTbl {u32 num; {u16 id, type; u32 Flg}[num]};
# the u16 id doubles as {u8 x0, id} in SstList (espgen.h:30), see the port note in FORMATS.md); u32 Tpl, Anm, Est,
# Sst, Path, Obj table offsets @0x18..0x2C (EffOfsTbl {u32 num; u32 ofs[num] relative to the table}; 0 = none):
# TPL, cAnm, cEspSeqHead (EST and SST, esp.h:105), Path (path.h:26 {u16 num; pad 2; PATH_UNIT 0x28 {Vec pos, normal;
# f32 Length; u8 weight_id[3], weight_num, weight_rate[3]; pad 5}[num]}), EffEfmEnt (eff_sys.cpp:33 {u32 x0,
# ofsModel, ofsTpl, ofsMot, ofsX} relative to the entry -> BIN, TPL, EspEfmMotTbl {u32 num; u32 ofs[]} of FCV
# motions, shape header (FCV layout)).

ESQ_HEAD = '4HHBB3f3fBBHII'     # u16 num + 3 more u16 (PS2 cEspSeqHead data_num[4]); records = num
ESQ_TBL = ('4BH2BI' '9f' 'f' '21f' '5f' '4B' '4f' '6H' 'BbH' '8B' '4B' 'II' 'I' '9f' '4B' '4s' '4B' '4B' '4b' '4h'
           '3f' '4b' '4B')
PATH_UNIT = '3f3ff3BB3B5s'
assert struct.calcsize('<' + ESQ_HEAD) == 0x30 and struct.calcsize('<' + ESQ_TBL) == 0x12C
assert struct.calcsize('<' + PATH_UNIT) == 0x28


def _esq_layout(d, e):
    L = _L(d, e)
    n = L.get('H', 0)
    L.add(0, 0x30, ESQ_HEAD)
    end = L.arr(0x30, ESQ_TBL, n)
    # stale tool data after the records (r11c: 0x38C bytes, core #1: two blocks) is not referenced: copied
    L.add(end, len(d), None)
    return L.finish()


def _pth_layout(d, e):
    L = _L(d, e)
    n = L.get('H', 0)
    L.add(0, 4, 'H2s')
    L.arr(4, PATH_UNIT, n)
    return L.finish()


SPAN_NESTED = {'ESQ': _esq_layout, 'PTH': _pth_layout}


def _eff(d, e):
    L = _L(d, e)
    h = L.get('12I', 0)
    if h[0] != 0xB:
        raise ConvertError(f'EFF: version {h[0]:#x}')
    L.add(0, 0x30, 'I')
    for o in sorted(set(x for x in h[1:6] if x)):
        _idtbl(L, o)
    spec = [(0x18 + 4 * i, h[6 + i], tag) for i, tag in enumerate(('TPL', 'ANM', 'ESQ', 'ESQ', 'PTH', 'EFM'))]
    return _counted_tables(L, d, spec)


def _efm(d, e):
    L = _L(d, e)
    x0, om, ot, omot, ox = L.get('5I', 0)
    L.add(0, 0x14, 'I')
    tables = [_single(4, om, 'BIN'), _single(8, ot, 'TPL')]
    if omot:
        tables.append(_counted_table(L, 0xC, omot, 'FCV'))
    if ox:
        tables.append(_single(0x10, ox, 'FCV'))
    head_end = min(tb.ofs for tb in tables)
    return L, head_end, tables, len(d), []


PARSERS = {'SMD': _smd, 'SHD': _shd, 'TEX': _tex, 'ITM': _itm, 'EFF': _eff, 'EFM': _efm}


# ---- nested conversion ----------------------------------------------------------------------------------------

def _anm_layout(d, e):
    L = _L(d, e)
    L.add(0, 0x10, ANM_HEAD)
    L.add(0x10, len(d), None)
    return L.spans


def _conv_nested(tag, data):
    if tag in SPAN_NESTED:
        return apply(data, SPAN_NESTED[tag](data, '>')), {'tag': tag, 'status': 'converted', 'in': len(data),
                                                           'out': len(data)}
    if tag == 'EFM':
        out, info = convert('EFM', data)
        return out, {'tag': 'EFM', 'status': info['status'], 'in': len(data), 'out': len(out),
                     'files': info['entries']}
    if tag == 'ANM':
        if len(data) < 0x10:
            raise ConvertError('ANM: short')
        return apply(data, _anm_layout(data, '>')), {'tag': 'ANM', 'status': 'converted', 'in': len(data),
                                                        'out': len(data)}
    if tag == 'TPL' and len(data) >= 12 and struct.unpack_from('>3I', data) == (0x0020AF30, 0, 0xC):
        # empty TEXPalette (no textures; SMD of r100 / r100_00): only the header words to swap
        out = struct.pack('<3I', 0x0020AF30, 0, 0xC) + data[12:]
        return out, {'tag': 'TPL', 'status': 'converted', 'in': len(data), 'out': len(out), 'note': 'empty TPL'}
    from . import container
    return container.convert_entry(tag, data)


def _check_nested(tag, out, orig):
    if out.rstrip(b'\x00') == orig.rstrip(b'\x00'):
        return []           # copied verbatim (the nested converter refused it)
    if tag == 'TPL' and len(orig) >= 12 and struct.unpack_from('>3I', orig) == (0x0020AF30, 0, 0xC):
        return [] if out[:12] == struct.pack('<3I', 0x0020AF30, 0, 0xC) and out[12:] == orig[12:] else ['empty TPL']
    if tag in SPAN_NESTED:
        sp, sp0 = SPAN_NESTED[tag](out, '<'), SPAN_NESTED[tag](orig, '>')
        return [] if sp == sp0 and values(out, sp, '<') == values(orig, sp0, '>') else [f'{tag} values differ']
    if tag == 'EFM':
        return check('EFM', out, orig)
    if tag == 'ANM':
        sp = _anm_layout(out, '<')
        return [] if values(out, sp, '<') == values(orig, _anm_layout(orig, '>'), '>') else ['ANM values differ']
    from . import container
    return container.check_entry(tag, out, orig)


def _head_ok(L, head_end):
    """The head's spans, clipped to the head (tables are rebuilt), gaps zero-checked."""
    sub = _L(L.d[:head_end], L.e)
    sub.spans = [s for s in L.spans if s[1] <= head_end]
    return sub.finish()


def convert(tag, d: bytes):
    """-> (bytes, info) with info {'status', 'entries': [nested recs]}."""
    if not d.strip(b'\x00').strip(b'\xcd'):
        return d, {'status': 'converted', 'entries': [], 'empty': True}
    L, head_end, tables, end, absent = PARSERS[tag](d, '>')
    head_spans = _head_ok(L, head_end)
    out = bytearray(apply(d[:head_end], head_spans))
    files = _entries(d, tables, end)
    recs, done = [], {}
    for tb in sorted(tables, key=lambda t: t.ofs):
        while len(out) % ALIGN:
            out.append(0)
        t_new = len(out)
        struct.pack_into('<I', out, tb.field, t_new)
        if tb.single:
            b, ftag = files[tb.ofs]
            conv, rec = _conv_nested(ftag, d[tb.ofs:b])
            rec['ofs'] = tb.ofs
            recs.append(rec)
            out += conv
            continue
        hdr = bytearray(tb.gap)
        new_slots = []
        body = bytearray()
        for s in tb.slots:
            if not s:
                new_slots.append(0)
                continue
            a = tb.ofs + s
            if a not in done:
                b, ftag = files[a]
                conv, rec = _conv_nested(ftag, d[a:b])
                rec['ofs'] = a
                recs.append(rec)
                pos = _align(t_new + tb.gap + len(body)) - t_new
                body += bytes(pos - tb.gap - len(body)) + conv
                done[a] = pos
            new_slots.append(done[a])
        # (shared slots inside one table only; a slot reused across tables is converted again)
        if tb.counted:
            struct.pack_into('<I', hdr, 0, len(tb.slots))
            struct.pack_into(f'<{len(new_slots)}I', hdr, 4, *new_slots)
        else:
            struct.pack_into(f'<{len(new_slots)}I', hdr, 0, *new_slots)
        out += hdr + body
        done = {}
    while len(out) % ALIGN:
        out.append(0)
    for f in absent:
        struct.pack_into('<I', out, f, len(out))
    bad = [r for r in recs if r['status'] not in ('converted', 'empty')]
    return bytes(out), {'status': 'partial' if bad else 'converted', 'entries': recs}


def parse_le(tag, d: bytes):
    """Converted entry -> (head spans, head_end, tables, files {start: (end, tag)})."""
    L, head_end, tables, end, absent = PARSERS[tag](d, '<')
    return _head_ok(L, head_end), head_end, tables, _entries(d, tables, end), absent


def check(tag, out: bytes, original=None) -> list:
    if not out.strip(b'\x00').strip(b'\xcd'):
        return [] if original is None or out == original else [f'{tag}: empty entry differs']
    try:
        sp, he, tbs, files, absent = parse_le(tag, out)
    except (ConvertError, struct.error) as ex:
        return [f'{tag}: {ex}']
    if original is None:
        return []
    L0, he0, tbs0, end0, absent0 = PARSERS[tag](original, '>')
    sp0 = _head_ok(L0, he0)
    if [(s[1] - s[0], s[2]) for s in sp] != [(s[1] - s[0], s[2]) for s in sp0] or he != he0:
        return [f'{tag}: head layout differs']
    fields = {tb.field for tb in tbs0} | set(absent0)
    if set(absent) != set(absent0):
        return [f'{tag}: absent tables differ']
    v_out = values(out, sp, '<')
    v_in = values(original, sp0, '>')
    # the table-offset fields are the only head values allowed to change: compare with them masked
    mo = bytearray(out[:he])
    mi = bytearray(apply(original[:he0], sp0))
    for f in fields:
        mo[f:f + 4] = mi[f:f + 4] = b'\0\0\0\0'
    if bytes(mo) != bytes(mi) or len(v_out) != len(v_in):
        return [f'{tag}: head values differ']
    files0 = _entries(original, tbs0, end0)
    probs = []
    if len(tbs) != len(tbs0):
        return [f'{tag}: {len(tbs)} tables, original {len(tbs0)}']
    for tb, tb0 in zip(sorted(tbs, key=lambda t: t.ofs), sorted(tbs0, key=lambda t: t.ofs)):
        if tb.single:
            for p in _check_nested(tb.tag, out[tb.ofs:files[tb.ofs][0]], original[tb0.ofs:files0[tb0.ofs][0]]):
                probs.append(f'{tag}/{tb.tag}@{tb0.ofs:#x}: {p}')
            continue
        if len(tb.slots) != len(tb0.slots) or [bool(s) for s in tb.slots] != [bool(s) for s in tb0.slots]:
            probs.append(f'{tag}: table {tb.tag} slots differ')
            continue
        for s, s0 in zip(tb.slots, tb0.slots):
            if not s:
                continue
            a, a0 = tb.ofs + s, tb0.ofs + s0
            o_bytes = out[a:files[a][0]]
            i_bytes = original[a0:files0[a0][0]]
            for p in _check_nested(tb.tag, o_bytes, i_bytes):
                probs.append(f'{tag}/{tb.tag}@{a0:#x}: {p}')
    return probs


# ---- EVS (event.cpp:100 EvsHeader / EvsEntry, :2868 EventMgr::SetEvs) ---------------------------------------------
# s32 num; u32 tblOfs (relative to the EVS) -> EvsEntry {u32 ofs (event block, relative to the EVS); u32 x4}[num].
# The event blocks are converted by container.convert_event (their size changes with TPLs) and re-laid out
# 0x20-aligned after the head; the table offsets are rewritten.

def convert_evs(d: bytes):
    from . import container
    n, t = struct.unpack_from('>iI', d, 0)
    if n == 0:
        return struct.pack('<iI', 0, t) + d[8:], {'status': 'converted', 'entries': [], 'events': 0}
    if n < 0 or t + 8 * n > len(d):
        raise ConvertError(f'EVS: {n} entries at {t:#x}')
    ents = [struct.unpack_from('>II', d, t + 8 * i) for i in range(n)]
    starts = sorted({o for o, _ in ents})
    head_end = starts[0]
    if t + 8 * n > head_end:
        raise ConvertError('EVS: table overlaps the first event')
    out = bytearray(d[:head_end])
    struct.pack_into('<iI', out, 0, n, t)
    new, recs = {}, []
    for a, b in zip(starts, starts[1:] + [len(d)]):
        while len(out) % ALIGN:
            out.append(0)
        conv, info = container.convert_event(d[a:b])
        new[a] = len(out)
        recs.append({'tag': 'EVT', 'ofs': a, 'status': info['status'], 'in': b - a, 'out': len(conv),
                     'files': info['entries'], 'packets': info.get('packets')})
        out += conv
    for i, (o, x4) in enumerate(ents):
        struct.pack_into('<II', out, t + 8 * i, new[o], x4)
    bad = [r for r in recs if r['status'] != 'converted']
    return bytes(out), {'status': 'partial' if bad else 'converted', 'entries': recs, 'events': n}


def check_evs(out: bytes, original=None) -> list:
    from . import container
    n, t = struct.unpack_from('<iI', out, 0)
    if original is None:
        return [] if n >= 0 else ['EVS: negative count']
    n0, t0 = struct.unpack_from('>iI', original, 0)
    if (n, t) != (n0, t0):
        return ['EVS: header differs']
    if n == 0:
        return [] if out[8:] == original[8:] else ['EVS: body differs']
    probs = []
    e_out = sorted({struct.unpack_from('<I', out, t + 8 * i)[0] for i in range(n)})
    e_in = sorted({struct.unpack_from('>I', original, t0 + 8 * i)[0] for i in range(n)})
    for (a, b), (a0, b0) in zip(zip(e_out, e_out[1:] + [len(out)]), zip(e_in, e_in[1:] + [len(original)])):
        _, info = container.convert_event(original[a0:b0])
        probs += [f'EVS@{a0:#x}: {p}' for p in container.check_event(out[a:b], original[a0:b0], info)]
    return probs
