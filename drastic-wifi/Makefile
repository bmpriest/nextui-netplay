CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fPIC -Iinclude -I../shim

CXX ?= c++
CXXFLAGS ?= -O2 -g

BUILD := build
# The radio: DSperate's Wi-Fi block and melonDS LAN transport (GPL-3.0-or-later)
# attached to DraStic by radio_hook.c. libstdc++ is linked in: DraStic is C and
# its runtime directory does not ship one.
LIB := $(BUILD)/libdrastic_wifi.so
# The earlier hand-written model and transport, kept for its diagnostics.
DIAG_LIB := $(BUILD)/libdrastic_wifi_diag.so
RADIO_CXX := src/radio/radio.cpp src/radio/wifi.cpp src/radio/lan_mp.cpp
ENET_SRC := $(addprefix src/radio/enet/,callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c)
ENET_DEFS := -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 \
	-DHAS_GETHOSTBYNAME_R=1 -DHAS_GETHOSTBYADDR_R=1 -DHAS_INET_PTON=1 \
	-DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1
RADIO_OBJ := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(RADIO_CXX)) \
	$(patsubst src/%.c,$(BUILD)/obj/%.o,$(ENET_SRC)) \
	$(BUILD)/obj/radio_hook.o $(BUILD)/obj/hook_helpers.o
HOOK_TEST := $(BUILD)/test_hook
MODEL_TEST := $(BUILD)/test_model
ADAPTER_TEST := $(BUILD)/test_adapter

.PHONY: all test clean

all: $(LIB)

$(BUILD):
	mkdir -p $@

$(LIB): $(RADIO_OBJ) src/radio/exports.map
	$(CXX) -shared -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--version-script=src/radio/exports.map -Wl,--no-undefined \
		-Wl,-z,defs $(RADIO_OBJ) -lpthread -o $@

$(BUILD)/obj/radio/enet/%.o: src/radio/enet/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -fPIC -fvisibility=hidden -w $(ENET_DEFS) -Isrc/radio/enet/include -c $< -o $@

$(BUILD)/obj/radio/%.o: src/radio/%.cpp src/radio/*.h include/drastic_wifi_radio.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -fPIC -fvisibility=hidden \
		-Iinclude -Isrc/radio -Isrc/radio/enet/include -c $< -o $@

$(BUILD)/obj/radio_hook.o: src/radio_hook.c include/drastic_wifi_radio.h include/drastic_wifi_hook.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fvisibility=hidden -c $< -o $@

$(BUILD)/obj/hook_helpers.o: src/hook.c include/drastic_wifi_hook.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fvisibility=hidden -DDWIFI_NO_RUNTIME -c $< -o $@

$(DIAG_LIB): src/hook.c src/model.c src/adapter.c src/transport.c ../shim/netlink.c \
	include/drastic_wifi_hook.h include/drastic_wifi_model.h \
	include/drastic_wifi_adapter.h include/drastic_wifi_transport.h | $(BUILD)
	$(CC) $(CFLAGS) -shared src/hook.c src/model.c src/adapter.c \
		src/transport.c ../shim/netlink.c -o $@

$(HOOK_TEST): tests/test_hook.c src/hook.c include/drastic_wifi_hook.h | $(BUILD)
	$(CC) $(CFLAGS) -DDWIFI_NO_RUNTIME tests/test_hook.c src/hook.c -o $@

$(MODEL_TEST): tests/test_model.c src/model.c include/drastic_wifi_model.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_model.c src/model.c -o $@

$(ADAPTER_TEST): tests/test_adapter.c src/adapter.c src/model.c \
	include/drastic_wifi_adapter.h include/drastic_wifi_model.h | $(BUILD)
	$(CC) $(CFLAGS) tests/test_adapter.c src/adapter.c src/model.c -o $@

test: $(HOOK_TEST) $(MODEL_TEST) $(ADAPTER_TEST)
	$(HOOK_TEST)
	$(MODEL_TEST)
	$(ADAPTER_TEST)

clean:
	rm -rf $(BUILD)
