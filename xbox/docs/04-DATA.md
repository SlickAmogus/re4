# 04 — Data: discs, files, endianness, asset pipeline

## 1. Getting the data off the discs

The discs are RVZ images. `decomp-toolkit` reads them directly:

```sh
tools/dtk.exe vfs ls  "disc1.rvz:files"
tools/dtk.exe vfs cp  "disc1.rvz:files" gamedata/disc1/files   # ~1.4 GB
tools/dtk.exe vfs cp  "disc2.rvz:files" gamedata/disc2/files   # ~1.2 GB
```

`xbox/tools/extract_discs.py` wraps this (download of `dtk` included). Nothing from `gamedata/` is
committed.

Retail disc layout (both discs share the structure; disc 2 holds St3 and a different Movie/Evd set):

```
files/Rel/*.rel        stage code overlays (we link them statically; the files are not needed)
files/em/*.drs         enemy / player / weapon archives: models, TPLs, motions, effects, the module REL
files/St1..St4/rNNN.das  room archives (yz2-compressed), rNNN_NN.dat room block files
files/Evd/*.evd        cutscene packet streams
files/etc/core.das     core data (fonts, common textures, tables), title.das, memcard.das, *.esl enemy lists
files/etc/Eng/, ss/    language text, sub-screen data
files/bgm/             bio4bgm.sbb (BGM bank), bio4str.hed/.sbb (streams), bio4midi.*, doorse.*, bgmtbl.dat
files/font/*.fnt       fonts
files/Movie/*.sfd      Sofdec movies
files/op/*.das         codec/opening per-stage data
```

## 2. How the game reads files

`dvd.cpp`: a request queue (`cDvdQueue`) over `DVDFastOpen(entrynum)` + `DVDReadAsyncPrio` with 32-byte
aligned sizes, completion callbacks, cancel, and MRAM→ARAM DMA through `ARQPostRequest`. Files are
addressed by `FileTbl[]` index (`DvdRead(fileNo, ...)`) or by name (`DvdReadN`); the table is resolved to
entry numbers once at boot with `DVDConvertPathToEntrynum`. `datactrl.cpp` (`cDataCtrl DC`) manages
"data units" that live in MRAM or ARAM (room blocks, enemy archives) and `block.cpp` streams room blocks
(`st%x/r%03x_%02x.dat`) as the player moves. `read.cpp` loads core/option/player/weapon/enemy data and
links each archive's REL (`DLL_Link`). `file.cpp` is the SN host file server (`SETROOT:` paths) — debug
only.

Port: `DVD*` → `_open/_read` on `<datadir>/files/...` (case-insensitive lookup table built at boot from a
generated file list, because the game mixes `St1`/`st1` and `Rel`/`rel`), reads complete synchronously or
on a worker thread and are reported done from `Dvd.Watcher()`; ARAM is a 16 MB `malloc`, `ARQ` is
`memcpy`. Both discs are merged into one directory; `DVDGetCurrentDiskID()->diskNumber` returns whatever
the game asked for, `DVDChangeDiskAsync` completes immediately.

## 3. Endianness: the core decision

The GameCube is big-endian; every file is a C struct image the game casts and uses in place, often
relocating 32-bit offsets into pointers (`calcModelAddr`, `calcTplAddr`, `MotionSetCore`, `GetDataExt`).
Options considered:

