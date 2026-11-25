#include <string.h>
#include "uvc.h"
#include "uac.h"
#include "audio.h"

/* Build with UVC_AUDIO=0 (see Makefile) for a video-only device. */
#ifndef UVC_AUDIO
#define UVC_AUDIO 1
#endif
/* USB endpoint number of the audio IN endpoint (the driver slot stays endpoints[2]). */
#ifndef UVC_AUDIO_EP
#define UVC_AUDIO_EP 2
#endif
/* Capture hooks on the sceAudio syscalls; 0 keeps the (silent) audio device only. */
#ifndef UVC_AUDIO_HOOKS
#define UVC_AUDIO_HOOKS 1
#endif

/*
 * USB definitions
 */

#define USB_DT_CS_DEVICE		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_DEVICE)
#define USB_DT_CS_CONFIG		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_CONFIG)
#define USB_DT_CS_STRING		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_STRING)
#define USB_DT_CS_INTERFACE		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_INTERFACE)
#define USB_DT_CS_ENDPOINT		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_ENDPOINT)

/*
 * UVC Configurable options
 */

#define DUMMY_INTERFACE			0
#define CONTROL_INTERFACE 		1
#define STREAM_INTERFACE		2

#define INTERFACE_CTRL_ID		0
#define INPUT_TERMINAL_ID		1
#define OUTPUT_TERMINAL_ID		2

#define FORMAT_INDEX_UNCOMPRESSED_YUY2	1

#if UVC_AUDIO
/* Audio function: dummy interface (carries the IAD), control, streaming. */
#define AUDIO_DUMMY_INTERFACE		3
#define AUDIO_CONTROL_INTERFACE		4
#define AUDIO_STREAM_INTERFACE		5
#define AUDIO_INPUT_TERMINAL_ID		1
#define AUDIO_OUTPUT_TERMINAL_ID	2
#define AUDIO_ENDPOINT_INDEX		2	/* endpoints[] slot and endpoint number */
#define AUDIO_ALT_STREAMING		1
#define AUDIO_EP_MAXPACKET		192	/* >= AUDIO_PKT_MAX_FRAMES * 4 */

/* High-speed bInterval of the audio endpoint (see Makefile) */
#ifndef UVC_AUDIO_HS_INTERVAL
#define UVC_AUDIO_HS_INTERVAL	4
#endif
#define NUM_INTERFACES			6
#define NUM_ENDPOINTS			3
#else
#define NUM_INTERFACES			3
#define NUM_ENDPOINTS			2
#endif

/*
 * Helper macros
 */

#define VIDEO_FRAME_SIZE_YUY2(w, h)		((w) * (h) * 2)

#define FRAME_BITRATE(w, h, bpp, interval)	(((w) * (h) * (bpp)) / ((interval) * 100 * 1E-9))
#define FPS_TO_INTERVAL(fps)			((1E9 / 100) / (fps))

/* Interface Association Descriptor */
static
unsigned char interface_association_descriptor[] = {
	UVC_INTERFACE_ASSOCIATION_DESC_SIZE,		/* Descriptor Size: 8 */
	UVC_INTERFACE_ASSOCIATION_DESCRIPTOR_TYPE,	/* Interface Association Descr Type: 11 */
	CONTROL_INTERFACE,				/* I/f number of first VideoControl i/f */
	0x02,						/* Number of Video i/f */
	USB_CLASS_VIDEO,				/* CC_VIDEO : Video i/f class code */
	UVC_SC_VIDEO_INTERFACE_COLLECTION,		/* SC_VIDEO_INTERFACE_COLLECTION : Subclass code */
	UVC_PC_PROTOCOL_UNDEFINED,			/* Protocol : Not used */
	0x00,						/* String desc index for interface */
};

DECLARE_UVC_HEADER_DESCRIPTOR(1);

