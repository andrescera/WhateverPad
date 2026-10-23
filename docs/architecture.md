# WhateverPad PS5 architecture

How the payload in this tree is put together, and where the planned ownership work
will fit. Two conventions:

- `path:symbol` means: the symbol is declared or defined in that file. Every citation in
  the as-built sections was grepped on this tree; the table with the commands is recorded
  with the plan evidence for this change.
- "planned: todo N" means the code does not exist in this tree yet. N is a todo in the
  foundation plan that drives this fork, which is project material and not part of this
  repository. Do not read a planned section as a description of today's behaviour.

`PRODUCT.md` holds the users, jobs, rules, and interface direction. This file holds the
code map.

## 1. Runtime shape

One C11 payload, one thread, one event loop (`src/ps5_main.c:main`). The payload:

- takes a lock so only one instance runs (`src/lock.c:lock_take`);
- opens the console's Bluetooth chip over USB (`src/hci_usb.c:hci_usb_open`);
- runs the Classic host and the LE host over one HCI transport abstraction
  (`src/hci.h:hci_ops`);
- prepares the virtual-pad layer that creates and feeds DualSense pads
  (`src/ps5_vpad.c:vpad_init`);
- serves one page and a small HTTP API on port 8095 (`src/web.c:web_start`);
- stops on request, on SIGTERM/SIGINT/SIGHUP, or when rest mode starts
  (`src/ps5_main.c:on_signal`, `src/ps5_power.c:power_state`).

There is no framework, no thread pool, and no coroutine layer. Blocking work is done in
bounded waits inside the loop.

## 2. Boot, loop, and shutdown

`src/ps5_main.c:main` is the whole outer lifecycle:

1. Create `/data/whateverpad`, open the log, read the console language
   (`src/ps5_main.c:STATE_DIR`, `src/log.c:log_open`).
2. Take the instance lock and clear a stale stop flag (`src/ps5_main.c:main`).
3. Delete the session bond file, because this build keeps pads for one run only
   (`src/ps5_main.c:main`; see section 12).
4. Start the page before touching Bluetooth, so the page can explain a later failure
   (`src/web.c:web_start`).
5. Install or remove the Media row entry (`src/ps5_apps.c:apps_install_launcher`,
   `src/launcher.c:launcher_write`).
6. Raise credentials and create the virtual-pad layer (`src/ps5_vpad.c:vpad_init`). If
   that fails, Bluetooth is never opened and the page says why
   (`src/ps5_main.c:main`).
7. Loop until stop: poll the host, poll the page, poll the virtual pads, re-open the page
   if the console address changed, notice rest mode, retry Bluetooth on request, and
   handle the `pair`/`stop` flag files (`src/host.c:host_poll`, `src/web.c:web_poll`,
   `src/ps5_vpad.c:vpad_poll`, `src/ps5_main.c:main`).
8. Shut down behind a ten-second alarm: stop the page, close the host, delete the session
   bond file, restore credentials, release the lock
   (`src/ps5_main.c:T_STOP_MAX_S`, `src/ps5_main.c:on_signal`,
   `src/ps5_vpad.c:vpad_creds_restore`). If the alarm fires first,
   `src/ps5_main.c:on_stop_timeout` deletes the bond file and exits.

## 3. Bluetooth transport

The host stack never talks to USB directly. It sees `hci_ops` (`src/hci.h:hci_ops`):
send a command, send an ACL packet, pump, pop an event, pop an ACL packet, close, plus
optional operations for diagnostics, yielding events to the system, read quotas, and chip
identification. That seam is what lets the same protocol code run against the real chip
and against the simulator.

- `src/hci_usb.c:hci_usb_open` binds the chip's USB interface and returns an `hci_t`.
- `src/usb_desc.c:usb_find_hci_functions` locates the HCI event and ACL endpoints in the
  USB configuration.
- `src/evstream.c:evstream_feed` reassembles HCI event stream packets across short USB
  reads.
- `src/hci_usb.c` keeps the pending event-read pool and the optional `yield` operation
  used when a foreign controller pages the console.

