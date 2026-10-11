// stub_game.cpp — stands in for the game's main() until the game units are compiled (RE4_GAME=1).
// Lets the toolchain, startup, logging and the null backend be verified on hardware today.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"

int re4_main(void)
{
    re4_log("stub_game: re4_main() — game units not compiled in this build (RE4_GAME=0)\n");
    if (gx_backend) {
        gx_backend->init(640, 480);
        for (int frame = 0; frame < 3; frame++) {
            gx_backend->begin_frame();
            GxVertex v[3] = {};
            gx_backend->draw(GX_PRIM_TRIANGLES, v, 3);
            gx_backend->end_frame();
        }
    }
    unsigned char* p = (unsigned char*) GC_ADDR(0x80460000);
    p[0] = 0x55;   // touch the arena where the GC had its XFB
    re4_log("stub_game: arena probe ok (0x80460000 -> %p)\n", (void*) p);
    return 0;
}
