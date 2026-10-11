"""Sound data (CT-14): sound banks, wavetables, stream headers. Headers and tables become little-endian;
the sample data stays GameCube DSP-ADPCM (bytes and nibbles, endian-neutral), because the port's first
audio backend is a software mixer that decodes DSP-ADPCM (xbox/port/snd/dspadpcm.c). Layout and sizes
are unchanged, so every DVD offset (bio4midi.hed, doorse.hed, record offsets) stays valid.

Struct truth (decomp line numbers on branch xbox):

  bank      a DvdHeader table (include/dvd.h:28; cDvdQueue, dvd.cpp:656-735): char sig[0x20] (kept), then
            0x20-byte records {u32 type, size, dest, ofs, sndType, sndArg, sndNo, x1C} terminated by type
            0xFFFFFFFF, zero to 0x400. type 1 TRANS_SND_BLK -> MRAM block (converted, below), type 2
            TRANS_SND_PCM -> ARAM samples (raw DSP-ADPCM, copied). Found as: the type-4 (TRANS_NEST) record
            of em/*.drs, etc/core.das, St*/rNNN.das; each entry of bgm/doorse.dat and bgm/bio4midi.dat
            (banks back to back, 0x20-aligned). Rooms carry two blocks (sndType 6 room, 5 foot).
  MRAM blk  SndBlkInit (snd.cpp:2486): sndType 3/4 (BGM, bio4midi.dat) -> the block IS an ISS block;
            otherwise u32 iss_ofs@0, u32 rnd_ofs@4, zero to iss_ofs. rnd table (seRandomCheck snd.cpp:640):
            u32 tbl[32] relative to the table (tbl[0] unused), tbl[g] -> SBL_DATA {u16 num, last, e[num]}
            (snd.cpp:632).
  ISS       Snd_iss_blk_init (snd_sub3.cpp:6): u32 sit_num, wt_ofs, sit_ofs, seq_ofs (relative to the ISS
            block; seq_ofs 0 = none). SND_SIT 0x18 (snd_drv.h:22): u16 note@0, s8 x8 @2, u16 pitch_l@A,
            pitch_h@C, u8 x8 @E, u16 flag@16. Sequence table (snd_seq3.cpp:152): u32 count, u32 ofs[count]
            relative to the table, then MIDI bytes (byte reads only, snd_seq2.cpp:191 -> kept).
  WT        WTFILEHEADER (snd_drv.h:104): u32 offsetPercussiveInst, offsetMelodicInst, offsetRegions,
            offsetArticulations, offsetSamples, offsetAdpcmContext (relative to the WT); tables run to the
            next offset (counts are not stored). include/dolphin/syn.h: WTINST u16 keyRegion[] (truncated
            to the keys used, or 128), WTREGION 0x18 {u8 unityNote, keyGroup; s16 fineTune; s32 attn;
            u32 loopStart, loopLength, articulationIndex, sampleIndex}, WTART 0x50 (20 x s32), WTSAMPLE 0x10
            {u16 format, sampleRate; u32 offset, length; u16 adpcmIndex; pad}, WTADPCM 0x2E {u16 a[8][2],
            gain, pred_scale, yn1, yn2, loop_pred_scale, loop_yn1, loop_yn2} (count = max adpcmIndex + 1).
  str hed   bgm/bio4str.hed (DvdRead(0) -> SND_DATA_TOP, snd.cpp:120): u32 ofs[2] -> stream blocks
            (Snd_str_blk_init snd_sub3.cpp:20): u32 rit_num, sbh_ofs, rit_ofs (relative to the block);
            sbh = u32 ofs[] relative to sbh (length = first non-zero offset / 4) -> SND_SHD 0x80
            (snd_drv.h:64: u32 flag, samples, nibbles, rate, start_nbl, lptop_nbl, lpend_nbl, offset;
            u16 coef[16], coefR[16], gain[2], ps[2], yn1[2], yn2[2], lps[2], lyn1[2], lyn2[2]; 4 pad bytes);
            SND_RIT 0x10 (snd_drv.h:47: s16 str_no; s8 ch, poly, vol, pan; pad 2; s8 aux_a, aux_b, pl_id;
            pad; u16 flag@C; s8 span, svol).
  midi hed  bgm/bio4midi.hed (SndMem.bgm_file, snd.cpp:100): u32 file offsets into bio4midi.dat (24).
  door hed  bgm/doorse.hed (SndDoorTbl include/snd.h:74): u32 file_ofs@0, num_ofs@4; u32 ofs[] at
            file_ofs (DvdRead offsets into doorse.dat), u16 count at num_ofs followed by u16 ids.
  .sbb      bio4bgm.sbb / bio4evt.sbb: raw DSP-ADPCM stream data, no header -> copied (status passthrough).

Swapped fields are listed exactly in FORMATS.md ("Sound"). Every converter builds a layout (list of
spans (start, end, fmt) with a struct format per element, None = raw bytes) from the big-endian input;
convert applies it, check rebuilds the layout from the little-endian output and compares every field
value with the big-endian original. Non-zero bytes outside every known span are refused (ConvertError).
"""
import struct