## 4. Classic host

`src/host.c` owns Bluetooth Classic: inquiry, paging, accept or reject of connection
requests, SDP, L2CAP channels, HID, authentication, and link lifetime. The main symbols:

| Concern | Symbol |
| --- | --- |
| Host lifetime | `src/host.c:host_open`, `src/host.c:host_poll` |
| Command submission | `src/host.c:send_cmd` (single submit path) |
| Event decoding | `src/host.c:on_event` |
| Pairing | `src/host.c:host_pair`, `src/host.c:pair_with` |
| SDP | `src/host.c:sdp_start`, `src/host.c:on_sdp` |
| L2CAP | `src/host.c:l2_send`, `src/host.c:chan_connect` |
| Input and readiness | `src/host.c:link_input`, `src/host.c:link_ready` |
| Bonds | `src/host.c:db_find`, `src/host.c:db_save`, `src/host.c:db_forget` |
| Forget and output | `src/host.c:host_forget`, `src/host.c:host_set_output` |
| Development surfaces | `src/host.c:host_tune_set`, `src/host.c:host_raw_cmd` |

The per-link state lives in `src/host_int.h:link_t`, inside the host structure defined in
the same file. The caller-facing contract is `src/host.h:host_events` and the `host_*`
functions in `src/host.h`.

One behaviour matters for the native-first rule: a connection request for a controller
WhateverPad does not know leaves the call to the system and then yields event reads for
30 seconds (`src/host.c:on_event`, case `0x04`; `src/host_int.h:T_YIELD`). ACL reads keep
running. That yield is the limit tracked as GAP-1.

## 5. LE host

`src/le.c` owns Bluetooth LE: scanning, connection to HID-over-GATT peripherals, SMP
pairing with Secure Connections, identity and key distribution, and LE input reports.

| Concern | Symbol |
| --- | --- |
| Setup and teardown | `src/le.c:le_setup`, `src/le.c:le_shutdown` |
| Per-loop work | `src/le.c:le_poll` |
| LE Meta events | `src/le.c:le_on_meta` (called from `src/host.c:on_event`, case `0x3E`) |
| Reports to the pad | `src/le.c:le_send_report` |
| Crypto primitives | `src/smp_crypto.c:smp_c1`, `src/smp_crypto.c:smp_f5` |

## 6. Controller profiles and input

Input is normalised into `src/pad.h:pad_state` and output into `src/pad.h:pad_output`.
A profile turns one controller family's bytes into those structures and back.

- `src/profiles.c:k_profiles` is the family table: DualShock 4, DualSense, Xbox, Switch
  Pro, Joy-Con, Wii. `src/profiles.c:profile_find` matches a vid/pid pair.
- DualShock 4 parsing and output: `src/profiles.c:ds4_parse`, `src/profiles.c:ds4_output`.
  The 8BitDo in Switch mode arrives through the Switch Pro profile, because that is the
  protocol it speaks.
- A pad with no matching profile is read from its own HID descriptor by
  `src/generic.c:generic_setup`, with built-in mappings for layouts that differ from the
  default (`src/generic.c:k_builtin`) and an optional per-device map file. The format and
  an example are in `maps/EXAMPLE.map`.
- A profile can carry an init or wake sequence and an output writer, which is why the
  table has more fields than parse and encode. Profiles are selected once per link, not
  per report.

Adding a family means a new row in `src/profiles.c:k_profiles`, its parse and output
functions, ids, and a simulator test in `tests/test_profiles.c` or `tests/test_compat.c`.

## 7. Virtual pad and console integration

The console receives a virtual DualSense, so the real pad never has to be a Sony device.
WhateverPad builds that device itself:

- `src/ps5_vpad.c:vpad_init` raises credentials, loads the needed system libraries, and
  reports whether virtual pads are possible at all.
- `src/ps5_vpad.c:vpad_add` asks for a pad for a slot. Creation and identification happen
  without blocking the loop; `src/ps5_vpad.c:vpad_poll` advances that work and
  `src/ps5_vpad.c:vpad_live` reports when the pad is usable.
