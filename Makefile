# WhateverPad PS5: Bluetooth game controllers on a jailbroken PS5.
#
#   make test                 builds and runs the tests on this machine
#   make ps5                  builds the payload (needs PS5_PAYLOAD_SDK)
#   make send PS5_HOST=ip     sends it to the console's payload loader

PS5_HOST ?= ps5
PS5_PORT ?= 9021
WHATEVERPAD_PROBE ?= 0

OWN_CORE := src/ownership.c src/session.c
CORE   := src/util.c src/log.c src/attrib.c src/crc32.c src/smp_crypto.c src/profiles.c src/generic.c src/bond_store.c src/host.c src/le.c $(OWN_CORE)
WEB    := src/web.c src/web_page.c
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -D_DEFAULT_SOURCE $(CPPFLAGS) -DWHATEVERPAD_PROBE=$(WHATEVERPAD_PROBE)

BUILD := build

.PHONY: all test test-attrib fuzz demo ps5 send clean check-generated sanitize ci quick

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
$(BUILD)/test_vpad.o: src/ps5_vpad.c | $(BUILD)
	cc $(HOST_CFLAGS) -D_DARWIN_C_SOURCE -Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym -Dsocket=test_vpad_socket -Dconnect=test_vpad_connect -c -o $@ $<

$(BUILD)/test_vpad_web.o: src/web.c | $(BUILD)
	cc $(HOST_CFLAGS) -Dbind=test_vpad_web_bind -c -o $@ $<

$(BUILD)/test_vpad: $(BUILD)/test_vpad.o $(BUILD)/test_vpad_web.o $(CORE) src/web_page.c tests/test_vpad.c | $(BUILD)
	cc $(HOST_CFLAGS) -Ddlopen=test_vpad_dlopen -Ddlsym=test_vpad_dlsym -o $@ $^

# Configuration, the console hotkey and the local address.
$(BUILD)/test_config: src/config.c src/hotkey.c src/netinfo.c src/log.c src/util.c tests/test_config.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

# Conservative legacy settings migration; fault injection is test-only.
$(BUILD)/test_migrate: src/migrate.c src/config.c src/log.c src/util.c tests/test_migrate.c | $(BUILD)
	cc $(HOST_CFLAGS) -DMIGRATE_TEST -o $@ $^

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

test: $(BUILD)/test_attrib $(BUILD)/test_bond_store $(BUILD)/test_hearbridge $(BUILD)/test_usb $(BUILD)/test_hci_usb $(BUILD)/test_session $(BUILD)/test_lock $(BUILD)/test_config $(BUILD)/test_migrate $(BUILD)/test_vpad $(BUILD)/test_crypto $(BUILD)/test_profiles $(BUILD)/test_host $(BUILD)/test_le $(BUILD)/test_compat $(BUILD)/test_web $(BUILD)/test_launcher
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
	./$(BUILD)/test_migrate

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

quick:
	$(MAKE) check-ownership test-explorer-results
	$(MAKE) test OWN_EXPLORE=0
	$(MAKE) sanitize OWN_EXPLORE=0
	$(MAKE) fuzz check-generated
	$(MAKE) test-attrib BUILD=build/probe WHATEVERPAD_PROBE=1
	$(MAKE) check-mutations OWN_EXPLORE=0
	$(MAKE) check-integration check-integration-mutations

fuzz: | $(BUILD)
	cc $(HOST_CFLAGS) $(SAN) -o $(BUILD)/test_fuzz $(CORE) tests/test_fuzz.c
	./$(BUILD)/test_fuzz
	cc $(HOST_CFLAGS) $(SAN) -o $(BUILD)/test_bond_store_fuzz src/bond_store.c tests/test_bond_store_fuzz.c
	./$(BUILD)/test_bond_store_fuzz
	cc $(HOST_CFLAGS) $(SAN) -o $(BUILD)/test_config_fuzz src/config.c src/hotkey.c src/netinfo.c src/log.c src/util.c tests/test_config.c
	./$(BUILD)/test_config_fuzz --fuzz

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

OWN_HEADERS := src/ownership.h src/session.h
OWN_EXPLORE ?= 1
OWN_EXPLORE_WORKERS ?= 4
OWN_TESTS := $(BUILD)/test_ownership $(BUILD)/test_session_policy
ifeq ($(OWN_EXPLORE),1)
OWN_TESTS += $(BUILD)/test_ownership_explore
endif

.PHONY: test-ownership check-mutations check-ownership sanitize-ownership test-ownership-case sanitize-ownership-case test-explorer-results
test: test-ownership
test-ownership: $(OWN_TESTS)
	./$(BUILD)/test_ownership
ifeq ($(OWN_EXPLORE),1)
	./$(BUILD)/test_ownership_explore
endif
	./$(BUILD)/test_session_policy

sanitize-ownership:
	$(MAKE) test-ownership BUILD=build/san HOST_CFLAGS="$(HOST_CFLAGS) $(SAN)"

