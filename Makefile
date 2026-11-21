# Parallel build using every available core.
MAKEFLAGS += -j$(shell nproc)

# FORCE (below) must not become the default goal; build.mak defines "all".
.DEFAULT_GOAL := all

TARGET = uvc
OBJS    = src/main.o src/log.o src/format_conversion.o stubs/sceDmacplus_driver.o

INCDIR   = include
CFLAGS   = -G0 -Wall -O2 -MMD -MP
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
