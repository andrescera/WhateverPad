<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5: liberte a sua PS5 do DualSense

**Uma ponte para comandos Bluetooth em PS5 com jailbreak.**

[English · Español](README.md) · [Français](README.fr.md) · [Italiano](README.it.md) · **Português** · [Deutsch](README.de.md) · [Русский](README.ru.md) · [简体中文](README.zh.md) · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [العربية](README.ar.md)

---

O WhateverPad PS5 permite-lhe usar na sua PS5 comandos de outras plataformas. Liga-os através do chip Bluetooth da própria consola e apresenta-os ao sistema como DualSense virtuais.

Não é preciso nenhum adaptador USB.

**Versão 0.9.5-beta.** Testada numa PS5 fat, firmware 10.01, com um DualShock 4 e um 8BitDo SF30 Pro (modo Switch), nos menus da consola, em jogos e em emuladores.

**Disponível em doze idiomas:** inglês, espanhol, francês, italiano, português, alemão, russo, chinês simplificado, chinês tradicional, japonês, coreano e árabe. A página do WhateverPad segue o idioma da consola.

---

## Sobre este projeto

O objetivo agora é que o WhateverPad funcione com mais comandos. Se o experimentar, envie-me o seu registo (veja «Ajude-nos a confirmar a compatibilidade» mais abaixo): cada registo ajuda.

O código-fonte completo está publicado neste repositório com licença GPL-3.0-or-later. Se desenvolve um emulador e quer integrar o WhateverPad, pode fazê-lo ao abrigo dessa licença. Escreva-me para [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com) se precisar de ajuda.

Resta um problema conhecido: se o DualSense for desligado enquanto o WhateverPad está a funcionar, pode ficar a piscar a azul. Espero que os projetos futuros não o tenham. Os projetos derivados do WhateverPad, do código que publiquei, muito provavelmente herdarão este problema. Faz parte do legado do WhateverPad, e é um sinal de que, pelo menos na parte Bluetooth, são forks do WhateverPad.

---

## Compilar a partir do código

