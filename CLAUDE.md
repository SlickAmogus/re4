# RE4 (GameCube decomp) → original Xbox port

This fork of [adonis-singh/re4](https://github.com/adonis-singh/re4) (the 100% matched decompilation of the
Resident Evil 4 GameCube **debug** build, G4BE08 "Nov 25 2004") carries an original-Xbox port built with
[nxdk](https://github.com/SlickAmogus/nxdk) (our fork with RXDK hardware-audio support). `xbox` is the
default branch and carries all port work under `xbox/`; `master` tracks the upstream decomp and is never
edited (merge it into `xbox` to pick up decomp fixes).

Start with `xbox/docs/00-PORTING-PLAN.md`, then the design doc for the subsystem you are touching.

## Rules for Claude Code sessions

1. **Graphify before grep.** This repo is meant to be indexed in the Graphify MCP workspace. For any
   code-navigation question (where is X defined, who calls X, what does X call, which files depend on Y, how
   does subsystem Z work) call `graphify_find`, `graphify_callers`, `graphify_callees`,
   `graphify_file_neighbors`, `graphify_impact` or `query_graph` **first**. Use `Grep`/`rg` only when Graphify
   reports the repo is not indexed, returns nothing for a symbol that must exist, or the question is about
   literal text (string constants, magic numbers, comments). Say which one you used when it matters.
   If `list_repositories` does not show `SlickAmogus/re4`, tell the user to add it in the Graphify web app;
   do not silently fall back for the whole session. Cloud sessions (Claude Code on the web) currently have
   no Graphify server at all: say so once at the start and use `Grep` for the session.
2. **Never edit `master`.** Port work goes on `xbox` (or a branch off it). Keep `src/` and `include/`
   mergeable with upstream: prefer new files under `xbox/`, force-included headers
   (`xbox/port/include/re4port/re4_xbox.h`) and macro overrides over editing game code. When a game file
   must change, make the change the smallest possible, guard it with `#ifdef RE4_PORT`, and leave a
   `// XBOX:` comment on the line. The decomp's `// COMPILER-DIFF:` tags, `#line` directives and
   `register ... asm("rN")` pins are load-bearing for upstream; strip them through macros at build time, not
   by editing.
3. **No game assets in the repo.** Disc data stays outside the tree (locally `C:\Claude\RE4-Xbox\gamedata\`,
   extracted from the retail RVZ images with decomp-toolkit, see `xbox/docs/04-DATA.md`). Tools that read
   it take a path argument. The user's discs are the **retail USA** release, not the debug discs the decomp
   builds; see `xbox/docs/01-ANALYSIS.md` for what that means.
4. **Debug tools are out of scope.** `src/Tools/`, `src/tools/`, `src/t_*/` (the in-game editors) and the SN
   debugger runtime in `src/lib` are not compiled into the port. On Windows checkouts `src/tools/` is
   excluded with sparse-checkout because it collides with `src/Tools/` on a case-insensitive filesystem.
5. **Cloud tasks.** Work that can be verified on a PC (no Xbox needed) is written up as
   `xbox/cloud-tasks/CT-NN-<name>.md` with explicit acceptance criteria, so it can be run from Claude Code on
   the web. Keep them self-contained: a cloud session only has this repo and no game data unless the task
   says how to get a sample.
6. **Reference ports.** When in doubt about an nxdk/Xbox mechanism, look at how the two finished ports did it
   before inventing anything: VibeDuke3D (`C:\Claude\jfduke3d-xbox`, C, RXDK DirectSound bridge, POLYMOST→NV2A)
   and Starship (`C:\Claude\starfox\Starship\xbox`, C++ on nxdk, libultraship Fast3D→NV2A backend with
   pbkit). Their lessons are summarised in `xbox/docs/06-LESSONS-FROM-PORTS.md`.
7. **Commit style.** One-line factual subject (`xbox: gx shim: implement GXSetBlendMode on NV2A`), body only
   when the why is not obvious. Do not run the formatter over decomp sources.

## Layout (branch `xbox`)

```
src/, include/, config/, tools/, docs/   upstream decomp, untouched
xbox/
  README.md               status, build, run
  docs/                   00-PORTING-PLAN, 01-ANALYSIS, 02-GRAPHICS, 03-AUDIO, 04-DATA, 05-SYSTEM,
                          06-LESSONS-FROM-PORTS, 07-COMPILER
  cloud-tasks/            self-contained tasks for Claude Code on the web
  port/                   the platform layer (reimplemented Nintendo SDK surface the game calls)
    include/re4port/      re4_xbox.h (force-included), gx_shim.h, os_shim.h, ...
    gx/                   GX state machine, display-list interpreter, TEV → NV2A, backends
    os/ dvd/ pad/ card/ snd/ mtx/   OS threads/alarms, DVD+ARAM, PAD, CARD, AX/MIX/AXFX, matrix math
    host/                 SDL2/OpenGL host backend (Linux/Windows PC) used for development and CI
  xbox_compat/            nxdk shims shared with the other ports (posix_io, startup, RXDK dsound bridge)
  tools/                  disc extraction, file-table checks, asset converters (Python)
  Makefile, build_xbox.sh nxdk build; Makefile.host for the PC build
```

## Building

- Xbox: `bash xbox/build_xbox.sh` (needs `C:\Claude\nxdk` or `NXDK_DIR`; MSYS2 make). Output
  `xbox/bin/default.xbe`.
- Host: `make -C xbox -f Makefile.host` (Linux, gcc-multilib for `-m32`, SDL2). See `07-COMPILER.md` for why
  the host build is 32-bit. Cloud sessions get `gcc-multilib`/`g++-multilib`, `jsonschema` and `pytest`
  from the SessionStart hook in `.claude/hooks/session-start.sh`.
- Port unit tests: `make -C xbox/port/tests && ./xbox/port/tests/<name>` (32-bit, no game data).

Neither build is functional yet; `xbox/README.md` tracks the current milestone.
