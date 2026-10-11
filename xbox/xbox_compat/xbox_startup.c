/* Xbox hardware pre-initialization and debug logging.
 * Called before main() via constructor attribute, so hardware resets run
 * before SDL_Init.
 *
 * - AC97 cold reset: clears stale audio DMA/interrupts from dashboard or
 *   previous app launch.  Without this, audio may not work on re-launch.
 * - Video mode: re-applies dashboard-configured resolution so nxdk HAL
 *   framebuffer matches (480i/480p/720p/1080i autodetect). */

#include <hal/video.h>
/* hal/audio.h NOT included — we do manual AC97 reset to avoid XAudioInit
 * grabbing IRQ 6, which RXDK's DirectSound needs for its AC97 driver. */
#include <hal/debug.h>
#include <hal/xbox.h>
#include <windows.h>   /* OutputDebugStringA is WINAPI (stdcall) */
#include <stdarg.h>
#include <stdio.h>    /* vsnprintf */
#include <stdlib.h>   /* atexit */
#include <string.h>
#include "fcntl.h"    /* _O_WRONLY / _O_CREAT / _O_TRUNC */

/* Use _open/_write directly (posix_io.c / NtCreateFile) rather than fopen.
 * fopen goes through pdclib stdio which silently failed; _open uses the same
 * NtCreateFile path that successfully reads DUKE3D.GRP. */
static int xbox_log_fd = -1;

/* Default off for release builds.  Cfg key [Debug]EnableLogging = 1 turns
 * it on; CONFIG_ReadSetup calls xbox_log_open_file() once the flag is set. */
int xbox_logging_enabled = 0;

/* _write is declared in io.h but we can also just declare it here */
extern int _write(int fd, const void *buf, unsigned int count);
extern int _open(const char *path, int flags, ...);
extern int _close(int fd);

/* Writable path prefix for config/saves/logs.
 * Probed at startup: D:\ first (works when running from HDD via FTP),
 * falls back to E:\re4\ (works when booting from XISO disc).
 * Other files use xbox_get_writedir() to build writable paths. */
static char xbox_writedir[64] = "D:\\";

/* Returns the writable directory prefix (with trailing backslash).
 * E.g. "D:\\" when running from HDD, "E:\\jfduke3d\\" from disc. */
const char *xbox_get_writedir(void)
{
    return xbox_writedir;
}

void xbox_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int len;

    if (!xbox_logging_enabled) return;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    /* Kernel debug output — captured by xemu console and debug tools */
    OutputDebugStringA(buf);

    /* File log — written with _write so it survives a crash */
    if (xbox_log_fd >= 0)
        _write(xbox_log_fd, buf, (unsigned)len);
}

/* Write a raw pre-formatted string directly to the log file.
 * Bypasses the 512-byte vsnprintf buffer in xbox_log(). */
void xbox_log_write(const char *str, int len)
{
    if (!xbox_logging_enabled) return;
    if (len <= 0) return;
    OutputDebugStringA(str);
    if (xbox_log_fd >= 0)
        _write(xbox_log_fd, str, (unsigned)len);
}

/* Called by CONFIG_ReadSetup after cfg load if EnableLogging is true.
 * Opens re4.log for write+truncate.  Idempotent. */
void xbox_log_open_file(void)
{
    char logpath[80];
    int n = 0;
    const char *s;
    const char *lf;

    if (xbox_log_fd >= 0) return;  /* already open */

    s = xbox_writedir;
    while (*s) logpath[n++] = *s++;
    lf = "re4.log";
    while (*lf) logpath[n++] = *lf++;
    logpath[n] = '\0';
    xbox_log_fd = _open(logpath, _O_WRONLY | _O_CREAT | _O_TRUNC, 0);
}

/* atexit handler: stop audio DMA and pause hardware before the quick-reboot
 * that nxdk's exit() triggers.  This gives the next app launch a cleaner
 * starting state. */
