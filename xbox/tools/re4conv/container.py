"""Containers: hakase .drs/.das, bare bodies (.dat), event blocks (.evd), nested ETM.

Struct truth (see FORMATS.md section 1 for the full notes):
  hakase   0x20 signature (bytes, kept) + 32-byte records {u32 type, size, 0, offset, p0, p1, 0, 0}
           terminated by type 0xFFFFFFFF, zero to 0x400 (tools/drs.py). Record 0 = body at 0x400,
           record 1 = type 4 sound bank or 0xFFFFFFFE (size 0, offset = file size). Output: every
           record word little-endian, body re-laid out, sound bank appended unchanged (passthrough,
           DSP-ADPCM is the ADPCM task's) at the new offset.
  .das     the same with a yz2 body: decompressed (tools/motion/archive.load_das); the Xbox never sees yz2.
  body     u32 count, rel_offset, 0, 0; u32 ofs[count]; char tag[count][4]; entries 0x20-aligned, an
           entry runs to the next offset (PL_ARC_PTR(arc, no) = arc->ofs[no] + arc, read.cpp GetDataExt).
           Output: same header layout little-endian, entries converted by tag and re-padded to 0x20
           (TPL entries change size); rel_offset != 0 -> the REL is dropped (static link), rel_offset
           written as 0, the REL size recorded in the manifest.
  event    EvtHeader (include/event.h:71): char tag[0x20], room[8]@0x20, no[0xC]@0x28, s32 sndFlag@0x34,
           pad@0x38, u32 pacOfs@0x40, pacSize@0x44, s32 nBin@0x48, u32 binOfs@0x4C; EvtBinEntry 0x40
           {name[0x30]; u32 ofs@0x30; u32 size@0x34; pad}. Header words and bin table swapped, bins
           converted by file extension and re-laid out 0x20-aligned after the original first-bin
           offset. The packet stream (pacOfs..pacOfs+pacSize, EvtPacket event.h:160) stays BIG-ENDIAN
           (second pass) -> the event's status is 'partial'.
  ETM      EtcArc (EtcModel.cpp:36): u32 num @0, files from 0x20 {u32 size (record stride) @0;
           char name[0x20] @0x20; data @0x40}; record sizes swapped, files converted by extension.

Each convert_* returns (bytes, info) where info is a manifest dict: status 'converted' (every entry
converted or legitimately empty), 'partial' (some entries passthrough/failed) and per-entry records
{i, tag, status, in, out[, error]}. A failed entry is copied verbatim and listed with its error.
"""
import struct

import drs        # tools/
import archive    # tools/motion
import yz2        # tools/motion

from . import ConvertError, NotThisFormat
from . import model, tpl, motion

ALIGN = 0x20
TABLE_SIZE = 0x400


def _align(n, a=ALIGN):
    return (n + a - 1) & ~(a - 1)


def _pad(b, fill=b'\0'):
    return b + fill * (_align(len(b)) - len(b))


# ---- entry dispatch ---------------------------------------------------------------------------------

ENTRY = {
    'BIN': (model.convert, model.check),
    'TPL': (tpl.convert, tpl.check),
    'FCV': (motion.convert, motion.check),
    'SEQ': (motion.convert_seq, motion.check_seq),
}


def convert_entry(tag: str, d: bytes):
    """-> (bytes, rec). rec['status']: converted / empty / passthrough / failed."""
    rec = {'tag': tag or '-', 'in': len(d)}
    if not d or (not tag and not any(d)):
        rec.update(status='empty', out=len(d))
        return d, rec
    try:
        if tag == 'ETM':
            out, sub = convert_etm(d)
            rec['files'] = sub['entries']
            rec['status'] = sub['status']
        elif tag in ENTRY:
            out = ENTRY[tag][0](d)
            rec['status'] = 'converted'
        else:
            out = d
            rec['status'] = 'passthrough'
    except NotThisFormat as e:
        out = d
        rec.update(status='passthrough', note=str(e))
    except (ConvertError, ValueError, IndexError, struct.error, AssertionError) as e:
        out = d
        rec.update(status='failed', error=f'{type(e).__name__}: {e}')
    rec['out'] = len(out)
    return out, rec


def check_entry(tag, d, original=None):
    if tag in ENTRY:
        return ENTRY[tag][1](d, original)
    return []


def _status(recs):
    bad = [r for r in recs if r['status'] in ('passthrough', 'failed', 'partial')]
    return 'converted' if not bad else 'partial'


# ---- bodies -----------------------------------------------------------------------------------------

def pack_body(entries, endian='<'):
    """[(tag4 bytes, data)] -> body bytes (rel_offset 0), entries 0x20-padded."""
    count = len(entries)
    hdr_end = 16 + 8 * count
    off = _align(hdr_end)
    offsets = []
    datas = []
    for _, data in entries:
        offsets.append(off)
        data = _pad(data, b'\xcd')
        datas.append(data)
        off += len(data)
    out = bytearray(struct.pack(f'{endian}4I', count, 0, 0, 0))
    out += struct.pack(f'{endian}{count}I', *offsets)
    for tag, _ in entries:
        out += tag
    out += b'\0' * (_align(hdr_end) - len(out))
    for data in datas:
        out += data
    return bytes(out)