static struct __attribute__((packed)) {
	struct UVC_HEADER_DESCRIPTOR(1) header_descriptor;
	struct uvc_input_terminal_descriptor input_terminal_descriptor;
	struct uvc_output_terminal_descriptor output_terminal_descriptor;
} video_control_descriptors = {
	.header_descriptor = {
		.bLength			= sizeof(video_control_descriptors.header_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_HEADER,
		.bcdUVC				= 0x0110,
		.wTotalLength			= sizeof(video_control_descriptors),
		.dwClockFrequency		= 48000000,
		.bInCollection			= 1,
		.baInterfaceNr			= {STREAM_INTERFACE},
	},
	.input_terminal_descriptor = {
		.bLength			= sizeof(video_control_descriptors.input_terminal_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_INPUT_TERMINAL,
		.bTerminalID			= INPUT_TERMINAL_ID,
		.wTerminalType			= UVC_ITT_VENDOR_SPECIFIC,
		.bAssocTerminal			= 0,
		.iTerminal			= 0,
	},
	.output_terminal_descriptor = {
		.bLength			= sizeof(video_control_descriptors.output_terminal_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_OUTPUT_TERMINAL,
		.bTerminalID			= OUTPUT_TERMINAL_ID,
		.wTerminalType			= UVC_TT_STREAMING,
		.bAssocTerminal			= 0,
		.bSourceID			= INPUT_TERMINAL_ID,
		.iTerminal			= 0,
	},
};

DECLARE_UVC_INPUT_HEADER_DESCRIPTOR(1, 1);
DECLARE_UVC_FRAME_UNCOMPRESSED(2);

static struct __attribute__((packed)) {
	struct UVC_INPUT_HEADER_DESCRIPTOR(1, 1) input_header_descriptor;
	struct uvc_format_uncompressed format_uncompressed_yuy2;
	struct UVC_FRAME_UNCOMPRESSED(2) frames_uncompressed_yuy2[1];
	struct uvc_color_matching_descriptor format_uncompressed_yuy2_color_matching;
} video_streaming_descriptors = {
	.input_header_descriptor = {
		.bLength			= sizeof(video_streaming_descriptors.input_header_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_INPUT_HEADER,
		.bNumFormats			= 1,
		.wTotalLength			= sizeof(video_streaming_descriptors),
		.bEndpointAddress		= USB_ENDPOINT_IN | 0x01,
		.bmInfo				= 0,
		.bTerminalLink			= OUTPUT_TERMINAL_ID,
		.bStillCaptureMethod		= 0,
		.bTriggerSupport		= 0,
		.bTriggerUsage			= 0,
		.bControlSize			= 1,
		.bmaControls			= {{0}, },
	},
	.format_uncompressed_yuy2 = {
		.bLength			= sizeof(video_streaming_descriptors.format_uncompressed_yuy2),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_FORMAT_UNCOMPRESSED,
		.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_YUY2,
		.bNumFrameDescriptors		= 1,
		.guidFormat			= UVC_GUID_FORMAT_YUY2,
		.bBitsPerPixel			= 16,
		.bDefaultFrameIndex		= 1,
		.bAspectRatioX			= 0,
		.bAspectRatioY			= 0,
		.bmInterfaceFlags		= 0,
		.bCopyProtect			= 0,
	},
	.frames_uncompressed_yuy2 = {
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 4,
			.bmCapabilities			= 0,
			.wWidth				= 480,
			.wHeight			= 272,
			.dwMinBitRate			= FRAME_BITRATE(480, 272, 12, FPS_TO_INTERVAL(30)),
			.dwMaxBitRate			= FRAME_BITRATE(480, 272, 12, FPS_TO_INTERVAL(60)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_YUY2(480, 272),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(60),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(60), FPS_TO_INTERVAL(30)},
		},
	},
	.format_uncompressed_yuy2_color_matching = {
		.bLength			= sizeof(video_streaming_descriptors.format_uncompressed_yuy2_color_matching),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_COLORFORMAT,
		.bColorPrimaries		= 0,
		.bTransferCharacteristics	= 0,
		.bMatrixCoefficients		= 0,
	},
};

#if UVC_AUDIO
/* Interface Association Descriptor for the audio function */
static
unsigned char audio_interface_association_descriptor[] = {
	UVC_INTERFACE_ASSOCIATION_DESC_SIZE,		/* Descriptor Size: 8 */
	UVC_INTERFACE_ASSOCIATION_DESCRIPTOR_TYPE,	/* Interface Association Descr Type: 11 */
	AUDIO_CONTROL_INTERFACE,			/* First AudioControl interface */
	0x02,						/* Number of audio interfaces */
	USB_CLASS_AUDIO,				/* Function class */
	UAC_SC_UNDEFINED,				/* Function subclass */
	0x00,						/* Function protocol */
	0x00,						/* String desc index for function */
};

static struct __attribute__((packed)) {
	struct uac_ac_header_descriptor header;
	struct uac_input_terminal_descriptor input_terminal;
	struct uac_output_terminal_descriptor output_terminal;
} audio_control_descriptors = {
	.header = {
		.bLength		= sizeof(audio_control_descriptors.header),
		.bDescriptorType	= UAC_DT_CS_INTERFACE,
		.bDescriptorSubtype	= UAC_AC_HEADER,
		.bcdADC			= 0x0100,
		.wTotalLength		= sizeof(audio_control_descriptors),
		.bInCollection		= 1,
		.baInterfaceNr		= {AUDIO_STREAM_INTERFACE},
	},
	.input_terminal = {
		.bLength		= sizeof(audio_control_descriptors.input_terminal),
		.bDescriptorType	= UAC_DT_CS_INTERFACE,
		.bDescriptorSubtype	= UAC_AC_INPUT_TERMINAL,
		.bTerminalID		= AUDIO_INPUT_TERMINAL_ID,
		.wTerminalType		= UAC_TERMINAL_LINE_CONNECTOR,
		.bAssocTerminal		= 0,
		.bNrChannels		= 2,
		.wChannelConfig		= UAC_CHANNEL_LEFT_FRONT | UAC_CHANNEL_RIGHT_FRONT,
		.iChannelNames		= 0,
		.iTerminal		= 0,
	},
	.output_terminal = {
		.bLength		= sizeof(audio_control_descriptors.output_terminal),
		.bDescriptorType	= UAC_DT_CS_INTERFACE,
		.bDescriptorSubtype	= UAC_AC_OUTPUT_TERMINAL,
		.bTerminalID		= AUDIO_OUTPUT_TERMINAL_ID,
		.wTerminalType		= UAC_TERMINAL_USB_STREAMING,
		.bAssocTerminal		= 0,
		.bSourceID		= AUDIO_INPUT_TERMINAL_ID,
		.iTerminal		= 0,
	},
};

/* Class-specific descriptors of the streaming interface's alternate setting 1 */
static struct __attribute__((packed)) {
	struct uac_as_general_descriptor general;
	struct uac_format_type_i_descriptor format;
} audio_streaming_descriptors = {
	.general = {
		.bLength		= sizeof(audio_streaming_descriptors.general),
		.bDescriptorType	= UAC_DT_CS_INTERFACE,
		.bDescriptorSubtype	= UAC_AS_GENERAL,
		.bTerminalLink		= AUDIO_OUTPUT_TERMINAL_ID,
		.bDelay			= 1,
		.wFormatTag		= UAC_FORMAT_TAG_PCM,
	},
	.format = {
		.bLength		= sizeof(audio_streaming_descriptors.format),
		.bDescriptorType	= UAC_DT_CS_INTERFACE,
		.bDescriptorSubtype	= UAC_AS_FORMAT_TYPE,
		.bFormatType		= UAC_FORMAT_TYPE_I,
		.bNrChannels		= 2,
		.bSubframeSize		= 2,
		.bBitResolution		= 16,
		.bSamFreqType		= 1,
		.tSamFreq		= {AUDIO_RATE & 0xFF, (AUDIO_RATE >> 8) & 0xFF, (AUDIO_RATE >> 16) & 0xFF},
	},
};

/*
 * Everything that follows the 7-byte endpoint prefix of the audio streaming
 * endpoint. The PSP bus driver (6.61) stores only those 7 bytes inline; for a
 * UAC 1.0 endpoint (bLength 9) bRefresh and bSynchAddress belong to the
 * extension, followed by the class-specific audio endpoint descriptor (no
 * controls). This is the layout the USB MIDI driver uses for its own
 * 9-byte audio-class endpoints.
 */
struct __attribute__((packed)) audio_endpoint_extension_desc {
	unsigned char bRefresh;
	unsigned char bSynchAddress;
	struct uac_cs_endpoint_descriptor cs;
};

static struct audio_endpoint_extension_desc audio_endpoint_extension = {
	.bRefresh		= 0,
	.bSynchAddress		= 0,
	.cs = {
		.bLength		= sizeof(struct uac_cs_endpoint_descriptor),
		.bDescriptorType	= UAC_DT_CS_ENDPOINT,
		.bDescriptorSubtype	= UAC_EP_GENERAL,
		.bmAttributes		= 0,
		.bLockDelayUnits	= 0,
		.wLockDelay		= 0,
	},
};

/* wTotalLength without the audio function */
#define VIDEO_CONFIG_LENGTH \
	(USB_DT_CONFIG_SIZE + 3 * USB_DT_INTERFACE_SIZE + 1 * USB_DT_ENDPOINT_SIZE + \
	 sizeof(interface_association_descriptor) + \
	 sizeof(video_control_descriptors) + \
	 sizeof(video_streaming_descriptors))

/* Everything the audio function adds to wTotalLength */
#define AUDIO_CONFIG_LENGTH \
	(3 * USB_DT_INTERFACE_SIZE +			/* dummy, control, streaming alt 0 */ \
	 1 * USB_DT_INTERFACE_SIZE +			/* streaming alt 1 */ \
	 UAC_DT_ENDPOINT_AUDIO_SIZE +			/* iso endpoint (9 bytes) */ \
	 sizeof(audio_interface_association_descriptor) + \
	 sizeof(audio_control_descriptors) + \
	 sizeof(audio_streaming_descriptors) + \
	 sizeof(struct uac_cs_endpoint_descriptor))
#else
#define AUDIO_CONFIG_LENGTH	0
#endif

/*
 * The bus driver reads its interface table as { up to two descriptors (the
 * alternate settings), count }. Same 12 bytes as struct InterfaceSettings.
 */
struct psp_usb_interfaces {
	struct InterfaceDescriptor *infp[2];
	unsigned int num;
};

/* Endpoint blocks */
static
struct SceUdcdEndpoint endpoints[NUM_ENDPOINTS] __attribute__((aligned(16))) = {
	{0, 0, 0},
	{1, 0, 0},
#if UVC_AUDIO
	{2, 0, 0},
#endif
};

/* Interface */
static
struct SceUdcdInterface interface __attribute__((aligned(16))) = {
	.expectNumber		= -1,
	.interfaceNumber	= 0,
	.numInterfaces		= NUM_INTERFACES
};

#if 0
/* String descriptors */
static
struct SceUdcdStringDescriptor string_descriptor_product = {
	8,
	USB_DT_STRING,
	{'P', 'S', 'P'}
};

static
struct SceUdcdStringDescriptor string_descriptor_serial = {
	8,
	USB_DT_STRING,
	{'U', 'V', 'C'}
};
#endif

/* Hi-Speed device descriptor */
static
struct SceUdcdDeviceDescriptor devdesc_hi __attribute__((aligned(16))) = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,				/* bcdUSB */
	USB_DEVICE_CLASS_MISCELLANEOUS,	/* bDeviceClass (Miscellaneous Device Class)*/
	0x02,				/* bDeviceSubClass (Common Class) */
	0x01,				/* bDeviceProtocol (Interface Association Descriptor) */
	64,				/* bMaxPacketSize0 */
	0,				/* idProduct */
	0,				/* idVendor */
	0x100,				/* bcdDevice */
	0,				/* iManufacturer */
	2,				/* iProduct */
	3,				/* iSerialNumber */
	1				/* bNumConfigurations */
};

/* Hi-Speed endpoint descriptors */
static
struct SceUdcdEndpointDescriptor endpdesc_hi[NUM_ENDPOINTS] __attribute__((aligned(16))) = {
	/* Video Streaming endpoints */
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | 0x01,		/* bEndpointAddress */
		USB_ENDPOINT_TYPE_BULK,		/* bmAttributes */
		0x200,				/* wMaxPacketSize */
		0x00				/* bInterval */
	},
#if UVC_AUDIO
	/* Audio Streaming endpoint: isochronous IN, one packet per millisecond */
	{
		.bLength		= UAC_DT_ENDPOINT_AUDIO_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_ENDPOINT_IN | UVC_AUDIO_EP,
		.bmAttributes		= UAC_EP_ATTR_ISO_ASYNC,
		.wMaxPacketSize		= AUDIO_EP_MAXPACKET,
		.bInterval		= UVC_AUDIO_HS_INTERVAL,
		.extra			= (unsigned char *)&audio_endpoint_extension,
		.extraLength		= sizeof(audio_endpoint_extension),
	},
#endif
	{
		0,
	}
};

/* Hi-Speed interface descriptor */
static
struct SceUdcdInterfaceDescriptor interdesc_hi[NUM_INTERFACES + (UVC_AUDIO ? 2 : 1)] __attribute__((aligned(16))) = {
	{	/* Dummy interface just to include the IAD */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		DUMMY_INTERFACE,                /* bInterfaceNumber */
		0,                              /* bAlternateSetting */
		0,                              /* bNumEndpoints */
		USB_CLASS_VENDOR_SPEC,          /* bInterfaceClass */
		0,                              /* bInterfaceSubClass */
		0,                              /* bInterfaceProtocol */
		0,                              /* iInterface */
		NULL,                           /* endpoints */
		(void *)&interface_association_descriptor,
		sizeof(interface_association_descriptor)
	},
	{	/* Standard Video Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		CONTROL_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOCONTROL,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&video_control_descriptors,
		sizeof(video_control_descriptors)
	},
	{	/* Standard Video Streaming Interface Descriptor */
		/* Alternate setting 0 = Operational Setting */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOSTREAMING,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_hi[0],		/* endpoints */
		(void *)&video_streaming_descriptors,
		sizeof(video_streaming_descriptors)
	},
#if UVC_AUDIO
	{	/* Dummy interface just to include the audio IAD */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_DUMMY_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VENDOR_SPEC,		/* bInterfaceClass */
		0,				/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&audio_interface_association_descriptor,
		sizeof(audio_interface_association_descriptor)
	},
	{	/* Standard Audio Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_CONTROL_INTERFACE,	/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOCONTROL,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&audio_control_descriptors,
		sizeof(audio_control_descriptors)
	},
	{	/* Audio Streaming Interface, alternate setting 0: no bandwidth */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOSTREAMING,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		NULL,
		0
	},
	{	/* Audio Streaming Interface, alternate setting 1: streaming */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_STREAM_INTERFACE,		/* bInterfaceNumber */
		AUDIO_ALT_STREAMING,		/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOSTREAMING,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_hi[1],		/* endpoints */
		(void *)&audio_streaming_descriptors,
		sizeof(audio_streaming_descriptors)
	},
#endif
	{
		0
	}
};

/* Hi-Speed settings */
static
struct psp_usb_interfaces settings_hi[NUM_INTERFACES] __attribute__((aligned(16))) = {
	{{&interdesc_hi[0], NULL}, 1},
	{{&interdesc_hi[1], NULL}, 1},
	{{&interdesc_hi[2], NULL}, 1},
#if UVC_AUDIO
	{{&interdesc_hi[3], NULL}, 1},
	{{&interdesc_hi[4], NULL}, 1},
	/* streaming interface: alternate settings 0 and 1 are the contiguous
	 * records interdesc[5] and [6]; the firmware steps from the first by one
	 * record per alternate, so the second word stays 0 */
	{{&interdesc_hi[5], NULL}, 2},
#endif
};

/* Hi-Speed configuration descriptor */
static
struct SceUdcdConfigDescriptor confdesc_hi __attribute__((aligned(16))) = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	(VIDEO_CONFIG_LENGTH + AUDIO_CONFIG_LENGTH),	/* wTotalLength */
	NUM_INTERFACES,		/* bNumInterfaces */
	1,			/* bConfigurationValue */
	0,			/* iConfiguration */
	0x80,			/* bmAttributes */
	250,			/* bMaxPower */
	(struct SceUdcdInterfaceSettings *)&settings_hi[0]
};

