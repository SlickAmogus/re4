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

int re4_main(void);   // the game's main(), C++ linkage (renamed by re4_xbox.h)
extern "C" int re4_mem_init(void);
extern "C" void re4_modules_init(void);
extern "C" void xbox_log_open_file(void);
extern "C" int xbox_logging_enabled;
extern "C" const char* xbox_get_writedir(void);

#define MAXRAM 0x03FFAFFF

// Converted assets live next to the XBE: D:\data\files\... (D: is the XBE's own directory under nxdk)
extern "C" const char* re4_data_dir;
const char* re4_data_dir = "D:\\data";

// Below 48 MB physical so the virtual address (0x80000000 + physical) passes the game's MEM1 range
// checks (re4_xbox.h RE4_VALID_PTR).
#define RE4_ARENA_MAXPHYS 0x02FFFFFF

extern "C" void* re4_xbox_alloc_contiguous(unsigned size)
{
    // Cached: the game's heaps live here and the CPU reads them constantly (write-combined memory is
    // uncached for reads). The GX backend copies textures/vertices into its own GPU memory.
    void* p = MmAllocateContiguousMemoryEx(size, 0, RE4_ARENA_MAXPHYS, 0, PAGE_READWRITE);
    if (p && ((unsigned) p < 0x80000000u || (unsigned) p + size - 1 > 0x82FFFFFFu))
        re4_log("arena %p outside 0x80000000..0x82FFFFFF: relocation checks will misfire\n", p);
    return p;
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

    // NV2A backend (CT-15); init must run before selection (selection sends the initial viewport).
    if (gx_nv2a_backend.init(640, 448) == 0) gx_select_backend(&gx_nv2a_backend);
    else gx_select_backend(&gx_null_backend);

    re4_modules_init();   // snapshot every REL module's initial .data before the game runs (CT-12)
    int rc = re4_main();
    re4_log("re4_main returned %d\n", rc);
    Sleep(3000);
    return rc;
}

#endif // NXDK