Precisa do [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

```
make test                                               # testes no computador, com o simulador Bluetooth
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk       # o ELF em dist/
tools/build-elf-docker.sh /path/to/ps5-payload-sdk      # ou o mesmo, dentro de um contentor
```

## Dentro do código

| Onde | O que faz |
| --- | --- |
| `src/host.c` | O anfitrião Bluetooth Classic: pesquisa, chamadas, emparelhamento, canais L2CAP, SDP e HID |
| `src/le.c` | Bluetooth LE: pesquisa, emparelhamento com Secure Connections e troca de identidade, HID sobre GATT |
| `src/profiles.c` | Um perfil por família de comandos: ids, como ler os relatórios, sequência de despertar, vibração e luzes |
| `src/generic.c`, `maps/` | Qualquer outro comando HID, lido através do seu próprio descritor e de um ficheiro de mapeamento opcional |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | O HCI por USB com o chip Bluetooth da consola, partilhado com o sistema |
| `src/ps5_vpad.c` | Cria o DualSense virtual e alimenta-o |
| `src/web.c`, `web/index.html` | A página e a sua API |
| `src/ps5_main.c` | O ciclo principal do payload |
| `tests/` | Os simuladores Bluetooth (`sim.c`, `sim_le.c`) e todos os testes |

Adicionar um comando é, na maioria dos casos, adicionar um perfil em `src/profiles.c` e um teste com o simulador. Os detalhes estão em [CONTRIBUTING.md](CONTRIBUTING.md).

---

## ⚠️ Importante: não desligue o DualSense com o WhateverPad a funcionar

Se o DualSense for desligado enquanto o WhateverPad está a funcionar, pode entrar no modo azul intermitente e deixar de funcionar até a consola ser reiniciada.

Para que a consola não o desligue sozinha após algum tempo sem uso, configure:

**Definições → Sistema → Poupança de energia → Definir tempo até os comandos desligarem → Não desligar**

Se não o desligar, tudo funciona normalmente.

---

## ⚠️ Importante: escolha «Jogo rápido / Convidado» para o comando externo

Quando prime PS num comando externo, a consola pergunta quem o vai usar. Escolha **Jogo rápido / Convidado**. O seu utilizador principal fica com o DualSense, e pode voltar a ele a qualquer momento pegando de novo no DualSense.

---

# Compatibilidade de comandos

| Comando | Estado | Notas |
| --- | --- | --- |
| **DualShock 4** | ✅ **Verificado** | Coloque-o em modo de emparelhamento (Share + PS) sempre que o emparelhar. |
| **8BitDo SF30 Pro (modo Switch)** | ✅ **Verificado** | Aparece como Switch Pro Controller. Use Configurar PS para o botão PS. |
| **GameSir G7 SE** | ✅ **Verificado** | |
| **Nintendo Switch Pro Controller** | 🧪 **Suportado, ainda não testado** | Mesmo protocolo que o 8BitDo em modo Switch. |
| **Comandos Xbox** | ✅ **Funcionam (segundo os utilizadores)** | Vários utilizadores confirmam que funcionam desde a 0.8. Se o seu não ligar, envie-me o seu registo e o modelo. |
| **Qualquer outro comando HID Bluetooth** | 🧪 **Perfil genérico** | Pode funcionar sem um perfil específico. |

**Comandos que acreditamos ser compatíveis** (suportados no código, ainda não testados numa consola): DualShock 4 v1, Hori Onyx, Razer Raiju Ultimate, Razer Raiju Tournament Edition, SCUF Vantage 2, Nintendo Switch Pro Controller, Nintendo Switch Online (SNES, N64, Mega Drive / Genesis), GameSir G3s, G4s, G7 Pro, T1s e T2a, Betop 2585N2, Bionik Vulkan, LanShen X1Pro, Xiaomi Mi Controller, Amazon Fire / iPega, iPega 9-series, 8BitDo SN30 Pro para Xbox Cloud Gaming, 8BitDo Ultimate 2.4G, SteelSeries, comando NVIDIA Shield (2017), Razer Serval, PowerA MOGA Hero, MOGA Pro e MOGA Pro 2, comando Atari VCS Modern, Hyperkin Scout, Mocute 050. Se experimentar algum, envie-me o seu registo.

A lista completa, comando a comando, está em [COMPATIBILITY.md](COMPATIBILITY.md).

---

# Como funciona

O WhateverPad PS5 corre como um payload residente numa PS5 com jailbreak. Usa o chip Bluetooth da consola, **partilhando-o com o sistema**, para encontrar e emparelhar comandos, ler os seus botões e apresentá-los à PS5 como DualSense virtuais.

**Não** modifica jogos, o firmware da PS5 nem o jailbreak. A sua PS5 já tem de conseguir executar payloads.

---

# Como usar

1. **Carregue o WhateverPad.** Descarregue `WhateverPad-PS5-0.9.5-beta.elf` em [Releases](https://github.com/andrescera/WhateverPad/releases) e carregue-o com o carregador de payloads que usa habitualmente.
2. **Abra a página.** Abra o **WhateverPad** na linha de Conteúdos multimédia da consola, ou `http://<ip-da-consola>:8095` a partir de um telemóvel ou computador da mesma rede.
3. **Emparelhe.** Prima **Emparelhar** e depois coloque o comando em modo de emparelhamento, perto da consola.

| Comando | Modo de emparelhamento |
| --- | --- |
| **DualShock 4** | Mantenha **Share + PS** até a barra de luz piscar duas vezes |
| **8BitDo (modo Switch)** | Mantenha **Pair** cerca de 3 segundos, até as luzes piscarem depressa |
| **Nintendo Switch Pro** | Prima o pequeno botão de sincronização junto à porta USB-C |
| **Outros comandos** | Siga as instruções do fabricante |

4. **Escolha o utilizador.** Quando a consola perguntar, escolha **Jogo rápido / Convidado**.

Podem usar-se vários comandos ao mesmo tempo, juntamente com o DualSense.

---

# Cada execução começa limpa

O WhateverPad não guarda comandos emparelhados entre execuções. Emparelhe os seus comandos sempre que iniciar o WhateverPad. Assim evitam-se emparelhamentos antigos que já não funcionam e o chip Bluetooth fica num estado conhecido.

Se um comando que foi desligado voltar a ligar-se sozinho e não responder, desligue-o e emparelhe-o de novo a partir da página.

---

# Desligar o WhateverPad

Pare-o sempre com **Desligar o WhateverPad PS5** na sua página, também antes de o executar de novo e antes de desligar ou reiniciar a consola. O botão **Sair** só fecha a página.

---

# Resolução de problemas

### Não encontra nenhum comando

1. Prima primeiro **Emparelhar** e coloque o comando em modo de emparelhamento depois.
2. Confirme que o comando está mesmo em modo de emparelhamento (por exemplo, a barra de luz do DualShock 4 deve piscar duas vezes).
3. Se o comando esteve emparelhado com outro aparelho e continua a não aparecer, reponha-o (DualShock 4: um clipe no pequeno orifício junto a L2 durante cerca de 5 segundos; 8BitDo Pro 2: mantenha Pair cerca de 5 segundos) e tente de novo.

### Há outro payload de comandos a funcionar

Outro payload que use o chip Bluetooth e a porta 8095 interfere. Pare-o a partir do seu próprio menu antes de executar o WhateverPad; o WhateverPad mostra um aviso quando o deteta.

### A página não abre

Confirme que o WhateverPad está a funcionar, que o telemóvel ou computador está na mesma rede que a PS5 e que usa o IP da consola com a porta `8095`.

---

# Ajude-nos a confirmar a compatibilidade

Se puder confirmar que o WhateverPad PS5 funciona (ou não funciona) com um comando, envie o registo (`/data/whateverpad/whateverpad.log`) para [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com), com o modelo do comando, o firmware da consola, o jailbreak e a versão do WhateverPad. Ajuda muito.

### Apoie o projeto

Muitas pessoas pedem-me que teste outros comandos e outras versões da consola. Qualquer ajuda é muito bem-vinda para **portar este projeto para a PS4** e para o **firmware 13.60 da PS5**, e para suportar **mais comandos**. Gostava muito de ter uma consola com o firmware 13.60 além da minha, com o 10.01. Não é obrigatório, mas quem quiser ajudar o projeto pode fazê-lo na minha página Throne: **[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# Agradecimentos

O WhateverPad PS5 é um fork do [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) de elmonomalva0. Estes projetos foram estudados para perceber como as coisas funcionam, e as suas ideias foram reescritas para o WhateverPad. Obrigado aos seus autores:

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)** (FGGstore): a ideia da parte Bluetooth, partilhar o chip Bluetooth da PS5 com o sistema.
- **[MissionControl](https://github.com/ndeadly/MissionControl)** (ndeadly): os perfis de comandos por fabricante e modelo, e o formato dos relatórios de muitos comandos.
- **Ghostcontrol** (StonedModder): como criar um DualSense virtual na PS5 e atribuí-lo a um utilizador.
- **[SplashDown](https://github.com/seregonwar/SplashDown)** (seregonwar) e **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: a sequência do comando virtual e o formato dos dados do comando.
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)** (X-F1REBALL-X): ideias sobre a negociação Bluetooth.
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)** (John Törnblom): as ferramentas com que tudo é compilado.

Licença: GPL-3.0-or-later ([LICENSE](LICENSE)).

Autor: **elmonomalvad0**, autor do [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5).
X: [@elmonomalvad0](https://x.com/elmonomalvad0) · Contacto: exposed.ct.ws@gmail.com
