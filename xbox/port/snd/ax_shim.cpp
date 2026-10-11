// ax_shim.cpp — the Nintendo audio SDK surface the game links (AX, MIX, AXFX, AXART, SYN, SEQ, AI;
// xbox/docs/sdk-symbols.md) over a backend-neutral voice model (re4port/ax_shim.h, CT-10).
//
// Prototypes come from the game's own headers (include/snd_sdk.h -> include/dolphin/*.h), so every
// definition here is checked against the declaration the game compiles with.
//
// AXVPB: the layout of include/dolphin/ax.h is kept byte for byte. The game reads only these fields
// directly (audit of src/game/snd_*.cpp):
//   pb.state              iss2:145, iss4:72/211/263, str1:108, str3:278/285  (0 = voice finished)
//   pb.addr.loopFlag      iss2:148
//   *(u32*)&pb.addr.currentAddressHi / endAddressHi                         iss2:149-150, str2:164
// On the little-endian port a u32 load of the Hi/Lo pair would read (Lo << 16) | Hi, so the shim
// stores every address pair it mirrors into the pb as a host-order u32 at the Hi field (the Hi u16
// then holds the low half). The game only *writes* AXPBADDR fields into its own local structs that
// it passes to AXSetVoiceAddr (iss3:276-296, str3:102-138), which are read here as (Hi << 16) | Lo.
// The authoritative voice state is Re4AxVoiceState (one per AXVPB::index); the pb is a mirror that
// is refreshed after every setter and after every AX frame.
//
// Order of one AX frame (re4ax_run_frames): backend renders 160 samples with the current parameters
// and runs the AUX callbacks -> voices that reached their end get pb.state = 0 -> the
// AXRegisterCallback callback (game: cb_audio_frame, snd_main.cpp:81) runs and programs the next one.
#include "re4port/re4_xbox.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define SND_SDK_NO_GX 1
#include "snd_sdk.h"   // types.h + dolphin/{ax,syn,seq,mix,axart,axfx,ar,dvd}.h + AXSetCompressor etc.
#include <dolphin/ai.h>

#include "re4port/ax_shim.h"

extern "C" {

// ================================================================================================
// AX
// ================================================================================================

static AXVPB            s_vpb[RE4AX_MAX_VOICES];
static Re4AxVoiceState  s_vs[RE4AX_MAX_VOICES];
static const Re4AxBackend* s_be;
static int              s_be_ok;
static int              s_ax_init;
static AXCallback       s_user_cb;
static void           (*s_aux_cb[2])(void*, void*);
static void*            s_aux_ctx[2];
static u32              s_ax_mode;
static u32              s_compressor;
static u32              s_step_mode;
static u32              s_frame_count;
static u32              s_max_dsp_cycles = 405000;   // 81 MHz DSP * 5 ms
static u32              s_acquire_serial;
static u32              s_voice_serial[RE4AX_MAX_VOICES];

AXPROFILE __AXLocalProfile;

static void mix_release_index(int idx);   // MIX section

static inline int vpb_ok(const AXVPB* p)
{
    return p >= &s_vpb[0] && p < &s_vpb[RE4AX_MAX_VOICES];
}
static inline int vidx(const AXVPB* p) { return (int) (p - &s_vpb[0]); }

static inline void put_pair(u16* hi, u32 v) { memcpy(hi, &v, 4); }   // host-order u32 at &Hi

static void mirror_pb(int i)
{
    AXVPB* p = &s_vpb[i];
    Re4AxVoiceState* v = &s_vs[i];
    p->pb.state = (u16) (v->playing ? 1 : 0);
    p->pb.type = v->type;
    p->pb.addr.loopFlag = (u16) v->loop_flag;
    p->pb.addr.format = v->format;
    put_pair(&p->pb.addr.loopAddressHi, v->loop);
    put_pair(&p->pb.addr.endAddressHi, v->end);
    put_pair(&p->pb.addr.currentAddressHi, v->cur);
    p->pb.adpcm.pred_scale = v->ps;
    p->pb.adpcm.yn1 = (u16) v->yn1;
    p->pb.adpcm.yn2 = (u16) v->yn2;
    p->pb.src.ratioHi = (u16) (v->ratio >> 16);
    p->pb.src.ratioLo = (u16) v->ratio;
    p->pb.src.currentAddressFrac = (u16) v->frac;
    p->pb.ve.currentVolume = v->ve;
    p->pb.lpf.on = (u16) v->lpf_on;
    p->pb.lpf.a0 = v->lpf_a0;
    p->pb.lpf.b0 = v->lpf_b0;
}

static void touch(int i, u32 dirty)
{
    if (s_be && s_be->voice_update) s_be->voice_update(i, &s_vs[i], dirty);
    mirror_pb(i);
}

static void reset_voice(int i)
{
    AXVPB* p = &s_vpb[i];
    Re4AxVoiceState* v = &s_vs[i];
    memset(p, 0, sizeof(*p));
    p->index = (u32) i;
    memset(v, 0, sizeof(*v));
    v->ve = 0x7FFF;
    v->ve_prev = 0x7FFF;
    v->ratio = 0x10000;
    v->src_type = AX_SRC_TYPE_LINEAR;
    mirror_pb(i);
}

Re4AxVoiceState* re4ax_voice(int idx) { return (idx >= 0 && idx < RE4AX_MAX_VOICES) ? &s_vs[idx] : 0; }
uint32_t re4ax_mode(void) { return s_ax_mode; }
int re4ax_compressor(void) { return (int) s_compressor; }
uint32_t re4ax_frame_count(void) { return s_frame_count; }

int re4ax_active_voices(void)
{
    int n = 0;
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) n += (s_vs[i].acquired && s_vs[i].playing) ? 1 : 0;
    return n;
}

int re4ax_call_aux(int bus, int32_t* l, int32_t* r, int32_t* s, int32_t* rs)
{
    if (bus < 0 || bus > 1 || !s_aux_cb[bus]) return 0;
    if (s_ax_mode == RE4AX_MODE_DPL2) {
        AX_AUX_DATA_DPL2 d;
        d.l = (s32*) l; d.r = (s32*) r; d.ls = (s32*) s; d.rs = (s32*) rs;
        s_aux_cb[bus](&d, s_aux_ctx[bus]);
    } else {
        AX_AUX_DATA d;
        d.l = (s32*) l; d.r = (s32*) r; d.s = (s32*) s;
        s_aux_cb[bus](&d, s_aux_ctx[bus]);
    }
    return 1;
}

void re4ax_set_backend(const Re4AxBackend* be) { s_be = be; }

void AXInitEx(u32 outputBufferMode)
{
    (void) outputBufferMode;
    if (s_ax_init) return;
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) reset_voice(i);
    s_user_cb = 0;
    s_aux_cb[0] = s_aux_cb[1] = 0;
    s_aux_ctx[0] = s_aux_ctx[1] = 0;
    s_ax_mode = RE4AX_MODE_STEREO;
    s_compressor = 0;
    s_frame_count = 0;
    if (!s_be) s_be = re4snd_soft_backend();
    s_be_ok = s_be->open ? s_be->open() : 1;
    if (!s_be_ok) re4_log("AX: backend %s failed to open; audio frames run silently\n", s_be->name);
    for (int i = 0; i < 2; i++)
        if (s_be->set_aux) s_be->set_aux(i, 0);
    s_ax_init = 1;
}

void AXInit(void) { AXInitEx(0); }

void AXQuit(void)
{
    if (!s_ax_init) return;
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) {
        if (s_vs[i].playing && s_be->voice_stop) s_be->voice_stop(i, &s_vs[i]);
        reset_voice(i);
    }
    if (s_be->close) s_be->close();
    s_user_cb = 0;
    s_aux_cb[0] = s_aux_cb[1] = 0;
    s_ax_init = 0;
}

