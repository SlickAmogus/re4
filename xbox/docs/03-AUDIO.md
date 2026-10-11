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

## 5. Timing

`cb_audio_frame` expects to run every 5 ms (160 samples at 32 kHz) and advances envelopes/sequences in
"audio frames". On Xbox we run it from a dedicated thread with a 5 ms wait (`KeDelayExecutionThread`),
guarded by a critical section against the game thread's request queue, or from the game loop at 60 Hz with
3–4 catch-up iterations. Thread is cleaner and avoids tying envelope rate to frame rate.

## 6. Host backend

SDL2 audio callback (48 kHz stereo float) with a software mixer: decode DSP-ADPCM, resample linearly,
apply volume/pan/LPF, sum. Reverb: a simple Schroeder reverb per AUX bus so the host sounds roughly right.
This is enough to validate the driver logic end-to-end without hardware.

## 7. Open points

- `AXSetVoiceLpf` has no per-voice DS equivalent; evaluate how audible it is (used for distance muffling)
  and consider pre-filtered variants or the GP DSP.
- Dolby Pro Logic II mode: the Xbox AC3 encoder makes a real 5.1; `SndSurWork` already computes rear
  contributions, so route them to BL/BR.
- `AXSetCompressor`: the APU has no global compressor; `SetVolume` headroom instead.
