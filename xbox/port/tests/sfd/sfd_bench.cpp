// xbox/port/tests/sfd/sfd_bench.cpp -- headless Sofdec decode benchmark (CT-07).
//
// Plays a retail .sfd through the game's own CRI stack (src/lib ADX/Sofdec/GCCI prepared by
// xbox/tools/prep_cri.py) the way cSofdec does (SofdecInit, mwPlyGetHdrInf, mwPlyCreateSofdec,
// mwPlyStartFname, then ADXM_ExecMain + mwPlyGetCurFrm/RelCurFrm once per 59.94 Hz field), on a
// virtual clock so it runs as fast as the decoder allows. Reports frames, resolution, ms per decoded
// frame (time inside ADXM_ExecMain = demux + MPEG-1 video + ADX audio decode) and the cost of the
// Y8/CbCr texture conversion; writes the Y plane of every Nth frame as PGM.
//
//   sfd_bench <movie.sfd> [--pgm DIR] [--every N] [--frames N] [--yraw FILE]
//
// Build: xbox/port/tests/sfd/build_sfd_bench.sh (32-bit Windows, clang -m32, lld).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "mwply.h"
#undef main  // re4_xbox.h renames the game's main

extern "C" {
extern const char* sfd_bench_path;
extern long long sfd_bench_dvd_bytes;
extern long long sfd_bench_pcm_bytes, sfd_bench_pcm_nonzero;
void sfd_bench_vsync(void);
void ADXM_ExecMain(void);
void ADXGC_SetupDvdFs(int mode);
}

