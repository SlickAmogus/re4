# 05 — System layer: OS, memory, threads, pad, saves, movies, reset

## 1. Entry and frame loop

GC: `__start` → `main()` (`src/game/main.cpp`): `systemStartInit` (OS, memory, VI/GX, DVD, PAD, CARD,
sound, scheduler), then the frame loop (`Render_before`, `PadRead`, `Render`, `TaskScheduler`, `IdSys`,
`Trans`, `Dvd.Watcher`, `SndWatcher`, fades, messages, `DrawOTag`, `iTaskScheduler`, `Render_done`,
block streaming, `DC.check`) with soft-reset handling (`systemResetCheck`, `RESET_KEEP_WORK` survives).

Xbox: nxdk's crt0 calls our `main()` in `xbox/port/xbox_main.cpp`, which runs the `xbox_startup.c`
constructor logic (AC97 reset, video mode, writable dir), initialises the port layer (`re4port_init`:
memory arena, GX backend, audio, pad, data directory) and then calls the game's `main()` renamed to
`re4_main` by a macro in the force-included header. The host does the same from `host_main.cpp`.

## 2. Memory

The game assumes a fixed 24 MB MEM1 at `0x80000000` with hard-coded region addresses (`SYS_MEM_TBL` in
`main_mem.cpp`, `SND_DATA_TOP 0x80370000`, `pFrame_buff` at `0x80460000`, FIFO at `0x803F0000`) and a
`VALID_PTR` range check. Port:

- One contiguous arena `MEM1_BASE` of 24 MB (`MmAllocateContiguousMemoryEx` on Xbox so the GPU can read
  model/display-list data out of it; `mmap` on the host). Every literal `0x80xxxxxx` in the game is
  rewritten through a macro `GC_ADDR(0x80xxxxxx)` = `MEM1_BASE + (addr - 0x80000000)` (the few sites are
  listed in `01-ANALYSIS.md`; `main_mem.cpp`, `main_sub.cpp`, `snd.cpp`, `datactrl.cpp`, `dvd.cpp`).
  `VALID_PTR` becomes an arena range check. The XFB and FIFO regions are unused on Xbox (the backend has
  its own surfaces) and are left as free arena space.
- `OSInitAlloc`/`OSCreateHeap`/`OSAllocFromHeap`/`OSFreeToHeap` (`src/lib/OSAlloc.c`) are plain C and
  compile as-is on the arena. The 13 logical heaps in `main_mem.cpp` keep working.
- ARAM: a 16 MB block; `ARAlloc`/`ARFree`/`ARQPostRequest`/`ARGetSize` trivially implemented; sound stream
  rings and data units live there as on GC.
- Xbox budget (64 MB): arena 24 + ARAM 16 + code/static ~8 + GPU vertex pool 2 + converted textures
  (read from the arena directly, no copy) + render targets (640×480×4 ×3 + shadow/filter targets ≈ 6)
  ≈ 56 MB. Tight but feasible; the debug-only `Debug_usb_addr` region is dropped and ARAM can be trimmed
  to what `UseAramSize`/`ARAM_END (0xD00000)` actually touch (13 MB).

## 3. Threads and the scheduler

`scheduler.cpp` creates one `OSThread` per task slot with its own stack and runs tasks cooperatively from
the main thread (`OSResumeThread` / `OSSuspendThread` / `OSWakeupThread` / `OSWaitSemaphore`). Interrupt
state (`OSDisableInterrupts`/`OSRestoreInterrupts`, 30 sites) protects queues touched by DVD/ARQ/AX
callbacks.

Port: **fibers** for task threads. `OSCreateThread` allocates a fiber context on the task's stack
(already allocated by the game), `OSResumeThread` switches to it, `OSSuspendThread(self)`/`OSSleepThread`
switch back to the scheduler fiber. A 40-line x86 context switch (`port/os/fiber_x86.S`, works on both
nxdk and Linux `-m32`) avoids kernel threads entirely and preserves the game's exact scheduling order.
Semaphores/thread queues become counters checked by the switch logic. Real OS threads are used only for:
the audio frame callback (5 ms), optional async DVD reads. `OSDisableInterrupts` → a critical section that
those threads also take. `GXSetCurrentGXThread` → no-op.

`OSAlarm` (`main.cpp`): a one-shot timer → checked from the frame loop or a timer thread.
`OSGetTime/OSGetTick` → `KeQueryPerformanceCounter` scaled to the GC bus clock (`OS_BUS_CLOCK`,
`OSTicksToSeconds`), so frame-time code keeps its units.

## 4. Video timing

