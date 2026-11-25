#ifndef UVC_UAC_H
#define UVC_UAC_H

/*
 * USB Audio Class 1.0 definitions: just what a single 16-bit stereo PCM
 * capture ("microphone"/line in) function needs.
 */

#define UAC_SC_UNDEFINED		0x00
#define UAC_SC_AUDIOCONTROL		0x01
#define UAC_SC_AUDIOSTREAMING		0x02

#define UAC_DT_CS_INTERFACE		0x24
#define UAC_DT_CS_ENDPOINT		0x25

/* Audio control interface descriptor subtypes */
#define UAC_AC_HEADER			0x01
#define UAC_AC_INPUT_TERMINAL		0x02
#define UAC_AC_OUTPUT_TERMINAL		0x03

/* Audio streaming interface descriptor subtypes */
#define UAC_AS_GENERAL			0x01
#define UAC_AS_FORMAT_TYPE		0x02

#define UAC_EP_GENERAL			0x01

#define UAC_FORMAT_TYPE_I		0x01
#define UAC_FORMAT_TAG_PCM		0x0001

#define UAC_TERMINAL_USB_STREAMING	0x0101
#define UAC_TERMINAL_LINE_CONNECTOR	0x0603

#define UAC_CHANNEL_LEFT_FRONT		0x0001
#define UAC_CHANNEL_RIGHT_FRONT		0x0002

/* Endpoint bmAttributes: isochronous, asynchronous (the device's own clock) */
#define UAC_EP_ATTR_ISO_ASYNC		0x05

/* UAC 1.0 standard AS isochronous endpoint descriptor: 7 bytes plus bRefresh
 * and bSynchAddress. */
#define UAC_DT_ENDPOINT_AUDIO_SIZE	9

struct __attribute__((packed)) uac_ac_header_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned short	bcdADC;
	unsigned short	wTotalLength;
	unsigned char	bInCollection;
	unsigned char	baInterfaceNr[1];
};

struct __attribute__((packed)) uac_input_terminal_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned char	bTerminalID;
	unsigned short	wTerminalType;
	unsigned char	bAssocTerminal;
	unsigned char	bNrChannels;
	unsigned short	wChannelConfig;
	unsigned char	iChannelNames;
	unsigned char	iTerminal;
};

struct __attribute__((packed)) uac_output_terminal_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned char	bTerminalID;
	unsigned short	wTerminalType;
	unsigned char	bAssocTerminal;
	unsigned char	bSourceID;
	unsigned char	iTerminal;
};

struct __attribute__((packed)) uac_as_general_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned char	bTerminalLink;
	unsigned char	bDelay;
	unsigned short	wFormatTag;
};

/* Type I format with one discrete sampling frequency. */
struct __attribute__((packed)) uac_format_type_i_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned char	bFormatType;
	unsigned char	bNrChannels;
	unsigned char	bSubframeSize;
	unsigned char	bBitResolution;
	unsigned char	bSamFreqType;
	unsigned char	tSamFreq[3];
};

struct __attribute__((packed)) uac_cs_endpoint_descriptor {
	unsigned char	bLength;
	unsigned char	bDescriptorType;
	unsigned char	bDescriptorSubtype;
	unsigned char	bmAttributes;
	unsigned char	bLockDelayUnits;
	unsigned short	wLockDelay;
};

#endif
