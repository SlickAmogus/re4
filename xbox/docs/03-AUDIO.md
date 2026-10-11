# 03 — Audio: AX on Xbox DirectSound

## 1. What the game does

Capcom's sound driver (`src/game/snd*.cpp`, C-style, ~15k lines) sits on the Nintendo AX/MIX/AXFX/AXART
libraries:

- **Init** (`snd_main.cpp`): `AXInitEx(1)`, `MIXInit`, `AXARTInit`, `AXRegisterCallback(cb_audio_frame)`
  (called every audio frame, 5 ms, from the DSP interrupt), `AXSetCompressor(1)`, `AXSetMode`/
  `MIXSetSoundMode` (mono / stereo / Dolby Pro Logic II from the saved option).
- **Sound effects** (`snd_iss*.cpp`): a request (`SND_REQ`: block, number, priority, pan, volume, pitch,
  position, owner) resolves a `SND_SIT` program in a DLS-like wavetable (`WTFILEHEADER`, `WTADPCM`) and
  programs an AX voice: `AXAcquireVoice(30, cb_drop_voice, 0)`, `AXSetVoiceType`, `AXSetVoiceAddr`
  (nibble addresses into **ARAM**), `AXSetVoiceAdpcm` (16 coefficients + predictor/scale),
  `AXSetVoiceAdpcmLoop`, `AXSetVoiceLoopAddr/EndAddr`, `AXSetVoiceLpf` (1-pole low-pass), `AXSetVoiceSrc`/
  `AXSetVoiceSrcType(1)`/`AXSetVoiceSrcRatio` (resampler), `AXSetVoicePriority`, `AXSetVoiceState`;
  `MIXInitChannel(voice, 0, vol, auxA, auxB, pan, span, 0)` then `MIXSetInput/Pan/SPan/AuxA/AuxB`. ADSR
  envelopes and LFO/pitch slides are software-driven per audio frame.
- **Sequences** (`snd_seq*.cpp`): a MIDI-like sequencer (`bgm/bio4midi.hed/.dat`) that plays notes as AX
  voices from the wavetable, same voice path.
- **Streams** (`snd_str*.cpp`): four stream works (`SND_STR`, ring buffers in ARAM at
  `0x700000/0x740000/0x780000/0x7C0000`, 256 KB each) fed by DVD reads of `bgm/bio4str.hed`+`.sbb`
  (BGM, voice-over, ambience), two voices per stereo stream, loop points in nibbles.
- **Effects** (`snd_efx.cpp`): `AXFXReverbHi`, `AXFXReverbStd`, `AXFXChorus`, `AXFXDelay` attached to AUX
  A/B buses; room data selects presets (`REVERB_INFO`); `SndSurWork` handles surround panning.
- **Memory**: sound tables at `0x80370000` (512 KB), per-block SE banks in MRAM/ARAM (`UseAramSize[14]`),
  `ARAM_FREE_BASE`.

Sample format everywhere is **GameCube DSP-ADPCM**: 8-byte frames of 1 header byte (predictor index +
scale) and 14 nibbles = 14 samples, 16 coefficients per sound, 32 kHz typical.

## 2. Xbox hardware

The Xbox APU (MCPX) with the XDK DirectSound library (RXDK objects in `xbox_compat/dsound_objs`):

- 256 hardware 2D voices (64 of them 3D), each with its own sample-rate conversion (`SetFrequency`),
  volume, per-mixbin volumes (`SetMixBinVolumes` over 32 mixbins: FL/FR/C/LFE/BL/BR + 24 effect sends),
  and native **Xbox ADPCM** (IMA-style, 36-byte stereo / 36-byte mono blocks of 64 samples) or PCM16.
- Global DSP (GP) runs effects on the mixbins; the XDK supplies an I3DL2 reverb (`i3dl2.obj`) with room
  presets; EP encodes AC3 for optical 5.1 (`ac97xmo.obj`).
- `DirectSoundDoWork()` must be pumped from the game loop (Duke: once per frame; Starship: bounded work per
  call to avoid the "death spiral").

## 3. Mapping

