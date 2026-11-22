# 11 · 播放体验补齐与 DirectML 真机验证（v1.2）

> 对应代码：本仓库根目录（构建 `./build.sh`，`lumen_tests.exe` 151 项全通过）
> 本文记录 `10` 号文档之后的一轮工作：**性能 HUD、完整播放控制、场序人工覆盖、进度记忆**，
> 以及把 `10` 里标为 ⚠️ 的 **DirectML 真机验证**做到可用。
> 附本轮踩到的 7 个真实缺陷与 3 个 DirectML 排查坑，全部可复现。

---

## 0. 一句话结论

| 项 | 之前（`10` 结束时） | 现在 |
|---|---|---|
| 性能可观测 | 无 | 右上角 HUD：分辨率 / 像素格式 / 硬解 / 进度 / fps / 丢帧 / 处理链 / 是否人工场序 |
| 播放控制 | 暂停、±5s、音量、A/B 切换 | 倍速、逐帧、±30s、静音、**按住 Tab 看原片**、鼠标进度条（mpv OSC） |
| 场序 | 只能自动判定 | `--field-order auto/tff/bff` + 播放中按 **P** 循环覆盖 |
| 进度记忆 | 无 | 退出写入 `lumen_resume.tsv`，下次自动跳回 |
| DirectML | ⚠️ 未验证（当时判断"本机无 DirectML.dll"） | ✅ 真机跑通 `ort(DML)`，并测出 CPU/DML 交叉点 |
| 单元测试 | 138 | **151** |

---

## 1. 本轮交付清单

| # | 功能 | 落地位置 | 对应需求 |
|---|---|---|---|
| 1 | 性能 HUD | `apps/lumen-player/main.cpp`（`HudPaint/HudRefresh`） | F-401 / F-402 |
| 2 | 完整播放控制 + 鼠标进度条 | 同上（`HandleKey`、mpv 参数） | F-102 / F-103 |
| 3 | 按住 Tab 看原片 | 同上（`WM_KEYDOWN` / `WM_KEYUP`） | F-305 |
| 4 | 场序人工覆盖 | `core/processing_chain`（`OverrideFieldOrder`）+ 3 个 CLI + 配置 | F-202 |
| 5 | 进度记忆 | `apps/lumen-player/main.cpp`（`LoadResumeTable` 等） | F-108 |
| 6 | DirectML 真机可用 | `engine/onnx_backend.cpp`（专用符号 + 预加载 DirectML） | F-301 前置 |
| 7 | 性能对照模型 | `tools/gen_test_model.py` 新增 `conv2x.onnx` | 基准测试 |

新增命令行/配置：

```
lumen-probe  --field-order auto|tff|bff|progressive
lumen-render --field-order ... --runtime-dir <目录>
lumen-player --field-order ...
lumen.json   "field_order": "auto",  "player": { "hud": true, "resume": true }
```

---

## 2. 三个值得解释的设计决定

### 2.1 性能 HUD：为什么自己画，而不是用 mpv 的 OSD

三个方案都试过，只有一个能同时满足"不闪、能看清、不挡鼠标"：

| 方案 | 问题 |
|---|---|
| `show-text` 每 500ms 刷一次 | 每次都是一条新 OSD 消息，文字会不断"闪一下"；且纯文本，没有底衬，白字压在亮画面上看不清 |
| 在主窗口上自绘 | mpv 用 `--wid` 创建的子窗口铺满整个客户区，会把父窗口画的东西全盖住 |
| **独立分层窗口（采用）** | `WS_EX_LAYERED` + `WS_EX_TRANSPARENT` + `WS_EX_TOPMOST`：悬浮在视频上、鼠标可穿透、不抢焦点 |

绘制用 `UpdateLayeredWindow` 逐像素 alpha，而不是 `SetLayeredWindowAttributes` 色键：
色键会把字形抗锯齿边缘一起抠掉，文字边缘出现彩色毛刺。

实现上有一个取巧但正确的做法：**GDI 不写 alpha 通道**，所以先把面板填黑、文字画白，
再用"像素亮度"当 alpha（文字处 = 灰度的预乘白，底色处 = 固定 156 的预乘黑）。
一次绘制，不需要两遍合成。

实测的取值都要短超时（200ms）：每个属性一次 IPC 往返，用默认 2 秒会在 mpv 忙于解码时
把 UI 线程卡住，表现为窗口假死。

### 2.2 进度记忆：为什么放弃 mpv 自带的 `--save-position-on-quit`

先试着用 mpv 的 `--save-position-on-quit=yes --watch-later-directory=...`，结果：

