PAK_NAME := $(shell jq -r .name pak.json)
PAK_TYPE := $(shell jq -r .type pak.json)
PAK_FOLDER := $(shell echo $(PAK_TYPE) | cut -c1)$(shell echo $(PAK_TYPE) | tr '[:upper:]' '[:lower:]' | cut -c2-)s

PUSH_SDCARD_PATH ?= /mnt/SDCARD
PUSH_PLATFORM ?= tg5040

SHELL := /bin/bash

PLATFORMS := tg5040 tg5050 h700

# ── Upstream repo and pinned revision ─────────────────────────────────────────
# GPLv3. Bumping this is a deliberate act: read the upstream log for CMake
# option changes first, because a renamed switch fails open (CMake ignores
# unknown -D cache args) and scripts/assert-build.sh is the only thing that
# would catch it.
#
# Pinned to a commit rather than a tag: the LAN join and netplay fixes Netplay.pak
# relies on landed after v3.0.5, and upstream main moves faster than its tags.
# DSPERATE_REPO can point at a local checkout to build without the network.
DSPERATE_REPO ?= https://github.com/beebono/DSperate
DSPERATE_REV  := ddf2910e6d80220c6b22b4df61f0786ac9879cda

# ── Docker toolchain images ───────────────────────────────────────────────────
# Pinned by digest to the same images build-platforms/platforms.conf uses for
# the rest of Netplay.pak, so a green build stays green.
TG5040_IMAGE := ghcr.io/loveretro/tg5040-toolchain@sha256:f131c6af64029a8723d0ce8d3c2682642f5f091b04714f6beedda9bec18477ab
TG5050_IMAGE := ghcr.io/loveretro/tg5050-toolchain@sha256:ed4aa21844feb84561abfeb05136c8a46f9c44d1d70c30f0984c11accb34a873
# The h700 image is the tg5040 image plus a patched mali-fbdev SDL2 under
# $PREFIX_LOCAL: same cross compiler, same TrimUI SDK sysroot.
H700_IMAGE   := ghcr.io/loveretro/h700-toolchain@sha256:bf5d8f9f5442cad789aa77176bc059ec823caefa3258bf30780eee681c777119

# ── Platform specific CPU flags ───────────────────────────────────────────────
TG5040_CPUFLAGS := -mcpu=cortex-a53 -mtune=cortex-a53
TG5050_CPUFLAGS := -mcpu=cortex-a55 -mtune=cortex-a55
H700_CPUFLAGS   := -mcpu=cortex-a53 -mtune=cortex-a53

# ── ABI ceiling, per platform ─────────────────────────────────────────────────
# Enforced by scripts/check-abi.sh against the stripped binary.
#
# The ceiling is per platform because the toolchains genuinely differ: tg5040
# and h700 are GCC 8.3.0 over a glibc 2.28 sysroot, while tg5050 is GCC 10.3.0
# over glibc 2.33. Measured at v1.15.1: tg5040 and h700 come in at GLIBC_2.18,
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
CROSS   := aarch64-nextui-linux-gnu-

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
# VULKAN=OFF: the presenter and GPU 3D raster need Vulkan 1.2 headers the
#   NextUI sysroots predate, and no supported device has a Vulkan driver.
# NET=ON: local wireless over the LAN is the reason Netplay.pak ships this.
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
	-DDSPERATE_VULKAN=OFF \
	-DDSPERATE_NET=ON \
	-DDSPERATE_PGO=$(DSPERATE_PGO)

# Upstream now links Threads::Threads and stdc++fs (GCC < 9) for the SDL
# frontend itself. This stays as belt and braces for the other targets
# (dsperate-headless and the tests do not get stdc++fs): CMAKE_EXE_LINKER_FLAGS
# lands AHEAD of the objects that reference them, where --as-needed drops them
# again, while CMAKE_CXX_STANDARD_LIBRARIES is appended at the very end of the
# link line. push-state/pop-state keeps --no-as-needed off everything else.
DSP_PTHREAD_LIBS := -Wl,--push-state,--no-as-needed -lpthread -Wl,--pop-state -lstdc++fs

.PHONY: all build clone verify-pin patch dist release bump-version push lint test test-native clean clean-docker

all: dist

# ── Clone and pin ─────────────────────────────────────────────────────────────

clone: $(SRC)/DSperate

# Only vendored third-party code (miniz, rcheevos, enet, libslirp) -- no
# submodules. The patches are a git am series, so the checkout's own history
# records exactly what was applied on top of the pin.
$(SRC)/DSperate:
	git clone $(DSPERATE_REPO) $@
	cd $@ && git checkout -q $(DSPERATE_REV)
	@# A patch that no longer applies is fatal: a drifted patch must not produce
	@# a green build with the workaround silently missing.
	cd $@ && git -c user.name=netplay -c user.email=netplay@localhost am -q $(PATCHES)/*.patch

# The pin is the commit the series was applied on, so assert that rather than
# HEAD, which is the last patch.
verify-pin: $(SRC)/DSperate
	@test "$$(cd $(SRC)/DSperate && git rev-parse HEAD~$$(ls $(PATCHES)/*.patch | wc -l))" = "$(DSPERATE_REV)" \
		|| { echo "error: $(SRC)/DSperate is not $(DSPERATE_REV) plus patches/"; exit 1; }

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
	  cmake --build /build/build/$(1) --target dsperate -- -j$$(nproc); \
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
	    -DCMAKE_CXX_STANDARD_LIBRARIES="$(DSP_PTHREAD_LIBS)"; \
	  cmake --build /build/build/ctest -- -j$$(nproc); \
	  ctest --test-dir /build/build/ctest --output-on-failure --timeout 300 \
	    $(if $(CTEST_EXCLUDE),-E "$(CTEST_EXCLUDE)",)'
else
	@echo "skipping ctest (RUN_CTEST=$(RUN_CTEST))"
endif

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
	@printf 'repo %s\nrev %s\ncommit %s\npatches %s\nflags %s\n' \
		"$(DSPERATE_REPO)" "$(DSPERATE_REV)" \
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
	shfmt -l -d -i 4 launch.sh config/platform.sh scripts/*.sh

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
