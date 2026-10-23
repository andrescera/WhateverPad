<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5: Free your PS5 from the DualSense

**A Bluetooth controller bridge for jailbroken PS5 consoles.**
**Un puente de mandos Bluetooth para PS5 con jailbreak.**

**[English](#english) · [Español](#español)** · [Français](README.fr.md) · [Italiano](README.it.md) · [Português](README.pt.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [简体中文](README.zh.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [العربية](README.ar.md)

---

> **WhateverPad is a fork of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by elmonomalva0.** The original code, credits and GPL-3.0-or-later licence are kept; this fork changes the name/branding and the DualSense reconnect handling. Controller reports and issues for this fork go to [this repository's issue tracker](https://github.com/andrescera/WhateverPad/issues).

<a name="english"></a>
# English

WhateverPad PS5 lets you use controllers from other platforms on your PS5. It connects them through the console's own Bluetooth chip and presents them to the system as virtual DualSense controllers.

No USB dongle required.

**Version 0.9.5-beta.** Tested on a PS5 fat, firmware 10.01, with a DualShock 4 and an 8BitDo SF30 Pro (Switch mode), in the console menus, in games and in emulators.

**Available in twelve languages:** English, Spanish, French, Italian, Portuguese, German, Russian, Simplified Chinese, Traditional Chinese, Japanese, Korean and Arabic. The WhateverPad page follows the console language.

---

## About this project

The goal now is to make WhateverPad work with more controllers. If you try it, please send me your log (see "Help us confirm compatibility" below): every log helps.

The full source code is published in this repository under GPL-3.0-or-later. If you develop an emulator and would like to integrate WhateverPad, you are welcome to do so under that license. Write to me at [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) if you need help.

One known issue remains: if the DualSense is switched off while WhateverPad is running, it can stay blinking blue. I hope future projects do not have it. Projects forked from WhateverPad, from the code I published, will very probably inherit this issue. It is part of WhateverPad's legacy, and a sign that, at least in their Bluetooth part, they are forks of WhateverPad.

---

## Build from source

You need the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                              # host tests, with the Bluetooth simulator
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk      # the ELF in dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk     # or the same, inside a container
```

## Inside the code

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
| `tests/` | The Bluetooth simulators (`sim.c`, `sim_le.c`) and all the tests |

Adding a controller is, in most cases, adding one profile in `src/profiles.c` and a test with the simulator. See [CONTRIBUTING.md](CONTRIBUTING.md) for the details.

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

The full list, controller by controller, is in [COMPATIBILITY.md](COMPATIBILITY.md).

---

# How it works

WhateverPad PS5 runs as a resident payload on a jailbroken PS5. It uses the console's own Bluetooth chip, **sharing it with the system**, to discover and pair controllers, read their input and present them to the PS5 as virtual DualSense controllers.

It does **not** modify games, the PS5 firmware or the jailbreak. Your PS5 must already be able to run payloads.

---

# How to use

1. **Load WhateverPad.** Download `WhateverPad-PS5-0.9.5-beta.elf` from [Releases](https://github.com/andrescera/WhateverPad/releases) and load it with the payload loader you normally use.
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

### Configure PS

For controllers without a usable PS button (Switch Pro, 8BitDo and others), the **Configure PS** button lets you choose **one button or a combination of three** that will act as the PS button. Choose how many, press them on the controller and hold them for one second: WhateverPad detects them by itself. The setting is saved for that controller model.

DualShock 4 and DualSense controllers have their own PS button, so this option does not appear for them.

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

Many people ask me to test other controllers and other console versions. Any help is very welcome to **port this project to PS4** and to **PS5 firmware 13.60**, and to support **more controllers**. I would love to have a console on firmware 13.60 as well as my 10.01. It is not mandatory, but anyone who wants to help the project can do so on my Throne page: **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# Credits

WhateverPad PS5 is a fork of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) by elmonomalva0. These projects were studied to understand how things work, and their ideas were written anew for WhateverPad. Thanks to their authors:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): the idea behind the Bluetooth part, sharing the PS5's own Bluetooth chip with the system.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): controller profiles by vendor and model, and the report layouts of many controllers.
- **Ghostcontrol** (StonedModder): how to create a virtual DualSense on the PS5 and assign it to a user.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) and **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the virtual pad sequence and the pad data layout.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): ideas on handling Bluetooth negotiation.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): the toolchain everything is built with.

License: GPL-3.0-or-later ([LICENSE](LICENSE)).

Author: **elmonomalvad0**, author of [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contact: exposed.ct.ws@gmail.com

---

<a name="español"></a>
# Español

WhateverPad PS5 te permite usar en tu PS5 mandos de otras plataformas. Los conecta a través del propio chip Bluetooth de la consola y los presenta al sistema como DualSense virtuales.

No hace falta ningún adaptador USB.

**Versión 0.9.5-beta.** Probada en una PS5 fat, firmware 10.01, con un DualShock 4 y un 8BitDo SF30 Pro (modo Switch), en los menús de la consola, en juegos y en emuladores.

**Disponible en doce idiomas:** español, inglés, francés, italiano, portugués, alemán, ruso, chino simplificado, chino tradicional, japonés, coreano y árabe. La página de WhateverPad sigue el idioma de la consola.

---

## Sobre este proyecto

El objetivo ahora es que WhateverPad funcione con más mandos. Si lo pruebas, por favor envíame tu log (mira «Ayúdanos a confirmar la compatibilidad» más abajo): cada log ayuda.

El código fuente completo está publicado en este repositorio con licencia GPL-3.0-or-later. Si desarrollas un emulador y quieres integrar WhateverPad, puedes hacerlo bajo esa licencia. Escríbeme a [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) si necesitas ayuda.

Queda un fallo conocido: si se apaga el DualSense con WhateverPad en marcha, puede quedarse en azul parpadeante. Espero que los próximos proyectos no lo tengan. Los que sean forks de WhateverPad, del código que publiqué, muy probablemente heredarán ese fallo. Es parte del legado de WhateverPad, y una muestra de que, al menos en su parte Bluetooth, son un fork de WhateverPad.

---

## Compilar desde el código

Necesitas el [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                              # pruebas en el ordenador, con el simulador Bluetooth
make ps5 PS5_PAYLOAD_SDK=/ruta/al/ps5-payload-sdk      # el ELF en dist/
tools/build-elf-docker.sh /ruta/al/ps5-payload-sdk     # o lo mismo, dentro de un contenedor
```

## Dentro del código

| Dónde | Qué hace |
| --- | --- |
| `src/host.c` | El anfitrión Bluetooth Classic: búsqueda, llamadas, emparejado, canales L2CAP, SDP y HID |
| `src/le.c` | Bluetooth LE: búsqueda, emparejado con Secure Connections e intercambio de identidad, HID sobre GATT |
| `src/profiles.c` | Un perfil por familia de mandos: ids, cómo leer sus informes, secuencia de despertar, vibración y luces |
| `src/generic.c`, `maps/` | Cualquier otro mando HID, leído con su propio descriptor y un archivo de mapeo opcional |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | El HCI por USB con el chip Bluetooth de la consola, que se comparte con el sistema |
| `src/ps5_vpad.c` | Crea el DualSense virtual y lo alimenta |
| `src/web.c`, `web/index.html` | La página y su API |
| `src/ps5_main.c` | El bucle principal del payload |
| `tests/` | Los simuladores Bluetooth (`sim.c`, `sim_le.c`) y todas las pruebas |

Añadir un mando es, en la mayoría de los casos, añadir un perfil en `src/profiles.c` y una prueba con el simulador. Los detalles están en [CONTRIBUTING.md](CONTRIBUTING.md).

---

## ⚠️ Importante: no apagues el DualSense con WhateverPad en marcha

Si el DualSense se apaga mientras WhateverPad está en marcha, puede entrar en el modo azul parpadeante y no funcionar hasta reiniciar la consola.

Para que la consola no lo apague sola tras un rato sin usarlo, configura:

**Ajustes → Sistema → Ahorro de energía → Establecer tiempo hasta que se apaguen los mandos → No apagar**

Si no lo apagas, todo funciona con normalidad.

---

## ⚠️ Importante: elige «Juego rápido / Invitado» para el mando externo

Cuando pulsas PS en un mando externo, la consola pregunta quién lo va a usar. Elige **Juego rápido / Invitado**. Tu usuario principal conserva el DualSense, y puedes volver a él en cualquier momento cogiendo el DualSense.

---

# Compatibilidad de mandos

| Mando | Estado | Notas |
| --- | --- | --- |
| **DualShock 4** | ✅ **Verificado** | Ponlo en modo emparejamiento (Share + PS) cada vez que lo emparejes. |
| **8BitDo SF30 Pro (modo Switch)** | ✅ **Verificado** | Aparece como Switch Pro Controller. Usa Configurar PS para el botón PS. |
| **GameSir G7 SE** | ✅ **Verificado** | |
| **Nintendo Switch Pro Controller** | 🧪 **Soportado, sin probar** | Mismo protocolo que el 8BitDo en modo Switch. |
| **Mandos Xbox** | ✅ **Funcionan (según usuarios)** | Varios usuarios confirman que funcionan desde la 0.8. Si el tuyo no conecta, envíame tu log y el modelo. |
| **Cualquier otro mando HID Bluetooth** | 🧪 **Perfil genérico** | Puede funcionar sin un perfil específico. |

**Mandos que creemos compatibles** (soportados en el código, aún sin probar en una consola): DualShock 4 v1, Hori Onyx, Razer Raiju Ultimate, Razer Raiju Tournament Edition, SCUF Vantage 2, Nintendo Switch Pro Controller, Nintendo Switch Online (SNES, N64, Mega Drive / Genesis), GameSir G3s, G4s, G7 Pro, T1s y T2a, Betop 2585N2, Bionik Vulkan, LanShen X1Pro, Xiaomi Mi Controller, Amazon Fire / iPega, iPega 9-series, 8BitDo SN30 Pro para Xbox Cloud Gaming, 8BitDo Ultimate 2.4G, SteelSeries, mando NVIDIA Shield (2017), Razer Serval, PowerA MOGA Hero, MOGA Pro y MOGA Pro 2, mando Atari VCS Modern, Hyperkin Scout, Mocute 050. Si pruebas alguno, envíame tu log.

La lista completa, mando por mando, está en [COMPATIBILITY.md](COMPATIBILITY.md).

---

# Cómo funciona

WhateverPad PS5 se ejecuta como un payload residente en una PS5 con jailbreak. Usa el propio chip Bluetooth de la consola, **compartiéndolo con el sistema**, para buscar y emparejar mandos, leer sus botones y presentarlos a la PS5 como DualSense virtuales.

**No** modifica juegos, el firmware de la PS5 ni el jailbreak. Tu PS5 ya debe poder ejecutar payloads.

---

# Cómo usarlo

1. **Carga WhateverPad.** Descarga `WhateverPad-PS5-0.9.5-beta.elf` desde [Releases](https://github.com/andrescera/WhateverPad/releases) y cárgalo con el cargador de payloads que uses normalmente.
2. **Abre la página.** Abre **WhateverPad** en *Contenido multimedia* de la consola, o `http://<ip-de-la-consola>:8095` desde un móvil o un PC de la misma red.
3. **Empareja.** Pulsa **Emparejar** y después pon el mando en modo emparejamiento, cerca de la consola.

| Mando | Modo emparejamiento |
| --- | --- |
| **DualShock 4** | Mantén **Share + PS** hasta que la barra de luz haga un doble destello |
| **8BitDo (modo Switch)** | Mantén **Pair** unos 3 segundos, hasta que las luces parpadeen rápido |
| **Nintendo Switch Pro** | Pulsa el pequeño botón de sincronización junto al puerto USB-C |
| **Otros mandos** | Sigue las instrucciones del fabricante |

4. **Elige el usuario.** Cuando la consola lo pregunte, elige **Juego rápido / Invitado**.

Se pueden usar varios mandos a la vez, junto con el DualSense.

### Configurar PS

Para los mandos sin un botón PS que funcione (Switch Pro, 8BitDo y otros), el botón **Configurar PS** permite elegir **un botón o una combinación de tres** que hará de botón PS. Eliges cuántos, los pulsas en el mando y los mantienes un segundo: WhateverPad los detecta solo. El ajuste se guarda para ese modelo de mando.

Los DualShock 4 y los DualSense tienen su propio botón PS, así que esta opción no aparece para ellos.

---

# Cada ejecución empieza limpia

WhateverPad no guarda mandos emparejados entre ejecuciones. Empareja tus mandos cada vez que arranques WhateverPad. Así se evitan emparejados antiguos que ya no funcionan y el chip Bluetooth queda en un estado conocido.

Si un mando que se apagó se reconecta solo y no responde, apágalo y vuelve a emparejarlo desde la página.

---

# Apagar WhateverPad

Páralo siempre con **Apagar WhateverPad PS5** en su página, también antes de volver a ejecutarlo y antes de apagar o reiniciar la consola. El botón **Salir** solo cierra la página.

---

# Solución de problemas

### No encuentra ningún mando

1. Pulsa **Emparejar** primero y pon el mando en modo emparejamiento después.
2. Comprueba que el mando está de verdad en modo emparejamiento (por ejemplo, la barra de luz del DualShock 4 debe hacer un doble destello).
3. Si el mando estuvo emparejado antes con otro aparato y sigue sin aparecer, haz un reset (DualShock 4: un clip en el agujero junto a L2 unos 5 segundos; 8BitDo Pro 2: mantén Pair unos 5 segundos) e inténtalo de nuevo.

### Hay otro payload de mandos en marcha

Otro payload que use el chip Bluetooth y el puerto 8095 interfiere. Páralo desde su propio menú antes de ejecutar WhateverPad; WhateverPad avisa en pantalla cuando lo detecta.

### La página no se abre

Comprueba que WhateverPad está en marcha, que tu móvil o PC está en la misma red que la PS5 y que usas la IP de la consola con el puerto `8095`.

---

# Ayúdanos a confirmar la compatibilidad

Si puedes confirmar que WhateverPad PS5 funciona (o no funciona) con un mando, envía el log (`/data/whateverpad/whateverpad.log`) a [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com), con el modelo del mando, el firmware de la consola, el jailbreak y la versión de WhateverPad. Esto ayuda mucho.

### Apoya el proyecto

Mucha gente me pide que pruebe otros mandos y otras versiones de la consola. Agradecemos cualquier ayuda para poder **portar este proyecto a PS4**, a las versiones **13.60 de PS5** y a **diferentes mandos**. Me encantaría tener una consola con el firmware 13.60 además de la mía, con el 10.01. No es obligatorio, pero quien quiera ayudar al proyecto puede hacerlo desde mi página de Throne: **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# Agradecimientos

WhateverPad PS5 es un fork de [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) de elmonomalva0. Estos proyectos se estudiaron para entender cómo funcionan las cosas, y sus ideas se escribieron de nuevo para WhateverPad. Gracias a sus autores:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): la idea de la parte Bluetooth, compartir el propio chip Bluetooth de la PS5 con el sistema.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): los perfiles de mandos por fabricante y modelo, y el formato de los informes de muchos mandos.
- **Ghostcontrol** (StonedModder): cómo crear un DualSense virtual en la PS5 y asignarlo a un usuario.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) y **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: la secuencia del mando virtual y el formato de los datos del mando.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): ideas sobre la negociación Bluetooth.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): las herramientas con las que se compila todo.

Licencia: GPL-3.0-or-later ([LICENSE](LICENSE)).

Autor: **elmonomalvad0**, autor de [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contacto: exposed.ct.ws@gmail.com
