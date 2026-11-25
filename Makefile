# Parallel build using every available core.
MAKEFLAGS += -j$(shell nproc)

# FORCE (below) must not become the default goal; build.mak defines "all".
.DEFAULT_GOAL := all

TARGET = uvc
OBJS    = src/main.o src/log.o src/format_conversion.o src/audio.o stubs/sceDmacplus_driver.o stubs/LoadCoreForKernel.o

INCDIR   = include
# UVC_AUDIO=0 builds a video-only device (no audio interface, no hooks).
# UVC_AUDIO_HOOKS=0 keeps the audio device but skips the sceAudio hooks (silent).
UVC_AUDIO ?= 1
UVC_AUDIO_HOOKS ?= 1
# A 0.5 s beep at the start of each audio stream, to prove the host receives samples.
UVC_AUDIO_BEEP ?= 1
# USB endpoint number of the audio IN endpoint. 2 completes requests; 3 never did.
UVC_AUDIO_EP ?= 2
# Timing experiment: fixed test packets sent after a sweeping delay (see src/main.c).
UVC_AUDIO_SWEEP ?= 0
# Experiment: cycle the USB bus driver's frame targeting once a second, with tagged test packets.
UVC_AUDIO_FRAMESWEEP ?= 0
# First way of feeding the isochronous endpoint (index into audio_strategies[] in src/main.c).
UVC_AUDIO_STRATEGY ?= 0
# High-speed bInterval of the audio endpoint. The PSP bus driver arms isochronous
# packets for frame (FN+1)|((1<<(bInterval-1))-1), so this decides when it sends.
UVC_AUDIO_HS_INTERVAL ?= 4
# bInterval the bus driver is given once the host has read the descriptors (0 = leave alone).
UVC_AUDIO_RT_INTERVAL ?= 0
# Patch the USB bus driver's isochronous frame targeting (see src/main.c).
# Resend each millisecond of audio every microframe until the next is due.
UVC_AUDIO_REPEAT ?= 1
UVC_USBBUS_PATCH ?= 1
UVC_USBBUS_ADD ?= 1
UVC_USBBUS_MASK ?= 0
CFLAGS   = -G0 -Wall -O2 -MMD -MP -DUVC_AUDIO=$(UVC_AUDIO) -DUVC_AUDIO_HOOKS=$(UVC_AUDIO_HOOKS) -DUVC_AUDIO_BEEP=$(UVC_AUDIO_BEEP) -DUVC_AUDIO_EP=$(UVC_AUDIO_EP) -DUVC_AUDIO_SWEEP=$(UVC_AUDIO_SWEEP) -DUVC_AUDIO_FRAMESWEEP=$(UVC_AUDIO_FRAMESWEEP) -DUVC_AUDIO_STRATEGY=$(UVC_AUDIO_STRATEGY) -DUVC_AUDIO_HS_INTERVAL=$(UVC_AUDIO_HS_INTERVAL) -DUVC_AUDIO_RT_INTERVAL=$(UVC_AUDIO_RT_INTERVAL) -DUVC_AUDIO_REPEAT=$(UVC_AUDIO_REPEAT) -DUVC_USBBUS_PATCH=$(UVC_USBBUS_PATCH) -DUVC_USBBUS_ADD=$(UVC_USBBUS_ADD) -DUVC_USBBUS_MASK=$(UVC_USBBUS_MASK)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)
LIBDIR   =
LDFLAGS  =
LIBS     = -lpspusb_driver -lpspusbbus_driver
DEPS     = $(OBJS:.o=.d)

PSP_FW_VERSION = 660
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = UVC - USB Video Class

EXTRA_CLEAN=$(DEPS)

# Build date/time with timezone, written as the first line of every log session.
# log.o is always rebuilt so the stamp matches the PRX that is actually produced.
BUILD_TIMESTAMP := $(shell date '+%Y-%m-%d %H:%M:%S %Z %z')
src/log.o: CFLAGS += -DBUILD_TIMESTAMP='"$(BUILD_TIMESTAMP)"'
# Every object is rebuilt on every make. File times inside a zip carry no
# timezone, so an extracted source file can look older than a stale object and
# make would silently link old code against new code.
$(OBJS): FORCE
.PHONY: FORCE
FORCE:

PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

copy: EBOOT.PBP
	@mkdir -p $(PSP_MS)/PSP/GAME/$(TARGET)
	@cp EBOOT.PBP $(PSP_MS)/PSP/GAME/$(TARGET)
	@sync
	@echo "Copied!"

copyprx: $(TARGET).prx
	@cp $^ $(PSP_MS)/seplugins/
	@sync
	@echo "Copied!"

-include $(DEPS)
