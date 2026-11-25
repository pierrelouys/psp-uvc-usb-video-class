#include <stdint.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include "pspdmacplus.h"
#include "usb.h"
#include "usb_descriptors.h"
#include "format_conversion.h"
#include "log.h"

PSP_MODULE_INFO("UVC", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);
/*
 * This module runs inside whatever process loads it (the XMB or a game). The
 * newlib default is to claim almost all free user memory on the first malloc,
 * which is harmless in the XMB but starves a game. Bound both the heap and the
 * main thread stack to what this plugin actually needs.
 */
PSP_HEAP_SIZE_KB(0);
PSP_MAIN_THREAD_STACK_SIZE_KB(16);

#define USB_DRIVERNAME "UVC"
#define USB_PRODUCT_ID 0x1337

/*
 * Replaces the SDK's libc init (weak symbol, meant to be overridden). The
 * default also sets up pthread, stdio descriptors, cwd and timezone before
 * main() in whatever process loads the plugin. The plugin uses none of that:
 * no stdio, no malloc, no locks (its log has its own formatter).
 */
void __libcglue_init(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
}

void __libcglue_deinit(void)
{
}

/* Sony's USB bus driver reports this when the requested state already holds
 * (pspusb.h: PSP_USB_ERROR_ALREADY_DONE). */
#define UVC_USB_ERROR_ALREADY_DONE	0x80243001

#define FRAME_SEND_TIMEOUT_US		1000000
#define FRAME_CANCEL_TIMEOUT_US		200000
#define MODULE_STOP_TIMEOUT_US		1000000
#define REBOOT_STOP_TIMEOUT_US		500000
#define STATS_PERIOD_US			5000000

#define MAX_UVC_VIDEO_FRAME_SIZE	VIDEO_FRAME_SIZE_YUY2(480, 272)

#define UVC_PAYLOAD_HEADER_SIZE		12
#define UVC_PAYLOAD_SIZE(frame_size)	(UVC_PAYLOAD_HEADER_SIZE + (frame_size))
#define MAX_UVC_PAYLOAD_TRANSFER_SIZE	UVC_PAYLOAD_SIZE(MAX_UVC_VIDEO_FRAME_SIZE)

#define EVENT_STOP_STREAM	(1u << 0)
#define EVENT_FRAME_SENT	(1u << 1)

struct uvc_frame {
	unsigned char header[UVC_PAYLOAD_HEADER_SIZE];
	unsigned char data[];
} __attribute__((packed));

static const struct uvc_streaming_control uvc_probe_control_setting_default = {
	.bmHint				= 0,
	.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_YUY2,
	.bFrameIndex			= 1,
	.dwFrameInterval		= FPS_TO_INTERVAL(60),
	.wKeyFrameRate			= 0,
	.wPFrameRate			= 0,
	.wCompQuality			= 0,
	.wCompWindowSize		= 0,
	.wDelay				= 0,
	.dwMaxVideoFrameSize		= MAX_UVC_VIDEO_FRAME_SIZE,
	.dwMaxPayloadTransferSize	= MAX_UVC_PAYLOAD_TRANSFER_SIZE,
	.dwClockFrequency		= 0,
	.bmFramingInfo			= 0,
	.bPreferedVersion		= 1,
	.bMinVersion			= 0,
	.bMaxVersion			= 0,
};

static struct uvc_streaming_control uvc_probe_control_setting;

static struct {
	unsigned char buffer[64];
	struct DeviceRequest ep0_req;
} __attribute__((aligned(64))) pending_recv;

static volatile int run = 1;
static volatile int stream;
static volatile SceUID main_thid = -1;
static SceUID uvc_frame_req_evflag = -1;
static volatile int frame_req_return_code;
static volatile int frame_req_transmitted;
/*
 * A frame is sent as UVC_FRAME_CHUNKS payload transfers of UVC_CHUNK_ROWS rows
 * each, converted into one of two small buffers while the other is on the
 * wire. A whole-frame buffer is 261 KB: too big for the kernel partition in
 * game mode, and if it is placed in the game's own (user) memory it is freed
 * under the USB driver when the game exits.
 */
#define UVC_FRAME_W		480
#define UVC_FRAME_H		272
#define UVC_CHUNK_ROWS		16
#define UVC_FRAME_CHUNKS	(UVC_FRAME_H / UVC_CHUNK_ROWS)
#define UVC_CHUNK_DATA_SIZE	(UVC_CHUNK_ROWS * UVC_FRAME_W * 2)
#define UVC_CHUNK_SIZE		(UVC_PAYLOAD_HEADER_SIZE + UVC_CHUNK_DATA_SIZE)
#define TX_BUF_COUNT		2
#define TX_BUF_ALIGN		64
#define TX_BUF_STRIDE		(((UVC_CHUNK_SIZE) + TX_BUF_ALIGN - 1) & ~(TX_BUF_ALIGN - 1))
#if UVC_AUDIO
#define TX_AUDIO_RING_SIZE	AUDIO_RING_BYTES
#else
#define TX_AUDIO_RING_SIZE	0
#endif
#define TX_BLOCK_SIZE		(TX_BUF_COUNT * TX_BUF_STRIDE + TX_AUDIO_RING_SIZE + TX_BUF_ALIGN)

_Static_assert(UVC_FRAME_H % UVC_CHUNK_ROWS == 0, "chunks must cover the frame exactly");

static SceUID tx_buf_blk = -1;
static unsigned char *tx_buf[TX_BUF_COUNT];
static void *audio_ring_mem;	/* AUDIO_RING_BYTES behind the tx buffers, or NULL */

/* User-partition figures only: documented user API, no kernel internals. */
static void log_user_memory(void)
{
	LOG("user partition: free %u, largest block %u\n",
	    (unsigned int)sceKernelTotalFreeMemSize(), (unsigned int)sceKernelMaxFreeMemSize());
}

static int tx_buf_alloc(void)
{
	static const int partitions[] = { PSP_MEMORY_PARTITION_KERNEL, PSP_MEMORY_PARTITION_USER };
	static const char *const names[] = { "kernel", "user (top)" };
	/* User memory is the game's: take it from the top so the bottom, where
	 * fixed-address homebrew loads, stays free. */
	static const int types[] = { PSP_SMEM_Low, PSP_SMEM_High };
	unsigned int i;

	for (i = 0; i < sizeof(partitions) / sizeof(partitions[0]); i++) {
		SceUID blk = sceKernelAllocPartitionMemory(partitions[i], "uvc_tx", types[i],
							   TX_BLOCK_SIZE, NULL);
		unsigned char *base;
		int n;

		if (blk < 0) {
			LOG("tx buffer alloc (%s partition, %u bytes) failed: 0x%08X\n",
			    names[i], (unsigned int)TX_BLOCK_SIZE, blk);
			continue;
		}
		tx_buf_blk = blk;
		base = (unsigned char *)(((uintptr_t)sceKernelGetBlockHeadAddr(blk) +
					  TX_BUF_ALIGN - 1) & ~(uintptr_t)(TX_BUF_ALIGN - 1));
		memset(base, 0, TX_BUF_COUNT * TX_BUF_STRIDE);
		for (n = 0; n < TX_BUF_COUNT; n++)
			tx_buf[n] = base + n * TX_BUF_STRIDE;
#if UVC_AUDIO
		audio_ring_mem = base + TX_BUF_COUNT * TX_BUF_STRIDE;
#endif
		LOG("tx buffers: %d x %u bytes at %p (%s partition)\n", TX_BUF_COUNT,
		    (unsigned int)UVC_CHUNK_SIZE, base, names[i]);
		return 0;
	}
	return -1;
}

static void tx_buf_free(void)
{
	int n;

	if (tx_buf_blk >= 0)
		sceKernelFreePartitionMemory(tx_buf_blk);
	tx_buf_blk = -1;
	audio_ring_mem = NULL;
	for (n = 0; n < TX_BUF_COUNT; n++)
		tx_buf[n] = NULL;
}

