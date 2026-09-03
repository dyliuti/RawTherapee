#!/usr/bin/env python3
"""
analyze_perf.py — 解析 rawengine 阶段耗时打点日志，输出各阶段耗时与占比。

配合 rtengine/rawperf.h 的打点使用。开启方式：
    set RAWENGINE_PERF=1
    set RAWENGINE_PERF_LOG=C:\\pack\\file\\perf_rcd.log

用法：
    python analyze_perf.py perf_rcd.log
    python analyze_perf.py perf_amazebilinear.log perf_rcd.log      # 多算法对比
    python analyze_perf.py --by-camera perf_rcd.log                 # 再按相机目录细分

日志行格式：
    rawperf file=<basename> stage=<stage> cost_ms=<double> [k=v ...]
"""

from __future__ import annotations

import argparse
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple

LINE_RE = re.compile(r"^rawperf\s+file=(?P<file>\S+)\s+stage=(?P<stage>\S+)\s+cost_ms=(?P<cost>[0-9.]+)(?P<extra>.*)$")

# 阶段层级：(stage, 缩进层级)。用于展示包含关系，避免把嵌套阶段误当成并列项相加。
STAGE_TREE: List[Tuple[str, int]] = [
    ("cli_total", 0),
    ("decode_total", 1),
    ("load", 2),
    ("rules", 2),
    ("process_image", 2),
    ("stage_init", 3),
    ("preprocess", 4),
    ("demosaic", 4),
    ("capture_sharpening", 4),
    ("get_image", 4),
    ("stage_transform", 3),
    ("color_convert", 4),
    ("geom_transform", 4),
    ("stage_early_resize", 3),
    ("stage_denoise", 3),
    ("stage_finish", 3),
    ("sharpening", 4),
    ("resize", 4),
    ("to_rgba", 2),
    ("fallback_crop", 2),
    ("jpeg_encode", 1),
]

STAGE_NOTE = {
    "cli_total":          "CLI 单张总耗时（解码+编码）",
    "decode_total":       "rawengine_decode 总耗时",
    "load":               "读文件 + libraw unpack + raw2image",
    "rules":              "规则匹配与参数装配（含 DCP 查找）",
    "process_image":      "RT 处理流水线总耗时",
    "stage_init":         "初始化 + 预处理 + demosaic + 取图",
    "preprocess":         "黑电平/坏点/暗场/平场",
    "demosaic":           "去马赛克本体",
    "capture_sharpening": "捕捉锐化（默认关闭）",
    "get_image":          "应用白平衡/曲线，输出 baseImg",
    "stage_transform":    "色彩空间转换 + 几何变换",
    "color_convert":      "输入空间 -> 工作空间（DCP/ICC）",
    "geom_transform":     "旋转/畸变/CA/暗角",
    "stage_early_resize": "提前缩放（fast 流水线）",
    "stage_denoise":      "降噪",
    "stage_finish":       "色调/局部调整/锐化/缩放/输出",
    "sharpening":         "锐化",
    "resize":             "缩放",
    "to_rgba":            "float -> 8bit RGBA",
    "fallback_crop":      "EXIF 兜底裁剪",
    "jpeg_encode":        "JPEG 编码落盘",
}


def parse_log(path: Path) -> Tuple[Dict[str, List[float]], Dict[str, str], Dict[str, Dict[str, List[float]]]]:
    """返回 (stage -> [cost...], file -> demosaic_method, camera -> stage -> [cost...])"""
    per_stage: Dict[str, List[float]] = defaultdict(list)
    methods: Dict[str, str] = {}
    per_camera: Dict[str, Dict[str, List[float]]] = defaultdict(lambda: defaultdict(list))

    if not path.is_file():
        raise SystemExit(f"日志不存在: {path}")

    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE_RE.match(raw_line.strip())
        if not m:
            continue
        stage = m.group("stage")
        cost = float(m.group("cost"))
        fname = m.group("file")
        extra = m.group("extra") or ""

        if stage == "demosaic_method":
            bm = re.search(r"bayer=(\S+)", extra)
            if bm:
                methods[fname] = bm.group(1)
            continue

        per_stage[stage].append(cost)
        if stage == "demosaic":
            mm = re.search(r"method=(\S+)", extra)
            if mm:
                methods.setdefault(fname, mm.group(1))

    return per_stage, methods, per_camera


def fmt(v: float) -> str:
    return f"{v:,.1f}"


