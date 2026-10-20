###########################################################
# Netplay.pak session-setup app
#
# Needs a NextUI checkout: the UI uses GFX_/PAD_ from common/api.c, which is
# compiled straight into minarch and so unreachable from the shim. We compile
# our own copy, exactly as the original pak's app did. The source and platform
# flags must come from the matching NextUI tree because platform.c is part of
# the app and is not a stable external SDK.
#
#   make PLATFORM=tg5040 NEXTUI=/path/to/NextUI
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050 my282 my355 h700
TARGET  = netplay.elf
BROKER_TARGET = netplay-broker.elf
WIFIPS_TARGET = netplay-wifips.elf
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

SOURCE   = main.c netsetup.c \
           $(COMMON)/api.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/scaler.c \
           $(PLATDIR)/platform.c

# NEXTUI_SDK_PREFIX is set by build-platforms and holds a libmsettings built
# against the pinned source; the in-tree path is the fallback for a plain
# toolchain container. Both are offered so either environment works.
SDKPREFIX ?= $(NEXTUI_SDK_PREFIX)
INCDIR   = -I. -I$(COMMON) -I$(PLATDIR) -I$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
INCDIR  += -I$(SDKPREFIX)/include
endif
CFLAGS  += $(OPT) $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -L$(WS)/$(PLATFORM)/libmsettings
ifneq (,$(SDKPREFIX))
LDFLAGS += -L$(SDKPREFIX)/lib
endif
LDFLAGS += -lmsettings -lsamplerate -lzip

# The detached broker links netsetup.c for discovery/compatibility logic but no
# frontend, audio, settings-library or archive APIs. Keep its idle process truly
# small instead of loading the setup app's entire SDL/GLES dependency graph.
BROKER_LDFLAGS = $(filter -L%,$(LDFLAGS)) -ldl

PRODUCT = ../bin/$(PLATFORM)/$(TARGET)
BROKER_PRODUCT = ../bin/$(PLATFORM)/$(BROKER_TARGET)
WIFIPS_PRODUCT = ../bin/$(PLATFORM)/$(WIFIPS_TARGET)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CROSS_COMPILE)gcc $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"
	$(CROSS_COMPILE)gcc broker.c netsetup.c -o $(BROKER_PRODUCT) $(CFLAGS) $(BROKER_LDFLAGS)
	@echo "built $(BROKER_PRODUCT)"
	$(CROSS_COMPILE)gcc wifi-ps.c -o $(WIFIPS_PRODUCT) -O2 -s -Wall -std=gnu99
	@echo "built $(WIFIPS_PRODUCT)"

clean:
	rm -f ../bin/*/$(TARGET) ../bin/*/$(BROKER_TARGET) ../bin/*/$(WIFIPS_TARGET)

.PHONY: all clean
