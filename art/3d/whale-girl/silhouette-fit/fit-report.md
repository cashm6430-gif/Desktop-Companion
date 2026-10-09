# 剪影拟合报告（路线 B，T1–T6）

* 输入：`art/3d/whale-girl-head-r2/character-round3.blend`（75 具名部件，未经改动）
* 参考：`art/3d/whale-girl/silhouette-fit/refs/`（T1 归一化剪影）
* 最优参数：`.local/fit-run/d-8e2ebe1350/params.json`
* 候选剪影：`.local/fit-run/d-8e2ebe1350/sil/`

## 1. 判据

目标函数由 T2.5 的诊断决定。裸 IoU 被实心面积主导（两块独立参考视图互相也只到
0.7951），
而决定角色辨识度的是外轮廓与分带覆盖，所以每一项都取**对称**形式：

```
J = 0.35·mean_IoU + 0.4·mean_outline_F1 + 0.25·mean(min_band_IoU)
```

首轮尝试证明这一点是必须的：只用「轮廓召回 + 分带覆盖」两个单边项时，优化器把裙摆和
头发整体吹大就把分数推到 0.7417，而 mean IoU 从 0.756 掉到 0.627；换成对称项后该解
在 53 个候选里排第 30 名（低于原始模型）。

## 2. 校验门

| 组 | 门 | 内容 | 结果 |
|---|---|---|---|
| T1 | V1a–V1d | 参考剪影二值 / 身高 1000 / 最宽行在下半 / 宽高比 | PASS |
| T2 | V2a–V2c | 渲染非空且归一化 / 侧向重合 ≥0.20 / 参考安全带 | PASS |
| T2 | V2d | 交叉配对判别（**仅标定用**） | PASS（2/3，最差边际 +0.0244）|
| T2.5 | V2e1–V2e3 | 复现 T2 基线 IoU / 算术恒等 / 非平凡 | PASS |
| T3 | V3a | 中性参数 → 逐顶点比特一致，**导出 GLB 与出厂件 md5 相同** | PASS |
| T3 | V3b | 参数单调且有效 | 20/20 参数单调且有效 |
| T3 | V3c | 法线零翻转 | PASS |
| T3 | V3d | 中性参数不新增对象 | PASS |
| T3 | V3e | 蒙皮权重完整（含新增部件） | PASS |
| T3 | V3f | 地面守卫：形变后最低点不低于鞋底 | PASS |
| T6 | V6a | 数值重算一致 | PASS |

V2d 在候选评估中被降为诊断输出：它证明的是**帧标定**可信（一次性、在出厂模型上验证），
若对每个候选都强制，一个恰好更接近轴对称的候选会被测量机制否掉而不是被打分。

V3f 的存在理由：所有剪影都按总高归一化，任何伸到鞋底以下的几何都会悄悄改变比较尺度，
让候选靠"长高"而不是"贴合"得分。地面接触点因此被冻结在出厂值。

```
V3a identity: params are non-neutral, 27/75 parts moved, +4 created, max delta = 0.186667 (informational)
V3d creation: neutral=False created=4 ['FitAdd_front_HairSideInnerL_0', 'FitAdd_front_HairSideInnerR_0', 'FitAdd_front_HairSideInnerL_1']... -> PASS
V3e skin integrity: unweighted=0 max|sum(w)-1|=2.98e-08 -> PASS
V3f ground guard: shipped min z=0.3570 -> deformed 0.3570 (delta +0.0000) -> PASS
deform report: touched=27 created=4 skipped_neutral=4 verts=9598
V3c orientation: flipped faces = 0 -> PASS
T3_RUNNER PASS (V3a,V3c..V3f)
```

## 3. 结果

| 指标 | front | left | back |
|---|---|---|---|
| IoU（基线） | 0.7739 | 0.6221 | 0.8128 |
| IoU（拟合） | 0.7899 | 0.6212 | 0.8613 |
| 参考覆盖率（基线→拟合） | 0.894 → 0.949 | 0.738 → 0.805 | 0.864 → 0.932 |
| 缺失率（基线→拟合） | 10.6% → 5.1% | 26.2% → 19.5% | 13.6% → 6.8% |
| 超额率（基线→拟合） | 15.6% → 20.1% | 18.7% → 29.6% | 6.3% → 8.2% |
| 轮廓 F1（基线→拟合） | 0.464 → 0.475 | 0.318 → 0.422 | 0.471 → 0.498 |

