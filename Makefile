.PHONY: dist dist-base dist-compatibility dist-full shim cores core-sources compatibility compatibility-sources compatibility-armv7 compatibility-aarch64 check-compatibility check-netplay-cores cores-gambatte cores-gambatte-dual cores-gpsp gblc-pak nds-pak nds-wifi-hooks nds-wifi-paks nds-wifi-a30-pak nds-wifi-brick-pak app test test-gambatte-dual clean help

PAK        := Netplay.pak
BASE_ARCHIVE   := dist/Netplay.pak.zip
COMPAT_ARCHIVE := dist/compatibility-cores.zip
FULL_ARCHIVE   := dist/Netplay-Full.pak.zip
BASE_STAGE     := dist/stage-base
COMPAT_STAGE   := dist/stage-compatibility
FULL_STAGE     := dist/stage-full
PLATFORMS  := tg5040 tg5050 my282 my355 h700

# build-platforms owns the image-digest/source-commit pairing for every target.
# Keep the sibling checkout as the convenient default, but allow CI and other
# workspaces to point at it without reproducing one developer's home directory.
BUILDER    ?= $(abspath $(CURDIR)/../../build-platforms)

help:
	@echo "make shim    cross-build the shim for $(PLATFORMS) (needs docker)"
	@echo "make cores   build the patched gambatte + gpsp cores (needs docker)"
	@echo "make cores-gambatte-dual  build the experimental in-process dual Gambatte core"
	@echo "make gblc-pak  build a standalone my282 GBLC emulator test pak"
	@echo "make nds-pak   build a standalone DraStic NDS pak for my282 (Miyoo A30)"
	@echo "make nds-wifi-paks  build trace-hook NDS paks for A30 and TrimUI Brick"
	@echo "make compatibility  build seven pinned cores for ARMv7 + AArch64"
	@echo "make core-sources  fetch pinned core sources and apply tracked patches"
	@echo "make app     build the session-setup app (needs docker + NEXTUI)"
	@echo "make test    run the host test suites"
	@echo "make dist    create Netplay.pak.zip, compatibility-cores.zip and Netplay-Full.pak.zip"
	@echo "make clean   remove dist/"

###########################################################

