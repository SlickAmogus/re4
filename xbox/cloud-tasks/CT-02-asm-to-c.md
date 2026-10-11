# CT-02 — C replacements for the PowerPC assembly

**Status:** open

## Goal

Every instruction of PowerPC assembly the port would otherwise compile has a C (or C + SSE) equivalent in
`xbox/port/`, with unit tests that compare against the semantics documented in the decomp.

## The list (from `xbox/docs/01-ANALYSIS.md`, "Assembly in game code")

| Site | File | What it does | Replacement |
|------|------|--------------|-------------|
| `CalcSk1_x(dst, src, n)` | `src/game/trans.cpp` ~2486 | skin n vertices: src = s16 x,y,z + s16 matrix index (8 bytes), palette of 3×4 float matrices at `0xE0000000 + idx*0x30` (locked cache), GQR6 scale; dst = s16 x,y,z (6 bytes) | `port/gx/skin.cpp`: palette pointer passed explicitly (see how `calcWeightMat`/`MakeWeightPalette` fill the locked-cache palette; the port stores it in a normal array) |
| `CalcSk1_x2` | same | s8 normals + u8 index (4 bytes) → s8 x,y,z (3 bytes) | same file |
| `setupGQR6(v)` | same | sets paired-single quantisation (scale bits) | store the scale in a global the C skinning uses |
| `PSQ_L_U8(p)`, `PSQ_L_U8_TO(dst,p)` | trans.cpp ~87 | u8 → f32 via GQR2 | `(f32)*(u8*)p` (check GQR2 scale set in `TaskExec_hook`: `li 3,4; oris 3,3,4; mtspr 914` → GQR2 = 0x00040004: type u8, scale 0) |
| `PSQ_L_S16`, `PSQ_ST_S16` | `src/game/shape.cpp` 178 | s16 ↔ f32 via GQR5 | find GQR5 value (scheduler.cpp `TaskExec_hook`), apply the scale |
| psq loads | `Espgen42.cpp`, `espgen45.cpp`, `em2b/em2b.cpp`, `dbmodule.cpp` | similar | same pattern |
| `SQRTF` | `src/game/math_sub.cpp` 656 | frsqrte + one Newton step | `sqrtf`, or the same estimate+Newton for bit-closeness |
| `SINF`, `COSF` | math_sub.cpp ~718/755 | paired-single polynomial over `Coeff[]`, `powx`, `sum` | port the polynomial in scalar C using the same `Coeff` table (read the asm) |
| `LIMIT_ANGLE` | math_sub.cpp 790 | wrap to [-PI, PI) by repeated add/sub | trivial loop |
| GQR setup | `src/game/scheduler.cpp` `TaskExec_hook` | `mtspr 912..919` | no-op, but record the constants |
| `memset_asm`, `memclr_asm`, `mtxclr_asm` | `src/game/memset_2.cpp`, `main_mem.cpp` | fills | `memset` wrappers (note `memclr_asm(p, n)` with `n` not multiple of 4 — byte tail) |
| yz2 decoder | `src/game/yz2asm.cpp` + `yz2code.cpp` | range-coded LZ, see `tools/motion/yz2.py` docstring | C port in `port/data/yz2.c`; the Python is the reference oracle |
| `exception.cpp` | — | OS error handler | not compiled |
| CRI units with asm | `src/lib/{mpv_hdec,mpv_lib,mpv_mc,mpv_umc,sfd_adxt,sfd_hds,sfd_mpv,sfd_pts,adx_dcd5,adx_tsvr,cri_cvfs}.c` | cache flush/invalidate, `mftb` timers, MWCC codeless pins | handled in CT-07 |

Also: 97 `register T x asm("rN")` pins and ~400 `asm("" : ...)` levers emit nothing; the preprocessor
(CT-03) strips the pins. Do not touch `src/`.

## Deliverables

- `xbox/port/gx/skin.cpp/.h`, `xbox/port/mtx/math_ppc.cpp/.h` (SQRTF/SINF/COSF/LIMIT_ANGLE),
  `xbox/port/os/memfill.c`, `xbox/port/data/yz2.c/.h`.
- `xbox/port/tests/`: a small test runner (plain C++, `make -C xbox/port/tests`) that checks: skinning
  against a naive float implementation on random data; SINF/COSF against `sinf/cosf` within 1e-5 over
  [-4π, 4π]; LIMIT_ANGLE; memfill edge cases; yz2 against `tools/motion/yz2.py` on a synthetic stream
  (write a tiny Python *encoder* for the test, or round-trip the decoder on a known-good packed sample if
  the user attaches `files/etc/memcard.das`, 60 KB).
- A note in `xbox/docs/07-COMPILER.md` listing the GQR constants and what each paired-single site assumed.

## Acceptance

`make -C xbox/port/tests && ./xbox/port/tests/run` passes on Linux x86-64 and with `-m32`.
