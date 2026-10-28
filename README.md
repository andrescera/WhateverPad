<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5: Free your PS5 from the DualSense

**A Bluetooth controller bridge for jailbroken PS5 consoles.**

**[English](README.md) · [Español](README.es.md)**

---

> **WhateverPad is a fork of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by elmonomalva0.** The original code, credits and GPL-3.0-or-later licence are kept. The fork renames the payload and its own files, and it has changed no behaviour yet. Upstream controller-compatibility reports still belong in the original project.

WhateverPad PS5 lets you use controllers from other platforms on your PS5. It connects them through the console's own Bluetooth chip and presents them to the system as virtual DualSense controllers.

No USB dongle required.

**Version 0.9.5-beta**, which is the current AnyPad PS5 code with the WhateverPad name and logo. Upstream tested it on a PS5 fat, firmware 10.01, with a DualShock 4 and an 8BitDo SF30 Pro (Switch mode), in the console menus, in games and in emulators. Nobody has run it on firmware 13.60 yet: those results will be published here once they come from the owner's own console.

**The WhateverPad page is available in twelve languages:** English, Spanish, French, Italian, Portuguese, German, Russian, Simplified Chinese, Traditional Chinese, Japanese, Korean and Arabic. It follows the console language.

---

## About this project

WhateverPad PS5 started on 3 October 2026 at about 14:20 (Spanish time), the day I published its first version. The idea came from MissionControl, which lets the Nintendo Switch use controllers from other consoles. WhateverPad may not have been the first project to use other controllers on the PS5: there were already projects that connected them by USB. But none connected them by Bluetooth, and WhateverPad was the first to do it. I had an idea and I carried it out.

The goal now is to make WhateverPad work with more controllers. If you try it, please send me your log (see "Help us confirm compatibility" below): every log helps.

The full source code is published in this repository under GPL-3.0-or-later (see "Open source" below). If you develop an emulator and would like to integrate WhateverPad, you are welcome to do so under that license. Write to me at [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) if you need help.

One known issue remains, inherited from upstream: if the DualSense is switched off while WhateverPad is running, it can stay blinking blue and stop responding until the console restarts. This fork does not fix it yet. The fix will appear here, in both languages, only after the owner has confirmed it on his own console.

---

## Open source

From version 0.9.5 the whole source code of WhateverPad PS5 is published in this repository, under GPL-3.0-or-later: the Bluetooth host, the controller profiles, the WhateverPad page, the tests and the build scripts.

While WhateverPad is running, its page offers on your local network the log in real time, a few settings of the Bluetooth connection that can be changed without restarting or rebuilding, and the pairing button.

Please keep in mind:

- The page has no PIN, as in every version: anyone on your local network can read the log and send commands to WhateverPad. Use it only on a network you trust, and stop WhateverPad when you finish.
- The log contains the Bluetooth addresses of your controllers and the id of your console user. Review it before sharing it.
- WhateverPad never connects to the internet.

### Share what you learn

