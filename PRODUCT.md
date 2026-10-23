# WhateverPad PS5: product definition

WhateverPad PS5 is a Bluetooth controller bridge for jailbroken PS5 consoles. It runs
as a resident payload, uses the console's own Bluetooth chip alongside the system, and
presents third-party controllers to the PS5 as virtual DualSense pads, so you can play
with a DualShock 4, an 8BitDo, or another supported pad without a USB dongle.

This file says who the product is for, what it must do, and how to judge it. Code facts
carry a `path:symbol` citation that resolves in this tree. Anything marked
"planned: todo N" does not exist yet; N is a todo in the foundation plan that drives this
fork. That plan is project material, not part of this repository. `docs/architecture.md`
maps the code.

## 1. Identity

| Fact | Value |
| --- | --- |
| Project | `andrescera/WhateverPad`, https://github.com/andrescera/WhateverPad |
| Licence | GPL-3.0-or-later, with the upstream credits kept |
| Lineage | A fork of AnyPad PS5 by elmonomalva0. AnyPad names appear only for provenance, history, migration, or an explicitly named legacy path. |
| Launcher entry | `WHTP00001`, Media row, opens the page in the console browser (`src/launcher.h:LAUNCHER_TITLE_ID`) |
| Page address | http://127.0.0.1:8095/ (`src/launcher.h:LAUNCHER_URL`, `src/web.h:WEB_PORT`) |
| Data root | `/data/whateverpad` (`src/ps5_main.c:STATE_DIR`) |
| Artwork | `assets/icon0.png` and `assets/logo.png`, identical files, kept as they are |
| Version in this tree | `0.9.5-beta` (`src/version.h:WHATEVERPAD_VERSION`) |

## 2. Who it is for

| User | Job | What good looks like |
| --- | --- | --- |
| PS5 player with a third-party Bluetooth controller | Play the console with the pad they already own, including console menus, games, and emulators | Pairing works first time, the pad keeps working through a session, and nothing about it needs a PC |
| Player who also owns a DualSense | Keep the native controller usable while the bridge runs | Native input always comes back; the bridge never holds the radio in a way that leaves the DualSense unable to reconnect |
| Returning AnyPad user | Keep the settings and controller knowledge they already have | Supported settings migrate once, sources are preserved, and every conflict is reported instead of silently overwritten |
| Maintainer and contributors | Add a controller profile, fix a bug, ship a build | One build recipe, deterministic host tests with a simulator, and a module map that explains where a change belongs |
| Local tools and scripts | Read the service state without a browser | Existing `/api/state`, `/api/live`, and `/api/log` routes and fields keep working |
| Reader of a translated page | Understand pairing, stopping, and troubleshooting in their language | The page and the README stay equivalent across the twelve supported languages, including right-to-left Arabic |

## 3. Reference controllers

The compatibility bar is two controllers: a **DualShock 4** and an **8BitDo in Switch
mode** (8BitDo SF30 Pro). A change that makes either of them connect less reliably does
not ship, however good it looks for another pad. The README states the same rule for
contributions (`README.md`: "must not alter how the DualShock 4 and the 8BitDo connect").

Supported ids live in `src/profiles.c:k_profiles`. The DualShock 4 ids are in
`src/profiles.c:ds4_ids`; 8BitDo pads in Switch mode arrive as Switch Pro Controllers
(`src/profiles.c:switch_ids`). Three 8BitDo ids are pinned to the Android button order in
the generic profile's built-in mapping table (`src/generic.c:k_builtin`), and any other
unknown pad is read through its own HID descriptor (`src/generic.c:generic_setup`).

Honesty about coverage: the verified column in `COMPATIBILITY.md` lists a DualShock 4
v2, an 8BitDo SF30 Pro in Switch mode, and a GameSir G7 SE. A DualShock 4 v1, the
8BitDo SN30 Pro for Xbox Cloud Gaming, the 8BitDo Ultimate 2.4G, and several more ids
are supported in code and have not been confirmed on a console. Do not move them into
the verified column without a real console and a log.

The native DualSense is not a product bridge target. The code includes a DualSense
parser and output profile (`src/profiles.c:k_profiles`); that is not a native-device
exclusion guard. Keeping native devices system-managed is the product rule, with
terminal release planned in todo 16.

## 4. Platform facts

- WhateverPad needs a jailbroken PS5 that can already run payloads. It is not a
  jailbreak, and it does not modify games, the firmware, or the jailbreak.
