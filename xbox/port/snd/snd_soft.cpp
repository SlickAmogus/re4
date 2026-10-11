// snd_soft.cpp — software AX backend (CT-10): mixes the AX voice model (re4port/ax_shim.h) on the CPU.
//
// Per 5 ms AX frame (160 samples at 32 kHz, the DSP's rate and the AUX callbacks' buffer size —
// cb_efx_clear_bass clears 0x280 bytes = 160 s32, snd_efx.cpp:209):
//   1. every running voice: fetch source samples from ARAM (re4_aram_base; AX addresses are nibbles
//      for DSP-ADPCM via dspadpcm.c, words for PCM16 big-endian, bytes for PCM8), loop / end handling
//      (end inclusive; type 1 "stream" voices keep their ADPCM history on a wrap), linear SRC with the
//      16.16 ratio (SRC type NONE = 1:1), the one-pole LPF, the ve gain and the nine AXPBMIX gains,
//      all ramped across the frame from the previous frame's values;
//   2. the AUX A / B buses go through the registered AUX callbacks (game: AXFX effects) and are added
//      to the main bus; a bus without a callback is discarded, as on the DSP;
//   3. main L/R/S (Ls/Rs in DPL2) -> stereo (in-phase downmix; Dolby Surround matrix in surround
//      mode), master gain, AXSetCompressor limiter, s16, optional 32 -> 48 kHz;
//   4. the frame goes to the sink.
//
// Sinks:
//   DSOUND   Xbox with RE4_AUDIO_RXDK (XDK DirectSound objects linked): one looping 2D PCM buffer
//            routed to FL/FR mixbins, written ahead of the play cursor, DirectSoundDoWork() per pump.
//            Init sequence follows jfduke3d-xbox driver_dsound_xbox.c (APU reset, DirectSound global
//            critical section, speaker config globals, DirectSoundCreate). No XAudioInit.
//   SDL      host RE4_GL: SDL queued audio (no callback thread, no locks).
//   NULL     anything else: discards, paced by the wall clock so the game's audio frames still run.
//   CAPTURE  tests: frames_wanted = 0, frames are produced by re4ax_run_frames().
// Every sink also hands its output to the capture callback when one is set.
#include "re4port/re4_xbox.h"
#include "re4port/ax_shim.h"
#include "dspadpcm.h"
#include <string.h>
#include <math.h>

#if defined(RE4_XBOX)
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#if defined(RE4_AUDIO_RXDK)
#include <dsound.h>
#define RE4SND_HAVE_DSOUND 1
#endif
#else
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>   // QueryPerformanceCounter
#else
#include <time.h>      // clock_gettime
#endif
#if defined(RE4_GL)
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED 1   // no main here; do not let SDL_main.h rename anything
#endif
#include <SDL.h>
#define RE4SND_HAVE_SDL 1
#endif
#endif

#ifndef RE4SND_TARGET_FRAMES
#define RE4SND_TARGET_FRAMES 10   // output queued ahead of the hardware: 10 AX frames = 50 ms
#endif
#ifndef RE4_DS_SPEAKER_CONFIG
#define RE4_DS_SPEAKER_CONFIG (DSSPEAKER_ENABLE_AC3 | DSSPEAKER_SURROUND)   // as jfduke3d-xbox
#endif

