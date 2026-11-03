.PHONY: corecmp app-spruce dist-spruce udhcpd check-udhcpd cores-dsp check-dsp cores-nds check-nds drastic-radio dist dist-base dist-compatibility dist-full shim cores core-sources compatibility compatibility-sources compatibility-armv7 compatibility-aarch64 check-compatibility check-netplay-cores cores-gambatte cores-gambatte-dual cores-gpsp cores-mgba cores-mgba-dual mgba-pak gblc-pak app test test-gambatte-dual clean help nds-pak nds-wifi-hooks nds-wifi-paks nds-wifi-a30-pak varnish check-varnish

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
# From the main checkout that is sbcs/build-platforms; from a worktree under
# .worktrees/<name>/ it sits two levels further up. Prefer whichever exists so
# the same command works in both, and fall back to the plain guess so a missing
# builder still reports the path it expected.
BUILDER_PATHS := $(abspath $(CURDIR)/../../build-platforms) $(abspath $(CURDIR)/../../../build-platforms)
BUILDER    ?= $(firstword $(wildcard $(BUILDER_PATHS)) $(firstword $(BUILDER_PATHS)))

# Varnish.pak, which draws the status icon and the quick menu, is bundled so the
# app can install it when either is switched on. Found the same way as the
# builder: a community-paks checkout beside my-paks.
VARNISH_PATHS := $(abspath $(CURDIR)/../../community-paks/nextui-varnish-pak) $(abspath $(CURDIR)/../../../community-paks/nextui-varnish-pak)
VARNISH    ?= $(firstword $(wildcard $(VARNISH_PATHS)) $(firstword $(VARNISH_PATHS)))
VARNISH_PLATFORMS := tg5040 tg5050 my355 h700

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
	@echo "make app-spruce  build the app for spruce ($(SPRUCE_PLATFORMS))"
	@echo "make dist-spruce  create Netplay-spruce.zip (App/Netplay)"
	@echo "make cores-dsp  build DSP.pak (DSperate, NDS local wireless) for $(DSP_PLATFORMS)"
	@echo "make cores-nds  build the NDS.pak (DraStic) local wireless overlay for $(NDS_PLATFORMS)"
	@echo "make varnish build the bundled Varnish.pak for $(VARNISH_PLATFORMS) (needs docker)"
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
GPSP_REV      := 5819380c2ffb0900219d700a382ee68c464ebb99
GPSP_PATCHES := cores/patches/gpsp-platforms.patch cores/patches/gpsp-001-rfu-disconnect.patch cores/patches/gpsp-002-rfu-queue-size.patch cores/patches/gpsp-003-netplay-version.patch
GAMBATTE_STAMP := $(GAMBATTE_SRC)/.netplay-fork-$(GAMBATTE_REV)
GAMBATTE_DUAL_STAMP := $(GAMBATTE_DUAL_SRC)/.netplay-fork-$(GAMBATTE_REV)
GPSP_STAMP := $(GPSP_SRC)/.netplay-patched-$(GPSP_REV)

#  mgba - carries a working lockstep cable in core code, but the libretro build
#         does not compile the two SIO drivers that use it. We add them; that is
#         the whole of this change for the single core. Pinned to the revision
#         NextUI 6.15.0 ships (0.11-1-7a12d6d, read from its cores) and built the
#         way it builds it: CMake, with its per-CPU flags (cores/build-mgba.sh).
MGBA_SRC  := $(CORE_SRC_ROOT)/mgba
MGBA_REPO := https://github.com/libretro/mgba
MGBA_REV  := 7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
MGBA_DUAL_FRONTEND := cores/mgba/libretro_dual.c
MGBA_PATCHES := cores/patches/mgba-001-build-lockstep-drivers.patch cores/patches/mgba-002-dual-frontend.patch
MGBA_STAMP := $(MGBA_SRC)/.netplay-patched-$(MGBA_REV)

FCEUMM_SRC := $(CORE_SRC_ROOT)/fceumm
PICODRIVE_SRC := $(CORE_SRC_ROOT)/picodrive
SNES9X_SRC := $(CORE_SRC_ROOT)/snes9x
SUPAFAUST_SRC := $(CORE_SRC_ROOT)/mednafen_supafaust
PCSX_SRC := $(CORE_SRC_ROOT)/pcsx_rearmed
# Compatibility cores we do not patch are pinned to the revisions spruce ships
# (prebuilt in the spruceOS repo since 2025-05-31, updated only on demand), so a
# compatibility session plays the same game spruce's own RetroArch would.
FCEUMM_REV := 3544ff567ecc417c170641587083b976739ef9db
PICODRIVE_REV := 7102a71ba632915563529d504469094792f777a8
SNES9X_REV := 49f484569ff2aec7ff08e7598a97d6c9e6eae72d
SUPAFAUST_REV := e25f66765938d33f9ad5850e8d6cd597e55b7299
PCSX_REV := febf2246848efb8937ab24c562bba20107bb46f0
# The cores put `git describe`/`rev-parse --short` into library_version (and
# pcsx into its state header). spruce's come from libretro's buildbot: a clone
# with no tags and 7-digit hashes. Match it, since RetroArch netplay compares
# version strings exactly.
COMPAT_GIT_LIKE_BUILDBOT = git -C "$(1)" config core.abbrev 7 && \
	git -C "$(1)" tag -l | xargs -r git -C "$(1)" tag -d >/dev/null