- 选项被接受（无报错、退出码 0），但 `watch_later` 目录**一个文件都不生成**；
- 换成规范名 `--watch-later-dir`、换默认目录、走键盘 `q` 退出、播到中途退出——全部无效。

也就是说这套能力在 mpv 0.42（shinchiro build）上**不可用且不报错**。
继续依赖它，等于把产品功能挂在一个"静默失效"的外部行为上。

改为自研，代价只有约 60 行：

- 文件：exe 旁 `lumen_resume.tsv`，制表符分隔 `路径 \t 位置秒 \t 总时长秒`，带 UTF-8 BOM；
- 写入时机：退出前、换片前；
- 阈值（两端必须一致，否则就是"记忆错乱"）：`位置 > 3 秒` 且 `剩余 > 5 秒` 才记录，否则删记录；
- 恢复时机：`loadfile` 之后**不能立刻 seek**（文件还没解析完会失败），
  先记下目标位置，由已有的 500ms 定时器在 `duration` 可读时执行。

选 TSV 而不是 JSON：退出路径上不能有任何可能失败的复杂序列化，而且用户能直接打开看、直接删。

### 2.3 场序覆盖：为什么是"覆盖处理链"而不是"重跑自检"

场序判错的表现是反交错后画面上下轻微抖动，用户很难描述，但一眼能看出"哪个更好"。
所以按 **P** 键的反馈必须是**瞬间**的。

重跑一次自检要 2—10 秒（6 次 ffmpeg 调用），这个延迟会让"对比"这件事根本做不成。
因此实现成：保留自检得到的 `base_chain`，覆盖时从它重新推导（而不是在旧链上叠加），
只改 `assumed_field_order` 与 `field_mode`，然后重挂滤镜——**毫秒级**。

语义约定（有单测守着）：

| 用户指定 | 原结论 | 结果 |
|---|---|---|
| TFF / BFF | 逐行 | **强制改为反交错**（否则"按了 P 没反应"） |
| Progressive | 隔行 | **取消场处理**（逐行素材去隔行会砍掉一半垂直信息） |
| auto | 任意 | 恢复自检原始结论 |

---

## 3. DirectML：从"不可用"到实测可用

`10` 号文档里写的是"本机无 `DirectML.dll`，只验证了回退 CPU"。这一轮把它做通了，
过程是三个连环坑，每一个都会让人得出错误结论。

### 3.1 坑一：NuGet 包里同名 DLL 按架构覆盖

`Microsoft.ML.OnnxRuntime.DirectML` 的 nupkg 里有三份同名 `onnxruntime.dll`
（win-x64 / win-x86 / win-arm64）。第一版脚本按 basename 平铺解压，最后写入的是 **arm64**，
表现为"能加载、但创建会话失败"。

`Microsoft.AI.DirectML` 的包更阴：`bin/` 下有 5 个 `DirectML.dll`，
其中 `x64-xbox-scarlett` 版本 46MB，按 basename 解压会覆盖掉正确的 `x64-win`（18MB）。

修法：**只解 `runtimes/win-x64/native/` 与 `bin/x64-win/`**，按架构建子目录，永不按名平铺。
脚本：`tools/dl_ort_dml.py`。

### 3.2 坑二：通用 provider 接口不覆盖 DML

```
GetAvailableProviders()  →  DmlExecutionProvider, CPUExecutionProvider     ← 说有
AppendExecutionProvider("DmlExecutionProvider") → Unknown provider name...  ← 说没有
```

同一份 DLL，两个接口给出相反答案。原因：ORT 的**字符串式** `SessionOptionsAppendExecutionProvider`
只注册了一部分 provider（实测为 OPENVINO / SNPE / XNNPACK / QNN / WEBNN / AZURE）；
DML 与 CUDA 走的是**专用导出符号** `OrtSessionOptionsAppendExecutionProvider_DML(options, device_id)`。

修法：`GetProcAddress` 取这个符号直接调用（不需要头文件，也不产生链接依赖），
失败时才回退 CPU。这段代码在 `onnx_backend.cpp` 的 `try_dedicated`。

### 3.3 坑三：system32 里的旧版 DirectML 被优先命中

装好运行时后仍然失败，而且错误消息是**空字符串**：

```
OrtSessionOptionsAppendExecutionProvider_DML:  （什么都没有）
```

原因：ORT 的 DML EP 内部是 `LoadLibrary("DirectML.dll")`，走进程默认搜索顺序，
于是命中了 `C:\Windows\System32\DirectML.dll`——Windows 自带的 **1.0.200713**，
而 ORT 1.20 要求 ≥ 1.15。版本不够，创建 EP 失败，且没填错误消息。

