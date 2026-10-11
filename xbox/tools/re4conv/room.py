"""Room block entries (CT-14): fixed-layout tables of St*/rNNN.das bodies (and core.das), converted field by
field to little-endian with the span engine (spans.py). Entries holding nested BIN/TPL (SMD, SHD, TEX,
ITM, EFF) live in room_nested.py. Each builder takes (bytes, byte order) and returns (spans, info); see
FORMATS.md section 2 for the per-tag notes. Unknown non-zero bytes outside every known span are refused.

Shared: AREA_HIT_DATA (include/area.h:47) 0x30 = u8 be_flag, type; u16 pad; 11 x f32 (every union body
is f32) -> AREA.
"""
import struct

from . import ConvertError, NotThisFormat
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


# ---- SAR (light_area.cpp:11 LightAreaHed / LightAreaData) ------------------------------------------------
# u32 num, pad to 0x10; 0xD8-byte entries {u8 x0, x1, lightNoPl, lightNoEm; AREA_HIT_DATA@4; u32 flag@34;
# s8 power@38; u8 lightNoSub@39; pad to 0xD8 (copied)}.

SAR_ENT = '4s' + AREA + 'IbB158s'


def sar_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('I', 0)
    L.add(0, 4, 'I')
    L.add(4, 0x10, None)
    L.arr(0x10, SAR_ENT, n)
    return L.finish(), {'areas': n}


# ---- EAR (include/espgen.h:55 SstArea / SstAreaEnt) -------------------------------------------------------
# u32 num, u32 ver_no, pad 8; 0x98-byte entries {u8 no, be_flag, area_no, pad; AREA_HIT_DATA@4; u32 flag@34;
# pad to 0x98 (copied)}.

EAR_ENT = '4s' + AREA + 'I96s'


def ear_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('I', 0)
    L.add(0, 8, 'I')
    L.add(8, 0x10, None)
    L.arr(0x10, EAR_ENT, n)
    return L.finish(), {'areas': n}


# ---- CAM (include/cam_ctrl.h, CameraControl::calcAddr cam_ctrl.cpp:282) ---------------------------------------
# CAM_FILE_HEADER 0x10 {char Version[4] "B40x"; u8 nCdat, nAdat, nLdat; pad}; then CUT_INFO[nAdat] 0x10
# {u8 Attr; pad 7; u32 pAdat, pCdat (file offsets, relocated in place by calcAddr; 0 = none)},
# AREA_DATA[nAdat] 0x30 {u8 Be_flag; s8 No, Suffix; u8 Attr; f32 Dir; u8 Type_char, Type_addr; pad;
# f32 Height@20, Y@24; s32 nVer@28; u32 pVer@2C -> Vec[nVer]}, CAMERA_DATA[nCdat] 0x34 {u8 Be_flag; s8 No, Id;
# u8 Attr; Vec offset@4; u32 pFrame@10 -> u16[nPoint]; f32 floor_ratio@14; pad; s32 nPoint@20; u32 pCampos@24,
# pTarget@28 -> Vec[nPoint]; u32 pRoll@2C, pFovy@30 -> f32[nPoint]}, LERP_DATA[nLdat] 0x10 {u8 Be_flag;
# s8 SrcNo, SrcSuffix, DstNo, DstSuffix; u8 Attr; pad 2; s32 InterFrame@8; pad 4}. Offsets stay offsets.

CAM_HDR = '4s3B9s'
CAM_CUT = 'B7sII'
CAM_AREA = 'BbbBfBB22sffiI'
CAM_DATA = 'BbbB3fIf8siIIII'
CAM_LERP = 'B4bB2si4s'