/* Hi-Speed configuration */
static
struct SceUdcdConfiguration config_hi __attribute__((aligned(16))) = {
	&confdesc_hi,
	(struct SceUdcdInterfaceSettings *)&settings_hi[0],
	&interdesc_hi[0],
	&endpdesc_hi[0]
};

/* Full-Speed device descriptor */
static
struct SceUdcdDeviceDescriptor devdesc_full __attribute__((aligned(16))) = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,				/* bcdUSB */
	USB_DEVICE_CLASS_MISCELLANEOUS,	/* bDeviceClass (Miscellaneous Device Class)*/
	0x02,				/* bDeviceSubClass (Common Class) */
	0x01,				/* bDeviceProtocol (Interface Association Descriptor) */
	0x40,				/* bMaxPacketSize0 */
	0,				/* idProduct */
	0,				/* idVendor */
	0x100,				/* bcdDevice */
	0,				/* iManufacturer */
	2,				/* iProduct */
	3,				/* iSerialNumber */
	1				/* bNumConfigurations */
};

/* Full-Speed endpoint descriptors */
static
struct SceUdcdEndpointDescriptor endpdesc_full[NUM_ENDPOINTS] __attribute__((aligned(16))) = {
	/* Video Streaming endpoints */
	{
		USB_DT_ENDPOINT_SIZE,
		USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | 0x01,		/* bEndpointAddress */
		USB_ENDPOINT_TYPE_BULK,		/* bmAttributes */
		0x40,				/* wMaxPacketSize */
		0x00				/* bInterval */
	},
#if UVC_AUDIO
	/* Audio Streaming endpoint: isochronous IN, one packet per millisecond */
	{
		.bLength		= UAC_DT_ENDPOINT_AUDIO_SIZE,
		.bDescriptorType	= USB_DT_ENDPOINT,
		.bEndpointAddress	= USB_ENDPOINT_IN | UVC_AUDIO_EP,
		.bmAttributes		= UAC_EP_ATTR_ISO_ASYNC,
		.wMaxPacketSize		= AUDIO_EP_MAXPACKET,
		.bInterval		= 1,
		.extra			= (unsigned char *)&audio_endpoint_extension,
		.extraLength		= sizeof(audio_endpoint_extension),
	},
#endif
	{
		0,
	}
};

