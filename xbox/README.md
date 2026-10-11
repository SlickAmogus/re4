# Resident Evil 4 for the original Xbox

A port of Resident Evil 4 (GameCube, 2005) to the original Xbox, built from the complete decompilation of the
GameCube debug build with [nxdk](https://github.com/SlickAmogus/nxdk) and the same toolchain, hardware-audio
path and NV2A rendering techniques as our [VibeDuke3D](https://github.com/SlickAmogus/VibeDuke3D) and
[Starship](https://github.com/SlickAmogus/Starship) Xbox ports.

**Status: Phase 1, the whole game compiles and links.** `make RE4_GAME=1` builds every non-tool game
unit and all 105 gameplay REL modules with nxdk's clang into `bin/default.xbe`. Implemented so far: the GX
state machine and display-list interpreter (CT-08), the TEV catalogue (CT-05), C replacements for the
PowerPC assembly (CT-02), the DSP-ADPCM / Xbox ADPCM codecs (CT-06), the file-resolution table (CT-01).
The remaining SDK surface (214 symbols, `docs/sdk-symbols.md`) is bound to logging stubs, so the XBE
starts but does not run the game yet. Asset conversion (CT-04) is in progress.
The research, the architecture and the task breakdown are in `docs/`; PC-verifiable work is written up in
`cloud-tasks/`.

## How it works

The decomp gives us the game as ~570,000 lines of C and C++ that call the Nintendo GameCube SDK (GX graphics,
AX audio, DVD, OS, PAD, CARD) and a few hundred lines of PowerPC assembly. The port keeps the game code and
replaces the SDK underneath it:

| GameCube                           | Xbox                                                     |
|------------------------------------|----------------------------------------------------------|
| GX (TEV, display lists, EFB copies)| NV2A through pbkit: register combiners, vertex programs  |
| AX / MIX / AXFX (DSP mixer)        | RXDK DirectSound: hardware voices, mixbins, I3DL2 reverb |
| DVD + ARAM streaming               | HDD files + a plain RAM block                            |
| OS threads, alarms, cache ops      | fibers / Win32 threads, no-ops                           |
| PAD                                | SDL2 game controller (nxdk)                              |
| CARD (memory card)                 | save files on the HDD                                    |
| CRI Sofdec movies (MPEG-1 + ADX)   | the same CRI C code, compiled for x86                    |
| REL overlays                       | statically linked module table                           |
| big-endian data                    | assets converted to little-endian offline                |

See `docs/00-PORTING-PLAN.md` for the phases and `docs/01-ANALYSIS.md` for the measurements behind the plan.

## Building

Xbox (Windows, MSYS2 make, nxdk fork at `C:\Claude\nxdk` or `$NXDK_DIR`):

```sh
bash xbox/build_xbox.sh
```

Host PC build (Linux, used for development, tests and cloud tasks):

```sh
sudo apt-get install gcc-multilib g++-multilib          # 32-bit toolchain; plus libsdl2-dev:i386 libgl-dev:i386 for RE4_GL=1
make -C xbox -f Makefile.host                           # port layer + stub game -> xbox/build/host/re4_host
make -C xbox/port/tests && ./xbox/port/tests/gx         # port unit tests (no game data needed)
```

Claude Code cloud sessions install the 32-bit toolchain automatically through `.claude/hooks/session-start.sh`.

## Running on an Xbox (boot test)

```sh
bash xbox/build_xbox.sh RE4_GAME=1                         # bin/default.xbe
cd xbox/tools && py -3 -m re4conv convert <disc1/files> <disc2/files> <converted>   # ~40 min, once
bash xbox/package_xbox.sh <converted> <dist/RE4>           # default.xbe + data/files
```

FTP the folder to the console, launch `default.xbe`, then read `re4.log` from the same folder. At this
stage the log is the deliverable: it shows how far boot gets and which unimplemented SDK functions are
hit first (`UNIMPLEMENTED function called from ...`; map the address to a symbol with the linker map).
Nothing is drawn yet and there is no sound.

## Game data

You need your own GameCube discs. This port targets the **retail USA release** (two discs). Extract them with
decomp-toolkit (`dtk vfs cp "disc1.rvz:files" out/disc1/files`) and run the asset converter described in
`docs/04-DATA.md`. No game data is ever committed to this repository.

## Directory layout

```
docs/          design documents (read 00 first)
cloud-tasks/   self-contained work items for Claude Code on the web
port/          platform layer: GX, OS, DVD, PAD, CARD, AX reimplementations + host backend
xbox_compat/   nxdk shims shared with the other Xbox ports
tools/         Python tools (disc extraction, file-table checks, converters)
```