| AX                                        | Xbox DirectSound                                             |
|-------------------------------------------|--------------------------------------------------------------|
| `AXAcquireVoice` / `AXFreeVoice`          | pool of `IDirectSoundBuffer` hardware voices (64 preallocated) |
| `AXSetVoiceAddr` + ADPCM coefs + loop     | buffer bound to a converted sample (Xbox ADPCM) with `DSBCAPS_LOOPING` / `SetLoopRegion` |
| `AXSetVoiceSrcRatio`                      | `SetFrequency(32000 * ratio)`                                |
| `AXSetVoiceState(1/0)`                    | `Play` / `Stop`                                              |
| `MIXSetInput` (dB attenuation)            | `SetVolume` (hundredths of dB, same unit!)                   |
| `MIXSetPan/SPan`                          | `SetMixBinVolumes` on FL/FR/BL/BR (+C/LFE for dialogue)      |
| `MIXSetAuxA/AuxB`                         | mixbin volumes into the two effect-send bins                 |
| `AXSetVoiceLpf`                           | `SetLFO`? no — a per-voice low-pass is not in DS; approximate with the EP/GP filter or skip |
| `AXFXReverb*`/`Chorus`/`Delay`            | `IDirectSound::SetI3DL2Listener` presets on the send bins; chorus/delay via DSP image if needed |
| `AXRegisterCallback` (5 ms)               | a timer thread or per-frame call of `cb_audio_frame` with elapsed-time catch-up |
| `AXSetMode` / `MIXSetSoundMode`           | speaker config from the dashboard (`DSSPEAKER_*`), AC3 encode when 5.1 |
| Streams in ARAM                           | `IDirectSoundStream` or looping buffers refilled from the existing DVD read logic |

## 4. Sample conversion

Decision: **convert all DSP-ADPCM to Xbox ADPCM offline** in the asset converter, keeping the bank layouts
(`WTFILEHEADER` / `SND_IBLK` tables) so the game's lookup code is untouched; only `pcm_adrs` targets and
the nibble arithmetic change. Reasons: same ~4:1 ratio as DSP-ADPCM (PCM16 would quadruple the ~10 MB of
SE banks), hardware decode, no CPU mixing. The port's AX shim maps nibble addresses to Xbox ADPCM block
addresses with a per-sound table emitted by the converter. Loop points must land on 64-sample block
boundaries; the converter pads loops and logs the few that cannot be aligned exactly.

Streams (`.sbb`) are converted the same way into a second file next to the original so the DVD read offsets
in `bio4str.hed` are rewritten by the converter.

A software DSP-ADPCM decoder (`CT-06`) is still written: the host build uses it to play through SDL, the
converter uses it, and it is the fallback if hardware ADPCM loop handling turns out inexact.

### ADPCM conventions (CT-06)

Code: `xbox/port/snd/dspadpcm.{h,c}`, `xbox/port/snd/xboxadpcm.{h,c}`, `xbox/tools/re4conv/adpcm.py`
(tests: `xbox/port/tests/ct06/`, `xbox/tools/re4conv/tests/test_adpcm.py`).

- **DSP header byte:** low nibble = scale exponent, high nibble = coefficient pair (Dolphin/libogc/
  vgmstream; the CT-06 task text had them swapped). Nibbles high first; 64-bit accumulator.
- **Coefficient order:** AXPBADPCM `a[8][2]` flattened = pair i is `coef[2i], coef[2i+1]`. SND_SHD
  stores pair i as `coef[i], coef[i+8]` (`snd_str3.cpp:108-111`); deinterleave first
  (`adpcm.shd_coefs_to_ax`).
