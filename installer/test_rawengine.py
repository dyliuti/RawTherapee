#!/usr/bin/env python3
"""
test_rawengine.py — 仅执行 RawTherapee rawengine-cli 解码与耗时统计

功能：
1) 递归扫描输入目录中的 RAW/图片文件
2) 逐文件调用 rawengine-cli 解码（保持输出目录结构）
3) 统计每张图片解码耗时
4) 按子目录输出 *_time.txt，记录该子目录每张图耗时与总耗时

用法:
    python test_rawengine.py
"""

import os
import sys
import time
import shutil
import pathlib
import tempfile
import subprocess
from collections import defaultdict


INPUT_DIR = r"C:\pack\file\input"
OUTPUT_DIR = r"C:\pack\file\output\rawtherapee"
CLI_EXE = r"C:\pack\project\raw\RawTherapee\installer\output\bin\rawengine-cli.exe"
ENGINE_NAME = "rawtherapee"

RAW_EXTENSIONS = {
    ".arw", ".cr2", ".cr3", ".nef", ".nrw", ".raf", ".dng",
    ".rw2", ".orf", ".pef", ".srw", ".erf", ".3fr", ".mef",
    ".mrw", ".rwl", ".kdc", ".dcr", ".raw", ".rwz",
    ".jpg", ".jpeg", ".tif", ".tiff",
}


def find_input_files(directory: str) -> list:
    return [
        p for p in pathlib.Path(directory).rglob("*")
        if p.is_file() and p.suffix.lower() in RAW_EXTENSIONS
    ]


def safe_name_from_rel_parent(rel_parent: pathlib.Path) -> str:
    if str(rel_parent) in ("", "."):
        return "root"
    return str(rel_parent).replace("\\", "_").replace("/", "_")


def decode_all(input_dir: str, output_dir: str, timeout_sec: int = 600) -> int:
    input_root = pathlib.Path(input_dir)
    output_root = pathlib.Path(output_dir)
    output_root.mkdir(parents=True, exist_ok=True)

    input_files = sorted(find_input_files(input_dir))
    if not input_files:
        print(f"[{ENGINE_NAME}] 输入目录中没有可解码文件: {input_dir}")
        return 1

    # 记录: 子目录 -> [(文件相对路径, 耗时秒)]
    per_dir_times = defaultdict(list)

    total = len(input_files)
    global_start = time.monotonic()

    for idx, src_path in enumerate(input_files, start=1):
        rel_path = src_path.relative_to(input_root)
        rel_parent = rel_path.parent
        out_subdir = output_root / rel_parent
        out_subdir.mkdir(parents=True, exist_ok=True)

        print(f"[{ENGINE_NAME}] 解码进度: {idx}/{total}")
        print(f"[{ENGINE_NAME}] 当前文件: {src_path}")

        start_one = time.monotonic()

        # 使用临时单文件目录，避免 CLI 递归输入时平铺输出
        with tempfile.TemporaryDirectory(prefix="rt_decode_") as tmp_in:
            tmp_in_path = pathlib.Path(tmp_in)
            staged_src = tmp_in_path / src_path.name
            shutil.copy2(src_path, staged_src)

            cmd = [CLI_EXE, str(tmp_in_path), str(out_subdir)]
            try:
                result = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    timeout=timeout_sec,
                    encoding="utf-8",
                    errors="replace",
                )
            except subprocess.TimeoutExpired:
                print(f"[{ENGINE_NAME}] 超时: {src_path}")
                return 2

        elapsed_one = time.monotonic() - start_one
        per_dir_times[str(rel_parent)].append((str(rel_path), elapsed_one))
        print(f"[{ENGINE_NAME}] 单张耗时: {elapsed_one:.3f}s")

        if result.returncode != 0:
            print(f"[{ENGINE_NAME}] 解码失败: {src_path}")
            out = (result.stdout or "") + (result.stderr or "")
            if out.strip():
                print("--- CLI 输出(前 3000 字符) ---")
                print(out[:3000])
            return result.returncode

    global_elapsed = time.monotonic() - global_start

    # 输出每个子目录的 time 文件
    for rel_parent_str, items in per_dir_times.items():
        rel_parent = pathlib.Path(rel_parent_str)
        target_dir = output_root / rel_parent
        fname = f"{safe_name_from_rel_parent(rel_parent)}_time.txt"
        fpath = target_dir / fname

        sub_total = sum(t for _, t in items)
        with open(fpath, "w", encoding="utf-8") as f:
            f.write(f"engine: {ENGINE_NAME}\n")
            f.write(f"input_dir: {input_dir}\n")
            f.write(f"output_subdir: {target_dir}\n")
            f.write("\n")
            f.write("per-image decode time (seconds):\n")
            for rel_file, t in items:
                f.write(f"- {rel_file}: {t:.6f}\n")
            f.write("\n")
            f.write(f"subdir_total_seconds: {sub_total:.6f}\n")

        print(f"[{ENGINE_NAME}] 已写入耗时文件: {fpath}")

    print("=" * 60)
    print(f"[{ENGINE_NAME}] 全部解码完成")
    print(f"[{ENGINE_NAME}] 总文件数: {total}")
    print(f"[{ENGINE_NAME}] 总耗时: {global_elapsed:.3f}s")
    print("=" * 60)

    return 0


def main() -> int:
    print("=" * 60)
    print(" RawTherapee rawengine 解码脚本")
    print(f" 输入目录  : {INPUT_DIR}")
    print(f" 输出目录  : {OUTPUT_DIR}")
    print(f" CLI 路径  : {CLI_EXE}")
    print(f" CLI 存在  : {os.path.isfile(CLI_EXE)}")
    print("=" * 60)

    if not os.path.isfile(CLI_EXE):
        print(f"[{ENGINE_NAME}] CLI 未找到: {CLI_EXE}")
        return 1
    if not os.path.isdir(INPUT_DIR):
        print(f"[{ENGINE_NAME}] 输入目录不存在: {INPUT_DIR}")
        return 1

    return decode_all(INPUT_DIR, OUTPUT_DIR)


if __name__ == "__main__":
    sys.exit(main())