目标函数：**0.5348 → 0.5619**（
mean IoU 0.7363 → 0.7575，
outline F1 0.4177 → 0.4650，
最差分带 IoU 0.4403 → 0.4432）

回归下限检查（拟合后）：front:hem/legs 0.740 (下限 0.66)；front:torso 0.960 (下限 0.8)；left:torso 0.861 (下限 0.72)；left:hem/legs 0.029 (下限 0.02)；back:hem/legs 0.910 (下限 0.78)；back:torso 0.934 (下限 0.75)

最优参数：

```json
{
 "hair_side_flare": 0.146667,
 "hair_back_volume": 0.186667,
 "bang_length": 0.073333,
 "skirt_flare": 0.106667,
 "foot_length": 0.45,
 "foot_height": 1.3,
 "front_lock_extra": 2.0,
 "front_lock_fwd": 0.26
}
```

## 4. 平台期与结构性缺口

* front / back 两视的缺失率被压掉一半以上，侧视（left）也终于从 0.262 降到
  0.195 —— 靠的是**新增**前发落发层，而不是拉伸已有部件。
* **代价要一起看**：覆盖率是靠增加体积换来的，所以超额率同步上升（front
  15.6% → 20.1%，
  left 18.7% → 29.6%）。
  目标函数之所以用对称项，正是为了让这笔交易被计入而不是被忽略：它仍选择这个解，
  说明净收益（IoU 与轮廓 F1 同时上升）大于代价，但"更像"不等于"更瘦"。
* 但侧视仍是三视里最差的一个，且 `hem/legs` 分带几乎覆盖不到参考：参考在侧向能读出
  腿与靴子的体块，而模型该区域是一块平盘。这类缺口靠"形变已有部件"结构性无解，
  本轮的参数扩展方向（复制同族发锁再参数化变换）是唯一真正推动了侧视的手段。
* 参数收敛表现出明显的边际递减：倒数五个参数里有三个的增益 < 0.003，
  说明在**当前几何预算**内已经接近该空间的极限。

## 5. 结论

1. 度量与流水线是可复现的：中性参数导出与出厂 GLB **逐字节相同**，基线 IoU 在放大
   归一化画布后仍逐位不变，报告的全部数值可由 `--rescore` 重算得到。
2. 参数化拟合能拿到确定的正收益，但收益量级有限，且集中在正面/背面。
3. 侧视的剩余缺口是**部件缺失**而非形状误差，后续应继续沿"新增部件"方向扩展
   （侧向可读的腿/靴体块、独立尾鳍），而不是继续调形变参数。

## 6. 复现

```bash
# 参考剪影（T1）
python tools/build_ref_silhouettes.py

# 帧标定 + 基线（T2，严格模式只在出厂模型上跑）
blender --background --factory-startup --python tools/render_silhouette.py -- \
    assets/3d/whale-girl-head-r2/character-round3.glb art/3d/whale-girl/silhouette-fit/t2-baseline --strict-calibration

# 形变层校验门（T3）
blender --background --factory-startup art/3d/whale-girl-head-r2/character-round3.blend \
    --python tools/fit_silhouette_template.py -- --probe all

# 坐标下降（T4）
python tools/fit_silhouette_search.py --descent --base '{}'

# 重排全部缓存候选（不跑 Blender）
python tools/fit_silhouette_search.py --rescore

# 本报告（T6）
python tools/report_silhouette_fit.py --best .local/fit-run/d-8e2ebe1350

# 并排对照图（写入 .local/fit-run/fit-compare-sheet.png）
python tools/diagnose_silhouette_fit.py \
    "shipped baseline|art/3d/whale-girl/silhouette-fit/t2-baseline" \
    "fitted|.local/fit-run/d-8e2ebe1350"
```

## 7. 已知限制

* 参考的 left/back 视图是 AI 推演的，不是原始作画；V2c 只保证它与正面自洽到
  0.7951，因此目标函数
  的分数量级不应被当作绝对质量分。
* 新增部件用的是同族发锁的复制体，形态语言与原件一致，但没有独立的艺术设计；
  正式采用前需要按项目工作流出审批材料。
* 本轮未改动任何作者源文件与正式资产，全部产出在 `.local/fit-run/` 与
  `art/3d/whale-girl/silhouette-fit/`。
