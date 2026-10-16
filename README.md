<div align="center">
  <img src="images/readme-cover.png" width="260" alt="鲸鱼娘桌宠">
  <h1>鲸鱼娘桌宠 · Desktop Companion</h1>
  <p>一个住在你桌面上的 Live2D 小女孩。</p>
</div>

---

## 中文

### 她是谁

一只用 **Live2D Cubism** 实时驱动的桌宠：她会**盯着你的鼠标**、眨眼、招手、被摸头，也会玩狗尾巴草、钻纸箱、贴便笺、扒饭、盖眼罩打盹。

更重要的是——**她陪你一起工作**。接上你的 AI 编码助手（**Codex / Claude / WorkBuddy**）后，你一开工她就坐到你旁边上班，久了她会伸个懒腰，收工了回头看你一眼。

<p align="center"><img src="images/readme-desk.png" width="320" alt="陪她工作"></p>

### 特性

- **表情与互动**：通用注视（眼球+头随光标）、变化眨眼（含双眨）、挥手、摸头、拖拽随动与贴边吸附。
- **道具场景**：狗尾巴草、纸箱躲猫猫、便笺、来碗饭香、眼罩小睡。
- **工作陪伴**：与她并坐的工作桌面；连续工作约 10 秒后入座，中途「歇一下」伸展，收工「回望」，忙满一轮还有一句「完成庆祝」。
- **AI 助手联动**：Codex / Claude / WorkBuddy 的钩子安装器，工作回合自动驱动她的状态。
- **桌面体验**：透明无边框、逐像素点穿、不抢焦点；托盘菜单、全局热键 **Ctrl+Alt+H**、滚轮缩放、性能档位（省电/标准/流畅）。

### 安装（Windows）

1. 下载 **`DesktopCompanion-1.0.0-setup.exe`** 并运行；**免管理员**（装到当前用户目录）。
2. 安装向导里可选：**接入 AI 助手钩子**（默认勾选）、**开机自启**、**桌面快捷方式**。
3. 装完她就会出现在桌角。**右键托盘图标**打开菜单。

> 卸载时会自动移除装上去的钩子，不留痕。

### 让桌宠跟随你的 AI 助手

安装时勾选「接入 AI 助手钩子」即可。手动重装钩子：

```powershell
# 默认给所有已检测到的客户端装（Codex / WorkBuddy / Claude）
powershell -ExecutionPolicy Bypass -File tools\install-hooks.ps1
# 或指定：-Clients codex | workbuddy | claude | all
```

各家位置不同，安装器已分别处理：

| 客户端 | 钩子位置 | 说明 |
| --- | --- | --- |
| Codex | `~/.codex/hooks.json` | — |
| WorkBuddy | `~/.workbuddy/settings.json` | — |
| Claude Code（CLI） | `~/.claude/settings.json` | 命令行版会读 |
| **Claude 桌面端** | `<每个会话>\.claude\settings.json` | 桌面端用 `--setting-sources=user`，只读会话级配置；`tools/seed-cowork-hooks.ps1` 负责补种（可配计划任务） |

> 桌宠必须**开着**，钩子才有接收方。

### 从源码构建

**依赖**：Windows 10/11 · Visual Studio 2022（MSVC）· CMake + Ninja · **Qt 6.8** · Conan · **Live2D Cubism SDK for Native 5-r.5**。

> Cubism SDK 属 Live2D 专有，**本仓库不含**；请自行到 Live2D 官网获取并放到仓库根目录的 `CubismSdkForNative-5-r.5/`。

```bat
:: 官方入口（含 conan/configure/windeployqt/ctest；需要完整 vcvarsall 环境）
tools\build.cmd
:: 受限终端里改用（手工拼 MSVC 环境，只做增量构建 + ctest）
tools\dev.cmd
:: 打包（portable zip；装了 Inno Setup 6 还会出 setup.exe）
powershell -ExecutionPolicy Bypass -File tools\package.ps1 -Version 1.0.0
```

### 目录结构

