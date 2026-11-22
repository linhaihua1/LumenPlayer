# LumenPlayer · Android

老片观感增强播放器的 Android 端。**这是源码交付，不是预编译 APK**——
需要你用自己的 Android 环境编一次（下面第 2 节）。

---

## 1. 它现在能干什么

| 能力 | 状态 |
|---|---|
| 选择视频并播放（Media3 / ExoPlayer，硬解直通） | ✅ |
| 从其他应用「用 LumenPlayer 打开」接收视频 | ✅ |
| 自动抽帧 → 判定片源是逐行 / 隔行 / 3:2 pulldown | ✅ |
| 显示判定依据、置信度、建议（含场序 tff/bff） | ✅ |
| 播放时实际应用反交错 / IVTC 滤镜 | ❌ 未做 |
| AI 超分（480p → 1080p） | ❌ 未做 |
| 控制台式诊断（对应桌面 `--self-test`） | ❌ 未做 |

判定逻辑与桌面版**是同一份 C++ 代码**（`../src/core/`），不是另写一遍。
所以同一片源在两端应该得到相同结论；如果不同，那是 bug。

---

## 2. 环境要求

| 组件 | 版本 | 说明 |
|---|---|---|
| JDK | **17** | AGP 8.5 硬性要求，JDK 8/11 会直接报 "Unsupported class file" |
| Android Studio | Ladybug (2024.2) 或更新 | 也可以纯命令行 |
| Android SDK Platform | **34** | compileSdk / targetSdk |
| Android SDK Build-Tools | 34.0.0 | |
| NDK | **26.x 或 27.x** | 编 `liblumen_core_jni.so` |
| CMake | **3.22.1** | 与 NDK 自带版本一致，SDK Manager 里单独装 |

命令行编译（Windows）：

```bash
cd android
# 指向你的 SDK（Android Studio 装的通常在 %LOCALAPPDATA%\Android\Sdk）
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
set JAVA_HOME=C:\path\to\jdk-17
gradlew.bat assembleDebug
```

产物：

```
android/app/build/outputs/apk/debug/app-debug.apk
```

装到设备：

```bash
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

> 仓库里已带 `gradle-wrapper.jar` 与 `gradlew`，不需要本机预装 Gradle。
> `gradle/wrapper/gradle-wrapper.properties` 里的下载地址指向腾讯云镜像
> （`services.gradle.org` 在国内常长时间卡住）。网络条件好可以改回官方。

**网络不通时**：`settings.gradle.kts` 已经把阿里云 Maven 镜像排在官方源前面。
若仍然拉不到依赖，检查 `maven.aliyun.com` 是否可达。

---

## 3. 目录结构

```
android/
├── settings.gradle.kts              仓库配置（镜像优先）
├── build.gradle.kts                 插件版本
├── gradle.properties                JVM 内存等
├── gradlew / gradlew.bat / wrapper   Gradle 自举
└── app/
    ├── build.gradle.kts             compileSdk / ABI / CMake 接入点
    ├── proguard-rules.pro           JNI 方法名保留规则
    └── src/main/
        ├── AndroidManifest.xml      SAF 选片，不申请存储权限
        ├── cpp/
        │   ├── CMakeLists.txt       ★ 把 ../../../../../src/core 编进 .so
        │   └── lumen_jni.cpp        JNI 桥：字节数组 ↔ FrameBuffer ↔ JSON
        ├── java/com/lumen/player/
        │   ├── NativeAnalyzer.kt    external 声明 + 延迟加载 .so
        │   ├── SourceAnalyzer.kt    抽帧（平台相关）+ 调 native + 解析结果
        │   └── MainActivity.kt      播放器 + 结论展示
        └── res/                     布局 / 主题 / 图标（图标是 vector，无二进制）