shim:
	@for p in $(PLATFORMS); do \
		echo "== shim $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd shim && make PLATFORM='$$p'"' || exit 1; \
	done
	@ls -la bin/*/netplay_shim.so | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

# Cores the pak ships in place of NextUI's.
#
#  gambatte - NextUI builds it without HAVE_NETWORK, so Game Link is compiled
#             out entirely and no configuration can reach it. Plus a serial
#             receive deadline (cores/patches/).
#  gpsp     - the two RFU fixes from the original pak: disconnect handling and
#             a packet queue deep enough for TCP's bursty delivery.
#
# Both are the same upstream sources NextUI uses; only the build differs.
CORE_SRC_ROOT ?= .cache/cores
GAMBATTE_SRC := $(CORE_SRC_ROOT)/gambatte
GAMBATTE_DUAL_SRC := $(CORE_SRC_ROOT)/gambatte-dual
GPSP_SRC     := $(CORE_SRC_ROOT)/gpsp
GAMBATTE_REPO := https://github.com/bmpriest/gambatte-libretro.git
GAMBATTE_REV  := b3803b3dd27a2a7f28ce39a426e1453eee7376c9
GPSP_REPO     := https://github.com/libretro/gpsp.git
GPSP_REV      := 69e86ebe89f14c3f5f75b809c12c0a953b3d6ce4
GPSP_PATCHES := cores/patches/gpsp-platforms.patch cores/patches/gpsp-001-rfu-disconnect.patch cores/patches/gpsp-002-rfu-queue-size.patch cores/patches/gpsp-003-netplay-version.patch
GAMBATTE_STAMP := $(GAMBATTE_SRC)/.netplay-fork-$(GAMBATTE_REV)
GAMBATTE_DUAL_STAMP := $(GAMBATTE_DUAL_SRC)/.netplay-fork-$(GAMBATTE_REV)
GPSP_STAMP := $(GPSP_SRC)/.netplay-patched-$(GPSP_REV)

FCEUMM_SRC := $(CORE_SRC_ROOT)/fceumm
PICODRIVE_SRC := $(CORE_SRC_ROOT)/picodrive
SNES9X_SRC := $(CORE_SRC_ROOT)/snes9x
SUPAFAUST_SRC := $(CORE_SRC_ROOT)/mednafen_supafaust
PCSX_SRC := $(CORE_SRC_ROOT)/pcsx_rearmed
FCEUMM_REV := afe65ef1b4328c0bf2df05fb47a6b31a45bccf47
PICODRIVE_REV := b0be121b7d58d6ee1ee2809974e62893c80a8264
SNES9X_REV := 185488cd83aaf274752a742c94d45561cbecb7af
SUPAFAUST_REV := d6187e5337e6c2646d003db3ab1936727ca75301
PCSX_REV := 050981b

core-sources: $(GAMBATTE_STAMP) $(GPSP_STAMP)

compatibility-sources: core-sources $(FCEUMM_SRC)/.compat-pinned $(PICODRIVE_SRC)/.compat-pinned \
	$(SNES9X_SRC)/.compat-pinned \
	$(SUPAFAUST_SRC)/.compat-pinned $(PCSX_SRC)/.compat-pinned

define compat_source
$(1)/.compat-pinned:
	@rm -rf "$(1)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q $(2) "$(1)"
	@git -C "$(1)" checkout -q $(3)
	@touch "$$@"
endef

$(eval $(call compat_source,$(FCEUMM_SRC),https://github.com/libretro/libretro-fceumm.git,$(FCEUMM_REV)))
$(eval $(call compat_source,$(SNES9X_SRC),https://github.com/libretro/snes9x.git,$(SNES9X_REV)))
$(eval $(call compat_source,$(SUPAFAUST_SRC),https://github.com/libretro/supafaust.git,$(SUPAFAUST_REV)))

$(PICODRIVE_SRC)/.compat-pinned: cores/patches/picodrive-old-arm-hwcap.patch
	@rm -rf "$(PICODRIVE_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q https://github.com/irixxxx/picodrive.git "$(PICODRIVE_SRC)"
	@git -C "$(PICODRIVE_SRC)" checkout -q "$(PICODRIVE_REV)"
	@git -C "$(PICODRIVE_SRC)" submodule update --init --recursive
	@patch -d "$(PICODRIVE_SRC)" -p1 < cores/patches/picodrive-old-arm-hwcap.patch
	@touch "$@"

$(PCSX_SRC)/.compat-pinned: cores/patches/pcsx-rearmed-old-arm-hwcap.patch
	@rm -rf "$(PCSX_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q https://github.com/libretro/pcsx_rearmed.git "$(PCSX_SRC)"
	@git -C "$(PCSX_SRC)" checkout -q "$(PCSX_REV)"
	@git -C "$(PCSX_SRC)" submodule update --init frontend/libpicofe
	@patch -d "$(PCSX_SRC)" -p1 < cores/patches/pcsx-rearmed-old-arm-hwcap.patch
	@touch "$@"

$(GAMBATTE_STAMP):
	@rm -rf "$(GAMBATTE_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q "$(GAMBATTE_REPO)" "$(GAMBATTE_SRC)"
	@git -C "$(GAMBATTE_SRC)" checkout -q "$(GAMBATTE_REV)"
	@touch "$@"

$(GAMBATTE_DUAL_STAMP): $(GAMBATTE_STAMP)
	@rm -rf "$(GAMBATTE_DUAL_SRC)"
	@cp -a "$(GAMBATTE_SRC)" "$(GAMBATTE_DUAL_SRC)"
	@touch "$@"

$(GPSP_STAMP): $(GPSP_PATCHES)
	@rm -rf "$(GPSP_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q "$(GPSP_REPO)" "$(GPSP_SRC)"
	@git -C "$(GPSP_SRC)" checkout -q "$(GPSP_REV)"
	@patch -d "$(GPSP_SRC)" -p1 < cores/patches/gpsp-platforms.patch
	@patch -d "$(GPSP_SRC)" -p1 < cores/patches/gpsp-001-rfu-disconnect.patch
	@patch -d "$(GPSP_SRC)" -p1 < cores/patches/gpsp-002-rfu-queue-size.patch
	@patch -d "$(GPSP_SRC)" -p1 < cores/patches/gpsp-003-netplay-version.patch
	@touch "$@"

# The `platform=` string is not our platform id - it selects a branch inside the
# core's own Makefile, and an unrecognised one can silently fall through to
# Windows. Our tracked patches add tg5040/tg5050/my282; the two newer aarch64
# targets borrow the branch matching their CPU family. Determinism does not enter into it
# - both these cores are link-cable only, where each device runs its own
# console and no state is ever compared.
define core_build
	@for p in $(PLATFORMS); do \
			echo "== $(1) $$p"; \
			mkdir -p dist/cores/$$p; \
			bp=$$p; case $$p in my355) bp=tg5050 ;; h700) bp=tg5040 ;; esac; \
			$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
				CMD="make -C $(2) $(3) platform=$$bp clean >/dev/null 2>&1 && \
				make -C $(2) $(3) platform=$$bp -j4 && \
				cp $(2)/$(1)_libretro.so dist/cores/$$p/" || exit 1; \
		done
endef

cores: core-sources cores-gambatte cores-gambatte-dual cores-gpsp

cores-gambatte:
	@test -d "$(GAMBATTE_SRC)" || { echo "missing $(GAMBATTE_SRC)"; exit 1; }
	$(call core_build,gambatte,$(GAMBATTE_SRC),-f Makefile.libretro HAVE_NETWORK=1)
	@for p in $(PLATFORMS); do \
		printf "   %-7s %s  link=%s\n" "$$p" \
			"$$(strings -n 6 dist/cores/$$p/gambatte_libretro.so | grep -m1 '^v0\.5\.0')" \
			"$$(strings -n 6 dist/cores/$$p/gambatte_libretro.so | grep -c gambatte_gb_link_mode)"; \
	done
	@mkdir -p dist/compatibility/armv7 dist/compatibility/aarch64
	@cp dist/cores/my282/gambatte_libretro.so dist/compatibility/armv7/
	@cp dist/cores/tg5040/gambatte_libretro.so dist/compatibility/aarch64/
	@echo "   (link=0 means HAVE_NETWORK did not take - stock NextUI gambatte reads 0)"

# The dual build is a Netplay implementation core, not a compatibility core.
# It is selected only for same-ROM instanced Gambatte sessions; ordinary link
# play continues to use the network-capable single-console build above.
cores-gambatte-dual: $(GAMBATTE_DUAL_STAMP)
	@for p in $(PLATFORMS); do \
		echo "== gambatte-dual $$p"; \
		mkdir -p dist/cores-experimental/$$p; \
		bp=$$p; case $$p in my355) bp=tg5050 ;; h700) bp=tg5040 ;; esac; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD="make -C $(GAMBATTE_DUAL_SRC) -f Makefile.libretro HAVE_NETWORK=1 NETPLAY_DUAL_INSTANCE=1 platform=$$bp clean >/dev/null 2>&1 && \
				make -C $(GAMBATTE_DUAL_SRC) -f Makefile.libretro HAVE_NETWORK=1 NETPLAY_DUAL_INSTANCE=1 platform=$$bp -j4 && \
				cp $(GAMBATTE_DUAL_SRC)/gambatte_libretro.so dist/cores-experimental/$$p/gambatte_dual_libretro.so" || exit 1; \
	done
	@for p in $(PLATFORMS); do \
		printf "   %-7s %s\n" "$$p" \
			"$$(strings -n 6 dist/cores-experimental/$$p/gambatte_dual_libretro.so | grep -m1 '^v0\.5\.0-netdual')"; \
	done

test-gambatte-dual: $(GAMBATTE_DUAL_STAMP)
	@test -n "$(ROM_A)" -a -n "$(ROM_B)" || { \
		echo "usage: make test-gambatte-dual ROM_A=/path/a.gb ROM_B=/path/b.gb"; exit 2; }
	@$(MAKE) -s -C "$(GAMBATTE_DUAL_SRC)" test-dual-contract ROM_A="$(abspath $(ROM_A))" ROM_B="$(abspath $(ROM_B))"

# Standalone A30 feasibility package. The archive expands directly into the SD
# card root and is deliberately separate from every Netplay release artifact.
GBLC_STAGE := dist/gblc-my282
GBLC_ARCHIVE := dist/GBLC-my282.pak.zip

gblc-pak: $(GAMBATTE_DUAL_STAMP)
	@$(MAKE) -s -C "$(BUILDER)" build PLATFORM=my282 PROJECT="$$PWD" \
		CMD="make -C $(GAMBATTE_DUAL_SRC) -f Makefile.libretro HAVE_NETWORK=1 NETPLAY_DUAL_INSTANCE=1 platform=my282 clean >/dev/null 2>&1 && \
			make -C $(GAMBATTE_DUAL_SRC) -f Makefile.libretro HAVE_NETWORK=1 NETPLAY_DUAL_INSTANCE=1 platform=my282 -j4"
	@rm -rf "$(GBLC_STAGE)"
	@mkdir -p "$(GBLC_STAGE)/Emus/my282/GBLC.pak" \
		"$(GBLC_STAGE)/Roms/Game Boy Link Cable (GBLC)"
	@cp testing/GBLC.pak/launch.sh testing/GBLC.pak/default.cfg \
		testing/GBLC.pak/README.txt "$(GBLC_STAGE)/Emus/my282/GBLC.pak/"
	@cp "$(GAMBATTE_DUAL_SRC)/gambatte_libretro.so" \
		"$(GBLC_STAGE)/Emus/my282/GBLC.pak/gambatte_dual_libretro.so"
	@cp testing/roms/README.txt "$(GBLC_STAGE)/Roms/Game Boy Link Cable (GBLC)/"
	@chmod +x "$(GBLC_STAGE)/Emus/my282/GBLC.pak/launch.sh"
	@rm -f "$(GBLC_ARCHIVE)"
	@cd "$(GBLC_STAGE)" && zip -qr "$(abspath $(GBLC_ARCHIVE))" Emus Roms
	@echo "built $(GBLC_ARCHIVE)"

# Standalone A30 DraStic package. The open compatibility layer comes from the
# current MyMinUI A30 release; DraStic itself comes from the user's local
# steward-fu/nds checkout because it is proprietary and is not committed here.
NDS_STAGE := dist/nds-my282
NDS_ARCHIVE := dist/NDS-my282.pak.zip
NDS_RUNTIME_NAME := MyMinUI-20260805b-0-my282.zip
NDS_RUNTIME_ZIP ?= .cache/nds/$(NDS_RUNTIME_NAME)
NDS_RUNTIME_URL := https://github.com/Turro75/MyMinUI/releases/download/MyMinUI-20260805/$(NDS_RUNTIME_NAME)
NDS_RUNTIME_SHA256 := 98976677bd425596e9a441e8ccc3f135eb476de5f165c3a2ee43cb468d2cff23
NDS_SOURCE ?= $(abspath $(shell git rev-parse --path-format=absolute --git-common-dir)/../../../cores/nds)

$(NDS_RUNTIME_ZIP):
	@mkdir -p "$(dir $@)"
	@curl -fL "$(NDS_RUNTIME_URL)" -o "$@.tmp"
	@printf '%s  %s\n' "$(NDS_RUNTIME_SHA256)" "$@.tmp" | sha256sum -c -
	@mv "$@.tmp" "$@"

nds-pak: $(NDS_RUNTIME_ZIP)
	@test -f "$(NDS_SOURCE)/drastic/drastic32" || { \
		echo "missing $(NDS_SOURCE)/drastic/drastic32" >&2; \
		echo "set NDS_SOURCE=/path/to/steward-fu/nds" >&2; exit 1; }
	@printf '%s  %s\n' "$(NDS_RUNTIME_SHA256)" "$(NDS_RUNTIME_ZIP)" | sha256sum -c -
	@rm -rf "$(NDS_STAGE)" "$(NDS_ARCHIVE)"
	@mkdir -p "$(NDS_STAGE)/Emus/my282/NDS.pak" \
		"$(NDS_STAGE)/Roms/Nintendo DS (NDS)"
	@unzip -q "$(NDS_RUNTIME_ZIP)" 'Emus/my282/NDS.pak/drastic/*' -d "$(NDS_STAGE)/runtime"
	@cp -a "$(NDS_STAGE)/runtime/Emus/my282/NDS.pak/drastic" \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/"
	@rm -rf "$(NDS_STAGE)/runtime"
	@cp "$(NDS_SOURCE)/drastic/drastic32" \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/drastic/drastic"
	@base64 -d testing/NDS.pak/nds.cfg.b64 \
		> "$(NDS_STAGE)/Emus/my282/NDS.pak/drastic/res/nds.cfg"
	@cp testing/NDS.pak/launch.sh testing/NDS.pak/hard-quit.sh \
		testing/NDS.pak/README.txt \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/"
	@cp testing/roms/NDS-README.txt "$(NDS_STAGE)/Roms/Nintendo DS (NDS)/README.txt"
	@chmod 755 "$(NDS_STAGE)/Emus/my282/NDS.pak/launch.sh" \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/hard-quit.sh" \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/drastic/drastic"
	@cd "$(NDS_STAGE)" && zip -qr "$(abspath $(NDS_ARCHIVE))" Emus Roms
	@echo "built $(NDS_ARCHIVE)"

# First multiplayer milestones: architecture-specific Wi-Fi MMIO hooks plus an
# opt-in, independently owned state model. Passthrough remains the default until
# IRQ, emulated-time and radio behavior are implemented and device-tested.
NDS_WIFI_A30_ARCHIVE := dist/NDS-WiFi-a30.pak.zip
NDS_WIFI_BRICK_STAGE := dist/nds-wifi-brick
NDS_WIFI_BRICK_ARCHIVE := dist/NDS-WiFi-brick.pak.zip
NDS_BRICK_SOURCE ?= $(abspath $(shell git rev-parse --path-format=absolute --git-common-dir)/../../../cores/minui-nintendo-ds-pak)

nds-wifi-hooks:
	@docker run --rm -v "$(CURDIR):/src" -w /src/drastic-wifi \
		nextui-my282-toolchain:local make BUILD=build-a30 \
		CC=arm-a30-linux-gnueabihf-gcc \
		CFLAGS='-O2 -g -march=armv7-a -mfpu=neon -mfloat-abi=hard' all
	@docker run --rm -v "$(CURDIR):/src" -w /src/drastic-wifi \
		ghcr.io/loveretro/tg5040-toolchain:latest make BUILD=build-brick \
		CC=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-gcc \
		CFLAGS='-O2 -g -march=armv8-a' all

nds-wifi-a30-pak: nds-pak nds-wifi-hooks
	@cp drastic-wifi/build-a30/libdrastic_wifi.so \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/drastic/"
	@cp testing/NDS.pak/WIFI-README.txt \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/"
	@rm -f "$(NDS_WIFI_A30_ARCHIVE)"
	@cd "$(NDS_STAGE)" && zip -qr "$(abspath $(NDS_WIFI_A30_ARCHIVE))" Emus Roms
	@echo "built $(NDS_WIFI_A30_ARCHIVE)"

nds-wifi-brick-pak: nds-wifi-hooks
	@test -f "$(NDS_BRICK_SOURCE)/drastic/drastic" || { \
		echo "missing Brick DraStic runtime in $(NDS_BRICK_SOURCE)" >&2; exit 1; }
	@$(MAKE) -C "$(NDS_BRICK_SOURCE)" build
	@rm -rf "$(NDS_WIFI_BRICK_STAGE)"
	@mkdir -p "$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak" \
		"$(NDS_WIFI_BRICK_STAGE)/Roms/Nintendo DS (NDS)"
	@cp -a "$(NDS_BRICK_SOURCE)/drastic" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/"
	@cp -a "$(NDS_BRICK_SOURCE)/bin" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/"
	@cp "$(NDS_BRICK_SOURCE)/launch.sh" "$(NDS_BRICK_SOURCE)/pak.json" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/"
	@cp testing/NDS.pak/brick-settings.json \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/drastic/resources/settings.json"
	@cp drastic-wifi/build-brick/libdrastic_wifi.so \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/drastic/libs/"
	@cp testing/NDS.pak/WIFI-README.txt \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/"
	@chmod 755 "$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/launch.sh" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/drastic/drastic" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/drastic/show_hotkeys" \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/bin/"*
	@sed -i 's|LD_PRELOAD=$$EMU_DIR/libs/libadvdrastic.so|DRASTIC_WIFI_MODE="$$(test -f "$$EMU_DIR/wifi_model.enabled" \&\& echo model \|\| echo passthrough)" DRASTIC_WIFI_CYCLE="$$(test -f "$$EMU_DIR/wifi_model.enabled" \&\& echo 1 \|\| echo 0)" DRASTIC_WIFI_IRQ="$$(test -f "$$EMU_DIR/wifi_model.enabled" \&\& echo 1 \|\| echo 0)" DRASTIC_WIFI_SESSION="/mnt/SDCARD/Tools/tg5040/Netplay.pak/state/session" DRASTIC_WIFI_TRACE="/tmp/NDS-wifi-$$(date +%Y%m%d-%H%M%S).txt" LD_PRELOAD="$$EMU_DIR/libs/libdrastic_wifi.so:$$EMU_DIR/libs/libadvdrastic.so"|' \
		"$(NDS_WIFI_BRICK_STAGE)/Emus/tg5040/NDS.pak/launch.sh"
	@cp testing/roms/NDS-README.txt \
		"$(NDS_WIFI_BRICK_STAGE)/Roms/Nintendo DS (NDS)/README.txt"
	@rm -f "$(NDS_WIFI_BRICK_ARCHIVE)"
	@cd "$(NDS_WIFI_BRICK_STAGE)" && zip -qr "$(abspath $(NDS_WIFI_BRICK_ARCHIVE))" Emus Roms
	@echo "built $(NDS_WIFI_BRICK_ARCHIVE)"

nds-wifi-paks: nds-wifi-a30-pak nds-wifi-brick-pak

cores-gpsp:
	@test -d "$(GPSP_SRC)" || { echo "missing $(GPSP_SRC)"; exit 1; }
	$(call core_build,gpsp,$(GPSP_SRC),)
	@for p in $(PLATFORMS); do \
		printf "   %s: rfu queue depth " "$$p"; \
		grep -c RFU_PKT_QUEUE_SIZE $(GPSP_SRC)/rfu.c; \
	done

COMPAT_CORES := fceumm picodrive snes9x mednafen_supafaust pcsx_rearmed gambatte gpsp

compatibility: compatibility-sources cores compatibility-armv7 compatibility-aarch64

compatibility-armv7:
	@$(MAKE) -s -C "$(BUILDER)" build PLATFORM=my282 PROJECT="$$PWD" \
		CMD='COMPAT_START=$(COMPAT_START) sh cores/build-compat.sh armv7'

compatibility-aarch64:
	@$(MAKE) -s -C "$(BUILDER)" build PLATFORM=tg5040 PROJECT="$$PWD" \
		CMD='COMPAT_START=$(COMPAT_START) sh cores/build-compat.sh aarch64'

check-compatibility:
	@for arch in armv7 aarch64; do for c in $(COMPAT_CORES); do \
		test -f dist/compatibility/$$arch/$${c}_libretro.so || { \
			echo "missing compatibility $$arch/$$c - run 'make compatibility' first"; exit 1; }; \
	done; done

# The UI needs GFX_/PAD_ from NextUI's common/api.c, which is compiled into
# minarch and unreachable from the shim - so the app carries its own copy.
NEXTUI ?= ../../NextUI

# NEXTUI=/opt/nextui-src is where build-platforms mounts the pinned tree for the
# platform being built - not a path on this machine.
app:
	@for p in $(PLATFORMS); do \
		echo "== app $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd app && make PLATFORM='$$p' NEXTUI=/opt/nextui-src"' || exit 1; \
	done
	@ls -la bin/*/netplay.elf | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