- It shares the console's Bluetooth chip with the system. Bluetooth startup failures
  appear on the page if its listener is running. An occupied port 8095 prevents
  WhateverPad's page from opening: `src/web.c:web_start` logs the bind/listen failure,
  closes and frees the listener, and returns NULL. `src/ps5_main.c:main` reports that
  failure by console notification; its outer loop attempts a later rebind.
- Limits are fixed: four live pads (`src/host.h:HOST_MAX_PADS`) and sixteen remembered
  devices (`src/bond_store.h:BOND_MAX`). A full store logs a capacity error instead of
  evicting another controller (`src/host.c:db_put`).
- Firmware claims stay inside what has been observed. Upstream documents console testing
  on firmware 10.01 (`README.md`, version line) and asks for help testing 13.60.
  WhateverPad on 13.60 is a project goal, not a verified claim, and nothing here claims
  firmware 14.x.

## 5. The native-first rule

A game must never lose native input because the bridge consumed a request meant for the
system. That single rule drives the rest:

- Unowned traffic is the system's. A connection request for a controller WhateverPad does
  not know is left unanswered so the system can take the next attempt
  (`src/host.c:on_event`, case `0x04`). Today it can yield event reads for 30 seconds
  (`src/host_int.h:T_YIELD`), but only outside pairing/setup, with a yield-capable
  transport and when the yield rate limit allows. It does not replay the consumed
  request or release ACL reads; that is the limit tracked as GAP-1.
- Recovery must be bounded and complete. The rule demands that a release ends bridged
  input, drops the resources WhateverPad acquired, and finishes inside a fixed budget.
- Stopping must be predictable. Today page Stop sets a flag and detected rest mode breaks
  the outer loop. Only after that loop exits does a ten-second alarm guard shutdown
  (`src/ps5_main.c:main`, `src/ps5_main.c:T_STOP_MAX_S`); its fallback unlinks the session
  store and exits (`src/ps5_main.c:on_stop_timeout`). This is not a measured request-to-exit
  bound or proof of complete cleanup; the single release budget is planned in todos 4
  and 13 to 15.

## 6. Approved trade-offs

These four decisions were approved with the product and are not open questions. The
"state on this tree" column keeps them honest.

| Trade-off | What it means | State on this tree |
| --- | --- | --- |
| Native-first terminal release ends bridged input but preserves bonds | Under both policies the release ends bridged input and keeps the pairings. Under the default `NO_RESTORE` policy it issues no controller-side undo and no Disconnect: the radio links are left for the controllers or the user to drop, and the page and the log say so. Only a probe-enabled `SINGLE_PRODUCER` policy may issue a guarded Disconnect | Planned: todos 4, 13 to 16. Today an unknown request is left unanswered and event reads may yield for 30 s, subject to section 5's conditions (`src/host.c:on_event`, `src/host_int.h:T_YIELD`) |
| Manual payload relaunch is required after native release and rest | The payload does not resurrect itself | Partly today: rest mode stops the payload and asks for a relaunch (`src/ps5_main.c:main`, `src/ps5_power.c:power_state`) |
| Automatic reconnect means reconnect while the service owns an active transport, not reacquisition after terminal release | The bridge keeps pairing state alive during a run; it does not silently take the radio back later | The inbound call path for a known pad exists (`src/host.c:on_event`); durable reconnect after relaunch is planned: todos 17 to 19 |
| No automatic resident resume and no external-loader integration | WhateverPad starts when a person starts it | Planned: no work in this plan adds either |

Release is stated as amended: OP:62's "disconnects every bridged pad" means the bridged input
ends, not that the radio link is always torn down. Both policies are planned (todos 4 and
13 to 16). `NO_RESTORE` is the default and needs no unmeasured assumption.
`SINGLE_PRODUCER` exists only if the PS5 probe in todo 8 records that verdict; a guarded
Disconnect then also needs a live handle lease and recent accepted input, and any
ambiguity revokes it.

## 7. What the product promises, and what this tree does today