core-sources: $(GAMBATTE_STAMP) $(GPSP_STAMP)

compatibility-sources: core-sources $(FCEUMM_SRC)/.compat-pinned-$(FCEUMM_REV) \
	$(PICODRIVE_SRC)/.compat-pinned-$(PICODRIVE_REV) $(SNES9X_SRC)/.compat-pinned-$(SNES9X_REV) \
	$(SUPAFAUST_SRC)/.compat-pinned-$(SUPAFAUST_REV) $(PCSX_SRC)/.compat-pinned-$(PCSX_REV)

define compat_source
$(1)/.compat-pinned-$(3):
	@rm -rf "$(1)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q $(2) "$(1)"
	@git -C "$(1)" checkout -q $(3)
	@$(call COMPAT_GIT_LIKE_BUILDBOT,$(1))
	@touch "$$@"
endef

$(eval $(call compat_source,$(FCEUMM_SRC),https://github.com/libretro/libretro-fceumm.git,$(FCEUMM_REV)))
$(eval $(call compat_source,$(SNES9X_SRC),https://github.com/libretro/snes9x.git,$(SNES9X_REV)))
$(eval $(call compat_source,$(SUPAFAUST_SRC),https://github.com/libretro/supafaust.git,$(SUPAFAUST_REV)))

$(PICODRIVE_SRC)/.compat-pinned-$(PICODRIVE_REV): cores/patches/picodrive-old-arm-hwcap.patch
	@rm -rf "$(PICODRIVE_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q https://github.com/irixxxx/picodrive.git "$(PICODRIVE_SRC)"
	@git -C "$(PICODRIVE_SRC)" checkout -q "$(PICODRIVE_REV)"
	@$(call COMPAT_GIT_LIKE_BUILDBOT,$(PICODRIVE_SRC))
	@git -C "$(PICODRIVE_SRC)" submodule update --init --recursive
	@patch -d "$(PICODRIVE_SRC)" -p1 < cores/patches/picodrive-old-arm-hwcap.patch
	@touch "$@"

$(PCSX_SRC)/.compat-pinned-$(PCSX_REV): cores/patches/pcsx-rearmed-old-arm-hwcap.patch
	@rm -rf "$(PCSX_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q https://github.com/libretro/pcsx_rearmed.git "$(PCSX_SRC)"
	@git -C "$(PCSX_SRC)" checkout -q "$(PCSX_REV)"
	@$(call COMPAT_GIT_LIKE_BUILDBOT,$(PCSX_SRC))
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

$(MGBA_STAMP): $(MGBA_PATCHES) $(MGBA_DUAL_FRONTEND) shim/include/gambatte_dual.h
	@rm -rf "$(MGBA_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q "$(MGBA_REPO)" "$(MGBA_SRC)"
	@git -C "$(MGBA_SRC)" checkout -q "$(MGBA_REV)"
	@patch -d "$(MGBA_SRC)" -p1 < cores/patches/mgba-001-build-lockstep-drivers.patch
	@cp "$(MGBA_DUAL_FRONTEND)" "$(MGBA_SRC)/src/platform/libretro/libretro_dual.c"
	@cp "shim/include/gambatte_dual.h" "$(MGBA_SRC)/src/platform/libretro/gambatte_dual.h"
	@patch -d "$(MGBA_SRC)" -p1 < cores/patches/mgba-002-dual-frontend.patch
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

# The platforms Netplay ships MGBA.pak for. cores/build-mgba.sh also knows
# tg5050 and my355 (NextUI 6.15.0's flags), so adding them here is all it takes.
MGBA_PLATFORMS := tg5040 h700 my282

cores-mgba: $(MGBA_STAMP)
	@bad=""; for p in $(PLATFORMS); do \
		case " $(MGBA_PLATFORMS) " in *" $$p "*) ;; *) bad="$$bad $$p" ;; esac; \
	done; \
	if [ -n "$$bad" ]; then \
		echo "cores-mgba: no mGBA platform branch for:$$bad"; \
		echo "  supported: $(MGBA_PLATFORMS)  (override with PLATFORMS=)"; \
		exit 1; \
	fi
	@for p in $(PLATFORMS); do \
		echo "== mgba $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD="sh cores/build-mgba.sh $$p $(MGBA_SRC) dist/cores/$$p/mgba_libretro.so" || exit 1; \
	done

cores-mgba-dual: $(MGBA_STAMP)
	@bad=""; for p in $(PLATFORMS); do \
		case " $(MGBA_PLATFORMS) " in *" $$p "*) ;; *) bad="$$bad $$p" ;; esac; \
	done; \
	if [ -n "$$bad" ]; then \
		echo "cores-mgba-dual: no mGBA platform branch for:$$bad"; \
		echo "  supported: $(MGBA_PLATFORMS)  (override with PLATFORMS=)"; \
		exit 1; \
	fi
	@for p in $(PLATFORMS); do \
		echo "== mgba-dual $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD="sh cores/build-mgba.sh $$p $(MGBA_SRC) dist/cores-experimental/$$p/mgba_dual_libretro.so dual" || exit 1; \
	done
	@for p in $(PLATFORMS); do \
		printf "   %-7s abi=%s scheduler=%s\n" "$$p" \
			"$$(nm -D --defined-only dist/cores-experimental/$$p/mgba_dual_libretro.so 2>/dev/null | grep -c retro_dual_get_abi_version)" \
			"$$(strings dist/cores-experimental/$$p/mgba_dual_libretro.so | grep -c 0.11-1-netdual2)"; \
	done
	@for p in $(PLATFORMS); do \
		printf "   %-7s lockstep symbols: gb=%s gba=%s\n" "$$p" \
			"$$(nm --defined-only dist/cores-experimental/$$p/mgba_dual_libretro.so 2>/dev/null | grep -c 'GBSIOLockstep')" \
			"$$(nm --defined-only dist/cores-experimental/$$p/mgba_dual_libretro.so 2>/dev/null | grep -c 'GBASIOLockstep')"; \
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
NDS_BRICK_SOURCE ?= $(abspath $(shell git rev-parse --path-format=absolute --git-common-dir)/../../../cores/minui-nintendo-ds-pak)

nds-wifi-hooks:
	@docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR):/src" -w /src/cores/drastic-wifi \
		nextui-my282-toolchain:local make BUILD=build-a30 \
		CC=arm-a30-linux-gnueabihf-gcc \
		CFLAGS='-O2 -g -march=armv7-a -mfpu=neon -mfloat-abi=hard' build-a30/libdrastic_wifi_diag.so
	@$(MAKE) --no-print-directory drastic-radio

# The radio for DraStic 2.5.2.0 (AArch64): one library serves every platform
# that runs that build.
DRASTIC_RADIO := cores/drastic-wifi/build-brick/libdrastic_wifi.so
TG5040_TOOLCHAIN := ghcr.io/loveretro/tg5040-toolchain@sha256:f131c6af64029a8723d0ce8d3c2682642f5f091b04714f6beedda9bec18477ab

drastic-radio:
	@docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR):/src" -w /src/cores/drastic-wifi \
		$(TG5040_TOOLCHAIN) make -s BUILD=build-brick \
		CC=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-gcc \
		CXX=/opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-g++ \
		CFLAGS='-O2 -g -march=armv8-a' CXXFLAGS='-O2 -g -march=armv8-a' \
		build-brick/libdrastic_wifi.so
	@docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR):/src" -w /src/cores/drastic-wifi \
		$(TG5040_TOOLCHAIN) /opt/aarch64-nextui-linux-gnu/bin/aarch64-nextui-linux-gnu-strip \
		--strip-unneeded -o build-brick/libdrastic_wifi.stripped.so build-brick/libdrastic_wifi.so