def report_single(label: str, per_stage: Dict[str, List[float]], methods: Dict[str, str]) -> None:
    total_ref = per_stage.get("decode_total") or per_stage.get("cli_total")
    ref_mean = statistics.mean(total_ref) if total_ref else 0.0
    n_images = len(total_ref) if total_ref else 0

    print("=" * 92)
    print(f"日志: {label}")
    print(f"样本数: {n_images} 张    decode_total 均值: {fmt(ref_mean)} ms")
    if methods:
        used = sorted(set(methods.values()))
        print(f"demosaic 算法: {', '.join(used)}")
    print("-" * 92)
    print(f"{'阶段':<34}{'次数':>6}{'均值ms':>11}{'中位ms':>11}{'占decode':>10}   说明")
    print("-" * 92)

    for stage, depth in STAGE_TREE:
        vals = per_stage.get(stage)
        if not vals:
            continue
        mean_v = statistics.mean(vals)
        med_v = statistics.median(vals)
        pct = (mean_v / ref_mean * 100.0) if ref_mean > 0 else 0.0
        name = ("  " * depth) + stage
        print(f"{name:<34}{len(vals):>6}{fmt(mean_v):>11}{fmt(med_v):>11}{pct:>9.1f}%   {STAGE_NOTE.get(stage, '')}")

    # 未在层级表中出现的阶段（新增打点时兜底展示）
    unknown = sorted(set(per_stage) - {s for s, _ in STAGE_TREE})
    for stage in unknown:
        vals = per_stage[stage]
        mean_v = statistics.mean(vals)
        pct = (mean_v / ref_mean * 100.0) if ref_mean > 0 else 0.0
        print(f"{stage:<34}{len(vals):>6}{fmt(mean_v):>11}{fmt(statistics.median(vals)):>11}{pct:>9.1f}%   (未分类)")
    print("=" * 92)


def report_compare(runs: List[Tuple[str, Dict[str, List[float]], Dict[str, str]]]) -> None:
    """多份日志横向对比：以第一份为基准，展示各阶段均值差异。"""
    base_label, base_stage, _ = runs[0]

    print()
    print("=" * 92)
    print(f"横向对比（基准 = {base_label}）")
    print("-" * 92)
    header = f"{'阶段':<28}"
    for label, _, _ in runs:
        header += f"{label[:16]:>17}"
    header += f"{'差值ms':>11}{'差异%':>9}"
    print(header)
    print("-" * 92)

    for stage, depth in STAGE_TREE:
        if not any(stage in s for _, s, _ in runs):
            continue
        row = f"{('  ' * depth) + stage:<28}"
        means = []
        for _, per_stage, _ in runs:
            vals = per_stage.get(stage)
            if vals:
                mv = statistics.mean(vals)
                means.append(mv)
                row += f"{fmt(mv):>17}"
            else:
                means.append(None)
                row += f"{'-':>17}"

        if len(means) >= 2 and means[0] is not None and means[-1] is not None:
            delta = means[-1] - means[0]
            pct = (delta / means[0] * 100.0) if means[0] > 0 else 0.0
            row += f"{delta:>+11.1f}{pct:>+8.1f}%"
        print(row)
    print("=" * 92)
    print("差值 = 最后一份 - 基准；负数表示最后一份更快。")


def main() -> int:
    parser = argparse.ArgumentParser(description="分析 rawengine 阶段耗时打点日志")
    parser.add_argument("logs", nargs="+", help="一个或多个 perf 日志文件")
    parser.add_argument("--label", action="append", default=None,
                        help="为对应日志指定显示名（可重复，顺序与日志一致）")
    args = parser.parse_args()

    runs: List[Tuple[str, Dict[str, List[float]], Dict[str, str]]] = []
    for idx, log in enumerate(args.logs):
        path = Path(log)
        per_stage, methods, _ = parse_log(path)
        if not per_stage:
            print(f"警告: {path} 中没有解析到 rawperf 记录，确认 RAWENGINE_PERF=1 是否生效")
            continue
        if args.label and idx < len(args.label):
            label = args.label[idx]
        else:
            # 优先用日志里记录的算法名做标签，便于对比时一眼看出是哪个算法
            used = sorted(set(methods.values()))
            label = used[0] if len(used) == 1 else path.stem
        runs.append((label, per_stage, methods))

    if not runs:
        return 1

    for label, per_stage, methods in runs:
        report_single(label, per_stage, methods)

    if len(runs) >= 2:
        report_compare(runs)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

