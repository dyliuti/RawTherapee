#!/usr/bin/env python3
"""
test_rawengine.py — RawTherapee 工程 rawengine-cli 批量解码脚本（非单元测试）

功能：
1) 递归扫描 INPUT_DIR 下所有 RAW/图片
2) 按输入目录结构输出到 OUTPUT_DIR
3) 打印解码进度：已解码/总数 + 当前文件路径
4) 输出耗时统计（总耗时、单张均值）
"""

import os
import sys
import time
import shutil
import pathlib
import tempfile
import subprocess
from collections import defaultdict


# ============================================================
# 路径配置
# ============================================================
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

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff"}


# ============================================================
# 工具函数
# ============================================================
def find_input_files(directory: str) -> list[pathlib.Path]:
    return [
        p for p in pathlib.Path(directory).rglob("*")
        if p.is_file() and p.suffix.lower() in RAW_EXTENSIONS
    ]


def find_output_images(directory: str) -> list[pathlib.Path]:
    return [
        p for p in pathlib.Path(directory).rglob("*")
        if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS
    ]


def expected_output_relpaths(input_root: pathlib.Path, input_files: list[pathlib.Path]) -> set[str]:
    rels = set()
    for f in input_files:
        rels.add(str(f.relative_to(input_root).with_suffix(".jpg")).replace("\\", "/").lower())
    return rels


def decode_all(input_dir: str, output_dir: str, timeout: int = 600) -> tuple[int, list[float], list[str]]:
    """
    返回：
    - returncode（0 成功，非 0 失败）
    - per_file_elapsed（每张耗时秒）
    - errors（失败详情）
    """
    input_root = pathlib.Path(input_dir)
    output_root = pathlib.Path(output_dir)
    output_root.mkdir(parents=True, exist_ok=True)

    input_files = sorted(find_input_files(input_dir))
    total = len(input_files)
    if total == 0:
        return 2, [], [f"输入目录中没有可解码文件: {input_dir}"]

    per_file_elapsed: list[float] = []
    errors: list[str] = []

    for idx, src_path in enumerate(input_files, start=1):
        rel = src_path.relative_to(input_root)
        out_subdir = output_root / rel.parent
        out_subdir.mkdir(parents=True, exist_ok=True)

        print(f"[{ENGINE_NAME}] 解码进度: {idx}/{total}")
        print(f"[{ENGINE_NAME}] 当前文件: {src_path}")

        t0 = time.monotonic()
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
                    timeout=timeout,
                    encoding="utf-8",
                    errors="replace",
                )
            except subprocess.TimeoutExpired:
                errors.append(f"TIMEOUT: {src_path}")
                return 1, per_file_elapsed, errors

        elapsed = time.monotonic() - t0
        per_file_elapsed.append(elapsed)

        if result.returncode != 0:
            errors.append(
                f"RET={result.returncode}: {src_path}\n"
                f"stdout:\n{(result.stdout or '')[:1000]}\n"
                f"stderr:\n{(result.stderr or '')[:1000]}"
            )
            return result.returncode, per_file_elapsed, errors

    return 0, per_file_elapsed, errors


def print_summary(input_files: list[pathlib.Path], output_files: list[pathlib.Path], elapsed_list: list[float], total_elapsed: float) -> None:
    expected = expected_output_relpaths(pathlib.Path(INPUT_DIR), input_files)
    actual = {
        str(p.relative_to(pathlib.Path(OUTPUT_DIR))).replace("\\", "/").lower()
        for p in output_files
    }

    print("\n" + "=" * 60)
    print(" 解码统计")
    print(f" 输入文件数         : {len(input_files)}")
    print(f" 输出图片数         : {len(output_files)}")
    print(f" 总耗时(秒)         : {total_elapsed:.2f}")
    if elapsed_list:
        print(f" 单张平均耗时(秒)   : {sum(elapsed_list) / len(elapsed_list):.2f}")
        print(f" 单张最长耗时(秒)   : {max(elapsed_list):.2f}")
        print(f" 单张最短耗时(秒)   : {min(elapsed_list):.2f}")

    missing = sorted(expected - actual)
    extra = sorted(actual - expected)

    if missing or extra:
        print(" 目录结构校验       : 不一致")
        print(f" 缺失文件数         : {len(missing)}")
        print(f" 多余文件数         : {len(extra)}")
        if missing:
            print(" 缺失示例(前10):")
            for x in missing[:10]:
                print(f"   - {x}")
        if extra:
            print(" 多余示例(前10):")
            for x in extra[:10]:
                print(f"   - {x}")
    else:
        print(" 目录结构校验       : 一致")

    # 按目录统计输出数量
    counter = defaultdict(int)
    out_root = pathlib.Path(OUTPUT_DIR)
    for p in output_files:
        parent = str(p.parent.relative_to(out_root)).replace("\\", "/")
        if parent == ".":
            parent = "(root)"
        counter[parent] += 1

    print("\n 分目录输出数量:")
    for k in sorted(counter.keys()):
        print(f"  {k}: {counter[k]}")

    print("=" * 60)


# ============================================================
# 入口
# ============================================================
def main() -> int:
    print("=" * 60)
    print(" rawtherapee 工程 rawengine-cli 批量解码")
    print(f" 输入目录  : {INPUT_DIR}")
    print(f" 输出目录  : {OUTPUT_DIR}")
    print(f" CLI 路径  : {CLI_EXE}")
    print(f" CLI 存在  : {os.path.isfile(CLI_EXE)}")
    print("=" * 60)

    if not os.path.isfile(CLI_EXE):
        print(f"[ERROR] CLI 不存在: {CLI_EXE}")
        return 1
    if not os.path.isdir(INPUT_DIR):
        print(f"[ERROR] 输入目录不存在: {INPUT_DIR}")
        return 1

    input_files = sorted(find_input_files(INPUT_DIR))
    if not input_files:
        print(f"[ERROR] 输入目录中没有 RAW/图片文件: {INPUT_DIR}")
        return 1

    t0 = time.monotonic()
    code, elapsed_list, errors = decode_all(INPUT_DIR, OUTPUT_DIR)
    total_elapsed = time.monotonic() - t0

    if code != 0:
        print(f"\n[FAILED] 解码失败，returncode={code}")
        for e in errors:
            print(e)
        return code

    output_files = sorted(find_output_images(OUTPUT_DIR))
    print_summary(input_files, output_files, elapsed_list, total_elapsed)
    print("[SUCCESS] 解码完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
