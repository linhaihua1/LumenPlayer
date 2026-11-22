# LumenPlayer（流明播放器）

把 90—00 年代的 480p / 720p 影视素材，增强到 1080p「蓝光级观感」。

> 「蓝光级观感」是**观感目标**，不是分辨率承诺。480p 素材不可能还原从未记录的信息；
> 真正的蓝光来自 35mm 胶片扫描。本产品做的是：不新增伪影、锐度提升、时序稳定、保留颗粒。

需求与设计文档在：[`LumenPlayer_需求与设计文档/`](LumenPlayer_需求与设计文档/)（16 篇，按开发阶段编号）

---

## 现在能做什么

**发布物只有一个文件：`dist/LumenPlayer.exe`（约 124MB，双击即用，免安装）。**
mpv 运行时以资源形式嵌在里面，首次启动自动释放到 `%LOCALAPPDATA%\LumenPlayer`，
之后直接复用。用户不需要知道 mpv 是什么，也不需要挑该点哪个 exe。

源码里还有三个命令行工具（`lumen-probe` / `lumen-render` / `lumen-batch`），
它们是**开发者工具，不随发布包分发**，需要时自己编译。

| 程序 | 作用 | 是否发布 |
|---|---|---|
| `LumenPlayer.exe` | 播放器：拖放/选择文件 + 自动场处理 + A/B 对比 + 进度条 + 信息栏 + 画质设置 | ✅ 唯一发布物 |
| `lumen-probe` | 片源自检：判隔行 / telecine / 色彩 / 颗粒，给出处理方案 | 开发工具 |
| `lumen-render` | 单文件离线导出 1080p（含音轨封装） | 开发工具 |
| `lumen-batch` | 目录批量导出、断点续跑、CSV 报告 | 开发工具 |
| `lumen_tests` | 151 项单元测试 | 开发工具 |

规模：41 个源文件、约 11000 行（含注释）。

---

## 四条处理链路

```
① 自检    ffprobe → 抽帧 → 场结构/色彩/颗粒判定 → ProcessingChain
② 导出    ffmpeg 解码 → [策略前处理] → ISRBackend 超分 → ffmpeg 编码 → 音轨封装
③ 批量    扫描目录 → 逐个调起 ②（子进程隔离） → 日志 + CSV 报告
④ 播放    lumen-player(UI) ⇄ 命名管道 ⇄ mpv.exe（解码/渲染/字幕）
              mpv 侧用 vf 挂载同一套场处理滤镜，A/B 一键切换
```

**同一个 `ProcessingChain` 同时驱动 ② 和 ④**：导出看到的效果和播放看到的一致，
不会出现「预览挺好、导出翻车」。

---

## 目录结构

```
LumenPlayer/
├── LICENSE                 LNC-SL 1.0：源码公开，非商业免费，商用需授权
├── THIRD-PARTY-NOTICES.md   第三方组件许可边界（mpv GPL、微软 DLL 等）
├── lumen.example.json      配置模板（复制为 lumen.json 生效）
├── build.bat / build.sh    一键构建（无需 CMake）
├── dist/                   发布物：只有一个 LumenPlayer.exe（构建产物，不入库）
├── android/                Android 工程（复用 src/core，独立 Gradle 构建）
├── tools/build_single.py   构建单文件 exe（嵌入 mpv 运行时）
├── tools/mpv-dist/         嵌入用的 mpv 运行时源文件（不入库，自备 mpv.exe + d3dcompiler_43.dll）
├── tools/gen_test_model.py 生成测试用 ONNX 模型（验证 ORT 链路，几 KB）
├── tools/ui_probe.py       无人值守界面探针（启动→注入点击→按客户区截图）
├── testdata/               本地测试素材目录（素材不入库，留目录占位）
├── third_party/onnxruntime/ 官方 C API 头文件（仅声明，运行期动态加载）
├── src/
│   ├── core/               ★ 产品层：纯 C++17，零第三方依赖（可搬去 Android/鸿蒙）
│   │   ├── types / json / frame_desc / frame_buffer
│   │   ├── field_analyzer   ★ 隔行 / telecine 判定
│   │   ├── color_policy     ★ 601/709 与 range 决策
│   │   ├── grain_policy     ★ 颗粒估计与回注决策
│   │   ├── source_info      ffprobe 解析 + 元数据自检
│   │   ├── processing_chain ★ 处理链（接缝 3）
│   │   └── config           配置文件读写（含相对路径解析）
│   ├── engine/             引擎层
│   │   ├── sr_backend       引擎契约（接缝 2）+ 工厂
│   │   ├── dummy_backend    CPU 双三次（画质基线）
│   │   └── onnx_backend     ONNX Runtime（DML/CUDA/CPU，分块+羽化）
│   ├── platform/
│   │   ├── process          子进程执行 / 异步启动
│   │   ├── stream_pipe      流式管道（导出用）
│   │   └── mpv_ipc          mpv JSON IPC 客户端（命名管道）
│   ├── service/inspector    片源自检编排
│   └── apps/                probe / render / batch / player
└── tests/test_main.cpp      单元测试（无第三方框架）
```

