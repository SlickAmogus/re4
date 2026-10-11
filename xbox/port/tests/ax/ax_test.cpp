// ax_test.cpp — CT-10: scripted AX/MIX/AXFX calls against xbox/port/snd/{ax_shim,snd_soft}.cpp,
// rendered through the software mixer's capture sink into WAVs (xbox/port/tests/out/) and checked for
// duration, level, pitch, end-of-voice signalling (pb.state / pb.addr as the game reads them), AUX
// callbacks, AXFX reverb tail, voice stealing, 48 kHz output and re4_audio_pump pacing. 32-bit host.
//
//   make -C xbox/port/tests/ax test            (see Makefile; Windows: CXX/CC = LLVM clang, -fuse-ld=lld)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(long long* c);
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long* f);
static void sleep_ms(unsigned ms) { Sleep(ms); }
static double now_ms(void)
{
    long long c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double) c * 1000.0 / (double) f;
}
#else
#include <time.h>
static void sleep_ms(unsigned ms) { struct timespec t = { (time_t) (ms / 1000), (long) (ms % 1000) * 1000000L }; nanosleep(&t, 0); }
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}
#endif

#define SND_SDK_NO_GX 1
#include "snd_sdk.h"
#include <dolphin/ai.h>
#include "re4port/ax_shim.h"
#include "../ct06/dspadpcm_ref_enc.h"

extern "C" {
void re4_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_log_once(const char*, const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void re4_fatal(const char* f, int l, const char* m) { printf("FATAL %s:%d %s\n", f, l, m); exit(2); }
unsigned char* re4_mem1_base;
unsigned char* re4_aram_base;
}

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- capture ---------------------------------------------------------------------------------
// interleaved stereo capture (no STL: the host MSVC STL rejects older clang)
struct Cap {
    int16_t* d;
    size_t n, cap;
    size_t size() const { return n; }
    void clear() { n = 0; }
    const int16_t& operator[](size_t i) const { return d[i]; }
    const int16_t* data() const { return d; }
    void push(const int16_t* p, size_t k)
    {
        if (n + k > cap) { cap = (n + k) * 2; d = (int16_t*) realloc(d, cap * sizeof(int16_t)); }
        memcpy(d + n, p, k * sizeof(int16_t));
        n += k;
    }
};
static Cap g_cap;
static void cap_fn(const int16_t* lr, uint32_t frames, void*) { g_cap.push(lr, (size_t) frames * 2); }

static void write_wav(const char* name, const Cap& s, uint32_t rate)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", OUT_DIR, name);
    FILE* f = fopen(path, "wb");   // NOLINT
    if (!f) { printf("note: cannot write %s\n", path); return; }
    uint32_t data = (uint32_t) s.size() * 2, v;
    uint16_t h;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 2; fwrite(&h, 2, 1, f);
    fwrite(&rate, 4, 1, f); v = rate * 4; fwrite(&v, 4, 1, f); h = 4; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s.data(), 2, s.size(), f);
    fclose(f);
}

// RMS of channel ch over stereo frames [a, b)
static double rms(const Cap& s, size_t a, size_t b, int ch)
{
    double acc = 0;
    if (b * 2 > s.size()) b = s.size() / 2;
    if (b <= a) return 0;
    for (size_t i = a; i < b; i++) { double x = s[2 * i + ch]; acc += x * x; }
    return sqrt(acc / (double) (b - a));
}
// frequency from rising zero crossings of the left channel over [a, b)
static double freq(const Cap& s, size_t a, size_t b, uint32_t rate)
{
    int n = 0;
    size_t first = 0, last = 0;
    for (size_t i = a + 1; i < b && 2 * i < s.size(); i++)
        if (s[2 * (i - 1)] < 0 && s[2 * i] >= 0) { if (!n) first = i; last = i; n++; }
    return n > 1 ? (double) (n - 1) * rate / (double) (last - first) : 0;
}

// ---- ARAM sample ---------------------------------------------------------------------------
struct Sample { uint32_t byte; uint32_t len; uint8_t ps; DspAdpcmLoopCtx lc; };