test:
	@./shim/test/run.sh
	@./shim/test/link.sh
	@./shim/test/dual.sh
	@./shim/test/romscan.sh
	@./cores/tests/run.sh
	@./launcher/test.sh

###########################################################
# Test build.
#
# Built from an allowlist, not by excluding things from the repo. The repo
# carries ~16MB of patched per-NextUI-build binaries, a NextUI source overlay
# and the shim sources, none of which belong on the device - and an exclusion
# list silently ships whatever it forgets.
#
# Note this stages launcher/pak-launch.sh as launch.sh rather than the pak's own
# launch.sh. Both run bin/$PLATFORM/netplay.elf; pak-launch.sh is the one that
# sets SDCARD_PATH/SYSTEM_PATH/LOGS_PATH explicitly and creates the log dir, so
# it works when the frontend has not exported them.
#
# (An older comment here said netplay.elf was excluded from this build. That
# referred to a different, long-removed netplay.elf which installed patched
# system binaries - the thing the shim architecture replaced. The current app is
# shipped and is the pak's whole UI.)
###########################################################

BASE_DIST_CHECKS := $(foreach p,$(PLATFORMS),check-shim-$(p) check-app-$(p)) check-netplay-cores

dist: dist-base dist-compatibility dist-full
	@echo
	@du -h "$(BASE_ARCHIVE)" "$(COMPAT_ARCHIVE)" "$(FULL_ARCHIVE)"