标 ★ 的模块是真正的差异化：上游 40+ 个开源项目里没有等价物（它们 95% 做的是
动漫 1080p→4K，不做隔行与色域）。

---

## 编译

```bat
build.bat
```

或

```bash
./build.sh test      # 编译并跑测试
```

- 编译器：GCC 9+ / Clang 10+ / MSVC 2019+，C++17
- **编译期零第三方依赖**：ONNX Runtime 只用到官方头文件里的声明，
  运行期用 `LoadLibrary` 动态解析，不需要下载 100MB SDK
- 没用系统包管理器的便携工具链可放在 `%USERPROFILE%\tools\mingw64`，
  `build.bat` / `build.sh` / `tools\build_single.py` 都会自动找到它

## 打包发布（Windows）

```bash
python tools/build_single.py              # 产出 dist/LumenPlayer.exe（单文件，约 124MB）
python tools/build_single.py --no-embed   # 只编译，不嵌 mpv（3MB，开发调试用）
```

产物只有一个文件：

```
dist/LumenPlayer.exe     双击运行，免安装、免解压、无需旁边的 mpv 目录
```

### 为什么是 124MB

mpv 的静态构建本身就有 121MB（解码器、滤镜、字幕、渲染器全在里面），
再加上 ~4MB 的 d3dcompiler_43.dll 和本程序自身。这是「单文件 + 零安装」的代价，
换来的是用户拿到就能播，不用先研究哪个 exe 是主程序。

### 运行时释放规则

