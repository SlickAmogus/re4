/* Xbox stub implementations for symbols missing from nxdk or the old codebase.
 * Provides:
 *   - _strlwr / _strupr  (compat.h maps strlwr → _strlwr under _MSC_VER)
 *   - _fileno / _getdcwd (MSVC CRT functions not in nxdk)
 *   - appicon_bmp stubs  (normally from Windows resource file)
 *   - DirectSound / WinMM audio driver stubs (drivers.c includes them under _WIN32)
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "sys/stat.h"
#include "fcntl.h"

/* ── String helpers ───────────────────────────────────────────────────────── */

/* compat.h maps: strlwr → _strlwr, strupr → _strupr under _MSC_VER */
char *_strlwr(char *s)
{
    for (char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    return s;
}

char *_strupr(char *s)
{
    for (char *p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}

/* Also export as non-underscore aliases (used by code that doesn't go through compat.h) */
char *strlwr(char *s) { return _strlwr(s); }
char *strupr(char *s) { return _strupr(s); }

/* ── _fileno / _getdcwd ───────────────────────────────────────────────────── */

/* _fileno: Convert FILE* to a pseudo-fd usable with _filelength.
 * We use a lookup table so _filelength can recover the FILE* and use fseek/ftell. */
#define FILENO_MAGIC  0x7F000000
#define FILENO_MAX    256

static FILE *fileno_table[FILENO_MAX];
static int   fileno_count = 0;

int _fileno(FILE *f)
{
    if (!f) return -1;
    /* Check if already registered */
    for (int i = 0; i < fileno_count; i++)
        if (fileno_table[i] == f) return FILENO_MAGIC + i;
    if (fileno_count < FILENO_MAX) {
        fileno_table[fileno_count] = f;
        return FILENO_MAGIC + fileno_count++;
    }
    return -1;
}

/* _filelength for pseudo-fds from _fileno (using fseek/ftell) */
long _filelength_from_file(int pseudo_fd)
{
    int idx = pseudo_fd - FILENO_MAGIC;
    if (idx < 0 || idx >= fileno_count || !fileno_table[idx]) return -1;
    FILE *f = fileno_table[idx];
    long cur = ftell(f);
    if (cur < 0) return -1;
    if (fseek(f, 0, 2 /* SEEK_END */) != 0) return -1;
    long end = ftell(f);
    fseek(f, cur, 0 /* SEEK_SET */);
    return end;
}

/* _getdcwd: Get current directory for a specific drive.
 * On Xbox, D:\ is always the game drive. */
char *_getdcwd(int drive, char *buf, int maxlen)
{
    (void)drive;
    const char *cwd = "D:\\";
    if (!buf) {
        buf = (char *)malloc(maxlen > 0 ? maxlen : 4);
        if (!buf) return NULL;
    }
    if (maxlen < 4) return NULL;
    strncpy(buf, cwd, maxlen);
    return buf;
}

/* ── App icon stubs (normally from gameres.rc) ────────────────────────────── */

/* sdlayer2.c references these extern arrays from the compiled resource file.
 * On Xbox we have no window icon, so provide empty stubs. */
const unsigned char appicon_bmp[] = { 0 };
const int appicon_bmp_size = 0;

/* ── DirectSound audio driver stubs ──────────────────────────────────────── */
/* drivers.c includes driver_directsound.h under _WIN32; since we're not
 * compiling driver_directsound.c, we need stub implementations here. */

const char *DirectSoundDrv_GetError(void) { return "DirectSound not available on Xbox"; }
const char *DirectSoundDrv_ErrorString(int e) { (void)e; return ""; }
int  DirectSoundDrv_PCM_Init(int *r, int *c, int *b, void (*cb)(void *, unsigned long), void *data)
     { (void)r; (void)c; (void)b; (void)cb; (void)data; return -1; }
void DirectSoundDrv_PCM_Shutdown(void) {}
int  DirectSoundDrv_PCM_BeginPlayback(char *buf, int bufsz, int n, void (*cb)(void))
     { (void)buf; (void)bufsz; (void)n; (void)cb; return -1; }
void DirectSoundDrv_PCM_StopPlayback(void) {}
void DirectSoundDrv_PCM_Lock(unsigned long pos, unsigned long len, void **p1, void **p2,
                              unsigned long *l1, unsigned long *l2)
     { (void)pos; (void)len; *p1 = NULL; *p2 = NULL; *l1 = 0; *l2 = 0; }
void DirectSoundDrv_PCM_Unlock(void *p1, unsigned long l1, void *p2, unsigned long l2)
     { (void)p1; (void)l1; (void)p2; (void)l2; }

/* ── WinMM audio driver stubs ────────────────────────────────────────────── */

const char *WinMMDrv_GetError(void) { return "WinMM not available on Xbox"; }
const char *WinMMDrv_ErrorString(int e) { (void)e; return ""; }
int  WinMMDrv_CD_Init(void) { return -1; }
void WinMMDrv_CD_Shutdown(void) {}
int  WinMMDrv_CD_Play(int t, int loop) { (void)t; (void)loop; return -1; }
void WinMMDrv_CD_Stop(void) {}
void WinMMDrv_CD_Pause(int p) { (void)p; }
int  WinMMDrv_CD_IsPlaying(void) { return 0; }
void WinMMDrv_CD_SetVolume(int v) { (void)v; }

int  WinMMDrv_MIDI_Init(int *devid, int *latency, void (*cb)(void)) { (void)devid; (void)latency; (void)cb; return -1; }
void WinMMDrv_MIDI_Shutdown(void) {}
int  WinMMDrv_MIDI_StartPlayback(void (*cb)(void)) { (void)cb; return -1; }
void WinMMDrv_MIDI_HaltPlayback(void) {}
void WinMMDrv_MIDI_SetTempo(int tempo, int division) { (void)tempo; (void)division; }
void WinMMDrv_MIDI_Lock(void) {}
void WinMMDrv_MIDI_Unlock(void) {}

/* ── OGG Vorbis library support (mingw-compiled .a needs these) ──────────── */
#include <math.h>
#include <errno.h>

/* ___chkstk_ms: stack probing for large allocations (>4KB).
 * Xbox has a flat memory model with pre-allocated stack, so just return.
 * Use asm name to avoid cdecl underscore prefix mismatch. */
__attribute__((naked)) void __attribute__((used))
__xbox_chkstk_ms(void) __asm__("___chkstk_ms");
__attribute__((naked)) void __xbox_chkstk_ms(void) {
    __asm__ volatile("ret");
}

/* __imp___errno: mingw libs reference errno through a DLL import pointer.
 * Use asm name to get exact symbol. */
static int *_xbox_errno_func(void) { return &errno; }
int *(*__xbox_imp_errno)(void) __asm__("__imp___errno") = _xbox_errno_func;

/* sincos: GNU math extension used by libvorbis. Not in nxdk's math lib. */
void sincos(double x, double *s, double *c) {
    *s = sin(x);
    *c = cos(x);
}