test-ownership-case: $(BUILD)/test_ownership_explore
	@./$(BUILD)/test_ownership_explore --policy "$(POLICY)" --case "$(CASE)"

sanitize-ownership-case:
	@$(MAKE) --no-print-directory test-ownership-case BUILD=build/san HOST_CFLAGS="$(HOST_CFLAGS) $(SAN)"

test-explorer-results: $(BUILD)/test_ownership_explore $(BUILD)/mutation-ack_by_opcode
	sh tests/test_explorer_results.sh $(BUILD)/test_ownership_explore $(BUILD)/mutation-ack_by_opcode

$(BUILD)/test_ownership: $(OWN_CORE) tests/test_ownership.c $(OWN_HEADERS) | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c,$^)

$(BUILD)/test_ownership_explore: $(OWN_CORE) tests/test_ownership_explore.c $(OWN_HEADERS) | $(BUILD)
	cc $(HOST_CFLAGS) -O2 -DOWN_EXPLORE_WORKERS=$(OWN_EXPLORE_WORKERS) -o $@ $(filter %.c,$^)

$(BUILD)/mutation-ack_by_opcode: $(OWN_CORE) tests/test_ownership_explore.c $(OWN_HEADERS) | $(BUILD)
	cc $(HOST_CFLAGS) -O2 -DOWN_MUTATION=ack_by_opcode -o $@ $(filter %.c,$^)

$(BUILD)/test_session_policy: src/session.c tests/test_session_policy.c src/session.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c,$^)

check-ownership:
	sh tools/check-ownership.sh

check-mutations: test-ownership
	@set -eu; for id in ack_by_opcode ownership_on_sent resend_missing_ack deadline_is_teardown rearm_undo_duplicate readback_is_lease claims_after_callback deadline_only_idle expired_retry failed_http_deadline retry_without_cleanup; do \
	  case $$id in \
	    ack_by_opcode) test=test_ownership_explore;; \
	    expired_retry|failed_http_deadline|retry_without_cleanup) test=test_session_policy;; \
	    *) test=test_ownership;; \
	  esac; \
	  cc $(HOST_CFLAGS) -O2 -DOWN_MUTATION=$$id $(OWN_CORE) tests/$$test.c -o $(BUILD)/mutation-$$id; \
	  status=0; $(BUILD)/mutation-$$id > $(BUILD)/mutation-$$id.log 2>&1 || status=$$?; \
	  if [ "$$status" -ne 1 ]; then printf '%s survived or failed invalidly (exit %s)\n' "$$id" "$$status"; exit 1; fi; \
	  first=""; while IFS= read -r line; do case $$line in FAIL*) first=$$line; break;; esac; done < $(BUILD)/mutation-$$id.log; \
	  if [ -z "$$first" ]; then printf '%s exited 1 without a test failure\n' "$$id"; exit 1; fi; \
	  printf '%s killed (exit 1): %s\n' "$$id" "$$first"; \
	done

.PHONY: test-integration test-integration-oracle check-integration check-integration-mutations
test: test-integration
ci: check-integration check-integration-mutations
INTEGRATION_SRCS := $(BUILD)/test_hci_usb.o $(BUILD)/test_vpad.o $(BUILD)/test_vpad_web.o $(CORE) src/web_page.c src/usb_desc.c src/evstream.c tests/sim.c tests/sim_le.c tests/test_integration_gen.c tests/sim.h tests/sim_le.h tests/sim_lifetime.h
$(BUILD)/test_integration_gen: $(INTEGRATION_SRCS) | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c %.o,$^)
$(BUILD)/test_integration_oracle: $(INTEGRATION_SRCS) tests/integration_oracle_faults.c | $(BUILD)
	cc $(HOST_CFLAGS) -Wl,--wrap=test_usb_ioctl,--wrap=web_start,--wrap=web_poll,--wrap=vpad_update,--wrap=sim_foreign_command -o $@ $(filter %.c %.o,$^)
test-integration-oracle: $(BUILD)/test_integration_gen $(BUILD)/test_integration_oracle
	sh tests/test_integration_oracle.sh ./$(BUILD)/test_integration_gen ./$(BUILD)/test_integration_oracle
test-integration: $(BUILD)/test_integration_gen test-integration-oracle
	./$(BUILD)/test_integration_gen
check-integration: $(BUILD)/test_integration_gen
	./$(BUILD)/test_integration_gen --full
check-integration-mutations: $(BUILD)/test_integration_gen test-integration-oracle
	@set -eu; for id in refuse-everything drop-input drop-delete foreign-delete duplicate-delete truncate-http leak-usb; do \
	  status=0; ./$(BUILD)/test_integration_gen --mutate $$id > $(BUILD)/integration-$$id.log 2>&1 || status=$$?; \
	  test "$$status" -eq 1 && grep -Eq '^(FAIL|PROPERTY .* FAIL)' $(BUILD)/integration-$$id.log; \
	  printf 'integration mutation %s rejected (exit %s)\n' "$$id" "$$status"; \
	done