```

---

## 4. 设计上的三个关键决定

### 4.1 core 层共享，不复制

`CMakeLists.txt` 直接编译仓库根目录的 `src/core/*.cpp`：

```cmake
set(LUMEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../..")
```

**为什么必须共享**：判定算法一旦出现两个副本，两边就会漂移，
最终表现为「同一部片子，电脑判隔行、手机判逐行」——
这种问题极难排查，因为两边单看都"有道理"。

代价：`android/` 目录**不能单独拷出去**，必须留在仓库内。
CMake 里加了 `EXISTS` 检查，位置不对会直接给出明确报错，而不是一堆找不到头文件。

`config.cpp` 被排除在外：它用了 `GetFullPathNameW` 等 Windows API。
Android 侧设置用 `SharedPreferences` 管理，不需要也不该把 Windows 配置逻辑编进来。

### 4.2 JNI 只传亮度平面

`SourceAnalyzer` 把 `Bitmap` 转成 8bit Y 平面（BT.601 studio swing）传进去，
JNI 侧逐行 memcpy 进 `FrameBuffer` 的 Y 平面。

- 场结构判定（梳齿、场间行差）本质是亮度域的差分，色度不参与
- 传 YUV 全帧会让 JNI 拷贝量翻 1.5 倍，对结论零影响
- **逐行拷贝不能省**：`FrameBuffer` 的 stride 是 32 字节对齐的，
  整块 memcpy 会让第 2 行起全部错位

### 4.3 抽帧必须连续，且不能缩放

这两个约束违反了不会报错，只会悄悄判错：

1. **不能缩放**。梳齿是「相邻两行属于不同场」造成的单行尖刺。
   把 720×480 缩到 360×240，两行会被平均成一行，梳齿直接被抹平 →
   算法看到干净画面 → 结论必然是「逐行」。
2. **不能均匀撒点**。3:2 pulldown 的判据是相邻帧之间的场重复模式
   （新/新/重/新/重）。每 1 秒取一帧，帧间隔变成 30 帧，这个周期根本不存在，
   telecine 永远检测不到。

所以实现是：从 40% 位置起**连续取 16 帧**（步长 33ms）。

补第二段的条件是「第一段判成逐行或判不出来」——
静止镜头（特写、空镜）本身没有运动，梳齿和场重复都看不出来，容易误判为逐行。
换到 75% 位置再测一次，取更"严重"的结论。对播放器来说，
把隔行误判成逐行（放着梳齿不管）比反过来糟糕得多。

---

## 5. 后续怎么往下做（按性价比排序）

### 第 1 步：让判定真的改善画面（1—2 天）

现在判定出来了但没作用在画面上。最小可行的接法是用 Media3 的
`Transformer` / `Effect`（`androidx.media3:media3-transformer`）：

- 逐行 → 不做任何事
- 隔行 → 挂 `yadif`/`bwdif` 等价的 GLSL 或 CPU 滤镜
- telecine → fieldmatch + decimate（这一步在移动端 CPU 上代价不小，先只做隔行）

**验收标准**：用一段 480i 老剧，暂停在运动画面上，A/B 能看到梳齿消失。

### 第 2 步：换成 libmpv 播放内核（约 1—2 周）

只有它能让 `bwdif` / `fieldmatch` / `ivtc` 这些成熟的 FFmpeg 滤镜直接可用，
和桌面版行为一致。要点：

1. 交叉编译 libmpv（NDK + meson + ninja），或使用社区预编译产物
2. 渲染走 `android-surface`：`--vo=mediacodec_embed` 或 `gpu` + `ANativeWindow`
3. 用 JNI 下发 `vf` 命令（桌面版 `BuildMpvFilters` 的逻辑可以照搬）
4. 音频输出用 `--ao=audiotrack`

注意许可：桌面版用的 libmpv 是 GPLv2+，Android 端同样受 GPL 约束，
上架前需评估（要闭源分发就得换成 LGPL 配置的构建，并去掉 GPL-only 组件）。

### 第 3 步：AI 超分（约 2—3 周）

- 运行时：ONNX Runtime Mobile（比完整版小很多）或 NCNN / MNN
- 模型：Real-ESRGAN 系列需转成 NNAPI / Vulkan 后端可跑的格式
- 现实预期：骁龙 8 系跑 480p→1080p 大约 30—80ms/帧，
  意味着 24fps 勉强实时、30fps 会掉帧。**别承诺"实时 1080p"**，
  更诚实的做法是「离线导出 1080p，播放时只做反交错」。

---

## 6. 已知风险（都是真的，不是免责声明）

| 风险 | 说明 |
|---|---|
| 硬解可能自带去隔行 | 个别机型 / 编码组合下，`MediaCodec` 输出前已做反交错，此时隔行片会被判成逐行。**验证方法**：用桌面版 `lumen-probe` 确认片子是隔行，再在手机上比对结论；不一致就是踩到这个坑。彻底解决要绕过 `MediaMetadataRetriever`，走 `MediaExtractor + MediaCodec + ImageReader` 拿原始 `YUV_420_888` 平面。 |
| 抽帧是逐帧 seek，慢 | `getFrameAtTime` 每次调用可能要从关键帧解到目标位置。1080p 上 16 帧约 2—4 秒。这是"分析一次、结果复用"的定位，不是每帧都分析。 |
| 只采样 0.5 秒 | 全片 45 分钟只看两个 0.5 秒窗口。对「正片隔行 + 花絮逐行」的碟片，结论只代表采样点。桌面版是靠多采样点 + 元数据交叉验证缓解的，这里还没有。 |
| Media3 版本固定 | `media3-*:1.4.1` 与 `compileSdk 34` 匹配。若升级 Media3 到 1.5+，需同步把 compileSdk 提到 35。 |

---

## 7. 常见报错

| 现象 | 原因 / 处理 |
|---|---|
| `Unsupported class file major version 61` | 用了 JDK 17 以下。设 `JAVA_HOME` 到 JDK 17。 |
| `NDK not configured` | SDK Manager 里装 NDK（26.x/27.x），或在 `local.properties` 写 `ndk.dir`。 |
| `CMake '3.22.1' was not found` | SDK Manager → SDK Tools → 勾选 CMake 3.22.1。 |
| `找不到 core 源码：...` | `android/` 被拷到仓库外了。放回 `LumenPlayer/android/`。 |
| 运行时报 `UnsatisfiedLinkError` | 设备 ABI 不在 `abiFilters` 里（默认 arm64-v8a / armeabi-v7a / x86_64）。看 `NativeAnalyzer.loadError`。 |
| 界面显示"原生库加载失败" | 同上；也可以先跑 `adb logcat -s LumenJni` 看 native 侧日志。 |
