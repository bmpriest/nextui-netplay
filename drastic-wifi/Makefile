CC ?= cc
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fPIC -Iinclude -I../shim

BUILD := build
LIB := $(BUILD)/libdrastic_wifi.so
HOOK_TEST := $(BUILD)/test_hook
MODEL_TEST := $(BUILD)/test_model
ADAPTER_TEST := $(BUILD)/test_adapter

.PHONY: all test clean

all: $(LIB)

$(BUILD):
	mkdir -p $@

$(LIB): src/hook.c src/model.c src/adapter.c src/transport.c ../shim/netlink.c \
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
