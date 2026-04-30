#!/usr/bin/env bash
# =============================================================================
# build_gui.sh — 编译 RawTherapee 完整 GUI 并收集运行所需文件到 output_gui/
#
# 用法（在 MSYS2 ucrt64 shell 中运行）:
#   bash build_gui.sh [--jobs N] [--skip-build] [--clean] [--run]
#
# 产物目录结构:
#   installer/output_gui/
#     rawtherapee.exe       — 主程序
#     *.dll                 — 运行时依赖 DLL
#     share/                — GTK 主题/图标/语言等数据
#     rtdata/               — RawTherapee 自身资源（配置/ICC/相机参数等）
# =============================================================================
set -euo pipefail

# ---------- 路径 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$RT_DIR/build"
OUTPUT_DIR="$SCRIPT_DIR/output_gui"

# ---------- 默认参数 ----------
JOBS=2
SKIP_BUILD=0
DO_CLEAN=0
DO_RUN=0

# ---------- 解析参数 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)       JOBS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --clean)      DO_CLEAN=1; shift ;;
        --run)        DO_RUN=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------- 验证工具链 ----------
if ! command -v cmake &>/dev/null; then
    echo "[ERROR] cmake not found. Please run in MSYS2 ucrt64 shell."
    exit 1
fi
if ! command -v ninja &>/dev/null; then
    echo "[ERROR] ninja not found. Install: pacman -S mingw-w64-ucrt-x86_64-ninja"
    exit 1
fi

echo "============================================================"
echo " RawTherapee GUI build"
echo " RawTherapee : $RT_DIR"
echo " Build dir   : $BUILD_DIR"
echo " Output dir  : $OUTPUT_DIR"
echo " Jobs        : $JOBS"
echo "============================================================"

# ---------- 清理 ----------
if [[ $DO_CLEAN -eq 1 ]]; then
    echo "[clean] Removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
fi

# ---------- 构建 ----------
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo ""
    echo "[cmake] Configuring ..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$RT_DIR" -B "$BUILD_DIR" \
        -G Ninja \
        -DWITH_SYSTEM_LIBRAW=ON \
        -DCMAKE_BUILD_TYPE=Release

    echo ""
    echo "[ninja] Building rawtherapee GUI (jobs=$JOBS) ..."
    echo "  Note: full GUI build is memory-intensive; use --jobs 1 if OOM occurs"
    ninja -C "$BUILD_DIR" -j"$JOBS" rth
else
    echo "[skip-build] Skipping build, using existing artifacts in $BUILD_DIR"
fi

# ---------- 验证产物 ----------
RT_EXE="$BUILD_DIR/rtgui/rawtherapee.exe"
if [[ ! -f "$RT_EXE" ]]; then
    echo "[ERROR] $RT_EXE not found. Build may have failed."
    exit 1
fi

# ---------- 准备输出目录 ----------
echo ""
echo "[collect] Preparing output directory ..."
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# ---------- 主程序 ----------
echo "[collect] rawtherapee.exe ..."
cp "$RT_EXE" "$OUTPUT_DIR/"

# ---------- 运行时 DLL ----------
echo "[collect] Runtime DLLs ..."
UCRT64_BIN="/ucrt64/bin"
DLL_SEEN="$SCRIPT_DIR/.gui_dlls_seen_$$"
: > "$DLL_SEEN"

collect_dlls_recursive() {
    local f="$1"
    ldd "$f" 2>/dev/null | grep -i "$UCRT64_BIN" | awk '{print $3}' | while IFS= read -r dll_path; do
        [[ -z "$dll_path" || ! -f "$dll_path" ]] && continue
        local dll_name
        dll_name="$(basename "$dll_path")"
        grep -qxF "$dll_name" "$DLL_SEEN" 2>/dev/null && continue
        echo "$dll_name" >> "$DLL_SEEN"
        cp -f "$dll_path" "$OUTPUT_DIR/"
        collect_dlls_recursive "$dll_path"
    done || true
}

collect_dlls_recursive "$OUTPUT_DIR/rawtherapee.exe"
rm -f "$DLL_SEEN"

DLL_COUNT=$(find "$OUTPUT_DIR" -maxdepth 1 -name "*.dll" | wc -l)
echo "  Copied $DLL_COUNT DLL(s)"

# ---------- GTK 运行时数据（主题/图标/GLib schema） ----------
echo "[collect] GTK runtime data ..."
mkdir -p "$OUTPUT_DIR/share"

# GLib schema（GTK 必需）
if [[ -d "/ucrt64/share/glib-2.0" ]]; then
    cp -r "/ucrt64/share/glib-2.0" "$OUTPUT_DIR/share/"
fi

# 图标（hicolor 最小集合）
if [[ -d "/ucrt64/share/icons" ]]; then
    cp -r "/ucrt64/share/icons" "$OUTPUT_DIR/share/"
fi

# GTK 默认主题
if [[ -d "/ucrt64/share/themes" ]]; then
    cp -r "/ucrt64/share/themes" "$OUTPUT_DIR/share/"
fi

# GTK 本地化（可选，仅复制英文以减小体积）
mkdir -p "$OUTPUT_DIR/share/locale"
for lang in en en_US; do
    if [[ -d "/ucrt64/share/locale/$lang" ]]; then
        cp -r "/ucrt64/share/locale/$lang" "$OUTPUT_DIR/share/locale/"
    fi
done

# ---------- RawTherapee 自身资源 ----------
# 注意: Windows BUILD_BUNDLE 时 DATADIR=".", 即数据文件须与 exe 同目录
# 因此将 rtdata/ 内容直接复制到 OUTPUT_DIR 根目录, 而非放入子目录
echo "[collect] RawTherapee rtdata (flat into exe dir) ..."
if [[ -d "$RT_DIR/rtdata" ]]; then
    cp -r "$RT_DIR/rtdata/." "$OUTPUT_DIR/"
fi
# 编译生成的资源（camconst.json 等）
BUILT_CAMCONST="$BUILD_DIR/rtengine/camconst.json"
if [[ -f "$BUILT_CAMCONST" ]]; then
    cp "$BUILT_CAMCONST" "$OUTPUT_DIR/"
fi

# ---------- 汇总 ----------
TOTAL_SIZE=$(du -sh "$OUTPUT_DIR" 2>/dev/null | awk '{print $1}')
echo ""
echo "============================================================"
echo " Output collected successfully:"
echo "  rawtherapee.exe : $(ls -lh "$OUTPUT_DIR/rawtherapee.exe" | awk '{print $5}')"
echo "  DLLs            : $DLL_COUNT"
echo "  Total size      : $TOTAL_SIZE"
echo "============================================================"
echo " Output directory: $OUTPUT_DIR"
echo "============================================================"

# ---------- 可选：直接运行 ----------
if [[ $DO_RUN -eq 1 ]]; then
    echo ""
    echo "[run] Launching rawtherapee.exe ..."
    cd "$OUTPUT_DIR"
    ./rawtherapee.exe &
fi
