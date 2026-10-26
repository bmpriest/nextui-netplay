PAK_NAME := $(shell jq -r .name pak.json)
PAK_TYPE := $(shell jq -r .type pak.json)
PAK_FOLDER := $(shell echo $(PAK_TYPE) | cut -c1)$(shell echo $(PAK_TYPE) | tr '[:upper:]' '[:lower:]' | cut -c2-)s

PUSH_SDCARD_PATH ?= /mnt/SDCARD
PUSH_PLATFORM ?= tg5040

SHELL := /bin/bash

PLATFORMS := tg5040 tg5050 h700

# ── Upstream repo and pinned version ──────────────────────────────────────────
# GPLv3. Bumping this is a deliberate act: read the upstream release notes for
# CMake option changes first, because a renamed switch fails open (CMake ignores
# unknown -D cache args) and scripts/assert-build.sh is the only thing that
# would catch it.
DSPERATE_REPO := https://github.com/beebono/DSperate
DSPERATE_TAG  := v3.0.5

# ── Docker toolchain images ───────────────────────────────────────────────────
TG5040_IMAGE := ghcr.io/loveretro/tg5040-toolchain:latest
TG5050_IMAGE := ghcr.io/loveretro/tg5050-toolchain:latest
# The h700 image is the tg5040 image plus a patched mali-fbdev SDL2 under
# $PREFIX_LOCAL: same cross compiler, same TrimUI SDK sysroot.
H700_IMAGE   := ghcr.io/loveretro/h700-toolchain:latest

# ── Platform specific CPU flags ───────────────────────────────────────────────
TG5040_CPUFLAGS := -mcpu=cortex-a53 -mtune=cortex-a53
TG5050_CPUFLAGS := -mcpu=cortex-a55 -mtune=cortex-a55
H700_CPUFLAGS   := -mcpu=cortex-a53 -mtune=cortex-a53

# ── ABI ceiling, per platform ─────────────────────────────────────────────────
# Enforced by scripts/check-abi.sh against the stripped binary.
#
# The ceiling is per platform because the toolchains genuinely differ: tg5040
# and h700 are GCC 8.3.0 over a glibc 2.28 sysroot, while tg5050 is GCC 10.3.0
# over glibc 2.33. Measured at v3.0.5: tg5040 and h700 come in at GLIBC_2.18,
# tg5050 at GLIBC_2.33.
#
# Each ceiling is the toolchain's own floor, not the device's, so it fails loudly
# if a build ever reaches outside its sysroot. The devices are looser -- 2.33 on
# the TrimUI SDK, 2.35 on H700 stock -- so none of these is close to a limit.
TG5040_GLIBC_MAX ?= 2.28
TG5050_GLIBC_MAX ?= 2.33
H700_GLIBC_MAX   ?= 2.28

# ── Build knobs ───────────────────────────────────────────────────────────────
# PGO stays off: pgo/aarch64/MANIFEST fingerprints the compiler and flags it was
# generated with (GCC 13.3.0), and upstream's configure calls message(FATAL_ERROR)
# on a mismatch. See TECHNICAL.md.
DSPERATE_PGO ?= OFF
RUN_CTEST ?= 1
# Parallel compile and link jobs inside the containers; empty means one per CPU.
# Every test executable is an LTO link of the whole core, so ctest's final
# links can exhaust a desktop Docker VM's memory at one job per core.
# `make build JOBS=4` matches the 4-core CI runner.
JOBS ?=
DSP_JOBS := $(if $(JOBS),$(JOBS),$$(nproc))
# `input` is excluded because it links SDL2 and the container cannot run it: the
# only libSDL2 here is the sysroot's, and putting the sysroot on LD_LIBRARY_PATH
# drags its glibc in front of the container's loader, which dies on
# "undefined symbol: __libc_vfork, version GLIBC_PRIVATE". Nothing to do with
# the code -- it builds fine, and it is the one test that needs a runnable SDL2.
CTEST_EXCLUDE ?= ^input$$

