# WhateverPad PS5: Bluetooth game controllers on a jailbroken PS5.
#
#   make test                 builds and runs the tests on this machine
#   make ps5                  builds the payload (needs PS5_PAYLOAD_SDK)
#   make send PS5_HOST=ip     sends it to the console's payload loader

PS5_HOST ?= ps5
PS5_PORT ?= 9021
WHATEVERPAD_PROBE ?= 0
WHATEVERPAD_ATTRIB_POLICY ?= OWN_POLICY_NO_RESTORE
WHATEVERPAD_PROBE_CREDS ?= raised

OWN_CORE := src/ownership.c src/session.c
CORE   := src/util.c src/log.c src/attrib.c src/probe.c src/crc32.c src/smp_crypto.c src/profiles.c src/generic.c src/bond_store.c src/host.c src/le.c $(OWN_CORE)
WEB    := src/web.c src/web_page.c
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g -Isrc -D_DEFAULT_SOURCE $(CPPFLAGS) -DWHATEVERPAD_PROBE=$(WHATEVERPAD_PROBE) -DWHATEVERPAD_ATTRIB_POLICY=$(WHATEVERPAD_ATTRIB_POLICY)
ifeq ($(WHATEVERPAD_PROBE),1)
ifeq ($(filter $(WHATEVERPAD_PROBE_CREDS),raised original),)
$(error WHATEVERPAD_PROBE_CREDS must be raised or original)
endif
HOST_CFLAGS += -DWHATEVERPAD_PROBE_ORIGINAL=$(if $(filter original,$(WHATEVERPAD_PROBE_CREDS)),1,0)
endif

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

$(BUILD)/test_web_product_loop: $(CORE) $(WEB) tests/sim.c tests/test_web_product_loop.c tests/test_web.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_web.c,$^)

test: test-web-product-loop
test-web-product-loop: $(BUILD)/test_web_product_loop
	./$(BUILD)/test_web_product_loop

# The page against a simulated pad, to look at in a browser: make demo
$(BUILD)/web_demo: $(CORE) $(WEB) tests/sim.c tests/web_demo.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^ -lm

demo: $(BUILD)/web_demo
	./$(BUILD)/web_demo

$(BUILD)/test_host: $(CORE) tests/sim.c tests/test_host.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_setup_retry: $(CORE) tests/sim.c tests/test_setup_retry.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

test: test-setup-retry
test-setup-retry: $(BUILD)/test_setup_retry
	./$(BUILD)/test_setup_retry

$(BUILD)/test_setup_wait: $(CORE) tests/sim.c tests/test_setup_wait.c tests/test_setup_retry.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_setup_retry.c,$^)

test: test-setup-wait
test-setup-wait: $(BUILD)/test_setup_wait
	./$(BUILD)/test_setup_wait

$(BUILD)/test_setup_reopen: $(CORE) tests/sim.c tests/test_setup_reopen.c tests/test_setup_retry.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_setup_retry.c,$^)

test: test-setup-reopen
test-setup-reopen: $(BUILD)/test_setup_reopen
	./$(BUILD)/test_setup_reopen

