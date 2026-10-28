<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5: libérate del DualSense

**Un puente de mandos Bluetooth para PS5 con jailbreak.**

**[English](README.md) · Español**

---

> **WhateverPad es un fork de [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) de elmonomalva0.** Se conservan el código original, los créditos y la licencia GPL-3.0-or-later. El fork renombra el payload y sus propios archivos, y todavía no ha cambiado ningún comportamiento. Los informes de compatibilidad de mandos siguen perteneciendo al proyecto original.

WhateverPad PS5 te permite usar en tu PS5 mandos de otras plataformas. Los conecta a través del propio chip Bluetooth de la consola y los presenta al sistema como DualSense virtuales.

No hace falta ningún adaptador USB.

**Versión 0.9.5-beta**, que es el código actual de AnyPad PS5 con el nombre y el logo de WhateverPad. La prueba original se hizo en una PS5 fat, firmware 10.01, con un DualShock 4 y un 8BitDo SF30 Pro (modo Switch), en los menús de la consola, en juegos y en emuladores. Nadie lo ha ejecutado aún en el firmware 13.60: esos resultados se publicarán aquí cuando salgan de la consola del dueño.

**La página de WhateverPad está disponible en doce idiomas:** español, inglés, francés, italiano, portugués, alemán, ruso, chino simplificado, chino tradicional, japonés, coreano y árabe. Sigue el idioma de la consola.

---

## Sobre este proyecto

WhateverPad PS5 empezó el 3 de octubre de 2026, hacia las 14:20 (hora de España), el día que publiqué su primera versión. La idea partió de MissionControl, que permite usar en la Nintendo Switch mandos de otras consolas. Quizá WhateverPad no fue el primer proyecto para usar otros mandos en la PS5: ya había proyectos que los conectaban por USB. Pero ninguno los conectaba por Bluetooth, y WhateverPad fue el primero en hacerlo. Tuve una idea y la llevé a cabo.

El objetivo ahora es que WhateverPad funcione con más mandos. Si lo pruebas, por favor envíame tu log (mira «Ayúdanos a confirmar la compatibilidad» más abajo): cada log ayuda.

El código fuente completo está publicado en este repositorio con licencia GPL-3.0-or-later (mira «Código abierto» más abajo). Si desarrollas un emulador y quieres integrar WhateverPad, puedes hacerlo bajo esa licencia. Escríbeme a [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) si necesitas ayuda.

Queda un fallo conocido, heredado de la versión original: si se apaga el DualSense con WhateverPad en marcha, puede quedarse en azul parpadeante y no responder hasta reiniciar la consola. Este fork todavía no lo corrige. La corrección aparecerá aquí, en los dos idiomas, solo cuando el dueño la haya confirmado en su propia consola.

---

## Código abierto

Desde la versión 0.9.5 todo el código fuente de WhateverPad PS5 está publicado en este repositorio, con licencia GPL-3.0-or-later: el anfitrión Bluetooth, los perfiles de mandos, la página de WhateverPad, las pruebas y los scripts de compilación.

Mientras WhateverPad está en marcha, su página ofrece en tu red local el log en tiempo real, algunos ajustes de la conexión Bluetooth que se pueden cambiar sin reiniciar ni recompilar, y el botón de emparejar.

Ten en cuenta:

- La página no tiene PIN, como en todas las versiones: cualquiera en tu red local puede leer el log y mandar órdenes a WhateverPad. Úsalo solo en una red de confianza y detén WhateverPad cuando termines.
- El log contiene las direcciones Bluetooth de tus mandos y el id de tu usuario de la consola. Revísalo antes de compartirlo.
- WhateverPad nunca se conecta a internet.

### Comparte lo que aprendes

