###########################################################
# Netplay.pak session-setup app
#
# Needs a NextUI checkout: the UI uses GFX_/PAD_ from common/api.c, which is
# compiled straight into minarch and so unreachable from the shim. We compile
# our own copy, exactly as the original pak's app did. The source and platform
# flags must come from the matching NextUI tree because platform.c is part of
# the app and is not a stable external SDK.
#
#   make PLATFORM=tg5040 NEXTUI=/path/to/NextUI [FIRMWARE=spruce]
#
# FIRMWARE=spruce builds the same app for spruce on the same hardware. NextUI's
# UI code still draws it, but spruce owns the device's settings, so libmsettings
# is replaced by spruce/msettings.c, which never touches the hardware; and the
# UI's compiled-in card root moves into the app's own folder (see spruce/).
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050 my282 my355 h700
TARGET  = netplay.elf
BROKER_TARGET = netplay-broker.elf
WIFIPS_TARGET = netplay-wifips.elf
QUICKMENU_TARGET = netplay-quickmenu.elf
NEXTUI ?= ../../../NextUI

ifeq (,$(PLATFORM))
PLATFORM = $(UNION_PLATFORM)
endif
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif

WS       = $(NEXTUI)/workspace
COMMON   = $(WS)/all/common
PLATDIR  = $(WS)/$(PLATFORM)/platform

# This is the platform ABI contract: CPU tuning, SDL/GLES include paths and the
# exact platform libraries. Reconstructing it here had already made tg5050 and
# my355 generic aarch64 builds and missed h700's GLES linking exception.
include $(PLATDIR)/makefile.env

FIRMWARE ?= nextui
ifeq (,$(filter $(FIRMWARE),nextui spruce))
$(error invalid FIRMWARE '$(FIRMWARE)'. Supported: nextui spruce)
endif

SOURCE   = main.c netsetup.c hints.c nextui/chordcap.c \
           $(COMMON)/api.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/scaler.c \
           $(PLATDIR)/platform.c

# NEXTUI_SDK_PREFIX is set by build-platforms and holds a libmsettings built
# against the pinned source; the in-tree path is the fallback for a plain
# toolchain container. Both are offered so either environment works.
SDKPREFIX ?= $(NEXTUI_SDK_PREFIX)
INCDIR   = -I. -Inextui -I$(COMMON) -I$(PLATDIR) -I$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
INCDIR  += -I$(SDKPREFIX)/include
endif

BIN      = ../bin/$(PLATFORM)
ifeq (spruce,$(FIRMWARE))
# platform.h fixes the card root NextUI's UI reads its fonts and settings from.
# A copy with that root moved into App/Netplay/nextui is put first on the
# include path; its include guard is the original's, so it is the one
# platform.c sees too.
SPRUCE_INC = ../build/spruce/$(PLATFORM)/include
INCDIR  := -I$(SPRUCE_INC) $(INCDIR)
SOURCE  += spruce/msettings.c
BIN      = ../bin/spruce/$(PLATFORM)
FIRMWARE_FLAGS = -DNETPLAY_SPRUCE
endif

CFLAGS  += $(OPT) $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" $(FIRMWARE_FLAGS) -std=gnu99
LDFLAGS += -L$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
LDFLAGS += -L$(SDKPREFIX)/lib
endif
ifeq (nextui,$(FIRMWARE))
LDFLAGS += -lmsettings
endif
LDFLAGS += -lsamplerate -lzip

# The detached broker links netsetup.c for discovery/compatibility logic but no
# frontend, audio, settings-library or archive APIs. Keep its idle process truly
# small instead of loading the setup app's entire SDL/GLES dependency graph.
BROKER_LDFLAGS = $(filter -L%,$(LDFLAGS)) -ldl

PRODUCT = $(BIN)/$(TARGET)
BROKER_PRODUCT = $(BIN)/$(BROKER_TARGET)
WIFIPS_PRODUCT = $(BIN)/$(WIFIPS_TARGET)
QUICKMENU_PRODUCT = $(BIN)/$(QUICKMENU_TARGET)

all: $(if $(SPRUCE_INC),$(SPRUCE_INC)/platform.h)
	@mkdir -p $(BIN)
	$(CROSS_COMPILE)gcc $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"
	$(CROSS_COMPILE)gcc broker.c netsetup.c -o $(BROKER_PRODUCT) $(CFLAGS) $(BROKER_LDFLAGS)
	@echo "built $(BROKER_PRODUCT)"
	$(CROSS_COMPILE)gcc wifi-ps.c -o $(WIFIPS_PRODUCT) -O2 -s -Wall -std=gnu99
	@echo "built $(WIFIPS_PRODUCT)"
ifeq (nextui,$(FIRMWARE))
	# Like the broker: netsetup.c's hosting without the frontend. It reads
	# evdev itself and has Varnish draw, so it links no SDL at all.
	$(CROSS_COMPILE)gcc nextui/quickmenu.c netsetup.c -o $(QUICKMENU_PRODUCT) $(CFLAGS) $(BROKER_LDFLAGS)
	@echo "built $(QUICKMENU_PRODUCT)"
endif

$(SPRUCE_INC)/platform.h: $(PLATDIR)/platform.h
	@mkdir -p $(SPRUCE_INC)
	sed 's|^#define SDCARD_PATH "/mnt/SDCARD"$$|#define SDCARD_PATH "/mnt/SDCARD/App/Netplay/nextui"|' $< > $@
	@grep -q 'App/Netplay/nextui' $@ || { echo "SDCARD_PATH not found in $<"; rm -f $@; exit 1; }

clean:
	rm -f ../bin/*/$(TARGET) ../bin/spruce/*/* ../bin/*/$(BROKER_TARGET) ../bin/*/$(WIFIPS_TARGET) ../bin/*/$(QUICKMENU_TARGET)

.PHONY: all clean