# ── Paths ─────────────────────────────────────────────────────────────────────
ROOT    := $(shell pwd)
SRC     := $(ROOT)/src
BUILD   := $(ROOT)/build
DIST    := $(ROOT)/dist/$(PAK_NAME).pak
CONFIG  := $(ROOT)/config
PATCHES := $(ROOT)/patches
OVERLAY := $(ROOT)/overlay
CROSS   := aarch64-nextui-linux-gnu-

# The vendored MinUI sources the clone rule copies into src/frontend/sdl/, and
# the ctest suite it copies into tests/. stb_*.h are nothings/stb, unmodified.
MINUI_OVERLAY := minui.h minui.cpp minui_gfx.h minui_gfx.cpp minui_menu.h minui_menu.cpp \
	minui_pad.h minui_resample.h minui_resample.cpp minui_shader.h minui_shader.cpp minui_shader_presets.h minui_shortcuts.h \
	minui_stb.cpp \
	stb_image.h stb_truetype.h
MINUI_OVERLAY_TESTS := minui_menu_test.cpp minui_shader_test.cpp

DOCKER_SCRIPT := /build/scripts/docker-env.sh
DOCKER_RUN_tg5040 := docker run --rm -v $(ROOT):/build $(TG5040_IMAGE) $(DOCKER_SCRIPT)
DOCKER_RUN_tg5050 := docker run --rm -v $(ROOT):/build $(TG5050_IMAGE) $(DOCKER_SCRIPT)
DOCKER_RUN_h700   := docker run --rm -v $(ROOT):/build $(H700_IMAGE)   $(DOCKER_SCRIPT)

# ── Shared CMake cache arguments ──────────────────────────────────────────────
#
# WAYLAND=OFF: upstream compiles a Wayland dmabuf scanout tier whenever the SDL2
#   it configures against reports wayland. No MinUI device runs a compositor and
#   the tier would bind an SDL_SysWMinfo layout the runtime SDL2 does not share.
#   Upstream's guidance to integrators is the flag, not a patch.
# HEADLESS=OFF: dsperate-headless is a measurement harness, not shipped.
# TESTS=OFF: test binaries must never reach dist. ctest gets its own build dir.
# CHEEVOS=ON is safe: src/cheevos/cheevos_http.cpp declares curl's types itself
#   and dlopens the library, so the sysroot needs no curl headers.
# NET=ON builds netplay (vendored slirp and enet). It is upstream's default too,
#   but spelled out so an upstream default change cannot flip it unnoticed.
# VULKAN=OFF: none of these SoCs has Vulkan, and upstream enables the tier
#   whenever the build host happens to have <vulkan/vulkan.h>, so leaving it to
#   detection would make the binary depend on the container image.
DSP_CMAKE_COMMON := \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DDSPERATE_JIT=ON \
	-DDSPERATE_NEON=ON \
	-DDSPERATE_SDL=ON \
	-DDSPERATE_WAYLAND=OFF \
	-DDSPERATE_HEADLESS=OFF \
	-DDSPERATE_TESTS=OFF \
	-DDSPERATE_CHEEVOS=ON \
	-DDSPERATE_NET=ON \
	-DDSPERATE_VULKAN=OFF \
	-DDSPERATE_PGO=$(DSPERATE_PGO)

# DSperate uses std::thread but never links Threads::Threads. Upstream never
# notices because glibc 2.34 folded libpthread into libc; below that it is a
# real library. CMAKE_EXE_LINKER_FLAGS lands AHEAD of the objects that reference
# pthread_create, where --as-needed drops it again -- CMAKE_CXX_STANDARD_LIBRARIES
# is appended at the very end of the link line, the one place it survives.
# push-state/pop-state keeps --no-as-needed off everything else.
DSP_PTHREAD_LIBS := -Wl,--push-state,--no-as-needed -lpthread -Wl,--pop-state

# The test executables use std::filesystem through the core, and on GCC 8 that
# lives in a separate libstdc++fs. Upstream links it into dsperate itself when
# GCC < 9, but not into the tests. GCC 10 still ships the library, so this
# needs no compiler check; it is ctest-only because the shipping binary
# already gets it from upstream. https://github.com/beebono/DSperate/issues/24
DSP_CTEST_LIBS := $(DSP_PTHREAD_LIBS) -lstdc++fs

