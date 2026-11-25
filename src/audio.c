/*
 * Game audio capture for the UVC plugin.
 *
 * Approach (taken from remotejoy-audio, simplified):
 *  - the sceAudio output syscalls are replaced by wrappers. Each wrapper runs
 *    the original first, so the firmware's own argument validation applies,
 *    and only then copies the PCM that was submitted;
 *  - every hardware channel (and the SRC/Output2 channel) is summed, with the
 *    volume the game asked for, onto one timeline indexed by real playback
 *    time, so channels that started at different moments stay aligned;
 *  - the USB side pulls that timeline back out one isochronous packet at a
 *    time, a fixed distance behind "now".
 *
 * Differences from remotejoy-audio:
 *  - the hooks are installed when the plugin starts, before the game has
 *    reserved any channel, so every channel's format and length is learned
 *    from sceAudioChReserve & co. directly. No scanning of the audio driver's
 *    code or private tables, no hardcoded addresses;
 *  - the syscall table is patched through SystemControl's
 *    sctrlHENPatchSyscall, looked up at run time. If the running CFW does not
 *    export it the module still loads and the audio is simply silent;
 *  - a channel keeps its own write cursor so consecutive buffers land
 *    back to back instead of wherever the scheduler happened to run the game
 *    thread, and non-44.1 kHz SRC sources are resampled.
 */
#include <stdint.h>
#include <string.h>
#include <pspkernel.h>
#include <pspintrman.h>
#include <psploadcore.h>
#include "audio.h"
#include "log.h"

/* Provided by stubs/LoadCoreForKernel.S (kernel import). */
extern SceModule *sceKernelFindModuleByName(const char *modname);

/* sceAudio_driver "sceAudio" library NIDs, checked against the SDK stubs. */
#define NID_ChReserve			0x5EC81C55	/* (ch, samples, format)	*/
#define NID_SetChannelDataLen		0xCB2E439E	/* (ch, samples)		*/
#define NID_ChangeChannelConfig		0x95FD0C2D	/* (ch, format)			*/
#define NID_Output			0x8C1009B2	/* (ch, vol, buf)		*/
#define NID_OutputBlocking		0x136CAF51	/* (ch, vol, buf)		*/
#define NID_OutputPanned		0xE2D56B2D	/* (ch, lvol, rvol, buf)	*/
#define NID_OutputPannedBlocking	0x13F592BC	/* (ch, lvol, rvol, buf)	*/
#define NID_SRCChReserve		0x38553111	/* (samples, freq, channels)	*/
#define NID_SRCOutputBlocking		0xE0727056	/* (vol, buf)			*/
#define NID_Output2Reserve		0x01562BA3	/* (samples)			*/
#define NID_Output2OutputBlocking	0x2D53F36E	/* (vol, buf)			*/
#define NID_Output2ChangeLength		0x63F2889C	/* (samples)			*/

/* SystemControl (every PRO/ARK/ME CFW): void sctrlHENPatchSyscall(void *addr, void *newaddr) */
#define NID_sctrlHENPatchSyscall	0x826668E9

#define AUDIO_RING_MASK		(AUDIO_RING_FRAMES - 1)
#define AUDIO_HW_CHANNELS	8
#define AUDIO_CHAN_SRC		8			/* SRC and Output2 share one channel */
#define AUDIO_CHAN_COUNT	9

/* The read cursor trails real time by this much so every channel has finished
 * writing the stretch being read (must exceed one channel buffer's jitter). */
#define AUDIO_LAG_FRAMES	1103			/* 25 ms */
#define AUDIO_READ_RESYNC	2205			/* 50 ms: give up and re-align */
/* A channel's own cursor is re-aligned to real time when it is this far off. */
#define AUDIO_CHAN_BEHIND	1500
#define AUDIO_CHAN_AHEAD	2400

#define AUDIO_VOL_MAX		0xFFFF			/* 0x8000 is unity */

/* Where the timeline came from: 60 s is exactly 2646000 frames, so rebasing
 * the 32-bit microsecond clock every minute never drifts or wraps. */
#define AUDIO_REBASE_US		60000000u
#define AUDIO_REBASE_FRAMES	2646000u

/* user + kernel RAM on every model (the original validated the pointer already) */
#define RAM_LO			0x08000000u
#define RAM_HI			0x0C000000u
#define VRAM_LO			0x04000000u
#define VRAM_HI			0x04200000u

static short *ring;			/* AUDIO_RING_FRAMES stereo frames */
static volatile int active;

static u32 mix_drain;			/* next absolute frame the USB side reads */
static u32 clock_t0;
static u32 clock_frames;
static int clock_valid;

