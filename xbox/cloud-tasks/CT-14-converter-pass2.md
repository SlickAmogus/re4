# CT-14 — Asset converter, second pass: room blocks, effects, events, sound banks

**Status:** mostly done (2026-10-11, local run on the retail discs): sound tables, all room tables, EFF/EST/SST, event packets, MDT, fonts, UWF converted; MAP/PTH/DMY/OSD/DRA left passthrough. See progress/CT-14.md and FORMATS.md 2a/3a.

## Goal

Everything the first pass (CT-04, `xbox/tools/re4conv/FORMATS.md`) copies as passthrough gets converted
to the little-endian layout, so the game can use it in place on x86:

| Format | Where | Struct source |
|--------|-------|---------------|
| Room blocks `SMD/SMX LIT SHD EFF/EAR SAR TEX ITM/ETM/ETS CAM BLK EVS/FSE/AEV/ITA` | `St*/rNNN.das`, `rNNN_NN.dat` | `include/block.h`, `smd.h`, `light.h`, `atari.h`, `cam_ctrl.h`, `sce.h`, `item.h` and the loaders in `src/game/block.cpp`, `smd.cpp`, `light.cpp`, `atari.cpp`, `cam_ctrl.cpp`, `sce_*.cpp` |
| Effect data `EFF`, `EST`, `SST` | em/, rooms, etc/ | `include/esp.h`, `espgen.h`, `est.h`, `eff_sys.cpp` |
| Event packet streams (the part of `.evd` left big-endian) | `Evd/*.evd` | `include/event.h` (`EvtPacket` at :160), `src/game/event.cpp` |
| `MDT` (message data) and fonts | `etc/Eng/*`, `ss/*`, `font/*.fnt` | `include/mes.h`, `font.h`, `mes.cpp` |
| Sound banks and streams | `bgm/*`, the type-4 record of `.drs` | `include/snd_drv.h` (`WTFILEHEADER`, `SND_IBLK`, `SHD`), `snd_ram.cpp`, `snd_str*.cpp`; samples DSP-ADPCM → Xbox ADPCM with `re4conv/adpcm.py` (CT-06) and an address map (see `03-AUDIO.md`) |
| Undocumented tags `FCS MAP PTH DMY` | various | find the readers with `GetDataExt(arc, "TAG"`, document in `FORMATS.md` |

## Method (the pattern that worked in CT-04)

For each format: find the loader in `src/` (where `GetDataExt(..., "TAG", n)` or the file name is used),
list every field the code reads and its width, write `convert(bytes)` swapping field by field, and a
`check()` that re-parses the output and compares with a big-endian parse of the input. Offsets that the
game relocates in place (`calc*Addr` style functions) must stay offsets. Validate: sizes and every offset
in range; refuse unknown versions loudly.

Record progress in `xbox/cloud-tasks/progress/CT-14.md` as you go (the ct-worker rules,
`.claude/agents/ct-worker.md`).

## Acceptance

`py -3 -m re4conv check` on the attached samples reports each newly handled tag as converted;
unittests with synthetic big-endian structures for each format; `FORMATS.md` status table updated.
