<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5 : libérez votre PS5 de la DualSense

**Un pont pour manettes Bluetooth sur les PS5 jailbreakées.**

[English · Español](README.md) · **Français** · [Italiano](README.it.md) · [Português](README.pt.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [简体中文](README.zh.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [العربية](README.ar.md)

---

WhateverPad PS5 vous permet d'utiliser sur votre PS5 des manettes d'autres plateformes. Il les connecte par la puce Bluetooth de la console et les présente au système comme des DualSense virtuelles.

Aucun adaptateur USB n'est nécessaire.

**Version 0.9.5-beta.** Testée sur une PS5 fat, firmware 10.01, avec une DualShock 4 et une 8BitDo SF30 Pro (mode Switch), dans les menus de la console, dans les jeux et dans les émulateurs.

**Disponible en douze langues :** anglais, espagnol, français, italien, portugais, allemand, russe, chinois simplifié, chinois traditionnel, japonais, coréen et arabe. La page d'WhateverPad suit la langue de la console.

---

## À propos de ce projet

L'objectif est maintenant que WhateverPad fonctionne avec davantage de manettes. Si vous l'essayez, envoyez-moi votre journal (voir « Aidez-nous à confirmer la compatibilité » plus bas) : chaque journal aide.

Le code source complet est publié dans ce dépôt sous licence GPL-3.0-or-later. Si vous développez un émulateur et souhaitez intégrer WhateverPad, vous pouvez le faire sous cette licence. Écrivez-moi à [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) si vous avez besoin d'aide.

Il reste un problème connu : si la DualSense est éteinte pendant que WhateverPad fonctionne, elle peut rester en bleu clignotant. J'espère que les futurs projets ne l'auront pas. Les projets dérivés d'WhateverPad, à partir du code que j'ai publié, hériteront très probablement de ce problème. Cela fait partie de l'héritage d'WhateverPad, et c'est un signe qu'au moins pour leur partie Bluetooth, ce sont des forks d'WhateverPad.

---

## Compiler depuis le code

Il vous faut le [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                               # tests sur l'ordinateur, avec le simulateur Bluetooth
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk       # l'ELF dans dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk      # ou la même chose, dans un conteneur
```

## Dans le code

| Où | Ce que cela fait |
| --- | --- |
| `src/host.c` | L'hôte Bluetooth Classic : recherche, appels, appairage, canaux L2CAP, SDP et HID |
| `src/le.c` | Bluetooth LE : recherche, appairage Secure Connections et échange d'identité, HID sur GATT |
| `src/profiles.c` | Un profil par famille de manettes : identifiants, lecture des rapports, séquence de réveil, vibration et lumières |
| `src/generic.c`, `maps/` | Toute autre manette HID, lue grâce à son propre descripteur et à un fichier de mappage facultatif |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | Le HCI par USB avec la puce Bluetooth de la console, partagée avec le système |
| `src/ps5_vpad.c` | Crée la DualSense virtuelle et l'alimente |
| `src/web.c`, `web/index.html` | La page et son API |
| `src/ps5_main.c` | La boucle principale du payload |
| `tests/` | Les simulateurs Bluetooth (`sim.c`, `sim_le.c`) et tous les tests |

Ajouter une manette revient, la plupart du temps, à ajouter un profil dans `src/profiles.c` et un test avec le simulateur. Les détails sont dans [CONTRIBUTING.md](CONTRIBUTING.md).

---

## ⚠️ Important : n'éteignez pas la DualSense pendant que WhateverPad fonctionne

Si la DualSense est éteinte pendant que WhateverPad fonctionne, elle peut passer en bleu clignotant et ne plus fonctionner jusqu'au redémarrage de la console.

Pour que la console ne l'éteigne pas toute seule après un moment sans utilisation, réglez :

**Paramètres → Système → Économie d'énergie → Délai avant l'arrêt des manettes → Ne pas éteindre**

Si vous ne l'éteignez pas, tout fonctionne normalement.

---

## ⚠️ Important : choisissez « Partie rapide / Invité » pour la manette externe

Quand vous appuyez sur PS sur une manette externe, la console demande qui va l'utiliser. Choisissez **Partie rapide / Invité**. Votre utilisateur principal garde la DualSense, et vous pouvez y revenir à tout moment en reprenant la DualSense.

---

# Compatibilité des manettes

| Manette | État | Remarques |
| --- | --- | --- |
| **DualShock 4** | ✅ **Vérifiée** | Mettez-la en mode association (Share + PS) à chaque association. |
| **8BitDo SF30 Pro (mode Switch)** | ✅ **Vérifiée** | Elle apparaît comme une Switch Pro Controller. Utilisez Configurer PS pour la touche PS. |
| **GameSir G7 SE** | ✅ **Vérifiée** | |
| **Manette Nintendo Switch Pro** | 🧪 **Prise en charge, pas encore testée** | Même protocole que la 8BitDo en mode Switch. |
| **Manettes Xbox** | ✅ **Fonctionnent (selon les utilisateurs)** | Des utilisateurs confirment qu'elles fonctionnent depuis la 0.8. Si la vôtre ne se connecte pas, envoyez-moi votre journal et son modèle. |
| **Toute autre manette HID Bluetooth** | 🧪 **Profil générique** | Peut fonctionner sans profil spécifique. |

**Manettes que nous pensons compatibles** (prises en charge dans le code, pas encore testées sur une console) : DualShock 4 v1, Hori Onyx, Razer Raiju Ultimate, Razer Raiju Tournament Edition, SCUF Vantage 2, Nintendo Switch Pro Controller, Nintendo Switch Online (SNES, N64, Mega Drive / Genesis), GameSir G3s, G4s, G7 Pro, T1s et T2a, Betop 2585N2, Bionik Vulkan, LanShen X1Pro, Xiaomi Mi Controller, Amazon Fire / iPega, iPega 9-series, 8BitDo SN30 Pro pour Xbox Cloud Gaming, 8BitDo Ultimate 2.4G, SteelSeries, manette NVIDIA Shield (2017), Razer Serval, PowerA MOGA Hero, MOGA Pro et MOGA Pro 2, manette Atari VCS Modern, Hyperkin Scout, Mocute 050. Si vous en essayez une, envoyez-moi votre journal.

La liste complète, manette par manette, se trouve dans [COMPATIBILITY.md](COMPATIBILITY.md).

---

# Comment ça marche

WhateverPad PS5 s'exécute comme un payload résident sur une PS5 jailbreakée. Il utilise la puce Bluetooth de la console, **en la partageant avec le système**, pour trouver et associer des manettes, lire leurs commandes et les présenter à la PS5 comme des DualSense virtuelles.

Il **ne** modifie **ni** les jeux, **ni** le firmware de la PS5, **ni** le jailbreak. Votre PS5 doit déjà pouvoir exécuter des payloads.

---

# Utilisation

1. **Chargez WhateverPad.** Téléchargez `WhateverPad-PS5-0.9.5-beta.elf` depuis les [Releases](https://github.com/andrescera/WhateverPad/releases) et chargez-le avec votre chargeur de payloads habituel.
2. **Ouvrez la page.** Ouvrez **WhateverPad** dans la rangée Médias de la console, ou `http://<ip-de-la-console>:8095` depuis un téléphone ou un ordinateur du même réseau.
3. **Associez.** Appuyez sur **Associer**, puis mettez la manette en mode association, près de la console.

| Manette | Mode association |
| --- | --- |
| **DualShock 4** | Maintenez **Share + PS** jusqu'à ce que la barre lumineuse clignote deux fois |
| **8BitDo (mode Switch)** | Maintenez **Pair** environ 3 secondes, jusqu'à ce que les voyants clignotent vite |
| **Nintendo Switch Pro** | Appuyez sur le petit bouton de synchronisation près du port USB-C |
| **Autres manettes** | Suivez les instructions du fabricant |

4. **Choisissez l'utilisateur.** Quand la console le demande, choisissez **Partie rapide / Invité**.

Plusieurs manettes peuvent être utilisées en même temps, avec la DualSense.

---

# Chaque exécution part de zéro

WhateverPad ne garde pas les manettes associées d'une exécution à l'autre. Associez vos manettes à chaque démarrage d'WhateverPad. Cela évite les anciennes associations qui ne fonctionnent plus et laisse la puce Bluetooth dans un état connu.

Si une manette qui avait été éteinte se reconnecte toute seule et ne répond pas, éteignez-la et associez-la de nouveau depuis la page.

---

# Arrêter WhateverPad

Arrêtez-le toujours avec **Arrêter WhateverPad PS5** sur sa page, y compris avant de le relancer et avant d'éteindre ou de redémarrer la console. Le bouton **Quitter** ferme seulement la page.

---

# Dépannage

### Aucune manette n'est trouvée

1. Appuyez d'abord sur **Associer**, puis mettez la manette en mode association.
2. Vérifiez que la manette est vraiment en mode association (par exemple, la barre lumineuse de la DualShock 4 doit clignoter deux fois).
3. Si la manette a déjà été associée à un autre appareil et n'apparaît toujours pas, réinitialisez-la (DualShock 4 : un trombone dans le petit trou près de L2 pendant environ 5 secondes ; 8BitDo Pro 2 : maintenez Pair environ 5 secondes) et réessayez.

### Un autre payload de manettes est en cours d'exécution

Un autre payload qui utilise la puce Bluetooth et le port 8095 gêne WhateverPad. Arrêtez-le depuis son propre menu avant de lancer WhateverPad ; WhateverPad affiche un avertissement quand il le détecte.

### La page ne s'ouvre pas

Vérifiez qu'WhateverPad fonctionne, que votre téléphone ou ordinateur est sur le même réseau que la PS5 et que vous utilisez l'adresse IP de la console avec le port `8095`.

---

# Aidez-nous à confirmer la compatibilité

Si vous pouvez confirmer que WhateverPad PS5 fonctionne (ou ne fonctionne pas) avec une manette, envoyez le journal (`/data/whateverpad/whateverpad.log`) à [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com), avec le modèle de la manette, le firmware de la console, le jailbreak et la version d'WhateverPad. Cela aide beaucoup.

### Soutenir le projet

Beaucoup de personnes me demandent d'essayer d'autres manettes et d'autres versions de la console. Toute aide est la bienvenue pour **porter ce projet sur PS4** et sur le **firmware 13.60 de la PS5**, et pour prendre en charge **davantage de manettes**. J'aimerais beaucoup avoir une console en firmware 13.60 en plus de la mienne en 10.01. Ce n'est pas obligatoire, mais ceux qui veulent aider le projet peuvent le faire sur ma page Throne : **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# Remerciements

WhateverPad PS5 est un fork d'[AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) par elmonomalva0. Ces projets ont été étudiés pour comprendre le fonctionnement des choses, et leurs idées ont été réécrites pour WhateverPad. Merci à leurs auteurs :

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore) : l'idée de la partie Bluetooth, partager la puce Bluetooth de la PS5 avec le système.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly) : les profils de manettes par fabricant et modèle, et le format des rapports de nombreuses manettes.
- **Ghostcontrol** (StonedModder) : comment créer une DualSense virtuelle sur la PS5 et l'attribuer à un utilisateur.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) et **[shadPS4](https://github.com/shadps4-emu/shadPS4)** : la séquence de la manette virtuelle et le format des données de la manette.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): des idées sur la négociation Bluetooth.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom) : les outils avec lesquels tout est compilé.

Licence : GPL-3.0-or-later ([LICENSE](LICENSE)).

Auteur : **elmonomalvad0**, auteur d'[AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5).
X : [@elmonomalvad0](https://x.com/elmonomalvad0) · Contact : exposed.ct.ws@gmail.com
