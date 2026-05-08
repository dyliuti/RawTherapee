#!/usr/bin/env python3
"""
test_rawengine.py — RawTherapee 工程 rawengine 单元测试

测试 RawTherapee 工程 rawengine-cli，将转码图片输出到:
    C:/download/output_rawtherapee

用法:
    python test_rawengine.py           # 运行所有测试
    python test_rawengine.py -v        # 详细输出
    python -m pytest test_rawengine.py -v  # pytest 方式运行
"""

import os
import sys
import subprocess
import time
import pathlib
import unittest
import statistics
from PIL import Image

# ============================================================
# 路径配置
# ============================================================
INPUT_DIR  = r"C:\download\input"
OUTPUT_DIR = r"C:\download\output_rawtherapee"

# rawengine-cli 可执行文件路径（RawTherapee 工程编译产物）
CLI_EXE = r"C:\project\raw\RawTherapee\installer\output\bin\rawengine-cli.exe"

ENGINE_NAME = "rawtherapee"

# RAW/图片文件扩展名（rawengine-cli 支持的格式）
RAW_EXTENSIONS = {
    ".arw", ".cr2", ".cr3", ".nef", ".nrw", ".raf", ".dng",
    ".rw2", ".orf", ".pef", ".srw", ".erf", ".3fr", ".mef",
    ".mrw", ".rwl", ".kdc", ".dcr", ".raw", ".rwz",
    ".jpg", ".jpeg", ".tif", ".tiff",
}

# ============================================================
# 工具函数
# ============================================================

def run_cli(input_dir: str, output_dir: str, timeout: int = 600) -> tuple:
    """
    运行 rawengine-cli，返回 (returncode, stdout+stderr, elapsed_seconds)。
    output_dir 会自动创建。
    """
    os.makedirs(output_dir, exist_ok=True)
    t0 = time.monotonic()
    try:
        result = subprocess.run(
            [CLI_EXE, input_dir, output_dir],
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace",
        )
    except subprocess.TimeoutExpired:
        return -1, "TIMEOUT", time.monotonic() - t0
    elapsed = time.monotonic() - t0
    combined = result.stdout + result.stderr
    return result.returncode, combined, elapsed


def find_input_files(directory: str) -> list:
    """返回 directory 下所有 RAW/图片文件路径（递归）。"""
    return [
        str(p) for p in pathlib.Path(directory).rglob("*")
        if p.is_file() and p.suffix.lower() in RAW_EXTENSIONS
    ]


def find_output_images(directory: str) -> list:
    """返回 directory 下所有输出图片（jpg/png/tiff）路径（递归）。"""
    image_exts = {".jpg", ".jpeg", ".png", ".tif", ".tiff"}
    return [
        str(p) for p in pathlib.Path(directory).rglob("*")
        if p.is_file() and p.suffix.lower() in image_exts
    ]


def validate_image(path: str) -> dict:
    """
    用 Pillow 打开并验证图片，返回验证信息字典：
    { "ok": bool, "width": int, "height": int, "mode": str, "error": str|None }
    """
    try:
        with Image.open(path) as img:
            img.verify()
        with Image.open(path) as img:
            w, h = img.size
            mode = img.mode
        return {"ok": True, "width": w, "height": h, "mode": mode, "error": None}
    except Exception as e:
        return {"ok": False, "width": 0, "height": 0, "mode": "", "error": str(e)}


# ============================================================
# 测试类
# ============================================================

