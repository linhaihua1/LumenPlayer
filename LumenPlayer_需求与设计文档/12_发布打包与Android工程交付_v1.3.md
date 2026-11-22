# 12 · 发布打包与 Android 工程交付（v1.3）

> **日期**：2026-11-21
> **本轮目标**：清空测试数据 → 产出 Windows 可分发包 → 产出 Android 工程
> **结论**：Windows 包已验收（zip 解压即用，含 mpv 与 DirectML 运行时）；Android 为**源码工程**，未在本机编译（无 SDK/NDK，且 Google 源不可达）

---

## 1. 测试数据清理

`testdata/` 全部清空（59MB：7 个样本片、5 个输出片、对比图、batch 目录、测试模型），
只保留空目录与 `.gitkeep`。

### 1.1 清理暴露的第一个缺陷：单测静默降级

清理后重跑单测：**151 项 → 136 项**，其余全部 PASS。

原因：`OnnxBackendUniformFrame` 等 15 项断言依赖 `testdata/models/nearest2x.onnx`，
文件不存在时走 `[SKIP]` 分支直接 return。

**为什么这是缺陷而不是"合理的跳过"**：
回归报告从「151 项全绿」变成「136 项全绿」，两者都是绿色、都返回 0，
没人会注意到少了 15 项覆盖率。真实项目里这类"沉默的绿色"比红色更危险。

**修法**：把 292 字节的最小模型（`Resize + nearest` 2×）base64 内嵌进 `tests/test_main.cpp`，
首次调用时解到工作目录。测试从此自包含——不依赖任何外部数据文件。

优先级保持不变：`testdata/models/*` 存在时仍优先用它（便于人工换大模型验证），
不存在才用内嵌模型。

---

## 2. Windows 发布包

### 2.1 产物

```
dist/LumenPlayer-0.2.0-win64/          165MB（可直接运行，免安装）
dist/LumenPlayer-0.2.0-win64.zip       64MB（分发用）
```

包内：

| 路径 | 内容 | 大小 |
|---|---|---|
| `lumen-player.exe` / `probe` / `render` / `batch` | 四个程序 | 共 13MB |
| `mpv/mpv.exe` | 播放内核（shinchiro build 0.42） | 121MB |
| `mpv/d3dcompiler_43.dll` | mpv 的 D3D11 着色器编译依赖 | 4.3MB |
| `runtime/onnxruntime.dll` + `DirectML.dll` | AI 超分运行时（1.20.1 + 1.15.4） | 33MB |
| `lumen.json` | 发布版配置（全默认，路径留空走自动查找） | 1.5KB |
| `README-FIRST.txt` | 使用说明（含"播放器不做 AI 超分"的明确声明） | 4.5KB |
| `THIRD-PARTY-NOTICES.txt` | mpv GPLv2+ / ORT MIT / DirectML 的来源与源码获取方式 | 1.2KB |
| `Run-LumenPlayer.bat` | 启动器（切工作目录 + 透传参数，GBK 编码） | 167B |

**不打进包的东西**：源码、tests、ffmpeg（158MB，只影响自检与导出，播放不需要）。

### 2.2 布局与查找规则对齐

包内路径不是随便定的，必须和代码里的查找顺序一致：

| 组件 | 查找顺序（`onnx_backend.cpp` / `lumen-player/main.cpp`） | 包内位置 |
|---|---|---|
| `mpv.exe` | 配置 → `exe/mpv.exe` → `exe/mpv/mpv.exe` → `exe/runtime/mpv.exe` → PATH | `mpv/mpv.exe` |
| `onnxruntime.dll` | `runtime_dir` → `exe/` → `exe/runtime/` → 系统 | `runtime/` |
| `DirectML.dll` | 与 onnxruntime.dll **同目录**（见 2.3 的修复） | `runtime/` |

验证：包内直接 `--backend ort --provider dml`，无需再写 `--runtime-dir`。

### 2.3 打包暴露的第二个缺陷：DirectML 静默回退

在干净目录跑 `--provider dml`：

```
[2/4] 超分后端: ort(CPU)      ← 请求的是 dml
[警告] 未能在运行时中启用 GPU 执行提供程序（请求=dml），已回退到 CPU
       底层原因：DmlExecutionProvider: Unknown provider name. ...
                 Dml: （空）
                 OrtSessionOptionsAppendExecutionProvider_DML: （运行时未给出说明）
```

而同一份 DLL 在开发目录里能正常跑到 `ort(DML)`。逐层排查：

1. 两个目录的 `onnxruntime.dll` / `DirectML.dll` md5 **完全一致** → 不是文件问题
2. 差异只在参数：开发时写了 `--runtime-dir`（显式），包里靠自动查找
3. 读 `LoadRuntime`：候选列表里有 `exe\runtime\onnxruntime.dll`，
   但后面"抢先加载 DirectML"的 `preload_directml` 只尝试了 `runtime_dir_hint` 与 `exe_dir`

