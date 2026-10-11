// os_misc.cpp — the rest of the Dolphin OS surface the game calls (xbox/docs/05-SYSTEM.md, sdk-symbols.md):
// time base, alarms, console/IPL queries, reset, ROM font, PPC/debugger leftovers.
//
// Time: GameCube timer ticks run at bus clock / 4 = 40.5 MHz (OS_TIMER_CLOCK). The game converts with its
// own OSTicksTo* macros and the SDK's OSTicksToCalendarTime (src/lib/OSTime.c, compiled), so OSGetTime /
// OSGetTick report ticks in that unit, derived from the platform's high-resolution counter.
#include "re4port/re4_xbox.h"
#include <string.h>
#include <time.h>
#ifdef RE4_XBOX
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#endif

extern "C" {

typedef long long OSTime;
typedef unsigned int OSTick;
typedef int BOOL;

static const unsigned long long GC_TIMER_HZ = 162000000ull / 4;

static unsigned long long host_ticks(void)
{
#ifdef RE4_XBOX
    static unsigned long long freq;
    if (!freq) freq = (unsigned long long) KeQueryPerformanceFrequency();
    unsigned long long c = (unsigned long long) KeQueryPerformanceCounter();
    // c * GC_TIMER_HZ / freq without overflow for ~a year of uptime
    return (c / freq) * GC_TIMER_HZ + ((c % freq) * GC_TIMER_HZ) / freq;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * GC_TIMER_HZ + (unsigned long long) ts.tv_nsec * GC_TIMER_HZ / 1000000000ull;
#endif
}

// Epoch: the GameCube counts from 2000-01-01; seed the counter with the wall clock so save timestamps
// (card.cpp OSTicksToCalendarTime) are meaningful.
static long long s_epoch_offset;
static int s_time_init;

static void time_init(void)
{
    if (s_time_init) return;
    s_time_init = 1;
    long long secs_since_2000 = 0;
#ifdef RE4_XBOX
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);                                  // 100 ns since 1601-01-01
    secs_since_2000 = (long long) (now.QuadPart / 10000000LL) - 12591158400LL;
#else
    secs_since_2000 = (long long) time(0) - 946684800LL;
#endif
    s_epoch_offset = secs_since_2000 * (long long) GC_TIMER_HZ - (long long) host_ticks();
}

OSTime OSGetTime(void) { time_init(); return (OSTime) host_ticks() + s_epoch_offset; }
OSTick OSGetTick(void) { return (OSTick) host_ticks(); }

// ---- alarms: the game uses one-shot alarms only from main.cpp; fired from re4_os_poll() ----------
struct OSAlarm {                    // include/dolphin/os/OSAlarm.h layout (0x28)
    void (*handler)(OSAlarm*, void*);
    unsigned tag;
    OSTime fire;
    OSAlarm* prev;
    OSAlarm* next;
    OSTime period;
    OSTime start;
};
enum { MAX_ALARMS = 16 };
static OSAlarm* s_alarms[MAX_ALARMS];

void OSInitAlarm(void) { memset(s_alarms, 0, sizeof s_alarms); }
void OSCreateAlarm(OSAlarm* a) { memset(a, 0, sizeof *a); }

void OSCancelAlarm(OSAlarm* a)
{
    for (int i = 0; i < MAX_ALARMS; i++) if (s_alarms[i] == a) s_alarms[i] = 0;
    a->handler = 0;
}

static void arm(OSAlarm* a, OSTime fire, OSTime period, void (*h)(OSAlarm*, void*))
{
    OSCancelAlarm(a);
    a->handler = h;
    a->fire = fire;
    a->period = period;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!s_alarms[i]) { s_alarms[i] = a; return; }
    }
    re4_log("OSSetAlarm: alarm table full\n");
}

void OSSetAlarm(OSAlarm* a, OSTime tick, void (*h)(OSAlarm*, void*)) { arm(a, OSGetTime() + tick, 0, h); }
void OSSetAbsAlarm(OSAlarm* a, OSTime t, void (*h)(OSAlarm*, void*)) { arm(a, t, 0, h); }
void OSSetPeriodicAlarm(OSAlarm* a, OSTime start, OSTime period, void (*h)(OSAlarm*, void*)) { arm(a, start, period, h); }