class TestRawTherapeeEngine(unittest.TestCase):
    """RawTherapee 工程 rawengine-cli 单元测试。"""

    def setUp(self):
        if not os.path.isfile(CLI_EXE):
            self.skipTest(
                f"[{ENGINE_NAME}] CLI 未找到: {CLI_EXE}\n"
                "  请先构建 RawTherapee 工程（参考 RawTherapee/INSTALL 文档）。"
            )
        if not os.path.isdir(INPUT_DIR):
            self.skipTest(f"输入目录不存在: {INPUT_DIR}")

        self.input_files = find_input_files(INPUT_DIR)
        if not self.input_files:
            self.skipTest(f"输入目录中没有 RAW/图片文件: {INPUT_DIR}")

    # ------------------------------------------------------------------
    # Test 01: CLI 正常退出
    # ------------------------------------------------------------------
    def test_01_cli_exits_successfully(self):
        """CLI 应以 returncode=0 退出。"""
        ret, output, elapsed = run_cli(INPUT_DIR, OUTPUT_DIR)
        print(f"\n  [{ENGINE_NAME}] 耗时 {elapsed:.1f}s  returncode={ret}")
        if output.strip():
            print("  --- CLI 输出 (前 3000 字符) ---")
            print(output[:3000])
        self.assertEqual(
            ret, 0,
            f"[{ENGINE_NAME}] CLI 返回非零退出码 {ret}\n{output[:500]}"
        )

    # ------------------------------------------------------------------
    # Test 02: 输出文件数量匹配输入
    # ------------------------------------------------------------------
    def test_02_output_files_created(self):
        """输出文件数量应等于输入文件数量。"""
        run_cli(INPUT_DIR, OUTPUT_DIR)
        output_files = find_output_images(OUTPUT_DIR)
        self.assertGreater(
            len(output_files), 0,
            f"[{ENGINE_NAME}] 输出目录为空: {OUTPUT_DIR}"
        )
        self.assertEqual(
            len(output_files), len(self.input_files),
            f"[{ENGINE_NAME}] 输入 {len(self.input_files)} 个文件，"
            f"但输出 {len(output_files)} 个\n"
            f"  输入: {[os.path.basename(f) for f in self.input_files]}\n"
            f"  输出: {[os.path.basename(f) for f in output_files]}"
        )

    # ------------------------------------------------------------------
    # Test 03: 输出图片可正常打开（完整性校验）
    # ------------------------------------------------------------------
    def test_03_output_images_valid(self):
        """所有输出图片必须可被 Pillow 加载且格式完整。"""
        run_cli(INPUT_DIR, OUTPUT_DIR)
        output_files = find_output_images(OUTPUT_DIR)
        if not output_files:
            self.fail(f"[{ENGINE_NAME}] 无输出文件可校验")

        errors = []
        for fpath in output_files:
            info = validate_image(fpath)
            if not info["ok"]:
                errors.append(f"  {os.path.basename(fpath)}: {info['error']}")
        if errors:
            self.fail(
                f"[{ENGINE_NAME}] {len(errors)} 个输出文件无效:\n"
                + "\n".join(errors)
            )

    # ------------------------------------------------------------------
    # Test 04: 输出图片尺寸合理
    # ------------------------------------------------------------------
    def test_04_output_resolution_reasonable(self):
        """输出图片宽高均应大于 100px。"""
        run_cli(INPUT_DIR, OUTPUT_DIR)
        output_files = find_output_images(OUTPUT_DIR)
        for fpath in output_files:
            with self.subTest(file=os.path.basename(fpath)):
                info = validate_image(fpath)
                self.assertTrue(info["ok"], f"图片无法打开: {fpath} — {info['error']}")
                self.assertGreater(info["width"],  100,
                    f"宽度异常: {info['width']}px  ({fpath})")
                self.assertGreater(info["height"], 100,
                    f"高度异常: {info['height']}px  ({fpath})")
                print(
                    f"\n  [{ENGINE_NAME}] {os.path.basename(fpath)}: "
                    f"{info['width']}x{info['height']} {info['mode']}"
                )

    # ------------------------------------------------------------------
    # Test 05: 像素值合理性检查
    # ------------------------------------------------------------------
    def test_05_output_pixel_values_sane(self):
        """
        对每张输出图片转成 RGB，检查：
          - 不全黑（avg > 5）
          - 不全白（avg < 250）
          - 有足够颜色多样性（std > 10）
        """
        run_cli(INPUT_DIR, OUTPUT_DIR)
        output_files = find_output_images(OUTPUT_DIR)
        for fpath in output_files:
            with self.subTest(file=os.path.basename(fpath)):
                with Image.open(fpath) as img:
                    rgb = img.convert("RGB")
                    rgb.thumbnail((200, 200))
                    pixels = list(rgb.getdata())
                    all_channels = [v for px in pixels for v in px]
                    avg = sum(all_channels) / len(all_channels)
                    std = statistics.stdev(all_channels)
                print(
                    f"\n  [{ENGINE_NAME}] {os.path.basename(fpath)}: "
                    f"pixel avg={avg:.1f}  std={std:.1f}"
                )
                self.assertGreater(avg,  5,   f"图像疑似全黑 (avg={avg:.1f})")
                self.assertLess   (avg,  250,  f"图像疑似全白 (avg={avg:.1f})")
                self.assertGreater(std,  10,   f"图像颜色多样性过低 (std={std:.1f})")


# ============================================================
# 独立运行入口
# ============================================================

if __name__ == "__main__":
    print("=" * 60)
    print(f" RawTherapee 工程 rawengine 单元测试")
    print(f" 输入目录  : {INPUT_DIR}")
    print(f" 输出目录  : {OUTPUT_DIR}")
    print(f" CLI 路径  : {CLI_EXE}")
    print(f"   CLI 存在: {os.path.isfile(CLI_EXE)}")
    print("=" * 60)

    loader = unittest.TestLoader()
    suite  = loader.loadTestsFromTestCase(TestRawTherapeeEngine)
    runner = unittest.TextTestRunner(verbosity=2, stream=sys.stdout)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