`VIWaitForRetrace`, `VIFlush`, `VISetNextFrameBuffer`, `VISetBlack`, `VIConfigure` → the backend's
present/vsync. The game targets 30 fps on GC (`vsync_cnt`, two retraces per frame); keep the same pacing
first, then evaluate 60.

## 5. Pad

`PADRead` fills `PADStatus[4]` (buttons u16, sticks s8, triggers u8, err). Mapping proposal (Xbox Duke
controller, SDL2 GameController in nxdk):

| GC                | Xbox                                   |
|-------------------|----------------------------------------|
| A                 | A                                      |
| B                 | B                                      |
| X                 | X                                      |
| Y                 | Y                                      |
| Z                 | Black (or right stick click)           |
| L (analog)        | Left trigger (analog)                  |
| R (analog)        | Right trigger (analog)                 |
| Start             | Start                                  |
| D-pad             | D-pad                                  |
| Main stick        | Left stick                             |
| C-stick           | Right stick                            |
| (none)            | White → map menu, Back → knife/quick   |

RE4 uses R for aim, A for action, B for run/cancel, X/Y for the menu, Z for the map, C-stick for the
camera — all have natural Xbox equivalents. `PADControlMotor` → SDL rumble (the Xbox pad has two motors,
the GC one; drive both). `PADClamp`/`PADSetAnalogMode` → no-ops, `PADRecalibrate` → no-op.

## 6. Saves (CARD)

See `04-DATA.md` section 6. The async CARD API is implemented synchronously with the callback invoked on
the next frame so `card.cpp`'s state machine (which polls `CARDGetResultCode`) is unchanged. `CARDProbeEx`
reports one card in slot A always present; slot B absent. `CARDFormatAsync`/`Delete` map to directory ops.

## 7. Movies (Sofdec)

Plan A: compile `src/lib` `adx_*`, `sfd_*`, `mpv_*`, `mps_*`, `cri_cvfs.c`, `gcci*.c` for x86. They are
C; the CodeWarrior-specific bits are `#pragma`s, `asm { mr rX, rX }` codeless pins (strip), and real
cache/SPR asm in 11 units (DCFlush/DCInvalidate → no-ops, `mftb` → timer). `gcci` (the GameCube CVFS
device) reads through `DVDRead`; our DVD shim covers it. The frame pool output is YUV planes the game
uploads as I8 textures (`sofdec.cpp`), which the GX shim handles like any texture. CPU cost: MPEG-1
640×448 at 30 fps on a 733 MHz P3 is roughly 40–60% of a core with a decent decoder; the CRI decoder was
tuned for a 485 MHz PPC, so it should fit, but measure early (`CT-07` builds and times it on the host).

Plan B: transcode `.sfd` offline to Bink/Theora-like formats and use pl_mpeg (MPEG-1) plus a tiny ADX
decoder, keeping the game's `cSofdec` interface.

### Movies: measurements (CT-07)

**Decision: Plan A.** The game's own CRI stack runs on x86 unchanged except for build-time preparation,
decodes the retail movies correctly, and costs well under a frame on the host.

Build: the 138 CRI units of `configure.py` `CRI_LIBS` (adx 46, sfd/mwsfd 89, gcci 3) go
through `xbox/tools/prep_cri.py` before `prep.py`. It removes every MWCC `asm {}` block (codeless levers,
cache/SPR ops), replaces the real asm kernels with portable C (`port/include/re4port/cri/*.inc`:
paired-single IDCT `dct_fsri_c.inc`, merge kernels `mpv_umc_c.inc`, BE-SWAR motion compensation
`mpv_mc_c.inc`, Y8/CbCr + RGBA8 converters `cftyp422_c.inc`; `mftb` -> `OSGetTime`), and fixes endianness
(MPEG bit readers byte-swap each 32-bit window word, including `mpvabdec_NintraSkipFirst`; the run/level
`const Uint32` tables read as `Sint16` pairs get their halves swapped; ADX header/block loads go through
`RE4_BE_*`; the SFD header (SFH) is stored little-endian on disc, so its `SWAP16/32` become identity) plus
four calls that relied on an argument left in a register (`skip_func(mpv, n)`, `mwPlyChkSupply(mwply)`,
`ADXB_DecodeHeader(.., len)`, `SFTIM_Init(.., p1)`). All 138 units compile with `nxdk-cc`; all 15 movie
symbols of `sdk-symbols.md` are defined. Unresolved after the CRI objects: the SDK surface (AR/ARQ, AX/MIX,
DC*, DVD*, OS threads/ticks, VIWaitForRetrace), MWCC runtime (`__cvt_fp2unsigned`, `__float_nan`), libc,
and ~20 CRI-internal names referenced only from functions the GameCube linker dead-stripped (closed by
`link_resolve.py` stubs).

