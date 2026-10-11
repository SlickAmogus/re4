# Resident Evil 4 for the original Xbox

A port of Resident Evil 4 (GameCube, 2005) to the original Xbox, built from the complete decompilation of the
GameCube debug build with [nxdk](https://github.com/SlickAmogus/nxdk) and the same toolchain, hardware-audio
path and NV2A rendering techniques as our [VibeDuke3D](https://github.com/SlickAmogus/VibeDuke3D) and
[Starship](https://github.com/SlickAmogus/Starship) Xbox ports.

**Status: scaffolding (Phase 0).** The platform-layer skeleton builds to a `default.xbe` with nxdk
(startup, logging, memory arena, null GX backend, stub game loop); the game units are not compiled yet.
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
make -C xbox -f Makefile.host
```

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
