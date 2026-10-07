# 挥手打招呼 r1 — 设计卡（2026-10-07 已验收采用；r2 轴心重做）

状态：**已验收采用（2026-10-07 用户真机批准）**。概念与运行时实现
均通过；启动问候未启用（可选追加项，用户未要求）。
实现：`src/motion/WaveBehavior.{h,cpp}`（Raise→SwingOut/SwingIn×2→
Settle→Release 多通道状态机）+ `ParameterMotion` 转发（playWave/
cancelWave/exitWave，离开 Idle 自动 Release、Delete/Grass 直接取消）+
托盘「打个招呼」（非站姿置灰）+ `--render-interaction wave` 场景。
CTest 9/9 + ParameterMotionTest 54/54（新增 3 项 wave 测试）。审核图
`review-sheet.png`。
概念卡：`concept-card.png`（四拍 = `--render-pose` 真实 Native 渲染帧，
非手绘参考）。探针证据：`build/arm-probe/probe-sheet.png`（12 姿势轴扫描，
不入 Git）。

## 能力探针结论（推翻旧结论）

旧记录「手臂轴轮廓≈0」是 **34 网格旧模型** 的结论。R19 采用 41 参数
模型（含完整右臂素材）后实测：ArmRA ±65° 变化 1.3~1.5 万 px、肘
7~9k px、腕 3.2k px——**肩/肘/腕全部有显著剪影且形态完整**，无支离
破碎。腕只有右腕（WristRA ±25），左臂无腕轴。

## 概念：挥手打招呼——「你来啦！」

站姿 Idle 限定（坐姿时双手在键盘上，不挥手）。

- **触发**：托盘「打个招呼」；可选追加：启动问候（窗口出现 0.8s 后
  自动挥一次）——是否启用由用户定
- **关键拍**（全现有轴，零网格形变）：
  - A 抬臂 ~0.5s：ArmRA 0→50 + ElbowRA 0→35，视线先一步找到用户
    （EyeBallX 0.2）
  - B/C 摆动 2 轮（**r2 轴心重做：肩驱动**——用户真机反馈「轴用错了，
    怎么会在手腕部」，原腕键 ±22~25 与肘差相当、视觉读作手腕甩动）：
    外摆（肩 62/肘 20/腕 +6）↔ 内摆（肩 36/肘 52/腕 -6），肩跨 26°、
    腕仅跟随点缀，每向 ~0.3s，共 ~1.2s
  - D 收臂 ~0.5s：臂归位 + EyeSmile 0.85（弯眼笑意；**嘴保持站姿闭ω**。
    ~~SmileOpen=1 开口咧嘴~~ 已按用户裁决 r2 移除——真机观感「诡异，
    贼笑」；对比帧 build/arm-probe/smile-compare.png 后选定闭眼弯+ω）
  - 总时长 ~2.7s，结束回 Idle
- **打断**：Delete/Grass 优先取消，拖动让位；坐姿 Busy 期间托盘项置灰；
  **用户 hook（摸头/转身/伸展）抢占一切自发场景**（r2 触发规则：hook
  是命令、调度器场景是填充——hook 入口先 cancelWave/Box/Rice，绝不
  叠画也绝不被拒）
- **实现**：新 `WaveBehavior`（行为模块惯例：begin/advance/apply/
  cancel），ParameterMotion 一行转发；无窗口图层道具

## 验收口径

280px 下能读出「在跟我打招呼」；摆动节奏轻快不拖沓；手臂各帧无
撕裂/穿模（探针已初验）；打断能干净回位。