nds-wifi-a30-pak: nds-pak nds-wifi-hooks
	@cp cores/drastic-wifi/build-a30/libdrastic_wifi_diag.so \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/drastic/libdrastic_wifi.so"
	@cp testing/NDS.pak/WIFI-README.txt \
		"$(NDS_STAGE)/Emus/my282/NDS.pak/"
	@rm -f "$(NDS_WIFI_A30_ARCHIVE)"
	@cd "$(NDS_STAGE)" && zip -qr "$(abspath $(NDS_WIFI_A30_ARCHIVE))" Emus Roms
	@echo "built $(NDS_WIFI_A30_ARCHIVE)"

nds-wifi-paks: nds-wifi-a30-pak

# BusyBox udhcpd for the platforms whose firmware has no DHCP server but can
# serve an ad hoc network (the H700 runs its AP through wpa_supplicant). One
# applet, built in the pinned toolchain image (the H700 shares tg5040's
# compiler and sysroot); the tarball is checked against its published SHA-256.
BUSYBOX_VERSION  := 1.36.1
BUSYBOX_SHA256   := b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
BUSYBOX_TARBALL  := $(CORE_SRC_ROOT)/busybox-$(BUSYBOX_VERSION).tar.bz2
UDHCPD_PLATFORMS := h700