/* Full-Speed interface descriptor */
static
struct SceUdcdInterfaceDescriptor interdesc_full[NUM_INTERFACES + (UVC_AUDIO ? 2 : 1)] __attribute__((aligned(16))) = {
	{	/* Dummy interface just to include the IAD */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		DUMMY_INTERFACE,                /* bInterfaceNumber */
		0,                              /* bAlternateSetting */
		0,                              /* bNumEndpoints */
		USB_CLASS_VENDOR_SPEC,          /* bInterfaceClass */
		0,                              /* bInterfaceSubClass */
		0,                              /* bInterfaceProtocol */
		0,                              /* iInterface */
		NULL,                           /* endpoints */
		(void *)&interface_association_descriptor,
		sizeof(interface_association_descriptor)
	},
	{	/* Standard Video Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		CONTROL_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOCONTROL,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&video_control_descriptors,
		sizeof(video_control_descriptors)
	},
	{	/* Standard Video Streaming Interface Descriptor */
		/* Alternate setting 0 = Operational Setting */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOSTREAMING,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_full[0],		/* endpoints */
		(void *)&video_streaming_descriptors,
		sizeof(video_streaming_descriptors)
	},
#if UVC_AUDIO
	{	/* Dummy interface just to include the audio IAD */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_DUMMY_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VENDOR_SPEC,		/* bInterfaceClass */
		0,				/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&audio_interface_association_descriptor,
		sizeof(audio_interface_association_descriptor)
	},
	{	/* Standard Audio Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_CONTROL_INTERFACE,	/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOCONTROL,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&audio_control_descriptors,
		sizeof(audio_control_descriptors)
	},
	{	/* Audio Streaming Interface, alternate setting 0: no bandwidth */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOSTREAMING,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		NULL,
		0
	},
	{	/* Audio Streaming Interface, alternate setting 1: streaming */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		AUDIO_STREAM_INTERFACE,		/* bInterfaceNumber */
		AUDIO_ALT_STREAMING,		/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_AUDIO,		/* bInterfaceClass */
		UAC_SC_AUDIOSTREAMING,		/* bInterfaceSubClass */
		0,				/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_full[1],		/* endpoints */
		(void *)&audio_streaming_descriptors,
		sizeof(audio_streaming_descriptors)
	},
#endif
	{
		0
	}
};