dist-base: $(BASE_DIST_CHECKS)
	@rm -rf "$(BASE_STAGE)" "$(BASE_ARCHIVE)"
	@mkdir -p "$(BASE_STAGE)/$(PAK)/launcher" "$(BASE_STAGE)/$(PAK)/state" dist

	@cp pak.json "$(BASE_STAGE)/$(PAK)/"
	@cp launcher/pak-launch.sh "$(BASE_STAGE)/$(PAK)/launch.sh"
	@cp launcher/minarch.elf launcher/launch-stub.sh launcher/adhoc-join.sh launcher/wifi-watchdog.sh \
	    launcher/install-stubs.sh launcher/wrap-pak.sh launcher/bind-mount.sh launcher/pre-launch.sh \
	    launcher/mount-common.sh launcher/gameswitcher.sh launcher/gameswitcher-launch.sh \
	    "$(BASE_STAGE)/$(PAK)/launcher/"
	@cp launcher/session.conf.example "$(BASE_STAGE)/$(PAK)/"
	@mkdir -p "$(BASE_STAGE)/$(PAK)/cores/override"
	@for p in $(PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/cores/override/$$p"; \
		cp dist/cores/$$p/gambatte_libretro.so dist/cores/$$p/gpsp_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/override/$$p/"; \
		cp dist/cores-experimental/$$p/gambatte_dual_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/override/$$p/"; \
	done
	@for p in $(PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/bin/$$p"; \
		cp bin/$$p/netplay_shim.so "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		done
	@chmod 755 "$(BASE_STAGE)/$(PAK)/launch.sh" "$(BASE_STAGE)/$(PAK)/launcher"/*
	@find "$(BASE_STAGE)" -name '.DS_Store' -delete
	@cd "$(BASE_STAGE)" && zip -q -r "../Netplay.pak.zip" "$(PAK)" -x '*/.*'

dist-compatibility: check-compatibility
	@rm -rf "$(COMPAT_STAGE)" "$(COMPAT_ARCHIVE)"
	@mkdir -p "$(COMPAT_STAGE)/$(PAK)/cores/compatibility"
	@cp -R dist/compatibility/armv7 dist/compatibility/aarch64 \
		"$(COMPAT_STAGE)/$(PAK)/cores/compatibility/"
	@cp cores/compatibility-platforms.txt "$(COMPAT_STAGE)/$(PAK)/cores/compatibility/PLATFORMS.txt"
	@cd "$(COMPAT_STAGE)" && zip -q -r "../compatibility-cores.zip" "$(PAK)" -x '*/.*'

dist-full: dist-base check-compatibility
	@rm -rf "$(FULL_STAGE)" "$(FULL_ARCHIVE)"
	@cp -a "$(BASE_STAGE)" "$(FULL_STAGE)"
	@mkdir -p "$(FULL_STAGE)/$(PAK)/cores/compatibility"
	@cp -R dist/compatibility/armv7 dist/compatibility/aarch64 \
		"$(FULL_STAGE)/$(PAK)/cores/compatibility/"
	@cp cores/compatibility-platforms.txt "$(FULL_STAGE)/$(PAK)/cores/compatibility/PLATFORMS.txt"
	@cd "$(FULL_STAGE)" && zip -q -r "../Netplay-Full.pak.zip" "$(PAK)" -x '*/.*'

check-netplay-cores:
	@for p in $(PLATFORMS); do for c in gambatte gpsp; do \
		test -f dist/cores/$$p/$${c}_libretro.so || { \
			echo "missing netplay core $$p/$$c - run 'make cores' first"; exit 1; }; \
	done; \
	test -f dist/cores-experimental/$$p/gambatte_dual_libretro.so || { \
		echo "missing netplay core $$p/gambatte_dual - run 'make cores' first"; exit 1; }; \
	done

check-shim-%:
	@test -f bin/$*/netplay_shim.so || { \
		echo "missing bin/$*/netplay_shim.so - run 'make shim' first"; exit 1; }

check-app-%:
	@test -f bin/$*/netplay.elf || { \
		echo "missing bin/$*/netplay.elf - run 'make app' first"; exit 1; }

clean:
	rm -rf dist
