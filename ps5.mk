# The console payload, built with ps5-payload-sdk. Called from Makefile.

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

.DEFAULT_GOAL := all
VERSION := $(shell sed -n 's/^\#define WHATEVERPAD_VERSION "\(.*\)"/\1/p' src/version.h)
BUILD_REF ?= $(shell GIT_MASTER=1 git describe --always --dirty)
WHATEVERPAD_PROBE ?= 0
WHATEVERPAD_ATTRIB_POLICY ?= OWN_POLICY_NO_RESTORE
WHATEVERPAD_PROBE_CREDS ?= raised
ELF   := dist/WhateverPad-PS5-$(VERSION).elf
BUILD := build/ps5
BUILD_ID := build/build_id.h

CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -Isrc -Ibuild -MMD -MP -DWHATEVERPAD_PROBE=$(WHATEVERPAD_PROBE) -DWHATEVERPAD_ATTRIB_POLICY=$(WHATEVERPAD_ATTRIB_POLICY)
ifeq ($(WHATEVERPAD_PROBE),1)
ifeq ($(filter $(WHATEVERPAD_PROBE_CREDS),raised original),)
$(error WHATEVERPAD_PROBE_CREDS must be raised or original)
endif
PROBE_TAG := T8-v2-$(WHATEVERPAD_PROBE_CREDS)
ELF := dist/WhateverPad-PS5-$(VERSION)-$(PROBE_TAG).elf
BUILD := build/ps5-$(PROBE_TAG)
BUILD_ID := $(BUILD)/build_id.h
CFLAGS := $(filter-out -Ibuild,$(CFLAGS)) -I$(BUILD) -DWHATEVERPAD_PROBE_ORIGINAL=$(if $(filter original,$(WHATEVERPAD_PROBE_CREDS)),1,0)
endif
LDLIBS  += -lScePad -lSceUserService -lSceSystemService -lSceAppInstUtil -ldl

SRCS := src/util.c src/log.c src/attrib.c src/probe.c src/crc32.c src/smp_crypto.c src/profiles.c src/generic.c \
        src/bond_store.c src/host.c src/le.c src/ownership.c src/session.c \
        src/web.c src/web_page.c \
        src/config.c src/migrate.c src/evstream.c src/hotkey.c src/lock.c src/netinfo.c src/usb_desc.c \
        src/hci_usb.c src/ps5_power.c src/ps5_ui.c src/ps5_apps.c src/ps5_sysinfo.c src/launcher.c src/icon_data.c src/ps5_vpad.c src/ps5_main.c
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all FORCE
all: $(ELF).sha256

$(BUILD_ID): src/version.h FORCE
	@mkdir -p $(dir $@)
	@test -n "$(VERSION)" && test -n "$(BUILD_REF)"
	@printf '%s\n' '#include "version.h"' \
	  '#define WHATEVERPAD_BUILD_ID "$(VERSION)+$(BUILD_REF)$(if $(PROBE_TAG),+$(PROBE_TAG))"' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/ps5_main.o: $(BUILD_ID)

$(ELF): $(OBJS)
	@mkdir -p dist
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(ELF).sha256: $(ELF)
	sha256sum $< > $@

$(BUILD)/%.o: src/%.c ps5.mk
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)
