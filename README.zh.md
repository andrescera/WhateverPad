<p align="center"><img src="assets/icon0.png" width="180" alt="WhateverPad PS5"></p>

# WhateverPad PS5：让你的 PS5 摆脱 DualSense

**适用于已越狱 PS5 的蓝牙手柄桥接工具。**

[English · Español](README.md) · [Français](README.fr.md) · [Italiano](README.it.md) · [Português](README.pt.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · **简体中文** · [繁體中文](README.zh-Hant.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [العربية](README.ar.md)

---

WhateverPad PS5 让你在 PS5 上使用其他平台的手柄。它通过主机自带的蓝牙芯片连接这些手柄，并以虚拟 DualSense 手柄的形式呈现给系统。

无需 USB 适配器。

**版本 0.9.5-beta。** 已在 PS5 fat、固件 10.01 上，使用 DualShock 4 和 8BitDo SF30 Pro（Switch 模式）测试，涵盖主机菜单、游戏和模拟器。

**提供十二种语言：** 英语、西班牙语、法语、意大利语、葡萄牙语、德语、俄语、简体中文、繁体中文、日语、韩语和阿拉伯语。WhateverPad 页面会跟随主机的语言。

---

## 关于这个项目

现在的目标是让 WhateverPad 支持更多手柄。如果你试用了它，请把你的日志发给我（见下文“帮助我们确认兼容性”）：每一份日志都有帮助。

完整的源代码已在本仓库中以 GPL-3.0-or-later 许可发布。如果你开发模拟器并想集成 WhateverPad，可以在该许可下进行。如需帮助，请发邮件到 [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com)。

仍有一个已知问题：如果在 WhateverPad 运行时关闭 DualSense，它可能会一直蓝灯闪烁。我希望今后的项目不会有这个问题。基于我发布的 WhateverPad 代码的分支（fork）很可能会继承这个问题。这是 WhateverPad 留下的印记，也说明至少在蓝牙部分，它们是 WhateverPad 的分支。

---

## 从源码构建

你需要 [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)。

```
make test                                               # 在电脑上运行测试，使用蓝牙模拟器
make ps5 PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk       # 在 dist/ 中生成 ELF
tools/build-elf-docker.sh /path/to/ps5-payload-sdk      # 或者在容器中完成同样的事
```

## 代码结构

| 位置 | 作用 |
| --- | --- |
| `src/host.c` | 蓝牙经典主机：搜索、呼叫、配对、L2CAP 通道、SDP 和 HID |
| `src/le.c` | 蓝牙 LE：扫描、使用 Secure Connections 配对并交换身份、基于 GATT 的 HID |
| `src/profiles.c` | 每个手柄系列一个配置：标识、如何读取报告、唤醒序列、震动和灯光 |
| `src/generic.c`, `maps/` | 其他任何 HID 手柄，通过其自身的描述符和可选的映射文件读取 |
| `src/hci_usb.c`, `src/usb_desc.c`, `src/evstream.c` | 通过 USB 使用主机蓝牙芯片的 HCI，该芯片与系统共用 |
| `src/ps5_vpad.c` | 创建虚拟 DualSense 并向其输送数据 |
| `src/web.c`, `web/index.html` | 页面及其 API |
| `src/ps5_main.c` | payload 的主循环 |
| `tests/` | 蓝牙模拟器（`sim.c`、`sim_le.c`）和所有测试 |

添加一款手柄，大多数情况下就是在 `src/profiles.c` 中添加一个配置，并用模拟器写一个测试。详情见 [CONTRIBUTING.md](CONTRIBUTING.md)。

---

## ⚠️ 重要：WhateverPad 运行时不要关闭 DualSense

如果在 WhateverPad 运行时关闭 DualSense，它可能进入蓝灯闪烁模式，在主机重启前无法使用。

为避免主机在一段时间不用后自动将其关闭，请设置：

**设置 → 系统 → 节能 → 设置手柄关闭前的时间 → 不关闭**

如果你不关闭它，一切正常。

---

## ⚠️ 重要：外接手柄请选择“快速游戏 / 访客”

当你在外接手柄上按下 PS 键时，主机会询问由谁使用。请选择**快速游戏 / 访客**。你的主用户保留 DualSense，随时拿起 DualSense 即可回到它。

---

# 手柄兼容性

| 手柄 | 状态 | 备注 |
| --- | --- | --- |
| **DualShock 4** | ✅ **已验证** | 每次配对都要让它进入配对模式（Share + PS）。 |
| **8BitDo SF30 Pro（Switch 模式）** | ✅ **已验证** | 显示为 Switch Pro Controller。PS 键请使用“设置 PS”。 |
| **GameSir G7 SE** | ✅ **已验证** | |
| **Nintendo Switch Pro Controller** | 🧪 **已支持，尚未测试** | 与 Switch 模式下的 8BitDo 协议相同。 |
| **Xbox 手柄** | ✅ **可用（用户反馈）** | 多位用户确认从 0.8 起可以使用。如果你的手柄无法连接，请把日志和型号发给我。 |
| **任何其他蓝牙 HID 手柄** | 🧪 **通用配置** | 没有专用配置也可能可用。 |

**我们认为可能兼容的手柄**（代码中已支持，尚未在主机上测试）：DualShock 4 v1、Hori Onyx、Razer Raiju Ultimate、Razer Raiju Tournament Edition、SCUF Vantage 2、Nintendo Switch Pro Controller、Nintendo Switch Online（SNES、N64、Mega Drive / Genesis）、GameSir G3s、G4s、G7 Pro、T1s 和 T2a、Betop 2585N2、Bionik Vulkan、LanShen X1Pro、Xiaomi Mi Controller、Amazon Fire / iPega、iPega 9-series、8BitDo SN30 Pro（Xbox Cloud Gaming 版）、8BitDo Ultimate 2.4G、SteelSeries、NVIDIA Shield 手柄（2017）、Razer Serval、PowerA MOGA Hero、MOGA Pro 和 MOGA Pro 2、Atari VCS Modern 手柄、Hyperkin Scout、Mocute 050。如果你试用了其中任何一款，请把日志发给我。

完整的逐款列表见 [COMPATIBILITY.md](COMPATIBILITY.md)。

---

# 工作原理

WhateverPad PS5 以常驻 payload 的形式运行在已越狱的 PS5 上。它使用主机的蓝牙芯片，**与系统共享**，来发现并配对手柄、读取输入，并把它们作为虚拟 DualSense 呈现给 PS5。

它**不会**修改游戏、PS5 固件或越狱本身。你的 PS5 必须已经能够运行 payload。

---

# 使用方法

1. **加载 WhateverPad。** 从 [Releases](https://github.com/andrescera/WhateverPad/releases) 下载 `WhateverPad-PS5-0.9.5-beta.elf`，用你平时使用的 payload 加载器加载。
2. **打开页面。** 在主机的“媒体”一行打开 **WhateverPad**，或在同一网络的手机或电脑上打开 `http://<主机IP>:8095`。
3. **配对。** 按下**配对**，然后在靠近主机的地方让手柄进入配对模式。

| 手柄 | 配对模式 |
| --- | --- |
| **DualShock 4** | 按住 **Share + PS**，直到灯条闪烁两次 |
| **8BitDo（Switch 模式）** | 按住 **Pair** 约 3 秒，直到指示灯快速闪烁 |
| **Nintendo Switch Pro** | 按下 USB-C 接口旁的小型同步键 |
| **其他手柄** | 按照制造商的说明操作 |

4. **选择用户。** 主机询问时，选择**快速游戏 / 访客**。

可以同时使用多个手柄，并与 DualSense 一起使用。

---

# 每次运行都从零开始

WhateverPad 不会在两次运行之间保存已配对的手柄。每次启动 WhateverPad 都要重新配对手柄。这样可以避免失效的旧配对，并让蓝牙芯片处于已知状态。

如果一个被关闭的手柄自行重新连接却没有反应，请将其关闭，然后在页面上重新配对。

---

# 停止 WhateverPad

请始终使用页面上的**关闭 WhateverPad PS5**来停止它，包括再次运行之前，以及关闭或重启主机之前。**退出**按钮只会关闭页面。

---

# 故障排除

### 找不到任何手柄

1. 先按**配对**，之后再让手柄进入配对模式。
2. 确认手柄确实处于配对模式（例如 DualShock 4 的灯条必须闪烁两次）。
3. 如果手柄以前与其他设备配对过，仍然没有出现，请将其重置（DualShock 4：用回形针按 L2 旁的小孔约 5 秒；8BitDo Pro 2：按住 Pair 约 5 秒），然后重试。

### 另一个手柄 payload 正在运行

另一个使用蓝牙芯片和 8095 端口的 payload 会造成干扰。运行 WhateverPad 之前，请先在它自己的菜单中停止它；WhateverPad 检测到这种情况时会显示提示。

### 页面打不开

请确认 WhateverPad 正在运行，手机或电脑与 PS5 在同一网络，并且使用的是主机的 IP 地址加端口 `8095`。

---

# 帮助我们确认兼容性

如果你能确认 WhateverPad PS5 可以（或不可以）与某款手柄配合使用，请把日志（`/data/whateverpad/whateverpad.log`）发送到 [exposed.ct.ws@gmail.com](mailto:exposed.ct.ws@gmail.com)，并注明手柄型号、主机固件、越狱方式和 WhateverPad 版本。这非常有帮助。

### 支持这个项目

很多人请我测试其他手柄和其他主机固件。非常欢迎任何帮助，把本项目**移植到 PS4** 和 **PS5 固件 13.60**，并支持**更多手柄**。除了我这台 10.01 的主机，我也很想有一台 13.60 固件的主机。这不是强制的，但想帮助这个项目的人可以通过我的 Throne 页面：**[throne.com/elmonomalvad0](https://throne.com/elmonomalvad0)**

---

# 致谢

WhateverPad PS5 是 [AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) 的分支，作者 elmonomalva0。我们研究了这些项目以理解其工作方式，并为 WhateverPad 重新编写了其中的思路。感谢它们的作者：

- **[FGG-PlayPods](https://github.com/FGGstore/FGG-PlayPods)**（FGGstore）：蓝牙部分的思路，即与系统共享 PS5 自带的蓝牙芯片。
- **[MissionControl](https://github.com/ndeadly/MissionControl)**（ndeadly）：按厂商和型号划分的手柄配置，以及许多手柄的报告格式。
- **Ghostcontrol**（StonedModder）：如何在 PS5 上创建虚拟 DualSense 并分配给用户。
- **[SplashDown](https://github.com/seregonwar/SplashDown)**（seregonwar）和 **[shadPS4](https://github.com/shadps4-emu/shadPS4)**：虚拟手柄的流程和手柄数据格式。
- **[HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5)**（X-F1REBALL-X）：关于蓝牙协商的思路。
- **[ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk)**（John Törnblom）：用于构建一切的工具链。

许可证：GPL-3.0-or-later（[LICENSE](LICENSE)）。

作者：**elmonomalvad0**，[AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5) 的作者。
X：[@elmonomalvad0](https://x.com/elmonomalvad0) · 联系：exposed.ct.ws@gmail.com