```
src/          C++ 运行时（Qt + Live2D Cubism native），行为模块在 src/motion/
assets/       Live2D 模型、动作曲线、道具贴图
art/          美术源与评审历史（PSD、切图、验收图）
tools/        构建 / 打包 / 钩子安装 / 建模脚本
installer/    Inno Setup 脚本 + portable 一键脚本
tests/        ctest 单元测试
godot/        早期 Godot 渲染端（冻结保留）
```

### 许可与致谢

- **角色美术与模型**：作者自制。
- **运行时**：Live2D Cubism Core（**Live2D 专有**）与 Qt 6（LGPL）。使用/分发请遵守各自许可。

---

## English

### What it is

A real-time **Live2D Cubism** desktop pet (Windows). She **watches your cursor**, blinks, waves, and lets you pat her head; she plays with a foxtail, hides in a box, leaves sticky notes, eats, and naps.

And she **works alongside you**: hooked to your AI coding agent (**Codex / Claude / WorkBuddy**), she sits down at her desk when you start, stretches when you've been at it a while, and glances back when you stop.

### Features

- **Face & interaction** — gaze (eyes + head follow the cursor), varied blinks (incl. double), wave, head-pat, drag-with-lag and edge snapping.
- **Prop scenes** — foxtail, box peek-a-boo, sticky note, rice bowl, eye-mask nap.
- **Work companion** — a shared desk; she seats herself after ~10 s of a work turn, takes a "歇一下" stretch, looks back when the session ends, and gives a little cheer for a turn that ran a while.
- **Agent hooks** — an installer for Codex / Claude / WorkBuddy drives her state from real work turns.
- **Desktop feel** — transparent frameless window, per-pixel click-through, never steals focus; tray menu, global hotkey **Ctrl+Alt+H**, wheel zoom, performance tiers.

### Install (Windows)

1. Download **`DesktopCompanion-1.0.0-setup.exe`** and run it — **no admin needed** (per-user install).
2. Optional steps in the wizard: **install AI-agent hooks** (on by default), **start at login**, **desktop shortcut**.
3. She shows up in a corner of your desktop. **Right-click the tray icon** for the menu.

> Uninstalling removes the hooks it installed.

### Agent hooks

Tick "install AI-agent hooks" during setup, or run it by hand:

```powershell
powershell -ExecutionPolicy Bypass -File tools\install-hooks.ps1   # -Clients codex|workbuddy|claude|all
```

| Client | Hook location |
| --- | --- |
| Codex | `~/.codex/hooks.json` |
| WorkBuddy | `~/.workbuddy/settings.json` |
| Claude Code (CLI) | `~/.claude/settings.json` |
| **Claude Desktop** | per-session `.claude\settings.json` (it runs with `--setting-sources=user`; `tools/seed-cowork-hooks.ps1` seeds them) |

The pet must be **running** to receive the events.

### Build from source

**Requirements**: Windows 10/11 · Visual Studio 2022 (MSVC) · CMake + Ninja · **Qt 6.8** · Conan · **Live2D Cubism SDK for Native 5-r.5**.

> The Cubism SDK is proprietary and **not in this repo**; fetch it from Live2D and place it at `CubismSdkForNative-5-r.5/`.

```bat
tools\build.cmd                                   :: official (conan + configure + windeployqt + ctest)
tools\dev.cmd                                     :: restricted-terminal build + ctest
powershell -File tools\package.ps1 -Version 1.0.0 :: portable zip, and setup.exe if Inno Setup 6 is installed
```

### Layout

```
src/          C++ runtime (Qt + Live2D Cubism native); behaviour modules in src/motion/
assets/       Live2D model, motion curves, prop textures
art/          art sources and review history (PSD, cut layers, approval renders)
tools/        build / packaging / hook installers / authoring scripts
installer/    Inno Setup script + portable one-click scripts
tests/        ctest unit tests
godot/        earlier Godot renderer (frozen, kept)
```

### License & credits

- **Character art & model**: by the author.
- **Runtime**: Live2D Cubism Core (**proprietary**) and Qt 6 (LGPL) — follow their respective licenses when using or redistributing.