- `src/ps5_vpad.c:vpad_update` feeds pad state, `src/ps5_vpad.c:vpad_press_ps` taps PS for
  the page's button, and `src/ps5_vpad.c:vpad_remove` deletes the pad.
- `src/ps5_vpad.c:vpad_creds_restore` puts the process credentials back on shutdown.

Around it: `src/ps5_ui.c:ui_open_browser` opens the page,
`src/ps5_apps.c:apps_install_launcher` registers the Media row entry,
`src/launcher.c:launcher_write` writes its `param.json` and icon, and
`src/ps5_sysinfo.c:sysinfo_log` records the console model and software version.
`src/hotkey.c:hotkey_update` implements menu-combination detection, but the payload never
calls it, and `src/ps5_main.c:main` states that this build has no console hotkey.

## 8. Configuration, bonds, and files

Everything the payload persists lives under `/data/whateverpad`
(`src/ps5_main.c:STATE_DIR`):

| File | Written by | Notes |
| --- | --- | --- |
| `whateverpad.log` | `src/log.c:log_line` | Tail served at `/api/log` through `src/log.c:log_since` |
| `config.ini` | `src/config.c:config_load` | Parser for `menu_combo` and `menu_hold_ms`; no production code calls it, so these are not operating settings in this build |
| `ps_combos.txt` | `src/config.c:pscombo_set` | One PS-button combination per controller model; loaded at startup with `src/config.c:pscombo_load` and set from the page |
| `pads.db` | `src/bond_store.c:bond_store_save` | Versioned, adapter-bound bond store; deleted at startup and stop by design in this build |
| `whateverpad.lock` | `src/lock.c:lock_take` | Single-instance guard |
| `pair`, `stop`, `no_icon`, `remove_icon` | `src/ps5_main.c:check_flags` and `main` | Small request files used by tooling and the launcher flow |
| `maps/` | user | Per-device generic mappings, format in `maps/EXAMPLE.map` |

The bond store is a real module even though this build discards the file each run:
fixed-size records (`src/bond_store.h:BOND_MAX`), a 64-byte header, encode and decode
checked against the adapter identity, and atomic persistence through temporary file,
fsync, rename, and directory fsync, with a fault-injection seam for tests
(`src/bond_store.h:bond_store_test_fault`). It stores no checksum: integrity rests on the
fixed layout, the adapter match, and the atomic replace. The one CRC in the payload seals
controller output reports instead (`src/crc32.c:crc32_seal_output`, used by the Sony
profile encoders such as `src/profiles.c:ds4_output`).

## 9. Page and HTTP API

One page, embedded in the payload, and a small JSON API.

- `web/index.html` is the source. `src/web_page.c:web_page_html` is generated from it by a
  Makefile rule that pipes the HTML through `xxd -i`. `src/icon_data.c:icon_png_data` is
  generated the same way from `assets/icon0.png`.
- `src/web.c:web_start` binds port 8095 on all interfaces and serves the embedded page.
  `src/web.c:web_poll` accepts and completes requests. The page is re-served if the
  console address changes or the listener dies (`src/ps5_main.c:main`,
  `src/netinfo.c:local_ip`).
- Routes are matched in `src/web.c:handle`: `GET /api/state`, `GET /api/live`,
  `GET /api/log`, `GET /api/tune`, `POST /api/tune`, `POST /api/hci`, `POST /api/pair`,
  `POST /api/forget`, `POST /api/ps`, `POST /api/pscombo`, `POST /api/retry`,
  `POST /api/stop`.
- `src/web.c:api_state` serialises the snapshot the page renders, `src/web.c:api_live`
  serves the live input view, `src/web.c:api_log` tails the log, and `src/web.c:api_hci`
  exposes one raw command for development.
- Every mutation needs the `X-WhateverPad: 1` header and a literal Host header
  (`src/web.c:handle`, `src/web.c:host_is_literal`). This is CSRF defence for a hostile
  page in a browser on the same network, not authentication.
