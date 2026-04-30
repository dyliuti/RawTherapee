#!/usr/bin/env bash
# =============================================================================
# build_intel_gui.sh — 编译 RawTherapee 完整 GUI (x86_64) 并收集运行所需文件
#
# 用法:
#   bash build_intel_gui.sh [--jobs N] [--skip-build] [--clean] [--run]
#
# 前提:
#   需要 Intel (x86_64) Homebrew（通常在 /usr/local），且已安装：
#   arch -x86_64 brew install cmake ninja gtk+3 gtkmm3 gtkmacintegration \
#                              libraw lensfun exiv2 lcms2 fftw librsvg
#
# 产物目录:
#   installer/output_gui_intel/
#     rawtherapee          — 主程序（x86_64）
#     *.dylib              — 运行时依赖动态库
#     rtdata/              — RawTherapee 资源
# =============================================================================
set -euo pipefail

# ---------- 路径 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$RT_DIR/build_intel_gui"
OUTPUT_DIR="$SCRIPT_DIR/output_gui_intel"

# ---------- 平台配置 ----------
HOMEBREW_PREFIX="/usr/local"
CMAKE_BIN="$HOMEBREW_PREFIX/bin/cmake"
ARCH="x86_64"

# ---------- 默认参数 ----------
JOBS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
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
if [[ ! -x "$CMAKE_BIN" ]]; then
    echo "[ERROR] cmake not found at $CMAKE_BIN"
    echo "        Intel Homebrew (x86_64) is required at $HOMEBREW_PREFIX"
    echo "        Install Intel Homebrew: arch -x86_64 /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    exit 1
fi
if [[ ! -x "$HOMEBREW_PREFIX/bin/ninja" ]]; then
    echo "[ERROR] ninja not found at $HOMEBREW_PREFIX/bin/ninja"
    echo "        Install: arch -x86_64 brew install ninja"
    exit 1
fi

echo "============================================================"
echo " RawTherapee GUI macOS Intel (x86_64) build"
echo " RawTherapee  : $RT_DIR"
echo " Build dir    : $BUILD_DIR"
echo " Output dir   : $OUTPUT_DIR"
echo " Homebrew     : $HOMEBREW_PREFIX"
echo " Jobs         : $JOBS"
echo "  Note: Full GUI build is memory-intensive."
echo "        Use --jobs 1 or --jobs 2 if OOM occurs."
echo "============================================================"

# ---------- 清理 ----------
if [[ $DO_CLEAN -eq 1 ]]; then
    echo "[clean] Removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
fi

# ---------- 构建 ----------
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo ""
    echo "[cmake] Configuring for $ARCH ..."
    mkdir -p "$BUILD_DIR"

    export PKG_CONFIG_PATH="$HOMEBREW_PREFIX/lib/pkgconfig:$HOMEBREW_PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

    "$CMAKE_BIN" -S "$RT_DIR" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
        -DCMAKE_PREFIX_PATH="$HOMEBREW_PREFIX" \
        -DWITH_SYSTEM_LIBRAW=ON \
        -DOSX_DEV_BUILD=ON \
        -DCMAKE_MAKE_PROGRAM="$HOMEBREW_PREFIX/bin/ninja"

    echo ""
    echo "[cmake] Building rth (rawtherapee GUI, jobs=$JOBS) ..."
    "$CMAKE_BIN" --build "$BUILD_DIR" -j"$JOBS" --target rth
else
    echo "[skip-build] Skipping build, using existing artifacts in $BUILD_DIR"
fi

# ---------- 验证产物 ----------
RT_BIN="$BUILD_DIR/rtgui/rawtherapee"
if [[ ! -f "$RT_BIN" ]]; then
    echo "[ERROR] $RT_BIN not found. Build may have failed."
    exit 1
fi

# ---------- 准备输出目录 ----------
echo ""
echo "[collect] Preparing output directory ..."
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# ---------- 主程序 ----------
echo "[collect] rawtherapee ..."
cp "$RT_BIN" "$OUTPUT_DIR/"