修法：加载 `onnxruntime.dll` 之后，**按绝对路径抢先加载同目录的 `DirectML.dll`**，
之后 ORT 的按名加载会直接复用已加载模块。这是 Windows 上"可选运行时"通用的坑。

### 3.4 顺带修的三处诊断能力（比修 bug 更重要）

1. append 失败不再直接 `ReleaseStatus` 丢弃消息，而是逐个 provider 记录原因；
2. 追加 `GetAvailableProviders` 的结果，直接告诉用户"这个 DLL 支持什么"；
3. 空错误消息时补一句"常见原因是 DirectML.dll 版本过低"，不让用户对着一个冒号发呆。

另外，**初始化成功但带告警**（GPU 不可用已回退 CPU）以前是静默的，
现在会打印 `[警告] ...`。否则用户看着 `ort(CPU)` 以为"用了 CPU 加速"，
白等几小时才发现吞吐只有 4 fps。

---

## 4. 性能实测（RTX 2060 / i7 / Windows 11，`int_480i.mpg` 720×480 → 1440×960）

测试模型两个，用途完全不同：

- `nearest2x.onnx`（0.3 KB）：最近邻，**几乎不耗算力**，只验证链路正确性；
- `conv2x.onnx`（307 KB，本轮新增）：32 通道 4 个残差块 + DepthToSpace，**有真实算力需求**。

| 模型 | 后端 | 分块 | ms/帧 | 吞吐 |
|---|---|---|---|---|
| nearest2x | CPU | 256 | **37.1** | 18.2 fps |
| nearest2x | DML | 256 | 50.8 | 13.5 fps |
| conv2x | CPU | 整帧 | 284.2 | 3.5 fps |
| conv2x | **DML** | **整帧** | **115.7** | **8.6 fps** |
| conv2x | DML | 256 | 219.5 | 4.6 fps |
| conv2x | DML | 128 | 287.2 | 3.5 fps |

三条结论：

1. **DML 只在"算力真的成为瓶颈"时才划算。** 最近邻模型上 GPU 反而慢 37%——
   纯粹是每块张量上传/下载与 D3D12 屏障的固定开销。这也是"GPU 一定更快"这种直觉最容易翻车的地方。
2. **分块在 720×480 上是纯损耗。** 整帧 115.7ms，切 256 变 219.5ms（近 2 倍）。
   分块的意义是"防止大帧打爆显存"，不是"提速"；默认 `tile_size = 0`（整帧）是对的。
3. 本轮数字**不代表产品性能**：真实超分模型（Real-ESRGAN 级）算力是本测试模型的 10 倍以上，
   1080p 实时仍需按 23ms/帧的预算做取舍（见 `08` 基准方案）。

---

## 5. 本轮七个真实缺陷（都不是理论问题）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | HUD 出现两行"片源 720x480" | 改代码时把一行 `lines.push_back(buf)` 落在了分支外面 | 删掉；行数从 5 变 6 就能立刻看出来 |
| 2 | HUD 最后一行被裁掉、字也发虚 | 进程未声明 DPI 感知，窗口被系统位图拉伸；面板宽度写死 520 | `SetProcessDPIAware()` + 按文字实测宽度动态定面板尺寸 + 按 DPI 缩放字号 |
| 3 | 时间那行显示 `0 / 0` | MinGW/UCRT 下宽字符 `swprintf` 的 `%s` 行为不一致 | 数字用 `swprintf`，字符串一律用 `std::wstring` 相加 |
| 4 | mpv 的 watch-later 不落盘 | 该 build 上能力静默失效 | 自研 TSV 进度表（见 2.2） |
| 5 | 退出时读 `time-pos` 总是失败 | 主窗口销毁 → `--wid` 子窗口消失 → mpv 立即退出 → 管道断开，全是竞态 | 用定时器缓存的 `last_pos`，退出时不再走 IPC |
| 6 | 进度文件中文注释乱码 | 无 BOM 的 UTF-8 被记事本当本地代码页读 | 写 UTF-8 BOM（与批量 CSV 同一个坑，同一个修法） |
| 7 | 明明有进度记录却"找不到" | 键直接用命令行传入的原始路径：`../testdata/a.mpg` 与 `..\testdata\a.mpg`、相对与绝对，字符串全都不一样 | 键统一规范化为「绝对路径 + 反斜杠 + 小写」（`GetFullPathNameW` + `CharLowerBuffW`），读入时也规范化 |

第 5 条值得单记：**"退出时再做收尾"这个模式在播放器里天然不安全**。
凡是退出时才需要的值，都必须在运行期就采样缓存下来。

---

## 6. 验证证据

