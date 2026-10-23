# Contributing to WhateverPad PS5

WhateverPad is a fork of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by elmonomalva0, under GPL-3.0-or-later ([LICENSE](LICENSE)). Issues and controller reports for this fork go to [this repository's issue tracker](https://github.com/andrescera/WhateverPad/issues).

Two rules come before anything else: `make test` must pass, and a change must not alter how the DualShock 4 and the 8BitDo connect. They are the reference.

## Building

You need the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                              # host tests, with the Bluetooth simulator
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk      # the ELF in dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk     # the same, inside a container
```

## Talking to a running WhateverPad

WhateverPad serves its page and its API on port 8095 of the console, and shows the address in its start-up notification. `tools/live.sh` wraps the API:

```
export WHATEVERPAD_IP=192.168.x.y        # the console
tools/live.sh state                      # JSON: version, chip, Bluetooth state, pads
tools/live.sh watch                      # the log in real time, saved in ./logs/
tools/live.sh settings                   # the settings that can be changed live
tools/live.sh set <name> <value>         # change one
tools/live.sh hci <op> [hex]             # one HCI command (never 0C03)
tools/live.sh pair                       # press "Pair" (60 s)
tools/live.sh stop                       # stop WhateverPad
```

The log file is `/data/whateverpad/whateverpad.log`, and an empty file `/data/whateverpad/stop` stops the payload. Through `curl`, every change needs the header `X-WhateverPad: 1` and an IP address as `Host`.

## Rules

1. **Never send an HCI Reset (opcode `0C03`) and never restart the Bluetooth chip.** The chip is shared with the console's own driver, and resetting it breaks the system's Bluetooth, including the DualSense. The page refuses `0C03`; do not look for another way.
2. **Do not change how the DualShock 4 and the 8BitDo connect.** New code goes in a new profile, or in a path chosen by the controller's vendor and product id, never in the common search, call and link flow.
3. **One change per version, tested before the next.** If something gets worse, go back to the version that worked.
4. **Do not switch off the DualSense while WhateverPad runs.** It can stay blinking blue until the console is restarted.
5. **Only your own console and your own network.** Never scan or connect to other people's devices.
6. **Review a log before sharing it.** It holds the Bluetooth addresses of the controllers and the id of the console user. Never share `/data/whateverpad/pads.db`: it holds link keys.
7. **The page has no PIN.** Anyone on the network can read the log and send commands while WhateverPad runs. Stop WhateverPad when you finish.

## Reading the log

Every line starts with the milliseconds since WhateverPad started. The first line of a run is the version and the build, so check it is the version you meant to test. Then, in a normal pairing:

| Line | Meaning |
| --- | --- |
| `chip: MediaTek (0e8d:3603)` or `Marvell/NXP (1286:2059)` | The console's Bluetooth chip. Only the first is verified on a real console |
| `chip: page scan activity: ...` | How much the system listens for incoming calls; WhateverPad lightens it while pairing |
| `inquiry: AA:BB:.. class 002508 eir name="..."` | A device answered the search |
| `paired, key stored`, `link encrypted` | The link is secure |
| `vendor 054c product 09cc`, `has no profile, reading its HID descriptor` | The controller was identified; without a profile it goes to the generic one |
| `channel 0x11 open`, `channel 0x13 open` | The HID channels |
| `ready as <profile> in slot N` | Linked, with its profile chosen |
| `first input (report 0x.., N bytes): virtual DualSense created` | It sent data, so the console now sees a controller |
| `not in its full report yet; asking again every 3 s (reports seen: ...)` | The controller stays in its simple mode; the reports seen tell what it sends |
| `incoming connection from ...: not ours, left to the system` | Another device is calling the console; WhateverPad steps aside |

A controller switched on normally calls the console, and WhateverPad steps aside so the system can answer. Searching and pairing a controller in pairing mode is the path that works every time.

## How the code is organised

`src/host.c` handles Bluetooth Classic (search, calls, pairing, L2CAP, SDP, HID), `src/le.c` handles Bluetooth LE, `src/hci_usb.c` talks HCI over USB to the shared chip, and `src/ps5_vpad.c` creates and feeds the virtual DualSense. `src/web.c` and `web/index.html` are the page and its API; `src/ps5_main.c` is the payload's main loop. `src/profiles.c` holds one profile per family of controllers, `src/generic.c` reads any other HID gamepad, and `maps/` holds the optional mapping files. `tests/sim.c` and `tests/sim_le.c` simulate a controller and the chip, so a change can be checked on a computer.

## Adding a controller

1. **Get a log** of the pairing with `tools/live.sh watch`, and note the vendor and product id.
2. **Try the generic profile first.** A line like `has no profile, reading its HID descriptor` means the controller may already work. A wrong button or axis is fixed by a mapping file in `/data/whateverpad/maps/VVVV_PPPP.map`, with `maps/EXAMPLE.map` as the model.
3. **If it needs its own profile** (a wake-up sequence, its own report layout, rumble or lights), add an entry in `src/profiles.c`: an id table, a `parse` function, and, when needed, `init_report` (wake-up steps), `build_output` (rumble and lights) and `needs_full`. `src/profiles.h` documents each field. Read a similar family first (`switch_*`, `ds4_*`, `xbox_*`).
4. **Teach the simulator** how the controller behaves (`tests/sim.h`, `sim_pad`: its reports, whether it needs waking, its PIN) and add a test in `tests/test_compat.c`. The test must fail without your change.
5. **Run `make test`.** Compare the `METRIC kind=... hash=...` lines printed for the DualShock 4 and the Switch Pro before and after: they must be identical. If they differ, your change touched the common flow; move it into the new profile.
6. **Build the ELF, load it on the console and test with the real controller**, watching the log.
7. **Report it** in [an issue](https://github.com/andrescera/WhateverPad/issues), using the controller template: brand and model, vendor and product id, console model and firmware, jailbreak, the chip line, the WhateverPad version, what did not work, the relevant log lines, and how you tested it.
