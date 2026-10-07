# Cubism SDK 本地补丁

SDK 目录（`CubismSdkForNative-5-r.5/`）不进 Git。重装 SDK 或换机器后，
按本文档重新应用以下补丁，并跑 `tools/dev.cmd build` 验证。

## 补丁 1：高级混合模式 shader 预编译开关（2026-10-07）

**问题**：`CubismShader_OpenGLES2::GenerateShaders()` 无条件预编译
ColorBlend×AlphaBlend 全组合（本机 482 个程序），首次创建 renderer 时在
Intel UHD 630 桌面 GL 驱动上耗时约 7-9 秒——这是启动慢的主因。本项目的
模型只用 Normal/Over（实测 39 个 drawable 全为 blend mode 0、无 offscreen），
这些程序全部是死重量。

**改动 1** — `Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.hpp`：

在 `static void DeleteInstance();` 声明之后加：

```cpp
    /**
     * @brief   高度なブレンドモード（ColorBlend/AlphaBlend の全組合せ）シェーダの
     *          事前生成を切り替える。最初の renderer 生成時に一度だけ行われる
     *          事前コンパイルは、環境によっては数秒かかる。Normal/Over と互換
     *          ブレンドしか使わないモデル群では false にすることで起動時の
     *          コンパイルを大幅に減らせる。既定は true（従来どおり全生成）。
     *          GenerateShaders が一度走った後の変更は反映されない。
     */
    static void SetBlendProgramsPrecompiled(csmBool enabled);
```

在 `_shaderSets` 成员之后加：

```cpp
    static csmBool s_precompileBlendPrograms;  ///< 高度なブレンドシェーダを事前生成するか
```

**改动 2** — `Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.cpp`：

在 `CubismShader_OpenGLES2::CubismShader_OpenGLES2()` 构造函数定义之前加：

```cpp
csmBool CubismShader_OpenGLES2::s_precompileBlendPrograms = true;

void CubismShader_OpenGLES2::SetBlendProgramsPrecompiled(csmBool enabled)
{
    s_precompileBlendPrograms = enabled;
}
```

`GenerateShaders()` 内有 **两处** `// ブレンドモードの組み合わせ分作成` 块
（第一处循环调用 `LoadShaderProgramFromFile` 编译，第二处循环调用
`SetShaderSet(..., true)` 采样器状态）。两处的开块 `{` 都改为
`if (s_precompileBlendPrograms) {`。

**改动 3** — `src/CubismCanvas.cpp`（应用侧，已提交进 Git）：

- 新增 `usesOnlyCompatibleBlendModes()` 助手：逐 drawable 用
  `csmBlendMode` 判定 Normal/Over 或 AddCompatible/MultiplyCompatible，
  且 `csmGetOffscreenCount()==0`。
- 在三个 renderer 创建之前：三个模型（主模/衣装副本/独立颈）全部兼容时
  调用 `SetBlendProgramsPrecompiled(false)`。未来若模型用到高级混合，
  检测自动失败，恢复 SDK 原行为。

**效果**：启动到桌宠显示 10.4s → 2.7s；渲染输出像素级一致（已验证）。

## 相关事实（勿再踩）

- 纹理上传必须 `glGenerateMipmap`：Cubism 渲染器内部会把部分通道切到
  `GL_LINEAR_MIPMAP_LINEAR`（CubismShader_OpenGLES2.cpp 末段），mip 链
  不完整时采样返回**全黑**。曾误删导致渲染全黑，已回滚。
- `build/startup_trace.log`（exe 旁）记录启动各阶段毫秒，GUI 无 stdout 时
  是唯一启动诊断通道；`tools/dev.cmd` 的 build 现在自动重签名 ship exe。
