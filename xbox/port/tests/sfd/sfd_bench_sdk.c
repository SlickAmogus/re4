/* xbox/port/tests/sfd/sfd_bench_sdk.c -- minimal Dolphin SDK surface for the host Sofdec benchmark
 * (CT-07). Just enough for the CRI stack to play a movie headless on a 32-bit Windows host:
 *   DVD*  -> stdio on the one .sfd given on the command line (every DVDOpen of "*.sfd" maps to it),
 *            reads complete synchronously and are reported done at the next status poll;
 *   OS*   -> a virtual clock advanced by the driver (one 59.94 Hz field per sfd_bench_vsync());
 *   AX/AR -> a fake ARAM and voices whose play position advances with the virtual clock, so ADXT
 *            sees audio being consumed (the movie's master clock);
 *   misc  -> cache ops no-ops, MWCC runtime helpers.
 * Not the port's shims: those are built for nxdk/SDL and are exercised by their own tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <dolphin/ax.h>
#include <dolphin/ar.h>
#include <dolphin/mix.h>

/* ---- virtual time ---------------------------------------------------------------------------- */
#define BENCH_TIMER_HZ (162000000 / 4)
static long long s_vticks;
static unsigned s_vfield;

OSTime OSGetTime(void) { return (OSTime) s_vticks; }
OSTick OSGetTick(void) { return (OSTick) s_vticks; }
void VIWaitForRetrace(void) { }

/* ---- DVD ------------------------------------------------------------------------------------- */
const char *sfd_bench_path;
long long sfd_bench_dvd_bytes;
/* ADX PCM pushed to ARAM (bytes, and how many of the s16 samples are non-zero) */
long long sfd_bench_pcm_bytes, sfd_bench_pcm_nonzero;
static FILE *s_files[16];
static DVDFileInfo *s_pend[16];
static int s_npend;

int DVDOpen(const char *name, DVDFileInfo *fi)
{
	int i;
	FILE *f;
	size_t n = strlen(name);
	const char *path = (n > 4 && (strcmp(name + n - 4, ".sfd") == 0 || strcmp(name + n - 4, ".SFD") == 0)) ? sfd_bench_path : name;

	f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "DVDOpen: %s (%s) not found\n", name, path);
		return 0;
	}
	for (i = 1; i < 16 && s_files[i]; i++) {
	}
	if (i == 16) {
		fclose(f);
		return 0;
	}
	s_files[i] = f;
	memset(fi, 0, sizeof(*fi));
	fi->startAddr = (u32) i;
	fseek(f, 0, SEEK_END);
	fi->length = (u32) ftell(f);
	fi->cb.state = DVD_STATE_END;
	return 1;
}

int DVDClose(DVDFileInfo *fi)
{
	int i = (int) fi->startAddr;
	if (i > 0 && i < 16 && s_files[i]) {
		fclose(s_files[i]);
		s_files[i] = NULL;
	}
	return 1;
}

static void dvd_poll(void)
{
	while (s_npend > 0) {
		DVDFileInfo *fi = s_pend[--s_npend];
		fi->cb.state = DVD_STATE_END;
		if (fi->callback) {
			fi->callback((s32) fi->cb.transferredSize, fi);
		}
	}
}

int DVDReadAsyncPrio(DVDFileInfo *fi, void *addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
{
	int i = (int) fi->startAddr;
	size_t got = 0;
	(void) prio;
	if (i > 0 && i < 16 && s_files[i]) {
		fseek(s_files[i], offset, SEEK_SET);
		got = fread(addr, 1, (size_t) length, s_files[i]);
		if ((s32) got < length) {
			memset((u8 *) addr + got, 0, (size_t) length - got);
		}
	}
	sfd_bench_dvd_bytes += length;
	fi->callback = cb;
	fi->cb.addr = addr;
	fi->cb.offset = (u32) offset;
	fi->cb.length = (u32) length;
	fi->cb.transferredSize = (u32) length;
	fi->cb.state = DVD_STATE_BUSY;
	if (s_npend < 16) {
		s_pend[s_npend++] = fi;
	}
	return 1;
}

s32 DVDReadPrio(DVDFileInfo *fi, void *addr, s32 length, s32 offset, s32 prio)
{
	DVDReadAsyncPrio(fi, addr, length, offset, NULL, prio);
	dvd_poll();
	return length;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *cb)
{
	dvd_poll();
	return cb->state;
}

s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }
s32 DVDGetTransferredSize(DVDFileInfo *fi) { return (s32) fi->cb.transferredSize; }

