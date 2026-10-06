# R19 独立纯发补底 — 技术方案（已实现，待用户视觉复核 2026-10-06）

状态：**已实现并通过全部工程校验**。上游交接：`8379a19`（desk-work-20261006-r1 handoff）。
候选：`candidate-model/`（39 网格/41 参数/5 页）；作者源 `author-source/edited.cmo3`；
机器入口 `handoff-state.json`；工程证据 `staging-report.json`；审查帧 `review-r19/`。

## 1. 缺陷本体（已核实）

- `ArtMeshBackHair`（335 顶点）= 双翼补底网格。UV 图集两翼矩形
  `[2959,8,315,350]` / `[3323,8,315,350]`，经 ownership offset `[-2671,550]`
  映射回源画布即两翼 bbox `[288,558,603,908]` / `[652,558,967,908]`。
- 图集中该区域纹理混有旧裙布像素 → 姿势变化（删除扑击、回望）时露出布色碎屑。
- 网格 footprint 未覆盖旧裙剪出的缺口（R17：原宽度有裙边三角缺口）。
- `ArtMeshBackHair2`（547 顶点）= 后发主体（两大真发束+完整呆毛=3 强核），
  含重复耳尖/小孤立碎屑，属于另一项清理（图集岛屿，当前只能运行时派生）。

## 2. R18 失败机制（不重蹈）

`runtimeBackdropCleanup`：运行时把纯发翼贴图重绘进图集两矩形 +
`supportScale=[1.7,1.25]` 在 `CubismCanvas.cpp:867` 绕 centre/top 拉伸顶点。
失败：补上裙缺口的同时盖住外侧原发束、露出平直裁切边。**禁止再靠 GPU 放大补洞。**

## 3. 管线能力（已实证）

- `tools/prepare_desk_work.py` → `author-plan.json`（schema 2）；
  `tools/edit_live2d_author.py` → `PSD2LiveParentModelEdit.java`（固定 CMO
  `f7ee66b4`，独立 JVM/store）；`tools/stage_desk_work.py` → 483 上下文
  old34 位精确校验 + 候选模型目录。
- **`insert_meshes` 支持任意顶点数/索引/页**（positions≥6 floats，任意 indices，
  texture_page 可指新追加页）。`mesh_grids` 可改既有网格 keyform 通道。
- `bind_complete_quad`（prepare_desk_work.py:52）已示范：从 rig JSON 的
  `native_default_source1254` + 父网格 grid cells 逐 cell 仿射映射把新网格
  绑进父变形器运动。多顶点网格同样适用（对每顶点施加 cell 仿射）。
- 坐标契约：root default origin `[31.238708,-67.263]`，extent `[1215.79,1384.53]`；
  DeformBodyShift 父局部 = `(world-origin)/extent`；sleeve/laptop 走
  `world_to_parent` 最小二乘（rig JSON 提供 `native_default_source1254`）。

## 4. R19 设计

1. **新页**：`arm-backing-hair-only.png`（SHA `39cffd0d…`，1254×1254，双翼纯发，
   alpha bbox `[130,603,593,1114]` / `[661,602,1124,1114]`）追加为 texture_page 4。
2. **新网格** `ArtMeshHairSupportPure`（或左右两片）：
   - 几何 = ArtMeshBackHair 同源 335 顶点轮廓的**超集**：原翼轮廓 +
     下缘沿发束形状外扩的多顶点裙摆缺口覆盖区（发束状锯齿轮廓，非矩形），
     保证覆盖旧补底全部可见布像素与 R17 三角缺口，不越外侧原发束内缘。
   - UV：整翼映射进原翼 bbox（与 R18 runtime 映射一致），外扩区按
     邻近发束延伸取 UV（生成翼有 463×511 富余内容）。
   - 绑定：parent = ArtMeshBackHair 父变形器；keyforms 复制其 grid 运动
     （rig JSON cell 仿射，逐顶点）；OPACITY 通道 = 恒 1（不做模式开关，
     站姿/坐姿都需要补底）。
   - draw_order：紧贴 ArtMeshBackHair 之上（仍低于身体全部网格）。
3. **旧补底不动**：ArtMeshBackHair 保持原样 → 483 位精确保留证明继续成立，
   布像素被新网格盖住。若像素级复核发现新网格覆盖不住的外露布像素，
   再评估 `mesh_grids` 通道编辑（会改默认外观，须单独审批）。
4. **staging**：新 stage 变体接受新增 backing 网格（扩展 stage_desk_work 的
   allowed-added 集合），不启用 `--backdrop-cleanup`（R18 路线弃用）。
5. **验证**：old34 位精确 483 上下文 → renderer `--review-model` 捕获
   default / 抬手 / desk-delete / turn-ended × 深浅背景 × 280px →
   自查外缘/原发束/呆毛 → 交用户视觉复核（pending，不采用）。

## 5. 实施记录（2026-10-06 晚）

- `tools/prepare_hair_backing.py` → `hair-backing-plan.json`（SHA `300cba02…`）。
  关键发现：ArtMeshBackHair 网格本身无参数轴（运动全在父变形器链），逐字复制
  顶点 + 单一 rest keyform 即可完整继承运动。翼外 82 个 UV 边缘顶点按最近翼
  矩形钳制（最大 3.05px）。参照 MOC 必须用候选 MOC（`6401fea4`），不能用正式
  等价 MOC（Java 端按参照 MOC 的 artMesh 声明图集页）。
- 渲染序落点：整数 draw_order 无法表达 2.5（PSD2Live 库拒绝
  `FractionalDrawOrderNotStorable`）；并列规则=同部分新节点在前、跨部分按
  Part 遍历位。draw=3 落在 headwear 之上——翼区与头饰无空间重叠，其余遮挡
  关系与理想落点等价，采用 draw=3。曾试验放宽 Java 校验为浮点，库仍拒绝，
  已回滚驱动改动。
- 导出对插入网格做三角形环绕规范化（顶点集不变）；stage 校验按三角形多重集
  比较。新旧 MOC 的 artMesh 索引表不同，跨模型取索引必须按名解析。
- `tools/stage_hair_backing.py`：old38 位精确 483 上下文（两模型同参数对比；
  ParamDeskVisible 双侧同钉）、拓扑/渲染序/静止位置全过。出 `stage-r19`。
- 捕获自检：R19 vs R17 仅 ~400px 抗锯齿差异（领口区，无可视缺陷）；
  R19 vs R15 = 预期内容替换 + 耳尖重复片/碎屑清除，呆毛完整。
- **遗留**：R17 的"裙边三角缺口"在静态帧不可复现（黑楔形两帧均有，为头发
  阴影），属内容裁决——负空间区域显示纯发是否可接受，交用户目检。

## 6. 待实现清单（已完成）

- [x] `tools/prepare_hair_backing.py`
- [x] `tools/stage_hair_backing.py`
- [x] 渲染捕获与像素自检
- [x] 视觉复核材料（`review-r19/` + `evidence/`）