**根因**：`onnxruntime.dll` 放在 `exe\runtime\` 时，`DirectML.dll` 从未被预加载。
ORT 内部按名 `LoadLibrary("DirectML.dll")` 走进程默认搜索顺序，
命中 `C:\Windows\System32\DirectML.dll`（1.0.200713，2019 年，远低于 ORT 1.20 要求的 1.15），
EP 创建失败，且 ORT 返回的错误消息是**空字符串**——现场只能看到"回退到 CPU"。

**修法**：以「`onnxruntime.dll` 实际加载路径」推目录，而不是再猜一遍层级：

```cpp
wchar_t mod[MAX_PATH * 4];
GetModuleFileNameW(dll, mod, ...);   // dll 是刚刚加载成功的模块句柄
// 取目录 → preload_directml(该目录)
```

回退链保留（`runtime_dir` → `exe/` → `exe/runtime/`），但正常情况下不再用到。

复测：包内 `--provider dml` → `ort(DML)`，单帧 30.7ms。
**教训**：依赖文件的目录，以主模块的实际位置为准；自己再推一遍目录层级，早晚会漏一种布局。

### 2.4 验收证据

从 zip 解压到全新目录（无任何历史文件），跑播放链路自检：

```
[1/5] mpv: ...\lumen_accept\LumenPlayer-0.2.0-win64\mpv\mpv.exe
      mpv 版本：mpv v0.42.0-38-gbcde6b93c
[3/5] file-loaded: 是       时长：4.00 s
[4/5] 自检结论：interlaced  deinterlace  720x480
[5/5] 挂载滤镜 0：bwdif=mode=send_frame:parity=tff:deint=all  ->  成功
[5/5] 挂载滤镜 1：deblock=filter=strong:block=8               ->  成功
[5/5] 挂载滤镜 2：hqdn3d=1.5:1.5:6:6                          ->  成功
      A/B 切回原片后 vf = (空)
