# xbox_compat

nxdk shims shared with [VibeDuke3D](https://github.com/SlickAmogus/VibeDuke3D) (`xbox_compat/` there) and
Starship. Copied, not submoduled, so this port can diverge; keep the file names so fixes can be ported back.

| File | Purpose |
|------|---------|
| `xbox_startup.c` | constructor: AC97 cold reset without `XAudioInit` (keeps IRQ 6 free for RXDK), S/PDIF enable, 640x480 default video mode, writable-directory probe (`D:\` then `E:\<game>\`), `_write`-based crash-safe logger (`xbox_log`, `xbox_log_write`, `xbox_log_open_file`) |
| `posix_io.c`, `io.h`, `fcntl.h`, `sys/` | `_open/_read/_write/_lseek/_close` over `NtCreateFile`; pdclib's `fopen` was unreliable |
| `dsound_bridge.c` | `__stdcall` Win32 wrappers and thread trampolines the MSVC-built XDK DirectSound objects import |
| `msvc_compat.c` | MSVC runtime symbols (`__ftol2`, `_alldiv`, ...) for the same objects |
| `xbox_stubs.c` | misc stubs |
| `xbox_defs.h`, `xtl.h` | force-include/compat headers used by the Duke build (RE4 uses `port/include/re4port/re4_xbox.h` instead; these stay for `dsound_bridge.c`) |
| `rxdk_include/` | `dsound.h`, `dsfxparm.h` and pack headers for the XDK DirectSound API |
| `dsound_objs/` | **not in git** (Microsoft proprietary). Copy the stripped `.obj` files from a local VibeDuke3D tree; the Makefile enables the RXDK audio path only when they are present |