struct chan {
	u32 next;			/* absolute frame the next buffer starts at */
	int valid;
	int samples;			/* frames per buffer, 0 = unknown */
	int mono;
};
static struct chan chans[AUDIO_CHAN_COUNT];
static int src_rate = AUDIO_RATE;

static struct audio_stats stats;

/* Start-up self-test: a short beep at the head of every stream the host opens,
 * so "the host receives samples" can be told apart from "the game was silent".
 * Build with UVC_AUDIO_BEEP=0 to remove it. */
#ifndef UVC_AUDIO_BEEP
#define UVC_AUDIO_BEEP		1
#endif
#define BEEP_FRAMES		(AUDIO_RATE / 2)
#define BEEP_HZ			440
#define BEEP_LEVEL		8000
static int beep_left;
static int beep_phase;
static unsigned int peak_in_acc, peak_out_acc;

static struct audio_event events[AUDIO_EVENTS_MAX];
static volatile int event_count;

enum {
	H_ChReserve, H_SetChannelDataLen, H_ChangeChannelConfig, H_Output, H_OutputBlocking,
	H_OutputPanned, H_OutputPannedBlocking, H_SRCChReserve, H_SRCOutputBlocking,
	H_Output2Reserve, H_Output2OutputBlocking, H_Output2ChangeLength, H_COUNT
};

/* Count every wrapper entry, and remember the first few with their arguments. */
static void rec(int hook, int a0, int a1, int a2, int ret)
{
	int n;

	stats.wrapper_calls[hook]++;
	n = event_count;
	if (n >= AUDIO_EVENTS_MAX)
		return;
	events[n].time_us = sceKernelGetSystemTimeLow();
	events[n].hook = hook;
	events[n].a0 = a0;
	events[n].a1 = a1;
	events[n].a2 = a2;
	events[n].ret = ret;
	events[n].active = active;
	event_count = n + 1;
}

/* Originals, filled in before the matching syscall is patched. */
static int (*orig_ChReserve)(int, int, int);
static int (*orig_SetChannelDataLen)(int, int);
static int (*orig_ChangeChannelConfig)(int, int);
static int (*orig_Output)(int, int, void *);
static int (*orig_OutputBlocking)(int, int, void *);
static int (*orig_OutputPanned)(int, int, int, void *);
static int (*orig_OutputPannedBlocking)(int, int, int, void *);
static int (*orig_SRCChReserve)(int, int, int);
static int (*orig_SRCOutputBlocking)(int, void *);
static int (*orig_Output2Reserve)(int);
static int (*orig_Output2OutputBlocking)(int, void *);
static int (*orig_Output2ChangeLength)(int);

static void (*patch_syscall)(void *addr, void *newaddr);

/* ---------------------------------------------------------------------- */
/* Timeline                                                                */
/* ---------------------------------------------------------------------- */

/* Absolute frame number of "now". Call with interrupts suspended. */
static u32 now_frame(void)
{
	u32 now = sceKernelGetSystemTimeLow();
	u32 dus;

	if (!clock_valid) {
		clock_t0 = now;
		clock_frames = 0;
		clock_valid = 1;
	}
	dus = now - clock_t0;
	if (dus >= AUDIO_REBASE_US) {
		u32 n = dus / AUDIO_REBASE_US;

		clock_t0 += n * AUDIO_REBASE_US;
		clock_frames += n * AUDIO_REBASE_FRAMES;
		dus -= n * AUDIO_REBASE_US;
	}
	/* dus < 60e6, so neither product overflows 32 bits */
	return clock_frames + (dus / 10000u) * 441u + ((dus % 10000u) * 441u) / 10000u;
}

static short sat_add(short a, short b)
{
	int v = (int)a + (int)b;

	if (v > 32767)
		v = 32767;
	if (v < -32768)
		v = -32768;
	return (short)v;
}

static int ram_buf_ok(const void *buf, int bytes)
{
	u32 a = (u32)buf;
	u32 e;

	if (buf == NULL || bytes <= 0 || (a & 1))
		return 0;
	a &= 0x1FFFFFFF;		/* fold the cached/uncached/kernel bits */
	e = a + (u32)bytes;
	if (e < a)
		return 0;
	if (a >= RAM_LO && e <= RAM_HI)
		return 1;
	if (a >= VRAM_LO && e <= VRAM_HI)
		return 1;
	return 0;
}

/*
 * Mix one submitted buffer onto the timeline.
 * who: hardware channel 0-7, or AUDIO_CHAN_SRC. rate: the buffer's sample rate.
 */
