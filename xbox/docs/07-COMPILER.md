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