static void free_voice(int i)
{
    if (s_vs[i].playing && s_be && s_be->voice_stop) s_be->voice_stop(i, &s_vs[i]);
    reset_voice(i);
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    if (!s_ax_init) AXInit();
    int pick = -1;
    for (int i = 0; i < RE4AX_MAX_VOICES; i++)
        if (!s_vs[i].acquired) { pick = i; break; }
    if (pick < 0) {
        // steal the lowest-priority voice below the request (the oldest on a tie), like AXAlloc
        for (int i = 0; i < RE4AX_MAX_VOICES; i++) {
            u32 pr = s_vs[i].priority;
            if (pr >= priority) continue;
            if (pick < 0 || pr < s_vs[pick].priority ||
                (pr == s_vs[pick].priority && s_voice_serial[i] < s_voice_serial[pick]))
                pick = i;
        }
        if (pick < 0) return 0;
        AXVPB* victim = &s_vpb[pick];
        void (*cb)(void*) = victim->callback;
        free_voice(pick);   // the game's drop callback only clears its own bookkeeping
        mix_release_index(pick);
        if (cb) cb(victim);
        reset_voice(pick);
    }
    AXVPB* p = &s_vpb[pick];
    Re4AxVoiceState* v = &s_vs[pick];
    v->acquired = 1;
    v->priority = priority;
    s_voice_serial[pick] = ++s_acquire_serial;
    p->priority = priority;
    p->callback = callback;
    p->userContext = userContext;
    touch(pick, RE4AX_DIRTY_ALL);
    return p;
}

void AXFreeVoice(AXVPB* p)
{
    if (!vpb_ok(p)) return;
    free_voice(vidx(p));
}

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].priority = priority;
    p->priority = priority;
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    s_aux_cb[0] = callback;
    s_aux_ctx[0] = context;
    if (s_be && s_be->set_aux) s_be->set_aux(0, callback != 0);
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    s_aux_cb[1] = callback;
    s_aux_ctx[1] = context;
    if (s_be && s_be->set_aux) s_be->set_aux(1, callback != 0);
}

void AXSetMode(u32 mode) { s_ax_mode = mode; }
u32 AXGetMode(void) { return s_ax_mode; }
void AXSetStepMode(u32 i) { s_step_mode = i; }
void AXSetCompressor(u32 switch_) { s_compressor = switch_; }

AXCallback AXRegisterCallback(AXCallback callback)
{
    AXCallback old = s_user_cb;
    s_user_cb = callback;
    return old;
}

void AXInitProfile(AXPROFILE* profile, u32 maxProfiles) { (void) profile; (void) maxProfiles; }
u32 AXGetProfile(void) { return 0; }

void AXSetVoiceSrcType(AXVPB* p, u32 type)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].src_type = type;
    p->pb.srcSelect = (u16) (type == AX_SRC_TYPE_NONE ? 2 : (type == AX_SRC_TYPE_LINEAR ? 1 : 0));
    touch(vidx(p), RE4AX_DIRTY_SRC);
}

void AXSetVoiceState(AXVPB* p, u16 state)
{
    if (!vpb_ok(p)) return;
    int i = vidx(p);
    Re4AxVoiceState* v = &s_vs[i];
    int run = state == 1;
    if (run && !v->playing) {
        v->playing = 1;
        v->ended = 0;
        if (s_be && s_be->voice_start) s_be->voice_start(i, v);
    } else if (!run && v->playing) {
        v->playing = 0;
        if (s_be && s_be->voice_stop) s_be->voice_stop(i, v);
    }
    touch(i, RE4AX_DIRTY_STATE);
}

void AXSetVoiceType(AXVPB* p, u16 type)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].type = type;
    touch(vidx(p), RE4AX_DIRTY_TYPE);
}

// a new gain set: ramp from the gains in force at the start of this frame (vol_prev)
static void set_gains(int i, const u16* bins, int has_bins, int has_ve, u16 ve)
{
    Re4AxVoiceState* v = &s_vs[i];
    if (has_bins) memcpy(v->vol, bins, sizeof(v->vol));
    if (has_ve) v->ve = ve;
    if (!v->vol_primed) {
        memcpy(v->vol_prev, v->vol, sizeof(v->vol));
        v->ve_prev = v->ve;
    }
    touch(i, RE4AX_DIRTY_MIX | RE4AX_DIRTY_VE);
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    if (!vpb_ok(p) || !mix) return;
    u16 b[RE4AX_BIN_COUNT];
    b[RE4AX_BIN_L] = mix->vL;      b[RE4AX_BIN_R] = mix->vR;      b[RE4AX_BIN_S] = mix->vS;
    b[RE4AX_BIN_AL] = mix->vAuxAL; b[RE4AX_BIN_AR] = mix->vAuxAR; b[RE4AX_BIN_AS] = mix->vAuxAS;
    b[RE4AX_BIN_BL] = mix->vAuxBL; b[RE4AX_BIN_BR] = mix->vAuxBR; b[RE4AX_BIN_BS] = mix->vAuxBS;
    p->pb.mix = *mix;
    set_gains(vidx(p), b, 1, 0, 0);
}

void AXSetVoiceItdOn(AXVPB* p) { (void) p; }
void AXSetVoiceItdTarget(AXVPB* p, u16 lShift, u16 rShift) { (void) p; (void) lShift; (void) rShift; }
void AXSetVoiceUpdateIncrement(AXVPB* p) { (void) p; }
void AXSetVoiceUpdateWrite(AXVPB* p, u16 param, u16 data) { (void) p; (void) param; (void) data; }
void AXSetVoiceDpop(AXVPB* p, AXPBDPOP* dpop) { (void) p; (void) dpop; }
void AXSetVoiceFir(AXVPB* p, AXPBFIR* fir) { (void) p; (void) fir; }

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    if (!vpb_ok(p) || !ve) return;
    set_gains(vidx(p), 0, 0, 1, ve->currentVolume);
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    if (!vpb_ok(p)) return;
    // the DSP adds delta per sample; approximate: the target is reached at the end of this frame
    int t = (int) s_vs[vidx(p)].ve + (int) delta * (int) RE4AX_FRAME_SAMPLES;
    if (t < 0) t = 0;
    if (t > 0xFFFF) t = 0xFFFF;
    set_gains(vidx(p), 0, 0, 1, (u16) t);
}

void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    if (!vpb_ok(p) || !addr) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    v->loop_flag = addr->loopFlag != 0;
    v->format = addr->format;
    v->loop = ((u32) addr->loopAddressHi << 16) | addr->loopAddressLo;
    v->end = ((u32) addr->endAddressHi << 16) | addr->endAddressLo;
    v->cur = ((u32) addr->currentAddressHi << 16) | addr->currentAddressLo;
    touch(vidx(p), RE4AX_DIRTY_ADDR);
}

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].loop_flag = loop != 0;
    touch(vidx(p), RE4AX_DIRTY_ADDR);
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].loop = addr;
    touch(vidx(p), RE4AX_DIRTY_ADDR);
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].end = addr;
    touch(vidx(p), RE4AX_DIRTY_ADDR);
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    if (!vpb_ok(p)) return;
    s_vs[vidx(p)].cur = addr;
    touch(vidx(p), RE4AX_DIRTY_ADDR);
}

void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* adpcm)
{
    if (!vpb_ok(p) || !adpcm) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    for (int k = 0; k < 8; k++) {
        v->coef[2 * k] = (s16) adpcm->a[k][0];
        v->coef[2 * k + 1] = (s16) adpcm->a[k][1];
    }
    v->gain = adpcm->gain;
    v->ps = adpcm->pred_scale;
    v->yn1 = (s16) adpcm->yn1;
    v->yn2 = (s16) adpcm->yn2;
    memcpy(p->pb.adpcm.a, adpcm->a, sizeof(adpcm->a));
    p->pb.adpcm.gain = adpcm->gain;
    touch(vidx(p), RE4AX_DIRTY_ADPCM);
}

void AXSetVoiceSrc(AXVPB* p, AXPBSRC* src_)
{
    if (!vpb_ok(p) || !src_) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    v->ratio = ((u32) src_->ratioHi << 16) | src_->ratioLo;
    v->frac = src_->currentAddressFrac;
    for (int k = 0; k < 4; k++) v->hist[k] = (s16) src_->last_samples[k];
    touch(vidx(p), RE4AX_DIRTY_SRC);
}

