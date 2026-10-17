# 更新日志 · Changelog

## 1.0.1 — 2026-10-17

修掉三个用户报的问题，外加 Claude 桌面端联动方式的一次改造。

### 中文

**修复**
- **思考冒泡在靠屏幕右边时被切掉**：她坐在屏幕右缘时，"?" 冒泡会整块跑到屏幕外（而她自己看着还是好好的），把窗口拖回来也修不好。根因不在画：冒泡画在**窗口右侧三分之一**，而窗口原本允许最多探出屏幕右缘 184px。现在**整个窗口必须留在屏幕内**（拖拽、缩放、启动恢复位置都走同一条约束），贴边吸附照旧；历史遗留的越界位置启动时会自愈。
- **Codex 读不了钩子配置**：安装器写出的 `~/.codex/hooks.json` 带 UTF-8 BOM，Codex 会**整个拒读**，桌宠于是静默不再反应。原因是 PowerShell 5.1 的 `Set-Content -Encoding utf8` 会写 BOM（pwsh 7 不会）。现在所有钩子配置一律以**无 BOM UTF-8** 写出。
- **开发构建其实一直没签名**：签名脚本的时间戳要走直连网络，本机只能走代理，所以每次构建都在静默发**未签名**的 exe（正是脚本想避免的"未知发布者"提示）。现在时间戳不可达时退回不带时间戳签名。

**改动**
- **思考时不再咧嘴笑**：之前"想到办法"那一拍会把嘴张开成露齿笑，观感诡异。现在改成**闭口浅笑**（同一拍的表现力由嘴型、脸颊和眼眸承担）。
- **Claude 桌面端的钩子改由桌宠自己补种**：桌面端只认**每个会话各自的** `.claude/settings.json`，所以每个新对话都得补一次。以前靠一个每 2 分钟跑一次的计划任务，PowerShell 会**闪一下黑框**；现在桌宠启动后自己静默补种（进程内、不弹窗），随安装包一起生效。只想手动补一次时仍可用 `tools\seed-cowork-hooks.ps1`；卸载时同一脚本的 `-Remove` 会把补种过的会话配置清干净，所以「卸载不留痕」照旧成立。

**安装包**
- `DesktopCompanion-1.0.1-setup.exe`。

---

### English

**Fixes**
- **The thinking bubble was cut off near the right edge of the screen.** With the pet docked right, the "?" bubble moved entirely off-screen while she still looked fine, and dragging her back did not repair it. The cause was not the painting: the bubble is drawn in the **right third of the window**, and the window was allowed to hang up to 184 px past the screen edge. The **whole window must now stay on screen** (drag, zoom and the startup restore all use the same rule); edge snapping is unchanged, and a position saved under the old rule heals on the next launch.
- **Codex could not read its hook config.** The installer wrote `~/.codex/hooks.json` with a UTF-8 BOM, which Codex rejects outright — the pet then silently stopped reacting. Cause: PowerShell 5.1's `Set-Content -Encoding utf8` stamps a BOM (pwsh 7 does not). Every hook config is now written as **BOM-less UTF-8**.
- **Dev builds were shipping unsigned.** The signing script's timestamp step needs a direct connection; this host has only a proxy, so every rebuild silently produced an **unsigned** exe — exactly the "unknown publisher" prompt the script exists to prevent. Signing now falls back to no timestamp when the timestamp server is unreachable.

**Changes**
- **No more grin while thinking.** The "eureka" beat used to open the mouth into a bare-toothed grin, which read as creepy. It is now a **closed smile** (mouth shape, cheek and eye smile carry the beat).
- **The pet now installs the Claude Desktop hooks itself.** The desktop app only reads the **per-session** `.claude/settings.json`, so every new conversation needs seeding. That used to be a Scheduled Task running every two minutes, which flashed a console window; the pet now does it in-process and windowless, and the behaviour ships with the installer. `tools\seed-cowork-hooks.ps1` remains for a manual one-off, and uninstall runs the same script with `-Remove` — so the seeded per-session configs are cleaned up too and uninstalling still leaves no trace.