/* Full-Speed settings */
static
struct psp_usb_interfaces settings_full[NUM_INTERFACES] __attribute__((aligned(16))) = {
	{{&interdesc_full[0], NULL}, 1},
	{{&interdesc_full[1], NULL}, 1},
	{{&interdesc_full[2], NULL}, 1},
#if UVC_AUDIO
	{{&interdesc_full[3], NULL}, 1},
	{{&interdesc_full[4], NULL}, 1},
	/* streaming interface: alternate settings 0 and 1 are the contiguous
	 * records interdesc[5] and [6]; the firmware steps from the first by one
	 * record per alternate, so the second word stays 0 */
	{{&interdesc_full[5], NULL}, 2},
#endif
};

/* Full-Speed configuration descriptor */
static
struct SceUdcdConfigDescriptor confdesc_full __attribute__((aligned(16))) = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	(VIDEO_CONFIG_LENGTH + AUDIO_CONFIG_LENGTH),	/* wTotalLength */
	NUM_INTERFACES,		/* bNumInterfaces */
	1,			/* bConfigurationValue */
	0,			/* iConfiguration */
	0x80,			/* bmAttributes */
	250,			/* bMaxPower */
	(struct SceUdcdInterfaceSettings *)&settings_full[0]
};

/* Full-Speed configuration */
static
struct SceUdcdConfiguration config_full __attribute__((aligned(16))) = {
	&confdesc_full,
	(struct SceUdcdInterfaceSettings *)&settings_full[0],
	&interdesc_full[0],
	&endpdesc_full[0]
};

