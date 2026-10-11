# Cloud tasks

Self-contained work items for Claude Code on the web (or any session without an Xbox). Each task says what
to read, what to produce, and how to verify it on a PC. Pick one, work on branch `xbox` (or a branch off
it named `xbox-ct-NN`), open a PR against `xbox` when the acceptance criteria pass.

Before starting any task: read `/CLAUDE.md` and `xbox/docs/00-PORTING-PLAN.md`. Use the Graphify MCP tools
for code navigation when the repo is indexed (rule 1 in `CLAUDE.md`); cloud sessions have no Graphify and
use grep.

Cloud environment: Ubuntu with clang/gcc; `.claude/hooks/session-start.sh` installs `gcc-multilib`,
`g++-multilib` (for `-m32`), `jsonschema` and `pytest` at session start, so `make -C xbox -f Makefile.host`
and `make -C xbox/port/tests` work out of the box. There is no nxdk in the cloud: Xbox builds are local.

Game data: cloud sessions have none. Tasks that need samples say which small files to synthesise or ask
the user to attach (a single `files/em/em10.drs` is 10 MB; `files/etc/core.das` is 2.4 MB; one room
`St1/r100.das` is a few MB). Never commit game data.

| ID    | Task                                              | Needs data | Depends on |
|-------|---------------------------------------------------|------------|------------|
| CT-01 | File table vs retail disc audit                   | file list only (included) | — |
| CT-02 | C replacements for the PowerPC asm                | no         | —          |
| CT-03 | Static module table + build preprocessor + host build bring-up | no | CT-02 |
| CT-04 | Asset converter (re4conv) with validators          | samples    | —          |
| CT-05 | TEV / GX usage catalogue                          | no         | —          |
| CT-06 | DSP-ADPCM decoder + Xbox ADPCM encoder            | no         | —          |
| CT-07 | CRI Sofdec on x86: compile, stub asm, time it     | one .sfd   | CT-03      |
| CT-08 | GX display-list interpreter + GX state machine    | one .bin   | CT-05      |
| CT-09 | Host OpenGL backend and TEV → GLSL compiler       | one room   | CT-08      |
| CT-10 | Host audio backend (SDL) over the AX shim         | one bank   | CT-06      |

Status is tracked in each task file under "Status".
