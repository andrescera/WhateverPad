# WhateverPad PS5: Bluetooth game controllers on a jailbroken PS5.
#
#   make test                 builds and runs the tests on this machine
#   make ps5                  builds the payload (needs PS5_PAYLOAD_SDK)
#   make send PS5_HOST=ip     sends it to the console's payload loader

PS5_HOST ?= ps5
PS5_PORT ?= 9021
WHATEVERPAD_PROBE ?= 0

CORE   := src/util.c src/log.c src/attrib.c src/crc32.c src/smp_crypto.c src/profiles.c src/generic.c src/bond_store.c src/host.c src/le.c
WEB    := src/web.c src/web_page.c
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -D_DEFAULT_SOURCE $(CPPFLAGS) -DWHATEVERPAD_PROBE=$(WHATEVERPAD_PROBE)

BUILD := build

.PHONY: all test test-attrib fuzz demo ps5 send clean check-generated sanitize ci

all: test

$(BUILD):
	@mkdir -p $(BUILD)

# Tracked embeds are updated explicitly, never as a side effect of a build.
check-generated: | $(BUILD)
	sh tools/generate-assets.sh $(BUILD)/generated
	@status=0; for file in web_page.c icon_data.c; do \
	  if ! cmp -s src/$$file $(BUILD)/generated/$$file; then \
	    echo "generated file differs: src/$$file" >&2; status=1; \
	  fi; \
	done; exit $$status

$(BUILD)/test_web: $(CORE) $(WEB) tests/sim.c tests/test_web.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

# The page against a simulated pad, to look at in a browser: make demo
$(BUILD)/web_demo: $(CORE) $(WEB) tests/sim.c tests/web_demo.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^ -lm

demo: $(BUILD)/web_demo
	./$(BUILD)/web_demo

$(BUILD)/test_host: $(CORE) tests/sim.c tests/test_host.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_compat: $(CORE) tests/sim.c tests/test_compat.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_profiles: $(CORE) tests/test_profiles.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_crypto: src/smp_crypto.c tests/test_crypto.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_le: $(CORE) tests/sim_le.c tests/test_le.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_bond_store: src/bond_store.c tests/test_bond_store.c | $(BUILD)
	cc $(HOST_CFLAGS) -DBOND_STORE_TEST -o $@ $^

# The console source for virtual pads, against stand-ins for the console's libraries.
# Keep the loader stand-ins from intercepting the sanitizer runtime's own calls.
$(BUILD)/test_vpad: src/ps5_vpad.c src/log.c src/util.c tests/test_vpad.c | $(BUILD)
	cc $(HOST_CFLAGS) -D_DARWIN_C_SOURCE -Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym -o $@ $^

# Configuration, the console hotkey and the local address.
$(BUILD)/test_config: src/config.c src/hotkey.c src/netinfo.c src/log.c src/util.c tests/test_config.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

# The instance lock.
$(BUILD)/test_lock: src/lock.c src/log.c src/util.c tests/test_lock.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_launcher: src/launcher.c src/icon_data.c tests/test_launcher.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

# Endpoints from the chip's descriptor (checked against a real dump) and event reassembly.
$(BUILD)/test_usb: src/usb_desc.c src/evstream.c tests/test_usb.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_hci_usb.o: src/hci_usb.c src/hci_usb.h src/hci.h src/attrib.h tests/usb_standin/dev/usb/usb.h tests/usb_standin/dev/usb/usb_ioctl.h tests/usb_standin/syscalls.h | $(BUILD)
	cc $(HOST_CFLAGS) -Itests/usb_standin -include tests/usb_standin/syscalls.h -c -o $@ $<

$(BUILD)/test_hci_usb: $(BUILD)/test_hci_usb.o src/usb_desc.c src/evstream.c src/log.c src/util.c src/attrib.c tests/test_hci_usb.c tests/usb_standin/dev/usb/usb.h tests/usb_standin/dev/usb/usb_ioctl.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c %.o,$^)

$(BUILD)/test_attrib: $(CORE) $(BUILD)/test_hci_usb.o src/usb_desc.c src/evstream.c tests/sim.c tests/test_attrib.c tests/test_hci_usb.c src/attrib.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_hci_usb.c,$(filter %.c %.o,$^))

test-attrib: $(BUILD)/test_attrib
	./$(BUILD)/test_attrib

$(BUILD)/test_session: $(CORE) tests/sim.c tests/sim_le.c tests/test_session.c tests/sim.h tests/sim_le.h tests/sim_lifetime.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c,$^)

test: $(BUILD)/test_attrib $(BUILD)/test_bond_store $(BUILD)/test_hearbridge $(BUILD)/test_usb $(BUILD)/test_hci_usb $(BUILD)/test_session $(BUILD)/test_lock $(BUILD)/test_config $(BUILD)/test_vpad $(BUILD)/test_crypto $(BUILD)/test_profiles $(BUILD)/test_host $(BUILD)/test_le $(BUILD)/test_compat $(BUILD)/test_web $(BUILD)/test_launcher
	./$(BUILD)/test_attrib
	./$(BUILD)/test_bond_store
	./$(BUILD)/test_usb
	./$(BUILD)/test_hci_usb
	./$(BUILD)/test_session
	./$(BUILD)/test_lock
	./$(BUILD)/test_config
	./$(BUILD)/test_vpad
	./$(BUILD)/test_crypto
	./$(BUILD)/test_profiles
	./$(BUILD)/test_host
	./$(BUILD)/test_le
	./$(BUILD)/test_compat
	./$(BUILD)/test_web
	./$(BUILD)/test_launcher
	./$(BUILD)/test_hearbridge

# Malformed input through every parser, under the sanitizers.
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
sanitize:
	$(MAKE) test BUILD=build/san HOST_CFLAGS="$(HOST_CFLAGS) $(SAN)"

ci:
	$(MAKE) test
	$(MAKE) sanitize
	$(MAKE) fuzz
	$(MAKE) check-generated
	$(MAKE) test-attrib BUILD=build/probe WHATEVERPAD_PROBE=1
	@checks="$$( $(MAKE) -qp | awk -F: '/^check-(ownership|mutations):/ { print $$1 }' )"; \
	  if [ -n "$$checks" ]; then $(MAKE) $$checks; fi

fuzz: | $(BUILD)
	cc $(HOST_CFLAGS) $(SAN) -o $(BUILD)/test_fuzz $(CORE) tests/test_fuzz.c
	./$(BUILD)/test_fuzz
	cc $(HOST_CFLAGS) $(SAN) -o $(BUILD)/test_bond_store_fuzz src/bond_store.c tests/test_bond_store_fuzz.c
	./$(BUILD)/test_bond_store_fuzz

# ---- the console payload ----------------------------------------------------

ps5: check-generated
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined)
endif
	$(MAKE) -f ps5.mk

send: ps5
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) dist/WhateverPad-PS5-*.elf

clean:
	rm -rf $(BUILD)

# Targeted regressions from the direct HearBridge review.
$(BUILD)/test_hearbridge: $(CORE) tests/sim.c tests/test_hearbridge.c tests/test_host.c | $(BUILD)
	cc $(HOST_CFLAGS) -DEXPECT_CFG=1 -DEXPECT_BACKOFF=1 -o $@ $(CORE) tests/sim.c tests/test_hearbridge.c
