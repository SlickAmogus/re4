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
| sound bank (record type 4; inner records 1 and 2) | em/*.drs, etc/core.das, St*/rNNN.das | converted (CT-14) | Nested DvdHeader table; MRAM block tables little-endian, DSP-ADPCM samples unchanged (`sound.py`, section 3a). Re-laid out after the body. |
| event (`.evd`) | Evd/rNNNsMM.evd | converted (CT-14) | `EvtHeader` (`include/event.h:71`): `char tag[0x20]` path, `room[8]@0x20`, `no[0xC]@0x28`, `s32 sndFlag@0x34`, `u32 pacOfs@0x40`, `pacSize@0x44`, `s32 nBin@0x48`, `u32 binOfs@0x4C`; `EvtBinEntry` 0x40 = `name[0x30], u32 ofs@0x30, u32 size@0x34`. Header words and the bin table are swapped and the bins converted by extension (.bin/.tpl/.fcv/.seq/.mdt ...). **The packet stream (`EvtPacket` `event.h:160`: `int id, u32 flag, s16 cut, frame, size` + per-id payload) is passthrough** — second pass.. CT-14: the packet stream is converted too (`events.py`, section 2a). |
| ETM (nested) | room `ETM` entries | converted | `EtcArc` (`EtcModel.cpp:36`): `u32 num` at 0, files from 0x20, each `{u32 size (record stride) @0; char name[0x20] @0x20; data @0x40}`; files fetched by name (`GetEtcAddr`). Record sizes swapped, contents converted by extension. |

## 2. Entry formats (by tag)