static int usb_ep0_req_send(const void *data, unsigned int size)
{
	static struct UsbbdDeviceRequest req;

	req = (struct UsbbdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)data,
		.size = size,
		.isControlRequest = 0,
		.onComplete = NULL,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	int ret;

	sceKernelDcacheWritebackRange(data, size);

	ret = sceUsbbdReqSend(&req);
	if (ret < 0)
		LOG("EP0 send of %u bytes failed: 0x%08X\n", size, ret);
	return ret;
}

static void usb_ep0_req_recv_on_complete(struct UsbbdDeviceRequest *req);

static int usb_ep0_enqueue_recv_for_req(const struct DeviceRequest *ep0_req)
{
	static struct UsbbdDeviceRequest req;

	pending_recv.ep0_req = *ep0_req;

	req = (struct UsbbdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)pending_recv.buffer,
		.size = pending_recv.ep0_req.wLength,
		.isControlRequest = 0,
		.onComplete = &usb_ep0_req_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	int ret;

	sceKernelDcacheInvalidateRange(pending_recv.buffer, pending_recv.ep0_req.wLength);

	ret = sceUsbbdReqRecv(&req);
	if (ret < 0)
		LOG("EP0 recv of %u bytes failed: 0x%08X\n", pending_recv.ep0_req.wLength, ret);
	return ret;
}

static int uvc_frame_req_init(void)
{
	uvc_frame_req_evflag = sceKernelCreateEventFlag("uvc_frame_req_evflag", 0, 0, NULL);
	if (uvc_frame_req_evflag < 0) {
		return uvc_frame_req_evflag;
	}

	return 0;
}

static int uvc_frame_req_fini(void)
{
	SceUID id = uvc_frame_req_evflag;

	uvc_frame_req_evflag = -1;
	return sceKernelDeleteEventFlag(id);
}

static void uvc_handle_video_streaming_req_recv(const struct DeviceRequest *req)
{
	struct uvc_streaming_control *streaming_control =
		(struct uvc_streaming_control *)pending_recv.buffer;

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Probe SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			LOG("Commit SET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);

			LOG("Start streaming!\n");
			stream = 1;
			//sceKernelSetEventFlag(uvc_event_flag_id, 1);
			break;
		}
		break;
	}
}

void usb_ep0_req_recv_on_complete(struct UsbbdDeviceRequest *req)
{
	switch (pending_recv.ep0_req.wIndex & 0xFF) {
	case STREAM_INTERFACE:
		uvc_handle_video_streaming_req_recv(&pending_recv.ep0_req);
		break;
	}
}

static void uvc_handle_interface_ctrl_req(const struct DeviceRequest *req)
{
	LOG("  uvc_handle_interface_ctrl_req\n");
}

static void uvc_handle_input_terminal_req(const struct DeviceRequest *req)
{
	LOG("  uvc_handle_input_terminal_req %x, %x\n", req->wValue, req->bRequest);
}

static void uvc_handle_output_terminal_req(const struct DeviceRequest *req)
{
	LOG("  uvc_handle_output_terminal_req\n");
}

static void uvc_handle_video_streaming_req(const struct DeviceRequest *req)
{
	LOG("  uvc_handle_video_streaming_req %x, %x\n", req->wValue, req->bRequest);

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_MIN:
		case UVC_GET_MAX:
		case UVC_GET_DEF:
			LOG("Probe GET_DEF, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting_default.bFormatIndex,
			    uvc_probe_control_setting_default.bmFramingInfo);
			usb_ep0_req_send(&uvc_probe_control_setting_default,
					 sizeof(uvc_probe_control_setting_default));
			break;
		case UVC_GET_CUR:
			LOG("Probe GET_CUR, bFormatIndex: %d, bmFramingInfo: %x\n",
			    uvc_probe_control_setting.bFormatIndex,
			    uvc_probe_control_setting.bmFramingInfo);
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_CUR:
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	}
}

/*
 * Audio over USB: a USB Audio Class 1.0 isochronous IN endpoint. The host
 * selects alternate setting 1 of the streaming interface when an application
 * opens the device; until then nothing is queued and nothing is captured. A
 * dedicated thread keeps AUDIO_PKT_SLOTS one-packet (1 ms) requests queued:
 * each completion refills and requeues its slot.
 */
#if UVC_AUDIO
#define AUDIO_PKT_SLOTS		16
#define AUDIO_PKT_BUF		(AUDIO_PKT_MAX_FRAMES * AUDIO_FRAME_BYTES)
#define AUDIO_EV_WAKE		(1u << AUDIO_PKT_SLOTS)
#define AUDIO_EV_ALL		((1u << (AUDIO_PKT_SLOTS + 1)) - 1)
#define AUDIO_WAIT_US		250000
#define AUDIO_CANCEL_WAIT_US	300000
#define AUDIO_STOP_TIMEOUT_US	500000
#define AUDIO_RETRY_US		100000
#define AUDIO_THREAD_PRIORITY	0x1C
#define AUDIO_THREAD_STACK	(4 * 1024)

_Static_assert(AUDIO_PKT_MAX_FRAMES * AUDIO_FRAME_BYTES <= AUDIO_EP_MAXPACKET,
	       "audio packets must fit the endpoint's wMaxPacketSize");

static SceUID audio_thid = -1;
static SceUID audio_evflag = -1;
static volatile int audio_run;
static volatile int audio_streaming;
static unsigned char audio_pkt[AUDIO_PKT_SLOTS][AUDIO_PKT_BUF] __attribute__((aligned(64)));
/* One request per cache-line-aligned slot: the bus driver updates a request in
 * place, so it must not share a cache line with its neighbour. */
struct audio_req_slot {
	struct UsbbdDeviceRequest r;
} __attribute__((aligned(64)));
static struct audio_req_slot audio_req[AUDIO_PKT_SLOTS];
static volatile int audio_req_rc[AUDIO_PKT_SLOTS];
static unsigned int audio_pending;	/* slots with a request queued; audio thread only */

static struct {
	unsigned int packets;
	unsigned int errors;
	unsigned int submit_errors;
	unsigned int stalls;
} audio_usb_stats;

static void audio_set_alt(int alt)
{
	int on = (alt == AUDIO_ALT_STREAMING);

	if (on == audio_streaming)
		return;
	LOG("Audio streaming %s\n", on ? "on" : "off");
	audio_streaming = on;
	if (audio_evflag >= 0)
		sceKernelSetEventFlag(audio_evflag, AUDIO_EV_WAKE);
}

/*
 * How requests are fed to the isochronous endpoint. The PSP's USB stack is not
 * documented for isochronous use, so several ways are tried in turn: the first
 * one that keeps completing requests is kept. Each is tried for
 * AUDIO_STRAT_TRIAL_US; a strategy "works" if it completes AUDIO_STRAT_MIN_OK
 * requests in that time.
 */
struct audio_strategy {
	const char *name;
	int depth;		/* requests kept queued */
	int pkts;		/* 1 ms packets per request */
};
#define AUDIO_BIG_PKTS		4
#define AUDIO_BIG_SLOTS		4
#define AUDIO_STRAT_MIN_OK	20

static const struct audio_strategy audio_strategies[] = {
	/* Measured on the PSP: only a request queued after the previous one completed
	 * keeps completing. Queueing several ahead stalls after the first. */
	{ "1 deep, one packet per request", 1, 1 },
	{ "1 deep, 4 packets per request", 1, AUDIO_BIG_PKTS },
	{ "16 deep, one packet per request", AUDIO_PKT_SLOTS, 1 },
	{ "2 deep, one packet per request", 2, 1 },
	{ "4 deep, one packet per request", 4, 1 },
};
/* A bus capture showed that with one request in flight the host almost never
 * receives a packet (the request completes, but the packet is gone before the
 * host polls). UVC_AUDIO_STRATEGY picks the first strategy tried; a request
 * queued ahead of the boundary is what has to be tested. */