| Capability | Promise | Today | Where |
| --- | --- | --- | --- |
| Pair a controller | Pair, put the pad in pairing mode, choose Quick Play / Guest | Works | `src/host.c:host_pair`, `README.md` |
| Use the pad | Input reaches games, menus, and emulators as a virtual DualSense | Works for the verified pads | `src/ps5_vpad.c:vpad_update`, `src/profiles.c:k_profiles` |
| Reconnect within a run | A known pad that calls in is taken without re-pairing | The inbound Classic path accepts a known pad when a slot is available; that path is not new hardware verification | `src/host.c:on_event` |
| Reconnect after a relaunch | Pair once, then reconnect from power-on | Not yet: the session store is deleted at startup and at stop (`src/ps5_main.c:main`, `src/ps5_main.c:on_stop_timeout`) | Planned: todos 17 to 19 |
| Forget | Remove a controller and its credentials | Removes the local bond, requests a link drop, and may send Classic virtual-cable unplug; it does not confirm remote key erasure. The split into Connect, Disconnect, and Forget is planned: todos 20, 21, 24 | `src/host.c:host_forget` |
| Disconnect without forgetting | Keep credentials, stop automatic reconnects until asked | Not yet | Planned: todos 20, 21, 24 |
| Stop | Always stop from the page before power-off or relaunch | Sets a stop flag; shutdown has the post-loop alarm described in section 5, not a complete-cleanup guarantee | `src/ps5_main.c:main`, `src/web.c:handle` |
| Rest mode | Stop before rest and say that a relaunch is needed | Detected rest state breaks the loop and notifies that relaunch is needed; it uses the same shutdown path and limits | `src/ps5_main.c:main`, `src/ps5_power.c:power_state` |
| Settings | Per-model PS combination; menu hotkey and hold time | The per-model combination works. The `config.ini` parser and the hotkey module exist, but the payload calls neither, so menu hotkey and hold time are not operating settings | `src/config.c:pscombo_set`, `src/config.c:pscombo_load`, `src/config.c:config_load`, `src/hotkey.c:hotkey_update`, `src/ps5_main.c:main` |
| Diagnostics | Input/state view, incremental logs, file log tail, tuning, raw HCI for development | Input uses `/api/state`; `/api/live` returns in-memory log lines with a sequence cursor; `/api/log` tails the configured file. Limits are in section 9 | `src/web.c:api_state`, `src/web.c:api_live`, `src/web.c:api_log`, `src/web.c:api_hci` |

Nothing in this table says that a planned capability exists. When a todo lands, move the
row from "planned" to "works" with a citation and a test, not before.

## 8. Non-goals

- No USB dongle or PC-side bridge: the console's own radio is the transport.
- No second Bluetooth stack, no kernel module, no firmware patch.
- Not a HEN, toolbox, cheat engine, or fan-control utility.
- No Sony proprietary code, and no reimplementation of Sony binaries.
- No cloud service, telemetry, account, or paid dependency.
- No automatic resident resume, no external-loader integration, no self-restart after a
  native release.
- No PS4 port in this repository and no claim for PS5 firmware above the tested ceiling.
- No bond, key, or IRK export through HTTP, and no "authentication" claim for the page.
- No new brand: the artwork is fixed and the interface direction follows it.

## 9. Settings and security boundary

- Today the page listens on port 8095 on every interface (`src/web.h:WEB_PORT`,
  `src/web.c:web_start`). The approved posture is loopback by default, with LAN access as
  an explicit opt-in (planned: todo 21). Either way, treat LAN control as trusted-LAN
  control: it has no authentication and no encryption.
- Mutations require the `X-WhateverPad: 1` header, and requests must carry an IPv4-literal
  or localhost Host header (`src/web.c:handle`, `src/web.c:host_is_literal`).
  That defends against a hostile web page in a browser on the same network; it does not
  stop someone who can talk to the port directly.
- Developer surfaces exist today: `/api/tune` and `/api/hci` can change Bluetooth
  behaviour or send a raw command (`src/web.c:api_hci`,
  `src/host.c:host_raw_cmd`). They are development tools, not user features, and the plan
  closes them behind an opt-in developer mode (planned: todo 21).
- Settings that are live: the per-model PS-button combination in `ps_combos.txt`, loaded at
  startup (`src/config.c:pscombo_load`, `src/ps5_main.c:main`) and set from the page
  (`src/config.c:pscombo_set`). `menu_combo` and `menu_hold_ms` are parsed by
  `src/config.c:config_load`, but the payload never calls that parser, and
  `src/hotkey.c:hotkey_update` is never called either, so no menu hotkey is active in this
  build; the comment in `src/ps5_main.c` that describes one is stale. Wiring the settings
  is planned: todos 7 and 17.
- Migration from AnyPad copies supported configuration only when the destination is
  absent, preserves the originals, and reports conflicts (planned: todos 7 and 17). It
  never imports system bonds, adapter-unbound keys, or a legacy LAN/developer opt-in.

## 10. Honesty rules

These apply to the page copy, the READMEs, the architecture notes, PR bodies, and any
report about this project.

1. Separate what is measured from what is planned. A planned item names its todo; an
   as-built claim names code or a log.
2. Hardware claims need hardware evidence. The simulator proves logic, not that a
   controller pairs on a console.