1. Byte-swap at load on the Xbox, per format. Works, costs CPU and a parser per format on the console.
2. Accessor macros in game code. 500k lines; no.
3. **Convert offline to little-endian with the same layout** (chosen). The decomp's `tools/motion` does
   this already for motions ("the little-endian re-serialisation of a motion, same layout, every
   u16/f32/s16 field byte-swapped, so the game's byte-wise readers produce the GameCube values unchanged").
   The converter runs on a PC, can be unit-tested in cloud sessions against the Python parsers in
   `tools/motion/` (`fcv.py`, `modelbin.py`, `gxtex.py`, `archive.py`, `drs.py`, `yz2.py`), and lets us
   also rewrite textures and audio into NV2A/APU-native encodings in the same pass.

Consequences:

- Pointers stay 32-bit on both targets (Xbox is 32-bit; the host build is `-m32`). Offsets relocated in
  place remain valid.
- Any struct the converter does not know is a bug that shows up as garbage; the converter therefore
  validates (sizes, in-range offsets, magic words) and refuses unknown blocks loudly.
- Display-list streams: counts and indices are 16-bit BE inside the model data. The converter swaps them
  too (the interpreter in `CT-03` has a `big_endian` flag so it can read raw GC data on the host for
  cross-checking).

## 4. Formats to convert (the converter's checklist)

| Format / tag | Where                                   | Defined in                               | Notes |
|--------------|-----------------------------------------|------------------------------------------|-------|
| `.drs` / `.das` container | em/, St*/, etc/, op/       | `tools/drs.py`, `tools/motion/archive.py`| entry table of (tag, offset, size); `.das` body may be yz2 |
| yz2          | St*/rNNN.das, etc/memcard.das           | `tools/motion/yz2.py`, `src/game/yz2code.cpp`, `yz2asm.cpp` | decompress offline; Xbox never sees yz2 |
| REL in `.drs`| em/*.drs                                | `tools/drs.py rel_size`                  | dropped (static link) |
| `BIN` model  | em/, etc/, rooms                        | `include/model.h` (`cModelData`, `cPolyHeader`), `tools/motion/modelbin.py` | header, parts records, vertex/normal/colour/uv arrays, weights, display lists, shape table, joint tables |
| `TPL` texture palette | everywhere                     | `include/tpl.h`, `tools/motion/gxtex.py` | descriptors → converted images (section 5) |
| `FCV` motion | em/, Evd/, rooms                        | `include/motion.h`, `tools/motion/fcv.py` | ten key layouts |
| `SEQ`        | em/                                     | `fcv.py parse_seq`                       |      |
| `EFF`, `EST`, `SST` effects | em/, rooms, etc/         | `include/esp.h`, `espgen.h`, `est.h`     |      |
| Room blocks `SMD/SMX LIT SHD EFF/EAR SAR TEX ITM/ETM/ETS CAM BLK EVS/FSE/AEV/ITA` | St*/ | `include/block.h`, `smd.h`, `light.h`, `atari.h`, `cam_ctrl.h`, `sce.h` | one struct family each |
| `ESL` enemy lists | etc/*.esl                          | `include/em_set.h`                       |      |
| Collision `SAR`/`EAT` | rooms                          | `include/atari.h`, `at_sub.h`            |      |
| `EVD` cutscenes | Evd/                                 | `include/event.h`                        | packet streams |
| Sound banks | bgm/*.sbb, *.hed, doorse.*, bio4midi.* | `include/snd_drv.h` (`WTFILEHEADER`, `SHD`, `SND_IBLK`) | DSP-ADPCM → Xbox ADPCM (03-AUDIO) |
| Text / fonts | etc/Eng, ss/, font/*.fnt               | `include/mes.h`, `include/font.h`        |      |
| Sub-screen `ss_*.dat` | ss/<lang>/                    | `src/Sscrn/*.cpp`                        |      |
| Movies `.sfd`| Movie/                                 | CRI                                      | **not converted** (CRI parses its own BE stream) |
| `SizeTbl.dat`, `bgmtbl.dat`, `op/*.das` | etc/, bgm/, op/ | `include/dvd.h`, `snd.h`, `ss_term.cpp` |      |

Converter design (`CT-04`): Python package `xbox/tools/re4conv/` with one module per format, each exposing
`convert(bytes) -> bytes` and a `check(bytes)` validator, driven by a manifest of the disc tree. The same
struct knowledge is mirrored into C (`xbox/port/data/formats/*.h`) only where the Xbox needs to walk a
structure at runtime (textures bind, DL interpreter). Output: `out/files/...` mirroring the disc plus
`out/manifest.json` (original sizes, hashes, texture handles, ADPCM address maps).

## 5. Texture output format

`ConvertedImage` header (little-endian): magic `RE4X`, width, height, NV2A format code, mip count, swizzled
flag, data size, then the swizzled (or DXT1) image bytes, mips appended. The TPL descriptor's `data`
pointer is rewritten to point at this blob inside the converted TPL so `GXInitTexObj(&obj, hdr->data,
w, h, format, ...)` binds without copying. CI formats are expanded through their TLUT at conversion;
`GXLoadTlut` becomes a no-op. The thermal palette swap (`CalcTplAddrC8`) is handled by converting the
thermo TPL twice.

## 6. Saves

`card.cpp` writes a fixed image (blocks of 8 KB, `CARDCreateAsync(chan, name, blocks << 13, ...)`) with
icon/banner `CardStat`. Port: `E:\UDATA\<titleid>\<slot>\RE4SAVE.BIN` (or next to the XBE when run from
HDD, as Duke probes), same bytes, so the save format stays the game's own. Two "slots" emulate the two
memory-card channels.

## File resolution

Audit of every name the game asks for against the retail USA discs: `py -3 xbox/tools/filelist_check.py`
(input `xbox/tools/retail_filelist.txt`, output `xbox/port/dvd/filenames.inc`, task `CT-01`).

**How a name becomes a disc read.** `FileTblEntry FileTbl[]` (`src/game/dvd.cpp:7`, 254 entries of
`{"path", entrynum}`, `include/dvd.h:231`) is resolved once at boot by `cDvd::FileTblExistCheck`
(`dvd.cpp:1261`: `FileTbl[i].entrynum = DVDConvertPathToEntrynum(FileTbl[i].name)`, -1 when absent).
`DvdRead(fileNo)` (`dvd.cpp:346`) uses that cached entrynum; `DvdReadN(name)` (`dvd.cpp:365`) copies the
string into `DvdReqWork.fname` and `cDvdQueue::Initialize` converts it again at request time
(`dvd.cpp:864`). `cDvd::FileExistCheck` (`dvd.cpp:1275`) is the same conversion plus `DVDFastOpen` for the
length. The SDK conversion is **case-insensitive** (`src/lib/dvdfs.c:50`, `isSame()` compares with
`tolower`), which is why the code can say `rel/st1_0.rel`, `st1/r100.das`, `SS/cmn/...`, `Font/...`,
`movie/...`, `etc/sizetbl.dat` while the discs spell them `Rel/`, `St1/`, `ss/Cmn/`, `font/`, `Movie/`,
`etc/SizeTbl.dat` (926 of the requested names differ only in case). The Xbox shim must lower-case both
sides of its lookup; `filenames.inc` records the exact disc spelling for every name.

**`.das` vs `.drs`.** `FileTbl` names every enemy, player and weapon archive as `em/<name>.das`
(126 entries), but no disc, retail or debug, has such a file: both carry `em/<name>.drs`
(`config/G4BE08/config.yml:18` says the REL of the debug disc is inside `files/em/<name>.drs` too). The
mechanism is the macro `SET_DRS_NAME(p)` in `src/game/read.cpp:107-113`: it walks `p` to the `.` and
overwrites the three extension bytes with `drs` **in the `FileTbl[]` string itself** (the table is
writable `.data`, so the entry stays `.drs` afterwards). It runs right before the by-name read in
`readEmData` (`read.cpp:383-387`, when the `EmFileTbl` row has `dll != 0`, which is every row with a
file), `ReadPlayerData` (`read.cpp:651-654`, unconditionally) and `ReadWepData` (`read.cpp:919-925`,
when `dll != 0`); all three then call `DVD_READ_N(FileTbl[..].name, ...)`, so the fresh
`DVDConvertPathToEntrynum` sees the `.drs` spelling. The boot-time entrynum of those entries is -1 and is
never used: no `DvdRead(fileNo)` caller uses an `em/` index (the by-index readers are `snd.cpp:92/102/108/
114/130/1690/2518` for `bgm/*` and `etc/memcard.das`, `read.cpp:199` for `etc/core.das`, and
`roomdata.cpp:297` for the room RELs `rel/stN_M.rel`). The companion `rel/<name>.rel` entries (98) are
only used as a non-zero "has a DLL" flag (`EmFileTbl` `{file, dll, 1}` rows, `read.cpp:272-343`,
`wep_data_*` `read.cpp:727-797`); the REL is carved out of the `.drs` after the read. `filenames.inc`
therefore lists each `em/*.das` as an alias of its `.drs` file (`filetbl_das_alias`) so the shim's boot
check succeeds, and the `rel/em*/wep*/pl*.rel` names as `RE4_FILE_MISSING` (debug-only).

**Dynamic names** (hand-listed in the script with their source line): room archive
`st%x/r%03x.das` (`read.cpp:158`), room sub-block `st%x/r%03x_%02x.dat` (`block.cpp:662`), events
`evd/rNNNsNN.evd` built by `EvtReadSub` from the 80 `"event/evd/..."` literals via `strstr("evd/")`
(`event.cpp:2295`) and `NameChange` (`event.cpp:2258`, `r`→`s` for the alternate costume: the discs have
38 `Evd/s*.evd`), `SS/%3s/option.dat` (`read.cpp:219`), `Font/stage%1d_j.fnt` / `Font/event%1d_j.fnt`
(`mes.cpp:461/465`), `SS/___/f%02dA.tpl` (`ss_file.cpp:889`), `SS/item/cap%02d` and `idm%03x` `.bin/.tpl`
(`ss_main.cpp:498-528`), `SS/cmn/ss_wep%02ld.dat` (`ss_model.cpp:71`), `SS/cmn/itm%02x.bin/.tpl`
(`sce_at.cpp:936`), `op/op%02d.das` (`ss_term.cpp:727`), `SS/cmn/ss_oc<n>.dat` (`ss_term.cpp:644`),
`SS/cmn/map_obj*.dat` (`ss_map.cpp:2300-2351`), `SS/___/chap%02ld.dat` (`sce_com.cpp:749`), and the
`"SS/___/"` language prefix filled by `setLangExt3` (`option.cpp:39`) / `sscrnSetLanguage`
(`sscrn.cpp:103`) with `jpn/eng/ger/fra/esp/ita`. The USA discs only ship `ss/Eng`, `ss/Cmn`, `ss/Item` and
`etc/Eng`, so the port must pin `pSys->language = 1`. Saves use `ss/cmn/save_<j|e|g|f|s|i>.dat`
(`card.cpp:172`, `fileext[] = "jeegfsie"`).

**Result** (1125 unique retail paths, 846 on both discs, 172 disc-1-only, 107 disc-2-only = `St3/*`,
`Evd/r3*`+`s3*`, `Rel/st3_*`, `Movie/ending.sfd`, `Movie/r333_ev.sfd`; `St2/r22c.das` is the one St2
file on disc 2): 1213 requested names resolve; every FileTbl entry resolves or is classified. Not on
retail: `FileTbl` `etc/moji8.tpl` (debug font, `eprintf.cpp:325`), `etc/title.das`, `etc/youdied.tpl`,
`etc/thank.tpl` (never read by index), `em/em06.das`, `em/wep99.das` (no table row names them);
`Etc/Ending.tpl` (`game.cpp:984` ending still, retail ends through `Movie/ending.sfd`), `movie/dmy.sfd`
(fallback, absence handled `sofdec.cpp:513`), `movie/e3_jpn.sfd` (JPN title), `etc/jpn/*` (language 0),
`evd/r100s01/02/42.evd` (dead `r100_evtName[]` rows, `r100.cpp:411`), `op/op04.das` (stage 4 has no
radio). Never requested by any name or pattern (37): `em/pl02.drs`, `em/pl04.drs`, `em/pl21.drs`,
`etc/emleon08/09.esl`, `etc/Eng/event.eff`, the `font/*_p.fnt`, `font/dvd_j.fnt`, `font/ss_*`,
`font/sscrn_*` fonts, `ss/Cmn/f01..12.tpl`, `ss/Cmn/map_obj2.dat`, `ss_em18.dat`, `ss_tel01.dat`,
`ss/Eng/chapter.dat`, `ss/Eng/tel*.fcv`, `ss/Eng/title.snd`, `opening.bnr`, `znull.dat` (some are read by
names inside archives or by the sub-screen REL's own tables; keep them in the converter manifest).
Debug-only paths (never a disc read): `debug/config.txt` (`debug.cpp:422`), `debug/roomInfo.dat`
(`main.cpp:366`), `d:\bio4/...` SN-host reads (`card.cpp:638/866`, `dvd.cpp:868` `SYS_SN_PC_READ`),
`x:/soft/...` (`event.cpp:365`).