void AXSetVoiceSrcRatio(AXVPB* p, f32 ratio)
{
    if (!vpb_ok(p)) return;
    float r = ratio * 65536.0f;
    if (!(r > 0.0f)) r = 0.0f;
    if (r > 4294967040.0f) r = 4294967040.0f;
    s_vs[vidx(p)].ratio = (u32) r;
    touch(vidx(p), RE4AX_DIRTY_SRC);
}

void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* adpcmloop)
{
    if (!vpb_ok(p) || !adpcmloop) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    v->loop_ps = adpcmloop->loop_pred_scale;
    v->loop_yn1 = (s16) adpcmloop->loop_yn1;
    v->loop_yn2 = (s16) adpcmloop->loop_yn2;
    p->pb.adpcmLoop = *adpcmloop;
    touch(vidx(p), RE4AX_DIRTY_LOOP);
}

void AXSetMaxDspCycles(u32 cycles) { s_max_dsp_cycles = cycles; }
u32 AXGetMaxDspCycles(void) { return s_max_dsp_cycles; }
// statistics only (snd_main.cpp:178): a plausible per-frame cost, ~1500 cycles per running voice
u32 AXGetDspCycles(void) { return 12000u + 1500u * (u32) re4ax_active_voices(); }

void AXSetVoiceLpf(AXVPB* p, AXPBLPF* lpf)
{
    if (!vpb_ok(p) || !lpf) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    v->lpf_on = lpf->on != 0;
    v->lpf_yn1 = (s16) lpf->yn1;
    v->lpf_a0 = lpf->a0;
    v->lpf_b0 = lpf->b0;
    touch(vidx(p), RE4AX_DIRTY_LPF);
}

void AXSetVoiceLpfCoefs(AXVPB* p, u16 a0, u16 b0)
{
    if (!vpb_ok(p)) return;
    Re4AxVoiceState* v = &s_vs[vidx(p)];
    v->lpf_a0 = a0;
    v->lpf_b0 = b0;
    touch(vidx(p), RE4AX_DIRTY_LPF);
}

// one-pole y = a0*x + b0*y1 (Q15) with a0 + b0 = 1 and cutoff `freq` Hz at 32 kHz
void AXGetLpfCoefs(u16 freq, u16* a0, u16* b0)
{
    double b = exp(-2.0 * 3.14159265358979323846 * (double) freq / 32000.0);
    int ib = (int) (b * 32768.0 + 0.5);
    if (ib > 0x7FFF) ib = 0x7FFF;
    if (b0) *b0 = (u16) ib;
    if (a0) *a0 = (u16) (0x7FFF - ib);
}

// ---- frame driver -----------------------------------------------------------------------------

void re4ax_run_frames(uint32_t n)
{
    if (!s_ax_init) return;
    while (n--) {
        if (s_be_ok && s_be->run_frame) s_be->run_frame();
        for (int i = 0; i < RE4AX_MAX_VOICES; i++) {
            Re4AxVoiceState* v = &s_vs[i];
            if (v->ended) {
                v->ended = 0;
                v->playing = 0;   // a one-shot that reached endAddress: the DSP sets pb.state = 0
            }
            memcpy(v->vol_prev, v->vol, sizeof(v->vol));
            v->ve_prev = v->ve;
            v->vol_primed = 1;
            mirror_pb(i);
        }
        s_frame_count++;
        if (s_user_cb) s_user_cb();
    }
}

void re4_audio_pump(void)
{
    if (!s_ax_init || !s_be) return;
    uint32_t n = s_be->frames_wanted ? s_be->frames_wanted() : 0;
    if (n > RE4AX_MAX_CATCHUP) n = RE4AX_MAX_CATCHUP;   // the rest of a backlog is dropped
    re4ax_run_frames(n);
    if (s_be->pump_end) s_be->pump_end();
}

// ================================================================================================
// MIX — semantics of the SDK's mix.c (src/lib/mix.c, same tables), applied to the voice model.
// Units: dB * 10 (-904 = silence), pan/span 0..127 (pan 0 = left, span 127 = front).
// MIXInitChannel applies at once; MIXSet*/Adjust* take effect at the next MIXUpdateSettings (the
// game calls it at the end of every audio frame) and ramp over the following frame.
// ================================================================================================