```

同一包内 `lumen-render --backend ort --provider cpu/dml` 均跑通。

> 注：这个合成样本实际是 BFF（`interlace=scan=bff`），自检报的是 TFF。
> 这不是新问题——场序判定仍是「元数据优先 + 缺省 TFF」，已在 README 的已知限制里列明，
> 且提供了 `--field-order bff` 与播放中按 P 覆盖。属已知的精度边界，不是回归。

### 2.5 打包脚本

`tools/make_release.py`：

```bash
python tools/make_release.py               # 完整包
python tools/make_release.py --no-mpv      # 精简包（用户自备 mpv）
python tools/make_release.py --no-runtime  # 不带 AI 运行时
```

两个实现细节值得留档：

1. **不用 `shutil.rmtree` 清旧目录**。在受管环境下它会走回收站实现，
   回收站不可用时整包中断（实测 `trash operation aborted`）。
   改为逐文件删除 + 失败忽略：正确性由「确定的文件名直接覆盖」+「压缩阶段过滤残留」保证。
2. **压缩阶段过滤 `*.tsv` / `*.log` / `dbg*`**。本机试跑会在包目录写
   `lumen_resume.tsv`（播放进度），带着开发机的路径发给用户既难看也泄露信息。

---

## 3. Android 工程

### 3.1 为什么是源码而不是 APK

三个硬条件都不满足：

| 条件 | 现状 |
|---|---|
| 代码基础 | `platform/` 与 `apps/` 全是 Win32（`windows.h`、命名管道、`CreateProcess`），仓库里没有任何 Android 代码 |
| 工具链 | 无 Android SDK / NDK / `ANDROID_HOME`；JDK 17 有（`C:\dev\jdk-17.0.13`） |
| 下载源 | `dl.google.com` 直连超时（实测 15s 无响应）；腾讯云镜像可达 |

APK 不是"打包"动作能产出的东西——它需要一个新工程、一套播放内核选型、
以及 NDK 交叉编译。所以本轮交付**可编译的完整工程源码**，
由使用者用自己的 Android Studio 编一次。这与需求方选择的路线一致。

### 3.2 交付内容

```
android/
├── settings.gradle.kts          阿里云 Maven 镜像优先（Google 源在该网络下不可达）
├── build.gradle.kts             AGP 8.5.2 / Kotlin 1.9.24
├── gradle.properties
├── gradlew + gradlew.bat + wrapper/   已带 wrapper.jar，无需本机装 Gradle
└── app/
    ├── build.gradle.kts         compileSdk 34 / minSdk 24 / ABI 三选 / CMake 接入
    ├── proguard-rules.pro       JNI 方法名保留
    └── src/main/
        ├── AndroidManifest.xml  SAF 选片，零存储权限
        ├── cpp/
        │   ├── CMakeLists.txt   直接编译 ../../../../../src/core/*
        │   └── lumen_jni.cpp    Y 平面数组 → FrameBuffer → JSON
        ├── java/com/lumen/player/
        │   ├── NativeAnalyzer.kt   external 声明 + 延迟加载（不用 init，避免 ExceptionInInitializerError）
        │   ├── SourceAnalyzer.kt   抽帧 + 调 native + 解析 + 二段复核
        │   └── MainActivity.kt     Media3 播放 + 结论展示 + VIEW intent
        └── res/                    布局 / 主题 / vector 图标（无二进制资源）
```

### 3.3 关键决定

| 决定 | 理由 |
|---|---|
| **core 共享，不复制** | 判定算法出现两份副本必然漂移，症状是「同片源电脑判隔行、手机判逐行」，极难排查。代价：`android/` 不能单独拷出仓库（CMake 里加了 `EXISTS` 检查，位置不对立刻报错） |
| **播放用 Media3 而非 libmpv** | libmpv 官方不发 Android 包，需自行交叉编译；Media3 是 Maven 依赖、硬解直通 SurfaceView，先把"能播"做对 |
| **JNI 只传 Y 平面** | 场结构判定是亮度域差分，色度零贡献；传 YUV 全帧让拷贝量翻 1.5 倍 |
| **抽帧必须连续、绝不缩放** | 缩放会把相邻两行平均掉，梳齿被抹平 → 必判逐行；均匀撒点会破坏 3:2 的 5 帧周期 → telecine 永远测不到。实现为「40% 处连续 16 帧，步长 33ms」 |
| **二段复核** | 静止镜头无运动，容易误判逐行。首段判成 progressive/unknown 时，在 75% 处再测一段，取"更严重"的结论——把隔行误判成逐行比反过来糟糕得多 |

### 3.4 离线验证（能做的都做了）

没有 NDK 不等于不能验证代码正确性：

1. **全部 5 个 XML 解析通过**（manifest / 布局 / 主题 / strings / 图标）
2. **C++ 侧语法检查通过**：用最小 JNI 桩头文件（`jni.h` / `android/log.h`）
   `g++ -fsyntax-only -std=c++17 -Wall -Wextra lumen_jni.cpp` → 零警告
3. **core 可移植性实证**：`types/json/frame_desc/frame_buffer/field_analyzer/`
   `color_policy/grain_policy/source_info/processing_chain` 九个文件在
   去掉 Windows 头文件后全部编译通过 —— 这是"core 能搬去 Android"的直接证据，
   而不是一句设计承诺
4. **JNI 函数名与包名一致性核对**：`Java_com_lumen_player_NativeAnalyzer_analyzeFrames`
   ↔ `com.lumen.player.NativeAnalyzer.analyzeFrames`
5. **CMake 相对路径实测**：`cpp/../../../../..` → `LumenPlayer`（`realpath` 验证）

**未验证**：Gradle sync、NDK 编译、真机运行。这三项必须在有 SDK 的机器上完成。

### 3.5 后续路线（按性价比）

| 阶段 | 内容 | 预估 |
|---|---|---|
| 1 | 判定结果真正作用到画面（Media3 `Effect` 挂反交错） | 1—2 天 |
| 2 | 换 libmpv 内核，直接用 `bwdif`/`fieldmatch`/`ivtc`，与桌面行为一致 | 1—2 周（含 GPL 合规评估） |
| 3 | AI 超分（ONNX Runtime Mobile / NCNN） | 2—3 周 |

**关于阶段 3 的现实预期**：骁龙 8 系跑 480p→1080p 约 30—80ms/帧，
24fps 勉强实时、30fps 会掉帧。诚实的产品定义是「离线导出 1080p，播放时只做反交错」，
而不是承诺"实时 1080p"。

---

## 4. 本轮改动清单

| 文件 | 改动 |
|---|---|
| `tests/test_main.cpp` | 内嵌 292 字节 ONNX 模型（base64），测试不再依赖 `testdata/` |
| `src/engine/onnx_backend.cpp` | DirectML 预加载目录改为从 `onnxruntime.dll` 实际路径推导 |
| `tools/make_release.py` | 新增：Windows 分发打包（两种裁剪模式、残留过滤） |
| `android/**` | 新增：完整 Android 工程（21 个文件） |
| `README.md` | 补充「打包发布」与「Android 端」两节 |
| `testdata/` | 清空（保留目录） |

---

## 5. 遗留与风险

| 项 | 状态 | 说明 |
|---|---|---|
| Android 未编译 | ⚠️ 未验证 | 需 SDK 34 + NDK 26/27 + JDK 17；README 已列全部报错对照表 |
| 抽帧被硬解去隔行 | ⚠️ 已知风险 | 个别机型 MediaCodec 输出已去隔行，隔行片会被判逐行。彻底解决要走 `MediaExtractor + MediaCodec + ImageReader` 拿原始 YUV |
| Android 只采样 0.5 秒 | ⚠️ 精度边界 | 对「正片隔行 + 花絮逐行」的碟片，结论只代表采样点 |
| 播放器实时 AI 超分 | ❌ 外部阻塞 | libaji 无 LICENSE，商用需作者书面授权 |
| 场序判定 | ⚠️ 精度边界 | 元数据优先 + 缺省 TFF；合成 BFF 样本会被报成 TFF（有覆盖手段） |
| 包体 64MB | 可接受 | 其中 mpv 121MB 压缩后占大头；`--no-mpv` 可出精简包 |