$(BUSYBOX_TARBALL):
	@mkdir -p "$(CORE_SRC_ROOT)"
	curl -fsSL -o "$@.part" "https://busybox.net/downloads/busybox-$(BUSYBOX_VERSION).tar.bz2"
	echo "$(BUSYBOX_SHA256)  $@.part" | sha256sum -c - >/dev/null
	mv "$@.part" "$@"

# The toolchain image has no bzip2, so the container gets a plain tar.
$(BUSYBOX_TARBALL:.bz2=): $(BUSYBOX_TARBALL)
	bzip2 -dkc "$<" > "$@"

udhcpd: $(BUSYBOX_TARBALL:.bz2=)
	@for p in $(UDHCPD_PLATFORMS); do \
		mkdir -p bin/$$p; \
		docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR):/src" -w /src \
			$(TG5040_TOOLCHAIN) sh cores/build-udhcpd.sh "$(BUSYBOX_TARBALL:.bz2=)" bin/$$p/udhcpd || exit 1; \
		printf "   %-7s %s\n" "$$p" "$$(du -h bin/$$p/udhcpd | cut -f1)"; \
	done

check-udhcpd:
	@for p in $(UDHCPD_PLATFORMS); do \
		test -x "bin/$$p/udhcpd" || { echo "missing bin/$$p/udhcpd - run 'make udhcpd' first"; exit 1; }; \
	done

# Stage and package the measurement pak for one platform, laid out as it sits on
# the card so the zip is self-describing: unpack it at the SD root and the pak
# and its ROM folder land where they belong. Same shape as gblc-pak.
#
#   make mgba-pak MGBA_PAK_PLATFORM=tg5040   -> dist/MGBA-tg5040.pak.zip
#   make mgba-pak MGBA_PAK_PLATFORM=my282    -> dist/MGBA-my282.pak.zip
MGBA_PAK_PLATFORM ?= tg5040
MGBA_PAK_STAGE   := dist/mgba-$(MGBA_PAK_PLATFORM)
MGBA_PAK_ARCHIVE := dist/MGBA-$(MGBA_PAK_PLATFORM).pak.zip
MGBA_PAK_DIR     := $(MGBA_PAK_STAGE)/Emus/$(MGBA_PAK_PLATFORM)/MGBA.pak
MGBA_ROM_DIR     := $(MGBA_PAK_STAGE)/Roms/Game Boy Advance (MGBA)