// Called by the port at safe points (VIWaitForRetrace, DVD watcher): runs due alarm handlers.
void re4_os_poll(void)
{
    OSTime now = OSGetTime();
    for (int i = 0; i < MAX_ALARMS; i++) {
        OSAlarm* a = s_alarms[i];
        if (!a || a->fire > now) continue;
        void (*h)(OSAlarm*, void*) = a->handler;
        if (a->period) a->fire += a->period; else s_alarms[i] = 0;
        if (h) h(a, 0);
    }
}

// ---- console / IPL ----------------------------------------------------------------------------
static unsigned s_sound_mode = 1;        // stereo
static unsigned s_progressive = 0;

void OSInit(void) { time_init(); OSInitAlarm(); }
unsigned OSGetConsoleType(void) { return 0x10000006; }      // retail HW2-class console; dev bit (0xF0000000) clear
unsigned OSGetConsoleSimulatedMemSize(void) { return 0x01800000; }
unsigned OSGetPhysicalMemSize(void) { return 0x01800000; }
unsigned OSGetSoundMode(void) { return s_sound_mode; }
void OSSetSoundMode(unsigned m) { s_sound_mode = m; }
unsigned OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(unsigned on) { s_progressive = on; }
BOOL OSGetResetButtonState(void) { return 0; }
void OSSetSaveRegion(void*, void*) { }                       // RESET_KEEP_WORK survives because the arena does
BOOL DBIsDebuggerPresent(void) { return 0; }

void OSResetSystem(int reset, unsigned code, BOOL menu)
{
    re4_log("OSResetSystem(%d, 0x%x, %d)\n", reset, code, menu);
#ifdef RE4_XBOX
    HalReturnToFirmware(menu ? HalQuickRebootRoutine : HalRebootRoutine);
#else
    extern void exit(int);
    exit(0);
#endif
}

// ROM font (IPL): used for the DVD error screen text. Not available: report "no font" so RomFont*
// prints nothing; the port shows disc errors through its own log.
unsigned short OSGetFontEncode(void) { return 0; }           // OS_FONT_ENCODE_ANSI
BOOL OSInitFont(void*) { return 0; }
char* OSGetFontTexture(const char* s, void** image, int* x, int* y, int* w)
{
    if (image) *image = 0;
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    return (char*) (s && *s ? s + 1 : s);
}

// ---- PowerPC performance monitor / sync / locked cache: no-ops ----------------------------------
void PPCSync(void) { }
unsigned PPCMfpmc1(void) { return 0; }
void PPCMtmmcr0(unsigned) { }
void PPCMtmmcr1(unsigned) { }
void PPCMtpmc1(unsigned) { }
void PPCMtpmc2(unsigned) { }
void PPCMtpmc3(unsigned) { }
void PPCMtpmc4(unsigned) { }
void LCEnable(void) { }

} // extern "C"

// ---- misc SDK leftovers the link listed (sdk-symbols.md "game-side") -------------------------------
extern "C" {
void EXIProbeReset(void) { }

// TPL lookup (SDK texPalette.c): the descriptor array is relocated in place by calcTplAddr.
struct TEXDescriptorP { void* textureHeader; void* CLUTHeader; };
struct TEXPaletteP { unsigned version; unsigned numDescriptors; TEXDescriptorP* descriptorArray; };
TEXDescriptorP* TEXGet(TEXPaletteP* pal, unsigned id) { return &pal->descriptorArray[id]; }
}

// game/exception.cpp is not compiled (PowerPC exception handler); main.cpp still calls its init.
void ExceptionInit(void) { }

// SN ProDG host file server (debug build: HDRead/HDWrite talk to the PC over the debugger link).
// No host here: every call fails, which file.cpp already handles (the debug save/load paths give up).
extern "C" {
int PCinit(void) { return -1; }
int PCcreat(const char*, int) { return -1; }
int PCopen(const char*, int, int) { return -1; }
int PCclose(int) { return -1; }
int PCread(int, void*, int) { return -1; }
int PCwrite(int, const void*, int) { return -1; }
int PClseek(int, int, int) { return -1; }
}

// CodeWarrior runtime names the MWCC-built CRI libraries reference (src/lib tealeaf on the GameCube).
extern "C" {
float __float_nan = __builtin_nanf("");                       // adx_dcd.c returns it for invalid input
unsigned __cvt_fp2unsigned(float f)                           // adx_tlk.c: float -> u32 with saturation
{
    if (!(f > 0.0f)) return 0;
    if (f >= 4294967296.0f) return 0xFFFFFFFFu;
    return (unsigned) f;
}
}
