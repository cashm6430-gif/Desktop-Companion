# FocusGuard —— 桌宠前台自愈守卫（关卡A窗口缺口修复）

## 问题（2026-10-08 侦破）
宠物窗口是 TOPMOST + WS_EX_NOACTIVATE，但桌面出现**前景空缺**（前台窗口被销毁，
或新窗口显示时桌面本就没有前台）时，Windows 内核会**同步**把前台强推给
新可见的 TOPMOST 窗口——宠物中招。实验证据链：

- notepad 起杀实验（制造空缺）：宠物被强推前台 3/3 复现（修复前）
- 引擎 show 序列干净：SW_SHOWNA / 全 flag 路径带 SWP_NOACTIVATE（源码核实）
- 样式排除：WS_EX_TOOLWINDOW / WS_EX_TRANSPARENT / 1×1 focus sink 全部无效
- **内核强推对用户态完全静默**：不经过 HCBT_ACTIVATE（WH_CBT 线程钩子收不到），
  也不发送 WM_ACTIVATE（WH_CALLWNDPROC 观察钩子只见到事后的 INACTIVE 通知）。
  任何用户态钩子都无法否决，只能事后归还
- Godot 侧：DisplayServer 每帧做焦点校正（GetForegroundWindow 比对），
  因此内核强推会在**下一帧**就变成引擎的 focus_entered——守卫必须抢在
  引擎帧级检查之前归还

## 修复设计（三层防御，GDExtension 实现，不改引擎二进制）
被系统强推前台的那一刻，宠物进程就是前台进程（拥有 SetForegroundWindow
权限），归还必然成功：

1. **CBT 线程钩子（WH_CBT，构造时安装）**：否决普通激活请求
   （HCBT_ACTIVATE 且 wparam==宠物 hwnd → return 1）。configure() 前
   `veto_all_` 全否决。对内核强推无效，但挡住普通路径。
2. **2ms 归还线程（SCENE 初始化即启动，非编辑器）**：轮询
   GetForegroundWindow——为 NULL 则补 shell 前台（防真空）；为宠物 hwnd
   则立即 SetForegroundWindow(shell)。把所有权窗口压到 ~2ms，低于引擎
   16ms 帧周期，Godot 的逐帧焦点校正观测不到。启动于引擎显示主窗口之前，
   这是唯一能覆盖"显示瞬间被强推"的层。
3. **节点每帧 poll + 证据**：foreign_history（最近 8 个外部前台）+
   强制分配事件写 JSONL（focus-guard-events.jsonl）+ get_stats 进 report。

辅助查询：`foreign_foreground_available()` 供 GDScript 判断桌面是否有
外部前台。析构必须 UnhookWindowsHookEx，否则进程退出时
STATUS_FATAL_USER_CALLBACK_EXCEPTION（已实测踩坑）。

## 已知上游问题与审查管线适配
- **godot-cpp#2024 / godot#1900**：首次编辑器扫描（冷导入）+ 已加载的
  GDExtension 类注册 → 编辑器关闭时 doc-cache 线程竞态崩溃（0xC0000005）。
  实测：冷导入必崩、热导入不崩、去掉类注册不崩、运行模式不受影响。
  对策：`review_toon_prototype.stage()` 在导入步暂扣 focus-guard.gdextension，
  导入完成后还原并补写 `.godot/extension_list.cfg`（运行时靠它发现扩展）。
  交互式编辑器首次打开项目仍会在退出时崩一次，属上游 bug，等待引擎修复。
- `.gdextension` 库条目须同时给 `windows.debug.x86_64`（编辑器二进制运行）
  和 `windows.template_debug.x86_64`（导出模板运行）。
- focus-guard/src、focus-guard/bin 放 `.gdignore`，防止 Godot 把 scons 的
  .obj 中间产物当 OBJ 模型导入。

## 构建
SConstruct + godot-cpp（.local/gdext/godot-cpp，不入 Git；SConstruct 默认用
本机引擎 `--dump-extension-api` 的 .local/gdext/extension_api.json 保证 API
逐位匹配 4.7.2）。产物 `focus-guard/bin/libfocus-guard*.dll` 由
review_toon_prototype.stage() 随项目复制进审查包。

## 验收（2026-10-08 全部通过）
- `review_toon_prototype.py --record-frames` + `check_toon_prototype.py`：
  `technical_checks_passed_visual_review_pending`（failed_checks 空）**2/2 稳定**
- notepad 起杀实验：`NOT_REPRODUCED`，守卫 restores_ok=1（2ms 内归还），
  引擎 has_focus=true 事件为零
- 运行模式退出码 0x0（析构 unhook 修复后；此前 0xC000041D）