extern "C" {

#define N RE4AX_FRAME_SAMPLES
#define MAX_STEP 0x80000u              // 8x
#define MAX_SRC  (N * 8u + 2u)

static int      s_sink_req = RE4SND_SINK_AUTO;
static int      s_sink = RE4SND_SINK_NULL;
static uint32_t s_out_rate = 32000;
static float    s_gain = -1.0f;        // < 0: sink default
static Re4SndCaptureFn s_cap_fn;
static void*    s_cap_ctx;

static int32_t  s_main[4][N];          // L, R, S (Ls), Rs
static int32_t  s_aux[2][4][N];        // AUX A / B: L, R, S (Ls), Rs (DPL2 AUX A only)
static int16_t  s_src[MAX_SRC];
static int16_t  s_out[N * 3 / 2 * 2 + 4];   // up to 240 stereo frames
static int16_t  s_prev_l, s_prev_r;   // 48 kHz resampler history
static float    s_comp_gain = 1.0f;
static int      s_open;

void re4snd_soft_config(int sink, uint32_t out_rate)
{
    s_sink_req = sink;
    s_out_rate = out_rate == 48000 ? 48000 : 32000;
}
void re4snd_soft_set_capture(Re4SndCaptureFn fn, void* ctx) { s_cap_fn = fn; s_cap_ctx = ctx; }
void re4snd_soft_set_gain(float gain) { s_gain = gain; }
uint32_t re4snd_soft_out_rate(void) { return s_out_rate; }
int re4snd_soft_sink(void) { return s_sink; }

static inline uint32_t out_frame_samples(void) { return s_out_rate / 200u; }      // per 5 ms
[[maybe_unused]] static inline uint32_t out_frame_bytes(void) { return out_frame_samples() * 4u; }  // s16 stereo

// ================================================================================================
// sinks
// ================================================================================================

// ---- wall clock (null sink) ----
static uint64_t now_us(void)
{
#if defined(RE4_XBOX)
    static uint64_t freq;
    if (!freq) freq = (uint64_t) KeQueryPerformanceFrequency();
    uint64_t c = (uint64_t) KeQueryPerformanceCounter();
    return (c / freq) * 1000000ull + (c % freq) * 1000000ull / freq;
#elif defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    uint64_t f = (uint64_t) freq.QuadPart, n = (uint64_t) c.QuadPart;
    return (n / f) * 1000000ull + (n % f) * 1000000ull / f;
#else
    struct timespec ts;   // (no <chrono>: the host MSVC STL rejects the installed clang)
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
#endif
}

static uint64_t s_null_last;
static uint64_t s_null_acc;

static uint32_t null_wanted(void)
{
    uint64_t t = now_us();
    if (!s_null_last) {
        s_null_last = t;
        return RE4SND_TARGET_FRAMES;   // prime like a real buffer
    }
    if (t > s_null_last) s_null_acc += t - s_null_last;
    s_null_last = t;
    uint64_t n = s_null_acc / RE4AX_FRAME_US;
    s_null_acc -= n * RE4AX_FRAME_US;  // a backlog beyond the shim's catch-up cap is dropped here
    return (uint32_t) (n > 0xFFFF ? 0xFFFF : n);
}

// ---- SDL (host) ----
#if defined(RE4SND_HAVE_SDL)
static SDL_AudioDeviceID s_sdl_dev;

static int sdl_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        re4_log("snd_soft: SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = (int) s_out_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = NULL;   // SDL_QueueAudio
    s_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!s_sdl_dev) {
        re4_log("snd_soft: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(s_sdl_dev, 0);
    re4_log("snd_soft: SDL audio %d Hz\n", have.freq);
    return 1;
}
static void sdl_close(void)
{
    if (s_sdl_dev) SDL_CloseAudioDevice(s_sdl_dev);
    s_sdl_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
static uint32_t sdl_wanted(void)
{
    uint32_t q = SDL_GetQueuedAudioSize(s_sdl_dev), fb = out_frame_bytes();
    uint32_t target = RE4SND_TARGET_FRAMES * fb;
    return q >= target ? 0 : (target - q + fb - 1) / fb;
}
static void sdl_push(const int16_t* lr, uint32_t frames) { SDL_QueueAudio(s_sdl_dev, lr, frames * 4u); }
#endif

// ---- XDK DirectSound (Xbox) ----
#if defined(RE4SND_HAVE_DSOUND)
EXTERN_C CRITICAL_SECTION g_DirectSoundCriticalSection;   // globals.obj
EXTERN_C DWORD g_dwDirectSoundOverrideSpeakerConfig;
EXTERN_C DWORD g_dwDirectSoundSpeakerConfig;

#define DS_FRAMES 32                   // ring = 32 AX frames (160 ms)
#define DS_GUARD_FRAMES 8              // silence written past the data (a stall plays silence, not old audio)
static IDirectSound*       s_ds;
static IDirectSoundBuffer* s_dsbuf;
static DWORD s_ds_size;
static DWORD s_ds_wpos;
static int   s_ds_primed;

static void ds_fill_zero(DWORD pos, DWORD bytes)
{
    LPVOID p1 = NULL, p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    if (FAILED(IDirectSoundBuffer_Lock(s_dsbuf, pos, bytes, &p1, &b1, &p2, &b2, 0))) return;
    if (p1) memset(p1, 0, b1);
    if (p2) memset(p2, 0, b2);
    IDirectSoundBuffer_Unlock(s_dsbuf, p1, b1, p2, b2);
}

static int ds_open(void)
{
    HRESULT hr;
    // APU to a clean state before DirectSoundCreate (the kernel leaves it half-configured; jfduke3d)
    {
        volatile unsigned long* apu = (volatile unsigned long*) 0xFE800000u;
        apu[0x1004 / 4] = 0;           // NV_PAPU_IEN
        apu[0x2000 / 4] = 0;           // NV_PAPU_SECTL
        apu[0x1100 / 4] = 0;           // NV_PAPU_FECTL
        apu[0x1000 / 4] = 0xFFFFFFFF;  // NV_PAPU_ISTS (write-1-to-clear)
        KeStallExecutionProcessor(100);
    }
    // dsound.lib expects its DLL init to have set this up; we link the .obj files directly
    RtlInitializeCriticalSection(&g_DirectSoundCriticalSection);
    DirectSoundOverrideSpeakerConfig(RE4_DS_SPEAKER_CONFIG);
    g_dwDirectSoundOverrideSpeakerConfig = RE4_DS_SPEAKER_CONFIG;
    g_dwDirectSoundSpeakerConfig = RE4_DS_SPEAKER_CONFIG;

    hr = DirectSoundCreate(NULL, &s_ds, NULL);
    if (FAILED(hr)) {
        re4_log("snd_soft: DirectSoundCreate failed hr=0x%08X\n", (unsigned) hr);
        s_ds = NULL;
        return 0;
    }

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = s_out_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = s_out_rate * 4;

    // FL/FR only; the pairs must stay in scope through CreateSoundBuffer
    DSMIXBINVOLUMEPAIR pairs[2];
    DSMIXBINS bins;
    pairs[0].dwMixBin = DSMIXBIN_FRONT_LEFT;
    pairs[0].lVolume = 0;
    pairs[1].dwMixBin = DSMIXBIN_FRONT_RIGHT;
    pairs[1].lVolume = 0;
    bins.dwMixBinCount = 2;
    bins.lpMixBinVolumePairs = pairs;

    s_ds_size = DS_FRAMES * out_frame_bytes();
    DSBUFFERDESC dsbd;
    memset(&dsbd, 0, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = 0;                  // 2D
    dsbd.dwBufferBytes = s_ds_size;
    dsbd.lpwfxFormat = &wfx;
    dsbd.lpMixBins = &bins;
    dsbd.dwInputMixBin = 0;
    hr = IDirectSound_CreateSoundBuffer(s_ds, &dsbd, &s_dsbuf, NULL);
    if (FAILED(hr)) {
        re4_log("snd_soft: CreateSoundBuffer failed hr=0x%08X\n", (unsigned) hr);
        IDirectSound_Release(s_ds);
        s_ds = NULL;
        s_dsbuf = NULL;
        return 0;
    }
    {
        LPVOID p1;
        DWORD b1;
        if (SUCCEEDED(IDirectSoundBuffer_Lock(s_dsbuf, 0, s_ds_size, &p1, &b1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
            memset(p1, 0, b1);
            IDirectSoundBuffer_Unlock(s_dsbuf, p1, b1, NULL, 0);
        }
    }
    hr = IDirectSoundBuffer_Play(s_dsbuf, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        re4_log("snd_soft: Play failed hr=0x%08X\n", (unsigned) hr);
        IDirectSoundBuffer_Release(s_dsbuf);
        IDirectSound_Release(s_ds);
        s_ds = NULL;
        s_dsbuf = NULL;
        return 0;
    }
    s_ds_wpos = 0;
    s_ds_primed = 0;
    re4_log("snd_soft: DirectSound %u Hz, %u-byte ring\n", (unsigned) s_out_rate, (unsigned) s_ds_size);
    return 1;
}

static void ds_close(void)
{
    if (s_dsbuf) {
        IDirectSoundBuffer_Stop(s_dsbuf);
        IDirectSoundBuffer_Release(s_dsbuf);
    }
    if (s_ds) IDirectSound_Release(s_ds);
    s_dsbuf = NULL;
    s_ds = NULL;
}

static uint32_t ds_wanted(void)
{
    DWORD play = 0, wc = 0;
    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(s_dsbuf, &play, &wc))) return 0;
    if (!s_ds_primed) {
        s_ds_wpos = wc;   // the first safe write position
        s_ds_primed = 1;
    }
    DWORD queued = (s_ds_wpos + s_ds_size - play) % s_ds_size;
    if (queued > s_ds_size / 2) {
        // the play cursor overtook us (the game stalled): restart just ahead of the hardware
        s_ds_wpos = wc;
        queued = (s_ds_wpos + s_ds_size - play) % s_ds_size;
    }
    DWORD fb = out_frame_bytes(), target = RE4SND_TARGET_FRAMES * fb;
    return queued >= target ? 0 : (uint32_t) ((target - queued + fb - 1) / fb);
}

static void ds_push(const int16_t* lr, uint32_t frames)
{
    DWORD bytes = frames * 4u;
    LPVOID p1 = NULL, p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    if (FAILED(IDirectSoundBuffer_Lock(s_dsbuf, s_ds_wpos, bytes, &p1, &b1, &p2, &b2, 0))) return;
    if (p1) memcpy(p1, lr, b1);
    if (p2) memcpy(p2, (const uint8_t*) lr + b1, b2);
    IDirectSoundBuffer_Unlock(s_dsbuf, p1, b1, p2, b2);
    s_ds_wpos = (s_ds_wpos + bytes) % s_ds_size;
}

static void ds_pump_end(void)
{
    if (s_dsbuf) ds_fill_zero(s_ds_wpos, DS_GUARD_FRAMES * out_frame_bytes());
    DirectSoundDoWork();   // drives the VP -> GP -> EP pipeline; required every pump
}
#endif

// ================================================================================================
// voice rendering
// ================================================================================================

static inline uint32_t aram_limit(uint16_t fmt)
{
    return fmt == RE4AX_FMT_PCM16 ? RE4_ARAM_SIZE / 2u : (fmt == RE4AX_FMT_PCM8 ? RE4_ARAM_SIZE : RE4_ARAM_SIZE * 2u);
}

// end of data: loop (returns 1) or stop (ended, returns 0)
static int wrap(Re4AxVoiceState* v, DspAdpcmState* st)
{
    if (!v->loop_flag) {
        v->ended = 1;
        return 0;
    }
    v->cur = v->loop;
    if (v->format == RE4AX_FMT_ADPCM && st) {
        if (v->type == 0) {
            st->pred_scale = (uint8_t) v->loop_ps;
            st->yn1 = v->loop_yn1;
            st->yn2 = v->loop_yn2;
        } else {
            // stream voice: history continues across the ring wrap; the frame header comes from ARAM
            dspadpcm_seek(st, re4_aram_base, v->loop);
        }
    }
    return 1;
}

// Fetches `need` source samples; zero-fills after a one-shot ended. Returns samples really read.
static uint32_t fetch(Re4AxVoiceState* v, uint32_t need, int16_t* out)
{
    uint32_t done = 0, lim = aram_limit(v->format), wraps = 0;
    const uint8_t* base = re4_aram_base;
    if (!base || v->end >= lim || v->cur >= lim || v->loop >= lim) {
        re4_log_once("ax-range", "snd_soft: voice address out of ARAM (cur %08x end %08x loop %08x fmt %x)\n",
                     (unsigned) v->cur, (unsigned) v->end, (unsigned) v->loop, (unsigned) v->format);
        v->ended = 1;
        memset(out, 0, need * sizeof(int16_t));
        return 0;
    }
    if (v->format == RE4AX_FMT_ADPCM) {
        DspAdpcmState st;
        memcpy(st.coef, v->coef, sizeof(st.coef));
        st.pred_scale = (uint8_t) v->ps;
        st.yn1 = v->yn1;
        st.yn2 = v->yn2;
        while (done < need && !v->ended) {
            uint32_t sc = dspadpcm_nibble_to_sample(v->cur), se = dspadpcm_nibble_to_sample(v->end);
            uint32_t avail = se >= sc ? se - sc + 1u : 0u;
            uint32_t n = need - done < avail ? need - done : avail;
            if (n) {
                v->cur = dspadpcm_decode(&st, base, v->cur, n, out + done);
                done += n;
            }
            if (n == avail) {
                if (!wrap(v, &st) || ++wraps > need + 1u) break;   // (a degenerate loop cannot spin)
            }
        }
        v->ps = st.pred_scale;
        v->yn1 = st.yn1;
        v->yn2 = st.yn2;
    } else {
        while (done < need && !v->ended) {
            if (v->cur > v->end) {
                if (!wrap(v, 0) || ++wraps > need + 1u) break;
                continue;
            }
            uint32_t avail = v->end - v->cur + 1u;
            uint32_t n = need - done < avail ? need - done : avail;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t a = v->cur + k;
                if (v->format == RE4AX_FMT_PCM16)
                    out[done + k] = (int16_t) ((base[a * 2u] << 8) | base[a * 2u + 1u]);   // big-endian ARAM
                else
                    out[done + k] = (int16_t) ((int8_t) base[a] * 256);
            }
            v->cur += n;
            done += n;
            if (n == avail) {
                if (!wrap(v, 0) || ++wraps > need + 1u) break;
            }
        }
    }
    if (done < need) memset(out + done, 0, (need - done) * sizeof(int16_t));
    return done;
}

static void render_voice(Re4AxVoiceState* v, int dpl2)
{
    uint32_t step = v->src_type == 0 ? 0x10000u : v->ratio;   // AX_SRC_TYPE_NONE plays 1:1
    if (step > MAX_STEP) step = MAX_STEP;
    uint32_t frac = v->frac & 0xFFFFu;
    uint32_t need = (uint32_t) (((uint64_t) frac + (uint64_t) step * N) >> 16);
    if (need > MAX_SRC) need = MAX_SRC;
    fetch(v, need, s_src);

    // gains, Q15, ramped from the previous frame's values; fixed point Q16 accumulators
    const uint16_t* g1 = v->vol;
    const uint16_t* g0 = v->vol_primed ? v->vol_prev : v->vol;
    int32_t* dst[RE4AX_BIN_COUNT];
    for (int b = 0; b < RE4AX_BIN_COUNT; b++) dst[b] = 0;
    dst[RE4AX_BIN_L] = s_main[0];
    dst[RE4AX_BIN_R] = s_main[1];
    dst[RE4AX_BIN_AL] = s_aux[0][0];
    dst[RE4AX_BIN_AR] = s_aux[0][1];
    dst[RE4AX_BIN_AS] = s_aux[0][2];
    if (dpl2) {
        dst[RE4AX_BIN_BL] = s_main[2];   // Ls
        dst[RE4AX_BIN_BR] = s_main[3];   // Rs
        dst[RE4AX_BIN_BS] = s_aux[0][3]; // AUX A Rs
    } else {
        dst[RE4AX_BIN_S] = s_main[2];
        dst[RE4AX_BIN_BL] = s_aux[1][0];
        dst[RE4AX_BIN_BR] = s_aux[1][1];
        dst[RE4AX_BIN_BS] = s_aux[1][2];
    }
    int nb = 0, bi[RE4AX_BIN_COUNT];
    int32_t gq[RE4AX_BIN_COUNT], dg[RE4AX_BIN_COUNT];
    for (int b = 0; b < RE4AX_BIN_COUNT; b++) {
        if (!dst[b] || (!g0[b] && !g1[b])) continue;
        bi[nb] = b;
        gq[nb] = (int32_t) g0[b] << 15;                                   // Q30
        dg[nb] = (((int32_t) g1[b] - (int32_t) g0[b]) << 15) / (int32_t) N;
        nb++;
    }
    uint16_t ve0 = v->vol_primed ? v->ve_prev : v->ve;
    int32_t veq = (int32_t) ve0 << 15, dve = (((int32_t) v->ve - (int32_t) ve0) << 15) / (int32_t) N;

    int32_t a = v->hist[2], b = v->hist[3];
    uint32_t si = 0;
    int32_t y1 = v->lpf_yn1;
    int lpf = v->lpf_on;
    int32_t la0 = v->lpf_a0, lb0 = v->lpf_b0;
    for (uint32_t k = 0; k < N; k++) {
        int32_t x = a + (int32_t) (((int64_t) (b - a) * (int32_t) frac) >> 16);
        frac += step;
        while (frac >= 0x10000u) {
            frac -= 0x10000u;
            a = b;
            b = si < need ? s_src[si] : 0;
            si++;
        }
        if (lpf) {
            x = (la0 * x + lb0 * y1) >> 15;
            y1 = x;
        }
        int32_t s = (int32_t) (((int64_t) x * (veq >> 15)) >> 15);
        veq += dve;
        for (int j = 0; j < nb; j++) {
            dst[bi[j]][k] += (int32_t) (((int64_t) s * (gq[j] >> 15)) >> 15);
            gq[j] += dg[j];
        }
    }
    v->frac = frac;
    v->hist[0] = v->hist[2];
    v->hist[1] = v->hist[3];
    v->hist[2] = (int16_t) a;
    v->hist[3] = (int16_t) b;
    v->lpf_yn1 = y1;
}

static inline int16_t clip16(int32_t x) { return (int16_t) (x > 32767 ? 32767 : (x < -32768 ? -32768 : x)); }

static void output_frame(void)
{
    const uint32_t mode = re4ax_mode();
    const float g = s_gain < 0.0f ? (s_sink == RE4SND_SINK_DSOUND ? 2.0f : 1.0f) : s_gain;
    const int comp = re4ax_compressor();
    int16_t lr[N * 2];
    for (uint32_t k = 0; k < N; k++) {
        float l = (float) s_main[0][k], r = (float) s_main[1][k];
        if (mode == RE4AX_MODE_DPL2) {
            l += 0.7071f * (float) s_main[2][k];
            r += 0.7071f * (float) s_main[3][k];
        } else if (mode == RE4AX_MODE_SURROUND) {
            l -= 0.7071f * (float) s_main[2][k];   // Dolby Surround matrix
            r += 0.7071f * (float) s_main[2][k];
        } else {
            l += 0.7071f * (float) s_main[2][k];
            r += 0.7071f * (float) s_main[2][k];
        }
        l *= g;
        r *= g;
        if (comp) {
            // AXSetCompressor(1): instant-attack peak limiter, ~50 ms release
            float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (pk * s_comp_gain > 30000.0f) s_comp_gain = 30000.0f / pk;
            else s_comp_gain += (1.0f - s_comp_gain) * (1.0f / 1600.0f);
            l *= s_comp_gain;
            r *= s_comp_gain;
        }
        lr[2 * k] = clip16((int32_t) l);
        lr[2 * k + 1] = clip16((int32_t) r);
    }
    const int16_t* out = lr;
    uint32_t frames = N;
    if (s_out_rate == 48000) {
        // 2:3 linear, one input sample of delay (s[-1] = last sample of the previous frame)
        for (uint32_t j = 0; j < N * 3u / 2u; j++) {
            uint32_t p3 = j * 2u, i = p3 / 3u, f = p3 % 3u;
            int32_t l0 = i ? lr[2 * (i - 1)] : s_prev_l, r0 = i ? lr[2 * (i - 1) + 1] : s_prev_r;
            int32_t l1 = lr[2 * i], r1 = lr[2 * i + 1];
            s_out[2 * j] = (int16_t) (l0 + (l1 - l0) * (int32_t) f / 3);
            s_out[2 * j + 1] = (int16_t) (r0 + (r1 - r0) * (int32_t) f / 3);
        }
        s_prev_l = lr[2 * (N - 1)];
        s_prev_r = lr[2 * (N - 1) + 1];
        out = s_out;
        frames = N * 3u / 2u;
    }
    switch (s_sink) {
#if defined(RE4SND_HAVE_SDL)
    case RE4SND_SINK_SDL: sdl_push(out, frames); break;
#endif
#if defined(RE4SND_HAVE_DSOUND)
    case RE4SND_SINK_DSOUND: ds_push(out, frames); break;
#endif
    default: break;
    }
    if (s_cap_fn) s_cap_fn(out, frames, s_cap_ctx);
}

static void soft_run_frame(void)
{
    const int dpl2 = re4ax_mode() == RE4AX_MODE_DPL2;
    memset(s_main, 0, sizeof(s_main));
    memset(s_aux, 0, sizeof(s_aux));
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) {
        Re4AxVoiceState* v = re4ax_voice(i);
        if (v->acquired && v->playing) render_voice(v, dpl2);
    }
    for (int bus = 0; bus < 2; bus++) {
        if (dpl2 && bus == 1) break;   // AUX B volumes carry the rear channels in DPL2
        int32_t (*x)[N] = s_aux[bus];
        if (!re4ax_call_aux(bus, x[0], x[1], x[2], x[3])) continue;
        int nch = dpl2 ? 4 : 3;
        for (int c = 0; c < nch; c++)
            for (uint32_t k = 0; k < N; k++) s_main[c][k] += x[c][k];
    }
    output_frame();
}

// ================================================================================================
// backend
// ================================================================================================

static int soft_open(void)
{
    int want = s_sink_req;
    if (want == RE4SND_SINK_AUTO) {
#if defined(RE4SND_HAVE_DSOUND)
        want = RE4SND_SINK_DSOUND;
#elif defined(RE4SND_HAVE_SDL)
        want = RE4SND_SINK_SDL;
#else
        want = RE4SND_SINK_NULL;
#endif
    }
    s_sink = RE4SND_SINK_NULL;
#if defined(RE4SND_HAVE_DSOUND)
    if (want == RE4SND_SINK_DSOUND && ds_open()) s_sink = RE4SND_SINK_DSOUND;
#endif
#if defined(RE4SND_HAVE_SDL)
    if (want == RE4SND_SINK_SDL && sdl_open()) s_sink = RE4SND_SINK_SDL;
#endif
    if (want == RE4SND_SINK_CAPTURE) s_sink = RE4SND_SINK_CAPTURE;
    if (s_sink == RE4SND_SINK_NULL && want != RE4SND_SINK_NULL)
        re4_log("snd_soft: output sink %d unavailable, using the null sink\n", want);
    s_null_last = 0;
    s_null_acc = 0;
    s_prev_l = s_prev_r = 0;
    s_comp_gain = 1.0f;
    s_open = 1;
    return 1;   // never fails: the null sink keeps the game's audio frames running
}

static void soft_close(void)
{
#if defined(RE4SND_HAVE_DSOUND)
    if (s_sink == RE4SND_SINK_DSOUND) ds_close();
#endif
#if defined(RE4SND_HAVE_SDL)
    if (s_sink == RE4SND_SINK_SDL) sdl_close();
#endif
    s_sink = RE4SND_SINK_NULL;
    s_open = 0;
}

static uint32_t soft_wanted(void)
{
    switch (s_sink) {
#if defined(RE4SND_HAVE_SDL)
    case RE4SND_SINK_SDL: return sdl_wanted();
#endif
#if defined(RE4SND_HAVE_DSOUND)
    case RE4SND_SINK_DSOUND: return ds_wanted();
#endif
    case RE4SND_SINK_CAPTURE: return 0;
    default: return null_wanted();
    }
}

static void soft_pump_end(void)
{
#if defined(RE4SND_HAVE_DSOUND)
    if (s_sink == RE4SND_SINK_DSOUND) ds_pump_end();
#endif
}

static const Re4AxBackend s_soft_backend = {
    "soft",
    soft_open,
    soft_close,
    0,              // voice_start: all state is in Re4AxVoiceState
    0,              // voice_stop
    0,              // voice_update
    0,              // set_aux: the callbacks are polled every frame
    soft_wanted,
    soft_run_frame,
    soft_pump_end,
};

const Re4AxBackend* re4snd_soft_backend(void) { return &s_soft_backend; }

} // extern "C"