from . import ConvertError
from .spans import _L, apply, values  # noqa: F401  (re-exported)

TABLE_SIZE = 0x400
END = 0xFFFFFFFF
SIG = bytes.fromhex('cab6be20') * 8

SIT = 'H8bHH8BH'           # 0x18
REGION = 'BBhiIIII'        # 0x18
SAMPLE = 'HHIIH2s'         # 0x10
ADPCM_SIZE = 0x2E
SHD = '8I46H4s'            # 0x80
RIT = 'h4b2s3bsH2b'        # 0x10
DVDHDR = '8I'              # 0x20


def _align(n, a=0x20):
    return (n + a - 1) & ~(a - 1)


# ---- layout builders ----------------------------------------------------------------------------------

def _wt(L, base, limit):
    """Wavetable at base, its last table ending at or before limit."""
    if base + 0x18 > limit:
        raise ConvertError(f'WT: header at {base:#x} past {limit:#x}')
    L.add(base, base + 0x18, 'I')
    perc, mel, reg, art, smp, adp = (L.u32(base + 4 * i) for i in range(6))
    if not (0x18 <= perc <= mel <= reg <= art <= smp <= adp <= limit - base):
        raise ConvertError(f'WT at {base:#x}: offsets out of order {perc:#x} {mel:#x} {reg:#x} {art:#x} '
                           f'{smp:#x} {adp:#x} (limit {limit - base:#x})')
    L.add(base + perc, base + reg, 'H')
    L.add(base + reg, base + art, REGION)
    L.add(base + art, base + smp, 'i')
    L.add(base + smp, base + adp, SAMPLE)
    n_adp = 0
    for o in range(base + smp, base + adp, 0x10):
        n_adp = max(n_adp, L.u16(o + 0xC) + 1)
    if base + adp + n_adp * ADPCM_SIZE > limit:
        raise ConvertError(f'WT at {base:#x}: {n_adp} ADPCM contexts past {limit:#x}')
    L.add(base + adp, base + adp + n_adp * ADPCM_SIZE, 'H')
    return {'regions': (art - reg) // 0x18, 'samples': (adp - smp) // 0x10, 'adpcm': n_adp}


def _iss(L, base, limit):
    L.add(base, base + 0x10, 'I')
    sit_num, wt, sit, seq = (L.u32(base + 4 * i) for i in range(4))
    size = limit - base
    # sit == size with sit_num 0: an empty SIT table at the end (em/wep19, wep41, wep42)
    if not (0x10 <= wt < size and 0x10 <= sit <= size and (seq == 0 or 0x10 <= seq < size)):
        raise ConvertError(f'ISS at {base:#x}: offsets {wt:#x} {sit:#x} {seq:#x} outside {size:#x}')
    bounds = sorted({wt, sit, size} | ({seq} if seq else set()))

    def nxt(o):
        i = bounds.index(o)
        return base + bounds[min(i + 1, len(bounds) - 1)]

    info = {'sit': sit_num}
    info.update(_wt(L, base + wt, nxt(wt)))
    if base + sit + 0x18 * sit_num > nxt(sit):
        raise ConvertError(f'ISS at {base:#x}: {sit_num} SITs past the next table')
    L.add(base + sit, base + sit + 0x18 * sit_num, SIT)
    if seq:
        s = base + seq
        n = L.u32(s)
        if s + 4 + 4 * n > nxt(seq):
            raise ConvertError(f'ISS at {base:#x}: sequence table count {n}')
        L.add(s, s + 4 + 4 * n, 'I')
        for i in range(n):
            o = L.u32(s + 4 + 4 * i)
            if not 4 + 4 * n <= o < nxt(seq) - s:
                raise ConvertError(f'ISS at {base:#x}: sequence {i} offset {o:#x}')
        L.add(s + 4 + 4 * n, nxt(seq), None)       # MIDI bytes
        info['seq'] = n
    return info


def _rnd(L, base, limit):
    if base + 0x80 > limit:
        raise ConvertError(f'rnd table at {base:#x} past {limit:#x}')
    L.add(base, base + 0x80, 'I')
    n = 0
    for g in range(32):
        o = L.u32(base + 4 * g)
        if not o:
            continue
        if o < 0x80 or base + o + 4 > limit:
            raise ConvertError(f'rnd table at {base:#x}: entry {g} offset {o:#x}')
        cnt = L.u16(base + o)
        if base + o + 4 + 2 * cnt > limit:
            raise ConvertError(f'rnd table at {base:#x}: entry {g} count {cnt}')
        L.add(base + o, base + o + 4 + 2 * cnt, 'H')
        n += 1
    return n


def _mram(L, base, size, snd_type):
    limit = base + size
    if snd_type in (3, 4):
        return _iss(L, base, limit)
    L.add(base, base + 8, 'I')
    iss, rnd = L.u32(base), L.u32(base + 4)
    if not (8 <= iss < size and iss < rnd <= size - 0x80):
        raise ConvertError(f'MRAM block at {base:#x}: iss {iss:#x} rnd {rnd:#x} size {size:#x}')
    info = _iss(L, base + iss, base + rnd)
    info['rnd'] = _rnd(L, base + rnd, limit)
    return info


def _bank(L, base, limit=None):
    """DvdHeader table + records at base; -> (end offset (0x20-aligned, <= limit), info)."""
    d = L.d
    if d[base:base + 0x20] != SIG:
        raise ConvertError(f'sound bank at {base:#x}: bad signature')
    L.add(base, base + 0x20, None)
    o = base + 0x20
    recs = []
    while True:
        if o + 0x20 > base + TABLE_SIZE:
            raise ConvertError(f'sound bank at {base:#x}: record table not terminated')
        r = struct.unpack_from(L.e + DVDHDR, d, o)
        L.add(o, o + 0x20, 'I')
        o += 0x20
        if r[0] == END:
            break
        recs.append(r)
    end = base + TABLE_SIZE
    blocks = []
    for ty, size, dest, ofs, snd_type, arg, no, x1c in recs:
        if ty not in (1, 2) or dest or no or x1c or ofs < TABLE_SIZE:
            raise ConvertError(f'sound bank at {base:#x}: unexpected record {ty} {size:#x} {dest:#x} {ofs:#x}')
        s = base + ofs
        if s + size > (limit if limit is not None else len(d)):
            raise ConvertError(f'sound bank at {base:#x}: record past the end')
        if ty == 1:
            bi = _mram(L, s, size, snd_type)
            bi.update(type=snd_type, arg=arg, size=size)
            blocks.append(bi)
        else:
            L.add(s, s + size, None)                # DSP-ADPCM samples, endian-neutral
            blocks.append({'pcm': size, 'type': snd_type})
        end = max(end, _align(s + size))
    return end, {'records': len(recs), 'blocks': blocks}


def bank_layout(d: bytes, e='>'):
    L = _L(d, e)
    end, info = _bank(L, 0, len(d))
    if end < len(d) and any(d[end:]):
        raise ConvertError(f'sound bank: {len(d) - end:#x} trailing bytes')
    return L.finish(), info


def banks_layout(d: bytes, e='>'):
    """Concatenated banks (bio4midi.dat, doorse.dat)."""
    L = _L(d, e)
    o, infos = 0, []
    while o < len(d):
        o, info = _bank(L, o, len(d))
        infos.append(info)
    return L.finish(), {'banks': len(infos), 'blocks': sum(len(i['blocks']) for i in infos)}


def str_hed_layout(d: bytes, e='>'):
    L = _L(d, e)
    first = L.u32(0)
    if first % 4 or not 8 <= first < len(d):
        raise ConvertError('bio4str.hed: bad block table')
    n = first // 4
    while n and L.u32(4 * (n - 1)) == 0:
        n -= 1
    blocks = [L.u32(4 * i) for i in range(n)]
    L.add(0, 4 * n, 'I')
    ends = sorted(blocks[1:]) + [len(d)]
    info = []
    for b, lim in zip(blocks, ends):
        L.add(b, b + 12, 'I')
        rit_num, sbh, rit = L.u32(b), L.u32(b + 4), L.u32(b + 8)
        if b + rit + 0x10 * rit_num > lim or b + sbh >= lim:
            raise ConvertError(f'bio4str.hed block at {b:#x}: tables past the block')
        L.add(b + rit, b + rit + 0x10 * rit_num, RIT)
        s = b + sbh
        ofs = []
        o = s
        while True:
            v = L.u32(o)
            if v:
                ofs.append(v)
            o += 4
            if ofs and o >= s + min(ofs):
                break
            if o >= lim:
                raise ConvertError(f'bio4str.hed block at {b:#x}: empty header table')
        L.add(s, s + min(ofs), 'I')
        for v in sorted(set(ofs)):
            if s + v + 0x80 > lim:
                raise ConvertError(f'bio4str.hed block at {b:#x}: SHD {v:#x} past the block')
            L.add(s + v, s + v + 0x80, SHD)
        info.append({'rit': rit_num, 'shd': len(set(ofs))})
    return L.finish(), {'blocks': info}


def midi_hed_layout(d: bytes, e='>'):
    if len(d) % 4:
        raise ConvertError('bio4midi.hed: size')
    return [(0, len(d), 'I')], {'offsets': len(d) // 4}


def door_hed_layout(d: bytes, e='>'):
    L = _L(d, e)
    L.add(0, 8, 'I')
    fo, no = L.u32(0), L.u32(4)
    if no + 2 > len(d) or fo < 8:
        raise ConvertError('doorse.hed: bad offsets')
    n = L.u16(no)
    if fo + 4 * n > no or no + 2 + 2 * n > len(d):
        raise ConvertError(f'doorse.hed: count {n}')
    L.add(fo, fo + 4 * n, 'I')
    # u16 count followed by u16 door ids (one per file, then zero padding)
    L.add(no, no + 2 + 2 * n, 'H')
    return L.finish(), {'files': n}


# ---- convert / check ----------------------------------------------------------------------------------

def _conv(builder):
    def convert(d: bytes) -> bytes:
        spans, _ = builder(d, '>')
        return apply(d, spans)

    def check(out: bytes, original=None) -> list:
        try:
            spans, _ = builder(out, '<')
        except (ConvertError, struct.error) as ex:
            return [f'{builder.__name__}: {ex}']
        if len(out) != (len(original) if original is not None else len(out)):
            return ['size changed']
        if original is not None:
            spans_in, _ = builder(original, '>')
            if spans_in != spans:
                return ['layout differs from the original']
            if values(out, spans, '<') != values(original, spans_in, '>'):
                return ['field values differ from the original']
        return []
    return convert, check


convert_bank, check_bank = _conv(bank_layout)
convert_banks, check_banks = _conv(banks_layout)
convert_str_hed, check_str_hed = _conv(str_hed_layout)
convert_midi_hed, check_midi_hed = _conv(midi_hed_layout)
convert_door_hed, check_door_hed = _conv(door_hed_layout)


def info(builder, d, e='>'):
    return builder(d, e)[1]


# relative path (lower case) -> (format name, convert, check); used by simple.handler-style dispatch
FILES = {
    'bgm/bio4str.hed': ('bio4str_hed', convert_str_hed, check_str_hed),
    'bgm/bio4midi.hed': ('bio4midi_hed', convert_midi_hed, check_midi_hed),
    'bgm/bio4midi.dat': ('sound_banks', convert_banks, check_banks),
    'bgm/doorse.hed': ('doorse_hed', convert_door_hed, check_door_hed),
    'bgm/doorse.dat': ('sound_banks', convert_banks, check_banks),
}


def handler(rel: str):
    return FILES.get(rel.replace('\\', '/').lower())
