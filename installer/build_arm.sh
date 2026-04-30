#!/usr/bin/env bash
# =============================================================================
# build_arm.sh — 构建 rawengine macOS ARM (Apple Silicon / arm64) 并收集产物
#
# 用法:
#   bash build_arm.sh [--jobs N] [--skip-build] [--clean]
#
# 产物目录结构:
#   installer/output/
#     include/          — C API 头文件（arm/intel 共享）
#     dylib/arm/        — rawengine.dylib (arm64) + 运行时依赖 dylib
#     dsym/arm/         — rawengine.dylib.dSYM 调试符号包
#     resource/         — 运行时色彩/相机资源（arm/intel 共享）
#     readme_mac.md     — macOS 集成说明文档
# =============================================================================
set -euo pipefail

# ---------- 路径 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$RT_DIR/build_arm"
OUTPUT_DIR="$SCRIPT_DIR/output"
DYLIB_DIR="$OUTPUT_DIR/dylib/arm"
DSYM_DIR="$OUTPUT_DIR/dsym/arm"

# ---------- 平台配置 ----------
HOMEBREW_PREFIX="/opt/homebrew"
CMAKE_BIN="$HOMEBREW_PREFIX/bin/cmake"
ARCH="arm64"

# ---------- 默认参数 ----------
JOBS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
SKIP_BUILD=0
DO_CLEAN=0
# WITH_SYSTEM_LIBRAW: ON=使用 brew 安装的 libraw，OFF=使用源码内嵌 libraw
WITH_SYSTEM_LIBRAW=OFF

# ---------- 解析参数 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)       JOBS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --clean)      DO_CLEAN=1; shift ;;
        --with-system-libraw) WITH_SYSTEM_LIBRAW=ON; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------- 验证工具链 ----------
if [[ ! -x "$CMAKE_BIN" ]]; then
    echo "[ERROR] cmake not found at $CMAKE_BIN"
    echo "        Install: brew install cmake"
    exit 1
fi
if [[ ! -x "$HOMEBREW_PREFIX/bin/ninja" ]]; then
    echo "[ERROR] ninja not found at $HOMEBREW_PREFIX/bin/ninja"
    echo "        Install: brew install ninja"
    exit 1
fi

# ---------- 检查必要依赖 ----------
export PKG_CONFIG_PATH="$HOMEBREW_PREFIX/lib/pkgconfig:$HOMEBREW_PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---------- 修复 Homebrew libffi.pc SDK 路径问题（macOS beta 版本） ----------
# 当运行 macOS beta 时（如 macOS 26），Homebrew 的 libffi.pc 引用 MacOSXNN.sdk，
# 但 Command Line Tools 可能尚未提供该 SDK。创建本地 pkgconfig 覆盖以指向实际 SDK。
_LIBFFI_FIX_DIR="/tmp/rt_pkgconfig_fix_arm"
_libffi_fix_applied=0
if pkg-config --exists libffi 2>/dev/null; then
    _ffi_includedir=$(pkg-config --variable=includedir libffi 2>/dev/null || true)
    if [[ -n "$_ffi_includedir" && ! -d "$_ffi_includedir" ]]; then
        _actual_sdk=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)
        if [[ -n "$_actual_sdk" && -d "$_actual_sdk/usr/include/ffi" ]]; then
            echo "[warn] libffi includedir '$_ffi_includedir' does not exist (macOS beta SDK mismatch)"
            echo "       Applying workaround: using SDK at $_actual_sdk"
            mkdir -p "$_LIBFFI_FIX_DIR"
            cat > "$_LIBFFI_FIX_DIR/libffi.pc" << LIBFFI_PC_EOF
prefix=${_actual_sdk}/usr
exec_prefix=/usr
libdir=\${exec_prefix}/lib
toolexeclibdir=\${libdir}
includedir=\${prefix}/include/ffi

Name: libffi
Description: Library supporting Foreign Function Interfaces
Version: 3.4-rc1
Libs: -L\${toolexeclibdir} -lffi
Cflags: -I\${includedir}
LIBFFI_PC_EOF
            export PKG_CONFIG_PATH="$_LIBFFI_FIX_DIR:$PKG_CONFIG_PATH"
            _libffi_fix_applied=1
        fi
    fi
fi

MISSING_DEPS=()
for pkg in gtk+-3.0 gtkmm-3.0 exiv2 lcms2 fftw3f lensfun librsvg-2.0; do
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        MISSING_DEPS+=("$pkg")
    fi
done
if [[ $WITH_SYSTEM_LIBRAW == ON ]]; then
    if ! pkg-config --exists libraw 2>/dev/null; then
        MISSING_DEPS+=("libraw")
    fi
fi