def _convert_entries(entries):
    out, recs = [], []
    for i, (tag4, data) in enumerate(entries):
        tag = drs.tag_name(tag4) if isinstance(tag4, bytes) else tag4
        conv, rec = convert_entry(tag, data)
        rec['i'] = i
        recs.append(rec)
        out.append((tag4 if isinstance(tag4, bytes) else tag4.encode().ljust(4, b'\0'), conv))
    return out, recs


def convert_body(d: bytes):
    """Bare body (.dat): -> (bytes, info)."""
    ents = archive.parse_body(d, '>')
    conv, recs = _convert_entries([(t.encode().ljust(4, b'\0'), b) for t, b in ents])
    return pack_body(conv), {'kind': 'body', 'status': _status(recs), 'entries': recs}


def parse_body_le(d: bytes):
    return archive.parse_body(d, '<')


# ---- hakase -----------------------------------------------------------------------------------------

def _records_le(sig, recs):
    out = bytearray(sig)
    for r in recs:
        out += struct.pack('<8I', *r)
    out += struct.pack('<8I', drs.END, 0, 0, 0, 0, 0, 0, 0)
    return bytes(out) + b'\0' * (TABLE_SIZE - len(out))


def _hakase_out(a, body, info):
    if a.snd is None:
        snd = b''
        snd_rec = (drs.EMPTY, 0, 0, TABLE_SIZE + len(body), 0, 0, 0, 0)
    else:
        snd = a.snd.pack()
        snd_rec = (4, len(snd), 0, TABLE_SIZE + len(body), 0, 0, 0, 0)
        info['sound'] = {'status': 'passthrough', 'size': len(snd),
                         'records': [{'type': t, 'p0': p0, 'p1': p1, 'size': len(b)} for t, p0, p1, b in a.snd.records]}
    return _records_le(a.sig, [(0, len(body), 0, TABLE_SIZE, 0, 0, 0, 0), snd_rec]) + body + snd


def open_hakase(d: bytes):
    """(drs.Drs with the body decompressed, yz2 info or None) — archive.load_das from bytes."""
    if not yz2.is_yz2(d, TABLE_SIZE):
        return drs.Drs(d), None
    sig, recs = drs.read_records(d)
    body, packed, _ = yz2.decode(d, TABLE_SIZE)
    yinfo = {'packed': packed, 'unpacked': len(body)}
    body = _pad(body)
    snd_rec = recs[1]
    snd = b'' if snd_rec[0] == drs.EMPTY else d[snd_rec[3]:snd_rec[3] + snd_rec[1]]
    snd_rec = ((drs.EMPTY, 0, 0, TABLE_SIZE + len(body), 0, 0, 0, 0) if snd_rec[0] == drs.EMPTY else
               (snd_rec[0], snd_rec[1], 0, TABLE_SIZE + len(body), snd_rec[4], snd_rec[5], 0, 0))
    d = bytes(drs.write_records(sig, [(0, len(body), 0, TABLE_SIZE, 0, 0, 0, 0), snd_rec]) + body + snd)
    return drs.Drs(d), yinfo


def convert_hakase(d: bytes):
    """.drs / .das (yz2 or plain body) -> (bytes, info)."""
    info = {'kind': 'hakase'}
    a, yinfo = open_hakase(d)
    if yinfo:
        info['yz2'] = yinfo
    conv, recs = _convert_entries(a.entries)
    if a.rel is not None:
        info['rel_dropped'] = len(a.rel)
    body = pack_body(conv)
    info['status'] = _status(recs)
    info['entries'] = recs
    return _hakase_out(a, body, info), info


def parse_hakase_le(d: bytes):
    """Converted container -> (sig, records, [(tag, bytes)], sound bytes)."""
    sig = d[:0x20]
    recs = []
    o = 0x20
    while True:
        r = struct.unpack_from('<8I', d, o)
        o += 32
        if r[0] == drs.END:
            break
        recs.append(r)
        if len(recs) > 8:
            raise ConvertError('container: record table not terminated')
    body = d[recs[0][3]:recs[0][3] + recs[0][1]]
    ents = archive.parse_body(body, '<')
    snd = d[recs[1][3]:recs[1][3] + recs[1][1]] if len(recs) > 1 and recs[1][0] != drs.EMPTY else b''
    return sig, recs, ents, snd


# ---- event ------------------------------------------------------------------------------------------