#ifndef UVC_AUDIO_STRATEGY
#define UVC_AUDIO_STRATEGY	0
#endif
#define AUDIO_STRATEGY_COUNT	((int)(sizeof(audio_strategies) / sizeof(audio_strategies[0])))

static unsigned char audio_big[AUDIO_BIG_SLOTS][AUDIO_BIG_PKTS * AUDIO_PKT_BUF] __attribute__((aligned(64)));
static int audio_strategy_idx = UVC_AUDIO_STRATEGY;
static int audio_ev_printed;
static unsigned int audio_strategy_ok;	/* completions with rc == 0 under the current strategy */

/* The first completions, recorded in the callback and printed from the main loop. */
#define AUDIO_TRACE_MAX		12
struct audio_trace {
	unsigned int time_us;
	int slot, rc, transmitted, size;
};
static struct audio_trace audio_trace[AUDIO_TRACE_MAX];
static volatile int audio_trace_count;
static int audio_trace_printed;

static unsigned int audio_last_done_us;
static unsigned int audio_dt_min = 0xFFFFFFFFu, audio_dt_max, audio_dt_sum, audio_dt_n;

static void audio_pkt_on_complete(struct UsbbdDeviceRequest *req)
{
	int idx = (int)(((const unsigned char *)req - (const unsigned char *)audio_req) /
			sizeof(struct audio_req_slot));
	int n = audio_trace_count;
	unsigned int now = sceKernelGetSystemTimeLow();

	if (req->returnCode == 0 && audio_last_done_us != 0) {
		unsigned int dt = now - audio_last_done_us;

		if (dt < audio_dt_min)
			audio_dt_min = dt;
		if (dt > audio_dt_max)
			audio_dt_max = dt;
		audio_dt_sum += dt;
		audio_dt_n++;
	}
	audio_last_done_us = now;

	if (idx < 0 || idx >= AUDIO_PKT_SLOTS)
		return;
	if (n < AUDIO_TRACE_MAX) {
		audio_trace[n].time_us = now;
		audio_trace[n].slot = idx;
		audio_trace[n].rc = req->returnCode;
		audio_trace[n].transmitted = req->transmitted;
		audio_trace[n].size = req->size;
		audio_trace_count = n + 1;
	}
	audio_req_rc[idx] = req->returnCode;
	if (audio_evflag >= 0)
		sceKernelSetEventFlag(audio_evflag, 1u << idx);
}

/*
 * The PSP bus driver normally targets isochronous packets at frame
 * (FN + 1) | (interval mask), which makes almost every packet miss the host's
 * poll. Measured on hardware: with the mask removed and "+ 1" kept, about 96% of
 * polls receive a packet (mask on: none; "+ 7": about 80%).
 */
#ifndef UVC_AUDIO_REPEAT
#define UVC_AUDIO_REPEAT	1
#endif

#ifndef UVC_USBBUS_PATCH
#define UVC_USBBUS_PATCH	1
#endif
#ifndef UVC_USBBUS_ADD
#define UVC_USBBUS_ADD		1
#endif
#ifndef UVC_USBBUS_MASK
#define UVC_USBBUS_MASK		0
#endif

#ifndef UVC_AUDIO_RT_INTERVAL
#define UVC_AUDIO_RT_INTERVAL	0
#endif

#ifndef UVC_AUDIO_SWEEP
#define UVC_AUDIO_SWEEP		0
#endif
#ifndef UVC_AUDIO_FRAMESWEEP
#define UVC_AUDIO_FRAMESWEEP	0
#endif
#define FSW_COUNT		12
#if UVC_AUDIO_SWEEP || UVC_AUDIO_FRAMESWEEP
/*
 * Timing experiment (make UVC_AUDIO_SWEEP=1). A bus capture of the real stream
 * showed the host polls the endpoint every millisecond and gets a zero-length
 * answer almost every time: a packet only reaches the host if it is queued at
 * the right moment relative to the host's frame. Completions are locked to
 * that frame, so the delay between a completion and the next send is the
 * phase. This sends fixed 44-frame packets after a delay that sweeps from 0
 * to SWEEP_PERIOD_US, and writes the measured delay (left channel, in us) and a
 * sequence number (right channel, 0x4000 | n) into the samples, so every packet
 * that does reach the host says which delay got it there.
 */
#define SWEEP_PERIOD_US		1200u
#define SWEEP_STEP_US		4u
#define SWEEP_FRAMES		44
static unsigned int sweep_seq;
#endif

static int audio_submit(int i)
{
	const struct audio_strategy *st = &audio_strategies[audio_strategy_idx];
	unsigned char *buf = st->pkts > 1 ? audio_big[i] : audio_pkt[i];
	int bytes = 0;
	int k, ret;

#if UVC_AUDIO_FRAMESWEEP
	{
		/* Experiment: every second the USB bus driver's frame targeting is
		 * changed (setting 0 is the stock behaviour) and the test packets
		 * carry the setting in the left channel, so the ones that reach the
		 * host show which setting works. */
		static const struct { unsigned char add, mask; } fsw[FSW_COUNT] = {
			{ 1, 1 }, { 0, 1 }, { 1, 0 }, { 0, 0 }, { 2, 0 }, { 3, 0 },
			{ 4, 0 }, { 5, 0 }, { 6, 0 }, { 7, 0 }, { 8, 0 }, { 9, 0 },
		};
		static int fsw_cur = -1;
		int setting = (int)((sceKernelGetSystemTimeLow() / 1000000u) % FSW_COUNT);
		short *p = (short *)buf;
		int n;

		(void)k;
		if (setting != fsw_cur) {
			usbbus_target_patch(fsw[setting].add, fsw[setting].mask);
			fsw_cur = setting;
		}
		for (n = 0; n < SWEEP_FRAMES; n++) {
			p[n * 2] = (short)(setting + 1);
			p[n * 2 + 1] = (short)(0x4000u | (sweep_seq & 0x3FFFu));
		}
		sweep_seq++;
		bytes = SWEEP_FRAMES * AUDIO_FRAME_BYTES;
	}
#elif UVC_AUDIO_SWEEP
	{
		unsigned int target = (sweep_seq * SWEEP_STEP_US) % SWEEP_PERIOD_US;
		unsigned int x;
		short *p = (short *)buf;
		int n;

		(void)k;
		if (target)
			sceKernelDelayThread(target);
		x = sceKernelGetSystemTimeLow() - audio_last_done_us;
		if (x > 30000u)
			x = 30000u;
		for (n = 0; n < SWEEP_FRAMES; n++) {
			p[n * 2] = (short)x;
			p[n * 2 + 1] = (short)(0x4000u | (sweep_seq & 0x3FFFu));
		}
		sweep_seq++;
		bytes = SWEEP_FRAMES * AUDIO_FRAME_BYTES;
	}
#else
	for (k = 0; k < st->pkts; k++) {
		int tries;

#if UVC_AUDIO_REPEAT
		if (st->pkts == 1) {
			/*
			 * With the bus driver patched (see UVC_USBBUS_*) one request
			 * completes per microframe, but the host only polls once per
			 * millisecond and a packet is only on the wire for the
			 * microframe it was armed in. So the same millisecond of audio is
			 * sent over and over until the next one is due; whichever copy
			 * the host polls is then the right one.
			 */
			static unsigned char chunk[AUDIO_PKT_BUF] __attribute__((aligned(64)));
			static int chunk_bytes;

			if (chunk_bytes == 0 || audio_frames_ready() >= AUDIO_MIN_PKT_FRAMES)
				chunk_bytes = audio_pull(chunk) * AUDIO_FRAME_BYTES;
			memcpy(buf, chunk, chunk_bytes);
			bytes = chunk_bytes;
			continue;
		}
#endif
		/* wait until a worthwhile amount of real time has passed */
		for (tries = 0; tries < 60 && audio_run && audio_streaming &&
		     audio_frames_ready() < AUDIO_MIN_PKT_FRAMES; tries++)
			sceKernelDelayThread(100);
		bytes += audio_pull(buf + bytes) * AUDIO_FRAME_BYTES;
	}
#endif

	sceKernelDcacheWritebackRange(buf, bytes);
	audio_req[i].r = (struct UsbbdDeviceRequest){
		.endpoint = &endpoints[AUDIO_ENDPOINT_INDEX],
		.data = buf,
		.size = bytes,
		.isControlRequest = 0,
		.onComplete = audio_pkt_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};
	ret = sceUsbbdReqSend(&audio_req[i].r);
	if (ret >= 0)
		audio_pending |= 1u << i;
	return ret;
}

