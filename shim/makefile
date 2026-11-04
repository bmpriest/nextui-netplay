###########################################################
# netplay shim - libretro core wrapper
#
# Deliberately standalone: no NextUI workspace, no libmsettings, no SDL.
# It depends only on libc and the vendored libretro.h, which is the whole
# point - the shim tracks the libretro ABI, not a NextUI build.
#
#   make PLATFORM=tg5040      cross-build for the device
#   make native               host build, for smoke testing the forwarding
###########################################################

SUPPORTED_PLATFORMS = tg5040 tg5050 my282 my355 h700

TARGET  = netplay_shim.so
SOURCES = shim.c netlink.c raproto.c overlay.c sha256.c romscan.c
INCDIR  = -Iinclude
WARN    = -Wall -Wextra -Wno-unused-parameter

# The device build is the point of this makefile; `native` is only ever asked
# for explicitly. Without this, `make PLATFORM=tg5040` would silently build for
# the host, because native happens to be defined first.
.DEFAULT_GOAL := all

###########################################################
# device build
###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

# Validate for the device build only - `native` and `clean` need none of this.
# MAKECMDGOALS is empty when `all` is reached as the default goal.
DEVICE_BUILD := 1
ifneq (,$(filter $(MAKECMDGOALS),native clean))
DEVICE_BUILD :=
endif

ifeq (1,$(DEVICE_BUILD))
ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. make PLATFORM=tg5040)
endif
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif
ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif
endif

# Per-platform arch flags, mirroring workspace/<PLATFORM>/platform/makefile.env.
# my282 is a 32-bit Cortex-A7, not aarch64, so this cannot be one setting.
# No -flto anywhere: the shim is a thin forwarding layer and LTO buys nothing
# while making symbol export behaviour harder to reason about.
ARCH_tg5040 = -mcpu=cortex-a53
ARCH_tg5050 = -mcpu=cortex-a55
ARCH_my282  = -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
ARCH_my355  = -mcpu=cortex-a55
ARCH_h700   = -mcpu=cortex-a53

CFLAGS  = $(ARCH_$(PLATFORM)) -O2 -fomit-frame-pointer -fPIC
CFLAGS += $(INCDIR) $(WARN) -std=gnu99
# --no-undefined: a symbol declared and called but never defined otherwise
# links cleanly and dies at dlopen time on the device, which is a very
# expensive way to find a typo.
# -lz: the peer's cartridge has to be found inside a zipped library, and the
# one archive that turns out to hold it has to be inflated. Both sysroots ship
# libz and both devices have libz.so.1; include/zlib.h is vendored because the
# my282 sysroot has the library but not the header.
LDFLAGS = -shared -Wl,--no-undefined -ldl -lpthread -lz

PRODUCT = ../bin/$(PLATFORM)/$(TARGET)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CROSS_COMPILE)gcc $(SOURCES) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)
	@echo "built $(PRODUCT)"

###########################################################
# host build - not installable, just proves the C is sound
###########################################################

native:
	@mkdir -p ../bin/native
	$(CC) $(SOURCES) -o ../bin/native/$(TARGET) \
		-shared -Wl,--no-undefined -fPIC $(INCDIR) $(WARN) -O2 -std=gnu99 -ldl -lpthread -lz

clean:
	rm -f $(foreach p,$(SUPPORTED_PLATFORMS),../bin/$(p)/$(TARGET)) ../bin/native/$(TARGET)

.PHONY: all native clean