def cam_layout(d, e='>'):
    L = _L(d, e)
    ver, n_c, n_a, n_l, _ = L.get(CAM_HDR, 0)
    if ver == b'EMPT':      # placeholder (core.das entry 8: "EMPT" + 0xCD fill); cameraDataVersion <= 1
        return [(0, len(d), None)], {'version': 'EMPT'}
    if ver[:3] != b'B40' or not b'2' <= ver[3:4] <= b'4':
        raise ConvertError(f'CAM: version {ver!r}')
    L.add(0, 0x10, CAM_HDR)
    o = L.arr(0x10, CAM_CUT, n_a)
    a0 = o
    o = L.arr(o, CAM_AREA, n_a)
    c0 = o
    o = L.arr(o, CAM_DATA, n_c)
    L.arr(o, CAM_LERP, n_l)
    for i in range(n_a):
        n_ver, p_ver = L.get('iI', a0 + 0x30 * i + 0x28)
        if n_ver:
            L.arr(p_ver, '3f', n_ver)
    junk = 0
    for i in range(n_c):
        c = c0 + 0x34 * i
        p_frame = L.get('I', c + 0x10)
        n_pt, p_pos, p_tgt, p_roll, p_fovy = L.get('iIIII', c + 0x20)
        if n_pt:
            # pFrame is only used by key-framed cameras; others (Id 8 rails) carry junk there
            if p_frame + 2 * n_pt <= len(d):
                L.arr(p_frame, 'H', n_pt)
            else:
                junk += 1
            L.arr(p_pos, '3f', n_pt)
            L.arr(p_tgt, '3f', n_pt)
            L.arr(p_roll, 'f', n_pt)
            L.arr(p_fovy, 'f', n_pt)
    return L.finish(), {'version': ver.decode('ascii', 'replace'), 'cuts': n_c, 'areas': n_a, 'lerps': n_l,
                         'junk_pframe': junk}

# ---- FSE (include/flr_at.h, flr_at.cpp) ----------------------------------------------------------------------
# FLR_AT_HEADER 0x10 {char magic[4] "FSE"; u16 version 0x103; u16 num; u8 cartridge_type, padd1; u16 padd2;
# u32 padd3}; FLR_AT_DATA 0x84 {u8 flag, type, no, group, priority; pad 15; AREA_HIT_DATA@14; payload@44 by
# type: 0 FLR_AT_SE_TYPE (u8 x4), 1 FLR_AT_SE_VOLCTRL (u8 rate[32]), 2 FLR_AT_BGM_VOL {u8 blk_no, sw; s8 set_vol[2];
# int time[2]; u16 str_blk, str_no; u32 str_fade_time}, 3 FLR_AT_THUNDER_VOL (s8 x2)}.

FSE_HDR = '4sHHBBHI'
FSE_HEAD = '5B15s' + AREA
FSE_PAYLOAD = {2: 'BBbb2iHHI44s'}


def fse_layout(d, e='>'):
    L = _L(d, e)
    magic, ver, n = L.get('4sHH', 0)
    if magic[:3] != b'FSE' or ver != 0x103:
        raise ConvertError(f'FSE: magic {magic!r} version {ver:#x}')
    L.add(0, 0x10, FSE_HDR)
    for i in range(n):
        o = 0x10 + 0x84 * i
        L.add(o, o + 0x44, FSE_HEAD)
        L.add(o + 0x44, o + 0x84, FSE_PAYLOAD.get(L.get('B', o + 1), '64s'))
    return L.finish(), {'records': n}


# ---- AEV / ITA (include/sce_at.h, sce_at.cpp:184 SceAtFileHead) -------------------------------------------------
# SceAtFileHead 0x10 {char magic[4] "AEV"/"ITA"; u16 version (0x104 / 0x105); u16 num; pad 8}; records 0x9C =
# SCE_AT_UNIT 0x5C {u32 tag (OT link); AREA_HIT_DATA@4; u8 be_flg, id, no, hit_type, trg_type, target_type,
# task_level, trg_type_bak @34; u32 pParam@3C, pFunc@40 (runtime); u8 priority, kind, waiting_type, waiting_no;
# s8 hit_dir_ang, hit_open_ang; u8 act_type, waiting_etc_id; u32 pParent@4C; s16 parts_no@50; u8 country,
# act_color; pad 8} + payload 0x40 by id (SCEAT_ID; ITA records are SCE_AT_DATA_ITEM).

SCE_UNIT = 'I' + AREA + '8BII4B2b2BIh2B8s'
SCE_PAYLOAD = {
    0: '16I',                       # NORMAL: cModel* pModel[16] (runtime)
    2: '64s',                       # EXEC: payload not read (sceAtFunc_exec sce_at.cpp:852 uses pParam/pFunc)
    1: '3ffBBBBIbBBBI32s',          # DOOR: Vec next_pos; f32 next_ang_y; u8 x4; TaskFunc pExitFunc; u8/s8 x4; void* pExitParam
    3: '3fI3f4H6Bh4f2B2s',          # ITEM (SCE_AT_DATA_ITEM)
    4: 'BBH60s',                    # FLG
    5: 'hhBBHB3s52s',               # MES
    8: 'I60s',                      # SAVE
    9: 'HBB60s',                    # SHD_DISP
    10: 'iBB2sif48s',               # DAMAGE
    11: 'IIB3siiIi36s',             # SCR_AT
    12: '3ffBB2sff36s',             # CAM_CTRL
    13: 'iI10I16s',                 # FIELD_INFO
    15: 'IIBB54s',                  # SKEY
    16: '3ffbBBBBB42s',             # LADDER
    17: 'HH60s',                    # USE
    18: 'BBBB8f3fIB11s',            # HIDE
    19: '3ffB47s',                  # POS_JUMP
}
SCE_ITEM_ID = 3


