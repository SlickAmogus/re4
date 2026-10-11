// os_shim.cpp — the OS* / DC* / VI* / PPC* surface the game calls (xbox/docs/05-SYSTEM.md).
// Phase 0: logging, memory arena, time; everything else is a logging stub that the host bring-up
// (CT-03) turns into real code. No game header is included here on purpose: the game's own
// declarations in include/dolphin/os.h carry CodeWarrior-isms; we match the ABI (cdecl, 32-bit).
#include "re4port/re4_xbox.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

unsigned char* re4_mem1_base = 0;
unsigned char* re4_aram_base = 0;

// --- logging -------------------------------------------------------------------------------------

void re4_log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef RE4_XBOX
    extern void xbox_log_write(const char* s, int len);
    xbox_log_write(buf, (int) strlen(buf));
#else
    fputs(buf, stderr);
#endif
}

// Logs a message once per key (used by the UNIMPLEMENTED stubs so a hot path does not flood the log).
void re4_log_once(const char* key, const char* fmt, ...)
{
    enum { MAX_KEYS = 512 };
    static const char* seen[MAX_KEYS];
    static int n;
    for (int i = 0; i < n; i++) {
        if (seen[i] == key || strcmp(seen[i], key) == 0) return;
    }
    if (n < MAX_KEYS) seen[n++] = key;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    re4_log("%s", buf);
}

void re4_fatal(const char* file, int line, const char* msg)
{
    re4_log("FATAL %s:%d: %s\n", file, line, msg);
#ifdef RE4_XBOX
    for (;;) { }   // keep the log readable on the debug channel
#else
    abort();
#endif
}

// The game's OSReport (313 call sites) and OSPanic.
void OSReport(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    re4_log("%s", buf);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    re4_fatal(file, line, msg);
}

// --- memory arena -------------------------------------------------------------------------------

// Called by the platform main before re4_main(). On Xbox the arena must be GPU-visible
// (MmAllocateContiguousMemoryEx below MAXRAM); on the host a plain aligned allocation.
int re4_mem_init(void)
{
#ifdef RE4_XBOX
    extern void* re4_xbox_alloc_contiguous(unsigned size);
    re4_mem1_base = (unsigned char*) re4_xbox_alloc_contiguous(RE4_MEM1_SIZE);
    re4_aram_base = (unsigned char*) re4_xbox_alloc_contiguous(RE4_ARAM_SIZE);
#else
    re4_mem1_base = (unsigned char*) aligned_alloc(0x1000, RE4_MEM1_SIZE);
    re4_aram_base = (unsigned char*) aligned_alloc(0x1000, RE4_ARAM_SIZE);
#endif
    if (!re4_mem1_base || !re4_aram_base) {
        re4_log("re4_mem_init: arena allocation failed (MEM1 %u, ARAM %u)\n", RE4_MEM1_SIZE, RE4_ARAM_SIZE);
        return -1;
    }
    memset(re4_mem1_base, 0, RE4_MEM1_SIZE);
    re4_log("re4_mem_init: MEM1 %p ARAM %p\n", re4_mem1_base, re4_aram_base);
    return 0;
}

// OSGetArenaLo/Hi: the game's SystemMemInit sets the heap between the end of the "ELF" (static data,
// must be < GC 0x80350000) and 0x817F4000. We report the whole arena as the ELF end so the game's
// "ELF size overflow" check passes and let main_mem.cpp place its heaps at their fixed addresses.
void* OSGetArenaLo(void) { return GC_ADDR(0x80300000); }
void* OSGetArenaHi(void) { return GC_ADDR(0x81800000); }
void OSSetArenaLo(void*) { }
void OSSetArenaHi(void*) { }

// --- cache ops: no-ops on x86 ------------------------------------------------------------------

void DCFlushRange(void*, unsigned) { }
void DCFlushRangeNoSync(void*, unsigned) { }
void DCStoreRange(void*, unsigned) { }
void DCStoreRangeNoSync(void*, unsigned) { }
void DCInvalidateRange(void*, unsigned) { }
void ICInvalidateRange(void*, unsigned) { }

// --- interrupts: a critical section shared with the audio/DVD threads (CT-03) --------------------

int OSDisableInterrupts(void) { return 1; }
int OSEnableInterrupts(void) { return 0; }
int OSRestoreInterrupts(int level) { return level; }

} // extern "C"