s32 DVDCancel(volatile DVDCommandBlock *cb)
{
	int i;
	for (i = 0; i < s_npend; i++) {
		if ((volatile DVDCommandBlock *) &s_pend[i]->cb == cb) {
			s_pend[i] = s_pend[--s_npend];
			break;
		}
	}
	cb->state = DVD_STATE_CANCELED;
	return 0;
}

/* ---- ARAM ------------------------------------------------------------------------------------ */
#define BENCH_ARAM_SIZE (16 << 20)
static u8 *s_aram;
static u32 s_aram_top = 0x4000;

u32 ARAlloc(u32 length)
{
	u32 a = s_aram_top;
	s_aram_top += (length + 31) & ~31u;
	if (s_aram == NULL) {
		s_aram = (u8 *) calloc(1, BENCH_ARAM_SIZE);
	}
	return a;
}

u32 ARFree(u32 *length)
{
	if (length) {
		*length = 0;
	}
	return s_aram_top;
}

void ARQPostRequest(ARQRequest *req, u32 owner, u32 type, u32 prio, u32 src, u32 dst, u32 len, ARQCallback cb)
{
	req->next = NULL;
	req->owner = owner;
	req->type = type;
	req->priority = prio;
	req->source = src;
	req->dest = dst;
	req->length = len;
	req->callback = cb;
	if (s_aram == NULL) {
		s_aram = (u8 *) calloc(1, BENCH_ARAM_SIZE);
	}
	if (type == 0) { /* MRAM -> ARAM */
		if (dst + len <= BENCH_ARAM_SIZE) memcpy(s_aram + dst, (const void *) src, len);
		{
			const short *q = (const short *) src;
			u32 k;
			static FILE *pcm_f;
			static int pcm_init;
			if (!pcm_init) {
				/* SFD_BENCH_PCM=file: log every transfer as (u32 ARAM dst, u32 len, PCM) */
				const char *e = getenv("SFD_BENCH_PCM");
				pcm_init = 1;
				pcm_f = e ? fopen(e, "wb") : NULL;
			}
			if (pcm_f) {
				fwrite(&dst, 4, 1, pcm_f);
				fwrite(&len, 4, 1, pcm_f);
				fwrite(q, 1, len, pcm_f);
			}
			sfd_bench_pcm_bytes += len;
			for (k = 0; k < len / 2; k++) sfd_bench_pcm_nonzero += q[k] != 0;
		}
	} else {
		if (src + len <= BENCH_ARAM_SIZE) memcpy((void *) dst, s_aram + src, len);
	}
	if (cb) {
		cb((u32) req);
	}
}

/* ---- AX / MIX: voices consume samples with the virtual clock -------------------------------- */
#define BENCH_NVOICE 8
static AXVPB s_vpb[BENCH_NVOICE];
static int s_vused[BENCH_NVOICE];
static double s_vfrac[BENCH_NVOICE];

AXVPB *AXAcquireVoice(u32 priority, void (*callback)(void *), u32 userContext)
{
	int i;
	for (i = 0; i < BENCH_NVOICE; i++) {
		if (!s_vused[i]) {
			s_vused[i] = 1;
			memset(&s_vpb[i], 0, sizeof(AXVPB));
			s_vpb[i].priority = priority;
			s_vpb[i].callback = callback;
			s_vpb[i].userContext = userContext;
			s_vpb[i].index = (u32) i;
			s_vfrac[i] = 0;
			return &s_vpb[i];
		}
	}
	return NULL;
}

