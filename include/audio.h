#ifndef UVC_AUDIO_H
#define UVC_AUDIO_H

/*
 * Game audio capture.
 *
 * The sceAudio output calls are hooked (syscall table, patched through the
 * CFW's SystemControl module), each original is run first, then the PCM it was
 * given is mixed into a ring buffer that the USB side drains one isochronous
 * packet at a time. Output is always 44100 Hz, 16-bit, stereo.
 */

#define AUDIO_RATE		44100
#define AUDIO_FRAME_BYTES	4

/* Largest packet audio_pull() can produce, in frames (nominal 44.1). */
#define AUDIO_PKT_MAX_FRAMES	48

/* One ring slot is a stereo 16-bit frame (4 bytes). */
#define AUDIO_RING_FRAMES	4096
#define AUDIO_RING_BYTES	(AUDIO_RING_FRAMES * AUDIO_FRAME_BYTES)

struct audio_stats {
	unsigned int hooks_mask;	/* bit n = hook n installed */
	unsigned int calls;		/* hooked output calls seen while active */
	unsigned int frames_in;		/* frames mixed into the ring */
	unsigned int frames_late;	/* frames dropped: already past the read cursor */
	unsigned int frames_overflow;	/* frames dropped: beyond the ring */
	unsigned int bad_ptr;		/* buffers rejected by the range check */
	unsigned int unknown_len;	/* calls skipped: buffer length unknown */
	unsigned int chan_resyncs;	/* a channel's write cursor was re-aligned */
	unsigned int pull_resyncs;	/* the read cursor was re-aligned */
	unsigned int channels_seen;	/* bit n = channel n (8 = SRC/Output2) */
	unsigned int wrapper_calls[12];	/* times each hook's wrapper ran, active or not */
	unsigned int active_now;
	unsigned int peak_in;		/* loudest sample mixed in since the last call (0..32767) */
	unsigned int peak_out;		/* loudest sample sent to USB since the last call, beep excluded */
};

/* The first few hooked calls, recorded as they happen and printed later (never
 * logged from inside a game's syscall). */
#define AUDIO_EVENTS_MAX	24
struct audio_event {
	unsigned int time_us;
	int hook;			/* index into the hook table, see audio.c */
	int a0, a1, a2;
	int ret;
	int active;
};

/* Copies events [from, count) into out (up to max); returns the new count. */
int audio_get_events(struct audio_event *out, int from, int max, const char **names_out);
const char *audio_hook_name(int hook);

/*
 * Look at the syscall table directly: how many slots hold one of our wrappers
 * and how many still hold an original function. Tells whether the patch took,
 * whether or not the game has played any sound.
 */
struct audio_probe {
	unsigned int base[2];		/* the two candidate table bases examined */
	unsigned int wrappers[2];	/* slots equal to one of our wrappers */
	unsigned int origs[2];		/* slots equal to one of the original functions */
	unsigned int scanned;		/* words scanned per base */
};
void audio_probe_table(struct audio_probe *out);

/*
 * Install the hooks. ring is AUDIO_RING_BYTES of memory (NULL: no capture,
 * audio_pull() then yields silence). Returns the number of hooks installed,
 * or < 0 if capture is unavailable. Never fatal.
 */
int audio_capture_init(void *ring);

/* Put every patched syscall back. Safe to call more than once. */
void audio_capture_shutdown(void);

/* Experiment: change how the USB bus driver picks the frame for an isochronous
 * packet. add is the "+ 1" (0..), use_mask keeps the bInterval mask. Returns
 * -1 if the firmware's code is not the expected one. */
int usbbus_target_patch(int add, int use_mask);

/* Only mix while a host is actually reading the audio stream. */
void audio_capture_set_active(int on);

/*
 * Fill dst with the next USB packet (stereo s16 LE) and return its length in
 * frames. Always returns a packet, silence if nothing was captured.
 */
int audio_pull(unsigned char *dst);

/* Frames of real time that have passed since the last audio_pull() (0..AUDIO_PKT_MAX_FRAMES). */
int audio_frames_ready(void);

/* Don't send a packet smaller than this unless the stream is stopping. */
#define AUDIO_MIN_PKT_FRAMES	40

void audio_get_stats(struct audio_stats *out);

#endif