# ---------- 运行时 dylib 依赖 ----------
echo "[collect] Runtime dylibs ..."
DYLIB_SEEN="$SCRIPT_DIR/.intel_gui_dylibs_seen_$$"
: > "$DYLIB_SEEN"

collect_dylibs_recursive() {
    local f="$1"
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        [[ "$dep" != "$HOMEBREW_PREFIX"* ]] && continue
        [[ ! -f "$dep" ]] && continue
        local dep_name
        dep_name="$(basename "$dep")"
        grep -qxF "$dep_name" "$DYLIB_SEEN" 2>/dev/null && continue
        echo "$dep_name" >> "$DYLIB_SEEN"
        cp -f "$dep" "$OUTPUT_DIR/"
        collect_dylibs_recursive "$dep"
    done || true
}

collect_dylibs_recursive "$OUTPUT_DIR/rawtherapee"
rm -f "$DYLIB_SEEN"

DYLIB_COUNT=$(find "$OUTPUT_DIR" -maxdepth 1 -name "*.dylib" | wc -l | tr -d ' ')
echo "  Copied ${DYLIB_COUNT} dylib(s)"

# ---------- 修正 dylib install names ----------
echo "[collect] Fixing install names ..."
for dylib in "$OUTPUT_DIR"/*.dylib; do
    [[ -f "$dylib" ]] || continue
    dylib_name="$(basename "$dylib")"
    install_name_tool -id "@rpath/$dylib_name" "$dylib" 2>/dev/null || true
    otool -L "$dylib" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        dep_name="$(basename "$dep")"
        [[ -f "$OUTPUT_DIR/$dep_name" ]] || continue
        [[ "$dep" == "@rpath/$dep_name" ]] && continue
        install_name_tool -change "$dep" "@rpath/$dep_name" "$dylib" 2>/dev/null || true
    done || true
done

otool -L "$OUTPUT_DIR/rawtherapee" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    dep_name="$(basename "$dep")"
    [[ -f "$OUTPUT_DIR/$dep_name" ]] || continue
    [[ "$dep" == "@rpath/$dep_name" ]] && continue
    install_name_tool -change "$dep" "@rpath/$dep_name" "$OUTPUT_DIR/rawtherapee" 2>/dev/null || true
done || true
install_name_tool -add_rpath "@executable_path" "$OUTPUT_DIR/rawtherapee" 2>/dev/null || true

# ---------- RawTherapee 自身资源 ----------
echo "[collect] RawTherapee rtdata ..."
mkdir -p "$OUTPUT_DIR/rtdata"
if [[ -d "$RT_DIR/rtdata" ]]; then
    cp -r "$RT_DIR/rtdata/." "$OUTPUT_DIR/rtdata/"
fi

BUILT_CAMCONST="$BUILD_DIR/rtengine/camconst.json"
if [[ -f "$BUILT_CAMCONST" ]]; then
    cp "$BUILT_CAMCONST" "$OUTPUT_DIR/rtdata/"
fi

# ---------- 汇总 ----------
TOTAL_SIZE=$(du -sh "$OUTPUT_DIR" 2>/dev/null | awk '{print $1}')
echo ""
echo "============================================================"
echo " Output collected successfully (x86_64 GUI):"
echo "  rawtherapee : $(ls -lh "$OUTPUT_DIR/rawtherapee" | awk '{print $5}')"
echo "  dylibs      : $DYLIB_COUNT"
echo "  Total size  : $TOTAL_SIZE"
echo "============================================================"
echo " Output directory: $OUTPUT_DIR"
echo "============================================================"

# ---------- 可选：直接运行 ----------
if [[ $DO_RUN -eq 1 ]]; then
    echo ""
    echo "[run] Launching rawtherapee ..."
    DYLD_LIBRARY_PATH="$OUTPUT_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$OUTPUT_DIR/rawtherapee" &
fi