mgba-pak:
	@$(MAKE) --no-print-directory cores-mgba PLATFORMS=$(MGBA_PAK_PLATFORM)
	@rm -rf "$(MGBA_PAK_STAGE)"
	@mkdir -p "$(MGBA_PAK_DIR)" "$(MGBA_ROM_DIR)"
	@cp testing/MGBA.pak/launch.sh testing/MGBA.pak/default.cfg \
		testing/MGBA.pak/README.txt "$(MGBA_PAK_DIR)/"
	@cp dist/cores/$(MGBA_PAK_PLATFORM)/mgba_libretro.so "$(MGBA_PAK_DIR)/"
	@cp testing/roms/README-mgba.txt "$(MGBA_ROM_DIR)/README.txt"
	@chmod +x "$(MGBA_PAK_DIR)"/*.sh
	@rm -f "$(MGBA_PAK_ARCHIVE)"
	@cd "$(MGBA_PAK_STAGE)" && zip -qr "$(abspath $(MGBA_PAK_ARCHIVE))" Emus Roms
	@echo "built $(MGBA_PAK_ARCHIVE)"

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
# DSperate (DSP.pak) - a standalone NDS emulator, not a libretro core. Its own
# build lives in cores/dsperate/ (toolchain images, patches, launcher); this stages
# one DSP.pak per platform, holding only that platform's binaries, the way
# MGBA.pak is shipped. launcher/nextui/pak-manage.sh installs it on request.
DSP_PLATFORMS := tg5040 tg5050 h700
DSP_STAGE     := dist/dsp
DSPERATE_REPO ?= https://github.com/beebono/DSperate

cores-dsp:
	@for p in $(DSP_PLATFORMS); do \
		echo "== dsp $$p"; \
		$(MAKE) -s -C cores/dsperate dist-$$p DSPERATE_REPO="$(DSPERATE_REPO)" >/dev/null || exit 1; \
		rm -rf "$(DSP_STAGE)/$$p/DSP.pak"; mkdir -p "$(DSP_STAGE)/$$p/DSP.pak"; \
		( cd cores/dsperate/dist/DSP.pak && for f in *; do \
			case " $(DSP_PLATFORMS) " in *" $$f "*) [ "$$f" = "$$p" ] || continue ;; esac; \
			cp -R "$$f" "$(CURDIR)/$(DSP_STAGE)/$$p/DSP.pak/"; \
		done ); \
		printf "   %-7s %s\n" "$$p" "$$(du -sh $(DSP_STAGE)/$$p/DSP.pak | cut -f1)"; \
	done

check-dsp:
	@for p in $(DSP_PLATFORMS); do \
		test -x "$(DSP_STAGE)/$$p/DSP.pak/$$p/dsperate" || { echo "missing DSP.pak $$p - run 'make cores-dsp' first"; exit 1; }; \
	done

# DraStic local wireless - not a pak but an overlay for the user's own NDS.pak
# (launcher/nextui/pak-manage.sh nds): the radio library and a session-aware
# launch.sh, plus the hash of the DraStic build the radio hooks (2.5.2.0, as
# minui-nintendo-ds-pak ships it). DraStic itself is never copied.
NDS_PLATFORMS  := tg5040 tg5050 h700
NDS_OVERLAY    := dist/nds
DRASTIC_SHA256 := 6e11511907f2188863f68434484d8e1fb70c6c4b597512bed02d38f60370ac11
# For a card with no NDS.pak, Netplay installs minui-nintendo-ds-pak's own
# release, unmodified, then adds the overlay. Its DraStic is DRASTIC_SHA256.
NDS_BASE_URL    := https://github.com/josegonzalez/minui-nintendo-ds-pak/releases/download/0.14.1/NDS.pak.zip
NDS_BASE_ZIP    := .cache/nds/NDS-0.14.1.pak.zip
NDS_BASE_SHA256 := ac4c957592e0856cb4c559cb8a9c29795baec961a789d2e4edcf26cbf06a9a65

$(NDS_BASE_ZIP):
	@mkdir -p $(dir $@)
	@curl -fL "$(NDS_BASE_URL)" -o "$@.tmp"
	@printf '%s  %s\n' "$(NDS_BASE_SHA256)" "$@.tmp" | sha256sum -c -
	@mv "$@.tmp" "$@"

cores-nds: drastic-radio $(NDS_BASE_ZIP)
	@printf '%s  %s\n' "$(NDS_BASE_SHA256)" "$(NDS_BASE_ZIP)" | sha256sum -c - >/dev/null
	@rm -rf "$(NDS_OVERLAY)/base" && mkdir -p "$(NDS_OVERLAY)/base/NDS.pak"
	@unzip -q "$(NDS_BASE_ZIP)" -d "$(NDS_OVERLAY)/base/NDS.pak"
	@test "$$(sha256sum $(NDS_OVERLAY)/base/NDS.pak/drastic/drastic | cut -d' ' -f1)" = "$(DRASTIC_SHA256)" || { \
		echo "NDS.pak release carries a different DraStic" >&2; exit 1; }
	@printf "   %-7s %s\n" base "$$(du -sh $(NDS_OVERLAY)/base | cut -f1)"
	@for p in $(NDS_PLATFORMS); do \
		rm -rf "$(NDS_OVERLAY)/$$p"; \
		mkdir -p "$(NDS_OVERLAY)/$$p/NDS.pak/drastic/libs"; \
		cp $(DRASTIC_RADIO:.so=.stripped.so) "$(NDS_OVERLAY)/$$p/NDS.pak/drastic/libs/libdrastic_wifi.so"; \
		cp cores/drastic-wifi/pak/launch.sh "$(NDS_OVERLAY)/$$p/NDS.pak/"; \
		chmod 755 "$(NDS_OVERLAY)/$$p/NDS.pak/launch.sh"; \
		echo "$(DRASTIC_SHA256)" > "$(NDS_OVERLAY)/$$p/drastic.sha256"; \
		printf "   %-7s %s\n" "$$p" "$$(du -sh $(NDS_OVERLAY)/$$p | cut -f1)"; \
	done

check-nds:
	@for p in $(NDS_PLATFORMS); do \
		test -f "$(NDS_OVERLAY)/$$p/NDS.pak/drastic/libs/libdrastic_wifi.so" || { echo "missing NDS overlay $$p - run 'make cores-nds' first"; exit 1; }; \
	done
	@test -f "$(NDS_OVERLAY)/base/NDS.pak/drastic/drastic" || { echo "missing bundled NDS.pak - run 'make cores-nds' first"; exit 1; }

app:
	@for p in $(PLATFORMS); do \
		echo "== app $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd app && make PLATFORM='$$p' NEXTUI=/opt/nextui-src"' || exit 1; \
	done
	@ls -la bin/*/netplay.elf bin/*/netplay-broker.elf bin/*/netplay-wifips.elf bin/*/netplay-quickmenu.elf | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

# testing/corecmp: the per-frame trace harness for comparing two builds of a
# core (usage at the top of testing/corecmp/corecmp.c and run-pair.sh).
corecmp:
	@for p in tg5040 h700; do \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd testing/corecmp && make PLATFORM='$$p'"' || exit 1; \
	done

###########################################################
# spruce: the same app, shim and cores as App/Netplay.
#
# Built per NextUI platform, since spruce runs on the same hardware: tg5040 for
# the TrimUI Brick and Smart Pro, h700 for the Anbernic H700 family. NextUI's UI
# code draws the app, so it needs NextUI's fonts and images beside it, and the
# libraries the app links that spruce's base images lack: libsamplerate and
# libzip everywhere, and on the H700 SDL2 itself. All come from the NextUI
# release the app is built against.
###########################################################

SPRUCE_PLATFORMS := tg5040 h700
SPRUCE_STAGE     := dist/stage-spruce
SPRUCE_ARCHIVE   := dist/Netplay-spruce.zip
NEXTUI_BASE_URL    := https://github.com/LoveRetro/NextUI/releases/download/v6.15.0/NextUI-20261013-0-base.zip
NEXTUI_BASE_SHA256 := d660d064a800bb7019b3f1f0c9d7254fdc33e92cf478e6cc9b480d6788be8f2e
NEXTUI_BASE_ZIP    := .cache/NextUI-20261013-0-base.zip
NEXTUI_SYSTEM      := dist/nextui-v6.15.0/.system
SPRUCE_LIBS_tg5040 := libsamplerate.so.0 libzip.so.5
SPRUCE_LIBS_h700   := libsamplerate.so.0 libzip.so.5 libcrypto.so.1.1 \
                      libSDL2-2.0.so.0 libSDL2_image-2.0.so.0 libSDL2_ttf-2.0.so.0

$(NEXTUI_BASE_ZIP):
	@mkdir -p .cache
	@curl -fsSL -o "$@.tmp" "$(NEXTUI_BASE_URL)"
	@printf '%s  %s\n' "$(NEXTUI_BASE_SHA256)" "$@.tmp" | sha256sum -c - >/dev/null
	@mv "$@.tmp" "$@"

$(NEXTUI_SYSTEM)/res/assets@2x.png: $(NEXTUI_BASE_ZIP)
	@printf '%s  %s\n' "$(NEXTUI_BASE_SHA256)" "$<" | sha256sum -c - >/dev/null
	@rm -rf dist/nextui-v6.15.0 && mkdir -p dist/nextui-v6.15.0
	@unzip -q -p "$<" MinUI.zip > dist/nextui-v6.15.0/MinUI.zip
	@cd dist/nextui-v6.15.0 && unzip -q MinUI.zip '.system/res/*' \
		$(foreach p,$(SPRUCE_PLATFORMS),$(foreach l,$(SPRUCE_LIBS_$(p)),'.system/$(p)/lib/$(l)')) && rm MinUI.zip
	@touch "$@"

app-spruce:
	@for p in $(SPRUCE_PLATFORMS); do \
		echo "== app spruce $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd app && make PLATFORM='$$p' FIRMWARE=spruce NEXTUI=/opt/nextui-src"' || exit 1; \
	done
	@ls -la bin/spruce/*/netplay*.elf | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