// Tables copied verbatim from the SDK mix.c (src/lib/mix.c): volume = 32768 * 10^(dB/20) for
// dB*10 = -904..+60; pan / DPL2 tables are attenuations in dB*10.
static const u16 s_mix_vol_tab[965] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12,
    13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 15, 15, 15,
    15, 15, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18,
    18, 19, 19, 19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22,
    22, 22, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 26, 26, 26, 26,
    27, 27, 27, 28, 28, 28, 29, 29, 29, 30, 30, 30, 31, 31, 32, 32,
    32, 33, 33, 33, 34, 34, 35, 35, 35, 36, 36, 37, 37, 38, 38, 38,
    39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46,
    47, 47, 48, 49, 49, 50, 50, 51, 51, 52, 53, 53, 54, 55, 55, 56,
    56, 57, 58, 58, 59, 60, 61, 61, 62, 63, 63, 64, 65, 66, 66, 67,
    68, 69, 70, 70, 71, 72, 73, 74, 75, 75, 76, 77, 78, 79, 80, 81,
    82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    98, 100, 101, 102, 103, 104, 106, 107, 108, 109, 111, 112, 113, 114, 116, 117,
    118, 120, 121, 123, 124, 126, 127, 128, 130, 131, 133, 135, 136, 138, 139, 141,
    143, 144, 146, 148, 149, 151, 153, 155, 156, 158, 160, 162, 164, 166, 168, 170,
    171, 173, 175, 178, 180, 182, 184, 186, 188, 190, 192, 195, 197, 199, 202, 204,
    206, 209, 211, 214, 216, 219, 221, 224, 226, 229, 231, 234, 237, 240, 242, 245,
    248, 251, 254, 257, 260, 263, 266, 269, 272, 275, 278, 282, 285, 288, 292, 295,
    298, 302, 305, 309, 312, 316, 320, 323, 327, 331, 335, 339, 343, 347, 351, 355,
    359, 363, 367, 371, 376, 380, 384, 389, 393, 398, 403, 407, 412, 417, 422, 427,
    431, 436, 442, 447, 452, 457, 462, 468, 473, 479, 484, 490, 495, 501, 507, 513,
    519, 525, 531, 537, 543, 550, 556, 562, 569, 576, 582, 589, 596, 603, 610, 617,
    624, 631, 638, 646, 653, 661, 669, 676, 684, 692, 700, 708, 716, 725, 733, 742,
    750, 759, 768, 777, 786, 795, 804, 813, 823, 832, 842, 852, 861, 871, 881, 892,
    902, 912, 923, 934, 945, 955, 967, 978, 989, 1001, 1012, 1024, 1036, 1048, 1060, 1072,
    1085, 1097, 1110, 1123, 1136, 1149, 1162, 1176, 1189, 1203, 1217, 1231, 1245, 1260, 1274, 1289,
    1304, 1319, 1334, 1350, 1365, 1381, 1397, 1414, 1430, 1446, 1463, 1480, 1497, 1515, 1532, 1550,
    1568, 1586, 1604, 1623, 1642, 1661, 1680, 1700, 1719, 1739, 1759, 1780, 1800, 1821, 1842, 1864,
    1885, 1907, 1929, 1951, 1974, 1997, 2020, 2043, 2067, 2091, 2115, 2140, 2164, 2190, 2215, 2241,
    2266, 2293, 2319, 2346, 2373, 2401, 2429, 2457, 2485, 2514, 2543, 2573, 2602, 2632, 2663, 2694,
    2725, 2757, 2789, 2821, 2853, 2887, 2920, 2954, 2988, 3023, 3058, 3093, 3129, 3165, 3202, 3239,
    3276, 3314, 3353, 3391, 3431, 3470, 3511, 3551, 3592, 3634, 3676, 3719, 3762, 3805, 3849, 3894,
    3939, 3985, 4031, 4078, 4125, 4173, 4221, 4270, 4319, 4369, 4420, 4471, 4523, 4575, 4628, 4682,
    4736, 4791, 4846, 4902, 4959, 5017, 5075, 5133, 5193, 5253, 5314, 5375, 5438, 5501, 5564, 5629,
    5694, 5760, 5827, 5894, 5962, 6031, 6101, 6172, 6243, 6316, 6389, 6463, 6538, 6613, 6690, 6767,
    6846, 6925, 7005, 7086, 7168, 7251, 7335, 7420, 7506, 7593, 7681, 7770, 7860, 7951, 8043, 8136,
    8230, 8326, 8422, 8520, 8618, 8718, 8819, 8921, 9025, 9129, 9235, 9342, 9450, 9559, 9670, 9782,
    9895, 10010, 10126, 10243, 10362, 10482, 10603, 10726, 10850, 10976, 11103, 11231, 11361, 11493, 11626, 11761,
    11897, 12035, 12174, 12315, 12458, 12602, 12748, 12895, 13045, 13196, 13349, 13503, 13659, 13818, 13978, 14140,
    14303, 14469, 14636, 14806, 14977, 15151, 15326, 15504, 15683, 15865, 16049, 16234, 16422, 16613, 16805, 17000,
    17196, 17396, 17597, 17801, 18007, 18215, 18426, 18640, 18856, 19074, 19295, 19518, 19744, 19973, 20204, 20438,
    20675, 20914, 21156, 21401, 21649, 21900, 22153, 22410, 22669, 22932, 23197, 23466, 23738, 24013, 24291, 24572,
    24857, 25144, 25436, 25730, 26028, 26329, 26634, 26943, 27255, 27570, 27890, 28213, 28539, 28870, 29204, 29542,
    29884, 30230, 30580, 30934, 31293, 31655, 32022, 32392, 32767, 33147, 33531, 33919, 34312, 34709, 35111, 35518,
    35929, 36345, 36766, 37192, 37622, 38058, 38499, 38944, 39395, 39851, 40313, 40780, 41252, 41730, 42213, 42702,
    43196, 43696, 44202, 44714, 45232, 45756, 46286, 46821, 47364, 47912, 48467, 49028, 49596, 50170, 50751, 51339,
    51933, 52535, 53143, 53758, 54381, 55011, 55648, 56292, 56944, 57603, 58270, 58945, 59627, 60318, 61016, 61723,
    62438, 63161, 63892, 64632, 65380,
};
static const int s_mix_pan_tab[128] = {
    0, 0, -1, -1, -1, -2, -2, -2, -3, -3, -4, -4, -4, -5, -5, -5,
    -6, -6, -7, -7, -7, -8, -8, -9, -9, -10, -10, -10, -11, -11, -12, -12,
    -13, -13, -14, -14, -14, -15, -15, -16, -16, -17, -17, -18, -18, -19, -20, -20,
    -21, -21, -22, -22, -23, -23, -24, -25, -25, -26, -26, -27, -28, -28, -29, -30,
    -30, -31, -32, -33, -33, -34, -35, -36, -36, -37, -38, -39, -40, -40, -41, -42,
    -43, -44, -45, -46, -47, -48, -49, -50, -51, -52, -54, -55, -56, -57, -59, -60,
    -61, -63, -64, -66, -67, -69, -71, -72, -74, -76, -78, -80, -83, -85, -87, -90,
    -93, -96, -99, -102, -106, -110, -115, -120, -126, -133, -140, -150, -163, -180, -210, -904,
};
static const int s_mix_dpl2_front[128] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1,
    -1, -1, -2, -2, -2, -2, -3, -3, -3, -4, -4, -4, -5, -5, -6, -6,
    -6, -7, -7, -8, -8, -9, -9, -10, -11, -11, -12, -12, -13, -14, -14, -15,
    -16, -17, -17, -18, -19, -20, -21, -21, -22, -23, -24, -25, -26, -27, -28, -29,
    -30, -31, -32, -34, -35, -36, -37, -38, -40, -41, -42, -44, -45, -47, -48, -50,
    -52, -53, -55, -57, -58, -60, -62, -64, -66, -68, -70, -73, -75, -77, -80, -82,
    -85, -88, -90, -93, -96, -100, -103, -106, -110, -114, -118, -122, -126, -131, -136, -141,
    -146, -152, -159, -166, -173, -181, -190, -201, -212, -225, -241, -261, -286, -321, -381, -960,
};
static const int s_mix_dpl2_rear[128] = {
    -61, -61, -60, -59, -59, -58, -58, -57, -56, -56, -55, -55, -54, -53, -53, -52,
    -52, -51, -50, -50, -49, -49, -48, -48, -47, -47, -46, -46, -45, -45, -44, -44,
    -43, -43, -42, -42, -41, -41, -40, -40, -39, -39, -38, -38, -38, -37, -37, -36,
    -36, -35, -35, -35, -34, -34, -33, -33, -32, -32, -32, -31, -31, -31, -30, -30,
    -29, -29, -29, -28, -28, -28, -27, -27, -27, -26, -26, -26, -25, -25, -25, -24,
    -24, -24, -23, -23, -23, -22, -22, -22, -21, -21, -21, -20, -20, -20, -20, -19,
    -19, -19, -18, -18, -18, -18, -17, -17, -17, -17, -16, -16, -16, -16, -15, -15,
    -15, -15, -14, -14, -14, -14, -13, -13, -13, -13, -13, -12, -12, -12, -12, -11,
};

#define MIXF_UPD_INPUT 0x10000000u
#define MIXF_UPD_MIX   0x40000000u
#define MIXF_MODE_MASK 0x00000007u   // bit0 AuxA pre-fader, bit1 AuxB pre-fader, bit2 mute

static MIXChannel s_mix[RE4AX_MAX_VOICES];
static u32 s_mix_mode = MIX_SOUND_MODE_STEREO;
static int s_mix_dvd_fader;

static u16 mix_vol(int db)
{
    if (db <= -904) return 0;
    if (db >= 60) return 0xFF64;
    return s_mix_vol_tab[db + 904];
}

static int clamp_pan(int pan) { return pan < 0 ? 0 : (pan > 127 ? 127 : pan); }

static void mix_set_pan(MIXChannel* c)
{
    int pan = clamp_pan(c->pan), ipan = 127 - pan;
    int span = clamp_pan(c->span), ispan = 127 - span;
    if (s_mix_mode == MIX_SOUND_MODE_DPL2) {
        c->l = s_mix_dpl2_front[pan];
        c->r = s_mix_dpl2_front[ipan];
        c->f = s_mix_dpl2_front[ispan];
        c->b = s_mix_dpl2_front[span];
        c->l1 = s_mix_dpl2_rear[ipan];
        c->r1 = s_mix_dpl2_rear[pan];
    } else {
        c->l = s_mix_pan_tab[pan];
        c->r = s_mix_pan_tab[ipan];
        c->f = s_mix_pan_tab[ispan];
        c->b = s_mix_pan_tab[span];
    }
}

static void mix_reset(MIXChannel* c)
{
    memset(c, 0, sizeof(*c));
    c->auxA = -960;
    c->auxB = -960;
    c->pan = 64;
    c->span = 127;
    mix_set_pan(c);
}