void AXFreeVoice(AXVPB *p) { s_vused[p - s_vpb] = 0; }
void AXSetVoiceAddr(AXVPB *p, AXPBADDR *addr) { p->pb.addr = *addr; }
void AXSetVoiceSrc(AXVPB *p, AXPBSRC *src) { p->pb.src = *src; }
void AXSetVoiceSrcType(AXVPB *p, u32 type) { p->pb.srcSelect = (u16) type; }
void AXSetVoiceState(AXVPB *p, u16 state) { p->pb.state = state; }
void MIXInitChannel(AXVPB *p, u32 mode, int input, int auxA, int auxB, int pan, int span, int fader) { }
void MIXReleaseChannel(AXVPB *p) { }
void MIXSetInput(AXVPB *p, int dB) { }
void MIXSetPan(AXVPB *p, int pan) { }

static u32 addr_get(u16 hi, u16 lo) { return ((u32) hi << 16) | lo; }

/* one 59.94 Hz field: advance the clock and every running voice (AX runs at 32 kHz; the SRC ratio
 * is 16.16) */
void sfd_bench_vsync(void)
{
	int i;
	s_vfield++;
	s_vticks = (long long) ((double) s_vfield * BENCH_TIMER_HZ / 59.94);
	for (i = 0; i < BENCH_NVOICE; i++) {
		AXVPB *p = &s_vpb[i];
		if (s_vused[i] && p->pb.state == 1) {
			double ratio = ((p->pb.src.ratioHi << 16) | p->pb.src.ratioLo) / 65536.0;
			u32 cur = addr_get(p->pb.addr.currentAddressHi, p->pb.addr.currentAddressLo);
			u32 end = addr_get(p->pb.addr.endAddressHi, p->pb.addr.endAddressLo);
			u32 loop = addr_get(p->pb.addr.loopAddressHi, p->pb.addr.loopAddressLo);
			double adv = 32000.0 / 59.94 * (ratio > 0 ? ratio : 1.0) + s_vfrac[i];
			u32 n = (u32) adv;
			s_vfrac[i] = adv - n;
			cur += n;
			if (end > loop && cur > end) {
				cur = loop + (cur - end - 1) % (end - loop + 1);
			}
			p->pb.addr.currentAddressHi = (u16) (cur >> 16);
			p->pb.addr.currentAddressLo = (u16) cur;
		}
	}
}

/* ---- OS -------------------------------------------------------------------------------------- */
static OSThread s_main_thread;
BOOL OSDisableInterrupts(void) { return 1; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }
OSThread *OSGetCurrentThread(void) { return &s_main_thread; }
s32 OSGetThreadPriority(OSThread *t) { return 16; }
BOOL OSSetThreadPriority(OSThread *t, s32 prio) { return 1; }
void OSYieldThread(void) { }
BOOL OSCreateThread(OSThread *t, void *(*func)(void *), void *param, void *stack, u32 size, OSPriority prio, u16 attr)
{
	fprintf(stderr, "OSCreateThread called (CRI threads are dead-stripped in this game)\n");
	return 0;
}
int OSJoinThread(OSThread *t, void *val) { return 1; }
void DCFlushRange(void *p, u32 n) { }
void DCStoreRange(void *p, u32 n) { }
void DCInvalidateRange(void *p, u32 n) { }

/* ---- MWCC runtime ---------------------------------------------------------------------------- */
/* adx_tlk.c declares `Uint32 __cvt_fp2unsigned(Float32)`, adx_dcd.c `extern float __float_nan` */
unsigned int __cvt_fp2unsigned(float f) { return f <= 0.0f ? 0u : f >= 4294967295.0f ? 0xFFFFFFFFu : (unsigned int) f; }
float __float_nan = __builtin_nanf("");

void re4_cri_asm_unported(const char *file, int line)
{
	fprintf(stderr, "unported CRI asm reached at %s:%d\n", file, line);
	abort();
}