$(BUILD)/test_compat: $(CORE) tests/sim.c tests/test_compat.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_profiles: $(CORE) tests/test_profiles.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_crypto: src/smp_crypto.c tests/test_crypto.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_le: $(CORE) tests/sim_le.c tests/test_le.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_host_transactions: $(CORE) tests/sim.c tests/sim_le.c tests/test_host_transactions.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_runtime_link_callbacks: tests/test_runtime_link_callbacks.c src/ps5_main.c src/host.h | $(BUILD)
	cc $(HOST_CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $<

$(BUILD)/test_settings_ack_interleavings: $(CORE) tests/sim.c tests/test_settings_ack_interleavings.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_le_crypto_owner: $(CORE) tests/test_le_crypto_owner.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $^

$(BUILD)/test_connection_namespace: $(CORE) tests/test_connection_namespace.c tests/test_le_crypto_owner.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(CORE) tests/test_connection_namespace.c

$(BUILD)/test_controller_fingerprints: $(CORE) tests/sim.c tests/test_controller_fingerprints.c tests/test_host.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(CORE) tests/sim.c tests/test_controller_fingerprints.c

test: $(BUILD)/test_host_transactions $(BUILD)/test_runtime_link_callbacks $(BUILD)/test_settings_ack_interleavings $(BUILD)/test_le_crypto_owner $(BUILD)/test_connection_namespace $(BUILD)/test_controller_fingerprints

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

USB_STANDIN := $(BUILD)/test_hci_usb.o tests/usb_standin/receipt.c tests/usb_standin/receipt.h
$(BUILD)/test_hci_usb.o: src/hci_usb.c src/hci_usb.h src/hci.h src/ownership.h src/session.h src/attrib.h src/probe.h tests/usb_standin/dev/usb/usb.h tests/usb_standin/dev/usb/usb_ioctl.h tests/usb_standin/syscalls.h tests/usb_standin/receipt.h | $(BUILD)
	cc $(HOST_CFLAGS) -Itests/usb_standin -include tests/usb_standin/syscalls.h -c -o $@ $<

$(BUILD)/test_hci_usb: $(USB_STANDIN) src/usb_desc.c src/evstream.c src/log.c src/util.c src/attrib.c src/probe.c $(OWN_CORE) tests/test_hci_usb.c tests/usb_standin/dev/usb/usb.h tests/usb_standin/dev/usb/usb_ioctl.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c %.o,$^)

$(BUILD)/test_usb_handle_reassignment: $(USB_STANDIN) src/usb_desc.c src/evstream.c src/log.c src/util.c src/attrib.c src/probe.c $(OWN_CORE) tests/test_usb_handle_reassignment.c tests/test_hci_usb.c | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_hci_usb.c,$(filter %.c %.o,$^))

$(BUILD)/test_attrib: $(CORE) $(USB_STANDIN) src/usb_desc.c src/evstream.c tests/sim.c tests/test_attrib.c tests/test_hci_usb.c src/attrib.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter-out tests/test_hci_usb.c,$(filter %.c %.o,$^))

test-attrib: $(BUILD)/test_attrib
	./$(BUILD)/test_attrib

$(BUILD)/test_probe_main: $(CORE) src/config.c tests/sim.c tests/test_probe_main.c src/ps5_main.c src/probe.h | $(BUILD)
	cc $(HOST_CFLAGS) -DSYS_thr_set_name=0 -o $@ $(filter-out src/ps5_main.c,$(filter %.c,$^))

.PHONY: test-probe-main test-probe
test-probe-main: $(BUILD)/test_probe_main
	./$(BUILD)/test_probe_main

# Separate output directories prevent stale objects from crossing credential modes.
test-probe:
	$(MAKE) test-attrib test-probe-main $(BUILD)/probe-raised/test_vpad $(BUILD)/probe-raised/test_hci_usb BUILD=$(BUILD)/probe-raised WHATEVERPAD_PROBE=1 WHATEVERPAD_PROBE_CREDS=raised
	./$(BUILD)/probe-raised/test_vpad
	./$(BUILD)/probe-raised/test_hci_usb
	$(MAKE) test-attrib test-probe-main $(BUILD)/probe-original/test_vpad $(BUILD)/probe-original/test_hci_usb BUILD=$(BUILD)/probe-original WHATEVERPAD_PROBE=1 WHATEVERPAD_PROBE_CREDS=original
	./$(BUILD)/probe-original/test_vpad
	./$(BUILD)/probe-original/test_hci_usb

test: test-probe-main

$(BUILD)/test_session: $(CORE) tests/sim.c tests/sim_le.c tests/test_session.c tests/sim.h tests/sim_le.h tests/sim_lifetime.h | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c,$^)

