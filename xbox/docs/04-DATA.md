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