if [[ ${#MISSING_DEPS[@]} -gt 0 ]]; then
    echo "[ERROR] Missing pkg-config packages: ${MISSING_DEPS[*]}"
    echo ""
    echo "  Install all required dependencies:"
    echo "    brew install gtk+3 gtkmm3 exiv2 little-cms2 fftw lensfun librsvg"
    echo "    brew install cairomm libsigc++ glibmm libiptcdata"
    if [[ $WITH_SYSTEM_LIBRAW == ON ]]; then
        echo "    brew install libraw"
    fi
    echo ""
    echo "  Note: fmt and libraw are bundled in source; no separate install needed"
    echo "        unless you pass --with-system-libraw."
    exit 1
fi

echo "============================================================"
echo " rawengine macOS ARM (arm64) build & collect"
echo " RawTherapee  : $RT_DIR"
echo " Build dir    : $BUILD_DIR"
echo " Output dir   : $OUTPUT_DIR"
echo " Homebrew     : $HOMEBREW_PREFIX"
echo " Jobs         : $JOBS"
echo "============================================================"

# ---------- 清理 ----------
if [[ $DO_CLEAN -eq 1 ]]; then
    echo "[clean] Removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    echo "[clean] Removing $DYLIB_DIR ..."
    rm -rf "$DYLIB_DIR"
    echo "[clean] Removing $DSYM_DIR ..."
    rm -rf "$DSYM_DIR"
fi

# ---------- 构建 ----------
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo ""
    echo "[cmake] Configuring for $ARCH (WITH_SYSTEM_LIBRAW=$WITH_SYSTEM_LIBRAW) ..."
    mkdir -p "$BUILD_DIR"

    "$CMAKE_BIN" -S "$RT_DIR" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
        -DCMAKE_PREFIX_PATH="$HOMEBREW_PREFIX" \
        -DLOCAL_PREFIX="$HOMEBREW_PREFIX" \
        -DWITH_SYSTEM_LIBRAW="$WITH_SYSTEM_LIBRAW" \
        -DOSX_DEV_BUILD=OFF \
        -DCMAKE_MAKE_PROGRAM="$HOMEBREW_PREFIX/bin/ninja"

    echo ""
    echo "[cmake] Building rawengine + rawengine-cli (jobs=$JOBS) ..."
    "$CMAKE_BIN" --build "$BUILD_DIR" -j"$JOBS" --target rawengine rawengine-cli
else
    echo "[skip-build] Skipping build, using existing artifacts in $BUILD_DIR"
fi

# ---------- 验证产物 ----------
RAWENGINE_DYLIB="$BUILD_DIR/rtengine/rawengine.dylib"
CLI_BIN="$BUILD_DIR/rtengine/rawengine-cli"

if [[ ! -f "$RAWENGINE_DYLIB" ]]; then
    echo "[ERROR] $RAWENGINE_DYLIB not found. Build may have failed."
    exit 1
fi

# ---------- 准备输出目录 ----------
echo ""
echo "[collect] Preparing output directories ..."
rm -rf "$DYLIB_DIR" "$DSYM_DIR"
mkdir -p "$DYLIB_DIR" "$DSYM_DIR"
mkdir -p "$OUTPUT_DIR/include"
mkdir -p "$OUTPUT_DIR/resource"

# ---------- 头文件 ----------
echo "[collect] Headers ..."
cp "$RT_DIR/rtengine/raw_engine.h"           "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/rawengine_types.h"      "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/raw_engine_error_def.h" "$OUTPUT_DIR/include/"

# ---------- 主 dylib ----------
echo "[collect] rawengine.dylib ..."
cp -f "$RAWENGINE_DYLIB" "$DYLIB_DIR/"

# ---------- CLI 工具 ----------
if [[ -f "$CLI_BIN" ]]; then
    echo "[collect] rawengine-cli ..."
    cp -f "$CLI_BIN" "$DYLIB_DIR/"
fi

# ---------- 递归收集运行时 dylib 依赖（来自 Homebrew） ----------
echo "[collect] Runtime dylibs ..."
DYLIB_SEEN="$SCRIPT_DIR/.arm_dylibs_seen_$$"
: > "$DYLIB_SEEN"

collect_dylibs_recursive() {
    local f="$1"
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        # 仅收集来自 Homebrew prefix 的依赖
        [[ "$dep" != "$HOMEBREW_PREFIX"* ]] && continue
        [[ ! -f "$dep" ]] && continue
        local dep_name
        dep_name="$(basename "$dep")"
        grep -qxF "$dep_name" "$DYLIB_SEEN" 2>/dev/null && continue
        echo "$dep_name" >> "$DYLIB_SEEN"
        cp -f "$dep" "$DYLIB_DIR/"
        collect_dylibs_recursive "$dep"
    done || true
}

collect_dylibs_recursive "$DYLIB_DIR/rawengine.dylib"
if [[ -f "$DYLIB_DIR/rawengine-cli" ]]; then
    collect_dylibs_recursive "$DYLIB_DIR/rawengine-cli"
fi
rm -f "$DYLIB_SEEN"

DYLIB_COUNT=$(find "$DYLIB_DIR" -name "*.dylib" | wc -l | tr -d ' ')
echo "  Collected ${DYLIB_COUNT} dylib(s) in dylib/arm/"

# ---------- 修正 install name（统一使用 @rpath） ----------
echo "[collect] Fixing install names ..."

fix_install_names() {
    local lib="$1"
    local lib_name
    lib_name="$(basename "$lib")"
    # 将 dylib 自身 ID 改为 @rpath/xxx.dylib
    install_name_tool -id "@rpath/$lib_name" "$lib" 2>/dev/null || true
    # 将对其他 dylib 的引用从绝对路径改为 @rpath/xxx.dylib
    otool -L "$lib" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        local dep_name
        dep_name="$(basename "$dep")"
        [[ -f "$DYLIB_DIR/$dep_name" ]] || continue
        [[ "$dep" == "@rpath/$dep_name" ]] && continue
        install_name_tool -change "$dep" "@rpath/$dep_name" "$lib" 2>/dev/null || true
    done || true
}

for dylib in "$DYLIB_DIR"/*.dylib; do
    [[ -f "$dylib" ]] || continue
    fix_install_names "$dylib"
done

# CLI 可执行文件：更新库引用并添加 @executable_path rpath
if [[ -f "$DYLIB_DIR/rawengine-cli" ]]; then
    otool -L "$DYLIB_DIR/rawengine-cli" 2>/dev/null | tail -n +2 | awk '{print $1}' | \
    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue
        dep_name="$(basename "$dep")"
        [[ -f "$DYLIB_DIR/$dep_name" ]] || continue
        [[ "$dep" == "@rpath/$dep_name" ]] && continue
        install_name_tool -change "$dep" "@rpath/$dep_name" "$DYLIB_DIR/rawengine-cli" 2>/dev/null || true
    done || true
    # 确保 CLI 能在同目录找到 dylib
    install_name_tool -add_rpath "@executable_path" "$DYLIB_DIR/rawengine-cli" 2>/dev/null || true
fi

# ---------- 提取 dSYM 调试符号 ----------
echo "[collect] Extracting dSYM symbols ..."
if command -v dsymutil &>/dev/null; then
    dsymutil "$DYLIB_DIR/rawengine.dylib" \
        -o "$DSYM_DIR/rawengine.dylib.dSYM" 2>/dev/null || true
    strip -S "$DYLIB_DIR/rawengine.dylib" 2>/dev/null || true
    echo "  rawengine.dylib → dsym/arm/rawengine.dylib.dSYM"

    if [[ -f "$DYLIB_DIR/rawengine-cli" ]]; then
        dsymutil "$DYLIB_DIR/rawengine-cli" \
            -o "$DSYM_DIR/rawengine-cli.dSYM" 2>/dev/null || true
        strip "$DYLIB_DIR/rawengine-cli" 2>/dev/null || true
        echo "  rawengine-cli   → dsym/arm/rawengine-cli.dSYM"
    fi
else
    echo "  [warn] dsymutil not found; dSYM not generated (install Xcode Command Line Tools)"
fi

# ---------- 运行时资源 ----------
echo "[collect] Runtime resources ..."
RTDATA_DIR="$RT_DIR/rtdata"

for dir in dcpprofiles iccprofiles profiles; do
    [[ -d "$RTDATA_DIR/$dir" ]] && cp -r "$RTDATA_DIR/$dir" "$OUTPUT_DIR/resource/"
done

for f in cammatrices.json dcraw.json rt.json; do
    [[ -f "$RTDATA_DIR/$f" ]] && cp "$RTDATA_DIR/$f" "$OUTPUT_DIR/resource/"
done

BUILT_CAMCONST="$BUILD_DIR/rtengine/camconst.json"
if [[ -f "$BUILT_CAMCONST" ]]; then
    cp "$BUILT_CAMCONST" "$OUTPUT_DIR/resource/"
elif [[ -f "$RTDATA_DIR/camconst.json" ]]; then
    cp "$RTDATA_DIR/camconst.json" "$OUTPUT_DIR/resource/"
fi

mkdir -p "$OUTPUT_DIR/resource/languages"
[[ -f "$RTDATA_DIR/languages/default" ]] && \
    cp "$RTDATA_DIR/languages/default" "$OUTPUT_DIR/resource/languages/"

RESOURCE_COUNT=$(find "$OUTPUT_DIR/resource" -type f | wc -l | tr -d ' ')
echo "  Copied ${RESOURCE_COUNT} resource file(s) to resource/"

# ---------- 汇总 ----------
DSYM_COUNT=$(find "$DSYM_DIR" -maxdepth 1 -name "*.dSYM" -type d | wc -l | tr -d ' ')
echo ""
echo "============================================================"
echo " Output collected successfully (arm64):"
echo "  include/       : $(find "$OUTPUT_DIR/include" -name "*.h" | wc -l | tr -d ' ') header(s)"
echo "  dylib/arm/     : ${DYLIB_COUNT} dylib(s)"
echo "  dsym/arm/      : ${DSYM_COUNT} dSYM bundle(s)"
echo "  resource/      : ${RESOURCE_COUNT} file(s)"
echo "============================================================"
echo " Output directory: $OUTPUT_DIR"
echo "============================================================"
