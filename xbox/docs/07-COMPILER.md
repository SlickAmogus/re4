# 07 — Compiler: from SN ProDG GCC 2.95 to clang (x86)

## Targets

- Xbox: nxdk's clang (`-target i386-pc-win32 -march=pentium3`), lld, libc++ from nxdk, pdclib. C++ with
  `-std=c++20 -fpermissive -fexceptions` (Starship's settings), 32-bit.
- Host: `clang`/`gcc -m32 -std=gnu++14 -fno-strict-aliasing -fpermissive` on Linux, SDL2 + OpenGL
  (32-bit libs: `gcc-multilib`, `libsdl2-dev:i386`, `libgl-dev:i386`). 32-bit is mandatory: the game
  stores pointers in `u32`, computes member addresses from literal byte offsets (`MOT_HIST`, `FlagChk`,
  `U32SetOfs`), and relocates file offsets in place.

Both use the same `xbox/port/include/re4port/re4_xbox.h` force-included (`-include`) into every game unit.

## What the force-included header does

```c
#define RE4_PORT 1
// 1. neutralise the matching levers
#define asm __re4_asm                      // see below: register pins and empty asm() statements
#define __attribute__(x) ...               // keep aligned(32) (needed by GX data), drop the rest
// 2. SDK types the game headers expect from <dolphin/...>
// 3. address macros: GC_ADDR(), VALID_PTR(), the fixed memory map constants
// 4. rename clashes: main -> re4_main, _prolog -> <module>_prolog (per-unit -D), memclr_asm -> re4_memclr
// 5. compiler shims: __builtin_expect, likely/unlikely, OSReport -> xbox_log
```

### `register T x asm("rN")` pins (97) and `asm("" : ...)` statements (~400)

They are GCC-syntax and clang accepts them on x86 only if the register name exists. A sed-style
preprocessing step is safer than relying on the compiler: the build generates `build/prep/<unit>.cpp`
from each game source with `tools/motion/host/prepare.py`-style rules (already used by the decomp's host
build): strip `asm("rN")`/`asm("fN")` from declarations, keep empty `asm("")` (valid everywhere), replace
the whole-function asm bodies (`yz2asm.cpp`, `memset_2.cpp`) and the paired-single blocks (list in
`01-ANALYSIS.md`) with `#include` of the C versions. `#line` directives are kept so errors point at the
real file. The Makefiles run this step per unit; nothing under `src/` changes.

### Expected clang complaints (fix by category)

- `(u32) ptr` casts: fine on 32-bit. `(void*) 0x80460000`: through `GC_ADDR`.
- GCC 2.95 implicit `int`, old-style `for` scoping, `friend` lookups, templates (`cManager<T>`, `cDbgToolMain<T>`):
  `-fpermissive -Wno-*`; the decomp was written to be readable so this is expected to be moderate.
- `typedef signed long s32;` in `include/types.h`: `long` is 32-bit on both targets. Keep.
- `operator new` returning zeroed memory from the game heap (`main_mem.cpp`): keep, link our own.
- `extern "C" void __main();` and `__start`: not linked.
- Name clashes with Windows headers (`SetCursor`, `DeleteFile`-like names, `Rect`, `Mtx`?): expect some;
  nxdk's `windows.h` is only included by the port layer, never by game units.
- `#pragma` from CodeWarrior sources in `src/lib`: `-Wno-unknown-pragmas`.
- `-fno-strict-aliasing` is required: the matching code aliases through casts everywhere.
- Floating point: PPC `fmadds` fuses; x86 SSE does not. The decomp's host tool offers `-ffp-contract=fast`
  (`-mfma`) to mimic; on a Pentium III there is no FMA, so the game will drift by 1 ulp in long chains.
  Gameplay does not depend on bit-exact floats, but note it for physics/QTE oddities.

### Statically linked modules instead of RELs

Each REL unit defines `extern "C" _prolog/_epilog/_unresolved`. The Makefile compiles each module
directory with `-D_prolog=<mod>_prolog -D_epilog=<mod>_epilog -D_unresolved=<mod>_unresolved` and the port
provides `port/os/module_table.cpp`: `{ "em10", em10_prolog, em10_epilog, bss_size }`. `DLL_Link`
(`main_sub.cpp`) → `re4_module_link(name)` looks the module up by the archive it came from and calls its
prolog; the REL bytes inside the `.drs` are ignored. Static data of all 114 modules therefore coexists in
one image; the GC loaded one stage module at a time into the DLL heap, so static initialisers that assume
"fresh bss on each load" must be re-run from the prolog (audit in `CT-03`).

The `Tools`, `t_*` modules are not compiled. `db_menu.cpp`'s table that names them stays (dead at runtime).

## Observed (keep appending as the builds progress)

- **2026-10-11, skeleton XBE builds.** nxdk's clang (`i386-pc-win32`) does not define `__GNUC__`, so
  `include/dolphin/types.h` hits `#error unknown compiler`; `re4_xbox.h` defines `__GNUC__ 4` before
  anything else (scoped to our units, nxdk libs untouched). C++ units use `NXDK_CXXFLAGS`, not
  `NXDK_CFLAGS`, so include paths must be added to both. `OutputDebugStringA` is `WINAPI` (stdcall):
  an implicit declaration links as cdecl and fails at link time; include `<windows.h>`. nxdk's
  `GEN_XISO` needs extract-xiso built with cmake; we create ISOs with the standalone
  `extract-xiso.exe` instead.

- **2026-10-11, first game-unit sweep (543 C++ units, nxdk clang, `make -k RE4_GAME=1`).** 221 compiled
  untouched. 386 failed, 504 errors in these categories, all now handled by `tools/prep.py` or flags:
  - 350 `invalid output constraint in asm`: the codeless levers `asm("" : "+r"(x))`, `"=m"`, `"=f"`,
    `: : "r"(x)` (x86 clang rejects `f`/`b`/`m`-only forms). prep.py rewrites tied outputs
    (`"=r"(a) : "0"(b)`) to `a = b;` and drops the rest; `asm volatile("" ::: "memory")` stays.
  - SDK matrix sources (`src/lib mtx.c mtxvec.c mtx44.c vec.c quat.c`) carry MWCC `asm void PSVECAdd(...) {
    psq_l ... }` function bodies next to the C versions: prep.py strips them; `re4_xbox.h` maps every
    `PS*` name the game uses to its `C_*` version (`PSMTXReorder` has no C version: `port/mtx/mtx_extra.c`).
    `fake_tgmath.h` (GC inline `sqrt(float)` etc. with `__frsqrte`) conflicts with pdclib: replaced by `<math.h>`.
  - `'wep/pl_handgun.cpp' file not found`, `'fdlibm/sf_sin.c'`: modules include shared sources relative
    to `src/`; add `-I src -I src/lib`.
  - `'lineSpace' is a private member of 'Message'` (rooms r20e/r224/r307): `-fno-access-control`.
  - `taking the address of a temporary object` (`&((Vec) {0,0,0})` compound literals in em2b/em39):
    `-Wno-address-of-temporary`. `case -1` on a `u32` switch (dvd.cpp, snd_str4.cpp): `-Wno-c++11-narrowing`.
  - `assigning to 'char *' from 'const char *'` (`strstr`/`strrchr` results in xml.cpp, main_mem.cpp,
    r332.cpp): literal site patches in prep.py.

### GQR constants (CT-02)

Paired-single `psq_l`/`psq_st` convert through a GQR (SPR 912+n). Field layout (LSB bit numbers):
LD_SCALE = bits 24-29 (6-bit signed), LD_TYPE = 16-18, ST_SCALE = 8-13, ST_TYPE = 0-2; types 0 f32,
4 u8, 5 u16, 6 s8, 7 s16. Load = int * 2^-LD_SCALE; store = trunc(f * 2^ST_SCALE), saturated.
Decoder: `re4_gqr_*` in `xbox/port/gx/skin.cpp`.

| GQR | Value | Set by | Meaning | Sites that assume it | C replacement |
|-----|-------|--------|---------|----------------------|---------------|
| 0 | 0 (SDK) | OS | f32 | `psq_l x,y,0,0` in math_sub.cpp SINF/COSF, trans.cpp palette loads | plain float |
| 2 | 0x00040004 | scheduler.cpp `TaskExec_hook` | u8, scale 0 | trans.cpp `PSQ_L_U8(_TO)`, Espgen42.cpp:448, espgen45.cpp:77 | `(f32)*(u8*)p` |
| 3 | 0x00050005 | TaskExec_hook | u16, scale 0 | none found | `(f32)*(u16*)p` |
| 4 | 0x00060006 | TaskExec_hook | s8, scale 0 | none found | `(f32)*(s8*)p` |
| 5 | 0x00070007 | TaskExec_hook | s16, scale 0 | shape.cpp:178/179 `PSQ_L_S16`/`PSQ_ST_S16`, dbmodule.cpp:823 | `(f32)*(s16*)p`; store = trunc + clamp to s16 |
| 6 | (shift<<24)\|(shift<<8)\|0x00070007 | trans.cpp:750 `setupGQR6(d->shift)` | s16 fixed point, scale `shift` | `CalcSk1_x` positions | `re4_calcsk1_s16(.., re4_gqr6)` |
| 6 | 0x32073207 | trans.cpp:771 | s16, scale 50 = -14 (load x2^14, store x2^-14) | `CalcSk1_x` on normals: the scale makes the translation column negligible | same |
| 6 | 0x20062006 | trans.cpp:776 | s8, scale 32 = -32 (load x2^32, store x2^-32) | `CalcSk1_x2` on s8 normals, same trick | `re4_calcsk1_s8(.., re4_gqr6)` |

GQR2-5 never change after `TaskExec_hook`, so on x86 the `mtspr` block is a no-op and the casts above
are exact. `setupGQR6(v)` becomes `re4_gqr6 = v` (xbox/port/include/re4port/skin.h); the skinning loops
decode the scale from it. Float-to-int stores truncate toward zero and saturate (Gekko semantics),
not round.

- **2026-10-11, the whole game links (Xbox).** `make RE4_GAME=1` builds 292 DOL-side units + 338 module
  units (105 REL modules) into `bin/default.xbe` (14.8 MB with debug info). What it took after the asm
  categories above:
  - `prep.py` also preps every header (`--dir include`), writes `#line` paths with forward slashes (clang
    reads `\o` in `"..\include\obj.h"` as an escape), handles asm levers inside macro bodies with a
    paren-balanced scanner, strips `.section` asm, the GQR setup in `scheduler.cpp`/`main.cpp`, every PS*
    SDK function incl. `static __PSMTX44RotAxisRadInternal`, and drops `asm("gcc_name")` labels.
  - Placement `operator new` in `cManager.h`/`cam_extra.h` collides with libc++ `<new>`: replaced by `#include <new>`.
  - `math_sub.h`'s `static inline fabsf` merges with pdclib's extern declaration under MSVC mode and
    becomes an exported duplicate: replaced by `__builtin_fabsf`. The game's fdlibm and SN newlib units
    (`src/game/*.c`, `src/lib/sf_*`) are dropped in favour of pdclib.
  - Game C++ is built `-fno-exceptions` (no EH in the game; avoids `__CxxFrameHandler3`).
  - The debug units of `src/game` stay in: `pLog`, `Draw_*`, `DbMenuExec` are called from everywhere.

## Linking the modules statically (implemented)

`tools/gen_units.py` takes the unit lists of the 114 RELs from `config/G4BE08/modules.py` (shared sources
such as `st/em_wrap.cpp` v1/v2/v3, `wep/pl_handgun.cpp`, `st1/st1.cpp` compile once **per module**, into
`build/mod/<unit>.<source stem>.obj`) and skips the 9 debug-tool RELs. `tools/isolate_modules.py` renames
every defined symbol of a module to `re4m_<mod>_<sym>` (llvm-objcopy `--redefine-syms`), entry points to
`re4mod_<mod>_prolog/_epilog/_unresolved`, keeps COMDAT/common symbols mergeable (COFF COMDAT found
from section characteristics with llvm-readobj), and gives a strong symbol defined by two objects of the
same module a per-object name (st4_0: r405 and r406 both define `setTexRender`; the SN linker resolved
each locally). Each module becomes `build/mod/<mod>.lib`.

`tools/link_resolve.py` closes the link: a probe link (`/demangle:no`) lists the undefined symbols and
each is bound with `/alternatename` (written to `build/link_aliases.rsp`):

| Rule | Count (2026-10-11) | Example |
|------|-------------------|---------|
| module import (REL→REL, and stage tables naming rooms of sibling RELs) | 223 | `?R105Init@@YAXXZ` → `re4m_st1_1_?R105Init@@YAXXZ` |
| explicit GCC 2.95 runtime / asm-label names | 9 | `___builtin_new` → `??2@YAPAXI@Z` |
| data type mismatch (MSVC mangles data types, GCC 2.95 did not) | 6 | `?Game@@3UGameWork@@A` → `?Game@@3UGAME_WORK@@A` |
| asm-labelled data, return-type mismatch, C/C++ linkage | 5 | `em21_trap_cam` → `re4m_em21_?em21_trap_cam@@3UCAMERA@@A` |
| missing implementation → `re4_unimpl_fn` / `re4_unimpl_data` (`build/link_stubs.c`) | 214 | `_AXAcquireVoice`, `_DVDFastOpen`, `_CARDProbeEx` ... |

The missing list, grouped by subsystem, is `docs/sdk-symbols.md`; it is the Phase-1 implementation to-do
list. `port/os/module_table.cpp` replaces `OSLink`/`OSUnlink`: it reads the module id from the REL header
the game loaded (so the asset converter must keep REL headers in the `.drs`) and calls that module's
renamed `_prolog`. `port/os/alias_thunks.cpp` forwards the two `__thiscall` members that room code calls
through free-function asm labels, and provides empty `_ctors`/`_dtors` (the CRT runs static constructors).

### Module lifecycle (CT-12, implemented)

A REL read from disc starts with its initial `.data`, zeroed `.bss`, and runs its static constructors.
`tools/gen_modseg.py` writes a per-module header force-included into every unit of the module:

```c
#pragma data_seg(".data$re4m_<mod>$m")
#pragma bss_seg(".bss$re4m_<mod>$m")
#pragma init_seg(".re4ct$re4m_<mod>$m")
```

plus a marker object defining `re4seg_{data,bss,ct}_{begin,end}` in the `$a`/`$z` subsections (renamed by
isolation to `re4m_<mod>_re4seg_*`). lld sorts `$`-grouped sections by name, so every module's data is
one contiguous range, and its dynamic initialisers are out of `.CRT$XCU` (the CRT no longer runs them at
boot; verified: no module object has a `.CRT$XCU` section, 2 modules have initialisers). `llvm-objcopy`
cannot rename COFF sections, which is why this is done with compiler pragmas.

`port/os/module_table.cpp`: `re4_modules_init()` (before `re4_main`) snapshots each module's `.data`.
`OSLink` of a freshly read header restores `.data`, zeroes `.bss` and runs the initialisers; `OSUnlink`
saves `.data`+`.bss`, and an `OSLink` of the same header address that `dvd_shim.cpp` has not overwritten
since (`re4_dvd_overwritten_since`) is a `cRoomData::restartRelData` and restores that saved state.
The game calls `_prolog`/`_epilog` itself through the header (`DLL_PROLOG`, `DLL_Unlink`).

## Linking the modules statically: duplicate symbols (original plan)

Each REL was its own link unit, so modules legitimately define the same symbols: every `wepNN` compiles
its own copy of `src/wep/pl_handgun.cpp` (via `#include`), every stage module links `src/st/*.cpp`, and
all define `_prolog/_epilog/_unresolved`. In one image these collide. Plan (CT-03):

1. Compile each module's units into its own object set (already separated by `units.mk`).
2. `llvm-nm --defined-only` over the module's objects gives the symbols it defines; symbols also defined by
   the DOL side (`GAME_UNITS`) or by another module get a per-module prefix through
   `llvm-objcopy --redefine-syms=<mod>.map` applied to **all** of that module's objects (so references
   inside the module follow the rename). Imports from the DOL stay untouched.
