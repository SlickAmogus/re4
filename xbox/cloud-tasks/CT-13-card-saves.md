# CT-13 — CARD (memory card) over save files

**Status:** open · cloud-friendly

## Goal

Implement the 17 CARD functions in `xbox/docs/sdk-symbols.md` in `xbox/port/card/card_shim.cpp` so the
game's own `src/game/card.cpp` state machine saves and loads unchanged (`xbox/docs/04-DATA.md` section 6,
`05-SYSTEM.md` section 6).

## Facts

- Prototypes: `include/card.h` (the game's own declarations, `CardFileInfo`, `CardStat`, callbacks
  `void (*)(s32 chan, s32 result)`). Result codes: `include/dolphin/card.h` (`CARD_RESULT_READY 0`,
  `BUSY -1`, `NOCARD -3`, `NOFILE -4`, `EXIST -7`, `NOENT -8`, `INSSPACE -9`, ...).
- The game polls `CARDGetResultCode(chan)` after every `*Async` call, so: perform the operation
  synchronously, set the channel's result, and invoke the callback (if any) from the port's poll point
  (`re4_dvd_poll` in `port/dvd/dvd_shim.cpp`, or a new `re4_card_poll` called from `VIWaitForRetrace` in
  `port/os/vi_pad.cpp`). Return `CARD_RESULT_BUSY` from `CARDGetResultCode` until then if the game expects
  to see BUSY (read `card.cpp` around the call sites listed in `01-ANALYSIS.md`).
- Slot A = present, 59 blocks (8 KB each, 0x2000), sector size 0x2000; slot B = `CARD_RESULT_NOCARD`.
- Storage: one directory per slot; each CARD file = one host file named after the CARD file name
  (`save_<x>.dat` patterns, see `card.cpp:172`), plus a side file holding its `CardStat` (icons, banner,
  comment offsets, time). Xbox: `E:\UDATA\4d530004\slotA\` (title id TBD) or next to the XBE when the
  probe in `xbox_compat/xbox_startup.c` finds a writable `D:\`; host: `<data>/../saves/slotA/`.
- `CARDGetSerialNo` returns a fixed 64-bit value per slot (the game ties saves to the card serial).
- `CARDCheckAsync`/`CARDMountAsync`/`CARDUnmount`/`CARDFormatAsync`: bookkeeping only.
- Use `_open/_read/_write/_close` (xbox_compat/io.h) on Xbox and POSIX on the host, as `dvd_shim.cpp` does.

## Tests

`xbox/port/tests/card/`: create → write 0x2000 × N blocks → close → open → read back → status set/get →
delete → `NOFILE`; free blocks accounting; `INSSPACE` when the slot is full. Host clang, 32-bit.

## Acceptance

Tests pass; the Xbox build drops the CARD group from `sdk-symbols.md` (`py -3 xbox/tools/gen_sdk_symbols.py`
after a local build — note in the PR that the local build must be re-run).