3. No latency, performance, or reliability claim without recorded samples from the exact
   build being discussed.
4. No claim outside the observed firmware and the reference controllers. "Supported in
   code" and "verified on a console" are different sentences.
5. Public support artifacts carry no secrets. Redact keys, IRKs, user ids, and controller
   addresses beyond what a pairing report needs.
6. Every public claim is derived from the exact tree being shipped, with the command that
   produced it recorded.
7. GPL-3.0-or-later, the upstream credit, and the fork lineage stay visible. A rename
   never erases AnyPad.
8. No AI attribution in commits, PRs, or copy, and no assistant framing in user
   documentation.

## 11. Interface direction

The console browser is the primary surface. The page is an operating tool, not a
marketing page: compact, code-led, and legible in a small square window. Keep the current
artwork and follow its palette.

Colour tokens were sampled from `assets/icon0.png` (512x512, paletted) by counting every
pixel, not by eye. The six tokens below are exact palette entries, with their share of the
262,144 pixels.

| Token | Value | Share of the icon | Use |
| --- | --- | --- | --- |
| `--wp-burgundy` | `#650107` | 158,421 px (60.43%) | Brand surface and page background |
| `--wp-burgundy-deep` | `#5C0105` | 18,553 px (7.08%) | Raised surface, cards, bars |
| `--wp-gold` | `#F8B627` | 24,819 px (9.47%) | Primary action, focus ring, active item |
| `--wp-teal` | `#25A38D` | 17,720 px (6.76%) | Connected and success states |
| `--wp-cream` | `#F2E5C6` | 1,334 px (0.51%) | Body text on dark surfaces |
| `--wp-ink` | `#17120E` | 5,735 px (2.19%) | Text and icons on gold or teal |

Contrast was computed with the WCAG 2.x relative luminance formula. The sampling script,
its output, and the full contrast table are recorded with the plan evidence for this
change.

| Pair | Ratio | Requirement | Verdict |
| --- | --- | --- | --- |
| Cream on burgundy | 10.76:1 | Body text 4.5:1 | Pass |
| Cream on burgundy-deep | 11.49:1 | Body text 4.5:1 | Pass |
| Ink on gold | 10.37:1 | Body text 4.5:1 | Pass |
| Gold on burgundy | 7.50:1 | Large text and focus 3:1 | Pass |
| Gold on burgundy-deep | 8.01:1 | Focus ring 3:1 | Pass |
| Teal on burgundy | 4.29:1 | Large text and non-text indicators 3:1 | Pass for large text and non-text only |
| Ink on teal | 5.93:1 | Body text 4.5:1 | Pass |
| Cream on ink | 14.88:1 | Body text 4.5:1 | Pass |
| Gold on ink | 10.37:1 | Focus ring 3:1 | Pass |

Teal on burgundy sits at 4.29:1, under the 4.5:1 threshold for normal text, so teal is not
approved for small labels or paragraphs. Use it for large status words, icons, and
non-text indicators; a small label sits in ink on a teal fill, at 5.93:1.

Layout and interaction rules, as approved:

- Compact top navigation replaces the permanently wide sidebar. The current page uses a
  236 px sidebar (`web/index.html`: `.shell` grid), which the console-first shell replaces
  (planned: todo 23).
- Views: Controllers, Settings, Help, Diagnostics.
- The first viewport answers four questions: is the service up, why not if it is down,
  what can I press to pair, and which controllers are connected or remembered.
- Input probes and animated visualisers are secondary, not the landing view.
- Stable DOM nodes keyed by device id, visible focus, keyboard and controller activation,
  a dialog focus trap and restore, predictable cancel and back behaviour, inline
  operation errors, reduced motion, and right-to-left layouts.
- No dependence on the Gamepad API: native focus, keyboard, and pointer input must work.
- Local or system fonts only, no CDN, no remote asset.
- Test geometries until the real console viewport is measured: 640x640 and 800x800
  squares, 960x540 landscape, 390x844 phone, 1440x900 desktop. These are test sizes, not
  claims about the console browser.

`DESIGN.md` is written after the interface shell is built (planned: todo 25). This section
is the direction that shell must satisfy, not a description of a page that exists.

## 12. Limits of this document

- Everything in section 7 marked "planned" is absent from this tree. Do not quote it as
  current behaviour.
- Colour values are stable because they come from fixed artwork. If the artwork changes,
  resample and recheck contrast before changing this table.
- This file does not replace `README.md` (user instructions, twelve languages),
  `COMPATIBILITY.md` (per-controller status), or `CHANGELOG.md` (history).
