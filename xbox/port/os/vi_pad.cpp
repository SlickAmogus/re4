// vi_pad.cpp — VI (video interface), PAD (controllers) and the GX system calls that are not rendering
// (xbox/docs/05-SYSTEM.md sections 4 and 5).
//
// VIWaitForRetrace is the game's once-per-retrace sync point; the port also uses it as the place where
// deferred "interrupt" work runs: DVD/ARQ completions, OS alarms, the post-retrace callback.
// Presentation itself belongs to the GX backend (GXCopyDisp / end_frame); VI only paces.
#include "re4port/re4_xbox.h"
#include "re4port/gx_shim.h"
#include <string.h>
#if defined(RE4_XBOX)
#include <hal/video.h>
#include <SDL.h>
#define RE4_HAVE_SDL 1
#elif defined(RE4_GL)
#include <SDL.h>
#define RE4_HAVE_SDL 1
#else
#include <time.h>
#endif

extern "C" {

typedef unsigned int u32;
typedef unsigned short u16;
typedef signed char s8;
typedef unsigned char u8;
typedef int BOOL;

void re4_dvd_poll(void);
void re4_os_poll(void);
void re4_card_poll(void);

// ---- VI -----------------------------------------------------------------------------------------
typedef void (*VIRetraceCallback)(u32 retraceCount);
static VIRetraceCallback s_post_cb, s_pre_cb;
static u32 s_retrace;
static int s_black;

void VIInit(void) { }
void VIConfigure(const void*) { }
void VIFlush(void) { }
void VISetBlack(BOOL black) { s_black = black; }
void VISetNextFrameBuffer(void*) { }
u32 VIGetTvFormat(void) { return 0; }        // VI_NTSC
u32 VIGetDTVStatus(void) { return 1; }       // "progressive capable": lets the game offer 480p
u32 VIGetNextField(void) { return s_retrace & 1; }
u32 VIGetRetraceCount(void) { return s_retrace; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) { VIRetraceCallback o = s_post_cb; s_post_cb = cb; return o; }
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) { VIRetraceCallback o = s_pre_cb; s_pre_cb = cb; return o; }

int re4_vi_black(void) { return s_black; }

void VIWaitForRetrace(void)
{
    if (s_pre_cb) s_pre_cb(s_retrace);
#if defined(RE4_XBOX)
    XVideoWaitForVBlank();
#elif defined(RE4_HAVE_SDL)
    SDL_Delay(16);
#else
    struct timespec ts = { 0, 16666667 };
    nanosleep(&ts, 0);
#endif
    s_retrace++;
    re4_dvd_poll();
    re4_card_poll();
    re4_os_poll();
    if (s_post_cb) s_post_cb(s_retrace);
}

// ---- GX system calls --------------------------------------------------------------------------------
static unsigned char s_fifo_obj[128];
typedef void (*GXDrawSyncCallback)(u16 token);
static GXDrawSyncCallback s_drawsync_cb;

void* GXInit(void*, u32) { return s_fifo_obj; }    // the GX state machine initialises lazily (gx_state.cpp)
void* GXSetCurrentGXThread(void) { extern void* OSGetCurrentThread(void); return OSGetCurrentThread(); }
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) { GXDrawSyncCallback o = s_drawsync_cb; s_drawsync_cb = cb; return o; }
void GXSetDrawSync(u16 token) { if (s_drawsync_cb) s_drawsync_cb(token); }   // the "GPU" is always done

// ---- PAD ----------------------------------------------------------------------------------------
struct PADStatus { u16 button; s8 stickX, stickY, substickX, substickY; u8 triggerLeft, triggerRight, analogA, analogB; s8 err; u8 pad; };

enum {
    B_LEFT = 0x1, B_RIGHT = 0x2, B_DOWN = 0x4, B_UP = 0x8, T_Z = 0x10, T_R = 0x20, T_L = 0x40,
    B_A = 0x100, B_B = 0x200, B_X = 0x400, B_Y = 0x800, B_START = 0x1000,
};
enum { PAD_ERR_NONE = 0, PAD_ERR_NO_CONTROLLER = -1 };