### 6.1 单元测试

```
通过 151 项，失败 0 项
```

新增 13 项，重点是三组：

- `ParseFieldOrderAcceptsCommonSpellings`：`auto/TFF/bottom/progressive` 均识别，
  拼错的 `tf` 必须被拒绝（静默接受 = 用户以为设置生效了）；
- `OverrideFieldOrderForcesDeinterlace` / `...ProgressiveCancelsDeinterlace`：覆盖语义的双向断言；
- `ConfigTemplateRoundTrip` 增加 `field_order` / `hud` / `resume` 三个字段的往返检查。

### 6.2 场序覆盖（命令行）

```
lumen-probe testdata/int_480i.mpg --field-order bff
  场处理: deinterlace（场序 bff）
  依据  : ... 用户强制场序 bff；
  - 场序由用户强制指定为 bff（已覆盖自动判定）
```

### 6.3 播放器 HUD（真实截图核对）

720×480 隔行样本，窗口 1280×720，HUD 内容：

```
int_480i.mpg
片源 720x480  yuv420p  软件解码
00:00:03 / 00:00:04   29.97 fps   丢帧 0（解码 0）
处理 deinterlace(tff) + 去块 + 降噪
超分 未接入实时路径（当前输出原分辨率）
```

面板与文字均完整、无裁切；鼠标可穿透到 mpv 的进度条。
**未验证**：鼠标拖动进度条的实际手感（需要人工操作）。

### 6.4 进度记忆（写入 + 跳回，两段都验过）

写入：40 秒样本播放约 14 秒后关闭窗口，`build/lumen_resume.tsv`：

```
﻿# LumenPlayer 播放进度（可安全删除，删除后所有片子从头发起重播）
# path	position_seconds	duration_seconds
c://users//administrator//desktop//lumenplayer//testdata//long_480i.mpg	31.1978	40.04
```

跳回：预先写入 `25.0` 秒，重开同一文件，**4 秒后 HUD 显示 `00:00:29 / 00:00:40`**，
退出时写回 `31.1978`——说明起播位置是 25 秒而不是 0（否则 4 秒处只会是 4 秒左右）。

注意键是绝对路径且全小写：这就是第 5 节第 7 条缺陷的修法，
否则"资源管理器双击"与"命令行相对路径"会各自记一份互不相干的进度。

### 6.5 DirectML

```
[2/4] 超分后端: ort(DML)  720x480 → 1440x960 → 1920x1080
[3/4] 开始处理 ...
```

GPU provider 生效，无回退告警；对比数据见第 4 节。

---

## 7. 未完成与风险（对 `10` §7 的更新）

| 项 | `10` 里的状态 | 现在 | 说明 |
|---|---|---|---|
| DML 真机验证 | ⚠️ | ✅ 已完成 | 本机 RTX 2060 跑通，含三处坑的修法 |
| 场序人工覆盖 | ❌ | ✅ 已完成 | CLI + 配置 + 播放器 P 键 |
| 播放器 AI 超分 | ❌ | ❌ 仍阻塞 | mpv 不能跑 ONNX；libaji 仓库**无 LICENSE**，商用需书面授权 |
| 分屏 A/B | ❌ | ❌ 不做 | 单解码器解不了两路；已提供"按住 Tab"作为等价手段 |
| 真实老片调参 | ❌ | ❌ 待素材 | 合成素材只能验逻辑 |
| 10bit / NV12 | ❌ 明确拒绝 | ❌ 仍拒绝 | ONNX 后端拒绝 P010，不做静默降转（避免画质问题难归因） |
| 鼠标进度条手感 | — | ⚠️ 待人工确认 | OSC 已启用，未做人工拖动验证 |
| 进度记忆的跨盘/改名场景 | — | ⚠️ 未测 | 键是绝对路径，文件改名或移动后旧记录失效（可接受，但会留下无用条目，暂无清理策略） |

---

## 8. 对既有文档的修订建议

1. `05` 里程碑 **W5「接 libaji / ORT」**：ORT 部分可标"离线 + GPU 双路径已验证"，libaji 仍为"待授权"。
2. `03` PRD **F-401 性能 HUD** 可标"已交付"，但验收口径需注明：当前 HUD 显示的是
   **解码/渲染侧指标**（fps、丢帧），引擎侧 ms/帧在播放路径未接入前无值可显示。
3. `02` 测试点 Checklist 增加三条：
   - 场序覆盖后画面抖动是否改善（人工目测）；
   - 按住 Tab 松开后是否恢复到"按住之前"的状态（而不是无条件恢复到处理后）；
   - 进度记忆在"播到结尾后关闭"时是否清除记录。
