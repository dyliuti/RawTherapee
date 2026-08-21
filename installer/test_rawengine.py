#!/usr/bin/env python3
"""
test_rawengine.py — RawTherapee 工程 rawengine-cli 批量解码与耗时统计

说明：
- 不再使用 unittest；直接执行批量解码
- 每张图单独调用 CLI，避免某一张卡住影响全部
- 每 15 秒打印心跳日志，避免"看起来卡死"
- 单张超时后强制结束进程并跳过继续
- 按子目录输出 *_time.txt（包含每张耗时和子目录总耗时）
- 支持 --method 参数，通过 RAWENGINE_DEMOSAIC 环境变量传递给 CLI
- 支持 --output-dir 参数覆盖默认输出目录
"""

import os
import sys
import time
import shutil
import pathlib
import tempfile
import argparse
import subprocess
from collections import defaultdict


INPUT_DIR  = r"C:\pack\file\原始"
OUTPUT_DIR = r"C:\pack\file\output\rawtherapee"
CLI_EXE    = r"C:\pack\project\raw\RawTherapee\installer\output\bin\rawengine-cli.exe"
ENGINE_NAME = "rawtherapee"

RAW_EXTENSIONS = {
    ".arw", ".cr2", ".cr3", ".nef", ".nrw", ".raf", ".dng",
    ".rw2", ".orf", ".pef", ".srw", ".erf", ".3fr", ".mef",
    ".mrw", ".rwl", ".kdc", ".dcr", ".raw", ".rwz",
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


def expected_output_path(src_path: pathlib.Path, out_subdir: pathlib.Path) -> pathlib.Path:
    return out_subdir / f"{src_path.stem}.jpg"


def run_one_file(cli_exe: str, src_path: pathlib.Path, out_subdir: pathlib.Path,
                 timeout_sec: int, env: dict = None) -> tuple:

    """返回: (status, elapsed_seconds, output_text)"""
    start_one = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="rt_decode_") as tmp_in:
        tmp_in_path = pathlib.Path(tmp_in)
        staged_src = tmp_in_path / src_path.name
        shutil.copy2(src_path, staged_src)

        cmd = [cli_exe, str(tmp_in_path), str(out_subdir)]
        proc_env = os.environ.copy()
        if env:
            proc_env.update(env)
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                env=proc_env,
            )

            lines = []
            last_heartbeat = time.monotonic()

            while True:
                line = proc.stdout.readline()
                if line:
                    lines.append(line)

                ret = proc.poll()
                now = time.monotonic()

                if ret is not None:
                    elapsed = now - start_one
                    if ret == 0:
                        return "OK", elapsed, "".join(lines)
                    return f"RET_{ret}", elapsed, "".join(lines)

                if now - last_heartbeat >= 15:
                    print(f"[{ENGINE_NAME}] 当前文件仍在解码，已耗时 {now - start_one:.1f}s")
                    last_heartbeat = now

                if now - start_one > timeout_sec:
                    if os.name == "nt":
                        subprocess.run(
                            ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    else:
                        proc.kill()
                    elapsed = now - start_one
                    return "TIMEOUT", elapsed, "".join(lines)

                time.sleep(0.1)

        except KeyboardInterrupt:
            try:
                if "proc" in locals() and proc.poll() is None:
                    if os.name == "nt":
                        subprocess.run(
                            ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                    else:
                        proc.kill()
            finally:
                raise
        except Exception as e:
            elapsed = time.monotonic() - start_one
            return "EXCEPTION", elapsed, str(e)


def decode_all(input_dir: str, output_dir: str, timeout_sec: int = 600,
               demosaic_method: str = None) -> int:
    input_root = pathlib.Path(input_dir)
    output_root = pathlib.Path(output_dir)
    output_root.mkdir(parents=True, exist_ok=True)

    env_override = {}
    if demosaic_method:
        env_override["RAWENGINE_DEMOSAIC"] = demosaic_method.lower()

    input_files = sorted(find_input_files(input_dir))
    if not input_files:
        print(f"[{ENGINE_NAME}] 输入目录中没有可解码文件: {input_dir}")
        return 1


    per_dir_times = defaultdict(list)  # rel_parent -> [(rel_file, elapsed, status)]
    failed_files = []
    success_count = 0
    skip_count = 0
    no_output_count = 0

    total = len(input_files)
    global_start = time.monotonic()

    for idx, src_path in enumerate(input_files, start=1):
        rel_path = src_path.relative_to(input_root)
        rel_parent = rel_path.parent
        out_subdir = output_root / rel_parent
        out_subdir.mkdir(parents=True, exist_ok=True)

        print(f"[{ENGINE_NAME}] 解码进度: {idx}/{total}")
        print(f"[{ENGINE_NAME}] 当前文件: {src_path}")

        expected_out = expected_output_path(src_path, out_subdir)
        if expected_out.exists():
            status = "SKIP"
            elapsed_one = 0.0
            out_text = ""
            skip_count += 1
            print(f"[{ENGINE_NAME}] 跳过(已存在): {expected_out}")
        else:
            status, elapsed_one, out_text = run_one_file(CLI_EXE, src_path, out_subdir, timeout_sec, env_override)

            if status == "OK" and not expected_out.exists():
                status = "NO_OUTPUT"
                no_output_count += 1
                msg = f"预期输出不存在: {expected_out}"
                out_text = f"{out_text}\n{msg}" if out_text else msg

        per_dir_times[str(rel_parent)].append((str(rel_path), elapsed_one, status))

        if status == "OK":
            success_count += 1
            print(f"[{ENGINE_NAME}] 单张耗时: {elapsed_one:.3f}s")
        elif status == "SKIP":
            pass
        else:
            print(f"[{ENGINE_NAME}] 失败: {src_path} [{status}]，已跳过继续")
            failed_files.append((str(rel_path), status))
            if out_text.strip():
                print("--- CLI 输出(前 1000 字符) ---")
                print(out_text[:1000])

    global_elapsed = time.monotonic() - global_start


    for rel_parent_str, items in per_dir_times.items():
        rel_parent = pathlib.Path(rel_parent_str)
        target_dir = output_root / rel_parent
        fname = f"{safe_name_from_rel_parent(rel_parent)}_time.txt"
        fpath = target_dir / fname

        sub_total = sum(t for _, t, _ in items)
        with open(fpath, "w", encoding="utf-8") as f:
            f.write(f"engine: {ENGINE_NAME}\n")
            f.write(f"input_dir: {input_dir}\n")
            f.write(f"output_subdir: {target_dir}\n")
            f.write("\n")
            f.write("per-image decode time (seconds):\n")
            for rel_file, t, status in items:
                f.write(f"- {rel_file}: {t:.6f} [{status}]\n")
            f.write("\n")
            f.write(f"subdir_total_seconds: {sub_total:.6f}\n")

        print(f"[{ENGINE_NAME}] 已写入耗时文件: {fpath}")

    print("=" * 60)
    if failed_files:
        print(f"[{ENGINE_NAME}] 解码完成，但存在失败文件")
    else:
        print(f"[{ENGINE_NAME}] 全部解码成功")
    print(f"[{ENGINE_NAME}] 总文件数: {total}")
    print(f"[{ENGINE_NAME}] 成功数: {success_count}")
    print(f"[{ENGINE_NAME}] 跳过数: {skip_count}")
    print(f"[{ENGINE_NAME}] 失败数: {len(failed_files)}")
    print(f"[{ENGINE_NAME}] 未输出数: {no_output_count}")
    print(f"[{ENGINE_NAME}] 总耗时: {global_elapsed:.3f}s")
    if failed_files:
        print(f"[{ENGINE_NAME}] 失败文件列表（前20）:")
        for rel, reason in failed_files[:20]:
            print(f"  - {rel} ({reason})")
    print("=" * 60)

    return 0 if not failed_files else 3



def main() -> int:
    parser = argparse.ArgumentParser(description="rawtherapee rawengine-cli 批量解码")
    parser.add_argument("--input-dir",  default=INPUT_DIR,  help="输入目录")
    parser.add_argument("--output-dir", default=OUTPUT_DIR, help="输出目录")
    parser.add_argument("--method",     default=None,
                        help="demosaic 算法 (amazebilinear/rcd/dcb/lmmse/...)")
    parser.add_argument("--timeout",    default=600, type=int, help="单张超时秒数")
    args = parser.parse_args()

    out_dir = args.output_dir
    # 若指定了 method 且输出目录是默认值，则自动追加 method 后缀，避免两组结果混在一起
    if args.method and args.output_dir == OUTPUT_DIR:
        out_dir = str(pathlib.Path(OUTPUT_DIR).parent / f"rawtherapee_{args.method.lower()}")

    print("=" * 60)
    print(" rawtherapee 工程 rawengine-cli 解码脚本")
    print(f" 输入目录  : {args.input_dir}")
    print(f" 输出目录  : {out_dir}")
    print(f" CLI 路径  : {CLI_EXE}")
    print(f" CLI 存在  : {os.path.isfile(CLI_EXE)}")
    print(f" Demosaic  : {args.method or '(默认)'}")
    print("=" * 60)

    if not os.path.isfile(CLI_EXE):
        print(f"[{ENGINE_NAME}] CLI 未找到: {CLI_EXE}")
        return 1
    if not os.path.isdir(args.input_dir):
        print(f"[{ENGINE_NAME}] 输入目录不存在: {args.input_dir}")
        return 1

    return decode_all(args.input_dir, out_dir, args.timeout, args.method)



if __name__ == "__main__":
    sys.exit(main())