// gains of the channel in the current sound mode -> AX bins (+ ve from input)
static void mix_compute(MIXChannel* c, u16* bins, u16* ve)
{
    u32 m = c->mode;
    int fA = (m & 1) ? 0 : c->fader, fB = (m & 2) ? 0 : c->fader;
    *ve = (m & 4) ? 0 : mix_vol(c->input);
    memset(bins, 0, sizeof(u16) * RE4AX_BIN_COUNT);
    switch (s_mix_mode) {
    case MIX_SOUND_MODE_MONO:
        bins[RE4AX_BIN_L] = bins[RE4AX_BIN_R] = mix_vol(c->fader + c->f);
        bins[RE4AX_BIN_S] = mix_vol(c->fader + c->b - 30);
        bins[RE4AX_BIN_AL] = bins[RE4AX_BIN_AR] = mix_vol(fA + c->auxA + c->f);
        bins[RE4AX_BIN_AS] = mix_vol(fA + c->auxA + c->b - 30);
        bins[RE4AX_BIN_BL] = bins[RE4AX_BIN_BR] = mix_vol(fB + c->auxB + c->f);
        bins[RE4AX_BIN_BS] = mix_vol(fB + c->auxB + c->b - 30);
        break;
    default:
    case MIX_SOUND_MODE_STEREO:
    case MIX_SOUND_MODE_SURROUND:
        bins[RE4AX_BIN_L] = mix_vol(c->fader + c->l + c->f);
        bins[RE4AX_BIN_R] = mix_vol(c->fader + c->r + c->f);
        bins[RE4AX_BIN_S] = mix_vol(c->fader + c->b - 30);
        bins[RE4AX_BIN_AL] = mix_vol(fA + c->auxA + c->l + c->f);
        bins[RE4AX_BIN_AR] = mix_vol(fA + c->auxA + c->r + c->f);
        bins[RE4AX_BIN_AS] = mix_vol(fA + c->auxA + c->b - 30);
        bins[RE4AX_BIN_BL] = mix_vol(fB + c->auxB + c->l + c->f);
        bins[RE4AX_BIN_BR] = mix_vol(fB + c->auxB + c->r + c->f);
        bins[RE4AX_BIN_BS] = mix_vol(fB + c->auxB + c->b - 30);
        break;
    case MIX_SOUND_MODE_DPL2:
        // main L/R + rear in the AuxB L/R volumes; AuxA 4-channel (AS = Ls, BS = Rs)
        bins[RE4AX_BIN_L] = mix_vol(c->fader + c->l + c->f);
        bins[RE4AX_BIN_R] = mix_vol(c->fader + c->r + c->f);
        bins[RE4AX_BIN_BL] = mix_vol(c->fader + c->l1 + c->b);
        bins[RE4AX_BIN_BR] = mix_vol(c->fader + c->r1 + c->b);
        bins[RE4AX_BIN_AL] = mix_vol(fA + c->auxA + c->l + c->f);
        bins[RE4AX_BIN_AR] = mix_vol(fA + c->auxA + c->r + c->f);
        bins[RE4AX_BIN_AS] = mix_vol(fA + c->auxA + c->l1 + c->b);
        bins[RE4AX_BIN_BS] = mix_vol(fA + c->auxA + c->r1 + c->b);
        break;
    }
}

static void mix_write_pb(AXVPB* p, const u16* b, u16 ve)
{
    p->pb.ve.currentVolume = ve;
    p->pb.ve.currentDelta = 0;
    p->pb.mix.vL = b[RE4AX_BIN_L];      p->pb.mix.vR = b[RE4AX_BIN_R];      p->pb.mix.vS = b[RE4AX_BIN_S];
    p->pb.mix.vAuxAL = b[RE4AX_BIN_AL]; p->pb.mix.vAuxAR = b[RE4AX_BIN_AR]; p->pb.mix.vAuxAS = b[RE4AX_BIN_AS];
    p->pb.mix.vAuxBL = b[RE4AX_BIN_BL]; p->pb.mix.vAuxBR = b[RE4AX_BIN_BR]; p->pb.mix.vAuxBS = b[RE4AX_BIN_BS];
    p->pb.mixerCtrl = (u16) (s_mix_mode == MIX_SOUND_MODE_DPL2 ? 0x4000 : 0);
}

static void mix_apply(MIXChannel* c)
{
    AXVPB* p = c->axvpb;
    if (!vpb_ok(p)) return;
    u16 b[RE4AX_BIN_COUNT], ve;
    mix_compute(c, b, &ve);
    c->v = ve;
    c->vL = b[RE4AX_BIN_L]; c->vR = b[RE4AX_BIN_R]; c->vS = b[RE4AX_BIN_S];
    c->vAL = b[RE4AX_BIN_AL]; c->vAR = b[RE4AX_BIN_AR]; c->vAS = b[RE4AX_BIN_AS];
    c->vBL = b[RE4AX_BIN_BL]; c->vBR = b[RE4AX_BIN_BR]; c->vBS = b[RE4AX_BIN_BS];
    mix_write_pb(p, b, ve);
    set_gains(vidx(p), b, 1, 1, ve);
}

static MIXChannel* mixc(AXVPB* p) { return vpb_ok(p) ? &s_mix[vidx(p)] : 0; }

static void mix_release_index(int idx) { s_mix[idx].axvpb = 0; }

void MIXInit(void)
{
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) mix_reset(&s_mix[i]);
    s_mix_dvd_fader = 0;
    s_mix_mode = MIX_SOUND_MODE_STEREO;
}

void MIXQuit(void) {}

void MIXSetSoundMode(u32 mode) { s_mix_mode = mode; }
u32 MIXGetSoundMode(void) { return s_mix_mode; }

void MIXInitChannel(AXVPB* axvpb, u32 mode, int input, int auxA, int auxB, int pan, int span, int fader)
{
    MIXChannel* c = mixc(axvpb);
    if (!c) return;
    c->axvpb = axvpb;
    c->mode = mode & MIXF_MODE_MASK;
    c->input = input;
    c->auxA = auxA;
    c->auxB = auxB;
    c->pan = pan;
    c->span = span;
    c->fader = fader;
    mix_set_pan(c);
    // immediate, no ramp (MIXInitChannel writes the pb with zero deltas)
    s_vs[vidx(axvpb)].vol_primed = 0;
    mix_apply(c);
}

void MIXReleaseChannel(AXVPB* axvpb)
{
    MIXChannel* c = mixc(axvpb);
    if (c) c->axvpb = 0;
}

void MIXResetControls(AXVPB* p)
{
    MIXChannel* c = mixc(p);
    if (!c) return;
    AXVPB* keep = c->axvpb;
    mix_reset(c);
    c->axvpb = keep;
    c->mode |= MIXF_UPD_INPUT | MIXF_UPD_MIX;
}

#define MIX_SETTER(Name, field, flag, expr)                         \
    void MIXSet##Name(AXVPB* p, int v)                              \
    {                                                               \
        MIXChannel* c = mixc(p);                                    \
        if (!c) return;                                             \
        c->field = (expr);                                          \
        c->mode |= (flag);                                          \
    }                                                               \
    void MIXAdjust##Name(AXVPB* p, int v)                           \
    {                                                               \
        MIXChannel* c = mixc(p);                                    \
        if (!c) return;                                             \
        v = c->field + v;                                           \
        c->field = (expr);                                          \
        c->mode |= (flag);                                          \
    }                                                               \
    int MIXGet##Name(AXVPB* p)                                      \
    {                                                               \
        MIXChannel* c = mixc(p);                                    \
        return c ? c->field : 0;                                    \
    }

MIX_SETTER(Input, input, MIXF_UPD_INPUT, v)
MIX_SETTER(AuxA, auxA, MIXF_UPD_MIX, v)
MIX_SETTER(AuxB, auxB, MIXF_UPD_MIX, v)
MIX_SETTER(Pan, pan, MIXF_UPD_MIX, clamp_pan(v))
MIX_SETTER(SPan, span, MIXF_UPD_MIX, clamp_pan(v))
MIX_SETTER(Fader, fader, MIXF_UPD_MIX, v)

void MIXAuxAPostFader(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode &= ~1u; c->mode |= MIXF_UPD_MIX; } }
void MIXAuxAPreFader(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode |= 1u | MIXF_UPD_MIX; } }
int MIXAuxAIsPostFader(AXVPB* p) { MIXChannel* c = mixc(p); return c ? !(c->mode & 1u) : 0; }
void MIXAuxBPostFader(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode &= ~2u; c->mode |= MIXF_UPD_MIX; } }
void MIXAuxBPreFader(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode |= 2u | MIXF_UPD_MIX; } }
int MIXAuxBIsPostFader(AXVPB* p) { MIXChannel* c = mixc(p); return c ? !(c->mode & 2u) : 0; }
void MIXMute(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode |= 4u | MIXF_UPD_INPUT; } }
void MIXUnMute(AXVPB* p) { MIXChannel* c = mixc(p); if (c) { c->mode &= ~4u; c->mode |= MIXF_UPD_INPUT; } }
int MIXIsMute(AXVPB* p) { MIXChannel* c = mixc(p); return c ? (c->mode & 4u) != 0 : 0; }

void MIXSetDvdStreamFader(int dB)
{
    if (dB < -49) dB = -49;
    if (dB > 0) dB = 0;
    s_mix_dvd_fader = dB;
}
int MIXGetDvdStreamFader(void) { return s_mix_dvd_fader; }