.PHONY: all build clone verify-pin patch dist release bump-version push lint test test-native screenshots clean clean-docker

all: dist

# ── Clone and pin ─────────────────────────────────────────────────────────────

clone: $(SRC)/DSperate

# Only vendored third-party code (miniz, rcheevos) -- no submodules.
$(SRC)/DSperate:
	git clone --depth 1 --branch $(DSPERATE_TAG) $(DSPERATE_REPO) $@
	@# The MinUI module is vendored here rather than patched in, so patch 0003
	@# only ever modifies files upstream already has.
	cp $(addprefix $(OVERLAY)/,$(MINUI_OVERLAY)) $@/src/frontend/sdl/
	cp $(addprefix $(OVERLAY)/tests/,$(MINUI_OVERLAY_TESTS)) $@/tests/
	@# A patch that no longer applies is fatal: a drifted patch must not produce
	@# a green build with the workaround silently missing.
	cd $@ && for p in $(PATCHES)/*.patch; do \
		[ -e "$$p" ] || continue; \
		echo "applying $$(basename $$p)"; \
		git apply "$$p"; \
	done

# A branch name in DSPERATE_TAG would clone fine and quietly make every build
# non-reproducible, so assert the checkout is an exact tag.
verify-pin: $(SRC)/DSperate
	@test "$$(cd $(SRC)/DSperate && git describe --tags --exact-match 2>/dev/null)" = "$(DSPERATE_TAG)" \
		|| { echo "error: $(SRC)/DSperate is not at tag $(DSPERATE_TAG)"; exit 1; }

# ── Per-platform build ────────────────────────────────────────────────────────

# $(1) platform  $(2) cpu flags
define DSP_BUILD
	mkdir -p $(BUILD)/$(1)
	$(DOCKER_RUN_$(1)) bash -c 'set -euo pipefail; \
	  cmake -S /build/src/DSperate -B /build/build/$(1) $(DSP_CMAKE_COMMON) \
	    -DCMAKE_TOOLCHAIN_FILE="$$CMAKE_TOOLCHAIN_FILE" \
	    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/build/build/$(1)/out \
	    -DCMAKE_C_FLAGS="-I$$CROSS_ROOT/include/ $(2) -pthread" \
	    -DCMAKE_CXX_FLAGS="-I$$CROSS_ROOT/include/ $(2) -pthread" \
	    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc $$DSP_SDL_LDFLAGS" \
	    -DCMAKE_CXX_STANDARD_LIBRARIES="$(DSP_PTHREAD_LIBS)" \
	    2>&1 | tee /build/build/$(1)/configure.log; \
	  cmake --build /build/build/$(1) --target dsperate -- -j$(DSP_JOBS); \
	  $$CC /build/scripts/pad-guid.c -o /build/build/$(1)/out/dsp-pad-guid -O2 \
	    $$(pkg-config --cflags --libs sdl2)'
	$(DOCKER_RUN_$(1)) /build/scripts/assert-build.sh /build/build/$(1)
endef

build-tg5040: verify-pin ; $(call DSP_BUILD,tg5040,$(TG5040_CPUFLAGS))
build-tg5050: verify-pin ; $(call DSP_BUILD,tg5050,$(TG5050_CPUFLAGS))
build-h700:   verify-pin ; $(call DSP_BUILD,h700,$(H700_CPUFLAGS))
.PHONY: build-tg5040 build-tg5050 build-h700

# ── Stage: strip, then gate on the exact bytes that ship ──────────────────────

# $(1) platform  $(2) glibc ceiling
#
# Copy, strip and check all happen inside one container: a host-side cp followed
# by an in-container strip races the bind mount's write visibility on macOS, and
# strip then fails with "file truncated" on a file the host has already written.
define DSP_STAGE
	$(DOCKER_RUN_$(1)) bash -c 'set -euo pipefail; \
	  mkdir -p /build/build/$(1)/staged; \
	  for b in dsperate dsp-pad-guid; do \
	    cp /build/build/$(1)/out/$$b /build/build/$(1)/staged/$$b; \
	    $(CROSS)strip --strip-unneeded /build/build/$(1)/staged/$$b; \
	  done'
	@# build/$(1)/out keeps the unstripped copies for symbolising device crashes.
	$(DOCKER_RUN_$(1)) /build/scripts/check-abi.sh $(2) \
	    /build/build/$(1)/staged/dsperate /build/build/$(1)/staged/dsp-pad-guid
endef

stage-tg5040: build-tg5040 ; $(call DSP_STAGE,tg5040,$(TG5040_GLIBC_MAX))
stage-tg5050: build-tg5050 ; $(call DSP_STAGE,tg5050,$(TG5050_GLIBC_MAX))
stage-h700:   build-h700   ; $(call DSP_STAGE,h700,$(H700_GLIBC_MAX))
.PHONY: stage-tg5040 stage-tg5050 stage-h700

build: clone verify-pin
	$(MAKE) stage-tg5040
	$(MAKE) stage-tg5050
	$(MAKE) stage-h700
	$(MAKE) test-native

# ── ctest ─────────────────────────────────────────────────────────────────────
#
# Built once, in its own directory so the shipping builds never carry
# DSPERATE_TESTS=ON. One flag set is enough: all three platforms build identical
# source with identical options and only -mcpu/-mtune differ, which changes
# scheduling and not semantics. Using the cortex-a53 build keeps the tested
# binary a strict ARMv8-A baseline.
#
# CI runs on ubuntu-24.04-arm, so the toolchain container is native arm64 and
# these aarch64 binaries execute directly -- no qemu-aarch64-static.
test-native: clone verify-pin
ifeq ($(RUN_CTEST),1)
	mkdir -p $(BUILD)/ctest
	$(DOCKER_RUN_tg5040) bash -c 'set -euo pipefail; \
	  cmake -S /build/src/DSperate -B /build/build/ctest $(DSP_CMAKE_COMMON) \
	    -DDSPERATE_TESTS=ON \
	    -DCMAKE_TOOLCHAIN_FILE="$$CMAKE_TOOLCHAIN_FILE" \
	    -DCMAKE_C_FLAGS="-I$$CROSS_ROOT/include/ $(TG5040_CPUFLAGS) -pthread" \
	    -DCMAKE_CXX_FLAGS="-I$$CROSS_ROOT/include/ $(TG5040_CPUFLAGS) -pthread" \
	    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc $$DSP_SDL_LDFLAGS" \
	    -DCMAKE_CXX_STANDARD_LIBRARIES="$(DSP_CTEST_LIBS)"; \
	  cmake --build /build/build/ctest -- -j$(DSP_JOBS); \
	  ctest --test-dir /build/build/ctest --output-on-failure --timeout 300 \
	    $(if $(CTEST_EXCLUDE),-E "$(CTEST_EXCLUDE)",)'
else
	@echo "skipping ctest (RUN_CTEST=$(RUN_CTEST))"
endif

# ── Screenshots ───────────────────────────────────────────────────────────────
#
# Every page of the pause menu, in both looks and at every panel size the pak
# runs on, as PNGs in build/screenshots. scripts/render-menu.cpp links the
# menu that ships and draws it over a stand-in game; nothing needs a device.
# Built and run in the tg5040 container like ctest, so it runs natively on an
# arm64 host and needs qemu anywhere else.
#
# The launcher fonts are fetched at a pinned commit rather than vendored:
# MinUI carries no licence and NextUI's is noncommercial, so neither can ship
# here. Without them, or offline, the menu's built-in font stands in.
SCREENSHOTS := $(BUILD)/screenshots
MINUI_FONT_URL  := https://raw.githubusercontent.com/shauninman/MinUI/53e0296ea5a2794290fb5783765af6cee0063445/skeleton/SYSTEM/res/BPreplayBold-unhinted.otf
NEXTUI_FONT_URL := https://raw.githubusercontent.com/LoveRetro/NextUI/be751f7812bcfc1181b2f0da285bfcb02a5d57c3/skeleton/SYSTEM/res/font1.ttf

screenshots: clone verify-pin
	mkdir -p $(SCREENSHOTS)/fonts $(SCREENSHOTS)/obj
	$(DOCKER_RUN_tg5040) bash -c 'set -euo pipefail; \
	  F=/build/build/screenshots/fonts; O=/build/build/screenshots/obj; \
	  fetch() { [ -s "$$2" ] || wget -q -O "$$2" "$$1" || { rm -f "$$2"; echo "screenshots: cannot fetch $$1; the built-in font stands in"; }; }; \
	  fetch "$(MINUI_FONT_URL)" $$F/BPreplayBold-unhinted.otf; \
	  fetch "$(NEXTUI_FONT_URL)" $$F/font1.ttf; \
	  cd /build/src/DSperate; \
	  for c in miniz miniz_tdef miniz_tinfl; do $$CC -O2 -w -Isrc -c src/core/cart/miniz/$$c.c -o $$O/$$c.o; done; \
	  $$CC -O2 -Isrc -c src/core/gpu/pow_compat.c -o $$O/pow_compat.o; \
	  $$CXX -std=c++17 -O2 -w -Isrc -c src/frontend/sdl/minui_stb.cpp -o $$O/minui_stb.o; \
	  $$CXX -std=c++17 -O2 -Isrc -static-libstdc++ -static-libgcc /build/scripts/render-menu.cpp \
	    src/frontend/sdl/minui_menu.cpp src/frontend/sdl/minui_gfx.cpp src/frontend/sdl/menu.cpp \
	    src/frontend/sdl/settings.cpp src/frontend/sdl/minui_shader.cpp src/frontend/sdl/minui_resample.cpp src/frontend/video/layout.cpp \
	    $$O/pow_compat.o \
	    $$O/minui_stb.o $$O/miniz.o $$O/miniz_tdef.o $$O/miniz_tinfl.o \
	    -lm -pthread -o $$O/render-menu; \
	  $$O/render-menu /build/build/screenshots $$F/BPreplayBold-unhinted.otf $$F/font1.ttf'

# ── Dist ──────────────────────────────────────────────────────────────────────

# $(1) platform
define DSP_DIST
	mkdir -p $(DIST)/$(1)
	cp $(BUILD)/$(1)/staged/dsperate $(DIST)/$(1)/dsperate
	cp $(BUILD)/$(1)/staged/dsp-pad-guid $(DIST)/$(1)/dsp-pad-guid
	chmod +x $(DIST)/$(1)/dsperate $(DIST)/$(1)/dsp-pad-guid
endef

dist-tg5040: stage-tg5040 dist-common ; $(call DSP_DIST,tg5040)
dist-tg5050: stage-tg5050 dist-common ; $(call DSP_DIST,tg5050)
dist-h700:   stage-h700   dist-common ; $(call DSP_DIST,h700)
.PHONY: dist-tg5040 dist-tg5050 dist-h700 dist-common

# Platform-independent payload, staged once at the pak root rather than copied
# per platform: unlike mupen64plus, DSperate takes every data path from its ini,
# so nothing has to sit beside the binary.
dist-common: clone
	mkdir -p $(DIST)/configs
	cp $(CONFIG)/dsperate/no-sticks.ini $(DIST)/configs/
	cp $(CONFIG)/dsperate/one-stick.ini $(DIST)/configs/
	cp $(CONFIG)/dsperate/two-sticks.ini $(DIST)/configs/
	@# Upstream's fully commented defaults, shipped as documentation only. The
	@# seeder never reads it; it is there so a user can see every key and its
	@# default without a network connection.
	cp $(SRC)/DSperate/configs/default.ini $(DIST)/configs/reference.ini
	cp $(CONFIG)/platform.sh $(DIST)/platform.sh
	cp launch.sh $(DIST)/launch.sh
	chmod +x $(DIST)/launch.sh
	cp pak.json $(DIST)/
	@# GPLv3: ship the licence with the binaries and record exactly what they
	@# were built from, so the corresponding-source obligation is answerable.
	cp $(SRC)/DSperate/LICENSE $(DIST)/LICENSE.DSperate
	@# Who wrote the shaders COLOUR and PATTERN port, and under what licence.
	cp THIRD_PARTY.md $(DIST)/THIRD_PARTY.md
	@# The oldest save state format this build can read. launch.sh compares it
	@# with a state's header before asking for --load-state, since a refused
	@# state fails the run. Read from the source so a format bump needs no
	@# change here; a missing match is fatal rather than a file launch.sh skips.
	@v="$$(sed -n 's/^constexpr u32 OLDEST_READABLE_VERSION = \([0-9][0-9]*\);$$/\1/p' \
		$(SRC)/DSperate/src/core/state/state.h)"; \
		test -n "$$v" || { echo "error: OLDEST_READABLE_VERSION not found in state.h"; exit 1; }; \
		printf '%s' "$$v" > $(DIST)/dsperate.state-version
	@printf 'repo %s\ntag %s\ncommit %s\npatches %s\nflags %s\n' \
		"$(DSPERATE_REPO)" "$(DSPERATE_TAG)" \
		"$$(cd $(SRC)/DSperate && git rev-parse HEAD)" \
		"$$(cd $(PATCHES) && ls *.patch | tr '\n' ' ')" \
		"$(DSP_CMAKE_COMMON)" > $(DIST)/dsperate.build-info

dist:
	$(MAKE) dist-tg5040
	$(MAKE) dist-tg5050
	$(MAKE) dist-h700
	@echo "=== dist/$(PAK_NAME).pak/ assembled ==="
	@find $(DIST) -type f | sort
	@du -sh $(DIST)

# ── Release ───────────────────────────────────────────────────────────────────

# The version bump belongs to a release, not to building an artifact: ci.yaml
# runs this target to produce a zip for review and sets no RELEASE_VERSION, so
# the bump is skipped there and pak.json keeps whatever version it has.
release: dist
	@if [ -n "$(RELEASE_VERSION)" ]; then \
		$(MAKE) bump-version; \
	else \
		echo "RELEASE_VERSION unset: keeping pak.json at $$(jq -r .version pak.json)"; \
	fi
	cp pak.json $(DIST)/
	cd $(DIST) && zip -r "../$(PAK_NAME).pak.zip" .
	ls -lah dist

# Guarded because an unset RELEASE_VERSION would otherwise quietly rewrite
# pak.json's version to the empty string. Only release.yaml sets it, and only
# release.yaml should be calling this.
bump-version:
	@test -n "$(RELEASE_VERSION)" \
		|| { echo "error: RELEASE_VERSION is not set"; exit 1; }
	jq '.version = "$(RELEASE_VERSION)"' pak.json > pak.json.tmp
	mv pak.json.tmp pak.json

push: release
	rm -rf "dist/$(PAK_NAME).pak.extracted"
	cd dist && unzip -q "$(PAK_NAME).pak.zip" -d "$(PAK_NAME).pak.extracted"
	adb push "dist/$(PAK_NAME).pak.extracted/." "$(PUSH_SDCARD_PATH)/$(PAK_FOLDER)/$(PUSH_PLATFORM)/$(PAK_NAME).pak"

# ── Checks ────────────────────────────────────────────────────────────────────

lint:
	shellcheck -s sh launch.sh config/platform.sh
	shellcheck -s bash scripts/*.sh
	shfmt -l -d -i 4 launch.sh config/platform.sh scripts/*.sh tests/*.bash

test:
	bats tests/

# Print the value of any make variable, e.g. `make print-H700_IMAGE`.
# tests/makefile.bats reads the build wiring through this.
print-%:
	@echo '$*=$($*)'

clean:
	rm -rf $(SRC) $(BUILD) $(ROOT)/dist

# Everything under src/ and build/ is written by root inside the containers, so
# a plain rm -rf from an unprivileged host account can fail. This deletes from
# inside a container instead.
clean-docker:
	docker run --rm -v $(ROOT):/build $(TG5040_IMAGE) rm -rf /build/src /build/build /build/dist