static Sample put_tone(uint32_t aram_byte, uint32_t len, double hz, double amp, uint32_t loop_start)
{
    int16_t* pcm = (int16_t*) malloc(len * sizeof(int16_t));
    int16_t* recon = (int16_t*) malloc(len * sizeof(int16_t));
    for (uint32_t i = 0; i < len; i++) pcm[i] = (int16_t) (amp * sin(2 * M_PI * hz * i / 32000.0));
    uint32_t bytes = dspadpcm_ref_encode(pcm, len, dspadpcm_ref_test_coef, re4_aram_base + aram_byte, recon);
    (void) bytes;
    Sample s;
    s.byte = aram_byte;
    s.len = len;
    s.ps = re4_aram_base[aram_byte];
    dspadpcm_ref_loop_ctx(re4_aram_base + aram_byte, recon, loop_start, &s.lc);
    free(pcm);
    free(recon);
    return s;
}

static int g_drops;
static AXVPB* g_dropped;
static void cb_drop(void* v) { g_drops++; g_dropped = (AXVPB*) v; }

// programs a voice the way iss_ax_set_para does (snd_iss3.cpp:255-343)
static AXVPB* start_voice(const Sample& s, int loop, uint32_t loop_start, uint32_t loop_len, float ratio,
                          int input_db, int auxa_db, int pan)
{
    AXVPB* v = AXAcquireVoice(30, cb_drop, 0);
    if (!v) return 0;
    AXPBADDR addr;
    AXPBADPCM ad;
    AXPBADPCMLOOP lp;
    AXPBSRC src;
    AXPBLPF lpf;
    uint32_t cur = s.byte * 2 + 2, lo, end;
    if (!loop) {
        lo = 0 * 2 + 2;   // the silent zero buffer at ARAM 0 (Snd_ctrl_work.zero_adrs)
        end = cur + s.len / 14 * 16 + s.len % 14;   // iss3 one-past-the-end quirk (03-AUDIO.md)
    } else {
        lo = cur + loop_start / 14 * 16 + loop_start % 14;
        uint32_t le = loop_start + loop_len - 1;      // inclusive end of the loop body
        end = cur + le / 14 * 16 + le % 14;
        lp.loop_pred_scale = s.lc.pred_scale;
        lp.loop_yn1 = (u16) s.lc.yn1;
        lp.loop_yn2 = (u16) s.lc.yn2;
        AXSetVoiceAdpcmLoop(v, &lp);
    }
    addr.loopFlag = (u16) loop;
    addr.format = 0;
    addr.loopAddressHi = (u16) (lo >> 16); addr.loopAddressLo = (u16) lo;
    addr.endAddressHi = (u16) (end >> 16); addr.endAddressLo = (u16) end;
    addr.currentAddressHi = (u16) (cur >> 16); addr.currentAddressLo = (u16) cur;
    for (int k = 0; k < 8; k++) { ad.a[k][0] = (u16) dspadpcm_ref_test_coef[2 * k]; ad.a[k][1] = (u16) dspadpcm_ref_test_coef[2 * k + 1]; }
    ad.gain = 0; ad.pred_scale = s.ps; ad.yn1 = 0; ad.yn2 = 0;
    uint32_t r = (uint32_t) (ratio * 65536.0f);
    src.ratioHi = (u16) (r >> 16); src.ratioLo = (u16) r; src.currentAddressFrac = 0;
    memset(src.last_samples, 0, sizeof src.last_samples);
    lpf.on = 0; lpf.yn1 = 0; lpf.a0 = 0; lpf.b0 = 0;
    AXSetVoiceType(v, 0);
    AXSetVoiceAddr(v, &addr);
    AXSetVoiceAdpcm(v, &ad);
    AXSetVoiceLpf(v, &lpf);
    AXSetVoiceSrc(v, &src);
    AXSetVoiceSrcType(v, 1);
    MIXInitChannel(v, 0, input_db, auxa_db, -960, pan, 127, 0);
    AXSetVoiceState(v, 1);
    return v;
}

static int g_frames_cb;
static void cb_frame(void) { g_frames_cb++; MIXUpdateSettings(); }

static int g_aux_calls;
static double g_aux_energy;
static void cb_aux(void* data, void*)
{
    AX_AUX_DATA* d = (AX_AUX_DATA*) data;
    g_aux_calls++;
    for (int i = 0; i < 160; i++) g_aux_energy += (double) d->l[i] * d->l[i];
}