test: $(BUILD)/test_attrib $(BUILD)/test_bond_store $(BUILD)/test_hearbridge $(BUILD)/test_usb $(BUILD)/test_hci_usb $(BUILD)/test_usb_handle_reassignment $(BUILD)/test_session $(BUILD)/test_lock $(BUILD)/test_config $(BUILD)/test_migrate $(BUILD)/test_vpad $(BUILD)/test_crypto $(BUILD)/test_profiles $(BUILD)/test_host $(BUILD)/test_le $(BUILD)/test_compat $(BUILD)/test_web $(BUILD)/test_launcher
	./$(BUILD)/test_attrib
	./$(BUILD)/test_bond_store
	./$(BUILD)/test_usb
	./$(BUILD)/test_hci_usb
	./$(BUILD)/test_usb_handle_reassignment
	./$(BUILD)/test_session
	./$(BUILD)/test_lock
	./$(BUILD)/test_config
	./$(BUILD)/test_vpad
	./$(BUILD)/test_crypto
	./$(BUILD)/test_profiles
	./$(BUILD)/test_host
	./$(BUILD)/test_le
	./$(BUILD)/test_host_transactions
	./$(BUILD)/test_runtime_link_callbacks
	./$(BUILD)/test_settings_ack_interleavings
	./$(BUILD)/test_le_crypto_owner
	./$(BUILD)/test_connection_namespace
	./$(BUILD)/test_controller_fingerprints
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
	$(MAKE) test-probe
	$(MAKE) test-probe BUILD=build/probe-san CPPFLAGS="$(SAN)"
	@checks="$$( $(MAKE) -qp | awk -F: '/^check-(ownership|mutations):/ { print $$1 }' )"; \
	  if [ -n "$$checks" ]; then $(MAKE) $$checks; fi

quick:
	$(MAKE) check-ownership test-explorer-results
	$(MAKE) test OWN_EXPLORE=0
	$(MAKE) sanitize OWN_EXPLORE=0
	$(MAKE) fuzz check-generated
	$(MAKE) test-probe
	$(MAKE) test-probe BUILD=build/probe-san CPPFLAGS="$(SAN)"
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
OWN_TESTS += $(BUILD)/test_ownership_explore $(BUILD)/test_explorer_key_storage
endif

.PHONY: test-ownership check-mutations check-ownership check-peer-reply-mutation check-link-generation-mutation sanitize-ownership test-ownership-case sanitize-ownership-case test-explorer-results
test: test-ownership
test-ownership: $(OWN_TESTS)
	./$(BUILD)/test_ownership
ifeq ($(OWN_EXPLORE),1)
	./$(BUILD)/test_explorer_key_storage
	./$(BUILD)/test_ownership_explore
endif
	./$(BUILD)/test_session_policy

sanitize-ownership:
	$(MAKE) test-ownership BUILD=build/san HOST_CFLAGS="$(HOST_CFLAGS) $(SAN)"

test-ownership-case: $(BUILD)/test_ownership_explore $(BUILD)/test_explorer_key_storage
	@./$(BUILD)/test_explorer_key_storage >&2
	@./$(BUILD)/test_ownership_explore --policy "$(POLICY)" --case "$(CASE)"

sanitize-ownership-case:
	@$(MAKE) --no-print-directory test-ownership-case BUILD=build/san HOST_CFLAGS="$(HOST_CFLAGS) $(SAN)"

test-explorer-results: $(BUILD)/test_ownership_explore $(BUILD)/mutation-ack_by_opcode $(BUILD)/test_explorer_key_storage
	./$(BUILD)/test_explorer_key_storage
	sh tests/test_explorer_results.sh $(BUILD)/test_ownership_explore $(BUILD)/mutation-ack_by_opcode

$(BUILD)/test_explorer_key_storage: $(OWN_CORE) tests/test_explorer_key_storage.c tests/test_ownership_explore.c $(OWN_HEADERS) | $(BUILD)
	cc $(HOST_CFLAGS) -O2 -o $@ $(OWN_CORE) tests/test_explorer_key_storage.c

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

$(BUILD)/mutation-reply_without_owned_peer: $(USB_STANDIN) src/usb_desc.c src/evstream.c src/log.c src/util.c src/attrib.c src/probe.c $(OWN_CORE) tests/test_hci_usb.c | $(BUILD)
	cc $(HOST_CFLAGS) -DOWN_MUTATION=reply_without_owned_peer -o $@ $(filter %.c %.o,$^)

check-peer-reply-mutation: $(BUILD)/mutation-reply_without_owned_peer
	@set -eu; status=0; $< --peer-reply > $(BUILD)/mutation-reply_without_owned_peer.log 2>&1 || status=$$?; \
	  test "$$status" -eq 1; grep -q '^FAIL ' $(BUILD)/mutation-reply_without_owned_peer.log; \
	  printf 'reply_without_owned_peer killed (exit %s)\n' "$$status"