static void mix_in(const void *buf, int frames, int mono, int who, int lvol, int rvol, int rate)
{
	const short *src = (const short *)buf;
	struct chan *c;
	u32 intr, pos, start0;
	u32 step = 0;
	int out_frames, horizon, skip, j;

	if (!active || ring == NULL)
		return;
	if (frames <= 0 || frames > 4096 || (unsigned int)who >= AUDIO_CHAN_COUNT)
		return;
	if (lvol < 0)
		lvol = 0;
	if (lvol > AUDIO_VOL_MAX)
		lvol = AUDIO_VOL_MAX;
	if (rvol < 0)
		rvol = 0;
	if (rvol > AUDIO_VOL_MAX)
		rvol = AUDIO_VOL_MAX;
	stats.calls++;
	stats.channels_seen |= 1u << who;
	if (lvol == 0 && rvol == 0)
		return;

	if (!ram_buf_ok(buf, mono ? frames * 2 : frames * 4)) {
		stats.bad_ptr++;
		return;
	}

	if (rate <= 0 || rate == AUDIO_RATE) {
		rate = AUDIO_RATE;
		out_frames = frames;
	} else {
		out_frames = (int)(((u32)frames * AUDIO_RATE) / (u32)rate);
		step = ((u32)rate << 16) / AUDIO_RATE;		/* source frames per output frame, 16.16 */
	}
	if (out_frames <= 0)
		return;

	intr = sceKernelCpuSuspendIntr();
	pos = now_frame();
	c = &chans[who];
	if (!c->valid || (int)(c->next - pos) > AUDIO_CHAN_AHEAD ||
	    (int)(pos - c->next) > AUDIO_CHAN_BEHIND) {
		if (c->valid)
			stats.chan_resyncs++;
		c->next = pos;
		c->valid = 1;
	}
	start0 = c->next;
	c->next += (u32)out_frames;

	/* the part already behind the read cursor can no longer be sent */
	skip = (int)(mix_drain - start0);
	if (skip < 0)
		skip = 0;
	/* and the part too far ahead has no room in the ring */
	horizon = (int)((mix_drain + AUDIO_RING_FRAMES - 64) - start0);
	if (horizon < 0)
		horizon = 0;
	if (skip > out_frames)
		skip = out_frames;
	stats.frames_late += (unsigned int)skip;
	if (out_frames > horizon) {
		stats.frames_overflow += (unsigned int)(out_frames - (horizon > skip ? horizon : skip));
		out_frames = horizon;
	}
	sceKernelCpuResumeIntr(intr);

	if (skip >= out_frames)
		return;
	stats.frames_in += (unsigned int)(out_frames - skip);

	/*
	 * Interrupts are on again. The read cursor is AUDIO_LAG_FRAMES behind
	 * real time and this span starts at or after real time, so the reader
	 * is not touching these slots.
	 */
	for (j = skip; j < out_frames; j++) {
		unsigned int idx = ((start0 + (u32)j) & AUDIO_RING_MASK) * 2;
		int l, r;

		if (step == 0) {
			if (mono) {
				l = r = src[j];
			} else {
				l = src[j * 2];
				r = src[j * 2 + 1];
			}
		} else {
			u32 p = (u32)j * step;
			int s0 = (int)(p >> 16);
			int s1 = (s0 + 1 < frames) ? s0 + 1 : s0;
			int fr = (int)(p & 0xFFFF);

			if (s0 >= frames)
				s0 = s1 = frames - 1;
			if (mono) {
				l = src[s0] + (((src[s1] - src[s0]) * fr) >> 16);
				r = l;
			} else {
				l = src[s0 * 2] + (((src[s1 * 2] - src[s0 * 2]) * fr) >> 16);
				r = src[s0 * 2 + 1] + (((src[s1 * 2 + 1] - src[s0 * 2 + 1]) * fr) >> 16);
			}
		}
		/* apply the volume the game asked for, as the hardware does */
		l = (l * lvol) >> 15;
		r = (r * rvol) >> 15;
		if (l > 32767)
			l = 32767;
		if (l < -32768)
			l = -32768;
		if (r > 32767)
			r = 32767;
		if (r < -32768)
			r = -32768;
		ring[idx] = sat_add(ring[idx], (short)l);
		ring[idx + 1] = sat_add(ring[idx + 1], (short)r);
		{
			unsigned int al = (unsigned int)(l < 0 ? -l : l);
			unsigned int ar = (unsigned int)(r < 0 ? -r : r);

			if (al > peak_in_acc)
				peak_in_acc = al;
			if (ar > peak_in_acc)
				peak_in_acc = ar;
		}
	}
}