dist-spruce: $(foreach p,$(SPRUCE_PLATFORMS),check-shim-$(p) check-spruce-app-$(p)) check-netplay-cores \
             $(NEXTUI_SYSTEM)/res/assets@2x.png
	@rm -rf "$(SPRUCE_STAGE)" "$(SPRUCE_ARCHIVE)"
	@D="$(SPRUCE_STAGE)/App/Netplay"; mkdir -p "$$D/nextui/.system" && \
	$(foreach p,$(SPRUCE_PLATFORMS),libs_$(p)="$(SPRUCE_LIBS_$(p))";) \
	launcher/flatten.sh spruce "$$D/launcher" && \
	mv "$$D/launcher/launch.sh" "$$D/launcher/config.json" "$$D/" && \
	rm -f "$$D/launcher/session.conf.example" && \
	cp launcher/common/session.conf.example pak_hints.json "$$D/" && \
	cp res/spruce-netplay.png "$$D/netplay.png" && \
	cp -R "$(NEXTUI_SYSTEM)/res" "$$D/nextui/.system/res" && \
	for p in $(SPRUCE_PLATFORMS); do \
		mkdir -p "$$D/bin/$$p" "$$D/cores/override/$$p" "$$D/nextui/.system/$$p/bin" && \
		cp app/spruce/governor.sh "$$D/nextui/.system/$$p/bin/" && \
		cp bin/$$p/netplay_shim.so bin/spruce/$$p/netplay.elf bin/spruce/$$p/netplay-broker.elf \
		   bin/spruce/$$p/netplay-wifips.elf "$$D/bin/$$p/" && \
		eval libs=\$$libs_$$p; \
		for l in $$libs; do cp -L "$(NEXTUI_SYSTEM)/$$p/lib/$$l" "$$D/bin/$$p/" || exit 1; done && \
		cp dist/cores/$$p/gambatte_libretro.so dist/cores/$$p/gpsp_libretro.so \
		   dist/cores-experimental/$$p/gambatte_dual_libretro.so "$$D/cores/override/$$p/" || exit 1; \
	done && \
	if [ -d dist/compatibility/aarch64 ]; then \
		mkdir -p "$$D/cores/compatibility" && \
		cp -R dist/compatibility/aarch64 "$$D/cores/compatibility/" && \
		cp cores/compatibility-platforms.txt "$$D/cores/compatibility/PLATFORMS.txt"; \
	fi && \
	chmod 755 "$$D/launch.sh" "$$D/launcher"/* "$$D"/bin/*/*
	@cd "$(SPRUCE_STAGE)" && zip -q -r "../Netplay-spruce.zip" App -x '*/.*'
	@du -h "$(SPRUCE_ARCHIVE)"