static void xbox_cleanup(void)
{
    xbox_log("XBOX: cleanup — pausing audio hardware\n");
    /* Stop AC97 bus master DMA engines (analog + digital) */
    volatile unsigned char *ac97 = (volatile unsigned char *)0xFEC00000u;
    ac97[0x11B] = 0x1E;  /* reset analog PCM out */
    ac97[0x17B] = 0x1E;  /* reset digital S/PDIF out */
}

__attribute__((constructor))
static void xbox_hw_preinit(void)
{
    /* --- AC97 audio cold reset (manual, without grabbing IRQ 6) ---
     * Quick-reboots may leave the AC97 controller with stale DMA descriptors,
     * running engines, or pending interrupts.
     *
     * We used to call XAudioInit() here, but it connects an ISR to IRQ 6
     * and never releases it.  RXDK's DirectSound needs IRQ 6 for its own
     * CAc97Device driver.  So we do the cold reset ourselves — just the
     * MMIO register pokes, no interrupt registration. */
    {
        volatile unsigned char *ac97 = (volatile unsigned char *)0xFEC00000u;

        /* Cold reset: deassert then assert bit 1 of Global Control (0x12C) */
        *(volatile unsigned long *)(ac97 + 0x12C) &= ~2u;
        /* Brief delay — kernel KeStallExecutionProcessor isn't available yet
         * in a constructor, so use a volatile loop */
        for (volatile int d = 0; d < 1000; d++) {}
        *(volatile unsigned long *)(ac97 + 0x12C) |= 2u;

        /* Wait for codec ready (bit 8 of Global Status at 0x130) */
        for (int timeout = 0; timeout < 100000; timeout++) {
            if (*(volatile unsigned long *)(ac97 + 0x130) & 0x100)
                break;
        }

        /* Reset bus master registers — clear stale DMA state */
        /* Analog PCM out: offset 0x11B, set bits 4,3,2,1 */
        ac97[0x11B] = 0x1E;
        while (ac97[0x11B] & 0x02) {}  /* wait for reset complete */

        /* Digital S/PDIF out: offset 0x17B */
        ac97[0x17B] = 0x1E;
        while (ac97[0x17B] & 0x02) {}

        /* Enable S/PDIF via PCI config register 0x4C bit 24 */
        {
            unsigned long spdifReg = 0;
            unsigned long aciSlot = (0 << 3) | 6;
            HalReadWritePCISpace(0, aciSlot, 0x4C, &spdifReg, sizeof(spdifReg), FALSE);
            spdifReg |= 0x01000000u;
            HalReadWritePCISpace(0, aciSlot, 0x4C, &spdifReg, sizeof(spdifReg), TRUE);
        }
    }

    /* --- Video mode ---
     * Set a safe 640x480 default.  The real display resolution is applied
     * later from duke3d.cfg (DisplayWidth/DisplayHeight) after config is
     * read in game.c. */
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    /* Register cleanup handler (runs before exit()'s HalReturnToFirmware). */
    atexit(xbox_cleanup);

    /* Probe writable directory: try D:\ first (HDD launch), fall back to
     * E:\re4\ (disc/XISO launch).  We test by creating+closing a file. */
    {
        static const char * const try_dirs[] = {
            "D:\\",
            "E:\\jfduke3d\\",
            "E:\\",
            NULL
        };
        for (int i = 0; try_dirs[i]; i++) {
            char probe[80];
            int n = 0;
            const char *d = try_dirs[i];
            while (*d && n < 64) probe[n++] = *d++;
            /* Append test filename */
            const char *tf = ".writeable";
            while (*tf && n < 78) probe[n++] = *tf++;
            probe[n] = '\0';

            int fd = _open(probe, _O_WRONLY | _O_CREAT | _O_TRUNC, 0);
            if (fd >= 0) {
                _close(fd);
                /* Copy the winning prefix */
                const char *s = try_dirs[i];
                char *dst = xbox_writedir;
                while (*s) *dst++ = *s++;
                *dst = '\0';
                break;
            }
        }
    }

    /* Log file is opened lazily by xbox_log_open_file() after cfg load
     * if [Debug]EnableLogging = 1.  Until then, all xbox_log calls no-op. */
}