def sce_layout(d, e='>'):
    L = _L(d, e)
    magic, ver, n = L.get('4sHH', 0)
    kind = magic[:3]
    if (kind, ver) not in ((b'AEV', 0x104), (b'ITA', 0x105)):
        raise ConvertError(f'AEV/ITA: magic {magic!r} version {ver:#x}')
    L.add(0, 8, '4sHH')
    L.add(8, 0x10, None)
    raw_ids = set()
    for i in range(n):
        o = 0x10 + 0x9C * i
        L.add(o, o + 0x5C, SCE_UNIT)
        rid = SCE_ITEM_ID if kind == b'ITA' else L.get('B', o + 0x35)
        f = SCE_PAYLOAD.get(rid)
        if f is None:
            f = '64s'
            if any(d[o + 0x5C:o + 0x9C]):
                raw_ids.add(rid)
        L.add(o + 0x5C, o + 0x9C, f)
    return L.finish(), {'records': n, 'raw_payload_ids': sorted(raw_ids)}

# ---- ETS (include/etc_model.h:122 ETS_DATA) --------------------------------------------------------------------
# u16 num@0, pad to 0x10; 0x28-byte records {u16 id; u16 no (u8 type = low byte); pad to 0x10; Vec ang@10, pos@1C}.

def ets_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('H', 0)
    L.add(0, 2, 'H')
    L.arr(0x10, 'HH12s6f', n)
    return L.finish(), {'records': n}


# ---- ESE (include/snd.h:179 SeAtHead / SE_AT_DATA) ---------------------------------------------------------------
# "ESE", u16 version 0x100, u16 num, pad 8; 0x2C-byte records {u8 be_flg, at_no; u16 flag; Vec pos; u16 x10, blk,
# x14, se_no, interval, delay, ctr; s16 call_num; u16 rnd_base, rnd_interval; pad 8}.

def ese_layout(d, e='>'):
    L = _L(d, e)
    magic, ver, n = L.get('4sHH', 0)
    if magic[:3] != b'ESE' or ver != 0x100:
        raise ConvertError(f'ESE: magic {magic!r} version {ver:#x}')
    L.add(0, 8, '4sHH')
    L.arr(0x10, 'BBH3f7HhHH8s', n)
    return L.finish(), {'records': n}


# ---- DSE (snd.cpp:1658 SndDoorSe) ---------------------------------------------------------------------------------
# u32 num; {u16 room; u16 door[5]}[num].

def dse_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('I', 0)
    L.add(0, 4, 'I')
    L.arr(4, '6H', n)
    return L.finish(), {'records': n}


# ---- CNS (cons.cpp:9 ConsRoom) -------------------------------------------------------------------------------------
# u32 num; u32 bits[(num >> 5) + 1]; u32 values[num].

def cns_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('I', 0)
    L.arr(0, 'I', 1 + (n >> 5) + 1 + n)
    return L.finish(), {'values': n}


# ---- EMI (include/embarrel.h:21 EmiData) ---------------------------------------------------------------------------
# int n@0, pad 4; 0x40-byte entries from 8 {u8 type, sub, state, pad; Vec pos@4; f32 rotY@10; pad to 0x40 (copied)}.

def emi_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('i', 0)
    L.add(0, 4, 'i')
    L.add(4, 8, None)
    L.arr(8, '4s3ff44s', n)
    return L.finish(), {'entries': n}


# ---- STB (include/snd.h:31 SND_INFO, snd.cpp:1605 room start) --------------------------------------------------------
# REVERB_INFO dpl2@0, stereo@20 {u16 Aux_core, Aux_enemy, Aux_weapon, Aux_room; f32 Delay, Time, Coloration, Damping,
# Mix, Crosstalk}; u32 offset_tbl[32]@40 -> SND_COMBINE_TBL 8 bytes (s8/u8 only, copied); offset_vol[32]@C0,
# offset_pitch[32]@140, offset_filter[32]@1C0 -> SND_TBL_INFO {u32 num; f32 scale; {f32 dist; u16 flag; u16 val}[num]}
# (offsets relative to the file, 0 = none; val is read as (s16)/(s8) of the u16 value -> a u16 swap is right).

