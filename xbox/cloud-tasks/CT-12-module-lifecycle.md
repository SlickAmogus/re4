# CT-12 — REL module lifecycle: fresh .data/.bss on every load

**Status:** open · cloud-friendly (design + tooling), runtime check local

## Problem

On the GameCube a stage or enemy REL is read from disc every time it is loaded, so its `.data` starts at
the initial values and its `.bss` at zero. The port links all 105 modules statically
(`xbox/port/os/module_table.cpp`): statics survive from one load to the next, and every module's static
constructors run once at boot. Rooms that rely on zeroed statics (counters, "first time" flags, cached
pointers into a previous room's heap) will misbehave on the second visit. `roomdata.cpp` stop/restart
(`stopRelData`/`restartRelData`) additionally backs up and restores the *GameCube* bss buffer, which is
not where our statics live.

## Design to implement

1. Give each module's data its own contiguous range: in `isolate_modules.py`, rename sections
   `.data` → `.data$re4m_<mod>$m`, `.bss` → `.bss$re4m_<mod>$m` (COFF groups `$`-suffixed sections and
   sorts them by suffix, so each module is contiguous), and add two tiny marker objects per module that
   define `re4m_<mod>_data_begin/_end` in `$a`/`$z` subsections (and the same for bss). ELF (host):
   use `--rename-section` to `.data.re4m_<mod>` plus a linker script fragment with `__start_`/`__stop_`
   style symbols, or `SORT_BY_NAME` with markers.
2. `module_list.inc` gains the four range symbols per module; `module_table.cpp` snapshots each module's
   `.data` once at boot (before anything runs) into a heap copy.
3. `OSLink` for a *fresh* load restores `.data` from the snapshot, zeroes `.bss`, and runs the module's
   static constructors (collect them into a per-module `.CRT$XCU`-like list during isolation, and remove
   them from the global CRT list so they do not run at boot) — then the game calls the prolog.
4. Telling fresh from restart: `cRoomData::restartRelData` links without re-reading. Implement
   `stopRelData`/`restartRelData` semantics in the port by making `OSUnlink` copy the module's live
   `.data/.bss` into a per-module save area and `OSLink` restore it when the same header address is
   relinked without an intervening `DVDRead` of that REL (track the header pointer + a generation counter
   bumped by `DVDReadAsyncPrio` on any `Rel/*.rel` or `.drs` file).
5. Tests: a synthetic two-module image (two tiny C++ files with statics and a ctor, built through the same
   isolate script) demonstrating reset-on-fresh-link and preserve-on-restart.

## Acceptance

Synthetic test passes on the host toolchain; the Xbox build still links; doc section in `07-COMPILER.md`.
