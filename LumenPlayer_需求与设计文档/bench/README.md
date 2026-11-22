# bench/ — W1 基准探针（只读评估工具）

**目的**：产物代码之前，先用真实老片测出「推理 ms/帧」，回答 M0 的 Go/No-Go。
**不产出任何产品代码**，全套是外部程序调度 + CSV。

> ⚠️ 合规：仅限本地自用评估，产物不得分发。上游 mpv-AnimeJaNai 为 CC BY-NC-SA 4.0（禁止商用），
> libaji 无 LICENSE 文件（默认保留所有权利）。样本请用你合法持有的素材。

---

## 0. 前置准备（一次性）

| 需要 | 从哪来 | 备注 |
|------|--------|------|
| Python 3.8+ | 系统已有则跳过 | 只用标准库，无需 pip install |
| ffmpeg | 官网/包管理器 | 需带 `-pix_fmt p010le` 输出能力（主流构建都有） |
| `aji_harness.exe` | `the-database/animejanai-inference` 的 release | 与 `aji.dll`、`aji_dml`/`aji_trt` 同目录 |
| ONNX 模型 | `realesr-general-x4v3`（Real-ESRGAN, BSD-3） | 需转 TensorRT engine，见下 |

**构建 TensorRT engine**（DirectML 路径不需要，但 harness 示例走 engine）：

```bash
trtexec --onnx=models/realesr-general-x4v3.onnx --fp16 \
        --minShapes=input:1x3x64x64 \
        --optShapes=input:1x3x480x720 \
        --maxShapes=input:1x3x544x960 \
        --saveEngine=models/realesr-general-x4v3.engine
```

> 形状按你的片源改：480p → `480x720`，576p → `576x720`，720p → `720x1280`。

**若用 DirectML**：ONNX 必须导出到 **opset ≤19**（缺 opset-20 `Gelu` 会静默回落 CPU，慢几十倍）。

---

## 1. 配置

```bash
cp config.example.json config.json
```

改三处：`aji_harness` 绝对路径、`models[].engine` 路径、`clips_csv`（默认同目录）。

`clips.csv` 需手工填 `path / width / height / fps`：

| 列 | 说明 |
|----|------|
| `width`/`height` | **实际帧尺寸**，不是显示尺寸。变形宽银幕片源填编码尺寸（如 720×480） |
| `fps` | 24 / 23.976 / 25 / 29.97 / 30，决定单帧预算 |
| `seek` | 起始秒，挑"有代表性"的位置（见 08 文档选片原则） |

---

## 2. 跑

```bash
cd bench

python bench.py probe            # 环境探测 → results/probe.json
python bench.py extract          # 抽 RAW → work/*.raw
python bench.py run              # 推理计时 → results/bench.csv
python bench.py report           # 生成报告 → results/report.md

# 或一次跑完
python bench.py all

# 先小样本试水
python bench.py run --only dvd_ntsc --limit 1
```

---

## 3. 判定口径

| 判定 | 条件 | 含义 |
|------|------|------|
| **PASS** | ms/帧 ≤ 单帧预算 × 70% | 可作默认档；30% 余量留给解码/后处理/呈现 |
| **MARGINAL** | 预算 × 70% < ms ≤ 预算 | 能跑但无余量；实战 seek/场景切换会掉帧，只配当"画质档" |
| **FAIL** | ms > 预算 | 不可用于实时；降档模型或改走离线路线 |
| **ERROR** | 无 ms 输出 | 先查 engine/形状/后端，别急着下结论 |

单帧预算：24fps→41.7ms、25fps→40.0ms、30fps→33.4ms。

---

## 4. 输出

```
results/
  probe.json    硬件/软件环境快照
  bench.csv     明细（可导入 Excel）
  report.md     汇总表 + 自动结论
work/
  <clip>.raw          抽取的原始帧（p010le）
  <clip>__<model>.raw 推理输出（仅用于人工看画质，体积大，测完可删）
```

`work/` 与 `results/` 都不入库。单个 1080p RAW 帧序列 120 帧约 250MB，注意磁盘。

---

## 5. 常见问题

| 现象 | 原因 |
|------|------|
| `体积偏小，核对 width/height` | clips.csv 的尺寸与实际帧尺寸不符，或 seek 超出片长 |
| 首次跑某个 engine 卡住 1—5 分钟 | TensorRT 在建 engine（每个模型/尺寸/GPU 组合一次），属正常 |
| 所有组合都 ERROR | `aji_harness` 缺 `aji_trt`/`nvonnxparser` 等运行时 DLL，或模型形状不匹配 |
| DirectML 明显比预期慢 | ONNX opset 过高导致算子回落 CPU，重新导出到 opset ≤19 |
| ms/帧 波动 >20% | 后台有占用或散热降频；锁功耗/关后台再跑一次 |