#ifdef RE4_HAVE_SDL
static SDL_GameController* s_pads[4];
static int s_pad_init;

static void pad_open(void)
{
    if (!s_pad_init) {
        s_pad_init = 1;
        if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    SDL_GameControllerUpdate();
    for (int i = 0; i < 4; i++) {
        if (s_pads[i] && !SDL_GameControllerGetAttached(s_pads[i])) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = 0;
        }
        if (!s_pads[i] && i < SDL_NumJoysticks() && SDL_IsGameController(i)) s_pads[i] = SDL_GameControllerOpen(i);
    }
}

static s8 axis8(SDL_GameController* c, SDL_GameControllerAxis a, int invert)
{
    int v = SDL_GameControllerGetAxis(c, a) / 256;      // -128..127
    if (invert) v = -v - 1;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (s8) v;
}
static u8 trig8(SDL_GameController* c, SDL_GameControllerAxis a) { return (u8) (SDL_GameControllerGetAxis(c, a) / 128); }
#endif

void PADInit(void) { }
BOOL PADReset(u32) { return 1; }
BOOL PADRecalibrate(u32) { return 1; }
void PADSetAnalogMode(u32) { }
void PADClamp(PADStatus*) { }   // SDL values are already within the clamped ranges the game expects

// Xbox / SDL mapping (05-SYSTEM.md): A A, B B, X X, Y Y, Black/RB Z, LT L, RT R, Start Start,
// d-pad, left stick main stick, right stick C-stick. GC sticks are +Y up; SDL axes are +Y down.
u32 PADRead(PADStatus* st)
{
    memset(st, 0, 4 * sizeof *st);
#ifdef RE4_HAVE_SDL
    pad_open();
    for (int i = 0; i < 4; i++) {
        SDL_GameController* c = s_pads[i];
        if (!c) { st[i].err = PAD_ERR_NO_CONTROLLER; continue; }
        u16 b = 0;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) b |= B_A;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) b |= B_B;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) b |= B_X;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) b |= B_Y;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) b |= B_START;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= T_Z;   // Black
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= B_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= B_RIGHT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= B_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= B_DOWN;
        st[i].triggerLeft = trig8(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        st[i].triggerRight = trig8(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (st[i].triggerLeft > 0xC0) b |= T_L;     // the GC triggers click at the end of their travel
        if (st[i].triggerRight > 0xC0) b |= T_R;
        st[i].button = b;
        st[i].stickX = axis8(c, SDL_CONTROLLER_AXIS_LEFTX, 0);
        st[i].stickY = axis8(c, SDL_CONTROLLER_AXIS_LEFTY, 1);
        st[i].substickX = axis8(c, SDL_CONTROLLER_AXIS_RIGHTX, 0);
        st[i].substickY = axis8(c, SDL_CONTROLLER_AXIS_RIGHTY, 1);
        st[i].err = PAD_ERR_NONE;
    }
    return 0x80000000u >> 0;
#else
    for (int i = 0; i < 4; i++) st[i].err = PAD_ERR_NO_CONTROLLER;
    return 0;
#endif
}

void PADControlMotor(int chan, u32 cmd)
{
#ifdef RE4_HAVE_SDL
    if (chan < 0 || chan > 3 || !s_pads[chan]) return;
    // PAD_MOTOR_RUMBLE = 1; STOP / STOP_HARD stop. The game pulses the motor itself (pad.cpp VibControl).
    SDL_GameControllerRumble(s_pads[chan], cmd == 1 ? 0xC000 : 0, cmd == 1 ? 0xC000 : 0, cmd == 1 ? 100 : 0);
#else
    (void) chan; (void) cmd;
#endif
}

} // extern "C"