static void reset_ax(uint32_t rate, int sink)
{
    AXQuit();
    g_cap.clear();
    re4snd_soft_config(sink, rate);
    re4snd_soft_set_capture(sink == RE4SND_SINK_CAPTURE ? cap_fn : 0, 0);
    AIInit(NULL);
    AXInitEx(1);
    MIXInit();
    AXARTInit();
    SYNInit();
    SEQInit();
    g_frames_cb = 0;
    AXRegisterCallback(cb_frame);
}

// ---- tests -----------------------------------------------------------------------------------

static void test_oneshot(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    Sample s = put_tone(0x10000, 32000, 1000.0, 16000.0, 0);   // 1 s, 1 kHz
    AXVPB* v = start_voice(s, 0, 0, 0, 1.0f, 0, -960, 64);
    CHECK(v != 0, "acquire");
    CHECK(v->pb.state == 1, "pb.state after start");
    uint32_t end = *(u32*) &v->pb.addr.endAddressHi;   // as snd_iss2.cpp:150 reads it
    uint32_t prev = *(u32*) &v->pb.addr.currentAddressHi;
    int frames = 0, mono_ok = 1;
    while (v->pb.state != 0 && frames < 400) {
        re4ax_run_frames(1);
        frames++;
        uint32_t cur = *(u32*) &v->pb.addr.currentAddressHi;
        if (v->pb.state && (cur < prev || cur > end + 1)) mono_ok = 0;
        prev = cur;
    }
    CHECK(mono_ok, "currentAddress (u32 read like iss2/str2) runs monotonic up to endAddress");
    CHECK(v->pb.addr.loopFlag == 0, "pb.addr.loopFlag mirror");
    CHECK(frames >= 200 && frames <= 202, "one-shot 32000 samples ends after %d frames (want 200..202)", frames);
    re4ax_run_frames(20);
    write_wav("ct10_oneshot_32k.wav", g_cap, 32000);
    double r = rms(g_cap, 1600, 30400, 0), rr = rms(g_cap, 1600, 30400, 1);
    double want = 16000.0 / sqrt(2.0) * pow(10.0, -30.0 / 200.0);   // sine RMS, -3 dB centre pan
    CHECK(fabs(r - want) < want * 0.05 && fabs(rr - want) < want * 0.05, "level L %.0f R %.0f want %.0f", r, rr, want);
    double f = freq(g_cap, 1600, 30400, 32000);
    CHECK(fabs(f - 1000.0) < 5.0, "pitch %.1f Hz want 1000", f);
    CHECK(rms(g_cap, 32100, 32000 + 20 * 160, 0) < 1.0, "silence after the end");
    CHECK(g_frames_cb == frames + 20, "AX frame callback ran every frame (%d vs %d)", g_frames_cb, frames + 20);
    AXFreeVoice(v);
    printf("oneshot: %d frames, L rms %.0f, %.1f Hz\n", frames, r, f);
}

static void test_loop_ratio(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    Sample s = put_tone(0x20000, 3200, 1000.0, 16000.0, 0);   // 100 periods, loops seamlessly
    AXVPB* v = start_voice(s, 1, 0, 3200, 1.0f, 0, -960, 0); // pan hard left
    AXSetVoiceSrcRatio(v, 1.5f);
    re4ax_run_frames(200);   // 1 s, 15 loop iterations
    CHECK(v->pb.state == 1, "looping voice still running");
    double l = rms(g_cap, 800, 32000, 0), r = rms(g_cap, 800, 32000, 1);
    double want = 16000.0 / sqrt(2.0);
    CHECK(fabs(l - want) < want * 0.05, "loop level L %.0f want %.0f", l, want);
    CHECK(r < 2.0, "hard-left pan: R %.1f", r);
    double f = freq(g_cap, 800, 32000, 32000);
    CHECK(fabs(f - 1500.0) < 10.0, "ratio 1.5 pitch %.1f Hz want 1500", f);
    // level must not dip at the loop points: check 10 ms windows
    double mn = 1e9;
    for (size_t w = 800; w + 320 <= 32000; w += 320) { double x = rms(g_cap, w, w + 320, 0); if (x < mn) mn = x; }
    CHECK(mn > want * 0.95, "no dropout at loop points (min window rms %.0f)", mn);
    AXSetVoiceState(v, 0);
    CHECK(v->pb.state == 0, "stop");
    size_t at = g_cap.size() / 2;
    re4ax_run_frames(10);
    CHECK(rms(g_cap, at + 2, at + 1600, 0) < 1.0, "silent after AXSetVoiceState(0)");
    write_wav("ct10_loop_ratio15.wav", g_cap, 32000);
    printf("loop: L rms %.0f, %.1f Hz, min window %.0f\n", l, f, mn);
    AXFreeVoice(v);
}

