# CT-10 — Host audio backend over the AX shim

**Status:** done (2026-10-11, local run; extended to the Xbox DirectSound sink) · depends on CT-06

Result: `xbox/port/include/re4port/ax_shim.h`, `xbox/port/snd/ax_shim.cpp`, `xbox/port/snd/snd_soft.cpp`
(software mixer; sinks DirectSound / SDL / null / capture), tests `xbox/port/tests/ax/`; the 5 ms frame is driven by
`re4_audio_pump()` from `VIWaitForRetrace` instead of a thread; SYN/SEQ are silent stubs. See 03-AUDIO.md sections 6-8
and `xbox/cloud-tasks/progress/CT-10.md`.

## Goal

The AX/MIX/AXFX/AXART surface the game calls (`03-AUDIO.md` section 1; exact list in `01-ANALYSIS.md`)
implemented once as a backend-neutral voice model (`xbox/port/snd/ax_shim.cpp`), with a host backend
that mixes in software through SDL2 so the sound driver can be exercised on a PC. The Xbox backend
(DirectSound) is written locally later against the same voice model.

## Deliverables

1. `ax_shim.h/.cpp`: `AXVPB`-compatible voice structs as the game uses them (`include/dolphin/ax.h`
   layouts matter only where the game pokes fields directly — audit `snd_iss3/4.cpp`, `snd_str*.cpp`,
   `snd_seq*.cpp`), `AXInitEx`, `AXQuit`, `AXAcquireVoice`, `AXFreeVoice`, `AXSetVoice*` (Type, Addr,
   Adpcm, AdpcmLoop, LoopAddr, EndAddr, Lpf, LpfCoefs, Src, SrcType, SrcRatio, State, Priority),
   `AXRegisterCallback`, `AXRegisterAuxA/BCallback`, `AXSetMode`, `AXSetCompressor`, `AXGetAuxA/B`;
   `MIXInit/Quit/InitChannel/ReleaseChannel/SetInput/Pan/SPan/AuxA/AuxB/UpdateSettings/SetSoundMode`;
   `AXFXReverbHi/Std`, `AXFXChorus`, `AXFXDelay` `Init/Settings/Shutdown` (store parameters, run a simple
   host reverb), `AXFXSetHooks`; `AXARTInit/Quit/ServiceSounds` (the AXART articulation helpers the game
   uses: check `snd_*` for which).
2. Backend vtable: `voice_start/stop/update(params)`, `set_aux(bus, params)`, `pump()`; host backend
   `snd_host_sdl.cpp` with the CT-06 decoder, linear resampling, LPF, pan/span to stereo (optionally 5.1
   downmixed), Schroeder reverb per AUX bus.
3. The 5 ms audio frame: a thread (host: `SDL_CreateThread`) calling the registered callback and
   `pump()`.
4. Tests: a scripted sequence of AX calls (acquire, set ADPCM from a synthetic tone, loop, state 1, ratio
   1.5, stop) renders to a WAV in `xbox/port/tests/out/` and asserts duration/level; aux callbacks fire.

## Acceptance

Tests pass; `xbox/docs/03-AUDIO.md` gets an "Implemented surface" table listing each function as
real / stored-only / no-op.
