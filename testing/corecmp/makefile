# corecmp for a device: run inside a NextUI toolchain container, as `make
# corecmp` in the top-level Makefile does.
#
#   make PLATFORM=tg5040     -> ../../build/corecmp/<PLATFORM>/corecmp

ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE - run inside the NextUI toolchain container)
endif

OUT = ../../build/corecmp/$(PLATFORM)

all:
	@mkdir -p $(OUT)
	$(CROSS_COMPILE)gcc -O2 -Wall -Wextra -std=gnu99 -I../../shim/include corecmp.c -o $(OUT)/corecmp -ldl
