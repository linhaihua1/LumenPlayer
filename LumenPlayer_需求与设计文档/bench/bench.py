#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LumenPlayer W1 基准探针 (bench probe)
-------------------------------------
用途：在写任何产品代码之前，用真实老片片段测出「超分推理到底跑不跑得进单帧预算」。

四个子命令：
  probe    环境探测（ffmpeg / aji_harness / GPU / 驱动），产出 results/probe.json
  extract  从样片抽取 RAW 帧序列（供 harness 直接读）
  run      逐 (片源 × 模型) 跑推理，测量 ms/帧，产出 results/bench.csv
  report   bench.csv → results/report.md（含判定结论）

合规声明：
  本工具仅用于本地技术评估，产物不得分发。上游 mpv-AnimeJaNai 为 CC BY-NC-SA 4.0
  （禁止商用），libaji 无 LICENSE 文件（默认保留所有权利）——仅限自用评估。
  样本素材请使用你合法持有的内容。

依赖：Python 3.8+ 标准库；外部程序 ffmpeg / aji_harness（路径在 config 中配置）。
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

# 单帧预算（ms）：片源帧率 → 可用时间
BUDGET_MS = {23.976: 41.7, 24.0: 41.7, 25.0: 40.0, 29.97: 33.4, 30.0: 33.4}
HEADROOM_RATIO = 0.70  # 保留 30% 余量给解码/后处理/呈现

DEFAULT_CONFIG = {
    "ffmpeg": "ffmpeg",
    "aji_harness": "aji_harness",
    "work_dir": "work",
    "out_dir": "results",
    "clips_csv": "clips.csv",
    "frames": 120,
    "pixel_format": "p010",
    "models": [],
}


# ---------------------------------------------------------------- 基础设施

def load_config(path: str) -> dict:
    cfg = dict(DEFAULT_CONFIG)
    p = Path(path)
    if p.exists():
        cfg.update(json.loads(p.read_text(encoding="utf-8")))
    else:
        print(f"[warn] 未找到配置 {path}，使用默认值")
    base = p.parent if p.parent else Path(".")
    cfg["_base"] = base
    return cfg


def which(cmd: str) -> str | None:
    """解析可执行文件路径：支持 PATH 查找与相对 config 目录的相对路径。"""
    if os.path.isabs(cmd) and Path(cmd).exists():
        return cmd
    found = shutil.which(cmd)
    if found:
        return found
    return None


