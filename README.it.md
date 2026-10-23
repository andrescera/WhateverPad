<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5: libera la tua PS5 dal DualSense

**Un ponte per controller Bluetooth sulle PS5 con jailbreak.**

[English · Español](README.md) · [Français](README.fr.md) · **Italiano** · [Português](README.pt.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [简体中文](README.zh.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [العربية](README.ar.md)

---

WhateverPad PS5 ti permette di usare sulla tua PS5 controller di altre piattaforme. Li collega tramite il chip Bluetooth della console e li presenta al sistema come DualSense virtuali.

Non serve nessun adattatore USB.

**Versione 0.9.5-beta.** Provata su una PS5 fat, firmware 10.01, con un DualShock 4 e un 8BitDo SF30 Pro (modalità Switch), nei menu della console, nei giochi e negli emulatori.

**Disponibile in dodici lingue:** inglese, spagnolo, francese, italiano, portoghese, tedesco, russo, cinese semplificato, cinese tradizionale, giapponese, coreano e arabo. La pagina di WhateverPad segue la lingua della console.

---

## Su questo progetto

L'obiettivo ora è far funzionare WhateverPad con più controller. Se lo provi, inviami il tuo log (vedi «Aiutaci a confermare la compatibilità» più sotto): ogni log aiuta.

Il codice sorgente completo è pubblicato in questo repository con licenza GPL-3.0-or-later. Se sviluppi un emulatore e vuoi integrare WhateverPad, puoi farlo con quella licenza. Scrivimi a [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) se ti serve aiuto.

Rimane un problema noto: se il DualSense viene spento mentre WhateverPad è in funzione, può restare con la luce blu lampeggiante. Spero che i progetti futuri non lo abbiano. I progetti derivati da WhateverPad, dal codice che ho pubblicato, molto probabilmente erediteranno questo problema. Fa parte dell'eredità di WhateverPad, ed è un segno che, almeno nella parte Bluetooth, sono fork di WhateverPad.

---

## Compilare dal codice

Ti serve il [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                               # test sul computer, con il simulatore Bluetooth
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk       # l'ELF in dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk      # oppure lo stesso, dentro un container
```

## Dentro il codice

| Dove | Che cosa fa |
| --- | --- |
| `src/host.c` | L'host Bluetooth Classic: ricerca, chiamate, associazione, canali L2CAP, SDP e HID |
| `src/le.c` | Bluetooth LE: ricerca, associazione con Secure Connections e scambio di identità, HID su GATT |
| `src/profiles.c` | Un profilo per famiglia di controller: id, come leggere i report, sequenza di risveglio, vibrazione e luci |
| `src/generic.c`, `maps/` | Qualsiasi altro controller HID, letto tramite il proprio descrittore e un file di mappatura facoltativo |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | L'HCI via USB con il chip Bluetooth della console, condiviso con il sistema |
| `src/ps5_vpad.c` | Crea il DualSense virtuale e lo alimenta |
| `src/web.c`, `web/index.html` | La pagina e la sua API |
| `src/ps5_main.c` | Il ciclo principale del payload |
| `tests/` | I simulatori Bluetooth (`sim.c`, `sim_le.c`) e tutti i test |

Aggiungere un controller significa, nella maggior parte dei casi, aggiungere un profilo in `src/profiles.c` e un test con il simulatore. I dettagli sono in [CONTRIBUTING.md](CONTRIBUTING.md).

---

## ⚠️ Importante: non spegnere il DualSense mentre WhateverPad è in funzione

Se il DualSense viene spento mentre WhateverPad è in funzione, può entrare nella modalità blu lampeggiante e non funzionare fino al riavvio della console.

Perché la console non lo spenga da sola dopo un po' di inattività, imposta:

**Impostazioni → Sistema → Risparmio energetico → Imposta il tempo per lo spegnimento dei controller → Non spegnere**

Se non lo spegni, tutto funziona normalmente.

---

## ⚠️ Importante: scegli «Gioco rapido / Ospite» per il controller esterno

Quando premi PS su un controller esterno, la console chiede chi lo userà. Scegli **Gioco rapido / Ospite**. Il tuo utente principale tiene il DualSense, e puoi tornarci in qualsiasi momento riprendendo il DualSense.

---

# Compatibilità dei controller

| Controller | Stato | Note |
| --- | --- | --- |
| **DualShock 4** | ✅ **Verificato** | Mettilo in modalità di associazione (Share + PS) ogni volta che lo associ. |
| **8BitDo SF30 Pro (modalità Switch)** | ✅ **Verificato** | Appare come Switch Pro Controller. Usa Configura PS per il tasto PS. |
| **GameSir G7 SE** | ✅ **Verificato** | |
| **Nintendo Switch Pro Controller** | 🧪 **Supportato, non ancora provato** | Stesso protocollo dell'8BitDo in modalità Switch. |
| **Controller Xbox** | ✅ **Funzionano (secondo gli utenti)** | Alcuni utenti confermano che funzionano dalla 0.8. Se il tuo non si connette, inviami il tuo log e il modello. |
| **Qualsiasi altro controller HID Bluetooth** | 🧪 **Profilo generico** | Può funzionare senza un profilo specifico. |

**Controller che pensiamo siano compatibili** (supportati nel codice, non ancora provati su una console): DualShock 4 v1, Hori Onyx, Razer Raiju Ultimate, Razer Raiju Tournament Edition, SCUF Vantage 2, Nintendo Switch Pro Controller, Nintendo Switch Online (SNES, N64, Mega Drive / Genesis), GameSir G3s, G4s, G7 Pro, T1s e T2a, Betop 2585N2, Bionik Vulkan, LanShen X1Pro, Xiaomi Mi Controller, Amazon Fire / iPega, iPega 9-series, 8BitDo SN30 Pro per Xbox Cloud Gaming, 8BitDo Ultimate 2.4G, SteelSeries, controller NVIDIA Shield (2017), Razer Serval, PowerA MOGA Hero, MOGA Pro e MOGA Pro 2, controller Atari VCS Modern, Hyperkin Scout, Mocute 050. Se ne provi uno, inviami il tuo log.

L'elenco completo, controller per controller, è in [COMPATIBILITY.md](COMPATIBILITY.md).

---

# Come funziona

WhateverPad PS5 viene eseguito come payload residente su una PS5 con jailbreak. Usa il chip Bluetooth della console, **condividendolo con il sistema**, per trovare e associare i controller, leggerne i comandi e presentarli alla PS5 come DualSense virtuali.

**Non** modifica i giochi, il firmware della PS5 né il jailbreak. La tua PS5 deve già poter eseguire payload.

---

# Come si usa

1. **Carica WhateverPad.** Scarica `WhateverPad-PS5-0.9.5-beta.elf` dalle [Releases](https://github.com/andrescera/WhateverPad/releases) e caricalo con il caricatore di payload che usi di solito.
2. **Apri la pagina.** Apri **WhateverPad** nella riga Contenuti multimediali della console, oppure `http://<ip-della-console>:8095` da un telefono o un computer della stessa rete.
3. **Associa.** Premi **Associa**, poi metti il controller in modalità di associazione, vicino alla console.

| Controller | Modalità di associazione |
| --- | --- |
| **DualShock 4** | Tieni premuti **Share + PS** finché la barra luminosa lampeggia due volte |
| **8BitDo (modalità Switch)** | Tieni premuto **Pair** per circa 3 secondi, finché le luci lampeggiano velocemente |
| **Nintendo Switch Pro** | Premi il piccolo pulsante di sincronizzazione accanto alla porta USB-C |
| **Altri controller** | Segui le istruzioni del produttore |

4. **Scegli l'utente.** Quando la console lo chiede, scegli **Gioco rapido / Ospite**.

Si possono usare più controller insieme, oltre al DualSense.

### Configura PS

Per i controller senza un tasto PS utilizzabile (Switch Pro, 8BitDo e altri), il pulsante **Configura PS** permette di scegliere **un tasto o una combinazione di tre** che farà da tasto PS. Scegli quanti, premili sul controller e tienili premuti per un secondo: WhateverPad li rileva da solo. L'impostazione viene salvata per quel modello di controller.

I DualShock 4 e i DualSense hanno il loro tasto PS, quindi per loro questa opzione non compare.

---

# Ogni avvio parte da zero

WhateverPad non conserva i controller associati tra un avvio e l'altro. Associa i tuoi controller ogni volta che avvii WhateverPad. Così si evitano vecchie associazioni che non funzionano più e il chip Bluetooth resta in uno stato noto.

Se un controller che era stato spento si ricollega da solo e non risponde, spegnilo e associalo di nuovo dalla pagina.

---

# Arrestare WhateverPad

Arrestalo sempre con **Arresta WhateverPad PS5** nella sua pagina, anche prima di avviarlo di nuovo e prima di spegnere o riavviare la console. Il pulsante **Esci** chiude solo la pagina.

---

# Risoluzione dei problemi

### Non trova nessun controller

1. Premi prima **Associa** e poi metti il controller in modalità di associazione.
2. Controlla che il controller sia davvero in modalità di associazione (per esempio, la barra luminosa del DualShock 4 deve lampeggiare due volte).
3. Se il controller è stato associato prima a un altro dispositivo e continua a non comparire, reimpostalo (DualShock 4: una graffetta nel forellino accanto a L2 per circa 5 secondi; 8BitDo Pro 2: tieni premuto Pair per circa 5 secondi) e riprova.

### È in funzione un altro payload per controller

Un altro payload che usa il chip Bluetooth e la porta 8095 interferisce. Arrestalo dal suo menu prima di avviare WhateverPad; WhateverPad mostra un avviso quando lo rileva.

### La pagina non si apre

Controlla che WhateverPad sia in funzione, che il telefono o il computer sia sulla stessa rete della PS5 e di usare l'indirizzo IP della console con la porta `8095`.

---

# Aiutaci a confermare la compatibilità

Se puoi confermare che WhateverPad PS5 funziona (o non funziona) con un controller, invia il log (`/data/whateverpad/whateverpad.log`) a [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com), con il modello del controller, il firmware della console, il jailbreak e la versione di WhateverPad. Aiuta molto.

### Sostieni il progetto

Molte persone mi chiedono di provare altri controller e altre versioni della console. Ogni aiuto è benvenuto per **portare questo progetto su PS4** e sul **firmware 13.60 della PS5**, e per supportare **più controller**. Mi piacerebbe molto avere una console con il firmware 13.60 oltre alla mia con il 10.01. Non è obbligatorio, ma chi vuole aiutare il progetto può farlo dalla mia pagina Throne: **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# Ringraziamenti

WhateverPad PS5 è un fork di [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) di elmonomalva0. Questi progetti sono stati studiati per capire come funzionano le cose, e le loro idee sono state riscritte per WhateverPad. Grazie ai loro autori:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): l'idea della parte Bluetooth, condividere il chip Bluetooth della PS5 con il sistema.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): i profili dei controller per produttore e modello, e il formato dei report di molti controller.
- **Ghostcontrol** (StonedModder): come creare un DualSense virtuale sulla PS5 e assegnarlo a un utente.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) e **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: la sequenza del controller virtuale e il formato dei dati del controller.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): idee sulla negoziazione Bluetooth.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): gli strumenti con cui si compila tutto.

Licenza: GPL-3.0-or-later ([LICENSE](LICENSE)).

Autore: **elmonomalvad0**, autore di [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contatto: exposed.ct.ws@gmail.com