static double now_ms()
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    // print the fault and a short stack walk (EBP chain) for llvm-symbolizer --obj=sfd_bench.exe
    unsigned base = (unsigned) (size_t) GetModuleHandleA(NULL);
    fprintf(stderr, "crash: code 0x%08lx at 0x%08x (image base 0x%08x) addr 0x%08x\n", ep->ExceptionRecord->ExceptionCode,
            (unsigned) (size_t) ep->ExceptionRecord->ExceptionAddress, base,
            (unsigned) ep->ExceptionRecord->ExceptionInformation[1]);
    unsigned* fp = (unsigned*) (size_t) ep->ContextRecord->Ebp;
    for (int i = 0; i < 16 && fp && !IsBadReadPtr(fp, 8); i++) {
        fprintf(stderr, "  ret 0x%08x\n", fp[1]);
        fp = (unsigned*) (size_t) fp[0];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// watchdog: if the field counter stops moving for 5 s, dump the main thread's EIP/EBP chain and exit
static volatile LONG s_heartbeat;
static HANDLE s_main_thread;
static DWORD WINAPI watchdog(void*)
{
    LONG last = -1;
    int still = 0;
    for (;;) {
        Sleep(1000);
        LONG hb = s_heartbeat;
        still = (hb == last) ? still + 1 : 0;
        last = hb;
        if (still >= 5) {
            CONTEXT c;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_FULL;
            SuspendThread(s_main_thread);
            GetThreadContext(s_main_thread, &c);
            fprintf(stderr, "hang: at 0x%08x (image base 0x%08x)\n", (unsigned) c.Eip, (unsigned) (size_t) GetModuleHandleA(NULL));
            unsigned* fp = (unsigned*) (size_t) c.Ebp;
            for (int i = 0; i < 16 && fp && !IsBadReadPtr(fp, 8); i++) {
                fprintf(stderr, "  ret 0x%08x\n", fp[1]);
                fp = (unsigned*) (size_t) fp[0];
            }
            ExitProcess(3);
        }
    }
}

static int s_errors;
static void err_cb(void* obj, const char* msg)
{
    (void) obj;
    fprintf(stderr, "CRI error: %s\n", msg ? msg : "(null)");
    s_errors++;
}

static void write_pgm(const char* path, const unsigned char* y, int w, int h, int pitch)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int r = 0; r < h; r++) fwrite(y + r * pitch, 1, (size_t) w, f);
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* pgm_dir = NULL;
    const char* yraw = NULL;
    int every = 30, max_frames = 1 << 30;
    if (argc < 2) {
        fprintf(stderr, "usage: %s movie.sfd [--pgm DIR] [--every N] [--frames N] [--yraw FILE]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(crash_filter);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    sfd_bench_path = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--pgm") && i + 1 < argc) pgm_dir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--yraw") && i + 1 < argc) yraw = argv[++i];
    }
    FILE* yraw_f = yraw ? fopen(yraw, "wb") : NULL;

    // dvd.cpp at boot, then cSofdec's SofdecInit
    ADXGC_SetupDvdFs(0);
    MWS_PLY_INIT_SFD iprm;
    memset(&iprm, 0, sizeof iprm);
    iprm.disp_cycle = 59.94f;
    iprm.x04 = 1;
    iprm.x08 = 1;
    iprm.x0C = 1;
    ADXM_SetCbErr(err_cb, NULL);
    double t0 = now_ms();
    mwPlyInitSfdFx(&iprm);
    double t_init = now_ms() - t0;

    // cSofdec::Initialize: header for the size
    static unsigned char hdr[0x5000];
    FILE* f = fopen(sfd_bench_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", sfd_bench_path); return 1; }
    fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    MWS_SFD_HDRINF info;
    memset(&info, 0, sizeof info);
    mwPlyGetHdrInf(hdr, sizeof hdr, &info);
    // include/mwply.h reads width/height as s16 at 0x0A/0x0E, the low halves of CRI's Sint32 fields
    // at 0x08/0x0C on a big-endian CPU. On x86 the whole words must be read (port fix needed in
    // mwply.h, see 05-SYSTEM.md "Movies: measurements").
    const int* hi = (const int*) &info;
    int hdr_w = hi[2], hdr_h = hi[3];
    printf("header: valid %d mode %d %dx%d picrate %d maxfrm %d afreq %d ach %d\n", hi[0], hi[1], hdr_w, hdr_h,
           hi[4], hi[5], hi[9], hi[10]);

    // cSofdec::startApp
    // the CRI parameter block is 0x30 bytes (MWSFD_CRPRM: buffmt at 0x24); the game's copy sits in
    // the memclr'ed AP_OBJ, so the tail is zero
    static union { MWS_PLY_CPRM_SFD p; unsigned char raw[0x30]; } cprm_u;
    MWS_PLY_CPRM_SFD& cprm = cprm_u.p;
    memset(&cprm_u, 0, sizeof cprm_u);
    cprm.compo_mode = 0;
    cprm.ftype = 1;
    cprm.max_bps = 8000000;
    cprm.nfrm_pool_wk = 4;
    cprm.max_width = hdr_w;
    cprm.max_height = hdr_h;
    cprm.max_stm = 2;
    cprm.wksize = mwPlyCalcWorkCprmSfd(&cprm);
    cprm.work = calloc(1, (size_t) cprm.wksize + 64);
    printf("work: %d bytes\n", cprm.wksize);
    MWPLY hn = mwPlyCreateSofdec(&cprm);
    if (!hn) { fprintf(stderr, "mwPlyCreateSofdec failed\n"); return 1; }
    mwPlyStartFname(hn, "movie/bench.sfd");

    // texture buffers as cSofdec::allocTexMem mode 0 (width rounded to 32, UV half size)
    int tw = (hdr_w + 31) & ~31, th = hdr_h;
    unsigned char* ybuf = (unsigned char*) calloc(1, (size_t) tw * th);
    unsigned char* cbuf = (unsigned char*) calloc(1, (size_t) tw * th);

    double t_exec = 0, t_exec_max = 0, t_cnv = 0;
    int frames = 0, fields = 0, last_fno = -1, dup = 0, stat = 0, w = 0, h = 0;
    for (fields = 0; fields < 20 * 60 * 60; fields++) {
        sfd_bench_vsync();
        InterlockedIncrement(&s_heartbeat);
        double a = now_ms();
        ADXM_ExecMain();
        double b = now_ms() - a;
        t_exec += b;
        if (b > t_exec_max) t_exec_max = b;
        MWS_FRM frm;
        memset(&frm, 0, sizeof frm);
        mwPlyGetCurFrm(hn, &frm);
        if (frm.bufadr != NULL) {
            w = frm.width;
            h = frm.height;
            if (frm.fno == last_fno) dup++;
            last_fno = frm.fno;
            double c = now_ms();
            mwPlyFxSetOutBufSize(hn, tw, th);
            mwPlyFxCnvFrmY84C44(hn, &frm, ybuf, cbuf);
            t_cnv += now_ms() - c;
            // MPV output planes: luma first, pitch = 16-aligned width rounded to 32 (MPVUMC_InitOutRfb)
            int ypitch = (((w + 15) / 16 * 16) + 31) / 32 * 32;
            if (yraw_f) for (int r = 0; r < h; r++) fwrite((unsigned char*) frm.bufadr + r * ypitch, 1, (size_t) w, yraw_f);
            if (pgm_dir && every > 0 && frames % every == 0) {
                char p[512];
                snprintf(p, sizeof p, "%s/f%05d.pgm", pgm_dir, frames);
                write_pgm(p, (const unsigned char*) frm.bufadr, w, h, ypitch);
            }
            frames++;
            mwPlyRelCurFrm(hn);
            if (frames >= max_frames) break;
        }
        stat = mwPlyGetStat(hn);
        if (stat == MWE_PLY_STAT_PLAYEND || stat == MWE_PLY_STAT_ERROR) break;
    }
    if (yraw_f) fclose(yraw_f);
    printf("status %d after %d fields (%.1f s of movie time), %d frames %dx%d, %d repeated fno, %d CRI errors\n",
           stat, fields, fields / 59.94, frames, w, h, dup, s_errors);
    printf("skips: decode %d, display %d\n", mwPlyGetNumSkipDec(hn), mwPlyGetNumSkipDisp(hn));
    printf("init (mwPlyInitSfdFx, IDCT tables): %.2f ms\n", t_init);
    printf("ADXM_ExecMain total %.1f ms, %.3f ms/frame, worst field %.2f ms\n", t_exec, frames ? t_exec / frames : 0.0, t_exec_max);
    printf("Y84C44 conversion %.3f ms/frame\n", frames ? t_cnv / frames : 0.0);
    printf("file bytes read %lld\n", sfd_bench_dvd_bytes);
    printf("ADX PCM to ARAM %lld bytes (%.1f s at 32 kHz stereo s16), %lld non-zero samples\n", sfd_bench_pcm_bytes,
           sfd_bench_pcm_bytes / (32000.0 * 2 * 2), sfd_bench_pcm_nonzero);
    mwPlyDestroy(hn);
    return (stat == MWE_PLY_STAT_ERROR || s_errors) ? 1 : 0;
}