void MIXUpdateSettings(void)
{
    for (int i = 0; i < RE4AX_MAX_VOICES; i++) {
        MIXChannel* c = &s_mix[i];
        if (!c->axvpb || !(c->mode & (MIXF_UPD_INPUT | MIXF_UPD_MIX))) continue;
        if (c->mode & MIXF_UPD_MIX) mix_set_pan(c);
        c->mode &= ~(MIXF_UPD_INPUT | MIXF_UPD_MIX);
        mix_apply(c);   // ramps from the gains of the frame just rendered
    }
}

// ================================================================================================
// AXFX — parameters are stored in the game's structs; the effects themselves are cheap stand-ins
// (Schroeder reverb for ReverbHi/Std/HiDpl2, a modulated delay for Chorus, a real feedback delay for
// Delay). State lives in a side table keyed by the struct address (the game passes &efx->rev_hi for
// every effect type, snd_efx.cpp:108-136, so the key is the address, not the type). Init/Settings/
// Shutdown return 1 on success (snd_efx.cpp:140). Work memory comes from AXFXSetHooks (game heap).
// ================================================================================================

static void* fx_default_alloc(u32 n) { return malloc(n); }
static void fx_default_free(void* p) { free(p); }
static void* (*s_fx_alloc)(u32) = fx_default_alloc;
static void (*s_fx_free)(void*) = fx_default_free;

void AXFXSetHooks(void* (*alloc)(u32), void (*free_)(void*))
{
    s_fx_alloc = alloc ? alloc : fx_default_alloc;
    s_fx_free = free_ ? free_ : fx_default_free;
}

enum { FX_NONE, FX_REVERB, FX_CHORUS, FX_DELAY };

#define FX_RATE        32000
#define FX_N           RE4AX_FRAME_SAMPLES
#define FX_COMBS       4
#define FX_APS         2
#define FX_MAX_PREDLY  3200            // 0.1 s
#define FX_CHO_LEN     1024            // 32 ms
#define FX_MAX_DELAY_MS 5000

static const int k_comb_len[FX_COMBS] = { 809, 861, 926, 983 };   // Freeverb lengths * 32/44.1
static const int k_ap_len[FX_APS] = { 403, 320 };
#define FX_SPREAD 17

struct FxReverb {
    float* comb[2][FX_COMBS];
    int    comb_pos[2][FX_COMBS];
    float  comb_lp[2][FX_COMBS];
    float  comb_g[2][FX_COMBS];
    float* ap[2][FX_APS];
    int    ap_pos[2][FX_APS];
    float* pre;
    int    pre_pos, pre_len;
    float  damp, wet;
};

struct FxChorus {
    float* line[4];
    int    pos;
    float  phase, base, depth, rate;
};

struct FxDelay {
    s32*   line[3];
    u32    len[3];
    u32    pos[3];
    u32    fb[3], out[3];
};

struct FxSlot {
    void* owner;
    int   kind;
    void* mem;      // one block from the hook allocator
    union { FxReverb rv; FxChorus ch; FxDelay dl; } u;
};

#define FX_SLOTS 8
static FxSlot s_fx[FX_SLOTS];

static FxSlot* fx_find(void* owner)
{
    for (int i = 0; i < FX_SLOTS; i++)
        if (s_fx[i].owner == owner && s_fx[i].kind != FX_NONE) return &s_fx[i];
    return 0;
}

static void fx_release(void* owner)
{
    FxSlot* s = fx_find(owner);
    if (!s) return;
    if (s->mem) s_fx_free(s->mem);
    memset(s, 0, sizeof(*s));
}

static FxSlot* fx_new(void* owner, int kind, u32 bytes)
{
    fx_release(owner);
    for (int i = 0; i < FX_SLOTS; i++) {
        FxSlot* s = &s_fx[i];
        if (s->kind != FX_NONE) continue;
        memset(s, 0, sizeof(*s));
        s->mem = bytes ? s_fx_alloc(bytes) : 0;
        if (bytes && !s->mem) return 0;
        if (s->mem) memset(s->mem, 0, bytes);
        s->owner = owner;
        s->kind = kind;
        return s;
    }
    re4_log("AXFX: out of effect slots\n");
    return 0;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void rv_settings(FxReverb* r, float time, float damping, float predelay, float mix, float coloration)
{
    time = clampf(time, 0.01f, 10.0f);
    // RT60: each comb loses 60 dB in `time` seconds; coloration shortens the low end a little
    for (int ch = 0; ch < 2; ch++)
        for (int k = 0; k < FX_COMBS; k++) {
            float len = (float) (k_comb_len[k] + ch * FX_SPREAD);
            r->comb_g[ch][k] = powf(10.0f, -3.0f * len / (time * (float) FX_RATE)) * (0.97f + 0.03f * clampf(coloration, 0.0f, 1.0f));
        }
    r->damp = clampf(damping, 0.0f, 1.0f) * 0.6f;
    r->pre_len = (int) (clampf(predelay, 0.0f, 0.1f) * (float) FX_RATE);
    if (r->pre_len >= FX_MAX_PREDLY) r->pre_len = FX_MAX_PREDLY - 1;
    r->wet = clampf(mix, 0.0f, 1.0f);
}

static u32 rv_bytes(void)
{
    u32 n = FX_MAX_PREDLY;
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < FX_COMBS; k++) n += (u32) (k_comb_len[k] + ch * FX_SPREAD);
        for (int k = 0; k < FX_APS; k++) n += (u32) (k_ap_len[k] + ch * FX_SPREAD);
    }
    return n * (u32) sizeof(float);
}

static int rv_init(void* owner, float time, float damping, float predelay, float mix, float coloration)
{
    FxSlot* s = fx_new(owner, FX_REVERB, rv_bytes());
    if (!s) return 0;
    FxReverb* r = &s->u.rv;
    float* m = (float*) s->mem;
    r->pre = m; m += FX_MAX_PREDLY;
    for (int ch = 0; ch < 2; ch++) {
        for (int k = 0; k < FX_COMBS; k++) { r->comb[ch][k] = m; m += k_comb_len[k] + ch * FX_SPREAD; }
        for (int k = 0; k < FX_APS; k++) { r->ap[ch][k] = m; m += k_ap_len[k] + ch * FX_SPREAD; }
    }
    rv_settings(r, time, damping, predelay, mix, coloration);
    return 1;
}

// in place: input = the AUX bus (n channels), output = the wet reverb (AX adds it to the main mix)
static void rv_process(FxReverb* r, s32** chans, int nch)
{
    for (int i = 0; i < (int) FX_N; i++) {
        float in = 0.0f;
        for (int c = 0; c < nch; c++) in += (float) chans[c][i];
        in *= 0.015f / (float) nch;   // Freeverb input gain
        float x = in;
        if (r->pre_len > 0) {
            int rp = r->pre_pos - r->pre_len;
            if (rp < 0) rp += FX_MAX_PREDLY;
            x = r->pre[rp];
            r->pre[r->pre_pos] = in;
            if (++r->pre_pos >= FX_MAX_PREDLY) r->pre_pos = 0;
        }
        float out[2];
        for (int ch = 0; ch < 2; ch++) {
            float acc = 0.0f;
            for (int k = 0; k < FX_COMBS; k++) {
                float* buf = r->comb[ch][k];
                int len = k_comb_len[k] + ch * FX_SPREAD;
                int p = r->comb_pos[ch][k];
                float y = buf[p];
                r->comb_lp[ch][k] = y * (1.0f - r->damp) + r->comb_lp[ch][k] * r->damp;
                buf[p] = x + r->comb_lp[ch][k] * r->comb_g[ch][k];
                if (++p >= len) p = 0;
                r->comb_pos[ch][k] = p;
                acc += y;
            }
            for (int k = 0; k < FX_APS; k++) {
                float* buf = r->ap[ch][k];
                int len = k_ap_len[k] + ch * FX_SPREAD;
                int p = r->ap_pos[ch][k];
                float b = buf[p];
                buf[p] = acc + b * 0.5f;
                acc = b - acc;
                if (++p >= len) p = 0;
                r->ap_pos[ch][k] = p;
            }
            out[ch] = acc * r->wet * 3.0f;   // Freeverb wet scale
        }
        s32 l = (s32) out[0], rr = (s32) out[1];
        chans[0][i] = l;
        if (nch > 1) chans[1][i] = rr;
        if (nch > 2) chans[2][i] = (l + rr) >> 2;   // a little into surround / Ls
        if (nch > 3) { chans[2][i] = l >> 1; chans[3][i] = rr >> 1; }
    }
}

