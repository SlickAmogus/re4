# re4conv — formats on the retail discs and their conversion status

Status values: **converted** (byte-swapped / re-encoded, validated), **passthrough** (copied verbatim and
listed in the manifest; the Xbox would read big-endian data — a second-pass item), **dropped** (not
needed on the Xbox). Offsets below are from the decomp's headers (`include/`) and loaders (`src/game/`);
line numbers refer to the `xbox` branch at the time of writing (CT-04 first pass).

Results on the real files are at the end.

## 1. Containers

| Kind | Files | Status | Notes |
|------|-------|--------|-------|
| ハカセ container (`.drs`, `.das`) | em/*.drs, St*/rNNN.das, etc/*.das, op/*.das | converted | 0x20 signature + 32-byte records `{u32 type, size, 0, offset, p0, p1, 0, 0}` terminated by `0xFFFFFFFF`, table zero to 0x400 (`tools/drs.py`). Record 0 (type 0) = body at 0x400, record 1 = type 4 sound bank or `0xFFFFFFFE` placeholder. All record words swapped. |
| body | inside the above; bare in St*/rNNN_NN.dat, ss/*/*.dat | converted | `u32 count, rel_offset, 0, 0; u32 ofs[count]; char tag[count][4]`; entries 0x20-aligned, no sizes (`read.cpp:989 GetDataExt`, `PL_ARC_PTR(arc,no) = arc->ofs[no] + arc`, index = entry + 4). Header words and `ofs[]` swapped; entries re-laid out (TPL entries change size) and re-padded to 0x20. |
| yz2 body | St*/rNNN.das, etc/memcard.das | converted (decompressed) | Text header `<packed hex>\t<unpacked hex>\n` + range-coded stream (`tools/motion/yz2.py`, `yz2code.cpp`). The Xbox never sees yz2: the body record size becomes the unpacked size. |
| REL in a body | em/*.drs (`rel_offset != 0`) | dropped | The module is linked statically. The body is cut at the REL, `rel_offset` is written as 0 and the manifest records the original REL size. |
| sound bank (record type 4; inner records 1 and 2) | em/*.drs, etc/core.das, St*/rNNN.das | passthrough | DSP-ADPCM bank (`include/snd_drv.h`), owned by the ADPCM task (`re4conv/adpcm.py`). Re-laid out after the body unchanged. |
| event (`.evd`) | Evd/rNNNsMM.evd | partly converted | `EvtHeader` (`include/event.h:71`): `char tag[0x20]` path, `room[8]@0x20`, `no[0xC]@0x28`, `s32 sndFlag@0x34`, `u32 pacOfs@0x40`, `pacSize@0x44`, `s32 nBin@0x48`, `u32 binOfs@0x4C`; `EvtBinEntry` 0x40 = `name[0x30], u32 ofs@0x30, u32 size@0x34`. Header words and the bin table are swapped and the bins converted by extension (.bin/.tpl/.fcv/.seq/.mdt ...). **The packet stream (`EvtPacket` `event.h:160`: `int id, u32 flag, s16 cut, frame, size` + per-id payload) is passthrough** — second pass. |
| ETM (nested) | room `ETM` entries | converted | `EtcArc` (`EtcModel.cpp:36`): `u32 num` at 0, files from 0x20, each `{u32 size (record stride) @0; char name[0x20] @0x20; data @0x40}`; files fetched by name (`GetEtcAddr`). Record sizes swapped, contents converted by extension. |

## 2. Entry formats (by tag)