#if UVC_AUDIO
/*
 * If the host refuses the full configuration, main() re-registers the driver
 * with a smaller one. Level 1 drops the class-specific audio endpoint
 * descriptor (the only descriptor that hangs off an endpoint); level 2 drops
 * the audio function altogether, which is exactly the video-only device.
 */
static void uvc_descriptors_degrade(int level)
{
	if (level == 1) {
		/* keep bRefresh/bSynchAddress (the endpoint stays 9 bytes), drop the rest */
		endpdesc_hi[1].extraLength = 2;
		endpdesc_full[1].extraLength = 2;
		confdesc_hi.wTotalLength -= sizeof(struct uac_cs_endpoint_descriptor);
		confdesc_full.wTotalLength -= sizeof(struct uac_cs_endpoint_descriptor);
	} else if (level >= 2) {
		memset(&interdesc_hi[3], 0, sizeof(interdesc_hi[3]));
		memset(&interdesc_full[3], 0, sizeof(interdesc_full[3]));
		memset(&endpdesc_hi[1], 0, sizeof(endpdesc_hi[1]));
		memset(&endpdesc_full[1], 0, sizeof(endpdesc_full[1]));
		confdesc_hi.wTotalLength = VIDEO_CONFIG_LENGTH;
		confdesc_full.wTotalLength = VIDEO_CONFIG_LENGTH;
		confdesc_hi.bNumInterfaces = 3;
		confdesc_full.bNumInterfaces = 3;
		interface.numInterfaces = 3;
	}
}
#endif