| 情况 | 行为 |
|---|---|
| 首次运行 | 把内嵌的 mpv 释放到 `%LOCALAPPDATA%\LumenPlayer\`（约 1—3 秒） |
| 之后运行 | 校验文件大小一致就直接复用，不再写盘 |
| 同目录有 `mpv.exe` / `mpv\mpv.exe` | 优先用用户自己的那份（便于替换 mpv 版本） |
| `lumen.json` 里配了 `player.mpv_path` | 最高优先级 |

释放用的是「先写 `.tmp` 再原子替换」，所以升级版本、或多个实例同时启动都不会
留下半截文件。

### 关于许可

**本项目整体开源（源代码公开），但商用受限。**

- 自有代码采用 [LICENSE](./LICENSE)（**LNC-SL 1.0**）：个人学习、研究、评测与社区无偿共享免费；
  销售、商业产品内置、SaaS、企业主营业务使用等**需事先取得书面授权**。
- 本协议不是 OSI 认证的开源许可证（OSI 不允许限制商用）。对外描述请用
  「源代码公开、非商用免费、商用需授权」，不要只写「MIT / Apache / 开源」。
- 第三方组件不受本协议约束，见 [THIRD-PARTY-NOTICES.md](./THIRD-PARTY-NOTICES.md)。
  mpv 是 GPLv2+；本程序以**独立进程 + JSON IPC** 调用它，没有链接其代码
  （ADR-001 进程边界隔离），但单文件里内嵌 `mpv.exe` 仍需遵守其 GPLv2+ 条款。
- 单文件发布不随附许可文件；程序内 **右键 → 关于 LumenPlayer** 显示许可摘要、
  mpv 版本、许可与源码地址。完整文本见仓库根目录的 `LICENSE`。

---

## Android 端

见 [`android/README.md`](./android/README.md)。

现状：**源码工程，不含预编译 APK**。播放用 Media3，场结构判定直接编译仓库的
`src/core`（不复制代码，两端结论必须一致）。播放路径的画面处理与 AI 超分尚未接入，
README 里写了按性价比排序的三步路线。

---

## 运行依赖

**普通用户：不需要任何依赖。** `dist/LumenPlayer.exe` 自带 mpv 运行时。

下面这张表是给**开发者**的（用 `--no-embed` 编译，或跑 lumen-render 等命令行工具）：

| 依赖 | 谁需要 | 放哪 |
|---|---|---|
| `mpv.exe`（v0.38+） | 只有开发构建的播放器 | exe 同目录 / `mpv\mpv.exe`，或 `lumen.json` 的 `player.mpv_path` |
| `onnxruntime.dll`（≥1.12） | 只有 `--backend ort` | 同上，或 `engine.runtime_dir` |
| `DirectML.dll`（≥1.15） | 只有 `--provider dml` | **必须与 `onnxruntime.dll` 同目录**；系统 `System32` 里那份是 1.0.x，太旧且会被优先命中，本程序会从自己目录抢先加载 |

GPU 运行时（`onnxruntime.dll` + `DirectML.dll`）不在本仓库里，一条命令取：
`python tools/dl_ort_dml.py`，它会解到 `tools/ort-dml/win-x64/`，随后
`lumen-render ... --provider dml --runtime-dir tools/ort-dml/win-x64` 即可直接试。

`tools/mpv-dist/`（嵌入单文件用的 mpv 运行时）同样不入库，自行放 `mpv.exe` +
`d3dcompiler_43.dll` 到该目录，或让 `build_single.py` 走 `find_mpv()` 的其他候选路径。

两者都不装也能跑：`lumen-probe` / `lumen-render`（dummy 后端）/ `lumen-batch` 完全不依赖它们。

---

## 用法

### 1. 片源自检

```bat
lumen-probe.exe "D:\剧集\S01E01.mkv"
lumen-probe.exe "D:\剧集\S01E01.mkv" --json
```

### 2. 离线导出

```bat
lumen-render.exe "in.mkv" "out.mp4" --limit 300      :: 先小段验证
lumen-render.exe "in.mkv" "out.mp4" --dry-run        :: 只看命令
lumen-render.exe "in.mkv" "out.mp4" --backend ort --model models\realesr-general-x4v3.onnx --provider dml
```

### 3. 批量导出

```bat
lumen-batch.exe "D:\老剧\" -o "D:\输出\"
lumen-batch.exe list.txt --preset quality --crf 16
lumen-batch.exe "D:\老剧\" --dry-run
```

输出 `原名_1080p.mp4`，已存在的自动跳过（断点续跑），
每个任务的日志在 `logs/`，总表在 `batch_report.csv`（带 BOM，Excel 直接开）。

### 4. 播放器

双击 `LumenPlayer.exe` 即可，不需要命令行。

```bat
LumenPlayer.exe                        :: 打开空白窗口，拖文件进来 或 点「打开文件」
LumenPlayer.exe "影片.mkv"              :: 直接播放
LumenPlayer.exe --self-test "影片.mkv"  :: 无窗口诊断（排查播放问题先跑这个）
```

**界面布局**

```
+------------------------------+-------------------+
|                              |  当前文件          |
|          视频区               |  文件名            |
|                              |  时长 / 分辨率 / 编码 |
|                              |  片源分析（隔行/场处理）|
+------------------------------+  [打开文件] [画质]  |
|  <<10  ⏸  10>>  音量  ——●——  时间  1.0×  ⚙  ⛶  |  [处理] [场序]     |
+------------------------------+-------------------+
```

- **控制条**：播放/暂停、后退 10 秒、前进 10 秒、可拖动进度条、音量、倍速、设置、全屏
- **右侧信息栏**：文件名、时长、分辨率、画面比例、帧率、音视频编码、码率、
  硬件解码方式，以及本产品的核心信息（片源节奏、场处理、色彩、超分状态）
- **设置菜单**（点 ⚙ 或右键任意位置）：窗口尺寸（100%/150%/200%/最大化）、
  画面比例（适应/拉伸/裁剪/原始像素）、输出分辨率（原生/720p/1080p/1440p/2160p）、
  播放速度、信息栏开关、性能浮层开关、场序切换、关于

| 键 | 功能 |
|---|---|
| 空格 | 暂停 / 继续（片尾按空格 = 重播） |
| ← → | 快退 / 快进 5 秒 |
| Shift + ← → | 快退 / 快进 30 秒 |
| PgUp / PgDn | 快进 / 快退 1 分钟 |
| Home / End | 片头 / 片尾 |
| 0—9 | 跳到 0%—90% |
| ↑ ↓ | 音量 ±5，M 静音 |
| 滚轮 | 音量 ±5 |
| **[ ]** | 减速 / 加速（`\` 恢复 1 倍速） |
| **.** , | 逐帧前进 / 后退 |
| **A** | A/B 对比：处理后 ↔ 原片（切换式） |
| **Tab（按住）** | 按住看原片，松开恢复——逐帧比对最顺手 |
| **P** | 场序循环 自动 → 上场优先 → 下场优先（画面抖动时用它排除场序错误） |
| I | 性能浮层开关（分辨率 / 帧率 / 丢帧 / 处理链） |
| S | 信息侧栏开关 |
| F / 双击画面 | 全屏 |
| O | 打开文件 |
| H | 快捷键帮助 + 当前片处理方案 |
| Esc / Q | 退出（全屏时 Esc 先退出全屏） |

进度记忆默认开启：退出时把位置写进 exe 旁的 `lumen_resume.tsv`，下次打开自动跳回
（播放位置 <3 秒或已播到结尾不记）。删掉这个文件即"全部从头开始"。

---

## 三条设计红线（改动前务必先读）

1. **GPL 传染不跨进程。** 解码走 ffmpeg 子进程；播放走 mpv.exe 子进程 + 命名管道，
   都不链接 libav* / libmpv。项目自有代码统一按 LNC-SL 1.0 公开并限制商用；
   不得把 mpv 代码当作自有代码重新授权。
2. **业务层不许出现引擎字样。** `core/` 与 `service/` 不得 include 后端头文件，
   更不得出现 `tensorrt` / `dml` 字符串。一旦泄漏，换后端就要改业务代码。
3. **实时路径禁止 CPU 回读。** `FrameDesc` 契约要求 GPU 帧全程设备常驻。
   当前 CPU 帧路径只用于离线导出与测试。

---

## 已知限制（诚实清单）

| 限制 | 现状 | 说明 |
|---|---|---|
| **播放器没有 AI 超分** | 只做场处理/去块/降噪 | mpv 不能跑 ONNX；GPU 超分要接 libaji，而其仓库无 LICENSE，商用需授权 |
| 实时超分只在离线路径 | `lumen-render` 能出真 1080p | 播放器当前输出原分辨率，HUD 里明确写出这一点 |
| 场序判定仍是"元数据 + 统计" | 像素级判定未做 | 已有 `--field-order auto/tff/bff` 与播放中按 P 手动覆盖 |
| 4:3 默认加左右黑边 | D6 的保守选项 | 裁边铺满作为可选预设，尚未实现 |
| 只支持 YUV420P 8bit | 主路径够用 | 10bit 后续补 |
| 播放器 A/B 是「切换」不是「分屏」 | 单解码器无法同时解两路 | 分屏需要双解码，成本高收益低 |
| `extra_args` 只按空格切分 | 不支持带空格的参数 | 需要时写进 `mpv_path` 同级脚本 |

---

## 单元测试

```bash
./build.sh test
```

覆盖：JSON（含中文/转义/非法）、帧缓冲布局、宽高比分类、**场结构判定**、
色彩推断、颗粒估计、处理链决策、引擎放大与路径校验、ffprobe 解析、
**配置读写与相对路径解析**、**ONNX 后端（往返精度 + 分块无接缝）**。

后两条需要 `onnxruntime.dll` 与测试模型，缺失时打印 `[SKIP]` 而不是失败——
这样 CI 不会因为没装运行时变红，本地又能真正验证。

---

## 验证记录（实测，非推断）

### 片源判定与导出

| 样本 | 自检结论 | 关键验证 |
|---|---|---|
| `int_480i.mpg` | **interlaced**，100% | 输出 24 帧 idet 全 Progressive（原 120 帧全 TFF） |
| `sd_43.mpg` | progressive + SAR 告警 | 内容居中、左右 240px 黑边，**未变形** |
| `prog_720p.mp4` | progressive，BT.709 | 不做矩阵转换 |
| `tc_ntsc.mpg` | **telecine**，90% | 180 帧经 IVTC → **144 帧**，23.976 fps |

### ONNX Runtime 后端

用 `tools/gen_test_model.py` 生成 0.3 KB 的 `nearest2x.onnx`（最近邻 2 倍，
结果确定，可精确断言）：

- 纯色 40×24 → 80×48，逐像素与输入灰度偏差 ≤2（验证通道顺序与色彩往返）
- 分块 64px + 羽化，100×60 → 200×120，整幅亮度极差 ≤3（**验证无接缝**）
- 实机吞吐（CPU）：720×480 → 1440×960，20.4 ms/帧

### 播放器链路（真实 mpv 0.42）

`lumen-player --self-test` 的三个样本输出：

```
int_480i  → file-loaded ✓  挂载 bwdif + deblock + hqdn3d ✓  A/B 切回后 vf=(空) ✓
tc_ntsc   → file-loaded ✓  挂载 fieldmatch+decimate ✓
prog_720p → file-loaded ✓  无需场处理（如实报告）✓
```

GUI 模式已验证：启动后 `lumen-player.exe` 与 `mpv.exe` 同时存活，IPC 与 `--wid`
嵌入均正常。**v1.4.1 补做了界面截图核对**：用 `tools/ui_probe.py` 在单文件发布版上
确认主菜单与「窗口尺寸 / 画面比例 / 输出分辨率 / 播放速度」四组子菜单全部弹出、
勾选正确、文案全中文；进度条拖动、暂停/继续、快进快退另见第 13 篇验证记录。

### 批量导出

3 个文件批量导出全部成功；再次运行 3 个全部「跳过」；CSV 报告与 per-job 日志正确。

### 全屏布局修复（v1.4.2）

修复了「全屏后再退出全屏，进度条和侧栏消失」：mpv 在自己的线程里会把 `--wid`
子窗口拉回整块客户区，覆盖我们的布局；同时判断改动时用的是缓存值而非实际矩形，
导致后续无法纠正。现在按实际矩形比对，并在窗口尺寸变化后延时夺回布局。
连续 3 次全屏切换、最大化/还原均已截图验证。详见
`LumenPlayer_需求与设计文档/15_全屏退出布局丢失修复_v1.4.2.md`。

### 界面收尾（v1.4.1）

`tools/ui_probe.py` 在 `dist/LumenPlayer.exe` 上完成 8 类截图验证：主菜单、四组子菜单、
播放中界面、性能浮层底衬、全中文复核、单文件首次释放后重放。
清空 `%LOCALAPPDATA%\LumenPlayer` 后启动，mpv 运行时自动释放、画面正常。
截图存于 `build/shots/`，逐项结论见 `LumenPlayer_需求与设计文档/14_界面收尾验证与缺陷修复_v1.4.1.md`。


---

## 下一步

1. **装 `onnxruntime.dll` + 一个真实模型**（Real-ESRGAN / RealCUGAN），跑 `--provider dml`
   量化 GPU 性能，看能否满足 23 ms 的实时预算
2. **接 libaji**：真 AI 超分的播放路径；先解决许可（无 LICENSE 文件 = 保留所有权利）
3. **真实老片调参**：合成图只能验逻辑，观感必须拿 6 类真实素材盲测
   （见 `08_W1基准测试执行方案`）