int AXFXReverbHiInit(AXFX_REVERBHI* rev)
{
    if (!rev) return 0;
    return rv_init(rev, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
}
int AXFXReverbHiShutdown(AXFX_REVERBHI* rev) { fx_release(rev); return 1; }
int AXFXReverbHiSettings(AXFX_REVERBHI* rev)
{
    FxSlot* s = fx_find(rev);
    if (!s || s->kind != FX_REVERB) return AXFXReverbHiInit(rev);
    rv_settings(&s->u.rv, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
    return 1;
}
void AXFXReverbHiCallback(AXFX_BUFFERUPDATE* bu, AXFX_REVERBHI* rev)
{
    FxSlot* s = fx_find(rev);
    if (!bu || !rev || rev->tempDisableFX || !s || s->kind != FX_REVERB) return;
    s32* ch[3] = { bu->left, bu->right, bu->surround };
    rv_process(&s->u.rv, ch, 3);
}

int AXFXReverbHiInitDpl2(AXFX_REVERBHI_DPL2* rev)
{
    if (!rev) return 0;
    return rv_init(rev, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
}
int AXFXReverbHiShutdownDpl2(AXFX_REVERBHI_DPL2* rev) { fx_release(rev); return 1; }
int AXFXReverbHiSettingsDpl2(AXFX_REVERBHI_DPL2* rev)
{
    FxSlot* s = fx_find(rev);
    if (!s || s->kind != FX_REVERB) return AXFXReverbHiInitDpl2(rev);
    rv_settings(&s->u.rv, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
    return 1;
}
void AXFXReverbHiCallbackDpl2(AXFX_BUFFERUPDATE_DPL2* bu, AXFX_REVERBHI_DPL2* rev)
{
    FxSlot* s = fx_find(rev);
    if (!bu || !rev || rev->tempDisableFX || !s || s->kind != FX_REVERB) return;
    s32* ch[4] = { bu->L, bu->R, bu->Ls, bu->Rs };
    rv_process(&s->u.rv, ch, 4);
}

int AXFXReverbStdInit(AXFX_REVERBSTD* rev)
{
    if (!rev) return 0;
    return rv_init(rev, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
}
int AXFXReverbStdShutdown(AXFX_REVERBSTD* rev) { fx_release(rev); return 1; }
int AXFXReverbStdSettings(AXFX_REVERBSTD* rev)
{
    FxSlot* s = fx_find(rev);
    if (!s || s->kind != FX_REVERB) return AXFXReverbStdInit(rev);
    rv_settings(&s->u.rv, rev->time, rev->damping, rev->preDelay, rev->mix, rev->coloration);
    return 1;
}
void AXFXReverbStdCallback(AXFX_BUFFERUPDATE* bu, AXFX_REVERBSTD* rev)
{
    FxSlot* s = fx_find(rev);
    if (!bu || !rev || rev->tempDisableFX || !s || s->kind != FX_REVERB) return;
    s32* ch[3] = { bu->left, bu->right, bu->surround };
    rv_process(&s->u.rv, ch, 3);
}

// Chorus: baseDelay / variation in ms, period in ms (AXFX_CHORUS). Output = the modulated delay.
static void cho_settings(FxChorus* c, const AXFX_CHORUS* p)
{
    c->base = clampf((float) p->baseDelay, 1.0f, 25.0f) * (FX_RATE / 1000.0f);
    c->depth = clampf((float) p->variation, 0.0f, 5.0f) * (FX_RATE / 1000.0f);
    float period = clampf((float) p->period, 100.0f, 10000.0f);
    c->rate = 6.2831853f / (period * (FX_RATE / 1000.0f));
}

int AXFXChorusInit(AXFX_CHORUS* c)
{
    if (!c) return 0;
    FxSlot* s = fx_new(c, FX_CHORUS, 3u * FX_CHO_LEN * (u32) sizeof(float));
    if (!s) return 0;
    float* m = (float*) s->mem;
    for (int k = 0; k < 3; k++) s->u.ch.line[k] = m + k * FX_CHO_LEN;
    cho_settings(&s->u.ch, c);
    return 1;
}
int AXFXChorusShutdown(AXFX_CHORUS* c) { fx_release(c); return 1; }
int AXFXChorusSettings(AXFX_CHORUS* c)
{
    FxSlot* s = fx_find(c);
    if (!s || s->kind != FX_CHORUS) return AXFXChorusInit(c);
    cho_settings(&s->u.ch, c);
    return 1;
}
void AXFXChorusCallback(AXFX_BUFFERUPDATE* bu, AXFX_CHORUS* c)
{
    FxSlot* s = fx_find(c);
    if (!bu || !s || s->kind != FX_CHORUS) return;
    FxChorus* ch = &s->u.ch;
    s32* io[3] = { bu->left, bu->right, bu->surround };
    for (int i = 0; i < (int) FX_N; i++) {
        float d = ch->base + ch->depth * 0.5f * (1.0f + sinf(ch->phase));
        ch->phase += ch->rate;
        if (ch->phase > 6.2831853f) ch->phase -= 6.2831853f;
        int di = (int) d;
        float fr = d - (float) di;
        for (int k = 0; k < 3; k++) {
            float* line = ch->line[k];
            line[ch->pos] = (float) io[k][i];
            int a = ch->pos - di, b = a - 1;
            if (a < 0) a += FX_CHO_LEN;
            if (b < 0) b += FX_CHO_LEN;
            io[k][i] = (s32) (line[a] + (line[b] - line[a]) * fr);
        }
        if (++ch->pos >= FX_CHO_LEN) ch->pos = 0;
    }
}

// Delay: delay[] in ms, feedback[] / output[] in percent (AXFX_DELAY).
static int dl_setup(AXFX_DELAY* d)
{
    u32 len[3], total = 0;
    for (int k = 0; k < 3; k++) {
        u32 ms = d->delay[k];
        if (ms < 1) ms = 1;
        if (ms > FX_MAX_DELAY_MS) ms = FX_MAX_DELAY_MS;
        len[k] = ms * (FX_RATE / 1000);
        total += len[k];
    }
    FxSlot* s = fx_new(d, FX_DELAY, total * (u32) sizeof(s32));
    if (!s) return 0;
    s32* m = (s32*) s->mem;
    for (int k = 0; k < 3; k++) {
        s->u.dl.line[k] = m;
        m += len[k];
        s->u.dl.len[k] = len[k];
        s->u.dl.fb[k] = d->feedback[k] > 99 ? 99 : d->feedback[k];
        s->u.dl.out[k] = d->output[k] > 100 ? 100 : d->output[k];
        d->currentSize[k] = len[k];
        d->currentPos[k] = 0;
        d->currentFeedback[k] = s->u.dl.fb[k];
        d->currentOutput[k] = s->u.dl.out[k];
    }
    d->left = s->u.dl.line[0];
    d->right = s->u.dl.line[1];
    d->sur = s->u.dl.line[2];
    return 1;
}

int AXFXDelayInit(AXFX_DELAY* d) { return d ? dl_setup(d) : 0; }
int AXFXDelaySettings(AXFX_DELAY* d) { return d ? dl_setup(d) : 0; }   // SDK also reallocates
int AXFXDelayShutdown(AXFX_DELAY* d) { fx_release(d); return 1; }
void AXFXDelayCallback(AXFX_BUFFERUPDATE* bu, AXFX_DELAY* d)
{
    FxSlot* s = fx_find(d);
    if (!bu || !s || s->kind != FX_DELAY) return;
    FxDelay* dl = &s->u.dl;
    s32* io[3] = { bu->left, bu->right, bu->surround };
    for (int k = 0; k < 3; k++) {
        s32* line = dl->line[k];
        u32 pos = dl->pos[k], len = dl->len[k];
        for (int i = 0; i < (int) FX_N; i++) {
            s32 y = line[pos];
            line[pos] = io[k][i] + (s32) (((s64) y * dl->fb[k]) / 100);
            io[k][i] = (s32) (((s64) y * dl->out[k]) / 100);
            if (++pos >= len) pos = 0;
        }
        dl->pos[k] = pos;
        d->currentPos[k] = pos;
    }
}

// ================================================================================================
// AXART — the game only calls AXARTInit/Quit/ServiceSounds (snd_main.cpp, snd.cpp) and never adds
// a sound, so the sound list is kept but articulators are not evaluated.
// ================================================================================================

static AXART_SOUND* s_art_list;

void AXARTInit(void) { s_art_list = 0; }
void AXARTQuit(void) { s_art_list = 0; }
void AXARTServiceSounds(void)
{
    if (s_art_list) re4_log_once("axart", "AXART: articulators are not evaluated by the port\n");
}
void AXARTAddSound(AXART_SOUND* sound)
{
    if (!sound) return;
    sound->prev = 0;
    sound->next = s_art_list;
    if (s_art_list) s_art_list->prev = sound;
    s_art_list = sound;
}
void AXARTRemoveSound(AXART_SOUND* sound)
{
    if (!sound) return;
    if (sound->prev) ((AXART_SOUND*) sound->prev)->next = sound->next;
    else if (s_art_list == sound) s_art_list = (AXART_SOUND*) sound->next;
    if (sound->next) ((AXART_SOUND*) sound->next)->prev = sound->prev;
    sound->next = sound->prev = 0;
}

// ================================================================================================
// SYN — silent stub. The game runs its own MIDI parser (snd_seq*.cpp, Snd_midi_sequencer) and only
// feeds 3-byte messages to SYNMidiInput; it never reads SYNSYNTH fields and closes a stopped
// sequence once SYNGetActiveNotes() == 0 (snd_seq3.cpp:60), so "no notes ever sound" is a consistent
// state: MIDI sequences (bio4midi) are silent, everything else is unaffected. Controller state is
// kept so SYNGetMidiController answers. Upgrade path: 03-AUDIO.md section 8.
// ================================================================================================

void SYNInit(void) {}
void SYNQuit(void) {}
void SYNRunAudioFrame(void) {}

void SYNInitSynth(SYNSYNTH* synth, void* wavetable, u32 aramBase, u32 zeroBase, u32 priorityVoiceAlloc,
                  u32 priorityNoteOn, u32 priorityNoteRelease)
{
    (void) wavetable;
    if (!synth) return;
    synth->next = 0;
    synth->aramBaseByte = aramBase;
    synth->aramBaseWord = aramBase >> 1;
    synth->aramBaseNibble = aramBase << 1;
    synth->zeroBaseByte = zeroBase;
    synth->zeroBaseWord = zeroBase >> 1;
    synth->zeroBaseNibble = zeroBase << 1;
    synth->priorityVoiceAlloc = priorityVoiceAlloc;
    synth->priorityNoteOn = priorityNoteOn;
    synth->priorityNoteRelease = priorityNoteRelease;
    synth->masterVolume = 0;
    synth->notes = 0;
    memset(synth->controller, 0, sizeof(synth->controller));
    for (int ch = 0; ch < 16; ch++) {
        synth->controller[ch][7] = 100;    // volume
        synth->controller[ch][10] = 64;    // pan
        synth->controller[ch][11] = 127;   // expression
    }
}

void SYNQuitSynth(SYNSYNTH* synth)
{
    if (synth) synth->notes = 0;
}

void SYNMidiInput(SYNSYNTH* synth, u8* input)
{
    if (!synth || !input) return;
    u8 st = input[0], ch = (u8) (st & 0x0F);
    if ((st & 0xF0) == 0xB0 && input[1] < 128) synth->controller[ch][input[1]] = (u8) (input[2] & 0x7F);
}

void SYNSetMasterVolume(SYNSYNTH* synth, s32 dB) { if (synth) synth->masterVolume = dB; }
s32 SYNGetMasterVolume(SYNSYNTH* synth) { return synth ? synth->masterVolume : 0; }
u32 SYNGetActiveNotes(SYNSYNTH* synth) { (void) synth; return 0; }
u8 SYNGetMidiController(SYNSYNTH* synth, u8 midiChannel, u8 function)
{
    return (synth && midiChannel < 16 && function < 128) ? synth->controller[midiChannel][function] : 0;
}

// ================================================================================================
// SEQ — the SDK sequencer is initialised and ticked by the game (snd_main.cpp:16/89) but never given
// a sequence (the game sequences MIDI itself). Kept as a state holder.
// ================================================================================================

void SEQInit(void) {}
void SEQQuit(void) {}
void SEQRunAudioFrame(void) {}
void SEQAddSequence(SEQSEQUENCE* sequence, u8* midiStream, void* wt, u32 aramBase, u32 zeroBase,
                    u32 priorityVoiceAlloc, u32 priorityNoteOn, u32 priorityNoteRelease)
{
    (void) midiStream;
    if (!sequence) return;
    sequence->state = 0;
    sequence->end = 0;
    SYNInitSynth(&sequence->synth, wt, aramBase, zeroBase, priorityVoiceAlloc, priorityNoteOn, priorityNoteRelease);
}
void SEQRemoveSequence(SEQSEQUENCE* sequence) { if (sequence) sequence->state = 0; }
void SEQRegisterControllerCallback(SEQSEQUENCE* sequence, u8 controller, void (*callback)(void*, u8))
{
    if (sequence && controller < 128) sequence->callback[controller] = callback;
}
void SEQSetState(SEQSEQUENCE* sequence, u32 state) { if (sequence) sequence->state = state; }
u32 SEQGetState(SEQSEQUENCE* sequence) { return sequence ? sequence->state : 0; }
void SEQSetTempo(SEQSEQUENCE* sequence, u32 trackIndex, f32 bpm)
{
    if (sequence && trackIndex < 64) sequence->track[trackIndex].beatsPerSec = bpm / 60.0f;
}
f32 SEQGetTempo(SEQSEQUENCE* sequence, u32 trackIndex)
{
    return (sequence && trackIndex < 64) ? sequence->track[trackIndex].beatsPerSec * 60.0f : 0.0f;
}
void SEQSetVolume(SEQSEQUENCE* sequence, s32 dB) { if (sequence) SYNSetMasterVolume(&sequence->synth, dB); }
s32 SEQGetVolume(SEQSEQUENCE* sequence) { return sequence ? SYNGetMasterVolume(&sequence->synth) : 0; }

// ================================================================================================
// AI — the audio interface DMA is replaced by the backend's output; only the state calls remain.
// ================================================================================================

static int s_ai_init;
static u32 s_ai_dsp_rate = AI_SAMPLERATE_32KHZ;
static u32 s_ai_stream_rate = AI_SAMPLERATE_48KHZ;
static u8 s_ai_vol_l, s_ai_vol_r;
static AIDCallback s_ai_dma_cb;
static AISCallback s_ai_stream_cb;

void AIInit(u8* stack) { (void) stack; s_ai_init = 1; }
void AIReset(void) { s_ai_init = 0; }
BOOL AICheckInit(void) { return s_ai_init; }
AIDCallback AIRegisterDMACallback(AIDCallback callback) { AIDCallback o = s_ai_dma_cb; s_ai_dma_cb = callback; return o; }
AISCallback AIRegisterStreamCallback(AISCallback callback) { AISCallback o = s_ai_stream_cb; s_ai_stream_cb = callback; return o; }
void AISetDSPSampleRate(u32 rate) { s_ai_dsp_rate = rate; }
u32 AIGetDSPSampleRate(void) { return s_ai_dsp_rate; }
void AISetStreamSampleRate(u32 rate) { s_ai_stream_rate = rate; }
u32 AIGetStreamSampleRate(void) { return s_ai_stream_rate; }
void AISetStreamVolLeft(u8 vol) { s_ai_vol_l = vol; }
u8 AIGetStreamVolLeft(void) { return s_ai_vol_l; }
void AISetStreamVolRight(u8 vol) { s_ai_vol_r = vol; }
u8 AIGetStreamVolRight(void) { return s_ai_vol_r; }

} // extern "C"