- `src/web.h:web_cfg` is the channel through which the page reaches the rest of the
  payload: host pointer, stop and retry flags, Bluetooth state and reason, log path,
  version, language, chip name, PS-button callbacks, and a physical-pad reader.

The current page is a desktop dashboard: a 236 px sidebar and a radar pairing box
(`web/index.html`, `.shell` and `.radar` rules, views `#v-home`, `#v-padsv`, `#v-guide`,
`#v-sys`). The console-first direction that replaces it is in `PRODUCT.md` section 11;
the shell itself is planned: todo 23, with flows in todo 24 and `DESIGN.md` in todo 25.

## 10. Tests and build

Host tests build the payload's real C sources against simulated transports, so the tests
exercise production code rather than a copy.

| Seam | File | Purpose |
| --- | --- | --- |
| Classic simulator | `tests/sim.c:sim_new` | HCI chip plus a controller: inquiry, SDP, L2CAP, HID, reports, refusals |
| LE simulator | `tests/sim_le.c:sim_le_new` | Advertisements, connection, SMP, GATT, delayed and injected events |
| Resource counters | `tests/sim_lifetime.h:sim_lifetime` | Opens, stops, uninits, closes, and reads |
| USB stand-in | `tests/usb_standin/dev/usb/usb.h` | Real `hci_usb.c` against a fake ioctl surface |
| Expected failures | `tests/test_session.c:expected_failure` | Regression cases that must keep failing until their fix lands |

`Makefile:test` builds and runs the binaries: bond store, HearBridge, USB descriptor,
USB transport, session, lock, config, virtual pad, crypto, profiles, host, LE,
compatibility, web, and launcher. `Makefile:fuzz` runs the fuzz targets. The console
payload is built by `ps5.mk`, from one `SRCS` list
(`ps5.mk:SRCS`), either with the SDK toolchain or through `tools/build-elf-docker.sh`.
Today `tools/build-elf-docker.sh` keeps its own compile loop, which duplicates `SRCS`;
one recipe plus `make sanitize`, `make check-generated`, and CI is planned: todo 5.

## 11. Module map

