# CT-04 — Asset converter (`re4conv`)

**Status:** done (first pass) — 2026-10-11. `py -3 -m re4conv convert|check` (run from `xbox/tools`)
converts both retail discs (1126 files: 611 converted, 454 partial, 46 passthrough, 15 dropped, 0 failed;
`check` 1126 ok); unittest suite in `re4conv/tests` passes. Containers, BIN, TPL, FCV/SEQ, ESL/SizeTbl/bgmtbl
are converted; room blocks, EFF, event packet streams, MDT, fonts and sound banks are passthrough (struct
families in `re4conv/FORMATS.md`, second pass). 12 malformed FCVs (pl14, wep*) are copied verbatim.
Progress/notes: `xbox/cloud-tasks/progress/CT-04.md`.

## Goal

A Python package `xbox/tools/re4conv/` that turns the extracted retail disc trees (`disc1/files`,
`disc2/files`) into the little-endian asset tree the port loads (`xbox/docs/04-DATA.md`), one module per
format, each with a validator, plus a manifest.

## Start from what exists

- `tools/drs.py` (containers), `tools/motion/archive.py`, `yz2.py`, `fcv.py` (motion parse +
  **serialise with endian flag** — this is the pattern), `modelbin.py` (model header/parts),
  `gxtex.py` (TPL + GC texture decode to RGBA).
- Struct definitions in `include/` (`model.h`, `tpl.h`, `motion.h`, `block.h`, `smd.h`, `light.h`,
  `atari.h`, `cam_ctrl.h`, `sce.h`, `esp.h`, `espgen.h`, `est.h`, `em_set.h`, `event.h`, `snd_drv.h`,
  `mes.h`, `font.h`, `dvd.h`). Field offsets are exact; placeholder names (`x1C`) still have exact types.

## Scope for this task (first pass)

1. Framework: `re4conv convert <disc1/files> <disc2/files> <out>` walks both trees, dispatches on
   path/extension/container tag, writes `out/files/...` + `out/manifest.json`. Unknown formats are copied
   verbatim **and listed** in the manifest as `passthrough`, so nothing silently goes missing.
2. Containers: `.drs`/`.das` entry tables (swap), yz2 bodies decompressed (the Xbox never sees yz2), the
   embedded REL entry dropped (size kept in the table as 0 and noted).
3. `BIN` models: header, parts records, arrays (positions s16, normals s8/s16, colours RGBA8 untouched,
   texcoords s16/u16), weights, joint/blend/flip tables, shape table, and the **display-list streams**:
   walk the FIFO stream (opcode, u16 count, per-vertex indices per the model's implied VCD — see
   `trans.cpp` `commonModelTrans` for which attributes exist: pos idx16, nrm idx16, clr idx16 if flags
   bit31, tex0 idx16, and `GXMatrixIndex1u8` for rigid multi-part models) and swap the 16-bit indices and
   counts. Validate: stream ends exactly at `buff_size`, indices < array counts.
4. `TPL`: descriptors swapped; images converted to `ConvertedImage` blobs (section 5 of `04-DATA.md`):
   GC tiled → linear RGBA via `gxtex.py`, then to the NV2A target format and Morton swizzle (copy the
   swizzle function from Starship's `gfx_nv2a.cpp`/Duke), DXT1 kept as DXT1 (re-tile the 4×4 blocks and
   swap the two colour u16s, swap bits within the 2-bit index rows order), mips preserved. CI4/CI8 expanded
   through the TLUT.
5. `FCV`/`SEQ`: via `fcv.serialise(m, '<')`.
6. Everything else in the table in `04-DATA.md`: implement the simple fixed-layout ones (ESL, SizeTbl,
   bgmtbl, font headers), leave complex room blocks for a second pass but **document each block's struct
   family** in `xbox/tools/re4conv/FORMATS.md` with the header file and the first field offsets.

## Verification without the discs

- Synthetic round-trips: build a BE structure with `struct.pack('>...')`, convert, compare with
  `struct.pack('<...')`.
- Byte-level oracle for motions: `fcv.parse(converted, '<') == fcv.parse(original, '>')`.
- Ask the user to attach `files/em/pl00.drs` (Leon, ~6 MB) and `files/etc/core.das` for a real run;
  the converter's `check` mode must report every entry as validated or passthrough.

## Acceptance

- `python3 -m re4conv convert` on the attached samples produces output; `python3 -m re4conv check`
  validates every converted entry; `pytest xbox/tools/re4conv/tests` passes.
- `FORMATS.md` lists every format seen on the retail disc with its status (converted / passthrough /
  unknown).
