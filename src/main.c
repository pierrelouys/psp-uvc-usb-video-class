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
#define TX_BLOCK_SIZE		(TX_BUF_COUNT * TX_BUF_STRIDE + TX_BUF_ALIGN)

_Static_assert(UVC_FRAME_H % UVC_CHUNK_ROWS == 0, "chunks must cover the frame exactly");

static SceUID tx_buf_blk = -1;
static unsigned char *tx_buf[TX_BUF_COUNT];

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
	return 0;
}

static int usb_attach(int usb_version)
{
	LOG("usb_attach %d\n", usb_version);
	return 0;
}

static void usb_detach(void)
{
	LOG("usb_detach\n");
	uvc_handle_video_abort();
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
	return 0;
}

static struct UsbDriver usb_driver = {
	.driverName			= USB_DRIVERNAME,
	.numEndpoints			= 2,
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
};

/* Shared with module_stop(), which must be able to unwind the USB driver
 * itself if the main thread cannot. */
static struct uvc_state uvc_st;

static void uvc_teardown(struct uvc_state *st)
{
	int ret;

	uvc_handle_video_abort();

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

static int uvc_run(struct uvc_state *st)
{
	struct stream_stats stats = {0};
	unsigned int stats_t0;
	int last_usb_state = -1;
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