| Area | Module | Responsibility | Citations |
| --- | --- | --- | --- |
| Entry and lifecycle | `src/ps5_main.c` | Outer loop, signals, flags, boot and shutdown order | `src/ps5_main.c:main`, `src/ps5_main.c:on_signal` |
| Transport | `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | USB chip access, endpoint discovery, event reassembly | `src/hci_usb.c:hci_usb_open`, `src/usb_desc.c:usb_find_hci_functions`, `src/evstream.c:evstream_feed` |
| Transport contract | `src/hci.h` | Operations the protocol code is allowed to use | `src/hci.h:hci_ops` |
| Classic | `src/host.c`, `src/host.h`, `src/host_int.h` | Inquiry, pairing, SDP, L2CAP, HID, links, bonds access | `src/host.c:host_poll`, `src/host.c:on_event`, `src/host.c:send_cmd` |
| LE | `src/le.c`, `src/smp_crypto.c` | LE scan, connection, SMP, GATT HID | `src/le.c:le_on_meta`, `src/smp_crypto.c:smp_f5` |
| Profiles | `src/profiles.c`, `src/generic.c`, `src/pad.h`, `src/crc32.c` | Controller families, descriptor-driven fallback, output-report CRC sealing | `src/profiles.c:k_profiles`, `src/generic.c:generic_setup`, `src/pad.h:pad_state`, `src/profiles.c:ds5_output` |
| Virtual pad | `src/ps5_vpad.c`, `src/ps5_vpad.h` | Virtual DualSense creation, identification, input, removal | `src/ps5_vpad.c:vpad_poll`, `src/ps5_vpad.c:vpad_update` |
| Console glue | `src/ps5_ui.c`, `src/ps5_apps.c`, `src/launcher.c`, `src/hotkey.c`, `src/ps5_power.c`, `src/ps5_sysinfo.c` | Browser, launcher entry, menu-combination module (not wired), rest mode, info log | `src/launcher.h:LAUNCHER_TITLE_ID`, `src/ps5_power.c:power_state`, `src/hotkey.c:hotkey_update` |
| Settings | `src/config.c` | `config.ini` parser (not wired) and per-model PS combinations (loaded at startup) | `src/config.c:config_load`, `src/config.c:pscombo_load` |
| Bonds | `src/bond_store.c` | Versioned credential store with adapter validation and atomic save | `src/bond_store.c:bond_store_save` |
| Page and API | `src/web.c`, `src/web.h`, `web/index.html`, `src/web_page.c`, `src/icon_data.c` | HTTP, routes, embedded page and icon | `src/web.c:handle`, `src/web.c:api_state` |
| Support | `src/log.c`, `src/lock.c`, `src/netinfo.c`, `src/util.c`, `src/version.h` | Log file with rotation and a live line buffer, lock, local address, helpers, version | `src/log.c:log_since`, `src/netinfo.c:local_ip` |
| Tests and build | `tests/`, `Makefile`, `ps5.mk`, `tools/build-elf-docker.sh` | Simulators, binaries, payload recipe | `tests/sim.c:sim_new`, `tests/sim_le.c:sim_le_new`, `ps5.mk:SRCS` |

## 12. As-built limits that shape the design

These are properties of this tree, not opinions. Each one is the gap a later todo closes.

- Pairing does not survive a relaunch. The store is unlinked at startup and at stop
  (`src/ps5_main.c:main`, `src/ps5_main.c:on_signal`). Reconnect is planned: todos 17 to 19.
- A connection request for an unknown controller costs 30 seconds of event yield
  (`src/host.c:on_event`, `src/host_int.h:T_YIELD`). Native-first terminal release is
  planned: todos 4, 13 to 16.
- Forget removes the bond and drops the link in one action
  (`src/host.c:host_forget`). Separate Connect, Disconnect, and Forget are planned:
  todos 20, 21, 24.
- Cleanup authority is spread over independent flags in `host.c`, `le.c`, and
  `ps5_vpad.c`, and some lifecycle cases are still registered as expected failures
  (`tests/test_session.c:expected_failure`). This is the bug class the ledger replaces
  (section 13).
- `config.ini` and the menu hotkey are not wired. `src/config.c:config_load` and
  `src/hotkey.c:hotkey_update` have no production caller, and `src/ps5_main.c` states there
  is no console hotkey; only `ps_combos.txt` is loaded (`src/config.c:pscombo_load`).
  Wiring the settings is planned: todos 7 and 17.
- There is no session module and no ownership module in this tree. Their absence is the
  point of the next section.
- The page is a desktop dashboard with a 236 px sidebar (`web/index.html`), not the
  console-first shell in `PRODUCT.md`.
- The Docker helper and `ps5.mk` carry separate source lists
  (`ps5.mk:SRCS`, `tools/build-elf-docker.sh`), so the two builds can drift. Todo 5 fixes it.

## 13. Planned: the ownership ledger (todos 4, 13 to 15)

**Status: planned: todos 4, 13-15. None of this exists in this tree.** It is summarised
here so the module map has a target, from the two design records written for this
change (module boundaries, and the ownership detail with its state machines and
invariants), with amendments A1 to A8 in the foundation plan.

The problem it solves: four independent gate reviews each found a new way for cleanup to
be armed by something that did not prove ownership. The recurring shape is a local flag
set on an attempt, a submission, or an acknowledgement that carried only an opcode. The
ledger makes authority a data property instead of a convention.

### Modules

| Module | Status | Responsibility |
| --- | --- | --- |
| `src/ownership.c/.h` | planned: todo 4 | Pure state machine: canonical records, transitions, correlation and ambiguity decisions, single-use tokens, cleanup eligibility, retirement. No I/O, no clock reads, no HTTP, no callbacks |
| `src/session.c/.h` | planned: todo 4 | Runtime phase, first release reason, cleanup in progress, immutable deadline, retry eligibility. Never acquisition proof |
| `src/attrib.c/.h` | planned: todo 3 | Counts every HCI acknowledgement as solicited, duplicate, late, malformed, unmatched, or NOP; metadata only, never parameter bytes |
| `src/host.c`, `src/le.c`, `src/hci_usb.c`, `src/ps5_vpad.c`, `src/web.c`, `src/ps5_main.c` | migrate in todos 13 to 15 | Keep protocol calibration, transport, HTTP, and disposal; stop holding parallel cleanup-authority flags |

### Rules the ledger enforces

- A record becomes acknowledged only from a receipt that proves its own invocation. An
  opcode match is an observation, not ownership. Readback equality is never a lease.
- A replacement gains authority only from its own acknowledgement plus a continuing
  lease. The predecessor's acknowledgement grants it nothing, while the original value is
  kept for restoration.
- Forward writes may be issued without knowing who will acknowledge them. A restore needs
  that record's own acknowledgement, its original value, and a valid lease.
- The ledger never submits Inquiry Cancel, Create Connection Cancel, LE Create Connection
  Cancel, or Reset from any path, including raw developer routes.
- At most one unacknowledged local command at a time. A missing acknowledgement
  quarantines the record; there is no retransmission.
- Cleanup is derived: callers ask the ledger for eligible effects and cannot build one by
  supplying an opcode and a reason.
- Teardown is one immutable deadline with a 250 ms finalization reserve. Nothing extends
  it.
- Two attribution policies. `NO_RESTORE` is the default: no controller-side undo at all,
  links are left for the controllers or the user to drop, and the page says so. It relies
  on no unmeasured assumption. `SINGLE_PRODUCER` exists only if the hardware probe in
  todo 8 records that verdict, and then restore leases need a fresh-value barrier behind
  the write's acknowledgement. Any ambiguity event revokes every lease and latches
  terminal release with reason `ambiguous`.
- Link ownership needs an acknowledged Create or Accept, a barrier, a successful terminal
  event for the expected peer, and a live handle lease. A Disconnect additionally needs
  recent accepted input on that handle, and is issued only under `SINGLE_PRODUCER`.
- Invariants I1 to I15 from the detail design are checked in tests, not asserted in prose.

### How it is verified

- Permanent effect-level reproducers for every historical failure, ported before any fix
  (todo 2).
- A bounded exhaustive explorer over the production transition code at depth 12, plus
  mutation checks (todo 4), and a seeded integration generator with an independent oracle
  (todo 12).
- Mutation checks: every deliberate mutation of the transition rules must make the suite
  fail (todo 4).
- A static ownership check that fails when record internals are used outside
  `src/ownership.c`, or when a non-backend file submits a forbidden command (todo 4).
- The console probe that chooses the policy, bound to firmware, chip ids, and rule set
  version (todo 8).

## 14. Planned: adjacent lanes

Short pointers, all absent from this tree.

- Settings and migration: `src/config.c` grows the four approved booleans and
  `src/migrate.c` copies supported AnyPad files without overwriting (planned: todo 7).
  Startup wiring is todo 17.
- Durable bonds and startup order (planned: todo 17). Reconnect for Classic (18) and LE
  identities (19). Device actions (20) and the web contract that exposes them (21, 22).
- Console-first shell, flows, locales, and browser coverage (planned: todos 23 to 25).
- Build plumbing: one recipe, `check-generated`, `sanitize`, CI (planned: todo 5).
- Documentation trim and community files (planned: todos 26, 27, 29).

## 15. Citation and maintenance rules

- Cite a symbol, not a line number. Symbols survive edits; line numbers rot. When a
  symbol is renamed, update its citations in the same commit.
- A citation in a section labelled as-built must grep on the tree. A citation in a
  planned section is a placeholder and must name its todo.
- When a todo lands, move its rows out of the planned sections, add the tests that prove
  them, and delete the "planned" label. Never leave both.
- `PRODUCT.md` owns product rules and the colour tokens. This file owns module
  boundaries. `DESIGN.md` (planned: todo 25) owns the finished interface specification.
