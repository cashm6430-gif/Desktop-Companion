# 碗边闻香 r1 — 设计卡（2026-10-07 概念审批中）

状态：**pending 用户概念审批**。

## 概念

休息时一大碗热白米饭滑到桌面上（笔记本右侧），饭香蒸汽升起：
她看碗出神 → 闭眼低头吸一口饭香（发梢晚停一拍）→ 睁眼 ω 嘴满足浅笑 +
小点头。点蒸汽会微微后仰避开、再好奇看回来；超时自行吃完把碗滑走。
原「杯边闻香」候选的茶杯改为大碗白米饭（用户裁决），丰盛感更强、
与鲸鱼娘世界观（鲸标饭碗）更贴。

## 三态关键拍（concept-card.png，真实 Native 渲染帧合成）

- **A 碗出现**：托盘菜单触发，碗从右侧滑入桌面（x505..680，底 y606 落在
  桌面顶面 585..620 内），视线先一步 snap 到碗（EyeBallX/Y 现有轴）
- **B 吸气**：双眼闭合 + EyeSmile 弯眼 + AngleY 微低头（6°），蒸汽浓一拍，
  HairBack/Front 晚 0.15s 停（现有滞后通道）
- **C 满足浅笑**：SmileOpen=1（ω 嘴）+ 看回碗方向 + 一个小点头；
  之后停 2-3s，碗滑出

## 工程（与纸箱躲猫猫同构，全复用现有架构）

- 道具：单张碗 PNG 走 FrameOverlay 图层管线（PetWindow ctor 一行注册）；
  蒸汽 QPainter 程序化贝塞尔（不生成素材、alpha 呼吸动画）
- 行为：`RiceBowlBehavior` 新文件（begin/advance/apply/cancel 惯例），
  ParameterMotion 一行转发
- 表情全部现有轴：EyeLOpen/EyeROpen/EyeSmile/AngleY/SmileOpen/EyeBallX/Y；
  **零网格形变**（遵守 2026-10-07 长期规则）
- 触发：托盘「来碗饭香」；进行中变「吃完啦」；Delete/Grass 打断即滑走；
  拖动让位退出
- 已知风险：碗与卷饼道具（fedProp）同时出现的层级——碗是独立层不进
  mask，drawFedProp 仍在 paintEvent，先后关系实现时定

## 素材

- `bowl-prop-concept.png`：ImageGen 生成（1 张，白底正视图，边缘
  flood-fill 抠白保高光，鲸标与画风匹配）
- 概念卡三面板 = `--render-pose` 三帧真实参数渲染（A 站姿基线帧、
  B 闭眼低头帧、C ω 笑帧）+ 碗合成 + 程序化蒸汽
