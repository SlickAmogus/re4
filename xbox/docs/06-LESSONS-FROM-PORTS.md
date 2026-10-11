# 06 — Lessons from the VibeDuke3D and Starship Xbox ports

What the two finished nxdk ports taught us, and what we reuse verbatim.

## VibeDuke3D (`C:\Claude\jfduke3d-xbox`, C, JFDuke3D on nxdk)

**Build system**
- `Makefile.nxdk` includes `$(NXDK_DIR)/Makefile` last. Include paths for our code go in `NXDK_CFLAGS`
  (so they come before nxdk's own, e.g. zlib's `crc32.h`), project-only flags (`-include xbox_defs.h`,
  `-Ixbox_compat`) are applied with a target-specific `CFLAGS +=` on our object list only, so they do not
  leak into nxdk's pdclib/xboxrt builds.
- MSYS2 `make` loses `NXDK_DIR` in subprocesses: `build_xbox.sh` exports it, adds explicit
  `-I$(NXDK_DIR)/lib/...` paths, and our nxdk fork's `nxdk-cc` auto-detects `NXDK_DIR` from its own path.
- `unset INCLUDE LIB LIBPATH`: MSVC's vcvars pollutes clang's MSVC-compat mode with PC SDK headers.
- `/safeseh:no` for the RXDK objects; lld 18 crashes on old MSVC `.debug$F` sections, so the RXDK `.obj`
  files are `llvm-objcopy`-stripped copies.
- Cg shaders (`polymost_vs.vp20`, `polymost_ps.ps.cg`) are compiled by nxdk's `vp20compiler`/`fp20compiler`
  into `.inl` headers at build time. **RE4 reuse:** TEV configurations can be expressed as fp20 register
  combiner programs instead of hand-packed `NV097_SET_COMBINER_*` words.

**Hardware audio (the RXDK path)**
- `xbox_compat/dsound_objs/*.obj` + `rxdk_include/dsound.h` are the real XDK DirectSound library. It is
  MSVC `__stdcall`; `dsound_bridge.c` provides `__stdcall` wrappers (`_CreateEventA@16` etc.) that call
  kernel APIs directly, thread trampolines, and MSVC runtime stubs (`__ftol2`, …) in `msvc_compat.c`.