static void test_mix_ramp(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    Sample s = put_tone(0x20000, 3200, 1000.0, 16000.0, 0);
    AXVPB* v = start_voice(s, 1, 0, 3200, 1.0f, 0, -960, 64);
    re4ax_run_frames(20);
    double a = rms(g_cap, 1600, 3200, 0);
    MIXSetInput(v, -60);    // -6 dB, applied at the next MIXUpdateSettings (cb_frame), ramped
    re4ax_run_frames(20);
    size_t n = g_cap.size() / 2;
    double b = rms(g_cap, n - 1600, n, 0);
    CHECK(fabs(20 * log10(b / a) + 6.0) < 0.3, "MIXSetInput -6 dB -> %.2f dB", 20 * log10(b / a));
    CHECK(MIXGetInput(v) == -60, "MIXGetInput");
    MIXSetPan(v, 127);
    re4ax_run_frames(4);
    n = g_cap.size() / 2;
    CHECK(rms(g_cap, n - 320, n, 0) < 2.0 && rms(g_cap, n - 320, n, 1) > 1000.0, "MIXSetPan 127 = right");
    MIXReleaseChannel(v);
    AXFreeVoice(v);
}

static void* fx_alloc(u32 n) { return malloc(n); }
static void fx_free(void* p) { free(p); }

static void test_aux_reverb(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    Sample s = put_tone(0x30000, 3200, 440.0, 12000.0, 0);
    g_aux_calls = 0;
    g_aux_energy = 0;
    AXRegisterAuxACallback(cb_aux, 0);
    AXVPB* v = start_voice(s, 0, 0, 0, 1.0f, 0, 0, 64);   // AUX A at 0 dB
    re4ax_run_frames(30);
    CHECK(g_aux_calls == 30, "AUX A callback per frame (%d)", g_aux_calls);
    CHECK(g_aux_energy > 1e9, "AUX A bus carries the voice (%g)", g_aux_energy);
    AXRegisterAuxACallback(0, 0);
    AXFreeVoice(v);
    re4ax_run_frames(5);

    // AXFX reverb on AUX A, as snd_efx.cpp sets it up
    static AXFX_REVERBSTD rev;
    memset(&rev, 0, sizeof rev);
    rev.time = 1.5f; rev.preDelay = 0.02f; rev.damping = 0.5f; rev.coloration = 0.5f; rev.mix = 0.8f;
    AXFXSetHooks(fx_alloc, fx_free);
    CHECK(AXFXReverbStdInit(&rev) == 1, "AXFXReverbStdInit returns 1");
    AXRegisterAuxACallback((void (*)(void*, void*)) AXFXReverbStdCallback, &rev);
    g_cap.clear();
    v = start_voice(s, 0, 0, 0, 1.0f, 0, 0, 64);
    MIXInitChannel(v, 1, 0, 0, -960, 64, 127, -960);   // mode 1 = AUX A pre-fader; fader -inf: no dry signal
    re4ax_run_frames(20);   // 0.1 s of tone (3200 samples)
    AXSetVoiceState(v, 0);
    re4ax_run_frames(100);  // 0.5 s tail
    write_wav("ct10_reverb_tail.wav", g_cap, 32000);
    double early = rms(g_cap, 3200 + 1600, 3200 + 4800, 0), late = rms(g_cap, 3200 + 12800, 3200 + 16000, 0);
    CHECK(early > 50.0, "reverb tail after the voice stopped (%.1f)", early);
    CHECK(late < early, "tail decays (%.1f -> %.1f)", early, late);
    CHECK(AXFXReverbStdSettings(&rev) == 1, "settings");
    CHECK(AXFXReverbStdShutdown(&rev) == 1, "shutdown");
    AXRegisterAuxACallback(0, 0);
    AXFreeVoice(v);
    printf("reverb: tail rms %.1f -> %.1f\n", early, late);

    // the other effects initialise and run without touching memory they do not own
    static AXFX_REVERBHI hi;
    static AXFX_CHORUS ch;
    static AXFX_DELAY dl;
    memset(&hi, 0, sizeof hi); hi.time = 2; hi.mix = 0.5f; hi.crosstalk = 0.1f;
    memset(&ch, 0, sizeof ch); ch.baseDelay = 15; ch.variation = 0; ch.period = 500;
    memset(&dl, 0, sizeof dl); for (int k = 0; k < 3; k++) { dl.delay[k] = 100; dl.feedback[k] = 50; dl.output[k] = 100; }
    CHECK(AXFXReverbHiInit(&hi) == 1 && AXFXChorusInit(&ch) == 1 && AXFXDelayInit(&dl) == 1, "hi/chorus/delay init");
    static s32 L[160], R[160], S[160];
    for (int i = 0; i < 160; i++) L[i] = R[i] = S[i] = i == 0 ? 10000 : 0;
    AXFX_BUFFERUPDATE bu = { L, R, S };
    AXFXDelayCallback(&bu, &dl);
    int ok = 1;
    for (int i = 0; i < 160; i++) if (L[i]) ok = 0;   // 100 ms delay: nothing yet in the first 5 ms
    for (int f = 0; f < 20; f++) { memset(L, 0, sizeof L); AXFXDelayCallback(&bu, &dl); if (f == 19) { int any = 0; for (int i = 0; i < 160; i++) any |= L[i] != 0; ok &= any; } }
    CHECK(ok, "delay: the impulse comes back after 100 ms (frame 20)");
    AXFXReverbHiCallback(&bu, &hi);
    AXFXChorusCallback(&bu, &ch);
    CHECK(AXFXReverbHiShutdown(&hi) == 1 && AXFXChorusShutdown(&ch) == 1 && AXFXDelayShutdown(&dl) == 1, "shutdowns");
}

