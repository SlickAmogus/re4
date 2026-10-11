// xbox_main.cpp — nxdk entry point for the RE4 port (xbox/docs/05-SYSTEM.md section 1).
// nxdk's crt0 calls main(); xbox_compat/xbox_startup.c has already run as a constructor (AC97 reset,
// video mode, writable-directory probe). The game's own main() is renamed re4_main by re4_xbox.h.
#ifdef NXDK

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <SDL.h>
#undef main   // keep the real main(): nxdk's crt0 calls main, not SDL_main

#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"

extern "C" int re4_main(void);
extern "C" int re4_mem_init(void);
extern "C" void xbox_log_open_file(void);
extern "C" int xbox_logging_enabled;
extern "C" const char* xbox_get_writedir(void);

#define MAXRAM 0x03FFAFFF

extern "C" void* re4_xbox_alloc_contiguous(unsigned size)
{
    return MmAllocateContiguousMemoryEx(size, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

int main(void)
{
    xbox_logging_enabled = 1;
    xbox_log_open_file();
    re4_log("=== RE4 Xbox port boot (writedir %s) ===\n", xbox_get_writedir());

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        re4_log("SDL_Init failed: %s\n", SDL_GetError());
    }
    if (re4_mem_init() != 0) {
        debugPrint("RE4: memory arena allocation failed\n");
        Sleep(5000);
        return 1;
    }

    // Phase 0: no renderer yet; the null backend records calls to the log.
    gx_select_backend(&gx_null_backend);

    int rc = re4_main();
    re4_log("re4_main returned %d\n", rc);
    Sleep(3000);
    return rc;
}

#endif // NXDK