Si adaptas WhateverPad a un mando, abre por favor una [incidencia](https://codeberg.org/elmonomalva0/Any-Pad-ps5/issues) con el modelo del mando, el firmware de la consola, el jailbreak, la versión de WhateverPad, qué has cambiado y por qué, y el log. Cada propuesta se revisa, y las compatibles entran en versiones posteriores. Un cambio debe seguir pasando `make test` y no debe alterar cómo conectan el DualShock 4 y el 8BitDo.

### Compilar desde el código

Necesitas el [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                              # las pruebas unitarias en el ordenador
make ps5 PS5_PAYLOAD_SDK=/ruta/al/ps5-payload-sdk      # el ELF en dist/
tools/build-elf-docker.sh /ruta/al/ps5-payload-sdk     # o lo mismo, dentro de un contenedor
```

### Por favor, mencióname

Si haces tu propia versión de WhateverPad, o la incluyes (o una parte) en un emulador, un HEN, un payload o cualquier otro programa, te lo agradeceré mucho si mencionas a **elmonomalvad0** y enlazas este proyecto. La licencia, GPL-3.0-or-later, ya exige que conserves los avisos de copyright y que distribuyas tus cambios con la misma licencia. La mención es una petición, no una condición, y es lo que hace que merezca la pena seguir compartiendo el trabajo.

### Dentro del código

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
| `tests/` | Las pruebas unitarias en el ordenador: analizadores, mapeos, codificadores y funciones de decisión |

Añadir un mando es, en la mayoría de los casos, añadir un perfil en `src/profiles.c` y una prueba unitaria.

---

## Novedades de la 0.9.5

- **Código abierto:** todo el código está publicado.

## Novedades de la 0.9

- **Más compatible:** admite más mandos y es compatible con los dos chips Bluetooth que llevan las PS5.
- **Errores corregidos.**
- **Nuevo entorno gráfico** en la página de WhateverPad.

## Novedades de la 0.8

### Emparejado más rápido

El emparejado es ahora mucho más rápido y fiable. En nuestras pruebas, el DualShock 4 y el 8BitDo aparecen en pocos segundos y quedan listos pocos segundos después, cuando antes a menudo hacían falta varios intentos.

### Más estable

- **Respuesta fluida:** los mandos reaccionan con suavidad mientras juegas, sin lag.
- **Mandos encendidos de forma normal** (en vez de en modo emparejamiento): ahora se emparejan al primer intento.
- **Tratamiento más inteligente** de los mandos que tardan en responder o rechazan un paso.

### Configurar PS

Para los mandos sin un botón PS que funcione (Switch Pro, 8BitDo y otros), el botón **Configurar PS** permite elegir **un botón o una combinación de tres** que hará de botón PS. Eliges cuántos, los pulsas en el mando y los mantienes un segundo: WhateverPad los detecta solo. El ajuste se guarda para ese modelo de mando.

Los DualShock 4 y los DualSense tienen su propio botón PS, así que esta opción no aparece para ellos.

### Doce idiomas

La página de WhateverPad sigue el idioma de la consola: español, inglés, francés, italiano, portugués, alemán, ruso, chino simplificado, chino tradicional, japonés, coreano y árabe (de derecha a izquierda). Cualquier otro idioma usa el inglés.

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

Estos resultados son los de la versión original, en el firmware 10.01. Este fork todavía no ha probado ningún mando en su propia consola.

La lista completa, mando por mando, está en [COMPATIBILITY.md](COMPATIBILITY.md).

---

# Cómo funciona

WhateverPad PS5 se ejecuta como un payload residente en una PS5 con jailbreak. Usa el propio chip Bluetooth de la consola, **compartiéndolo con el sistema**, para buscar y emparejar mandos, leer sus botones y presentarlos a la PS5 como DualSense virtuales.

**No** modifica juegos, el firmware de la PS5 ni el jailbreak. Tu PS5 ya debe poder ejecutar payloads.

---

# Cómo usarlo

1. **Carga WhateverPad.** Compila el ELF (mira «Compilar desde el código» más arriba: queda en `dist/`), y cárgalo con el cargador de payloads que uses normalmente.
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

Mucha gente me pide que pruebe otros mandos y otras versiones de la consola. Cualquier ayuda sirve, tanto para **portar este proyecto a PS4** como para **añadir más mandos**. La versión original solo tiene una consola con el firmware 10.01; las pruebas en el firmware 13.60 están previstas en la consola del dueño. Si quieres ayudar, el autor original acepta donaciones en **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**.

---

# Agradecimientos

WhateverPad PS5 es código propio. Estos proyectos se estudiaron para entender cómo funcionan las cosas, y sus ideas se escribieron de nuevo para WhateverPad. Gracias a sus autores:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): la idea de la parte Bluetooth, compartir el propio chip Bluetooth de la PS5 con el sistema.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): los perfiles de mandos por fabricante y modelo, y el formato de los informes de muchos mandos.
- **Ghostcontrol** (StonedModder): cómo crear un DualSense virtual en la PS5 y asignarlo a un usuario.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) y **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: la secuencia del mando virtual y el formato de los datos del mando.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): ideas sobre la negociación Bluetooth.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): las herramientas con las que se compila todo.

Licencia: GPL-3.0-or-later ([LICENSE](LICENSE)).

Autor: **elmonomalvad0**, el mismo autor que *sinfiltros* (el WhateverPad PS5 original en GitHub).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contacto: exposed.ct.ws@gmail.com