- `xbox_startup.c` runs as a constructor: manual AC97 cold reset **without** `XAudioInit` (it would grab
  IRQ 6 which RXDK's AC97 driver needs), S/PDIF enable through PCI config 0x4C bit 24, `atexit` DMA stop.
- `driver_dsound_xbox.c`: `DirectSoundCreate`, three looping buffers with explicit `DSMIXBINS` (front,
  center/LFE, back) for 5.1 through the AC3 encoder, `DirectSoundDoWork()` pumped from the game loop,
  volume boost (+12 dB) because the AC3 path is quiet, stereo→surround upmix, LFE feed. **RE4 reuse:** the
  bridge, the startup, the mixbin setup; RE4 adds per-voice hardware buffers instead of one mixed stream.

**I/O and logging**
- pdclib `fopen` failed silently; `posix_io.c` wraps `NtCreateFile`/`NtReadFile` as `_open/_read/_write`.
  Logging uses `_write` so it survives a crash. Writable directory is probed at boot (`D:\` when launched
  from HDD, `E:\<game>\` when launched from disc).
- `xbox_grplist.c`: directory listing through `FindFirstFile`-style NT calls.

**Rendering**
- POLYMOST → a minimal GL shim (`glbuild_xbox.c`) → pbkit. Texture swizzle (`nv2a_swizzle_addr`, Morton
  order) was written here and copied into Starship. 720p auto-detect from the dashboard video settings.
- Precaching textures at level load removed hitches; the same will matter for RE4's per-room texture sets.

## Starship (`C:\Claude\starfox\Starship\xbox`, C++ on nxdk, libultraship Fast3D → NV2A)

**C++ on nxdk**
- `NXDK_CXX = y`, `-std=c++20 -fpermissive -fexceptions`, `LDFLAGS += -stack:524288` (the default 64 KB
  stack is far too small for deep C++ call chains; RE4's tasks have their own stacks but the main thread
  will need this too).
- `-include xbox_compat.h` force-included everywhere: nxdk's clang target is `i386-pc-win32`, so `_WIN32`
  and `_MSC_VER` are defined and Windows code paths light up; the header gates them with `NXDK`/`PLATFORM_XBOX`
  instead of `#undef _WIN32` (which breaks libc++). `-iquote xbox_src/stubs` first so stub `spdlog.h`,
  `imgui.h` win over the real ones. No `std::filesystem` in nxdk's libc++.
- `#undef main` after `SDL.h` to keep the real `main()` (nxdk's crt0 calls `main`).
- `malloc` can return memory adjacent to `this`; a `memset` of a freshly allocated pool clobbered a member
  that was assigned too early. Assign after initialising.

**NV2A backend (`gfx_nv2a.cpp`, 1,442 lines)** — the template for `xbox/port/gx/gx_nv2a.cpp`
- Init: `pb_init`, `pb_show_front_screen`, `pb_back_buffer_width/height`; `NV097_SET_SPECULAR_ENABLE = 1`
  is **required** or the final combiner is bypassed and the GPU can halt; `SKIN_MODE_OFF`,
  `LIGHTING_ENABLE 0`, clip min/max, clear rect, depth/alpha/blend defaults, colour mask, stencil off,
  fog off (fog done in combiners), 4 texture stages disabled, `while (pb_busy())`.
- Per frame: `pb_reset`, `pb_target_back_buffer`, wait `pb_busy`, re-assert specular + transform mode
  (`PROGRAM`, `RANGE_MODE_PRIV`, program start 0), reset the state cache; end: `pb_busy` wait with a
  500 ms timeout, `pb_finished` to flip (vsynced).
- A passthrough vertex program (`vs_passthrough.vp`) compiled by `vp20compiler`; constants loaded through
  `NV097_SET_TRANSFORM_CONSTANT`. For RE4 the vertex program must do the GX transform: position matrix
  from the matrix palette index, normal matrix, texgen matrices, lighting — or we pre-transform on the CPU.
- Register combiners configured per unique colour-combiner mode (`ConfigureRegisterCombiners`): one general
  stage with `ICW/OCW` words, final combiner `SPECULAR_FOG_CW0/1`. RE4 needs up to 8 general stages.
- Vertex data streamed into a `MmAllocateContiguousMemoryEx(..., PAGE_READWRITE | PAGE_WRITECOMBINE)`
  pool below `MAXRAM 0x03FFAFFF`; batching draws with the same state (`FlushBatch`) plus a state cache
  took the port from ~20 to 60 fps.
- Textures: POT padding, Morton swizzle, `A8R8G8B8` with RGBA→BGRA conversion, 1024 max. RE4 should
  convert to swizzled/DXT formats offline instead.
- `xbox_log` to `D:\starship.log` through `fopen` worked here (nxdk fixed since Duke); keep the `_write`
  path as a fallback.

**Audio**: `XboxAudioPlayer.cpp` over the same RXDK objects; an "audio death spiral" (callbacks starving
the game loop) was fixed by bounding how much work the pump does per frame.

**Process**: `PORTING_NOTES.md` listed the phases and ticked them; the first milestone was a skeleton that
compiled with every backend method stubbed, which is what `xbox/port/` starts as here.

## What we do differently for RE4

- Two backends from the start (host OpenGL + NV2A) behind one `gx` interface, because RE4's renderer is far
  larger than Star Fox's and must be debugged on a PC.
- Assets converted offline (Starship's `.o2r` idea) rather than swapped at load, because the game relocates
  file offsets in place and expects the exact byte layout.
- Per-voice hardware audio (RE4 has up to 64 AX voices with pitch, pan, LPF) instead of one mixed PCM
  stream, which is what the XDK DirectSound library is for.