| Tag | Where | Defined in | Status | Notes |
|-----|-------|-----------|--------|-------|
| BIN | em/, etc/, rooms, evd, ETM, ITM/SMD/SHD tables | `include/model.h` cModelData (see `model.py` docstring for every offset) | converted | Header, `ModelDataHead[]`, s16 positions, s8/s16 normals, u16/s16 texcoords, RGBA8 colours (untouched), cWeight (u8) / cWeightExt (u16 ids), `cPolyHeader` words, display-list counts and INDEX16 indices (VCD pos, nrm, [clr0], tex0 — `trans.cpp:1122`), shape table, blend / flip tables. Header words 0x04 (0) and 0x08 (small u32, 0x50 on the discs) swapped as u32; versions 0x20010801/0x20030817 have a 0x40-byte header (no blend/flip words, `pHead` = 0x40). Validator: version, alignment, every offset in range, stream ends exactly at `buff_size`, indices < counts (texcoord/colour counts are bounded by their section span). |
| TPL | everywhere | `include/tpl.h`, `gxtex.py` | converted | `TEXPalette {u32 version 0x0020AF30; u32 n; u32 descOfs}`, `TEXDescriptor {u32 texHdr; u32 clut}`, `TEXHeader` 0x24 bytes (`u16 h, w; u32 fmt, data, wrapS, wrapT, minF, magF; f32 lodBias; u8 edge, minLOD, maxLOD, unpacked`), `CLUTHeader {u16 n; u8 unpacked, pad; u32 fmt; u32 data}`. Headers swapped in place; each image replaced by a `ConvertedImage` (`gx_shim.h`: `RE4X`, w, h, NV2A code, mips, swizzled, size) — CMPR → DXT1 (0x0C, w/h multiples of 4), everything else → A8R8G8B8: power-of-two SZ (0x06) Morton-swizzled, other sizes LU_IMAGE (0x12) linear; all 11 GX formats decoded (I4 I8 IA4 IA8 RGB565 RGB5A3 RGBA8 CI4 CI8 CI14X2 CMPR), CI formats expanded through their TLUT, CLUT data kept (LE u16) for the thermal TLUT, mips kept when maxLOD > minLOD. Blobs appended after the headers, `TEXHeader.data` points at the blob. Event placeholder `.tpl` bins (32 bytes of spaces) are passthrough. |
| FCV | em/, Evd/, rooms, ETM, EFF | `include/model.h` MotionData, `fcv.py` | converted | `fcv.serialise(fcv.parse(d, '>'), '<')`; oracle: LE parse == BE parse. Face `ShapeData` has the same layout. |
| SEQ | em/, ETM | `SEQUENCE_FILE` (`model.h:231`) | converted | `u16 count, u8 flag, u8 pad, {u16 frame, u8 se, u8 free}[]`. |
| EFF | em/, rooms, etc/, etc/<lang>/*.eff | `eff_sys.cpp:40 ESP_EFF_HEADER` | converted (CT-14, `room_nested.py`) | `u32 version 0xB; u32 tplIdTbl@4, estIdTbl@8, sstIdTbl@C, pathIdTbl@10, objIdTbl@14, tpl@18, anm@1C, est@20, sst@24, path@28, obj@2C` (file offsets); id tables `{u32 num; {u16 id, u16, u32}[]}`, offset tables `{u32 num; u32 ofs[]}` relative to the table; holds TPLs (`EspCalcTplAddr` eff_sys.cpp:546), effect models (`EffEfmEnt`, motions via `EspEfmMotTbl`), EST/SST scripts (`include/est.h`, `espgen.h`). Second pass: nested TPL/BIN/FCV plus the script structs. see 2a. |
| SMD | rooms (0 = room, 1 = common), St*/rNNN_NN.dat | `include/scroll.h:25 cSmd` | converted (CT-14, `room_nested.py`) | `u8 version@0, u8 flag@1 (bit0 group table), u16 nModel@2, u32 binTblOfs@4, tplTblOfs@8, motTblOfs@C`; works at 0x10 (`cSmdWork`: Vec pos@0, ang@C, scale@18; u8 binId, tplId, motId, workNo @24..27; flags@44); tables are `u32 ofs[]` relative to the table, pointing at nested BIN/TPL (`scroll.cpp:496`, `SmdInit scroll.cpp:52`). see 2a. |
| SMX | rooms | `scroll.h:65 cSmxData` | converted (CT-14, `room.py`) | `u8 version, u8 nData, pad to 0x10`; 0x90-byte `cSmxWork` entries: `u8 modelNo, id, otType, cullMode; u32 litSelectMask@4, flag@8, materialColor@C`. see 2a. |
| LIT | rooms (0, 1), core slot 0xB | `include/light.h:291 cLit` | converted (CT-14, `room.py`) | `u16 cutNum@0, u8 version@2, u8 nMaxLight@3, u32 ofs[cutNum]@4` → `cLightEnv` per cut (`GXColor ambient@0, u32 nLight@4, FOG@8, mirrorFog@18, s32 focusZ@28`, lights after 0x104). `cLit::versionUp` patches old versions in place. see 2a; also the event `.lit` bins. |
| SHD | rooms | `include/shadow.h:41 ShdHeader` | converted (CT-14, `room_nested.py`) | `u8 version (≤0x41), u8, u16 num@2, u32 tblOfs@4` (model offset table) ; 0x48-byte `ShdEntry` from 0x10 (pos, rot, scale Vecs; u8 model@24, shdCol@26). Nested BINs. see 2a. |
| EAR | rooms | `include/espgen.h:65 SstArea` | converted (CT-14, `room.py`) | `u32 num@0, u32 ver@4`; 0x98-byte `SstAreaEnt` from 0x10 (`u8 no, beFlag, areaNo; AREA_HIT_DATA@4; u32 flag@34`). see 2a. |
| SAR | rooms | `light_area.cpp:24 LightAreaHed` | converted (CT-14, `room.py`) | `u32 num@0`, pad to 0x10; 0xD8-byte entries (`u8 lightNoPl@2, lightNoEm@3; AREA_HIT_DATA@4; u32 flag@34; s8 power@38`). see 2a. |
| SAT / EAT | rooms, core | `include/atari.h:20 cSatFile`, `:40 cSatHeader` | converted (CT-14, `room.py`) | `u8 version@0 (0xFF single; bit7 set & != 0xFF: table of u32 ofs[] from 4)`, `u16 nVertex@2, nNormal@4, nEdge@6, nPolygon@A, nFloor@C, nSlope@E, nWall@10, nBlock@12`; arrays back to back from 0x14 (`atari.cpp:1082 cSat::init`). see 2a; core.das SAT #9/#10 (version 0x10, other layout, no reader found) stay passthrough. |
| TEX | rooms, core | `include/texture.h:32 TexData` | converted (CT-14, `room_nested.py`) | `u32 version 3@0, ofsId@4, ofsTpl@8, ofsAnm@C`; TPLs relocated by `cTexSys::CalcTplAddr` (texture.cpp:154). Nested TPLs. see 2a. |
| ITM | rooms | `item_model.cpp:11 ItemModelPack` | converted (CT-14, `room_nested.py`) | `u32@0, ofsId@4 ({u32 num; {u16 id, u16, u32}[]}), ofsBin@8, ofsTpl@C ({u32 num; u32 ofs[]} relative)`. Nested BIN/TPL. see 2a. |
| ETM | rooms | `EtcModel.cpp:36 EtcArc` | converted | see containers. |
| ETS | rooms | `include/etc_model.h:122 ETS_DATA` | converted (CT-14, `room.py`) | `u16 num@0`, pad to 0x10; 0x28-byte records (`u16 id@0, u16 no@2 (u8 type@3), Vec ang@10, Vec pos@1C`). see 2a. |
| CAM | rooms, core slot 0xC | `include/cam_ctrl.h:21 CAM_FILE_HEADER` | converted (CT-14, `room.py`) | `char[4] "B400".."B404"@0, u8 nCdat@4, nAdat@5, nLdat@6`, pad to 0x10; `CUT_INFO[nAdat]` (0x10: pAdat@8, pCdat@C), `AREA_DATA` 0x30, `CAMERA_DATA` 0x34, `LERP_DATA` 0x10; `CameraControl::calcAddr` (cam_ctrl.cpp:282) relocates pAdat, pCdat, pVer, pCampos, pTarget, pRoll, pFovy, pFrame in place. core.das's CAM is the 0x20-byte "EMPT" placeholder. see 2a; core.das CAM is an "EMPT" placeholder (copied). |
| BLK | rooms | `include/block.h:41 BlockHeader` | converted (CT-14, `room.py`) | `"BLK"@0, u16 version 0x100@4, u8 nBlock@7, u16 nArea@8, nConnect@A, u32 ofsLink@C, ofsArea@10, ofsConnect@14`. see 2a. |
| EVS | rooms | `event.cpp:100 EvsHeader` | converted (CT-14, `room_nested.convert_evs`) | `s32 num@0, u32 tblOfs@4`; entries `{u32 ofs; u32}` → embedded event files (same as `.evd`). events re-laid out through `container.convert_event`. |
| FSE | rooms | `include/flr_at.h:55 FLR_AT_HEADER` | converted (CT-14, `room.py`) | `"FSE"@0, u16 version 0x103@4, u16 num@6, u8 cartridgeType@8`; records from 0x10 (`u8 flag, type, no, group, priority; AREA_HIT_DATA@14; payload@44`). see 2a. |
| AEV / ITA | rooms | `sce_at.cpp:184 SceAtFileHead`, `include/sce_at.h:218` | converted (CT-14, `room.py`) | `"AEV"/"ITA"@0, u16 version (0x104 / 0x105)@4, u16 num@6`; records from 0x10 (`u32 tag@0 (OT link, written at runtime), AREA_HIT_DATA@4, u8 beFlg@34, id@35, no@36, priority@44`). see 2a. |
| ESE | rooms | `include/snd.h:179 SeAtHead` | converted (CT-14, `room.py`) | `"ESE"@0, u16 version 0x100@4, u16 num@6`; 0x2C-byte records from 0x10 (`u8 beFlg, atNo; u16 flag@2; Vec pos@4; u16 blk@12; u16 seNo@16`). see 2a. |
| DSE | rooms | `snd.cpp:1658 SndDoorSe` | converted (CT-14, `room.py`) | `u32 num; {u16 room; u16 door[5]}[]`. see 2a. |
| STB | rooms | `include/snd.h:31 SND_INFO` | converted (CT-14, `room.py`) | two `REVERB_INFO` (u16 aux x4, f32 x6) at 0 and 0x20, then `u32 offset_tbl[32]@40, offset_vol[32]@C0, offset_pitch[32]@140, offset_filter[32]@1C0`; curves scaled in place at load (snd.cpp:1588). see 2a. |
| RTP | rooms | `include/route_ck.h:26 RTP` | converted (CT-14, `room.py`) | `"2RTP"@0, u16 mapped@4, nPoint@6, nLine@8, nNext@A, u32 pPoint@C, pLine@10, pNext@14` (offsets, not relocated on GC). see 2a. |
| MDT | rooms, core, evd | `include/mes.h:12 MessageData` | converted (CT-14, `room.py`) | `u32@0, u32 ofs[language]@4`; language block `u32@0, u32 count@4, u32 ofs[count]@8` → u16 text. see 2a. |
| CNS | rooms | `cons.cpp:9 ConsRoom` | converted (CT-14, `room.py`) | `u32 num; u32 bits[(num>>5)+1]; u32 values[num]`. see 2a. |
| EMI | rooms | `include/embarrel.h:21 EmiData` | converted (CT-14, `room.py`) | `int n@0`, pad; 0x40-byte entries from 8 (`u8 type, sub, state; Vec pos@4; f32 rotY@10`). see 2a. |
| OSD | rooms | — | passthrough | stored in `pG->pOsd` (read.cpp:186); no reader found in game code; not needed. |
| DRA | rooms | `t_dr.cpp:31 DrHeader` | passthrough | debug tool only (`"DRA", u16 version 0x100, u16 nArea, nFile, u32 strOfs@C`). debug tool data only (`t_dr.cpp`), not compiled into the port. |
| UWF | rooms (fixed slot 0x28), core, etc/<lang>/*.uwf | `include/id_sys.h:129 ID_FILE_HEADER` | converted (CT-14, `room.py`) | `char[4] "1.00"/"2.00"@0, u8 groupNo@4, unitNum@5`, units from 8 (`IDSystem::set`, id_sys.cpp:201). see 2a. |
| VIB | core slot 7 | `include/pad.h:29 VibDataTbl` | converted (CT-14, `room.py`) | `{u32 num; u32 ofs[]}` → `VIB_INFO {u32 num; VIB_DATA[]}`, `VIB_DATA {u16 flag, delay, time; u8 sLevel, eLevel}`. see 2a. |
| TEX (core) | core | as above | converted (CT-14) |  as above. |
| "BIN" that is not a model | core.das entry 11 | — | passthrough | `u8 0x0B` count + `u32` offsets (a string/blob table); the model validator rejects it (version word invalid, `NotThisFormat`) and it is copied. |

## 2a. Room, effect and event tables (CT-14)

Modules: `spans.py` (layout engine: spans with a struct format per element, gaps must be zero, floats moved
as bit patterns), `room.py` (fixed-layout tables, converted in place, sizes unchanged), `room_nested.py`
(entries holding nested BIN/TPL/FCV: head converted, offset tables re-laid out because TPLs change size,
nested files 0x20-aligned, table offsets rewritten), `events.py` (packet streams), `fonts.py`. Every
`check()` re-parses the output little-endian and compares every field value with the big-endian input.
Unknown non-zero bytes outside every known span are refused (ConvertError -> entry `failed`); pads named
in the headers are copied. Entries holding only zero / 0xCD fill are copied (empty FSE of r120 / r217).

| Tag | Layout converted (Python struct codes; `s` = copied bytes) |
|-----|------------------------------------------------------------|
| AREA_HIT_DATA | `BBH11f` (area.h:47; every union body is f32). Shared by BLK, SAR, EAR, FSE, AEV, ITA. |
| BLK | header `4sHBBHH3I`; BlockLink `B3s8b` x nBlock; BlockArea `I`+AREA+`BBbB` x nArea; BlockConnect `BB2s16b` x nConnect. Empty BLKs are a 0x14-byte header + 0xCD. |
| LIT | `HBB` + `u32 ofs[CutNum]`; cLightEnv 0x104 (`room.LIT_ENV`: colours as bytes, Fog/MirrorFog `I2f4s`, FocusZ `i`, far_play_ratio / lod_bias `f`, pads copied); cLightWork 0x12C: head `4s3ff4sf4sIHHI`, spot union `16I`, work/sub 0x80 by Id (LightFuncTbl game.cpp:173: 2 `4f`, 3 `3f`, 4 LIT04 `HBBhh4s3fB`, 5 `2I` (cLightPath, runtime), 6 `3f`, 7 `6f`, others bytes), path 0x40 copied. |
| SAR | `I` + pad; `4s`+AREA+`IbB158s` x num. |
| EAR | `II` + pad; `4s`+AREA+`I96s` x num. |
| CAM | "B402".."B404": header `4s3B9s`; CUT_INFO `B7sII` x nAdat; AREA_DATA `BbbBfBB22sffiI` x nAdat (pVer -> `3f` x nVer); CAMERA_DATA `BbbB3fIf8siIIII` x nCdat (pFrame -> `H` x nPoint, pCampos/pTarget -> `3f`, pRoll/pFovy -> `f`); LERP_DATA `B4bB2si4s` x nLdat. Offsets stay offsets (calcAddr relocates). pFrame of Id-8 rail cameras is junk (out of range) and is not followed. core.das CAM = "EMPT" placeholder (copied). |
| FSE | header `4sHHBBHI` (v0x103); records 0x84 = `5B15s`+AREA + payload by type (2 FLR_AT_BGM_VOL `BBbb2iHHI44s`, others bytes). |
| AEV / ITA | header `4sHH`+pad (v0x104 / 0x105); SCE_AT_UNIT `I`+AREA+`8BII4B2b2BIh2B8s` + 0x40 payload by id (sce_at.h): 0 `16I`, 1 DOOR `3ffBBBBIbBBBI32s`, 3 ITEM `3fI3f4H6Bh4f2B2s` (every ITA record), 4 `BBH60s`, 5 `hhBBHB3s52s`, 8 `I60s`, 9 `HBB60s`, 10 `iBB2sif48s`, 11 `IIB3siiIi36s`, 12 `3ffBB2sff36s`, 13 `iI10I16s`, 15 `IIBB54s`, 16 `3ffbBBBBB42s`, 17 `HH60s`, 18 `BBBB8f3fIB11s`, 19 `3ffB47s`; 2 EXEC and unknown ids copied (EXEC's payload is not read, sceAtFunc_exec sce_at.cpp:852). |
| ETS | `H` + pad; `HH12s6f` x num. |
| ESE | `4sHH`+pad; `BBH3f7HhHH8s` x num. |
| DSE | `I` + `6H` x num. |
| CNS | `u32` x (1 + (num>>5)+1 + num). |
| EMI | `i` + pad; `4s3ff44s` x n. |
| STB | REVERB_INFO `4H6f` x2; `128I` offsets; SND_COMBINE_TBL 8 bytes copied; SND_TBL_INFO `If` + `fHH` x num (vol / pitch / filter may share one table). |
| RTP | `4s4H3I`; RTP_POINT `3fHH`; RTP_LINE `hH`; next table bytes. |
| SMX | `BB2s3I`; cSmxWork 0x90 `BBBBII4s` + Free + `4sff`: MaterialColor / SpecularColor are RGBA bytes (copied); Free (-> cObjScr::free) `29I`, type 1 `3fB3s25I` (ObjScrRotWork flag byte). |
| SAT / EAT | table form (version bit7, != 0xFF): `u8, u8 count, 2 bytes copied, u32 ofs[count]`; cSatFile `BB9H` (v0x20), Vec arrays `3f` x (nVertex + nNormal + nEdge), SAT_POLY `8HI`, cSatBlock tree `6f4HI` + child chain (flag bit0) or `H` x (nFloor+nSlope+nWall), siblings via m_pList relative offsets; the walked block count must equal nBlock. |
| MDT | `I` nLang + `I` x nLang; MesTblBlock `II` + `I` x count; message codes `H` to the block end (an empty MDT has every block at 0x1C). Also the event `.mdt` bins and ss bodies. |
| FCS | DatFocus (event.cpp:82) 0x810 = `516I` (two HERMITE_1_FIX + 2 f32); the event `.fcs` bins (0x820 with padding). |
| UWF | `4sBB2s` ("1.00"/"2.00"); ID_DATA_V1 `20s3f12fff4s3f5B3s6I` / V2 `20s3f12fff4s4s3f5B3s6I`; ofs[6] -> FUNC_PATH_PTR `b6sb`+`3f` x nPoint, FuncPathWork `ii`+`3f` x n, HERMITE_1_PTR `i`+`4f` x nPoint. |
| VIB | `I` + `I` x num; VIB_INFO `I` + `3H2B` x num. |
| SMD | head `BBHIII` (v0x40) + group table (`I` x (1+nGroup), Flag bit0) + cSmdWork `9f4BfI20sI` x nModel (also in group mode on the retail discs); slot tables (count = first offset / 4) of BIN / TPL / FCV; a table offset equal to the SMD size means none and is rewritten to the new size. The empty TPL `{0x0020AF30, 0, 0xC}` is converted by swapping its three words. |
| SHD | `BBHI` + pad; ShdEntry `9fBBB33s` x num; slot table of BINs. |
| TEX | `4I` (v3); TexIdTbl `I` + `HHI` x num; counted tables of TPL / cAnm (`HHhhHBBB3s` + bytes); offset 0 = no table. |
| ITM | `4I`; id table; counted tables of BIN / TPL. |
| EFF | `12I` (v0xB); id tables `I` + `HHI` x num; counted tables (0 = none) of TPL, cAnm, cEspSeqHead (EST and SST), Path, EffEfmEnt. cEspSeqHead `4HHBB3f3fBBHII` + cEspSeqTbl 0x12C x num (`room_nested.ESQ_TBL`, every field typed from esp.h:32); bytes after the num records (stale tool data, r11c / core) copied. Path `H2s` + PATH_UNIT `3f3ff3BB3B5s`. EffEfmEnt `5I` -> BIN, TPL, EspEfmMotTbl (counted FCV table), shape header (FCV). Standalone etc/<lang>/*.eff (IdTexData, same header) too. |
| EVS | `iI`; EvsEntry `II` -> event blocks (container.convert_event), re-laid out. |
| event packets | EvtPacket head `iIhhhh`; body by id: 8/15/16/17/18/29 `iii`, 9 `24s7i`, 13 `12si4s`, names and unused ids copied (`events.py`). All 149 .evd streams parse. |
| FNT | see section 3. |
| sound | see section 3a. |
| still passthrough | MAP (ss/Cmn/map_obj*.dat, 142; read by the sub-screen map, src/Sscrn/ss_map.cpp, not analysed yet), PTH, DMY, OSD (unread), DRA (debug tool), core.das SAT #9/#10 (v0x10) and BIN #11 (not a model), bio4*.sbb (raw DSP-ADPCM by design), Movie/*.sfd. |

### Port notes (game code that reads converted data in a byte-order-dependent way)

Not converter bugs: the data is converted field by field as the struct declares it, but the code below
reads it through a different view. Each needs an `// XBOX:` fix or an `#ifdef RE4_PORT` layout:

- `scroll.cpp:317-330` smxInit tests `(col & ~0xFF) == 0` on a u32 read of RGBA bytes (BE: "RGB = 0"); on LE
  the mask must be `0x00FFFFFF`.
- `EspGenPrm` (esp.h:10, cEspSeqTbl @0xCC): the two words are converted as u32; `prm.h.xCE/xD2` and
  `prm.b.xCF/xD3` read their low halves / bytes (Espgen42:1197, esp04:254, esp0c:87, esp4c:94, esp_efm:381,
  espgen45:1053) -> reverse EspGenPrmH/B member order per word on LE.
- `SstList` / EffIdTbl entries (espgen.h:30): `u16 no` is converted as u16; the `{u8 x0, id}` view
  (GetSstDispFlag) reads its low byte at +1 on BE -> swap the two bytes in the LE struct.
- Already-relocated tests compare against GC addresses: `CameraControl::calcAddr` `(s32)ptr < 0`,
  `cSat::blockInit` VALID_PTR on file offsets, `EventMgr::SetEvs` `(int)hdr >= 0`, `smxInit` / `cSmd::slide`
  0x80000000..0x82FFFFFF ranges.
- `snd_iss2.cpp:149`, `snd_str2.cpp:164` read `AXPBADDR.currentAddressHi:Lo` as one u32 (AX shim layout).

## 3. Standalone files

| File | Defined in | Status | Notes |
|------|-----------|--------|-------|
| etc/*.esl | `include/global.h:67 EM_LIST` (255/256 x 0x20, raw) | converted (`simple.py`) | `u8 beFlag, id, type, set; u32 flag@4; u16 hp@8; u8 character@B; s16 pos[3]@C; s16 rot[3]@12; u16 room@18; s16 guardR@1A; u8 x1C[4]`. |
| etc/SizeTbl.dat | `dvd.cpp:1159 SizeTableRead` → `pSizeTbl` | converted (`simple.py`) | `u32 count; {char name[8]; u32 size; u32 x}[count]`; read raw, never consumed by game code (`FileExistCheck` uses the DVD file table). |
| bgm/bgmtbl.dat | `include/snd.h:89 SndBgmTbl` | converted (`simple.py`) | `u32 room_ofs@0, u32 list_ofs@4`; room offsets array, `u16` room-id list ending `0xFFFF`, `SndBgmRoom {u32 num; SndBgmEnt {u32 id; u32 bgm[6]; u32 str[6]}[]}`. Validated by walking list + offsets (`SndBgmTblInit`); u32 region before `list_ofs`, u16 list after. |
| bgm/bio4str.hed, bio4midi.hed/.dat, doorse.hed/.dat | `include/snd_drv.h`, `snd.h` | converted (CT-14, `sound.py`) | see section 3a; sizes unchanged so every DVD offset stays valid. |
| bgm/bio4bgm.sbb, bio4evt.sbb | — | passthrough (by design) | raw DSP-ADPCM stream data, no header; endian-neutral. |
| font/*.fnt, FNT entries | `mes.cpp:338 setupFont` | converted (CT-14, `fonts.py`) | `u32 tplOfs@0 (0x20), u32 widthOfs@4` (relative to the file) + TPL (tpl.py, size changes) + s8 width pairs (copied); widthOfs rewritten. |
| etc/<lang>/*.eff | `id_tex.cpp:39 IdTexData` | converted (CT-14) | same header as EFF (version 0xB, ofsId@4, ofsTpl@18, ofsAnm@1C); TPLs via `cTexSys::CalcTplAddr`. |
| ss/<lang>/*.dat, ss/cmn/*.dat | `include/sscrn.h:19 SS_ARC_PTR` | passthrough | body-like `u32 ofs[]` table (no count/tags per the macro); entries: .fnt, ID texture .eff, ID layouts (`ID_FILE_HEADER`), message data. |
| Movie/*.sfd | CRI | passthrough | CRI parses its own stream. |
| Rel/*.rel | — | dropped | static link. |
| opening.bnr, dummy.dat, znull.dat | — | passthrough | |

## 3a. Sound (CT-14, `sound.py`)

Decision (for now): sample data stays **GameCube DSP-ADPCM** — 8-byte frames (1 header byte + 14
nibbles) are byte-oriented and endian-neutral; the port's first audio backend is a software mixer that
decodes DSP-ADPCM (`xbox/port/snd/dspadpcm.c`). Only headers and tables are swapped; layout and sizes
are unchanged, so `.hed` DVD offsets, `DvdHeader.ofs` and ARAM sizes stay valid. Converting samples to
Xbox ADPCM later (`adpcm.py`, CT-06) is a separate pass that will need the address map of 03-AUDIO.

Every converter builds a span layout from the big-endian input (struct format per element); `check`
rebuilds it from the output, requires the same layout and compares every field value. Non-zero bytes
outside every known span are refused (none on the retail discs).

| Structure | Where | Fields swapped (everything else is copied) |
|-----------|-------|---------------------------------------------|
| bank = DvdHeader table (`dvd.h:28`, `dvd.cpp:656`) | nested type-4 record, each bank of doorse.dat / bio4midi.dat | `char sig[0x20]` kept; records `u32 type, size, dest, ofs, sndType, sndArg, sndNo, x1C` (all 8 words) incl. the `0xFFFFFFFF` terminator. type 1 = MRAM block (below), type 2 = ARAM samples (copied). |
| MRAM block header (`SndBlkInit` snd.cpp:2486) | sndType != 3/4 | `u32 iss_ofs@0, u32 rnd_ofs@4` (zero to iss_ofs). sndType 3/4 (BGM) blocks are an ISS block directly. |
| random SE table (`seRandomCheck` snd.cpp:640) | at rnd_ofs | `u32 tbl[32]` (0x80 bytes; relative offsets, tbl[0] unused); each `SBL_DATA` `u16 num, last, e[num]`. |
| ISS block (`Snd_iss_blk_init` snd_sub3.cpp:6) | | `u32 sit_num, wt_ofs, sit_ofs, seq_ofs`; `SND_SIT` x sit_num: `u16 note@0, pitch_l@A, pitch_h@C, flag@16` (s8/u8 fields @2..9, @E..15 copied). sit_num 0 with sit_ofs == block size occurs (wep19/41/42). |
| sequence table (`snd_seq3.cpp:152`) | seq_ofs != 0 (BGM blocks) | `u32 count, u32 ofs[count]`; MIDI data after it copied (read byte-wise, `snd_seq2.cpp:191`). |
| WTFILEHEADER (`snd_drv.h:104`) | ISS + wt_ofs | 6 `u32` offsets. Tables run to the next offset. |
| WTINST (`syn.h:13`) | [percInst, regions) | `u16 keyRegion[]` (truncated to the keys used, or 128 per instrument). |
| WTREGION 0x18 | [regions, art) | `s16 fineTune@2, s32 attn@4, u32 loopStart@8, loopLength@C, articulationIndex@10, sampleIndex@14` (u8 unityNote, keyGroup copied). |
| WTART 0x50 | [art, samples) | all 20 `s32`. |
| WTSAMPLE 0x10 | [samples, adpcm) | `u16 format@0, sampleRate@2, u32 offset@4, length@8, u16 adpcmIndex@C` (2 pad bytes copied). |
| WTADPCM 0x2E | adpcm, count = max adpcmIndex + 1 | all 23 `u16`: `a[8][2]` coefficients, gain, pred_scale, yn1, yn2, loop_pred_scale, loop_yn1, loop_yn2. |
| bio4str.hed (`snd.cpp:120`, `Snd_str_blk_init` snd_sub3.cpp:20) | | `u32 ofs[2]` (blocks); block `u32 rit_num, sbh_ofs, rit_ofs`; `u32 sbh[]` (length = first non-zero offset / 4); `SND_SHD` 0x80: `u32 flag, samples, nibbles, rate, start_nbl, lptop_nbl, lpend_nbl, offset` + 46 `u16` (`coef[16], coefR[16], gain[2], ps[2], yn1[2], yn2[2], lps[2], lyn1[2], lyn2[2]`), 4 pad bytes copied; `SND_RIT` 0x10: `s16 str_no@0, u16 flag@C` (s8 fields copied). 2 blocks: 67 RIT / 64 SHD, 240 RIT / 234 SHD. |
| bio4midi.hed (`SndMem.bgm_file`) | | `u32[24]` offsets into bio4midi.dat (17 banks, sndType 3). |
| doorse.hed (`SndDoorTbl` snd.h:74) | | `u32 file_ofs, num_ofs`; `u32 ofs[n]` at file_ofs; `u16 n` + `u16 ids[n]` at num_ofs (12 banks in doorse.dat, sndType 7). |

Real data: all 202 (disc 1) / 174 (disc 2) nested banks, the 5 bgm table files convert and check clean.
Port note (not a converter issue): `snd_iss2.cpp:149` and `snd_str2.cpp:164` read
`AXPBADDR.currentAddressHi:Lo` as one `u32` — a big-endian word-pair trick the AX shim's layout must keep.

## 4. Results on the real files

### CT-14 full run (2026-10-11, both retail discs, ~17 min convert + 10 min check)

`check: 1126 ok, 0 with problems`. Files: converted 922, partial 189, passthrough 15, dropped 0, failed 0.
Entries: converted 73405, passthrough 461, partial 2, failed 20, empty 9912. Every event packet stream and
every nested sound bank converted.

Left over: MAP 142, OSD 126, DRA 126 (passthrough by design or not analysed), TPL 27 (32-byte event
placeholders), SEQ 24 (op/ terminal scripts), PTH 5, DMY 4, `bin ` 4, core SAT 2 + BIN 1. Failed (copied
verbatim): the 12 first-pass FCVs; 3 EffEfmEnt shape headers of St1/r10b EFF (FCV key type 3, not a plain FCV);
St3 r313/r314 SMD (version 0), r322/r323 SMD (version 0x31), r30f CAM (overlapping spans) - St3 only, open.

### CT-04 first pass (historical)

Run: `cd xbox/tools && py -3 -m re4conv convert <disc1/files> <disc2/files> <out>` (both retail USA discs,
1126 distinct files, ~40 min, mostly yz2 decoding in pure Python), then `check` (1126 ok, 0 problems).
Manifest: `<out>/manifest.json`. Paths identical on both discs are converted once; the only differing path (`opening.bnr`) has its disc-2 copy in `<out>/files_disc2/`.

Files: converted 611, partial 454 (containers holding passthrough entries; every event is partial because
of its packet stream), passthrough 46 (Movie/*.sfd, bgm sound files, fonts, misc), dropped 15 (Rel/*.rel),
failed 0.

Entries (inside containers, ETM files included):

| Tag | converted | passthrough | failed | notes |
|-----|-----------|-------------|--------|-------|
| BIN | 5133 | 1 | 0 | passthrough = core.das #11 (not a model) |
| TPL | 3541 | 27 | 0 | passthrough = 32-byte placeholder `.tpl` bins in events |
| FCV | 18963 | 0 | 12 | failed (copied verbatim): pl14 #8 (32-byte FCV whose key block is in the padding), wep09/10/21/24/31/32/47 #16/#28 (key counts/offsets past the entry) — `fcv.parse` rejects them; the game probably never plays them |
| SEQ | 6576 | 24 | 0 | passthrough = op/op0N.das "SEQ" entries (terminal operation scripts, `ss_term.cpp`, not `SEQUENCE_FILE`) |
| ETM | 13 | — | — | 113 more are partial (they hold passthrough files) |
| empty | — | — | — | 9912 zero-size/zero-tag slots |
| EFF 1015, LIT 2052, MDT 315, FCS 242, UWF 157, MAP 142, SAT 139, SMD 133, CAM 128, TEX 128, SHD/SMX/FSE/EAT/CNS 127, AEV/BLK/DRA/DSE/EAR/ESE/ETS/ITA/ITM/OSD/RTP/SAR/STB 126, EMI 46, PTH 5, DMY 4, `bin ` 4, EVS 3, FNT 3, VIB 1 | 0 | all | 0 | second pass (struct families in section 2; FCS, MAP, PTH, DMY, FNT-in-body undocumented yet = **unknown**) |

Standalone: ESL 13, SizeTbl 1, bgmtbl 1 converted (`simple.py`; identical on both discs, so converted once); 235 `.bin`, 287 `.tpl`, 4 `.fcv` files
converted; `.dat` bodies 62 converted + 55 partial.

Sample runs (CT-04 acceptance set, disc 1):

| File | in -> out bytes | entries |
|------|-----------------|---------|
| em/pl00.drs | 2221088 -> 2344640 | BIN 25, TPL 15, FCV 66, SEQ 23 converted; EFF 1 passthrough; 3 empty |
| em/em10.drs | 10628000 -> 11076800 | BIN 120, TPL 75, FCV 277, SEQ 223 converted; EFF 3 passthrough; REL 424336 B dropped |
| etc/core.das | 2528000 -> 2534272 | TPL 7 (incl. CI8+CLUT thermo, RGBA8), BIN 1 converted; 28 passthrough |
| St1/r100.das | 3766400 (yz2) -> 5981184 | 32 converted (BIN/TPL/FCV incl. ETM files), 33 passthrough, ETM partial |
| St1/r100_00.dat | 28448 -> 28448 | SMD passthrough |
| Evd/r100s00.evd | 239680 -> 239840 | BIN 5, TPL 3, FCV 2 converted, MDT passthrough, packets BE |
