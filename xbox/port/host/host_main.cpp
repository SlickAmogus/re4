// host_main.cpp — PC (Linux/Windows, 32-bit) entry point for development and cloud tests.
// Usage: re4_host [--data <converted asset dir>] [--frames N] [--headless]
#ifndef NXDK

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"

int re4_main(void);   // the game's main(), C++ linkage (renamed by re4_xbox.h)
extern "C" int re4_mem_init(void);

#undef main   // re4_xbox.h renames the game's main(); this is the real process entry point

const char* re4_data_dir = "data";
int re4_max_frames = -1;
int re4_headless = 0;

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) re4_data_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) re4_max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) re4_headless = 1;
        else { fprintf(stderr, "usage: %s [--data DIR] [--frames N] [--headless]\n", argv[0]); return 2; }
    }
    re4_log("=== RE4 host build (data %s) ===\n", re4_data_dir);
    if (re4_mem_init() != 0) return 1;
    gx_select_backend(&gx_null_backend);   // CT-09 adds gx_host_gl_backend
    return re4_main();
}

#endif