/* ret = queued sample count for blocking calls, 0 for the others */
static int pick_frames(int recorded, int ret)
{
	if (ret <= 0)
		return recorded;
	if (recorded <= 0 || recorded > ret)
		return ret;
	return recorded;
}

static void capture_hw(int ch, int lvol, int rvol, void *buf, int ret)
{
	int frames;

	if (ret < 0 || (unsigned int)ch >= AUDIO_HW_CHANNELS || !active)
		return;
	frames = pick_frames(chans[ch].samples, ret);
	if (frames <= 0) {
		stats.unknown_len++;
		return;
	}
	mix_in(buf, frames, chans[ch].mono, ch, lvol, rvol, AUDIO_RATE);
}

static void capture_src(int vol, void *buf, int ret, int mono, int rate)
{
	int frames;

	if (ret < 0 || !active)
		return;
	frames = pick_frames(chans[AUDIO_CHAN_SRC].samples, ret);
	if (frames <= 0) {
		stats.unknown_len++;
		return;
	}
	mix_in(buf, frames, mono, AUDIO_CHAN_SRC, vol, vol, rate);
}

/* ---------------------------------------------------------------------- */
/* Hook wrappers: original first, capture after it succeeded               */
/* ---------------------------------------------------------------------- */

static int my_ChReserve(int ch, int samples, int format)
{
	int ret = orig_ChReserve(ch, samples, format);

	rec(H_ChReserve, ch, samples, format, ret);

	if ((u32)ret < AUDIO_HW_CHANNELS) {
		chans[ret].samples = samples;
		chans[ret].mono = (format & 0x10) != 0;
		chans[ret].valid = 0;
	}
	return ret;
}

static int my_SetChannelDataLen(int ch, int samples)
{
	int ret = orig_SetChannelDataLen(ch, samples);

	rec(H_SetChannelDataLen, ch, samples, 0, ret);

	if (ret >= 0 && (u32)ch < AUDIO_HW_CHANNELS)
		chans[ch].samples = samples;
	return ret;
}

static int my_ChangeChannelConfig(int ch, int format)
{
	int ret = orig_ChangeChannelConfig(ch, format);

	rec(H_ChangeChannelConfig, ch, format, 0, ret);

	if (ret >= 0 && (u32)ch < AUDIO_HW_CHANNELS)
		chans[ch].mono = (format & 0x10) != 0;
	return ret;
}

static int my_Output(int ch, int vol, void *buf)
{
	int ret = orig_Output(ch, vol, buf);

	rec(H_Output, ch, vol, (int)buf, ret);
	capture_hw(ch, vol, vol, buf, ret);
	return ret;
}

static int my_OutputBlocking(int ch, int vol, void *buf)
{
	int ret = orig_OutputBlocking(ch, vol, buf);

	rec(H_OutputBlocking, ch, vol, (int)buf, ret);
	capture_hw(ch, vol, vol, buf, ret);
	return ret;
}

static int my_OutputPanned(int ch, int lvol, int rvol, void *buf)
{
	int ret = orig_OutputPanned(ch, lvol, rvol, buf);

	rec(H_OutputPanned, ch, lvol, rvol, ret);
	capture_hw(ch, lvol, rvol, buf, ret);
	return ret;
}

static int my_OutputPannedBlocking(int ch, int lvol, int rvol, void *buf)
{
	int ret = orig_OutputPannedBlocking(ch, lvol, rvol, buf);

	rec(H_OutputPannedBlocking, ch, lvol, rvol, ret);
	capture_hw(ch, lvol, rvol, buf, ret);
	return ret;
}

static int my_SRCChReserve(int samples, int freq, int channels)
{
	int ret = orig_SRCChReserve(samples, freq, channels);

	rec(H_SRCChReserve, samples, freq, channels, ret);

	if (ret >= 0) {
		chans[AUDIO_CHAN_SRC].samples = samples;
		chans[AUDIO_CHAN_SRC].mono = (channels == 1);
		chans[AUDIO_CHAN_SRC].valid = 0;
		src_rate = freq > 0 ? freq : AUDIO_RATE;
	}
	return ret;
}

static int my_SRCOutputBlocking(int vol, void *buf)
{
	int ret = orig_SRCOutputBlocking(vol, buf);

	rec(H_SRCOutputBlocking, vol, (int)buf, 0, ret);
	capture_src(vol, buf, ret, chans[AUDIO_CHAN_SRC].mono, src_rate);
	return ret;
}