If you adapt WhateverPad to a controller, please open an [issue](https://codeberg.org/elmonomalva0/Any-Pad-ps5/issues) with the controller model, the console firmware, the jailbreak, the WhateverPad version, what you changed and why, and the log. Each proposal gets reviewed, and the compatible ones go into later versions. A change must keep passing `make test` and must not alter how the DualShock 4 and the 8BitDo connect.

### Build from source

You need the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                              # the host unit tests
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk      # the ELF in dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk     # or the same, inside a container
```

### Please mention me

If you make your own version of WhateverPad, or you include it (or a part of it) in an emulator, a HEN, a payload or any other program, I would be very grateful if you mentioned **elmonomalvad0** and linked this project. The license, GPL-3.0-or-later, already requires you to keep the copyright notices and to distribute your changes under the same license. The mention is a request, not a condition, and it is what makes it worthwhile to keep sharing the work.

### Inside the code

| Where | What it does |
| --- | --- |
| `src/host.c` | The Bluetooth Classic host: search, calls, pairing, L2CAP channels, SDP and HID |
| `src/le.c` | Bluetooth LE: scanning, pairing with Secure Connections and identity exchange, HID over GATT |
| `src/profiles.c` | One profile per family of controllers: ids, how to read its reports, wake-up sequence, rumble and lights |
| `src/generic.c`, `maps/` | Any other HID gamepad, read through its own descriptor and an optional mapping file |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | The HCI over USB with the console's Bluetooth chip, which is shared with the system |
| `src/ps5_vpad.c` | Creates the virtual DualSense and feeds it |
| `src/web.c`, `web/index.html` | The page and its API |
| `src/ps5_main.c` | The main loop of the payload |
| `tests/` | The host unit tests: parsers, mappings, encoders and decision functions |

Adding a controller is, in most cases, adding one profile in `src/profiles.c` and a unit test.

---

## What is new in 0.9.5

- **Open source:** the whole code is published.

## What is new in 0.9

- **More compatible:** more controllers supported, and support for the two Bluetooth chips used in PS5 consoles.
- **Bug fixes.**
- **New graphical interface** for the WhateverPad page.

## What is new in 0.8

### Faster pairing

Pairing is now much faster and more reliable. In our tests the DualShock 4 and the 8BitDo are found in a few seconds and are ready a few seconds later, where earlier versions often needed several attempts.

### More stable

- **Smooth response:** controllers react fluidly while you play, without lag.
- **Controllers switched on normally** (instead of in pairing mode) are now paired at the first attempt.
- **Smarter handling** of controllers that are slow to answer or refuse a step.

### Configure PS

For controllers without a usable PS button (Switch Pro, 8BitDo and others), the **Configure PS** button lets you choose **one button or a combination of three** that will act as the PS button. Choose how many, press them on the controller and hold them for one second: WhateverPad detects them by itself. The setting is saved for that controller model.

DualShock 4 and DualSense controllers have their own PS button, so this option does not appear for them.

### Twelve languages

The WhateverPad page follows the console language: English, Spanish, French, Italian, Portuguese, German, Russian, Simplified Chinese, Traditional Chinese, Japanese, Korean and Arabic (right to left). Any other language falls back to English.

---

## ⚠️ Important: do not switch off the DualSense while WhateverPad is running

If the DualSense is switched off while WhateverPad is running, it can enter the blinking blue mode and stop working until the console is restarted.

So that the console does not switch it off by itself after a while without use, set:

**Settings → System → Power Saving → Set Time Until Controllers Turn Off → Don't Turn Off**

If you do not switch it off, everything works normally.

---

## ⚠️ Important: choose "Quick Play / Guest" for the external controller

When you press PS on an external controller, the console asks who will use it. Choose **Quick Play / Guest**. Your main user keeps the DualSense, and you can go back to it at any time by picking up the DualSense.

---

# Controller compatibility

| Controller | Status | Notes |
| --- | --- | --- |
| **DualShock 4** | ✅ **Verified** | Put it in pairing mode (Share + PS) every time you pair it. |
| **8BitDo SF30 Pro (Switch mode)** | ✅ **Verified** | It shows up as a Switch Pro Controller. Use Configure PS for the PS button. |
| **GameSir G7 SE** | ✅ **Verified** | |
| **Nintendo Switch Pro Controller** | 🧪 **Supported, not tested yet** | Same protocol as the 8BitDo in Switch mode. |
| **Xbox controllers** | ✅ **Working (reported by users)** | Users report them working since 0.8. If yours does not connect, please send me your log and its model. |
| **Any other Bluetooth HID gamepad** | 🧪 **Generic profile** | May work without a specific profile. |

**Controllers we believe are compatible** (supported in the code, not tested on a console yet): DualShock 4 v1, Hori Onyx, Razer Raiju Ultimate, Razer Raiju Tournament Edition, SCUF Vantage 2, Nintendo Switch Pro Controller, Nintendo Switch Online (SNES, N64, Mega Drive / Genesis), GameSir G3s, G4s, G7 Pro, T1s and T2a, Betop 2585N2, Bionik Vulkan, LanShen X1Pro, Xiaomi Mi Controller, Amazon Fire / iPega, iPega 9-series, 8BitDo SN30 Pro for Xbox Cloud Gaming, 8BitDo Ultimate 2.4G, SteelSeries, NVIDIA Shield controller (2017), Razer Serval, PowerA MOGA Hero, MOGA Pro and MOGA Pro 2, Atari VCS Modern controller, Hyperkin Scout, Mocute 050. If you try one of them, please send me your log.

These results are upstream's, from firmware 10.01. This fork has not tested any controller on its own console yet.

The full list, controller by controller, is in [COMPATIBILITY.md](COMPATIBILITY.md).

---

# How it works

WhateverPad PS5 runs as a resident payload on a jailbroken PS5. It uses the console's own Bluetooth chip, **sharing it with the system**, to discover and pair controllers, read their input and present them to the PS5 as virtual DualSense controllers.

It does **not** modify games, the PS5 firmware or the jailbreak. Your PS5 must already be able to run payloads.

---

# How to use

1. **Load WhateverPad.** Build the ELF (see "Build from source" above) or download `WhateverPad-PS5-0.9.5-beta.elf` from [Releases](https://github.com/andrescera/WhateverPad/releases), then load it with the payload loader you normally use.
2. **Open the page.** Open **WhateverPad** in the console's Media row, or open `http://<console-ip>:8095` from a phone or computer on the same network.
3. **Pair.** Press **Pair**, then put the controller in pairing mode, close to the console.

| Controller | Pairing mode |
| --- | --- |
| **DualShock 4** | Hold **Share + PS** until the light bar flashes twice |
| **8BitDo (Switch mode)** | Hold **Pair** for about 3 seconds, until the lights blink fast |
| **Nintendo Switch Pro** | Press the small sync button next to the USB-C port |
| **Other controllers** | Follow the manufacturer's instructions |

4. **Choose the user.** When the console asks, choose **Quick Play / Guest**.

Several controllers can be used at the same time, together with the DualSense.

---

# Every run starts clean

WhateverPad does not keep paired controllers between runs. Pair your controllers each time you start WhateverPad. This avoids old pairings that no longer work and keeps the Bluetooth chip in a known state.

If a controller that was switched off reconnects by itself and does not respond, switch it off and pair it again from the page.

---

# Stopping WhateverPad

Always stop it with **Stop WhateverPad PS5** on its page, also before running it again and before switching off or restarting the console. The **Exit** button only closes the page.

---

# Troubleshooting

### No controller is found

1. Press **Pair** first and put the controller in pairing mode afterwards.
2. Make sure the controller really is in pairing mode (for example, the DualShock 4 light bar must flash twice).
3. If the controller was paired with another device before and still does not show up, reset it (DualShock 4: a paper clip in the small hole next to L2 for about 5 seconds; 8BitDo Pro 2: hold Pair for about 5 seconds) and try again.

### Another controller payload is running

Another payload that uses the Bluetooth chip and port 8095 gets in the way. Stop it from its own menu before running WhateverPad; WhateverPad shows a notice when it detects this.

### The page does not open

Check that WhateverPad is running, that your phone or computer is on the same network as the PS5, and that you use the console's IP address with port `8095`.

---

# Help us confirm compatibility

If you can confirm that WhateverPad PS5 works (or does not work) with a controller, please send the log (`/data/whateverpad/whateverpad.log`) to [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com), with the controller model, the console firmware, the jailbreak and the WhateverPad version. This helps a lot.

### Support the project

Many people ask me to test other controllers and other console versions. Any help is welcome, both to **port this project to PS4** and to support **more controllers**. Upstream only has a console on firmware 10.01; the firmware 13.60 runs are planned on the owner's own console. If you want to help, the original author takes donations on **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**.

---

# Credits

WhateverPad PS5 is original code. These projects were studied to understand how things work, and their ideas were written anew for WhateverPad. Thanks to their authors:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): the idea behind the Bluetooth part, sharing the PS5's own Bluetooth chip with the system.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): controller profiles by vendor and model, and the report layouts of many controllers.
- **Ghostcontrol** (StonedModder): how to create a virtual DualSense on the PS5 and assign it to a user.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) and **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the virtual pad sequence and the pad data layout.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): ideas on handling Bluetooth negotiation.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): the toolchain everything is built with.

License: GPL-3.0-or-later ([LICENSE](LICENSE)).

Author: **elmonomalvad0**, the same author as *sinfiltros* (the original WhateverPad PS5 on GitHub).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contact: exposed.ct.ws@gmail.com