check-spruce-app-%:
	@test -f bin/spruce/$*/netplay.elf || { \
		echo "missing bin/spruce/$*/netplay.elf - run 'make app-spruce' first"; exit 1; }

test:
	@./shim/test/run.sh
	@./shim/test/link.sh
	@./shim/test/dual.sh
	@./shim/test/romscan.sh
	@./cores/tests/run.sh
	@./cores/tests/mgbalockstep.sh
	@./cores/tests/mgbadual.sh
	@./testing/test-mgba-launch.sh
	@./testing/test-mgba-manage.sh
	@./testing/test-dsp-manage.sh
	@./testing/test-nds-manage.sh
	@./testing/test-hints.sh
	@./testing/test-instanced-ui.sh
	@./testing/test-wifi-platform.sh
	@./testing/test-log-rotation.sh
	@./testing/test-wifi-watchdog.sh
	@./testing/test-status-icon.sh
	@./testing/test-quick-menu.sh
	@./testing/test-varnish-install.sh
	@./launcher/test.sh
	@PYTHONDONTWRITEBYTECODE=1 python3 tools/test-netplay-harness.py

###########################################################
# Test build.
#
# Built from an allowlist, not by excluding things from the repo. The repo
# carries ~16MB of patched per-NextUI-build binaries, a NextUI source overlay
# and the shim sources, none of which belong on the device - and an exclusion
# list silently ships whatever it forgets.
#
# Note this stages launcher/nextui/pak-launch.sh as launch.sh rather than the pak's own
# launch.sh. Both run bin/$PLATFORM/netplay.elf; pak-launch.sh is the one that
# sets SDCARD_PATH/SYSTEM_PATH/LOGS_PATH explicitly and creates the log dir, so
# it works when the frontend has not exported them.
#
# (An older comment here said netplay.elf was excluded from this build. That
# referred to a different, long-removed netplay.elf which installed patched
# system binaries - the thing the shim architecture replaced. The current app is
# shipped and is the pak's whole UI.)
###########################################################

BASE_DIST_CHECKS := $(foreach p,$(PLATFORMS),check-shim-$(p) check-app-$(p)) check-netplay-cores check-dsp check-nds check-varnish check-udhcpd

dist: dist-base dist-compatibility dist-full
	@echo
	@du -h "$(BASE_ARCHIVE)" "$(COMPAT_ARCHIVE)" "$(FULL_ARCHIVE)"