static void test_48k(void)
{
    reset_ax(48000, RE4SND_SINK_CAPTURE);
    Sample s = put_tone(0x10000, 32000, 1000.0, 16000.0, 0);
    AXVPB* v = start_voice(s, 0, 0, 0, 1.0f, 0, -960, 64);
    re4ax_run_frames(100);
    CHECK(g_cap.size() == 100 * 240 * 2, "48 kHz: 240 frames per AX frame (%u)", (unsigned) g_cap.size());
    double f = freq(g_cap, 2400, 24000, 48000);
    CHECK(fabs(f - 1000.0) < 5.0, "48 kHz pitch %.1f", f);
    write_wav("ct10_oneshot_48k.wav", g_cap, 48000);
    AXFreeVoice(v);
}

static void test_steal(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    AXVPB* vs[64];
    for (int i = 0; i < 64; i++) vs[i] = AXAcquireVoice(10 + (i == 37 ? -5 : 0), cb_drop, 0);
    int all = 1;
    for (int i = 0; i < 64; i++) all &= vs[i] != 0;
    CHECK(all, "64 voices");
    g_drops = 0;
    g_dropped = 0;
    AXVPB* x = AXAcquireVoice(4, cb_drop, 0);
    CHECK(x == 0 && g_drops == 0, "no voice below priority 4");
    x = AXAcquireVoice(30, cb_drop, 0);
    CHECK(x == vs[37] && g_drops == 1 && g_dropped == vs[37], "lowest priority (5) voice stolen with its callback");
    AXSetVoicePriority(vs[3], 1);
    x = AXAcquireVoice(30, cb_drop, 0);
    CHECK(x == vs[3] && g_drops == 2, "AXSetVoicePriority(1) makes it the next victim");
    for (int i = 0; i < 64; i++) AXFreeVoice(vs[i]);
}

