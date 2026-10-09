# 鲸鱼娘角色资产 — 技术预算与移交规格 v1

维护：技术美术
最后更新：2026-10-09（round4：三渲二外观 + 导出合并 + 引擎验证）

## 1. 交付物位置

| 产物 | 路径 | 说明 |
|---|---|---|
| 作者源 | `art/3d/whale-girl-head-r2/character-round3.blend` | 多对象、标准材质（非自发光）——美术编辑用 |
| 引擎资产 | `assets/3d/whale-girl-head-r2/character-round3.glb` | **单网格合并版**，带蒙皮 + 三段动画 |
| 生成脚本 | `tools/create_whale_head_candidate_r2.py` | 一键重建：`blender --background --factory-startup --python <脚本>` |
| 投影源 | `face-project.png`（脸，1254²）、`costume-project.png`（服装，1254²） | 均由 R19 分层 PSD 合成/清洗而来 |
| 发色渐变 LUT | `hair-gradient.json` | 逐高度从 PSD 采样的 29 个颜色锚点 |
| 引擎验证 | `godot/toon-prototype/tests/whale_import_check.gd` | 无头导入自检 |

## 2. 实测技术指标（LOD0）

| 指标 | 数值 | 预算 | 状态 |
|---|---|---|---|
| 三角面 | 36,512 | ≤ 40,000（PC 桌面宠物单角色） | ✅ 91% |
| 顶点 | 18,633（导入引擎后 19,813，UV/法线拆分） | — | ✅ |
| 材质槽 / 绘制调用 | **11**（合并前 73 个对象） | ≤ 16 | ✅ 69% |
| 骨骼 | 21 | ≤ 32 | ✅ |
| 动画 | idle / sit / wave | — | ✅ |
| 贴图 | 2× 1254² + 顶点色渐变 | 单角色 ≤ 2×2048² | ✅ |

引擎导入自检命令（本机 Godot 4.7.2）：

```
cd godot/toon-prototype
"D:/tool/godot/Godot_v4.7.2-stable_win64_console.exe" --headless --path . \
  --script res://tests/whale_import_check.gd
```

最近一次结果：
`meshes=1 surfaces=11 verts=19813 skinned=1 bones=21 materials=11 textures=2 anims=idle,sit,wave`

## 3. LOD 策略（项目特例，已评审）

桌宠是**固定机位、单角色常驻**场景——标准 LOD 链（LOD0-3 + 过渡距离）在此无收益。
本项目的既定策略：

- **正常态**：只用 LOD0（3.6 万面在桌面场景毫无压力）
- **最小化 / 遮挡态**：切单四边形剪影占位，不参与渲染剔除以外的一切开销
- **若日后上移动端**：LOD1 目标 12,000 面（保留发布上限内），并关闭描边 pass（见 §5）

## 4. 贴图管线

| 贴图 | 源分辨率 | 用途 | 引擎导入设置 |
|---|---|---|---|
| `face-project.png` | 1254² | 脸部平面投影（脸区材质槽） | 保持源尺寸导入，平台覆盖降至 1024（PC）/ ASTC 6×6（移动）；mipmap ON |
| `costume-project.png` | 1254² | 裙/胸衣/围裙/腰带环绕投影 | 同上 |
| 发色渐变 | —（顶点色 COLOR_0 导出） | 宝蓝→亮青渐变 | 无贴图开销 |

## 5. 着色器规格（引擎侧）

Blender 里的三渲二外观（`apply_toon_look()`）**只是预览装置**；正式外观由引擎着色器实现，
导出前会 `revert_toon_look()` 还原标准材质（否则 glTF 会把贴图导到自发光通道——已踩坑修复）。

| 引擎着色器 | 路径 | 用途 |
|---|---|---|
| 卡通着色 | `godot/toon-prototype/toon.gdshader` | unshaded + 双色阶（`shade_strength` 0.22）+ 可选贴图/顶点色两开关 |
| 描边 | `godot/toon-prototype/outline.gdshader` | `cull_front` 逆壳描边，宽度 0.009（与 Blender 预览 0.014 对应） |

材质映射：

| GLB 材质槽 | toon.gdshader 设置 |
|---|---|
| WhaleFaceTex / WhaleBodyTex | `use_texture=true`（base_color 保持白） |
| WhaleHairGradient（头/发束/呆毛） | `use_vertex_color=true`（COLOR_0 线性→显示空间已内建） |
| 其余纯色件（蕾丝/皮肤/藏青/鳍耳等） | `base_color` 对应颜色，两开关均关 |

**平台说明（按规范记录）**：本资产为 **PC 桌面宠物专用**，描边 pass 使绘制调用翻倍（11→22），
PC 无压力；移动端变体需关闭描边并降贴图（见 §3）。

## 6. 资产审查协议（沿用）

1. **几何审查**：三视角渲染 + 与 R19 并排对比（`round4-compare-sheet.png`），面部投影零漂移
2. **引擎审查**：无头导入自检（§2）+ 引擎内实机渲染（下一里程碑）
3. **签字前必查**：UV 完整（无撕裂）、蒙皮权重和=1（构建脚本硬断言）、合并后动画完整（idle/sit/wave）

## 7. 已知偏差（下一轮输入）

| 项 | 现状 | 目标 |
|---|---|---|
| 头发剪影 | 侧发量感仍偏"罩"，发梢平齐 | 成绺收束、发梢参差（对照 PSD 逐段取样） |
| 发带 | 白环+小褶皱 | 原画的褶边花边质感 |
| 前刘海 | 3D 刘海条与投影碎发有接缝感 | 减薄刘海条、让投影碎发主导 |
| 手部 | 袖口可见、手被裙摆部分遮挡 | 前移/外放至完全可见 |
