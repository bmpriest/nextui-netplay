.PHONY: dist dist-base dist-compatibility dist-full shim cores core-sources compatibility compatibility-sources compatibility-armv7 compatibility-aarch64 check-compatibility check-netplay-cores cores-gambatte cores-gambatte-dual cores-gpsp cores-mgba cores-mgba-dual mgba-pak gblc-pak app test test-gambatte-dual clean help

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

help:
	@echo "make shim    cross-build the shim for $(PLATFORMS) (needs docker)"
	@echo "make cores   build the patched gambatte + gpsp cores (needs docker)"
	@echo "make cores-gambatte-dual  build the experimental in-process dual Gambatte core"
	@echo "make gblc-pak  build a standalone my282 GBLC emulator test pak"
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

#  mgba - carries a working lockstep cable in core code, but the libretro build
#         does not compile the two SIO drivers that use it. We add them; that is
#         the whole of this change so far. Pinned to the same revision NextUI
#         builds (925f0f0b) so our core and the shipped one are the same mGBA:
#         later revisions moved the libretro build out of the repo root and the
#         platform patch no longer applies.
MGBA_SRC  := $(CORE_SRC_ROOT)/mgba
MGBA_REPO := https://github.com/libretro/mgba
MGBA_REV  := 925f0f0bc1f51c79ca5446d7427512c653e615b3
MGBA_DUAL_FRONTEND := cores/mgba/libretro_dual.c
MGBA_PATCHES := cores/patches/mgba-platforms.patch cores/patches/mgba-001-build-lockstep-drivers.patch cores/patches/mgba-002-dual-frontend.patch
MGBA_STAMP := $(MGBA_SRC)/.netplay-patched-$(MGBA_REV)

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

$(MGBA_STAMP): $(MGBA_PATCHES) $(MGBA_DUAL_FRONTEND) shim/include/gambatte_dual.h
	@rm -rf "$(MGBA_SRC)"
	@mkdir -p "$(CORE_SRC_ROOT)"
	@git clone -q "$(MGBA_REPO)" "$(MGBA_SRC)"
	@git -C "$(MGBA_SRC)" checkout -q "$(MGBA_REV)"
	@patch -d "$(MGBA_SRC)" -p1 < cores/patches/mgba-platforms.patch
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

# Only tg5040 (and h700, which core_build maps onto it) has an mGBA platform
# branch - ours is NextUI's, and NextUI only ever added that one. Building the
# others would hit the `else` at the bottom of mGBA's Makefile.libretro and
# silently produce a Windows target, exactly the failure the comment above
# core_build warns about. Restrict the loop rather than trust the caller.
# mGBA's `platform=` strings are its own, and do not line up with ours or with
# core_build's mapping. tg5040 gets a branch from the platform patch; my282 has
# none and uses the generic unix branch with CC from the toolchain, which is
# exactly what NextUI does for it. Anything else selects no branch and the build
# dies with "no makefile found" - or worse, falls through to Windows.
MGBA_PLATFORMS := tg5040 h700 my282

mgba_platform = $(if $(filter tg5040 h700,$(1)),tg5040,$(if $(filter my282,$(1)),unix,))

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
		mp=$$(case $$p in tg5040|h700) echo tg5040 ;; my282) echo unix ;; esac); \
		echo "== mgba $$p (mgba platform=$$mp)"; \
		mkdir -p dist/cores/$$p; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD="make -C $(MGBA_SRC) -f Makefile.libretro platform=$$mp clean >/dev/null 2>&1 && \
			make -C $(MGBA_SRC) -f Makefile.libretro platform=$$mp -j4 && \
			cp $(MGBA_SRC)/mgba_libretro.so dist/cores/$$p/" || exit 1; \
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
		mp=$$(case $$p in tg5040|h700) echo tg5040 ;; my282) echo unix ;; esac); \
		echo "== mgba-dual $$p (mgba platform=$$mp)"; \
		mkdir -p dist/cores-experimental/$$p; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD="make -C $(MGBA_SRC) -f Makefile.libretro NETPLAY_DUAL_INSTANCE=1 platform=$$mp clean >/dev/null 2>&1 && \
			make -C $(MGBA_SRC) -f Makefile.libretro NETPLAY_DUAL_INSTANCE=1 platform=$$mp -j4 && \
			cp $(MGBA_SRC)/mgba_libretro.so dist/cores-experimental/$$p/mgba_dual_libretro.so" || exit 1; \
	done
	@for p in $(PLATFORMS); do \
		printf "   %-7s abi=%s scheduler=%s\n" "$$p" \
			"$$(nm -D --defined-only dist/cores-experimental/$$p/mgba_dual_libretro.so 2>/dev/null | grep -c retro_dual_get_abi_version)" \
			"$$(strings dist/cores-experimental/$$p/mgba_dual_libretro.so | grep -c 0.11-dev-netdual1)"; \
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
app:
	@for p in $(PLATFORMS); do \
		echo "== app $$p"; \
		$(MAKE) -s -C "$(BUILDER)" build PLATFORM=$$p PROJECT="$$PWD" \
			CMD='sh -c "cd app && make PLATFORM='$$p' NEXTUI=/opt/nextui-src"' || exit 1; \
	done
	@ls -la bin/*/netplay.elf bin/*/netplay-broker.elf bin/*/netplay-wifips.elf bin/*/netplay-quickmenu.elf | awk '{printf "   %-46s %s bytes\n", $$NF, $$5}'

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
	@./testing/test-instanced-ui.sh
	@./testing/test-wifi-platform.sh
	@./testing/test-log-rotation.sh
	@./testing/test-wifi-watchdog.sh
	@./testing/test-status-icon.sh
	@./testing/test-quick-menu.sh
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
	@cp launcher/minarch.elf launcher/launch-stub.sh launcher/adhoc-join.sh launcher/wifi-watchdog.sh launcher/session-cleanup.sh \
	    launcher/install-stubs.sh launcher/wrap-pak.sh launcher/bind-mount.sh launcher/pre-launch.sh \
	    launcher/mount-common.sh launcher/state-path.sh launcher/gameswitcher.sh launcher/gameswitcher-launch.sh launcher/mgba-manage.sh \
	    launcher/wifi-platform.sh launcher/status-icon.sh launcher/quick-menu.sh \
	    "$(BASE_STAGE)/$(PAK)/launcher/"
	@mkdir -p "$(BASE_STAGE)/$(PAK)/res"
	@cp res/status@1x.png res/status@2x.png res/status@3x.png res/status@4x.png \
	    "$(BASE_STAGE)/$(PAK)/res/"
	@cp launcher/session.conf.example "$(BASE_STAGE)/$(PAK)/"
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
	@for p in $(PLATFORMS); do \
		mkdir -p "$(BASE_STAGE)/$(PAK)/bin/$$p"; \
		cp bin/$$p/netplay_shim.so "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-broker.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-wifips.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
		cp bin/$$p/netplay-quickmenu.elf "$(BASE_STAGE)/$(PAK)/bin/$$p/"; \
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