dist-base: $(BASE_DIST_CHECKS)
	@rm -rf "$(BASE_STAGE)" "$(BASE_ARCHIVE)"
	@mkdir -p "$(BASE_STAGE)/$(PAK)/launcher" "$(BASE_STAGE)/$(PAK)/state" dist

	@cp pak.json pak_hints.json "$(BASE_STAGE)/$(PAK)/"
	@cp launcher/nextui/pak-launch.sh "$(BASE_STAGE)/$(PAK)/launch.sh"
	@launcher/flatten.sh nextui "$(BASE_STAGE)/$(PAK)/launcher"
	@rm -f "$(BASE_STAGE)/$(PAK)/launcher/pak-launch.sh" "$(BASE_STAGE)/$(PAK)/launcher/session.conf.example"
	@for p in $(VARNISH_PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/varnish"; \
		cp -R dist/varnish/$$p "$(BASE_STAGE)/$(PAK)/varnish/$$p"; \
	done
	@mkdir -p "$(BASE_STAGE)/$(PAK)/res"
	@cp res/status@1x.png res/status@2x.png res/status@3x.png res/status@4x.png \
	    "$(BASE_STAGE)/$(PAK)/res/"
	@cp launcher/common/session.conf.example "$(BASE_STAGE)/$(PAK)/"
	@mkdir -p "$(BASE_STAGE)/$(PAK)/cores/override"
	@for p in $(PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/cores/override/$$p"; \
		cp dist/cores/$$p/gambatte_libretro.so dist/cores/$$p/gpsp_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/override/$$p/"; \
		cp dist/cores-experimental/$$p/gambatte_dual_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/override/$$p/"; \
	done
	@for p in $(MGBA_PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/cores/mgba/$$p/MGBA.pak" \
		             "$(BASE_STAGE)/$(PAK)/cores/override/$$p"; \
		cp testing/MGBA.pak/launch.sh testing/MGBA.pak/default.cfg testing/MGBA.pak/README.txt \
		   "$(BASE_STAGE)/$(PAK)/cores/mgba/$$p/MGBA.pak/"; \
		cp dist/cores/$$p/mgba_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/mgba/$$p/MGBA.pak/"; \
		cp dist/cores-experimental/$$p/mgba_dual_libretro.so \
		   "$(BASE_STAGE)/$(PAK)/cores/override/$$p/"; \
	done
	@for p in $(DSP_PLATFORMS); do \
		case " $(PLATFORMS) " in *" $$p "*) ;; *) continue ;; esac; \
		mkdir -p "$(BASE_STAGE)/$(PAK)/cores/dsp/$$p"; \
		cp -R "$(DSP_STAGE)/$$p/DSP.pak" "$(BASE_STAGE)/$(PAK)/cores/dsp/$$p/"; \
	done
	@for p in $(NDS_PLATFORMS); do \
		case " $(PLATFORMS) " in *" $$p "*) ;; *) continue ;; esac; \
		mkdir -p "$(BASE_STAGE)/$(PAK)/cores/nds"; \
		cp -R "$(NDS_OVERLAY)/$$p" "$(BASE_STAGE)/$(PAK)/cores/nds/"; \
	done
	@cp -R "$(NDS_OVERLAY)/base" "$(BASE_STAGE)/$(PAK)/cores/nds/"
	@for p in $(PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/bin/$$p"; \
		cp bin/$$p/netplay_shim.so "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-broker.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-wifips.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-quickmenu.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		done
	@for p in $(UDHCPD_PLATFORMS); do \
		cp bin/$$p/udhcpd "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
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
	@for p in $(MGBA_PLATFORMS); do \
		test -f dist/cores/$$p/mgba_libretro.so || { echo "missing mGBA core $$p"; exit 1; }; \
		test -f dist/cores-experimental/$$p/mgba_dual_libretro.so || { echo "missing mGBA dual core $$p"; exit 1; }; \
	done

# Varnish's overlay alone, without its ffmpeg: recording is Varnish's own
# feature and the full Varnish.pak brings it. Built with the same pinned
# toolchain images as everything else here.
varnish:
	@test -f "$(VARNISH)/ports/tg5040/Makefile" || { \
		echo "Varnish source not found at $(VARNISH) - set VARNISH=/path/to/nextui-varnish-pak"; exit 1; }
	@for p in $(VARNISH_PLATFORMS); do \
		echo "== varnish $$p"; \
		img=$$(bash -c '. "$(BUILDER)/platforms.conf" && platform_image "$$1"' _ $$p) || exit 1; \
		tc=$$(echo $$p | tr a-z A-Z)_TOOLCHAIN; \
		$(MAKE) -s -C "$(VARNISH)" $$p $$tc="$$img" >/dev/null || exit 1; \
		out=dist/varnish/$$p; rm -rf $$out; mkdir -p $$out/include $$out/scripts; \
		cp "$(VARNISH)/build/$$p/varnish" "$(VARNISH)/build/$$p/varnish_overlay.so" \
		   "$(VARNISH)/launch.sh" "$(VARNISH)/pak.json" "$(VARNISH)/LICENSE" $$out/ && \
		cp "$(VARNISH)/include/varnish.h" $$out/include/ && \
		cp "$(VARNISH)/scripts/varnish.sh" $$out/scripts/ || exit 1; \
	done
	@ls -la dist/varnish/*/varnish | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

# The quick menu is compiled against app/varnish.h, a copy of the bundled
# Varnish's header: they must agree, or the menu speaks a protocol the
# installed daemon does not.
check-varnish:
	@for p in $(VARNISH_PLATFORMS); do \
		test -x dist/varnish/$$p/varnish || { \
			echo "missing dist/varnish/$$p - run 'make varnish' first"; exit 1; }; \
		[ "$$(sed 1,4d app/varnish.h)" = "$$(sed 1d dist/varnish/$$p/include/varnish.h)" ] || { \
			echo "app/varnish.h differs from the bundled Varnish's header - update it from there"; exit 1; }; \
	done

check-shim-%:
	@test -f bin/$*/netplay_shim.so || { \
		echo "missing bin/$*/netplay_shim.so - run 'make shim' first"; exit 1; }

check-app-%:
	@test -f bin/$*/netplay.elf || { \
		echo "missing bin/$*/netplay.elf - run 'make app' first"; exit 1; }
	@test -f bin/$*/netplay-broker.elf || { \
		echo "missing bin/$*/netplay-broker.elf - run 'make app' first"; exit 1; }
	@test -f bin/$*/netplay-quickmenu.elf || { \
		echo "missing bin/$*/netplay-quickmenu.elf - run 'make app' first"; exit 1; }

clean:
	rm -rf dist