/* Cancel what is queued and wait until the bus driver has given every request back. */
static void audio_drain_queue(void)
{
	u32 bits;
	int ret;

	sceUsbbdReqCancelAll(&endpoints[AUDIO_ENDPOINT_INDEX]);
	while (audio_pending) {
		SceUInt timeout = AUDIO_CANCEL_WAIT_US;

		bits = 0;
		ret = sceKernelWaitEventFlag(audio_evflag, AUDIO_EV_ALL,
					     PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
		if (ret < 0) {
			LOG("audio: %u cancelled request(s) never completed: 0x%08X\n", audio_pending, ret);
			break;
		}
		audio_pending &= ~(bits & ((1u << AUDIO_PKT_SLOTS) - 1));
	}
	audio_pending = 0;
}

/* Runs while the host has the streaming interface open. */
static void audio_stream_session(void)
{
	static int logged_submit_err;
	u32 bits = 0;
	int i, ret = 0;
	int idle_logged = 0;

	/* completions left over from an earlier session */
	sceKernelPollEventFlag(audio_evflag, AUDIO_EV_ALL, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits);
	audio_pending = 0;
	audio_strategy_ok = 0;
#if UVC_USBBUS_PATCH
	/* applied again here, with the stream starting, as the working sweep did */
	usbbus_target_patch(UVC_USBBUS_ADD, UVC_USBBUS_MASK);
#endif
	audio_capture_set_active(1);

	while (audio_run && audio_streaming) {
		const struct audio_strategy *st = &audio_strategies[audio_strategy_idx];
		SceUInt timeout = AUDIO_WAIT_US;

		bits = 0;
		if (audio_pending == 0) {
			/* nothing on the wire: (re)prime the queue */
			for (i = 0; i < st->depth; i++) {
				ret = audio_submit(i);
				if (ret < 0)
					break;
			}
			if (audio_pending == 0) {
				audio_usb_stats.submit_errors++;
				if (!logged_submit_err) {
					logged_submit_err = 1;
					LOG("audio: sceUsbbdReqSend on the isochronous endpoint failed: 0x%08X\n", ret);
				}
				sceKernelDelayThread(AUDIO_RETRY_US);
				continue;
			}
			if (!idle_logged) {
				idle_logged = 1;
				LOG("audio: strategy %d (%s): %d request(s) queued\n", audio_strategy_idx,
				    st->name, st->depth);
			}
		}

		ret = sceKernelWaitEventFlag(audio_evflag, AUDIO_EV_ALL,
					     PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
		if (ret < 0) {
			/* queued, but the host is not collecting */
			audio_usb_stats.stalls++;
			if (audio_strategy_ok < AUDIO_STRAT_MIN_OK &&
			    audio_strategy_idx + 1 < AUDIO_STRATEGY_COUNT) {
				LOG("audio: strategy %d (%s) stalled after %u completion(s); trying the next\n",
				    audio_strategy_idx, st->name, audio_strategy_ok);
				audio_drain_queue();
				audio_strategy_idx++;
				audio_strategy_ok = 0;
				idle_logged = 0;
			} else if (audio_strategy_ok < AUDIO_STRAT_MIN_OK) {
				static int logged_final;

				if (!logged_final) {
					logged_final = 1;
					LOG("audio: no strategy keeps the isochronous endpoint completing\n");
				}
			}
			continue;
		}

		for (i = 0; i < AUDIO_PKT_SLOTS; i++) {
			int rc;

			if (!(bits & (1u << i)))
				continue;
			audio_pending &= ~(1u << i);
			rc = audio_req_rc[i];
			if (rc == PSP_USB_RETCODE_CANCELTRANSMISSION || rc == PSP_USB_RETCODE_CANCEL ||
			    rc == PSP_USB_RETCODE_CANCEL_ALL)
				continue;
			if (rc != 0) {
				static int last_rc;

				audio_usb_stats.errors++;
				if (rc != last_rc) {
					last_rc = rc;
					LOG("audio: packet completed with error 0x%08X\n", rc);
				}
			} else {
				audio_usb_stats.packets++;
				audio_strategy_ok++;
			}
			if (!audio_run || !audio_streaming)
				continue;
			ret = audio_submit(i);
			if (ret < 0) {
				audio_usb_stats.submit_errors++;
				if (!logged_submit_err) {
					logged_submit_err = 1;
					LOG("audio: requeue failed: 0x%08X\n", ret);
				}
			}
		}
	}

	/* The requests and buffers must not be reused while the bus driver owns them. */
	audio_capture_set_active(0);
	audio_drain_queue();
}

static int audio_thread(SceSize args, void *argp)
{
#if UVC_USBBUS_PATCH
	/* see the comment above usbbus_target_patch() in audio.c */
	usbbus_target_patch(UVC_USBBUS_ADD, UVC_USBBUS_MASK);
#endif
	while (audio_run) {
		u32 bits = 0;
		SceUInt timeout = AUDIO_WAIT_US;

		if (audio_streaming) {
			audio_stream_session();
			continue;
		}
		sceKernelWaitEventFlag(audio_evflag, AUDIO_EV_WAKE,
				       PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, &timeout);
	}
	return 0;
}

static int audio_usb_start(void)
{
	int ret;

	audio_evflag = sceKernelCreateEventFlag("uvc_audio_evflag", 0, 0, NULL);
	if (audio_evflag < 0) {
		ret = audio_evflag;
		audio_evflag = -1;
		return ret;
	}
	audio_run = 1;
	audio_streaming = 0;
	audio_thid = sceKernelCreateThread("uvc_audio", audio_thread, AUDIO_THREAD_PRIORITY,
					   AUDIO_THREAD_STACK, 0, NULL);
	if (audio_thid < 0) {
		ret = audio_thid;
		audio_thid = -1;
		return ret;
	}
	ret = sceKernelStartThread(audio_thid, 0, NULL);
	if (ret < 0) {
		sceKernelDeleteThread(audio_thid);
		audio_thid = -1;
	}
	return ret;
}

static void audio_usb_stop(void)
{
	SceUID thid = audio_thid;
	SceUID evid = audio_evflag;

	audio_run = 0;
	audio_streaming = 0;
	if (evid >= 0)
		sceKernelSetEventFlag(evid, AUDIO_EV_WAKE);
	if (thid >= 0) {
		SceUInt timeout = AUDIO_STOP_TIMEOUT_US;

		if (sceKernelWaitThreadEnd(thid, &timeout) < 0) {
			LOG("audio thread did not finish; terminating it\n");
			sceKernelTerminateThread(thid);
		}
		sceKernelDeleteThread(thid);
		audio_thid = -1;
	}
	audio_evflag = -1;
	if (evid >= 0)
		sceKernelDeleteEventFlag(evid);
}
#else
static inline void audio_set_alt(int alt)
{
	(void)alt;
}
#endif

static void uvc_handle_video_abort(void)
{
	LOG("uvc_handle_video_abort\n");

	if (stream) {
		stream = 0;

		sceUsbbdClearFIFO(&endpoints[1]);
		sceUsbbdReqCancelAll(&endpoints[1]);
	}
}

static void usb_handle_set_interface(const struct DeviceRequest *req)
{
	LOG("usb_handle_set_interface %x %x\n", req->wIndex, req->wValue);

	/* MAC OS sends Set Interface Alternate Setting 0 command after
	 * stopping to stream. This application needs to stop streaming. */
	if ((req->wIndex == STREAM_INTERFACE) && (req->wValue == 0))
		uvc_handle_video_abort();
#if UVC_AUDIO
	if ((req->wIndex & 0xFF) == AUDIO_STREAM_INTERFACE)
		audio_set_alt(req->wValue);
#endif
}

static void usb_handle_clear_feature(const struct DeviceRequest *req)
{
	LOG("usb_handle_clear_feature\n");

	/* Windows OS sends Clear Feature Request after it stops streaming,
	 * however MAC OS sends clear feature request right after it sends a
	 * Commit -> SET_CUR request. Hence, stop streaming only of streaming
	 * has started. */
	switch (req->wValue) {
	case USB_FEATURE_ENDPOINT_HALT:
		if ((req->wIndex & USB_ENDPOINT_ADDRESS_MASK) ==
		    endpoints[1].endpointNumber) {
			uvc_handle_video_abort();
		}
		break;
	}
}

static int usb_process_request(int recipient, int arg, struct DeviceRequest *req)
{
	LOG("usb_driver_process_request(recipient: %x, arg: %x)\n", recipient, arg);
	LOG("  request: %x type: %x wValue: %x wIndex: %x wLength: %x\n",
		req->bRequest, req->bmRequestType, req->wValue, req->wIndex, req->wLength);

	if (arg < 0)
		return -1;

	switch (req->bmRequestType) {
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0xA1 */
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x21 */
		switch (req->wIndex & 0xFF) {
		case CONTROL_INTERFACE:
			switch (req->wIndex >> 8) {
			case INTERFACE_CTRL_ID:
				uvc_handle_interface_ctrl_req(req);
				break;
			case INPUT_TERMINAL_ID:
				uvc_handle_input_terminal_req(req);
				break;
			case OUTPUT_TERMINAL_ID:
				uvc_handle_output_terminal_req(req);
				break;
			}
			break;
		case STREAM_INTERFACE:
			uvc_handle_video_streaming_req(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x01 */
		switch (req->bRequest) {
		case USB_REQ_SET_INTERFACE:
			usb_handle_set_interface(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_ENDPOINT: /* 0x02 */
		switch (req->bRequest) {
		case USB_REQ_CLEAR_FEATURE:
			usb_handle_clear_feature(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_DEVICE: /* 0x80 */
		switch (req->wValue >> 8) {
		case 0x0A: /* USB_DT_DEBUG */
			break;
		}
		break;
	default:
		LOG("Unknown bmRequestType: 0x%02X\n", req->bmRequestType);
	}

	return 0;
}

static int usb_change_setting(int interfaceNumber, int alternateSetting)
{
	LOG("usb_change %d %d\n", interfaceNumber, alternateSetting);
#if UVC_AUDIO
	if (interfaceNumber == AUDIO_STREAM_INTERFACE) {
#if UVC_AUDIO_RT_INTERVAL
		/*
		 * The bus driver arms an isochronous packet for frame
		 * (FN + 1) | ((1 << (bInterval - 1)) - 1), reading bInterval when
		 * the alternate setting is activated. Windows only streams if the
		 * descriptor it read says 4, so the host keeps seeing 4 and the
		 * value is lowered here, after the host has read the descriptors
		 * and before it selects alternate setting 1.
		 */
		if (alternateSetting == 0)
			endpdesc_hi[1].bInterval = UVC_AUDIO_RT_INTERVAL;
#endif
		audio_set_alt(alternateSetting);
	}
#endif
	return 0;
}

static volatile int usb_attached;	/* the host configured the device since the last activate */

static int usb_attach(int usb_version)
{
#if UVC_AUDIO && UVC_AUDIO_RT_INTERVAL
	/* a new enumeration must see the real descriptor again */
	endpdesc_hi[1].bInterval = UVC_AUDIO_HS_INTERVAL;
#endif
	usb_attached = 1;
	LOG("usb_attach %d\n", usb_version);
	return 0;
}

static void usb_detach(void)
{
	LOG("usb_detach\n");
	uvc_handle_video_abort();
	audio_set_alt(0);
}

static void usb_configure(int usb_version, int desc_count, struct InterfaceSettings *settings)
{
	LOG("usb_configure %d %d %p %d\n", usb_version, desc_count, settings, settings->numDescriptors);
}

static int usb_driver_start(int size, void *args)
{
	LOG("usb_driver_start\n");
	return 0;
}

static int usb_driver_stop(int size, void *args)
{
	LOG("usb_driver_stop\n");
	uvc_handle_video_abort();
	audio_set_alt(0);
	return 0;
}

static struct UsbDriver usb_driver = {
	.driverName			= USB_DRIVERNAME,
	.numEndpoints			= NUM_ENDPOINTS,
	.endpoints			= endpoints,
	.interface			= &interface,
	.descriptor_hi			= &devdesc_hi,
	.configuration_hi		= &config_hi,
	.descriptor			= &devdesc_full,
	.configuration			= &config_full,
	.stringDescriptors		= NULL,
	.processRequest			= &usb_process_request,
	.chageSetting			= &usb_change_setting,
	.attach				= &usb_attach,
	.detach				= &usb_detach,
	.configure			= &usb_configure,
	.start				= &usb_driver_start,
	.stop				= &usb_driver_stop,
	.link				= NULL
};

static void uvc_frame_send_req_on_complete(struct UsbbdDeviceRequest *req)
{
	frame_req_return_code = req->returnCode;
	frame_req_transmitted = req->transmitted;
	sceKernelSetEventFlag(uvc_frame_req_evflag, EVENT_FRAME_SENT);
}

/* Errors reported by the frame path that are not SCE error codes. */
#define UVC_ERR_LCDC_FORMAT_UNKNOWN	((int)0x80000001)
#define UVC_ERR_FRAME_SHORT		((int)0x80000002)
#define UVC_ERR_PIXEL_FORMAT_UNKNOWN	((int)0x80000003)
#define UVC_ERR_STOPPED			((int)0x80000004)

/* Returns 0 when the chunk was sent, 1 if the stream was aborted, < 0 on error. */
static int uvc_wait_chunk_sent(void)
{
	u32 event = 0;
	SceUInt timeout = FRAME_SEND_TIMEOUT_US;
	int ret;

	ret = sceKernelWaitEventFlagCB(uvc_frame_req_evflag, EVENT_STOP_STREAM | EVENT_FRAME_SENT,
				       PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &event, &timeout);

	if (ret < 0 || (event & EVENT_STOP_STREAM)) {
		/*
		 * The request is still queued (host not reading, or we are being
		 * stopped). The buffer and request must not be reused until the bus
		 * driver is done with them, so cancel and wait for the completion.
		 */
		u32 cancel_event = 0;
		SceUInt cancel_timeout = FRAME_CANCEL_TIMEOUT_US;
		int cancel_ret;

		sceUsbbdReqCancelAll(&endpoints[1]);
		cancel_ret = sceKernelWaitEventFlag(uvc_frame_req_evflag, EVENT_FRAME_SENT,
						    PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
						    &cancel_event, &cancel_timeout);
		if (cancel_ret < 0)
			LOG("Cancelled frame request never completed: 0x%08X\n", cancel_ret);
		return ret < 0 ? ret : UVC_ERR_STOPPED;
	}

	if (frame_req_return_code == -3) /* cancelled by uvc_handle_video_abort() */
		return 1;
	if (frame_req_return_code != 0)
		return frame_req_return_code;
	if (frame_req_transmitted != (int)UVC_CHUNK_SIZE)
		return UVC_ERR_FRAME_SHORT;

	return 0;
}

static int convert_and_send_frame_yuy2(int fid, void *fbaddr, int fbstride, int fbpixelformat,
				       unsigned int *csc_us, unsigned int *send_us)
{
	static struct UsbbdDeviceRequest req[TX_BUF_COUNT];
	int bytes_per_pixel;
	int in_flight = 0;
	int chunk;
	int ret;

	switch (fbpixelformat) {
	case PSP_DISPLAY_PIXEL_FORMAT_8888:
		bytes_per_pixel = 4;
		break;
	case PSP_DISPLAY_PIXEL_FORMAT_565:
	case PSP_DISPLAY_PIXEL_FORMAT_5551:
	case PSP_DISPLAY_PIXEL_FORMAT_4444:
		bytes_per_pixel = 2;
		break;
	default:
		return UVC_ERR_PIXEL_FORMAT_UNKNOWN;
	}

	for (chunk = 0; chunk < UVC_FRAME_CHUNKS; chunk++) {
		unsigned char *buf = tx_buf[chunk % TX_BUF_COUNT];
		const unsigned char *src = (const unsigned char *)fbaddr +
			(size_t)chunk * UVC_CHUNK_ROWS * fbstride * bytes_per_pixel;
		unsigned char *dst = &buf[UVC_PAYLOAD_HEADER_SIZE];
		unsigned int t0, t1;

		buf[0] = UVC_PAYLOAD_HEADER_SIZE;
		buf[1] = UVC_STREAM_EOH;
		if (fid)
			buf[1] |= UVC_STREAM_FID;
		if (chunk == UVC_FRAME_CHUNKS - 1)
			buf[1] |= UVC_STREAM_EOF;

		t0 = sceKernelGetSystemTimeLow();
		switch (fbpixelformat) {
		case PSP_DISPLAY_PIXEL_FORMAT_8888:
			r8g8b8a8_to_yuy2(src, dst, fbstride, UVC_FRAME_W, UVC_CHUNK_ROWS);
			break;
		case PSP_DISPLAY_PIXEL_FORMAT_565:
			r5g6b5_to_yuy2(src, dst, fbstride, UVC_FRAME_W, UVC_CHUNK_ROWS);
			break;
		case PSP_DISPLAY_PIXEL_FORMAT_5551:
			r5g5b5a1_to_yuy2(src, dst, fbstride, UVC_FRAME_W, UVC_CHUNK_ROWS);
			break;
		default:
			r4g4b4a4_to_yuy2(src, dst, fbstride, UVC_FRAME_W, UVC_CHUNK_ROWS);
			break;
		}
		sceKernelDcacheWritebackRange(buf, UVC_CHUNK_SIZE);
		t1 = sceKernelGetSystemTimeLow();
		*csc_us += t1 - t0;

		/* Shutdown (module_stop / module_reboot_before) may start at any moment:
		 * never queue another request once it has. */
		if (!run) {
			if (in_flight)
				uvc_wait_chunk_sent();
			return UVC_ERR_STOPPED;
		}

		/* The previous chunk was on the wire while this one was converted. */
		if (in_flight) {
			ret = uvc_wait_chunk_sent();
			*send_us += sceKernelGetSystemTimeLow() - t1;
			in_flight = 0;
			if (ret != 0)
				return ret < 0 ? ret : 0;
		}

		req[chunk % TX_BUF_COUNT] = (struct UsbbdDeviceRequest){
			.endpoint = &endpoints[1],
			.data = buf,
			.size = UVC_CHUNK_SIZE,
			.isControlRequest = 0,
			.onComplete = uvc_frame_send_req_on_complete,
			.transmitted = 0,
			.returnCode = 0,
			.next = NULL,
			.unused = NULL,
			.physicalAddress = NULL
		};
		ret = sceUsbbdReqSend(&req[chunk % TX_BUF_COUNT]);
		if (ret < 0)
			return ret;
		in_flight = 1;
	}

	{
		unsigned int t = sceKernelGetSystemTimeLow();

		ret = uvc_wait_chunk_sent();
		*send_us += sceKernelGetSystemTimeLow() - t;
	}
	return ret < 0 ? ret : 0;
}

static int get_display_params_lcdc(void **addr, int *pixelformat, int *width, int *stride)
{
	int lcdc_pixelfmt = -1;
	int ret;

	ret = sceDmacplusLcdcGetFormat(width, stride, &lcdc_pixelfmt);
	if (ret < 0)
		return ret;

	*addr = (void *)((uintptr_t)sceDmacplusLcdcGetBaseAddr() & ~0x40000000);

	switch (lcdc_pixelfmt) {
	case SCE_DMACPLUS_LCDC_FORMAT_RGBA8888:
		*pixelformat = PSP_DISPLAY_PIXEL_FORMAT_8888;
		break;
	case SCE_DMACPLUS_LCDC_FORMAT_RGB565:
		*pixelformat = PSP_DISPLAY_PIXEL_FORMAT_565;
		break;
	case SCE_DMACPLUS_LCDC_FORMAT_RGBA5551:
		*pixelformat = PSP_DISPLAY_PIXEL_FORMAT_5551;
		break;
	case SCE_DMACPLUS_LCDC_FORMAT_RGBA4444:
		*pixelformat = PSP_DISPLAY_PIXEL_FORMAT_4444;
		break;
	default:
		LOG("Unknown LCDC pixel format %d (addr %p, w %d, stride %d)\n",
		    lcdc_pixelfmt, *addr, *width, *stride);
		return UVC_ERR_LCDC_FORMAT_UNKNOWN;
	}

	return 0;
}

struct stream_stats {
	unsigned int frames;
	unsigned int errors;
	unsigned int csc_us_total;
	unsigned int send_us_total;
};

static int send_frame(struct stream_stats *stats)
{
	static int fid = 0;
	static int last_w = -1, last_stride = -1, last_fmt = -1;

	int ret;
	void *fbaddr = NULL;
	int fbwidth = 0;
	int fbstride = 0;
	int fbpixelformat = 0;
	unsigned int csc_us = 0, send_us = 0;

	ret = get_display_params_lcdc(&fbaddr, &fbpixelformat, &fbwidth, &fbstride);
	if (ret < 0)
		return ret;

	/* The scanout address flips between the game's two buffers every frame;
	 * only a change of geometry or format is worth a log line. */
	if (fbwidth != last_w || fbstride != last_stride || fbpixelformat != last_fmt) {
		LOG("Display mode: addr %p, w %d, stride %d, pixel format %d\n",
		    fbaddr, fbwidth, fbstride, fbpixelformat);
		last_w = fbwidth;
		last_stride = fbstride;
		last_fmt = fbpixelformat;
	}

	switch (uvc_probe_control_setting.bFormatIndex) {
	case FORMAT_INDEX_UNCOMPRESSED_YUY2:
		ret = convert_and_send_frame_yuy2(fid, fbaddr, fbstride, fbpixelformat,
						  &csc_us, &send_us);
		break;
	default:
		LOG("Unsupported negotiated format index %d\n",
		    uvc_probe_control_setting.bFormatIndex);
		return -1;
	}

	if (ret < 0)
		return ret;

	stats->frames++;
	stats->csc_us_total += csc_us;
	stats->send_us_total += send_us;
	fid ^= 1;

	return 0;
}

/* Everything main() brought up, so a failure at any step unwinds exactly that. */
struct uvc_state {
	int driver_registered;
	int bus_started_by_us;
	int driver_started;
	int activated;
	int evflag_created;
	int tx_buf_allocated;
	int audio_hooked;
	int audio_started;
};

/* Shared with module_stop(), which must be able to unwind the USB driver
 * itself if the main thread cannot. */
static struct uvc_state uvc_st;

static void uvc_teardown(struct uvc_state *st)
{
	int ret;

	uvc_handle_video_abort();

#if UVC_AUDIO
	/* Hooks first: no game thread may enter our code once the ring is going.
	 * Then the audio thread, before the USB driver and the memory under it. */
	if (st->audio_hooked) {
		audio_capture_shutdown();
		LOG("audio capture shut down\n");
	}
	if (st->audio_started)
		audio_usb_stop();
#endif

	if (st->activated) {
		ret = sceUsbDeactivate();
		LOG("sceUsbDeactivate: 0x%08X\n", ret);
	}
	if (st->driver_started) {
		ret = sceUsbStop(USB_DRIVERNAME, 0, 0);
		LOG("sceUsbStop(" USB_DRIVERNAME "): 0x%08X\n", ret);
	}
	if (st->bus_started_by_us) {
		ret = sceUsbStop(PSP_USB_BUS_DRIVERNAME, 0, 0);
		LOG("sceUsbStop(" PSP_USB_BUS_DRIVERNAME "): 0x%08X\n", ret);
	}
	if (st->driver_registered) {
		ret = sceUsbbdUnregister(&usb_driver);
		LOG("sceUsbbdUnregister: 0x%08X\n", ret);
	}
	if (st->tx_buf_allocated)
		tx_buf_free();
	if (st->evflag_created) {
		ret = uvc_frame_req_fini();
		LOG("event flag delete: 0x%08X\n", ret);
	}
	memset(st, 0, sizeof(*st));
}

#if UVC_AUDIO
/* Enumeration never completed: re-register the driver with a smaller descriptor set. */
#define UVC_ENUM_TIMEOUT_US	3000000

static void uvc_usb_degrade(struct uvc_state *st, int level)
{
	int ret;

	LOG("USB host never configured the device; retrying with %s\n",
	    level == 1 ? "no class-specific audio endpoint descriptor" : "video only (no audio)");
	log_flush();

	audio_set_alt(0);
	if (st->activated) {
		ret = sceUsbDeactivate();
		LOG("sceUsbDeactivate: 0x%08X\n", ret);
		st->activated = 0;
	}
	if (st->driver_started) {
		ret = sceUsbStop(USB_DRIVERNAME, 0, 0);
		LOG("sceUsbStop(" USB_DRIVERNAME "): 0x%08X\n", ret);
		st->driver_started = 0;
	}
	if (st->driver_registered) {
		ret = sceUsbbdUnregister(&usb_driver);
		LOG("sceUsbbdUnregister: 0x%08X\n", ret);
		st->driver_registered = 0;
	}

	uvc_descriptors_degrade(level);
	if (level >= 2) {
		usb_driver.numEndpoints = 2;
		if (st->audio_hooked) {
			audio_capture_shutdown();
			st->audio_hooked = 0;
		}
		if (st->audio_started) {
			audio_usb_stop();
			st->audio_started = 0;
		}
	}

	ret = sceUsbbdRegister(&usb_driver);
	LOG("sceUsbbdRegister: 0x%08X\n", ret);
	if (ret < 0)
		return;
	st->driver_registered = 1;
	ret = sceUsbStart(USB_DRIVERNAME, 0, 0);
	LOG("sceUsbStart(" USB_DRIVERNAME "): 0x%08X\n", ret);
	if (ret < 0)
		return;
	st->driver_started = 1;
	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
	       sizeof(uvc_probe_control_setting));
	stream = 0;
	usb_attached = 0;
	ret = sceUsbActivate(USB_PRODUCT_ID);
	LOG("sceUsbActivate(0x%04X): 0x%08X\n", USB_PRODUCT_ID, ret);
	if (ret >= 0)
		st->activated = 1;
}
#endif

static int uvc_run(struct uvc_state *st)
{
	struct stream_stats stats = {0};
	unsigned int stats_t0;
	int last_usb_state = -1;
	int degrade_level = 0;
	unsigned int enum_bad_since = 0;
	int enum_bad = 0;
	int last_frame_err = 0;
	unsigned int suppressed_frame_errs = 0;
	int ret;

	ret = uvc_frame_req_init();
	if (ret < 0) {
		LOG("Error creating frame event flag: 0x%08X\n", ret);
		return ret;
	}
	st->evflag_created = 1;

	log_user_memory();
	ret = tx_buf_alloc();
	if (ret < 0)
		return ret;
	st->tx_buf_allocated = 1;

#if UVC_AUDIO
	log_flush();	/* everything so far on disk before the audio setup */
	ret = audio_usb_start();
	if (ret < 0)
		LOG("audio: could not start the audio thread: 0x%08X; no audio\n", ret);
	else
		st->audio_started = 1;
#if UVC_AUDIO_HOOKS
	/* Not fatal: without the hooks the stream carries silence. */
	ret = audio_capture_init(audio_ring_mem);
	if (ret < 0)
		LOG("audio: capture unavailable (%d); the audio stream will be silent\n", ret);
	st->audio_hooked = 1;
#else
	LOG("audio: built without capture hooks (UVC_AUDIO_HOOKS=0); the audio stream is silent\n");
#endif
#endif

	ret = sceUsbbdRegister(&usb_driver);
	LOG("sceUsbbdRegister: 0x%08X\n", ret);
	if (ret < 0)
		return ret;
	st->driver_registered = 1;

	ret = sceUsbStart(PSP_USB_BUS_DRIVERNAME, 0, 0);
	LOG("sceUsbStart(" PSP_USB_BUS_DRIVERNAME "): 0x%08X\n", ret);
	if (ret == 0) {
		st->bus_started_by_us = 1;
	} else if ((unsigned int)ret != UVC_USB_ERROR_ALREADY_DONE) {
		return ret;
	} else {
		LOG("USB bus driver was already started by the system; leaving it running on exit\n");
	}

	ret = sceUsbStart(USB_DRIVERNAME, 0, 0);
	LOG("sceUsbStart(" USB_DRIVERNAME "): 0x%08X\n", ret);
	if (ret < 0)
		return ret;
	st->driver_started = 1;

	/* Current streaming settings start out as the defaults. */
	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
	       sizeof(uvc_probe_control_setting));
	stream = 0;

	LOG("endpoints: [1] number %d driver %d, [%d] number %d driver %d\n",
	    endpoints[1].endpointNumber, endpoints[1].driverEndpointNumber,
	    NUM_ENDPOINTS - 1, endpoints[NUM_ENDPOINTS - 1].endpointNumber,
	    endpoints[NUM_ENDPOINTS - 1].driverEndpointNumber);
	LOG("descriptors: wTotalLength hi %u full %u, %d interfaces, %d endpoints; "
	    "endpdesc %p interdesc %p settings %p\n", (unsigned int)confdesc_hi.wTotalLength,
	    (unsigned int)confdesc_full.wTotalLength, (int)confdesc_hi.bNumInterfaces,
	    (int)usb_driver.numEndpoints, (void *)endpdesc_hi, (void *)interdesc_hi,
	    (void *)settings_hi);
	usb_attached = 0;
	ret = sceUsbActivate(USB_PRODUCT_ID);
	LOG("sceUsbActivate(0x%04X): 0x%08X\n", USB_PRODUCT_ID, ret);
	if (ret < 0)
		return ret;
	st->activated = 1;

	stats_t0 = sceKernelGetSystemTimeLow();

	while (run) {
		int usb_state;

		sceDisplayWaitVblankStart();
		if (!run)
			break;

		usb_state = sceUsbGetState();
		if (usb_state != last_usb_state) {
			LOG("USB state 0x%03X -> 0x%03X\n", last_usb_state, usb_state);
			last_usb_state = usb_state;
		}

#if UVC_AUDIO
		/*
		 * Cable in, state "unknown" and the host never configured us: it
		 * refused the descriptors. Try a smaller set instead of leaving the
		 * device dead (video must keep working whatever the audio does).
		 */
		if (degrade_level < 2 && !usb_attached &&
		    (usb_state & PSP_USB_STATUS_CABLE_CONNECTED) &&
		    (usb_state & PSP_USB_STATUS_CONNECTION_UNKNOWN)) {
			unsigned int now = sceKernelGetSystemTimeLow();

			if (!enum_bad) {
				enum_bad = 1;
				enum_bad_since = now;
			} else if ((unsigned int)(now - enum_bad_since) >= UVC_ENUM_TIMEOUT_US) {
				degrade_level++;
				uvc_usb_degrade(st, degrade_level);
				enum_bad = 0;
				last_usb_state = -1;
				continue;
			}
		} else {
			enum_bad = 0;
		}
#endif

		if ((usb_state & PSP_USB_STATUS_CONNECTION_ESTABLISHED) && stream) {
			ret = send_frame(&stats);
			if (ret < 0) {
				stats.errors++;
				/* Report each distinct error once, then count the repeats. */
				if (ret != last_frame_err) {
					LOG("Frame error 0x%08X (%u identical error(s) suppressed before)\n",
					    ret, suppressed_frame_errs);
					last_frame_err = ret;
					suppressed_frame_errs = 0;
				} else {
					suppressed_frame_errs++;
				}
			} else {
				last_frame_err = 0;
			}
		}

		if ((unsigned int)(sceKernelGetSystemTimeLow() - stats_t0) >= STATS_PERIOD_US) {
			if (stats.frames || stats.errors) {
				unsigned int n = stats.frames ? stats.frames : 1;
				LOG("Stream: %u frames, %u errors, avg CSC %uus, avg USB send %uus\n",
				    stats.frames, stats.errors, stats.csc_us_total / n,
				    stats.send_us_total / n);
			}
#if UVC_AUDIO
			{
				struct audio_stats as;
				struct audio_probe pr;

				audio_get_stats(&as);
				audio_probe_table(&pr);
				LOG("Audio probe: syscall table A %p wrappers %u origs %u, B %p wrappers %u origs %u\n",
				    (void *)pr.base[0], pr.wrappers[0], pr.origs[0], (void *)pr.base[1],
				    pr.wrappers[1], pr.origs[1]);
				LOG("Audio (cumulative): %u packets, %u errors, %u submit errors, %u stalls\n",
				    audio_usb_stats.packets, audio_usb_stats.errors,
				    audio_usb_stats.submit_errors, audio_usb_stats.stalls);
				LOG("  capture: hooks 0x%03X, %u calls, %u frames in, %u late, %u overflow, "
				    "%u bad ptr, %u unknown len, resyncs chan %u read %u, channels 0x%03X\n",
				    as.hooks_mask, as.calls, as.frames_in, as.frames_late,
				    as.frames_overflow, as.bad_ptr, as.unknown_len, as.chan_resyncs,
				    as.pull_resyncs, as.channels_seen);
				LOG("  wrapper calls: ChReserve %u SetLen %u Config %u Out %u OutBlk %u Pan %u "
				    "PanBlk %u SRCRes %u SRCBlk %u O2Res %u O2Blk %u O2Len %u; capture active %u\n",
				    as.wrapper_calls[0], as.wrapper_calls[1], as.wrapper_calls[2],
				    as.wrapper_calls[3], as.wrapper_calls[4], as.wrapper_calls[5],
				    as.wrapper_calls[6], as.wrapper_calls[7], as.wrapper_calls[8],
				    as.wrapper_calls[9], as.wrapper_calls[10], as.wrapper_calls[11],
				    as.active_now);
				if (audio_dt_n) {
					LOG("  iso completion interval us: min %u avg %u max %u over %u\n",
					    audio_dt_min, audio_dt_sum / audio_dt_n, audio_dt_max, audio_dt_n);
					audio_dt_min = 0xFFFFFFFFu;
					audio_dt_max = audio_dt_sum = audio_dt_n = 0;
				}
				LOG("  levels since last report (of 32767): game audio in %u, sent to USB %u\n",
				    as.peak_in, as.peak_out);
				LOG("  iso endpoint: number %d (driver %d), %d bytes transmitted\n",
				    endpoints[AUDIO_ENDPOINT_INDEX].endpointNumber,
				    endpoints[AUDIO_ENDPOINT_INDEX].driverEndpointNumber,
				    endpoints[AUDIO_ENDPOINT_INDEX].transmittedBytes);
			}
			/* new first-calls and first-completions, if any */
			for (;;) {
				struct audio_event ev[8];
				int from = audio_ev_printed;
				int n = audio_get_events(ev, from, 8, NULL);
				int k;

				if (n <= from)
					break;
				for (k = 0; k < n - from; k++)
					LOG("  audio call: %s(%d, %d, %d) = %d%s [%u]\n", audio_hook_name(ev[k].hook),
					    ev[k].a0, ev[k].a1, ev[k].a2, ev[k].ret,
					    ev[k].active ? "" : " (capture not active)", ev[k].time_us);
				audio_ev_printed = n;
			}
			while (audio_trace_printed < audio_trace_count) {
				struct audio_trace *t = &audio_trace[audio_trace_printed++];

				LOG("  iso completion: slot %d rc 0x%08X transmitted %d of %d [%u]\n",
				    t->slot, t->rc, t->transmitted, t->size, t->time_us);
			}
#endif
			memset(&stats, 0, sizeof(stats));
			stats_t0 = sceKernelGetSystemTimeLow();
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct uvc_state *st = &uvc_st;
	int ret;

	main_thid = sceKernelGetThreadId();

	/* Not fatal: without a log the plugin still streams, it just cannot report. */
	log_init(argc > 0 ? argv[0] : NULL);

	LOG("UVC. USB Video Class. argc %d, argv[0] = %s\n", argc, argc > 0 ? argv[0] : "(none)");

	ret = uvc_run(st);
	if (ret < 0)
		LOG("UVC aborted: 0x%08X\n", ret);
	else
		LOG("UVC stopping\n");

	uvc_teardown(st);
	LOG("UVC done\n");
	log_shutdown();

	/*
	 * Do not return: the C runtime would call exit(), which ends in
	 * sceKernelExitGame() and would close the running game.
	 */
	main_thid = -1;
	sceKernelExitDeleteThread(ret < 0 ? ret : 0);
	return 0;
}

/*
 * Shared by module_stop() and module_reboot_before(). Must never block the
 * system for long or fail: the main thread is usually parked in
 * sceDisplayWaitVblankStart(), which does not return while the display is
 * being shut down, so it is explicitly released instead of waited on, and
 * killed if it still does not finish in time.
 */
static void uvc_shutdown(const char *who, SceUInt wait_us)
{
	SceUInt timeout = wait_us;
	SceUID thid = main_thid;
	int ret;

	/* Flushed at once, while the Memory Stick is certainly still usable, so
	 * the log always shows that the hook ran even if a later step hangs. */
	LOG("%s entered\n", who);
	log_flush();

	run = 0;
	stream = 0;
	if (uvc_frame_req_evflag >= 0)
		sceKernelSetEventFlag(uvc_frame_req_evflag, EVENT_STOP_STREAM);

	if (thid >= 0 && sceKernelGetThreadId() != thid) {
		sceKernelReleaseWaitThread(thid);
		ret = sceKernelWaitThreadEnd(thid, &timeout);
		if (ret < 0 && main_thid >= 0) {
			LOG("%s: main thread did not finish (0x%08X); terminating it\n", who, ret);
			sceKernelTerminateThread(thid);
		}
	}

	/*
	 * The USB bus driver keeps pointers into this module (usb_driver,
	 * descriptors, endpoints) and DMAs from the TX buffers. If either goes
	 * away while the driver is still registered or active, the next USB event
	 * crashes the system. No-op when the main thread already unwound
	 * everything.
	 */
	uvc_teardown(&uvc_st);
	LOG("%s done\n", who);
	log_flush();
}

/*
 * Called by the system when the plugin is unloaded.
 */
int module_stop(SceSize args, void *argp)
{
	uvc_shutdown("module_stop", MODULE_STOP_TIMEOUT_US);
	return 0;
}

/*
 * Called by loadexec on every module right before the system reboots, which is
 * what happens when a game exits (HOME -> Exit Game). The kernel does not
 * unload kernel-mode plugins on that path, so module_stop() never runs there;
 * without this hook the streaming thread keeps sending USB requests and
 * reading the display while the USB bus driver, the display and memory are
 * torn down underneath it, and the system crashes.
 */
int module_reboot_before(void *arg1, int arg2, int arg3, int arg4)
{
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	uvc_shutdown("module_reboot_before", REBOOT_STOP_TIMEOUT_US);
	return 0;
}