- **Nibble addresses:** byte = nibble/2 (`snd_str0.cpp` `read_end = nibbles / 2`); sample s of a
  frame-aligned buffer is at `s/14*16 + s%14 + 2` (`snd_iss3.cpp:270-284`). AX `endAddress` is
  **inclusive**: the stream ring wrap sets `end = ring + 0x3FFFF` = last nibble (`snd_str2.cpp:277-287`);
  on a wrap the DSP reloads pred_scale/yn1/yn2 from AXPBADPCMLOOP (the stream ring uses
  `{first header byte, 0, 0}`). (CT-10: that holds for type-0 voices; the stream wrap sets AXSetVoiceType(1) first,
  `snd_str2.cpp:292/302`, and a type-1 "stream" voice keeps yn1/yn2 across the wrap, as in Dolphin's
  AX HLE; the soft mixer re-reads the loop frame's header from ARAM.)
- **Unresolved: the iss3 end-address quirk.** `iss_ax_set_para` computes
  `end = cur + n/14*16 + n%14` with `n = length` (one-shot) or `n = loopStart + loopLength` (loop),
  i.e. the address of the sample *one past* the last one, while the SDK's DSPADPCM tool writes
  `ea = nibble(samples - 1)`. With an inclusive end, AX therefore plays one extra (padding) sample at
  the end of every iss3 one-shot and `loopLength + 1` samples per loop iteration (the extra sample
  is the first sample after the loop body). This is Nintendo's libsyn convention copied by Capcom.
  It is not yet known whether the bank data compensates (e.g. `loopLength` stored as length - 1) or
  the one-sample slip is just inaudible; check against real banks when the converter runs (CT-04).
  `dspadpcm_end_nibble()` keeps the SDK `samples - 1` form; `test_dspadpcm.c` models both. The
  converter takes loops as `(start, end_exclusive)` samples, so for iss3 pass
  `(loopStart, loopStart + loopLength)` (i.e. without the extra sample) unless the data shows otherwise.
- **Xbox ADPCM (0x0069):** 36 bytes/channel/block = `{s16 predictor = sample 0 verbatim, u8 step
  index, u8 0}` + 32 bytes (nibbles 0..62 = samples 1..63, nibble 63 padding), low nibble first;
  stereo = both headers, then 4-byte words alternating L/R. Reference bitwise IMA expansion. The
  encoder searches the best step index for the first block (avoids the ramp from index 0); later
  blocks carry the adapted index.
- **Loop padding** (`adpcm.py` docstring): the loop start is rotated forward to the next 64-sample
  boundary (lossless, the loop is periodic); a length that is not a multiple of 64 is unrolled
  `64/gcd(L,64)` times when that stays <= 65536 samples, otherwise resampled to the nearest multiple
  of 64 with a pitch-change warning. Loop region = blocks `[loop_block_start, loop_block_end)`.

## 5. Timing

`cb_audio_frame` expects to run every 5 ms (160 samples at 32 kHz) and advances envelopes/sequences in
"audio frames". On Xbox we run it from a dedicated thread with a 5 ms wait (`KeDelayExecutionThread`),
guarded by a critical section against the game thread's request queue, or from the game loop at 60 Hz with
3–4 catch-up iterations. Thread is cleaner and avoids tying envelope rate to frame rate.

## 6. Software backend (CT-10, implemented)

`xbox/port/snd/ax_shim.cpp` implements the SDK surface once over a backend-neutral voice model
(`xbox/port/include/re4port/ax_shim.h`: `Re4AxVoiceState` per AXVPB + a `Re4AxBackend` vtable);
`xbox/port/snd/snd_soft.cpp` is the first backend, a CPU mixer that works on every platform:

- per voice: DSP-ADPCM from ARAM (`re4_aram_base`, nibble addresses, `dspadpcm.c`), PCM16 (big-endian
  ARAM words) or PCM8; inclusive end, loop with AXPBADPCMLOOP reload for type 0, history kept and
  the loop frame header re-read for type 1 (stream ring, `snd_str2.cpp:292`); one-shots that reach the
  end get `pb.state = 0`; linear SRC with the 16.16 ratio (type NONE = 1:1, max 8x); one-pole LPF;
  `ve` and the nine AXPBMIX gains ramped over the 5 ms frame;
- AUX A/B buses (160 s32 per channel, 4 channels for AUX A in DPL2) through the registered AUX
  callbacks, then added to the main bus; no callback = bus discarded (as on the DSP);
- main L/R/S (Ls/Rs in DPL2) to stereo (in-phase downmix; Dolby Surround matrix in AX surround mode),
  master gain, the `AXSetCompressor` limiter, s16 at 32 kHz or (optional) 48 kHz;
- sinks: **Xbox** (`RE4_AUDIO_RXDK`, XDK DirectSound objects linked) one looping 2D PCM buffer on the
  FL/FR mixbins, written 50 ms ahead of the play cursor, silence guard past the data, underrun resync,
  `DirectSoundDoWork()` every pump; init copied from jfduke3d-xbox `driver_dsound_xbox.c` (APU register
  reset, `g_DirectSoundCriticalSection`, speaker-config globals, `DirectSoundCreate`, no XAudioInit);
  **host** `RE4_GL`: SDL queued audio; otherwise a **null** sink paced by the wall clock; **capture** for
  tests.

**Timing.** `re4_audio_pump()` (call it once per game frame from `VIWaitForRetrace`) runs as many AX
frames as the sink can take (SDL/DS: fill to 10 frames = 50 ms queued; null: elapsed wall time / 5 ms),
at most `RE4AX_MAX_CATCHUP` = 12 per call (a longer stall drops the backlog instead of spiralling).
Each frame: render with the current parameters -> AUX callbacks -> `pb.state`/`pb.addr` mirrors
refreshed -> the `AXRegisterCallback` callback (`cb_audio_frame`). Everything runs on the game's
kernel thread (the OS shim's threads are cooperative fibers), so no locks; the game's
`OSDisableInterrupts` pairs stay no-ops. Decision vs section 5: pumping from the frame loop instead of a
5 ms thread, because the pump is sink-paced (the envelope rate follows the audio clock, not the frame
rate) and needs no synchronisation with the game thread. At 60 Hz each pump runs 3-4 frames.

**AXVPB fields the game reads** (audit of `src/game/snd_*.cpp`): `pb.state` (iss2:145, iss4:72/211/263,
str1:108, str3:278/285), `pb.addr.loopFlag` (iss2:148) and `*(u32*)&pb.addr.currentAddressHi` /
`endAddressHi` (iss2:149-150, str2:164). The layout of `include/dolphin/ax.h` is kept; because a u32
load of a Hi/Lo pair is `(Lo << 16) | Hi` on little-endian, the shim mirrors every address pair as a
host-order u32 stored at the Hi field. Values the game passes in (`AXPBADDR` etc.) are read normally.

### Implemented surface

| Function(s) | Status |
|---|---|
| `AXInit`, `AXInitEx`, `AXQuit` | real (backend open/close) |
| `AXAcquireVoice`, `AXFreeVoice`, `AXSetVoicePriority` | real: 64 voices, steals the lowest priority below the request (oldest on a tie) and calls its drop callback |
| `AXSetVoiceType/Addr/Loop/LoopAddr/EndAddr/CurrentAddr/Adpcm/AdpcmLoop/Src/SrcType/SrcRatio/Lpf/LpfCoefs/State/Mix/Ve/VeDelta` | real (VeDelta approximated as reaching its target at the end of the frame); SRC types 2-4 (4-tap) are mixed linearly |
| `AXGetLpfCoefs` | real (one-pole formula, not the SDK table) |
| `AXSetVoiceItdOn/ItdTarget/UpdateIncrement/UpdateWrite/Dpop/Fir` | no-op (unused by the game) |
| `AXRegisterCallback`, `AXRegisterAuxACallback/BCallback` | real |
| `AXSetMode/GetMode` | real (stereo / surround / DPL2 render paths) |
| `AXSetCompressor` | real (peak limiter in the soft mixer) |
| `AXGetDspCycles`, `AXGetMaxDspCycles`, `AXSetMaxDspCycles`, `AXSetStepMode`, `AXInitProfile`, `AXGetProfile` | stored-only / plausible values (statistics display only) |
| `MIX*` (all of `mix.h`) | real: semantics and tables of the SDK `src/lib/mix.c`; Init applies at once, Set/Adjust at the next `MIXUpdateSettings` with a one-frame ramp |
| `MIXSetDvdStreamFader/Get` | stored-only (no DVD audio streaming in the game) |
| `AXFXReverbHi/ReverbStd/ReverbHiDpl2 Init/Settings/Shutdown/Callback` | real but approximate: parameters stay in the game struct, a Freeverb-style Schroeder reverb (4 combs + 2 all-pass per side; RT60 = `time`, `damping`, `preDelay`, wet = `mix`); `tempDisableFX` passes the bus through like the SDK |
| `AXFXChorus*` | real but approximate: modulated delay from `baseDelay`/`variation`/`period` |
| `AXFXDelay*` | real: per-channel feedback delay (`delay` ms, `feedback` %, `output` %) |
| `AXFXSetHooks` | real (effect memory from the game heap) |
| `AXARTInit/Quit/ServiceSounds`, `AXARTAddSound/RemoveSound` | sound list only; articulators not evaluated (the game never adds a sound) |
| `SYNInit/Quit/RunAudioFrame/InitSynth/QuitSynth/MidiInput/SetMasterVolume/GetMasterVolume/GetActiveNotes/GetMidiController` | stub: **MIDI sequences are silent**. Controller/master-volume state kept; `SYNGetActiveNotes` = 0, which `snd_seq3.cpp:60` takes as "sequence finished" |
| `SEQInit/Quit/RunAudioFrame` (+ `SEQAdd/RemoveSequence`, state/tempo/volume) | stored-only (the game runs its own MIDI sequencer, `snd_seq*.cpp`) |
| `AIInit`, `AIReset` (+ rate/volume/callback state calls) | stored-only (the backend replaces AI DMA) |

Tests: `xbox/port/tests/ax/` (`make -C xbox/port/tests/ax test`, 32-bit): one-shot duration/level/pitch
and the u32 address reads, loop + ratio 1.5 without dropouts, MIX ramp/pan, AUX callback per frame,
AXFX reverb tail and delay, 48 kHz, voice stealing, pump pacing and catch-up cap; WAVs in
`xbox/port/tests/out/ct10_*.wav`.

## 7. Open points

- `AXSetVoiceLpf` has no per-voice DS equivalent; evaluate how audible it is (used for distance muffling)
  and consider pre-filtered variants or the GP DSP.
- Dolby Pro Logic II mode: the Xbox AC3 encoder makes a real 5.1; `SndSurWork` already computes rear
  contributions, so route them to BL/BR.
- `AXSetCompressor`: the APU has no global compressor; `SetVolume` headroom instead.
- **MIDI (SYN).** The upgrade path for audible `bio4midi` sequences is the SDK's own synth in
  `src/lib/syn*.c` (with `seq.c`, `axart*.c`, `axfx` sources next to it). `mix.c`, `seq.c` and
  `axart*.c` compile with nxdk-cc unchanged, `syn*.c` needs the `fake_tgmath.h` guard pre-defined. They
  program voices by writing the AXPB directly and setting `AXVPB::sync` bits, and `synsample.c` /
  `synpitch.c` store Hi/Lo pairs as u32 (`*(u32*)&pb.addr = 0x10000` means loopFlag = 1 on big-endian),
  so the shim would first need a sync-aware path that reads those pb fields with the u32 convention.
- **Output gain on Xbox.** jfduke3d found the APU path (with AC3 encoding) quiet and boosts 4x; the
  DirectSound sink defaults to 2x (`re4snd_soft_set_gain`), to be tuned on hardware.

## 8. Later: per-voice hardware DirectSound backend (not implemented)

The `Re4AxBackend` hooks (`voice_start/stop/update` with dirty bits, `set_aux`, `frames_wanted`,
`run_frame`, `pump_end`) are shaped so a second backend can put AX voices on APU voices instead of the
CPU mixer:

- **Samples.** Converted offline to Xbox ADPCM (section 4); the converter emits a table from each
  sample's ARAM nibble start to its converted block data. `voice_start` looks the AX `cur` address up,
  binds a pooled `IDirectSoundBuffer` (64 preallocated, 2D) to that memory with `SetBufferData`
  (no copy), `SetPlayRegion` / `SetLoopRegion` in 36-byte blocks from the loop/end addresses, then
  `Play(DSBPLAY_LOOPING)` or one-shot.
- **Pitch.** `SetFrequency(ratio * 32000 / 65536)` (the AX ratio already contains sampleRate / 32000).
- **Gains.** `ve` -> `SetVolume` (hundredths of dB, `2000 * log10(g / 32768)`), the AXPBMIX bins ->
  `SetMixBinVolumes` on FL/FR (+BL/BR for S or DPL2 Ls/Rs, C/LFE optional), AUX A/B sends -> two
  `DSMIXBIN_FXSEND_*` bins feeding the GP: the game's effect callbacks are recognised by address
  (`AXFXReverbHiCallback` etc.) and mapped to `SetI3DL2Listener` presets from their `time`/`damping`/
  `preDelay`/`mix`; an unknown AUX callback (or `cb_efx_clear_bass`) means "send muted".
- **LPF.** `SetFilter` (the voice's DSFILTERDESC low-pass) from the one-pole a0/b0, or ignored.
- **End / position.** `frames_wanted` keeps the 5 ms cadence from a timer (no PCM sink); each frame
  `GetStatus` marks finished one-shots `ended`, and `GetCurrentPosition` is converted back to an AX
  nibble address for the `pb.addr` mirror. Streams (`snd_str*.cpp`) poll that address to refill their
  ARAM ring, so they stay on the software mixer (one soft DS buffer = the mix of the stream voices)
  until a stream path with `IDirectSoundStream` packets exists: a hybrid backend routes SE voices to
  hardware and stream voices to `snd_soft`.
- Trade-offs: no CPU mixing for SEs vs. approximate per-voice LPF and AXFX sound (section 7).
