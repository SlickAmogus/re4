/* ax_shim.h — backend-neutral AX voice model behind the port's AX/MIX/AXFX/AXART/SYN/SEQ/AI shim
 * (xbox/port/snd/ax_shim.cpp; CT-10, xbox/docs/03-AUDIO.md "Implemented surface").
 *
 * The game (src/game/snd_*.cpp) programs Nintendo AX voices through the SDK calls declared in
 * include/dolphin/{ax,mix,axfx,axart,syn,seq,ai}.h. ax_shim.cpp implements those calls once; the
 * per-voice state the DSP would hold lives in Re4AxVoiceState (one per AXVPB, indexed by
 * AXVPB::index) and a backend turns it into sound:
 *
 *   - snd_soft.cpp  software mixer (DSP-ADPCM/PCM from ARAM, SRC, LPF, MIX gains, AUX buses, the
 *                   registered AUX callbacks / AXFX effects) writing 32 or 48 kHz stereo s16 into a
 *                   sink: one XDK DirectSound streaming buffer (Xbox, RE4_AUDIO_RXDK), SDL queued
 *                   audio (host RE4_GL), a null sink paced by the wall clock, or a capture sink
 *                   for tests.
 *   - (later) a hardware backend with one DirectSound buffer per voice (03-AUDIO.md section 8).
 *
 * The AX frame is 5 ms = 160 samples at 32 kHz. re4_audio_pump() (call it from VIWaitForRetrace)
 * runs as many frames as the sink needs (capped at RE4AX_MAX_CATCHUP); each frame renders the voices
 * with the current parameters, runs the AUX callbacks, refreshes the AXVPB fields the game reads
 * (pb.state, pb.addr.*) and then calls the AXRegisterCallback callback (the game's cb_audio_frame),
 * exactly the DSP-interrupt order. Everything runs on the calling thread (the port's OS threads are
 * cooperative fibers on one kernel thread), so there are no locks.
 *
 * Plain C header, no SDK types: usable from the backends without the dolphin headers.
 */
#ifndef RE4PORT_AX_SHIM_H
#define RE4PORT_AX_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RE4AX_DSP_RATE      32000u  /* AX mixing rate */
#define RE4AX_FRAME_SAMPLES 160u    /* samples per 5 ms AX frame at 32 kHz */
#define RE4AX_FRAME_US      5000u
#define RE4AX_MAX_VOICES    64      /* AX_MAX_VOICES */
#define RE4AX_MAX_CATCHUP   12      /* max AX frames one re4_audio_pump() runs (60 ms) */

/* AXPBADDR.format */
#define RE4AX_FMT_ADPCM 0x00
#define RE4AX_FMT_PCM16 0x0A
#define RE4AX_FMT_PCM8  0x19

/* AX modes (AXSetMode) */
#define RE4AX_MODE_STEREO   0
#define RE4AX_MODE_SURROUND 1
#define RE4AX_MODE_DPL2     2

/* Mix bins of a voice, in AXPBMIX order of meaning. In DPL2 mode main Ls/Rs come from the AuxB
 * L/R volumes and the AuxA bus is 4-channel (AL, AR, AS = Ls, BS = Rs); AuxB is not mixed. */
enum {
    RE4AX_BIN_L, RE4AX_BIN_R, RE4AX_BIN_S,
    RE4AX_BIN_AL, RE4AX_BIN_AR, RE4AX_BIN_AS,
    RE4AX_BIN_BL, RE4AX_BIN_BR, RE4AX_BIN_BS,
    RE4AX_BIN_COUNT
};

/* Dirty bits passed to Re4AxBackend::voice_update. */
#define RE4AX_DIRTY_ADDR   0x001u
#define RE4AX_DIRTY_ADPCM  0x002u
#define RE4AX_DIRTY_LOOP   0x004u  /* AXPBADPCMLOOP */
#define RE4AX_DIRTY_SRC    0x008u
#define RE4AX_DIRTY_LPF    0x010u
#define RE4AX_DIRTY_MIX    0x020u
#define RE4AX_DIRTY_VE     0x040u
#define RE4AX_DIRTY_TYPE   0x080u
#define RE4AX_DIRTY_STATE  0x100u
#define RE4AX_DIRTY_ALL    0x1FFu

/* What the DSP would know about one voice. Addresses are AX addresses: nibbles for ADPCM, 16-bit
 * words for PCM16, bytes for PCM8, all relative to re4_aram_base. `end` is inclusive. */
