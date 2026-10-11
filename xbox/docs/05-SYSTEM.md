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

## 8. Reset, errors, debug

- `OSResetSystem` → return to dashboard (`HalReturnToFirmware`); the soft-reset loop in `main()` with
  `RESET_KEEP_WORK` is kept for the "reset to title" path.
- `OSSetErrorHandler`/`exception.cpp` → not installed; nxdk's default crash output + our log.
- `OSReport`, `pLog->err/warn`, `eprintf` → `xbox_log` to `<writedir>/re4.log` (Duke's `_write`-based
  logger), on-screen debug text disabled via `pG->debug_mode = 0`.
- `OSPanic`/`HALT()` → log + `Sleep` loop so the message is readable over the debug channel.