def stb_layout(d, e='>'):
    L = _L(d, e)
    L.arr(0, '4H6f', 2)
    L.arr(0x40, 'I', 128)
    n_tbl = 0
    for i in range(32):
        o = L.get('I', 0x40 + 4 * i)
        if o:
            L.add(o, o + 8, '8s')
    for o in sorted({L.get('I', 0xC0 + 4 * i) for i in range(96)} - {0}):
        n = L.get('I', o)
        L.add(o, o + 8, 'If')
        L.arr(o + 8, 'fHH', n)
        n_tbl += 1
    return L.finish(), {'curves': n_tbl}


# ---- RTP (include/route_ck.h:26 RTP) ------------------------------------------------------------------------------
# u32 ID "2RTP"; u16 mapped, nPoint, nLine, nNext; u32 pPoint, pLine, pNext (offsets, not relocated); RTP_POINT 0x10
# {Vec pos; u16 offLine, nLine}; RTP_LINE {s16 connect; u16 dist}; next = s8[nPoint][nPoint] (bytes).

def rtp_layout(d, e='>'):
    L = _L(d, e)
    ident, mapped, n_pt, n_ln, n_nx, p_pt, p_ln, p_nx = L.get('4s4H3I', 0)
    if ident != b'2RTP':
        raise ConvertError(f'RTP: id {ident!r}')
    L.add(0, 0x18, '4s4H3I')
    L.arr(p_pt, '3fHH', n_pt)
    L.arr(p_ln, 'hH', n_ln)
    L.add(p_nx, p_nx + n_nx, None)
    return L.finish(), {'points': n_pt, 'lines': n_ln}

# ---- SMX (include/scroll.h:65 cSmxData / cSmxWork, scroll.cpp:297 smxInit) ---------------------------------------
# cSmxData 0x10 {u8 Version, nData; u8 x2; u32 x3} then nData cSmxWork 0x90 {u8 ModelNo, Id, OtType, CullMode;
# u32 LitSelectMask@4, Flag@8; u32 MaterialColor@C; u8 Free[116]@10 (-> cObjScr::free, obj02.cpp); u32
# SpecularColor@84; f32 TexU@88, TexV@8C}. MaterialColor / SpecularColor are RGBA bytes copied with a u32 store
# into cModelInfo::color -> kept as bytes. Free: f32 words (type 2 ObjScrSwingWork 13 f32, r10c/r212/r217 Vec),
# type 1 ObjScrRotWork {Vec rotSpd; u8 flag@C}.

SMX_HDR = 'BB2s3I'


def smx_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('B', 1)
    L.add(0, 0x10, SMX_HDR)
    for i in range(n):
        o = 0x10 + 0x90 * i
        free = '3fB3s25I' if L.get('B', o + 1) == 1 else '29I'
        L.add(o, o + 0x90, 'BBBBII4s' + free + '4sff')
    return L.finish(), {'models': n}

# ---- SAT / EAT (include/atari.h:20 cSatFile, :40 cSatHeader, :51 cSatBlock; atari.cpp:663 create, :1113, :1137) ---
# Table form (m_Version bit7 set, != 0xFF): u8 0x8x, u8 count, 2 bytes (junk, copied), u32 ofs[count] relative to
# the table -> cSatFile. cSatFile 0x14 {u8 m_Version (0x20), x1; u16 nVertex, nNormal, nEdge, x8, nPolygon, nFloor,
# nSlope, nWall, nBlock}; Vec vtx[nVertex], nrm[nNormal], edge[nEdge]; SAT_POLY[nPolygon] 0x14 (at_sub.h:112:
# u16 v[3], m_Normal, e[3], m_Dummy7; u32 m_Status); then the cSatBlock tree: 0x24 {Vec min, m_Size; u16 nFloor,
# nSlope, nWall, m_Flag; u32 m_pList (relative offset of the next sibling, 0 = last; blockInit relocates)}, then
# either (m_Flag bit0) a child block chain or u16 idx[nFloor + nSlope + nWall].

SAT_FILE = 'BB9H'
SAT_POLY = '8HI'
SAT_BLOCK = '6f4HI'


def _sat_blocks(L, o, end, depth=0):
    n = 0
    if depth > 64:
        raise ConvertError('SAT: block tree too deep')
    while True:
        if o + 0x24 > end:
            raise ConvertError(f'SAT: block at {o:#x} past {end:#x}')
        L.add(o, o + 0x24, SAT_BLOCK)
        nf, ns, nw, flag, nxt = L.get('4HI', o + 0x18)
        n += 1
        if flag & 1:
            n += _sat_blocks(L, o + 0x24, end, depth + 1)
        else:
            L.arr(o + 0x24, 'H', nf + ns + nw)
        if not nxt:
            return n
        o += nxt