static void test_pump(void)
{
    reset_ax(32000, RE4SND_SINK_NULL);
    CHECK(re4snd_soft_sink() == RE4SND_SINK_NULL, "null sink");
    re4_audio_pump();
    CHECK(g_frames_cb == 10, "first pump primes 10 frames (%d)", g_frames_cb);
    sleep_ms(50);
    int before = g_frames_cb;
    re4_audio_pump();
    int d = g_frames_cb - before;
    CHECK(d >= 9 && d <= RE4AX_MAX_CATCHUP, "50 ms -> %d frames", d);
    sleep_ms(300);
    before = g_frames_cb;
    re4_audio_pump();
    double t_stall = now_ms();
    d = g_frames_cb - before;
    CHECK(d == RE4AX_MAX_CATCHUP, "300 ms stall capped at %d frames (%d)", RE4AX_MAX_CATCHUP, d);
    sleep_ms(20);
    double el = now_ms() - t_stall;
    before = g_frames_cb;
    re4_audio_pump();
    d = g_frames_cb - before;
    CHECK(d >= (int) (el / 5.0) - 1 && d <= (int) (el / 5.0) + 1, "backlog dropped: %.1f ms after the stall -> %d frames", el, d);
    printf("pump: frames run %d\n", g_frames_cb);
}

#if defined(RE4_GL)
// SDL sink: needs SDL2 (run with SDL_AUDIODRIVER=dummy on a headless machine)
static void test_sdl(void)
{
    reset_ax(32000, RE4SND_SINK_SDL);
    CHECK(re4snd_soft_sink() == RE4SND_SINK_SDL, "SDL sink opened");
    Sample s = put_tone(0x20000, 3200, 1000.0, 8000.0, 0);
    AXVPB* v = start_voice(s, 1, 0, 3200, 1.0f, 0, -960, 64);
    double t0 = now_ms();
    while (now_ms() - t0 < 500.0) { re4_audio_pump(); sleep_ms(16); }
    double el = now_ms() - t0;
    // 50 ms queued ahead + elapsed / 5 ms
    CHECK(g_frames_cb >= (int) (el / 5.0) - 4 && g_frames_cb <= (int) (el / 5.0) + 14,
          "SDL paced %d frames in %.0f ms", g_frames_cb, el);
    printf("sdl: %d frames in %.0f ms\n", g_frames_cb, el);
    AXFreeVoice(v);
    AXQuit();
}
#endif

static void test_sdk_misc(void)
{
    reset_ax(32000, RE4SND_SINK_CAPTURE);
    AXSetMode(2);
    MIXSetSoundMode(3);
    CHECK(AXGetMode() == 2 && MIXGetSoundMode() == 3, "modes");
    AXSetCompressor(1);
    CHECK(AXGetMaxDspCycles() > 0 && AXGetDspCycles() > 0, "dsp cycles");
    static SYNSYNTH syn;
    SYNInitSynth(&syn, 0, 0x100000, 0, 30, 30, 1);
    u8 on[3] = { 0x90, 60, 100 }, cc[3] = { 0xB0, 7, 90 };
    SYNMidiInput(&syn, on);
    SYNMidiInput(&syn, cc);
    SYNSetMasterVolume(&syn, -120);
    CHECK(SYNGetActiveNotes(&syn) == 0, "SYN stub: no active notes (snd_seq3 closes the sequence)");
    CHECK(SYNGetMidiController(&syn, 0, 7) == 90 && SYNGetMasterVolume(&syn) == -120, "SYN controller state");
    SYNQuitSynth(&syn);
    SEQRunAudioFrame();
    SYNRunAudioFrame();
    AXARTServiceSounds();
    // DPL2 render path with a voice on AUX A 4-channel
    Sample s = put_tone(0x20000, 3200, 1000.0, 16000.0, 0);
    AXVPB* v = start_voice(s, 1, 0, 3200, 1.0f, 0, -960, 64);
    re4ax_run_frames(10);
    CHECK(rms(g_cap, 800, 1600, 0) > 1000.0, "DPL2 + compressor renders");
    AXFreeVoice(v);
    AXSetMode(0);
    MIXSetSoundMode(1);
    AXSetCompressor(0);
    AXQuit();
    MIXQuit();
    SEQQuit();
    SYNQuit();
    AXARTQuit();
    AIReset();
}

#undef main   // re4_xbox.h renames the game's main
int main(void)
{
    re4_aram_base = (unsigned char*) calloc(1, RE4_ARAM_SIZE);
    if (!re4_aram_base) return 2;
    test_oneshot();
    test_loop_ratio();
    test_mix_ramp();
    test_aux_reverb();
    test_48k();
    test_steal();
    test_pump();
#if defined(RE4_GL)
    test_sdl();
#endif
    test_sdk_misc();
    printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