**Installer**
- `DesktopCompanion-1.0.1-setup.exe`.

---

## 1.0.0 — 2026-10-16

**首个正式版。** 一个用 Live2D 实时驱动的鲸鱼娘桌宠，能陪你的 AI 编码助手一起工作。

### 中文

**表情与互动**
- 通用注视：眼球与头部始终跟随光标；光标静止一会儿后她会自己四处看看。
- 眨眼有变化：间隔抖动（约 2.8–6.5 秒），约 18% 是双眨。
- 挥手（按住她约 0.6 秒触发）、摸头、拖拽随动、松手贴边吸附。

**道具与场景**
- 狗尾巴草、纸箱躲猫猫、便笺、来碗饭香、眼罩小睡。

**工作陪伴（本版核心）**
- 她有一张自己的工作台。接上 AI 助手后，**一个工作回合持续约 10 秒她就入座**陪你忙；连续工作会「歇一下」伸展；收工时回头看你一眼（收工回望），忙满 30 秒的一轮还会来一记「完成庆祝」。
- 开工时会有一个轻量的「开工致意」。这些动作都用了**身体/剪影轴**，在桌宠真实尺寸（约 280px）下也看得清。

**AI 助手联动**
- 统一的钩子安装器，支持 **Codex / Claude（桌面端 + CLI）/ WorkBuddy**，自动适配各自的钩子位置与命令格式；安装/卸载都只动桌宠自己的条目。

**桌面体验**
- 透明无边框、逐像素点穿、不抢焦点；托盘右键菜单（已重整分组）、全局热键 **Ctrl+Alt+H**、滚轮缩放（180–560px）、性能档位（省电/标准/流畅）；隐藏时暂停渲染。

**修复**
- 修好了「一打开就是坐姿、什么动作都没有」：工位场景改为**只在工作回合出现**、连续工作 10 秒后才入座；并修了一颗会让模型**整载不入**的陈旧哈希。
- 客户端被**硬杀**（不发收工信号）时，桌宠不再卡「忙」12 小时：新增托盘「结束忙碌（复位）」，过期门限缩到 4 小时。

**安装包**
- `DesktopCompanion-1.0.0-setup.exe`：每用户安装、**免管理员**；可选接入钩子 / 开机自启 / 桌面快捷方式；卸载自动移除钩子。

---

### English

**Face & interaction**
- Presence gaze: eyes and head follow the cursor; she looks around on her own once it's still.
- Varied blinks (jittered 2.8–6.5 s, ~18% double).
- Wave (hold her ~0.6 s), head-pat, drag-with-lag, and edge snapping on release.

**Props & scenes**
- Foxtail, box peek-a-boo, sticky note, rice bowl, eye-mask nap.

**Work companion (the headline)**
- She has her own desk. With an AI agent hooked up, **she seats herself after ~10 s of a work turn**; a long session earns a "歇一下" stretch; when you stop she glances back, and a turn that ran ≥30 s ends with a little cheer.
- A light "hello" plays when work starts. These beats drive the **body/silhouette** parameters so they actually read at the pet's real ~280 px size.

**Agent hooks**
- One installer for **Codex / Claude (Desktop + CLI) / WorkBuddy**, handling each client's hook location and command format; install/uninstall touches only the pet's own entries.

**Desktop feel**
- Transparent frameless window, per-pixel click-through, never steals focus; regrouped tray menu, global hotkey **Ctrl+Alt+H**, wheel zoom (180–560 px), performance tiers; rendering pauses while hidden.

**Fixes**
- "Opens sitting with no actions": the desk scene now appears **only during a work turn**, and only after 10 s; plus a stale metadata hash that made the model refuse to load at all.
- A hard-killed client no longer leaves her "busy" for 12 h: a tray **reset** item, and the stale-turn expiry shortened to 4 h.

**Installer**
- `DesktopCompanion-1.0.0-setup.exe`: per-user, **no admin**; optional agent hooks / autostart / desktop shortcut; uninstall removes the hooks.