$(BUILD)/mutation-drop_link_generation: $(CORE) tests/sim.c tests/sim_le.c tests/test_session.c | $(BUILD)
	cc $(filter-out -DWHATEVERPAD_ATTRIB_POLICY=%,$(HOST_CFLAGS)) -DWHATEVERPAD_ATTRIB_POLICY=OWN_POLICY_SINGLE_PRODUCER -DOWN_MUTATION=drop_link_generation -o $@ $^

check-link-generation-mutation: $(BUILD)/mutation-drop_link_generation
	@set -eu; status=0; $< --handle-reuse > $(BUILD)/mutation-drop_link_generation.log 2>&1 || status=$$?; \
	  test "$$status" -eq 1; grep -Fq 'FAIL ' $(BUILD)/mutation-drop_link_generation.log; \
	  grep -Fq '!own_link_current(host_ownership(h), other_generation)' $(BUILD)/mutation-drop_link_generation.log; \
	  printf 'drop_link_generation killed in lost-terminal handle reuse (exit %s)\n' "$$status"

check-mutations: test-ownership check-peer-reply-mutation check-link-generation-mutation
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
INTEGRATION_SRCS := $(USB_STANDIN) $(BUILD)/test_vpad.o $(BUILD)/test_vpad_web.o $(CORE) src/web_page.c src/usb_desc.c src/evstream.c tests/sim.c tests/sim_le.c tests/test_integration_gen.c tests/sim.h tests/sim_le.h tests/sim_lifetime.h
$(BUILD)/test_web_usb_idle: $(INTEGRATION_SRCS) tests/test_web_usb_idle.c src/ps5_main.c | $(BUILD)
	cc $(HOST_CFLAGS) -DSYS_thr_set_name=0 -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(filter-out tests/test_integration_gen.c src/ps5_main.c,$(filter %.c %.o,$^))

test: test-web-usb-idle
test-web-usb-idle: $(BUILD)/test_web_usb_idle
	./$(BUILD)/test_web_usb_idle

$(BUILD)/test_integration_gen: $(INTEGRATION_SRCS) | $(BUILD)
	cc $(HOST_CFLAGS) -o $@ $(filter %.c %.o,$^)
$(BUILD)/test_integration_oracle: $(INTEGRATION_SRCS) tests/integration_oracle_faults.c | $(BUILD)
	cc $(HOST_CFLAGS) -Wl,--wrap=test_usb_ioctl,--wrap=vpad_update,--wrap=sim_foreign_command -o $@ $(filter %.c %.o,$^)
test-integration-oracle: $(BUILD)/test_integration_gen $(BUILD)/test_integration_oracle
	sh tests/test_integration_oracle.sh ./$(BUILD)/test_integration_gen ./$(BUILD)/test_integration_oracle
test-integration: $(BUILD)/test_integration_gen test-integration-oracle
	./$(BUILD)/test_integration_gen
	$(MAKE) BUILD=$(BUILD)/single-producer WHATEVERPAD_ATTRIB_POLICY=OWN_POLICY_SINGLE_PRODUCER HOST_CFLAGS="$(filter-out -DWHATEVERPAD_ATTRIB_POLICY=%,$(HOST_CFLAGS)) -DWHATEVERPAD_ATTRIB_POLICY=OWN_POLICY_SINGLE_PRODUCER" $(BUILD)/single-producer/test_integration_gen
	./$(BUILD)/single-producer/test_integration_gen
	./$(BUILD)/test_integration_gen --seed 7
	./$(BUILD)/single-producer/test_integration_gen --seed 7
check-integration: $(BUILD)/test_integration_gen
	./$(BUILD)/test_integration_gen --full
check-integration-mutations: $(BUILD)/test_integration_gen test-integration-oracle
	@set -eu; for id in refuse-everything drop-input drop-delete foreign-delete duplicate-delete truncate-http leak-usb; do \
	  status=0; ./$(BUILD)/test_integration_gen --mutate $$id > $(BUILD)/integration-$$id.log 2>&1 || status=$$?; \
	  test "$$status" -eq 1 && grep -Eq '^(FAIL|PROPERTY .* FAIL)' $(BUILD)/integration-$$id.log; \
	  printf 'integration mutation %s rejected (exit %s)\n' "$$id" "$$status"; \
	done
