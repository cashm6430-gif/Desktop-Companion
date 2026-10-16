# 更新日志 · Changelog

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