typedef struct Re4AxVoiceState {
    int      acquired;      /* AXAcquireVoice'd and not yet freed */
    int      playing;       /* == pb.state (AX_PB_STATE_RUN = 1) */
    int      ended;         /* a one-shot reached its end this frame (backend sets, shim clears) */
    uint16_t type;          /* pb.type: 0 normal, 1 stream (loop does not reload the ADPCM history) */
    uint16_t format;        /* RE4AX_FMT_* */
    uint32_t priority;
    int      loop_flag;
    uint32_t cur, loop, end;
    /* ADPCM context (AXPBADPCM / AXPBADPCMLOOP) */
    int16_t  coef[16];      /* a[8][2] flattened */
    uint16_t gain;
    uint16_t ps;            /* pred_scale */
    int16_t  yn1, yn2;
    uint16_t loop_ps;
    int16_t  loop_yn1, loop_yn2;
    /* sample-rate converter (AXPBSRC) */
    uint32_t src_type;      /* AX_SRC_TYPE_*; NONE plays 1:1 */
    uint32_t ratio;         /* 16.16 */
    uint32_t frac;          /* 16-bit fractional position */
    int16_t  hist[4];       /* last decoded samples, hist[3] newest */
    /* one-pole low-pass (AXPBLPF): y = (a0*x + b0*y1) >> 15 */
    int      lpf_on;
    uint16_t lpf_a0, lpf_b0;
    int32_t  lpf_yn1;
    /* gains, Q15 (0x8000 = 1.0); vol_prev = gains at the start of the current frame (ramped) */
    uint16_t ve;
    uint16_t vol[RE4AX_BIN_COUNT];
    uint16_t vol_prev[RE4AX_BIN_COUNT];
    uint16_t ve_prev;
    int      vol_primed;    /* vol_prev valid (otherwise the first frame starts at vol) */
} Re4AxVoiceState;

/* A backend. All hooks optional except run_frame and frames_wanted. */
typedef struct Re4AxBackend {
    const char* name;
    int      (*open)(void);                 /* AXInit; returns 0 on failure (shim keeps running silently) */
    void     (*close)(void);                /* AXQuit */
    void     (*voice_start)(int idx, Re4AxVoiceState* v);
    void     (*voice_stop)(int idx, Re4AxVoiceState* v);
    void     (*voice_update)(int idx, Re4AxVoiceState* v, uint32_t dirty);
    void     (*set_aux)(int bus, int active);  /* bus 0 = A, 1 = B; active = a callback is registered */
    uint32_t (*frames_wanted)(void);        /* AX frames the output can take now (pacing) */
    void     (*run_frame)(void);            /* render one 5 ms frame (voices, AUX callbacks, output) */
    void     (*pump_end)(void);             /* after the frames of one pump (DirectSoundDoWork) */
} Re4AxBackend;

/* ---- shim services used by backends -------------------------------------------------------- */
Re4AxVoiceState* re4ax_voice(int idx);       /* 0..RE4AX_MAX_VOICES-1 */
uint32_t re4ax_mode(void);                   /* AXSetMode value */
int      re4ax_compressor(void);             /* AXSetCompressor value */
/* Runs the registered AUX callback of `bus` on 160-sample s32 buffers (in place). In DPL2 mode the
 * callback gets {l, r, ls, rs}, otherwise {l, r, s}. Returns 0 when no callback is registered. */
int      re4ax_call_aux(int bus, int32_t* l, int32_t* r, int32_t* s, int32_t* rs);

/* ---- control (port / tests) ------------------------------------------------------------------ */
void     re4ax_set_backend(const Re4AxBackend* be);  /* before AXInit; default = re4snd_soft_backend() */
void     re4ax_run_frames(uint32_t n);               /* run n AX frames now, no pacing (tests) */
uint32_t re4ax_frame_count(void);                    /* AX frames run since AXInit */
int      re4ax_active_voices(void);                  /* voices with pb.state == 1 */

/* Pumps audio: runs the AX frames the backend's output needs (<= RE4AX_MAX_CATCHUP; a larger backlog
 * is dropped), then the backend's per-pump work. Call once per game frame (VIWaitForRetrace). */
void     re4_audio_pump(void);

/* ---- software backend (snd_soft.cpp) ------------------------------------------------------ */
enum {
    RE4SND_SINK_AUTO = 0,   /* Xbox+RE4_AUDIO_RXDK: DirectSound; host RE4_GL: SDL; else null */
    RE4SND_SINK_NULL,       /* discard, paced by the wall clock */
    RE4SND_SINK_SDL,
    RE4SND_SINK_DSOUND,
    RE4SND_SINK_CAPTURE     /* hand every output block to the capture callback; frames_wanted = 0 */
};
typedef void (*Re4SndCaptureFn)(const int16_t* lr, uint32_t frames, void* ctx);

const Re4AxBackend* re4snd_soft_backend(void);
void     re4snd_soft_config(int sink, uint32_t out_rate);   /* before AXInit; out_rate 32000 or 48000 */
void     re4snd_soft_set_capture(Re4SndCaptureFn fn, void* ctx); /* also called for real sinks */
void     re4snd_soft_set_gain(float gain);                  /* master output gain (default 1, Xbox 2) */
uint32_t re4snd_soft_out_rate(void);
int      re4snd_soft_sink(void);                            /* sink actually opened */

#ifdef __cplusplus
}
#endif

#endif /* RE4PORT_AX_SHIM_H */