def convert_event(d: bytes):
    if d[:5] != b'event':
        raise ConvertError('not an event block')
    bins = archive.parse_event(d)       # validates layout
    n_bin, bin_ofs = struct.unpack_from('>iI', d, 0x48)
    first = min(struct.unpack_from('>I', d, bin_ofs + 0x40 * i + 0x30)[0] for i in range(n_bin)) if n_bin else len(d)
    o = bytearray(d[:first])
    for ofs in (0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C):
        o[ofs:ofs + 4] = o[ofs:ofs + 4][::-1]
    recs = []
    for i, (name, data) in enumerate(bins):
        ext = name.rsplit('.', 1)[-1].lower() if '.' in name else ''
        tag = archive.EXT_TAG.get('.' + ext, '')
        conv, rec = convert_entry(tag, data) if tag else (data, {'tag': ext, 'in': len(data), 'out': len(data),
                                                                  'status': 'passthrough'})
        rec.update(i=i, name=name)
        recs.append(rec)
        o += b'\0' * (_align(len(o)) - len(o))
        e = bin_ofs + 0x40 * i
        o[e + 0x30:e + 0x38] = struct.pack('<II', len(o), len(conv))
        o += conv
    o += b'\0' * (_align(len(o)) - len(o))
    pac_ofs, pac_size = struct.unpack_from('>II', d, 0x40)
    info = {'kind': 'event', 'status': 'partial', 'entries': recs,
            'packets': {'status': 'passthrough', 'ofs': pac_ofs, 'size': pac_size, 'note': 'packet stream big-endian'}}
    return bytes(o), info


def parse_event_le(d: bytes):
    n_bin, bin_ofs = struct.unpack_from('<iI', d, 0x48)
    out = []
    for i in range(n_bin):
        e = d[bin_ofs + 0x40 * i:bin_ofs + 0x40 * (i + 1)]
        name = e[:0x30].split(b'\0')[0].decode('ascii')
        ofs, size = struct.unpack('<2I', e[0x30:0x38])
        if ofs + size > len(d):
            raise ConvertError(f'event bin {name} outside the block')
        out.append((name, d[ofs:ofs + size]))
    return out


# ---- ETM --------------------------------------------------------------------------------------------

def convert_etm(d: bytes):
    files = archive.parse_etm(d, '>')
    o = bytearray(struct.pack('<I', len(files)) + d[4:0x20])
    recs = []
    p = 0x20
    for i, (name, data) in enumerate(files):
        rec_hdr = bytearray(d[p:p + 0x40])
        size_in, = struct.unpack_from('>I', d, p)
        tag = archive.EXT_TAG.get('.' + name.rsplit('.', 1)[-1].lower(), '') if '.' in name else ''
        conv, rec = convert_entry(tag, data) if tag else (data, {'tag': '-', 'in': len(data), 'out': len(data),
                                                                  'status': 'passthrough'})
        rec.update(i=i, name=name)
        recs.append(rec)
        body = _pad(conv)
        rec_hdr[0:4] = struct.pack('<I', 0x40 + len(body))
        o += rec_hdr + body
        p += size_in
    return bytes(o), {'kind': 'ETM', 'status': _status(recs), 'entries': recs}


def parse_etm_le(d: bytes):
    return archive.parse_etm(d, '<')


# ---- checks -----------------------------------------------------------------------------------------

def _check_list(ents_out, ents_in, recs):
    probs = []
    if len(ents_out) != len(ents_in):
        return [f'entry count {len(ents_out)} != {len(ents_in)}']
    for rec, (tag, out), (_, orig) in zip(recs, ents_out, ents_in):
        if rec['status'] == 'converted' and tag in ENTRY:
            for p in check_entry(tag, out, orig):
                probs.append(f'entry {rec["i"]} {tag}: {p}')
        elif rec['status'] in ('passthrough', 'failed', 'empty') and out.rstrip(b'\xcd') != orig.rstrip(b'\xcd') \
                and out.rstrip(b'\0') != orig.rstrip(b'\0'):
            probs.append(f'entry {rec["i"]} {tag}: passthrough bytes differ')
    return probs


def check_hakase(out: bytes, original: bytes, info: dict):
    try:
        _, _, ents, _ = parse_hakase_le(out)
    except (ConvertError, ValueError, struct.error) as e:
        return [f'container: {e}']
    ents_in = [(drs.tag_name(t), b) for t, b in open_hakase(original)[0].entries]
    return _check_list(ents, ents_in, info['entries'])


def check_body(out, original, info):
    try:
        ents = archive.parse_body(out, '<')
    except ValueError as e:
        return [f'body: {e}']
    return _check_list(ents, archive.parse_body(original, '>'), info['entries'])


def check_event(out, original, info):
    try:
        ents = parse_event_le(out)
    except (ConvertError, ValueError, struct.error) as e:
        return [f'event: {e}']
    ins = archive.parse_event(original)
    tagged = [(archive.EXT_TAG.get('.' + n.rsplit('.', 1)[-1].lower(), ''), b) for n, b in ents]
    return _check_list(tagged, ins, info['entries'])