/* Output2 is the same underlying channel: fixed 44.1 kHz stereo */
static int my_Output2Reserve(int samples)
{
	int ret = orig_Output2Reserve(samples);

	rec(H_Output2Reserve, samples, 0, 0, ret);

	if (ret >= 0) {
		chans[AUDIO_CHAN_SRC].samples = samples;
		chans[AUDIO_CHAN_SRC].mono = 0;
		chans[AUDIO_CHAN_SRC].valid = 0;
		src_rate = AUDIO_RATE;
	}
	return ret;
}

static int my_Output2ChangeLength(int samples)
{
	int ret = orig_Output2ChangeLength(samples);

	rec(H_Output2ChangeLength, samples, 0, 0, ret);

	if (ret >= 0)
		chans[AUDIO_CHAN_SRC].samples = samples;
	return ret;
}

static int my_Output2OutputBlocking(int vol, void *buf)
{
	int ret = orig_Output2OutputBlocking(vol, buf);

	rec(H_Output2OutputBlocking, vol, (int)buf, 0, ret);
	capture_src(vol, buf, ret, 0, AUDIO_RATE);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Installing the hooks                                                    */
/* ---------------------------------------------------------------------- */

struct hook {
	u32 nid;
	const char *name;
	void *wrapper;
	void **orig;		/* receives the original function */
	int installed;
};

static struct hook hooks[] = {
	{ NID_ChReserve,		"ChReserve",		my_ChReserve,		(void **)&orig_ChReserve },
	{ NID_SetChannelDataLen,	"SetChannelDataLen",	my_SetChannelDataLen,	(void **)&orig_SetChannelDataLen },
	{ NID_ChangeChannelConfig,	"ChangeChannelConfig",	my_ChangeChannelConfig,	(void **)&orig_ChangeChannelConfig },
	{ NID_Output,			"Output",		my_Output,		(void **)&orig_Output },
	{ NID_OutputBlocking,		"OutputBlocking",	my_OutputBlocking,	(void **)&orig_OutputBlocking },
	{ NID_OutputPanned,		"OutputPanned",		my_OutputPanned,	(void **)&orig_OutputPanned },
	{ NID_OutputPannedBlocking,	"OutputPannedBlocking",	my_OutputPannedBlocking, (void **)&orig_OutputPannedBlocking },
	{ NID_SRCChReserve,		"SRCChReserve",		my_SRCChReserve,	(void **)&orig_SRCChReserve },
	{ NID_SRCOutputBlocking,	"SRCOutputBlocking",	my_SRCOutputBlocking,	(void **)&orig_SRCOutputBlocking },
	{ NID_Output2Reserve,		"Output2Reserve",	my_Output2Reserve,	(void **)&orig_Output2Reserve },
	{ NID_Output2OutputBlocking,	"Output2OutputBlocking", my_Output2OutputBlocking, (void **)&orig_Output2OutputBlocking },
	{ NID_Output2ChangeLength,	"Output2ChangeLength",	my_Output2ChangeLength,	(void **)&orig_Output2ChangeLength },
};
#define HOOK_COUNT	((int)(sizeof(hooks) / sizeof(hooks[0])))

static int hooks_in_place;

/* Only dereference what looks like RAM; a wild read can bus-error the console. */
static int kptr_ok(const void *p, u32 bytes)
{
	u32 a = (u32)p & 0x1FFFFFFF;

	return p != NULL && a >= RAM_LO && a + bytes <= RAM_HI && a + bytes >= a;
}

/* Walk a module's export tables for the function with this NID. */
static void *find_export(const SceModule *m, const char *lib, u32 nid)
{
	u32 off = 0;

	if (m == NULL || !kptr_ok(m->ent_top, 16) || m->ent_size == 0 || m->ent_size > 0x2000)
		return NULL;

	while (off + sizeof(SceLibraryEntryTable) <= m->ent_size) {
		const SceLibraryEntryTable *e = (const SceLibraryEntryTable *)((const u8 *)m->ent_top + off);
		u32 total;
		u32 i;
		const u32 *tbl;

		if (e->len == 0)
			break;
		if (kptr_ok(e->libname, 1) && strcmp(e->libname, lib) == 0 &&
		    kptr_ok(e->entrytable, 8)) {
			total = (u32)e->stubcount + (u32)e->vstubcount;
			tbl = (const u32 *)e->entrytable;
			if (kptr_ok(tbl, total * 8)) {
				for (i = 0; i < e->stubcount; i++) {
					if (tbl[i] == nid)
						return (void *)tbl[total + i];
				}
			}
		}
		off += (u32)e->len * 4;
	}
	return NULL;
}

/*
 * Refuse to call a function that is not what its NID claims. Every
 * sctrlHENPatchSyscall implementation (PRO, ARK, ME) finds the syscall table
 * with "cfc0 rt, $12"; if that instruction is not near the start of the
 * resolved function, the NID was wrong for this CFW and calling it would crash.
 */
static int reads_syscall_table_reg(const void *fn)
{
	const u32 *w = (const u32 *)fn;
	int i;

	if (!kptr_ok(fn, 64 * 4))
		return 0;
	for (i = 0; i < 64; i++) {
		if ((w[i] & 0xFFE0FFFFu) == 0x40406000u)	/* cfc0 $rt, $12 */
			return 1;
	}
	return 0;
}

int audio_capture_init(void *ring_mem)
{
	SceModule *audio_mod;
	SceModule *sysctrl_mod;
	int n = 0;
	int i;

	if (hooks_in_place)
		return hooks_in_place;

	memset(&stats, 0, sizeof(stats));
	event_count = 0;
	memset(chans, 0, sizeof(chans));
	src_rate = AUDIO_RATE;
	ring = (short *)ring_mem;
	if (ring != NULL)
		memset(ring, 0, AUDIO_RING_BYTES);

	audio_mod = sceKernelFindModuleByName("sceAudio_Driver");
	if (audio_mod == NULL) {
		LOG("audio: module sceAudio_Driver not found; audio capture off\n");
		return -1;
	}
	sysctrl_mod = sceKernelFindModuleByName("SystemControl");
	patch_syscall = (void (*)(void *, void *))
		find_export(sysctrl_mod, "SystemCtrlForKernel", NID_sctrlHENPatchSyscall);
	if (patch_syscall == NULL) {
		LOG("audio: sctrlHENPatchSyscall not found (SystemControl %s); audio capture off\n",
		    sysctrl_mod ? "present" : "missing");
		return -2;
	}
	if (!reads_syscall_table_reg((void *)patch_syscall)) {
		LOG("audio: function %p for NID 0x%08X does not look like sctrlHENPatchSyscall; "
		    "audio capture off\n", (void *)patch_syscall, NID_sctrlHENPatchSyscall);
		patch_syscall = NULL;
		log_flush();
		return -3;
	}
	/* From here on a mistake can take the console down before the log's writer
	 * thread runs: write out what was learned so far, and again after each hook. */
	LOG("audio: sctrlHENPatchSyscall at %p, sceAudio_Driver at %p; patching\n",
	    (void *)patch_syscall, (void *)audio_mod);
	log_flush();

	for (i = 0; i < HOOK_COUNT; i++) {
		void *func = find_export(audio_mod, "sceAudio", hooks[i].nid);

		if (func == NULL) {
			LOG("audio: sceAudio %s (0x%08X) not exported\n", hooks[i].name, hooks[i].nid);
			continue;
		}
		*hooks[i].orig = func;		/* before the patch: the wrapper may run at once */
		patch_syscall(func, hooks[i].wrapper);
		hooks[i].installed = 1;
		stats.hooks_mask |= 1u << i;
		n++;
		LOG("audio: hooked %s (%p -> %p)\n", hooks[i].name, func, hooks[i].wrapper);
		log_flush();
	}
	/* make sure the patched table is what every CPU path sees */
	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();

	{
		struct audio_probe pr;

		audio_probe_table(&pr);
		LOG("audio: syscall table probe: base A %p wrappers %u origs %u; base B %p wrappers %u origs %u\n",
		    (void *)pr.base[0], pr.wrappers[0], pr.origs[0], (void *)pr.base[1],
		    pr.wrappers[1], pr.origs[1]);
		log_flush();
	}
	hooks_in_place = n;
	LOG("audio: %d of %d sceAudio hooks installed (mask 0x%03X)\n", n, HOOK_COUNT, stats.hooks_mask);
	return n;
}

static int usbbus_state;		/* 0 untried, 1 ready, -1 unusable */

void audio_capture_shutdown(void)
{
	int i;

	active = 0;
	if (patch_syscall != NULL) {
		for (i = HOOK_COUNT - 1; i >= 0; i--) {
			if (hooks[i].installed) {
				/* every slot that holds the wrapper gets the original back */
				patch_syscall(hooks[i].wrapper, *hooks[i].orig);
				hooks[i].installed = 0;
			}
		}
		sceKernelDcacheWritebackInvalidateAll();
		sceKernelIcacheInvalidateAll();
	}
	if (usbbus_state == 1)
		usbbus_target_patch(1, 1);	/* stock behaviour */
	hooks_in_place = 0;
	/* ring stays valid until the caller frees its memory */
}

/* ---------------------------------------------------------------------- */
/* USB bus driver frame-target experiment                                  */
/* ---------------------------------------------------------------------- */

/*
 * sceUSB_Driver arms an isochronous IN request from its SOF interrupt: it
 * reads the current frame number FN and gives the DMA descriptor the frame
 * (FN + 1) | ((1 << (bInterval - 1)) - 1). Two instructions do that:
 *   +0x6700  addiu a2, t8, 1       (the "+ 1")
 *   +0x6708  or    a0, a2, t4      (the interval mask)
 * Both are checked against the stock firmware before anything is written, so
 * a different firmware is left alone.
 */
#define USBBUS_ADD_OFF		0x6700u
#define USBBUS_MASK_OFF		0x6708u
#define USBBUS_ADD_ORIG		0x27060001u	/* addiu a2, t8, 1 */
#define USBBUS_NEXT_ORIG	0x250cffffu	/* addiu t4, t0, -1 */
#define USBBUS_MASK_ORIG	0x00cc2025u	/* or a0, a2, t4 */
#define USBBUS_MASK_NONE	0x00c02025u	/* or a0, a2, zero */

static volatile u32 *usbbus_add;
static volatile u32 *usbbus_mask;

int usbbus_target_patch(int add, int use_mask)
{
	u32 intr;

	if (usbbus_state == 0) {
		SceModule *m = sceKernelFindModuleByName("sceUSB_Driver");

		usbbus_state = -1;
		if (m != NULL) {
			volatile u32 *a = (volatile u32 *)(m->text_addr + USBBUS_ADD_OFF);
			volatile u32 *k = (volatile u32 *)(m->text_addr + USBBUS_MASK_OFF);

			if (a[0] == USBBUS_ADD_ORIG && a[1] == USBBUS_NEXT_ORIG &&
			    k[0] == USBBUS_MASK_ORIG) {
				usbbus_add = a;
				usbbus_mask = k;
				usbbus_state = 1;
				LOG("usb: sceUSB_Driver at %p, frame target instructions found\n", (void *)m);
			} else {
				LOG("usb: sceUSB_Driver instructions differ (%08X %08X %08X); not patching\n",
				    (unsigned)a[0], (unsigned)a[1], (unsigned)k[0]);
			}
		} else {
			LOG("usb: sceUSB_Driver not found; not patching\n");
		}
	}
	if (usbbus_state != 1)
		return -1;

	intr = sceKernelCpuSuspendIntr();
	*usbbus_add = 0x27060000u | ((u32)add & 0xFFFFu);
	*usbbus_mask = use_mask ? USBBUS_MASK_ORIG : USBBUS_MASK_NONE;
	sceKernelDcacheWritebackInvalidateAll();
	sceKernelIcacheInvalidateAll();
	sceKernelCpuResumeIntr(intr);
	{
		static u32 last;
		u32 now = ((u32)add << 1 | (u32)(use_mask != 0)) + 1;

		if (now != last) {
			last = now;
			LOG("usb: frame target now %08X %08X\n", (unsigned)*usbbus_add, (unsigned)*usbbus_mask);
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* USB side                                                                */
/* ---------------------------------------------------------------------- */

void audio_capture_set_active(int on)
{
	u32 intr = sceKernelCpuSuspendIntr();

	if (on && !active) {
		int i;

		if (ring != NULL)
			memset(ring, 0, AUDIO_RING_BYTES);
		for (i = 0; i < AUDIO_CHAN_COUNT; i++)
			chans[i].valid = 0;
		mix_drain = now_frame() - AUDIO_LAG_FRAMES;
#if UVC_AUDIO_BEEP
		beep_left = BEEP_FRAMES;
		beep_phase = 0;
#endif
	}
	active = on ? 1 : 0;
	sceKernelCpuResumeIntr(intr);
}

/*
 * Both audio_frames_ready() and audio_pull() size a packet by the real time that
 * has passed since the previous one, not by a fixed 44.1 frames per call: the
 * rate at which the host and the PSP's USB stack hand packets over is not 1 per
 * millisecond (measured: about 1170 per second), so only the clock can keep the
 * stream at exactly 44.1 kHz. A backlog is worked off at up to
 * AUDIO_PKT_MAX_FRAMES per packet.
 */
static int frames_owed(void)		/* interrupts suspended */
{
	return (int)((now_frame() - AUDIO_LAG_FRAMES) - mix_drain);
}

int audio_frames_ready(void)
{
	u32 intr = sceKernelCpuSuspendIntr();
	int err = frames_owed();

	sceKernelCpuResumeIntr(intr);
	if (err < 0)
		return 0;
	return err > AUDIO_PKT_MAX_FRAMES ? AUDIO_PKT_MAX_FRAMES : err;
}

int audio_pull(unsigned char *dst)
{
	short *out = (short *)dst;
	u32 intr, rpos;
	int frames, err, i;

	intr = sceKernelCpuSuspendIntr();
	err = frames_owed();
	if (err > AUDIO_READ_RESYNC || err < -AUDIO_READ_RESYNC) {
		/* far off (a stall, or the stream just started): re-align to now */
		mix_drain += (u32)err;
		if (ring != NULL)
			memset(ring, 0, AUDIO_RING_BYTES);
		stats.pull_resyncs++;
		err = 0;
	}
	frames = err;
	if (frames > AUDIO_PKT_MAX_FRAMES)
		frames = AUDIO_PKT_MAX_FRAMES;
	if (frames < 1)
		frames = 1;		/* a request cannot be empty; at most one frame early */
	rpos = mix_drain;
	mix_drain += (u32)frames;
	sceKernelCpuResumeIntr(intr);

	if (ring == NULL) {
		memset(dst, 0, (size_t)frames * AUDIO_FRAME_BYTES);
	} else {
		for (i = 0; i < frames; i++) {
			unsigned int idx = ((rpos + (u32)i) & AUDIO_RING_MASK) * 2;
			int l = ring[idx];
			int r = ring[idx + 1];
			unsigned int al = (unsigned int)(l < 0 ? -l : l);
			unsigned int ar = (unsigned int)(r < 0 ? -r : r);

			if (al > peak_out_acc)
				peak_out_acc = al;
			if (ar > peak_out_acc)
				peak_out_acc = ar;
			out[i * 2] = (short)l;
			out[i * 2 + 1] = (short)r;
			ring[idx] = 0;
			ring[idx + 1] = 0;
		}
	}
#if UVC_AUDIO_BEEP
	for (i = 0; i < frames && beep_left > 0; i++, beep_left--) {
		short v;

		beep_phase += BEEP_HZ;
		if (beep_phase >= AUDIO_RATE)
			beep_phase -= AUDIO_RATE;
		v = beep_phase < AUDIO_RATE / 2 ? BEEP_LEVEL : -BEEP_LEVEL;
		out[i * 2] = v;
		out[i * 2 + 1] = v;
	}
#endif
	return frames;
}

void audio_get_stats(struct audio_stats *out)
{
	*out = stats;
	out->active_now = (unsigned int)active;
	out->peak_in = peak_in_acc;
	out->peak_out = peak_out_acc;
	peak_in_acc = 0;
	peak_out_acc = 0;
}

int audio_get_events(struct audio_event *out, int from, int max, const char **names_out)
{
	int n = event_count;
	int i;

	(void)names_out;
	if (n > AUDIO_EVENTS_MAX)
		n = AUDIO_EVENTS_MAX;
	for (i = from; i < n && i - from < max; i++)
		out[i - from] = events[i];
	return i > from ? i : from;
}

static void probe_base(const u32 *b, struct audio_probe *out, int slot)
{
	u32 i;
	int h;

	out->base[slot] = (u32)b;
	out->wrappers[slot] = 0;
	out->origs[slot] = 0;
	if (!kptr_ok(b, out->scanned * 4))
		return;
	for (i = 0; i < out->scanned; i++) {
		u32 w = b[i] & 0x0FFFFFFFu;

		for (h = 0; h < HOOK_COUNT; h++) {
			if (!hooks[h].installed)
				continue;
			if (w == ((u32)hooks[h].wrapper & 0x0FFFFFFFu))
				out->wrappers[slot]++;
			else if (w == ((u32)*hooks[h].orig & 0x0FFFFFFFu))
				out->origs[slot]++;
		}
	}
}

void audio_probe_table(struct audio_probe *out)
{
	u32 c;

	memset(out, 0, sizeof(*out));
	out->scanned = 0x2000;
	__asm__ volatile("cfc0 %0, $12" : "=r"(c));
	probe_base((const u32 *)c, out, 0);
	if (kptr_ok((const void *)c, 4))
		probe_base((const u32 *)*(const u32 *)c, out, 1);
}

const char *audio_hook_name(int hook)
{
	if (hook < 0 || hook >= HOOK_COUNT)
		return "?";
	return hooks[hook].name;
}