def run(cmd: list[str], timeout: int | None = 600) -> tuple[int, str]:
    try:
        cp = subprocess.run(cmd, capture_output=True, text=True,
                            timeout=timeout, encoding="utf-8", errors="replace")
        return cp.returncode, (cp.stdout or "") + (cp.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, f"[timeout] {' '.join(cmd)}"
    except FileNotFoundError as e:
        return 127, f"[not found] {e}"
    except OSError as e:  # 被安全策略拦截等
        return 126, f"[blocked] {e}"


def read_clips(cfg: dict) -> list[dict]:
    path = cfg["_base"] / cfg["clips_csv"]
    if not path.exists():
        sys.exit(f"[fatal] 找不到样片清单：{path}")
    rows = []
    with path.open(encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if not (r.get("path") or "").strip():
                continue  # 未填写的模板行跳过
            r["width"] = int(r["width"])
            r["height"] = int(r["height"])
            r["fps"] = float(r["fps"])
            r["seek"] = float(r.get("seek") or 0)
            rows.append(r)
    if not rows:
        sys.exit("[fatal] clips.csv 里没有任何已填写的样片，请先补充 path/width/height/fps")
    return rows


def budget_for(fps: float) -> float | None:
    for k, v in BUDGET_MS.items():
        if abs(fps - k) < 0.2:
            return v
    return None


# ---------------------------------------------------------------- probe

def cmd_probe(cfg: dict) -> None:
    out = {}
    ff = which(cfg["ffmpeg"])
    out["ffmpeg"] = {"path": ff, "version": None}
    if ff:
        _, txt = run([ff, "-version"], timeout=30)
        out["ffmpeg"]["version"] = txt.splitlines()[0] if txt else None

    aji = which(cfg["aji_harness"])
    out["aji_harness"] = {"path": aji,
                          "exists": bool(aji and Path(aji).exists())}

    # GPU：优先 nvidia-smi；其次 PowerShell CIM（wmic 在 Win11 24H2 已移除且常被安全策略拦截）
    gpus = []
    smi = shutil.which("nvidia-smi")
    if smi:
        rc, txt = run([smi, "--query-gpu=name,driver_version,memory.total",
                       "--format=csv,noheader"], timeout=30)
        if rc == 0:
            for line in txt.strip().splitlines():
                if line.strip():
                    gpus.append({"source": "nvidia-smi", "info": line.strip()})
    if not gpus:
        ps = shutil.which("powershell") or shutil.which("pwsh")
        if ps:
            rc, txt = run([ps, "-NoProfile", "-NonInteractive", "-Command",
                           "Get-CimInstance Win32_VideoController | "
                           "Select-Object Name,DriverVersion | ConvertTo-Json -Compress"],
                          timeout=40)
            if rc == 0:
                try:
                    data = json.loads(txt.strip() or "null")
                    for d in (data if isinstance(data, list) else [data] if data else []):
                        if isinstance(d, dict) and d.get("Name"):
                            gpus.append({"source": "cim",
                                         "info": f"{d.get('Name')} / driver {d.get('DriverVersion')}"})
                except json.JSONDecodeError:
                    pass
    out["gpus"] = gpus

    od = cfg["_base"] / cfg["out_dir"]
    od.mkdir(parents=True, exist_ok=True)
    dst = od / "probe.json"
    dst.write_text(json.dumps(out, ensure_ascii=False, indent=2), encoding="utf-8")

    print("\n=== 环境探测 ===")
    print(f"ffmpeg      : {out['ffmpeg']['version'] or '缺失'}")
    print(f"aji_harness : {aji or '缺失'}")
    for g in gpus:
        print(f"GPU         : {g['info'] if 'info' in g else g}")
    print(f"\n已写入 {dst}")
    if not ff or not aji:
        print("[!] 环境不完整，请先补齐 ffmpeg 与 aji_harness 再继续")


# ---------------------------------------------------------------- extract

def cmd_extract(cfg: dict) -> None:
    ff = which(cfg["ffmpeg"])
    if not ff:
        sys.exit("[fatal] 找不到 ffmpeg")
    work = cfg["_base"] / cfg["work_dir"]
    work.mkdir(parents=True, exist_ok=True)
    n = int(cfg["frames"])
    pf = cfg["pixel_format"]

    for c in read_clips(cfg):
        src = Path(c["path"])
        if not src.exists():
            print(f"[skip] {c['id']}: 源文件不存在 {src}")
            continue
        dst = work / f"{c['id']}.raw"
        if dst.exists() and dst.stat().st_size > 0:
            print(f"[ok]   {c['id']}: 已存在，跳过抽取")
            continue
        cmd = [ff, "-hide_banner", "-loglevel", "error", "-nostdin",
               "-ss", f"{c['seek']:.3f}", "-i", str(src),
               "-frames:v", str(n), "-pix_fmt", f"{pf}le",
               "-f", "rawvideo", "-y", str(dst)]
        t0 = time.time()
        rc, txt = run(cmd)
        if rc != 0 or not dst.exists():
            print(f"[fail] {c['id']}: {txt.strip()[:200]}")
            continue
        size = dst.stat().st_size
        expect = c["width"] * c["height"] * 3 * n
        hint = "" if size >= expect * 0.85 else f"  [!] 体积偏小，核对 width/height（实际 {size}B / 预期约 {expect}B）"
        print(f"[ok]   {c['id']}: {size/1e6:.1f} MB, {time.time()-t0:.1f}s{hint}")


# ---------------------------------------------------------------- run

MS_PATTERNS = [
    re.compile(r"(?i)ms\s*(?:/|per\s*)\s*frame\s*[:=]?\s*([\d.]+)"),
    re.compile(r"(?i)frame.*?([\d.]+)\s*ms"),
    re.compile(r"([\d.]+)\s*ms"),
]


def parse_ms_per_frame(text: str) -> float | None:
    for pat in MS_PATTERNS:
        hits = pat.findall(text)
        if hits:
            try:
                return float(hits[-1])
            except ValueError:
                continue
    return None


def cmd_run(cfg: dict, only: str | None, limit: int | None) -> None:
    aji = which(cfg["aji_harness"])
    if not aji:
        sys.exit("[fatal] 找不到 aji_harness，先在 config.json 里配置绝对路径")
    models = cfg.get("models") or []
    if not models:
        sys.exit("[fatal] config.json 里 models 为空")
    work = cfg["_base"] / cfg["work_dir"]
    od = cfg["_base"] / cfg["out_dir"]
    od.mkdir(parents=True, exist_ok=True)

    clips = read_clips(cfg)
    if only:
        clips = [c for c in clips if only in c["id"] or only in c["category"]]
    if limit:
        clips = clips[:limit]

    rows = []
    for c in clips:
        raw = work / f"{c['id']}.raw"
        if not raw.exists():
            print(f"[skip] {c['id']}: 缺 {raw.name}，先跑 extract")
            continue
        budget = budget_for(c["fps"])
        for m in models:
            engine = m["engine"]
            ep = Path(engine)
            if not ep.is_absolute():
                ep = cfg["_base"] / engine
            if not ep.exists():
                print(f"[skip] {c['id']} × {m['id']}: 引擎不存在 {ep}")
                continue
            out_raw = work / f"{c['id']}__{m['id']}.raw"
            cmd = [aji, "--engine", str(ep),
                   "--input", str(raw),
                   "--width", str(c["width"]), "--height", str(c["height"]),
                   "--format", cfg["pixel_format"],
                   "--output", str(out_raw)]
            t0 = time.time()
            rc, txt = run(cmd, timeout=1800)
            wall = time.time() - t0
            ms = parse_ms_per_frame(txt) if rc == 0 else None
            eff_fps = (1000.0 / ms) if ms else None
            verdict = "ERROR"
            if ms and budget:
                if ms <= budget * HEADROOM_RATIO:
                    verdict = "PASS"
                elif ms <= budget:
                    verdict = "MARGINAL"
                else:
                    verdict = "FAIL"
            rows.append({
                "clip_id": c["id"], "category": c["category"],
                "model_id": m["id"], "width": c["width"], "height": c["height"],
                "src_fps": c["fps"], "ms_per_frame": f"{ms:.2f}" if ms else "",
                "eff_fps": f"{eff_fps:.1f}" if eff_fps else "",
                "budget_ms": f"{budget:.1f}" if budget else "",
                "verdict": verdict, "wall_s": f"{wall:.1f}",
                "note": (m.get("note", "") if rc == 0 else txt.strip().replace("\n", " ")[:120]),
            })
            print(f"[{verdict:<8}] {c['id']:<14} × {m['id']:<18} "
                  f"{ms if ms else '-':>7} ms/帧  预算 {budget if budget else '-'} ms")

    dst = od / "bench.csv"
    with dst.open("w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["clip_id"])
        w.writeheader()
        w.writerows(rows)
    print(f"\n已写入 {dst}（{len(rows)} 条记录）")


# ---------------------------------------------------------------- report

def cmd_report(cfg: dict) -> None:
    od = cfg["_base"] / cfg["out_dir"]
    src = od / "bench.csv"
    if not src.exists():
        sys.exit("[fatal] 没有 bench.csv，先跑 run")
    with src.open(encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))

    def num(x):
        try:
            return float(x)
        except (TypeError, ValueError):
            return None

    lines = ["# W1 基准测试报告", "",
             f"- 记录数：{len(rows)}",
             f"- 判定口径：ms/帧 ≤ 单帧预算 × 70% → PASS；≤ 预算 → MARGINAL；否则 FAIL", ""]

    cats: dict[str, list] = {}
    for r in rows:
        cats.setdefault(r["category"], []).append(r)

    lines += ["## 按片源类别", "",
              "| 类别 | 模型 | ms/帧 | 可达帧率 | 预算(ms) | 判定 |",
              "|------|------|-------|----------|----------|------|"]
    for cat, items in cats.items():
        for r in items:
            ms = num(r["ms_per_frame"])
            lines.append(
                f"| {cat} | {r['model_id']} | {ms:.2f} | {r['eff_fps']} | "
                f"{r['budget_ms']} | {r['verdict']} |" if ms else
                f"| {cat} | {r['model_id']} | - | - | {r['budget_ms']} | {r['verdict']} |")

    tally: dict[str, int] = {}
    for r in rows:
        tally[r["verdict"]] = tally.get(r["verdict"], 0) + 1

    lines += ["", "## 结论汇总", "", "| 判定 | 数量 |", "|------|------|"]
    for k in ("PASS", "MARGINAL", "FAIL", "ERROR"):
        if tally.get(k):
            lines.append(f"| {k} | {tally[k]} |")

    lines += ["", "## 决策建议（自动生成，需人工复核）", ""]
    if tally.get("FAIL") or tally.get("ERROR"):
        lines.append("- 存在 FAIL/ERROR：该档模型在目标硬件上不可用于实时；"
                     "考虑降一档模型、切 DirectML、或改走离线重制路线。")
    if tally.get("MARGINAL"):
        lines.append("- 存在 MARGINAL：能跑但无余量，实战中 seek、场景切换、后台占用会掉帧。"
                     "建议作为「画质档」而非默认档。")
    if tally.get("PASS"):
        lines.append("- 存在 PASS：该组合可作为默认档候选，进入 M1 原型验证。")
    lines += ["", "> 提醒：本报告只测推理耗时，不含解码/后处理/呈现。"
                  "实际是否掉帧以 M1 原型在 HUD 上的表现为准。"]

    dst = od / "report.md"
    dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"已写入 {dst}")


# ---------------------------------------------------------------- main

def main() -> None:
    ap = argparse.ArgumentParser(description="LumenPlayer W1 基准探针")
    ap.add_argument("cmd", choices=["probe", "extract", "run", "report", "all"])
    ap.add_argument("-c", "--config", default="config.json")
    ap.add_argument("--only", help="只跑 id/category 含该子串的样片")
    ap.add_argument("--limit", type=int, help="最多跑前 N 个样片")
    a = ap.parse_args()

    cfg = load_config(a.config)
    if a.cmd == "probe":
        cmd_probe(cfg)
    elif a.cmd == "extract":
        cmd_extract(cfg)
    elif a.cmd == "run":
        cmd_run(cfg, a.only, a.limit)
    elif a.cmd == "report":
        cmd_report(cfg)
    else:
        cmd_probe(cfg)
        cmd_extract(cfg)
        cmd_run(cfg, a.only, a.limit)
        cmd_report(cfg)


if __name__ == "__main__":
    main()