Bench: `xbox/port/tests/sfd/build_sfd_bench.sh` (clang -m32 + lld, host Windows) builds the same prepped
units with a virtual-clock SDK (`sfd_bench_sdk.c`: fopen DVD, fake ARAM/AX voices advancing at 32 kHz) and
plays a movie the way `cSofdec` does (`mwPlyInitSfdFx`, `mwPlyGetHdrInf`, `mwPlyCreateSofdec`,
`mwPlyStartFname`, then per 59.94 Hz field `ADXM_ExecMain` + `mwPlyGetCurFrm`/`mwPlyFxCnvFrmY84C44`/
`mwPlyRelCurFrm`).

| `Movie/demo0.sfd` (MPEG-1 512x336 29.97 fps, 1839 frames, ADX stereo 32 kHz, 61.4 s) | result |
|---|---|
| frames decoded / CRI errors / decode or display skips | 1839 / 0 / 0, end status PLAYEND |
| luma vs ffmpeg (every frame) | min 62.3 dB, median 66.7 dB (float IDCT rounding only) |
| ADX PCM vs a reference ADX decoder | left bit-exact, right max error 1 LSB; 61.5 s of PCM |
| `ADXM_ExecMain` (demux + MPEG-1 + ADX), Ryzen 7 5700X, `-O2 -march=native` | 0.78 ms/frame, worst field 1.9 ms |
| same, `-O2 -march=pentium3` (x87 + SSE1, no SSE2 in the objects) | 0.82 ms/frame, worst field 1.9 ms |
| `mwPlyFxCnvFrmY84C44` (texture tiling) | 0.03 ms/frame |
| ffmpeg reference, `-threads 1`, SIMD / plain C | 0.24 / 0.40 ms/frame |

733 MHz Pentium III estimate: the host runs ~6.3x the clock (4.6 GHz) and roughly 2.5-3.5x the work per
clock on this kind of integer/x87 code, so scale by 16-22x: **13-18 ms per movie frame, 40-55% of the
33.4 ms frame budget**, worst field ~30-40 ms (absorbed by the 4-frame pool). That fits: the game does
nothing else while a movie plays but draw one textured quad. Plan B would not be clearly cheaper (pl_mpeg is
plain C like ffmpeg's 0.40 ms path, ~2x better at best) and would need transcoding the discs, so it is
kept only as the fallback if hardware timing disagrees. Headroom if needed: an MMX/SSE1 IDCT and MC in
the `.inc` kernels (the float IDCT on x87 dominates).

Port obligations found on the way:
- `include/mwply.h` `MWS_SFD_HDRINF` reads width/height as `s16` at 0x0A/0x0E, the low halves of CRI's
  `Sint32` fields at 0x08/0x0C on a big-endian CPU; `cSofdec::draw` reads `frm.fno` as `((u16*)&fno)[1]`.
  Both read the wrong half on x86 and need `#ifdef RE4_PORT` fixes.
- `MWS_PLY_CPRM_SFD` is 0x24 bytes in `mwply.h` but CRI reads 0x30 (`buffmt` at 0x24): the game relies on
  its memclr'ed work; keep that zero.
- The ADX PCM that `ax_rna.c` writes to ARAM is native-endian (little-endian on x86) s16, unlike disc
  PCM/ADPCM: the AX shim must not byte-swap CRI voices.
- Missing SDK shims the CRI objects reference: `DVDCancel` (sync), `ARFree`, `MIXSetInput`, `MIXSetPan`,
  `OSJoinThread` (never called: CRI threads are not created in this game).
- If Plan B is ever needed: ADX decode is the standard 2-coefficient predictor (c1/c2 from the header
  cut-off, 18-byte frames of 32 nibbles), but CRI scales by `(scale & 0x1FFF) + 1`; ffmpeg uses `scale`
  and is off by ~43 dB SNR.

## 8. Reset, errors, debug

- `OSResetSystem` → return to dashboard (`HalReturnToFirmware`); the soft-reset loop in `main()` with
  `RESET_KEEP_WORK` is kept for the "reset to title" path.
- `OSSetErrorHandler`/`exception.cpp` → not installed; nxdk's default crash output + our log.
- `OSReport`, `pLog->err/warn`, `eprintf` → `xbox_log` to `<writedir>/re4.log` (Duke's `_write`-based
  logger), on-screen debug text disabled via `pG->debug_mode = 0`.
- `OSPanic`/`HALT()` → log + `Sleep` loop so the message is readable over the debug channel.