3. `_prolog`/`_epilog`/`_unresolved` are renamed unconditionally; the module table calls them by their
   prefixed names.
4. Cross-module references (a REL importing another REL's symbol, which the REL format allows) show up as
   remaining undefined symbols at link time and are resolved case by case (most likely `Sscrn` ↔ stage
   modules through the DOL's function-pointer tables, i.e. none).

The map files are generated by `tools/gen_module_table.py` from the compiled objects, not from the
decomp's `config/G4BE08/modules/*/symbols.txt`, so they track what the x86 compiler actually emitted.

## Build layout

```
xbox/Makefile          nxdk: SRCS = port + xbox_compat + $(GAME_SRCS) via build/prep/ ; include $(NXDK_DIR)/Makefile
xbox/Makefile.host     clang -m32, same unit list, SDL2/GL backend, builds re4_host
xbox/units.mk          the shared unit list: game, modules (with per-dir defines), lib subset
xbox/tools/prep.py     the preprocessing step (asm strip, body replacement), run per unit with deps
```

`units.mk` lists units explicitly (no wildcards) so a debug unit cannot sneak in and so the Xbox and host
builds compile the same set. The first milestone is "every unit compiles"; link errors then enumerate the
SDK symbols the port layer still owes, which is the to-do list for Phase 1.