def _sat_file(L, o, end):
    ver, _, n_v, n_n, n_e, _, n_p, n_f, n_s, n_w, n_b = L.get(SAT_FILE, o)
    if ver != 0x20:
        raise NotThisFormat(f'SAT file version {ver:#x}')
    if n_f + n_s + n_w != n_p:
        raise ConvertError(f'SAT at {o:#x}: polygon groups {n_f}+{n_s}+{n_w} != {n_p}')
    L.add(o, o + 0x14, SAT_FILE)
    p = L.arr(o + 0x14, '3f', n_v + n_n + n_e)
    p = L.arr(p, SAT_POLY, n_p)
    blocks = _sat_blocks(L, p, end) if n_b else 0
    if blocks != n_b:
        raise ConvertError(f'SAT at {o:#x}: {blocks} blocks walked, header says {n_b}')
    return n_p


def sat_layout(d, e='>'):
    L = _L(d, e)
    ver = L.get('B', 0)
    if ver != 0xFF and ver & 0x80:
        n = L.get('B', 1)
        L.add(0, 4, None)
        L.arr(4, 'I', n)
        ofs = [L.get('I', 4 + 4 * i) for i in range(n)]
        bounds = sorted(set(ofs)) + [len(d)]
        polys = [_sat_file(L, o, bounds[bounds.index(o) + 1]) for o in sorted(set(ofs))]
        return L.finish(), {'files': n, 'polygons': sum(polys)}
    n_p = _sat_file(L, 0, len(d))
    return L.finish(), {'files': 1, 'polygons': n_p}

# ---- MDT (include/mes.h:12 MessageData, mes.cpp:231 MesTblBlock / :239 getAddr) ---------------------------------
# u32 nLang (6: JP EN DE FR ES IT), u32 ofs[nLang] relative to the file -> MesTblBlock {u32 x0; u32 count;
# u32 ofs[count] relative to the block} -> u16 message codes (text + control codes, all u16) to the next block.

def mdt_layout(d, e='>'):
    L = _L(d, e)
    n = L.get('I', 0)
    if not 1 <= n <= 16:
        raise ConvertError(f'MDT: {n} languages')
    L.arr(0, 'I', 1 + n)
    blocks = sorted({L.get('I', 4 + 4 * i) for i in range(n)})
    msgs = 0
    for b, end in zip(blocks, blocks[1:] + [len(d)]):
        if b + 8 > len(d):
            continue        # empty MDT (St2 r20f, r21b, r222, ...): every block at 0x1C, cut by the 0xCD fill
        cnt = L.get('I', b + 4)
        t = L.arr(b, 'I', 2 + cnt)
        if t > end:
            raise ConvertError(f'MDT: block at {b:#x} table past {end:#x}')
        # text: u16 up to the block end; trailing pad (0 / 0xCD) of an odd tail is left raw
        tail = d[t:end]
        k = len(tail.rstrip(b'\x00').rstrip(b'\xcd'))
        k += k & 1
        L.add(t, t + k, 'H')
        msgs += cnt
    return L.finish(), {'languages': n, 'messages': msgs}


# ---- registry -----------------------------------------------------------------------------------------

BUILDERS = {
    'BLK': blk_layout,
    'LIT': lit_layout,
    'SAR': sar_layout,
    'EAR': ear_layout,
    'CAM': cam_layout,
    'FSE': fse_layout,
    'AEV': sce_layout,
    'ITA': sce_layout,
    'ETS': ets_layout,
    'ESE': ese_layout,
    'DSE': dse_layout,
    'CNS': cns_layout,
    'EMI': emi_layout,
    'STB': stb_layout,
    'RTP': rtp_layout,
    'SMX': smx_layout,
    'SAT': sat_layout,
    'EAT': sat_layout,
    'MDT': mdt_layout,
}


def layout(tag, d, e='>'):
    """(spans, info); an entry holding only zero / 0xCD fill (empty FSE of r120, r217) is copied."""
    if not d.strip(b'\x00').strip(b'\xcd'):
        return [(0, len(d), None)], {'empty': True}
    return BUILDERS[tag](d, e)


def convert(tag, d: bytes) -> bytes:
    spans, _ = layout(tag, d, '>')
    return apply(d, spans)


def check(tag, out: bytes, original=None) -> list:
    def b(x, e):
        return layout(tag, x, e)
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