| Tag | Where | Defined in | Status | Notes |
|-----|-------|-----------|--------|-------|
| BIN | em/, etc/, rooms, evd, ETM, ITM/SMD/SHD tables | `include/model.h` cModelData (see `model.py` docstring for every offset) | converted | Header, `ModelDataHead[]`, s16 positions, s8/s16 normals, u16/s16 texcoords, RGBA8 colours (untouched), cWeight (u8) / cWeightExt (u16 ids), `cPolyHeader` words, display-list counts and INDEX16 indices (VCD pos, nrm, [clr0], tex0 — `trans.cpp:1122`), shape table, blend / flip tables. Header words 0x04 (0) and 0x08 (small u32, 0x50 on the discs) swapped as u32; versions 0x20010801/0x20030817 have a 0x40-byte header (no blend/flip words, `pHead` = 0x40). Validator: version, alignment, every offset in range, stream ends exactly at `buff_size`, indices < counts (texcoord/colour counts are bounded by their section span). |
| TPL | everywhere | `include/tpl.h`, `gxtex.py` | converted | `TEXPalette {u32 version 0x0020AF30; u32 n; u32 descOfs}`, `TEXDescriptor {u32 texHdr; u32 clut}`, `TEXHeader` 0x24 bytes (`u16 h, w; u32 fmt, data, wrapS, wrapT, minF, magF; f32 lodBias; u8 edge, minLOD, maxLOD, unpacked`), `CLUTHeader {u16 n; u8 unpacked, pad; u32 fmt; u32 data}`. Headers swapped in place; each image replaced by a `ConvertedImage` (`gx_shim.h`: `RE4X`, w, h, NV2A code, mips, swizzled, size) — CMPR → DXT1 (0x0C, w/h multiples of 4), everything else → A8R8G8B8: power-of-two SZ (0x06) Morton-swizzled, other sizes LU_IMAGE (0x12) linear; all 11 GX formats decoded (I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 CI4 CI8 CI14X2 CMPR), CI formats expanded through their TLUT, CLUT data kept (LE u16) for the thermal TLUT, mips kept when maxLOD > minLOD. Blobs appended after the headers, `TEXHeader.data` points at the blob. Event placeholder `.tpl` bins (32 bytes of spaces) are passthrough. |
| FCV | em/, Evd/, rooms, ETM, EFF | `include/model.h` MotionData, `fcv.py` | converted | `fcv.serialise(fcv.parse(d, '>'), '<')`; oracle: LE parse == BE parse. Face `ShapeData` has the same layout. |
| SEQ | em/, ETM | `SEQUENCE_FILE` (`model.h:231`) | converted | `u16 count, u8 flag, u8 pad, {u16 frame, u8 se, u8 free}[]`. |
| EFF | em/, rooms, etc/, etc/<lang>/*.eff | `eff_sys.cpp:40 ESP_EFF_HEADER` | passthrough | `u32 version 0xB; u32 tplIdTbl@4, estIdTbl@8, sstIdTbl@C, pathIdTbl@10, objIdTbl@14, tpl@18, anm@1C, est@20, sst@24, path@28, obj@2C` (file offsets); id tables `{u32 num; {u16 id, u16, u32}[]}`, offset tables `{u32 num; u32 ofs[]}` relative to the table; holds TPLs (`EspCalcTplAddr` eff_sys.cpp:546), effect models (`EffEfmEnt`, motions via `EspEfmMotTbl`), EST/SST scripts (`include/est.h`, `espgen.h`). Second pass: nested TPL/BIN/FCV plus the script structs. |
| SMD | rooms (0 = room, 1 = common), St*/rNNN_NN.dat | `include/scroll.h:25 cSmd` | passthrough | `u8 version@0, u8 flag@1 (bit0 group table), u16 nModel@2, u32 binTblOfs@4, tplTblOfs@8, motTblOfs@C`; works at 0x10 (`cSmdWork`: Vec pos@0, ang@C, scale@18; u8 binId, tplId, motId, workNo @24..27; flags@44); tables are `u32 ofs[]` relative to the table, pointing at nested BIN/TPL (`scroll.cpp:496`, `SmdInit scroll.cpp:52`). |
| SMX | rooms | `scroll.h:65 cSmxData` | passthrough | `u8 version, u8 nData, pad to 0x10`; 0x90-byte `cSmxWork` entries: `u8 modelNo, id, otType, cullMode; u32 litSelectMask@4, flag@8, materialColor@C`. |
| LIT | rooms (0, 1), core slot 0xB | `include/light.h:291 cLit` | passthrough | `u16 cutNum@0, u8 version@2, u8 nMaxLight@3, u32 ofs[cutNum]@4` → `cLightEnv` per cut (`GXColor ambient@0, u32 nLight@4, FOG@8, mirrorFog@18, s32 focusZ@28`, lights after 0x104). `cLit::versionUp` patches old versions in place. |
| SHD | rooms | `include/shadow.h:41 ShdHeader` | passthrough | `u8 version (≤0x41), u8, u16 num@2, u32 tblOfs@4` (model offset table) ; 0x48-byte `ShdEntry` from 0x10 (pos, rot, scale Vecs; u8 model@24, shdCol@26). Nested BINs. |
| EAR | rooms | `include/espgen.h:65 SstArea` | passthrough | `u32 num@0, u32 ver@4`; 0x98-byte `SstAreaEnt` from 0x10 (`u8 no, beFlag, areaNo; AREA_HIT_DATA@4; u32 flag@34`). |
| SAR | rooms | `light_area.cpp:24 LightAreaHed` | passthrough | `u32 num@0`, pad to 0x10; 0xD8-byte entries (`u8 lightNoPl@2, lightNoEm@3; AREA_HIT_DATA@4; u32 flag@34; s8 power@38`). |
| SAT / EAT | rooms, core | `include/atari.h:20 cSatFile`, `:40 cSatHeader` | passthrough | `u8 version@0 (0xFF single; bit7 set & != 0xFF: table of u32 ofs[] from 4)`, `u16 nVertex@2, nNormal@4, nEdge@6, nPolygon@A, nFloor@C, nSlope@E, nWall@10, nBlock@12`; arrays back to back from 0x14 (`atari.cpp:1082 cSat::init`). |
| TEX | rooms, core | `include/texture.h:32 TexData` | passthrough | `u32 version 3@0, ofsId@4, ofsTpl@8, ofsAnm@C`; TPLs relocated by `cTexSys::CalcTplAddr` (texture.cpp:154). Nested TPLs. |
| ITM | rooms | `item_model.cpp:11 ItemModelPack` | passthrough | `u32@0, ofsId@4 ({u32 num; {u16 id, u16, u32}[]}), ofsBin@8, ofsTpl@C ({u32 num; u32 ofs[]} relative)`. Nested BIN/TPL. |
| ETM | rooms | `EtcModel.cpp:36 EtcArc` | converted | see containers. |
| ETS | rooms | `include/etc_model.h:122 ETS_DATA` | passthrough | `u16 num@0`, pad to 0x10; 0x28-byte records (`u16 id@0, u16 no@2 (u8 type@3), Vec ang@10, Vec pos@1C`). |
| CAM | rooms, core slot 0xC | `include/cam_ctrl.h:21 CAM_FILE_HEADER` | passthrough | `char[4] "B400".."B404"@0, u8 nCdat@4, nAdat@5, nLdat@6`, pad to 0x10; `CUT_INFO[nAdat]` (0x10: pAdat@8, pCdat@C), `AREA_DATA` 0x30, `CAMERA_DATA` 0x34, `LERP_DATA` 0x10; `CameraControl::calcAddr` (cam_ctrl.cpp:282) relocates pAdat, pCdat, pVer, pCampos, pTarget, pRoll, pFovy, pFrame in place. core.das's CAM is the 0x20-byte "EMPT" placeholder. |
| BLK | rooms | `include/block.h:41 BlockHeader` | passthrough | `"BLK"@0, u16 version 0x100@4, u8 nBlock@7, u16 nArea@8, nConnect@A, u32 ofsLink@C, ofsArea@10, ofsConnect@14`. |
| EVS | rooms | `event.cpp:100 EvsHeader` | passthrough | `s32 num@0, u32 tblOfs@4`; entries `{u32 ofs; u32}` → embedded event files (same as `.evd`). |
| FSE | rooms | `include/flr_at.h:55 FLR_AT_HEADER` | passthrough | `"FSE"@0, u16 version 0x103@4, u16 num@6, u8 cartridgeType@8`; records from 0x10 (`u8 flag, type, no, group, priority; AREA_HIT_DATA@14; payload@44`). |
| AEV / ITA | rooms | `sce_at.cpp:184 SceAtFileHead`, `include/sce_at.h:218` | passthrough | `"AEV"/"ITA"@0, u16 version (0x104 / 0x105)@4, u16 num@6`; records from 0x10 (`u32 tag@0 (OT link, written at runtime), AREA_HIT_DATA@4, u8 beFlg@34, id@35, no@36, priority@44`). |
| ESE | rooms | `include/snd.h:179 SeAtHead` | passthrough | `"ESE"@0, u16 version 0x100@4, u16 num@6`; 0x2C-byte records from 0x10 (`u8 beFlg, atNo; u16 flag@2; Vec pos@4; u16 blk@12; u16 seNo@16`). |
| DSE | rooms | `snd.cpp:1658 SndDoorSe` | passthrough | `u32 num; {u16 room; u16 door[5]}[]`. |
| STB | rooms | `include/snd.h:31 SND_INFO` | passthrough | two `REVERB_INFO` (u16 aux x4, f32 x6) at 0 and 0x20, then `u32 offset_tbl[32]@40, offset_vol[32]@C0, offset_pitch[32]@140, offset_filter[32]@1C0`; curves scaled in place at load (snd.cpp:1588). |
| RTP | rooms | `include/route_ck.h:26 RTP` | passthrough | `"2RTP"@0, u16 mapped@4, nPoint@6, nLine@8, nNext@A, u32 pPoint@C, pLine@10, pNext@14` (offsets, not relocated on GC). |
| MDT | rooms, core, evd | `include/mes.h:12 MessageData` | passthrough | `u32@0, u32 ofs[language]@4`; language block `u32@0, u32 count@4, u32 ofs[count]@8` → u16 text. |
| CNS | rooms | `cons.cpp:9 ConsRoom` | passthrough | `u32 num; u32 bits[(num>>5)+1]; u32 values[num]`. |
| EMI | rooms | `include/embarrel.h:21 EmiData` | passthrough | `int n@0`, pad; 0x40-byte entries from 8 (`u8 type, sub, state; Vec pos@4; f32 rotY@10`). |
| OSD | rooms | — | passthrough | stored in `pG->pOsd` (read.cpp:186); no reader found in game code. |
| DRA | rooms | `t_dr.cpp:31 DrHeader` | passthrough | debug tool only (`"DRA", u16 version 0x100, u16 nArea, nFile, u32 strOfs@C`). |
| UWF | rooms (fixed slot 0x28), core, etc/<lang>/*.uwf | `include/id_sys.h:129 ID_FILE_HEADER` | passthrough | `char[4] "1.00"/"2.00"@0, u8 groupNo@4, unitNum@5`, units from 8 (`IDSystem::set`, id_sys.cpp:201). |
| VIB | core slot 7 | `include/pad.h:29 VibDataTbl` | passthrough | `{u32 num; u32 ofs[]}` → `VIB_INFO {u32 num; VIB_DATA[]}`, `VIB_DATA {u16 flag, delay, time; u8 sLevel, eLevel}`. |
| TEX (core) | core | as above | passthrough | |
| "BIN" that is not a model | core.das entry 11 | — | passthrough | `u8 0x0B` count + `u32` offsets (a string/blob table); the model validator rejects it (version word invalid, `NotThisFormat`) and it is copied. |

## 3. Standalone files

| File | Defined in | Status | Notes |
|------|-----------|--------|-------|
| etc/*.esl | `include/global.h:67 EM_LIST` (255/256 x 0x20, raw) | converted (`simple.py`) | `u8 beFlag, id, type, set; u32 flag@4; u16 hp@8; u8 character@B; s16 pos[3]@C; s16 rot[3]@12; u16 room@18; s16 guardR@1A; u8 x1C[4]`. |
| etc/SizeTbl.dat | `dvd.cpp:1159 SizeTableRead` → `pSizeTbl` | converted (`simple.py`) | `u32 count; {char name[8]; u32 size; u32 x}[count]`; read raw, never consumed by game code (`FileExistCheck` uses the DVD file table). |
| bgm/bgmtbl.dat | `include/snd.h:89 SndBgmTbl` | converted (`simple.py`) | `u32 room_ofs@0, u32 list_ofs@4`; room offsets array, `u16` room-id list ending `0xFFFF`, `SndBgmRoom {u32 num; SndBgmEnt {u32 id; u32 bgm[6]; u32 str[6]}[]}`. Validated by walking list + offsets (`SndBgmTblInit`); u32 region before `list_ofs`, u16 list after. |
| bgm/*.sbb, *.hed, doorse.*, bio4midi.* | `include/snd_drv.h` | passthrough | ADPCM task. |
| font/*.fnt | `mes.cpp:338 setupFont` | passthrough (second pass) | `u32 tplOfs@0, u32 widthTblOfs@4` (relative to the file) + TPL + width table. |
| etc/<lang>/*.eff | `id_tex.cpp:39 IdTexData` | passthrough | same header as EFF (version 0xB, ofsId@4, ofsTpl@18, ofsAnm@1C); TPLs via `cTexSys::CalcTplAddr`. |
| ss/<lang>/*.dat, ss/cmn/*.dat | `include/sscrn.h:19 SS_ARC_PTR` | passthrough | body-like `u32 ofs[]` table (no count/tags per the macro); entries: .fnt, ID texture .eff, ID layouts (`ID_FILE_HEADER`), message data. |
| Movie/*.sfd | CRI | passthrough | CRI parses its own stream. |
| Rel/*.rel | — | dropped | static link. |
| opening.bnr, dummy.dat, znull.dat | — | passthrough | |

## 4. Results on the real files

(filled in by `re4conv convert` runs; see xbox/cloud-tasks/progress/CT-04.md)
