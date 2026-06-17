#!/usr/bin/env bash
# =============================================================================
# build.sh — 构建 rawengine 并收集集成所需产物
#
# 用法（在 MSYS2 ucrt64 shell 中运行）:
#   bash build.sh [--jobs N] [--skip-build] [--clean]
#
# 产物目录结构:
#   installer/output/
#     include/   — C API 头文件
#     lib/       — 静态库 librtengine.a
#     bin/       — rawengine-cli.exe + 全部运行时 DLL
#     pdb/       — DWARF 调试符号文件（.debug）
#     readme.md  — 集成说明文档
# =============================================================================
set -euo pipefail

# ---------- 路径 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT_DIR="$(dirname "$SCRIPT_DIR")"        # RawTherapee 根目录
BUILD_DIR="$RT_DIR/build"
OUTPUT_DIR="$SCRIPT_DIR/output"

# ---------- 默认参数 ----------
JOBS=2
SKIP_BUILD=0
DO_CLEAN=0

# ---------- 解析参数 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs)   JOBS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --clean)  DO_CLEAN=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------- 验证工具链 ----------
if [[ "$(uname -s)" == "Darwin" && -x "/opt/homebrew/bin/cmake" && -x "/opt/homebrew/bin/ninja" ]]; then
    export PATH="/opt/homebrew/bin:$PATH"
    export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
if ! command -v cmake &>/dev/null; then
    echo "[ERROR] cmake not found. Please run in MSYS2 ucrt64 shell."
    exit 1
fi
if ! command -v ninja &>/dev/null; then
    echo "[ERROR] ninja not found. Install: pacman -S mingw-w64-ucrt-x86_64-ninja"
    exit 1
fi
if ! command -v pkg-config &>/dev/null; then
    echo "[ERROR] pkg-config not found."
    exit 1
fi

if [[ "$(uname -s)" == "Darwin" ]] && pkg-config --exists libffi 2>/dev/null; then
    LIBFFI_FIX_DIR="/tmp/rt_pkgconfig_fix_build"
    FFI_INCLUDEDIR="$(pkg-config --variable=includedir libffi 2>/dev/null || true)"
    if [[ -n "$FFI_INCLUDEDIR" && ! -d "$FFI_INCLUDEDIR" ]]; then
        ACTUAL_SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
        if [[ -n "$ACTUAL_SDK" && -d "$ACTUAL_SDK/usr/include/ffi" ]]; then
            echo "[warn] libffi includedir '$FFI_INCLUDEDIR' does not exist; using SDK at $ACTUAL_SDK"
            mkdir -p "$LIBFFI_FIX_DIR"
            cat > "$LIBFFI_FIX_DIR/libffi.pc" << LIBFFI_PC_EOF
prefix=${ACTUAL_SDK}/usr
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
            export PKG_CONFIG_PATH="$LIBFFI_FIX_DIR${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        fi
    fi
fi

echo "============================================================"
echo " rawengine build & collect"
echo " RawTherapee : $RT_DIR"
echo " Build dir   : $BUILD_DIR"
echo " Output dir  : $OUTPUT_DIR"
echo " Jobs        : $JOBS"
echo "============================================================"

HOST_OS="$(uname -s)"
if [[ "$HOST_OS" == "Darwin" ]]; then
    CLI_NAME="rawengine-cli"
    RAWENGINE_LIB_NAME="therapee.dylib"
    RUNTIME_LIB_GLOB="*.dylib"
else
    CLI_NAME="rawengine-cli.exe"
    RAWENGINE_LIB_NAME="therapee.dll"
    RUNTIME_LIB_GLOB="*.dll"
fi

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
    CMAKE_ARGS=(
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
    )
    if pkg-config --exists 'libraw_r >= 0.21'; then
        CMAKE_ARGS+=(-DWITH_SYSTEM_LIBRAW=ON)
    else
        CMAKE_ARGS+=(-DWITH_SYSTEM_LIBRAW=OFF)
    fi
    if [[ "$(uname -s)" == "Darwin" ]]; then
        HOMEBREW_PREFIX="${HOMEBREW_PREFIX:-/opt/homebrew}"
        CMAKE_ARGS+=(
            -DCMAKE_OSX_DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET:-12.0}"
            -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
            -DCMAKE_PREFIX_PATH="$HOMEBREW_PREFIX"
            -DLOCAL_PREFIX="$HOMEBREW_PREFIX"
            -DJPEG_INCLUDE_DIR="$HOMEBREW_PREFIX/include"
            -DJPEG_LIBRARY="$HOMEBREW_PREFIX/lib/libjpeg.dylib"
            -DPNG_PNG_INCLUDE_DIR="$HOMEBREW_PREFIX/include"
            -DPNG_LIBRARY="$HOMEBREW_PREFIX/lib/libpng.dylib"
            -DTIFF_INCLUDE_DIR="$HOMEBREW_PREFIX/include"
            -DTIFF_LIBRARY="$HOMEBREW_PREFIX/lib/libtiff.dylib"
        )
    fi
    cmake -S "$RT_DIR" -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"

    echo ""
    echo "[ninja] Building rawengine-cli and therapee.dll (jobs=$JOBS) ..."
    ninja -C "$BUILD_DIR" -j"$JOBS" rawengine-cli rawengine
else
    echo "[skip-build] Skipping build, using existing artifacts in $BUILD_DIR"
fi

# ---------- 验证产物存在 ----------
CLI_EXE="$BUILD_DIR/rtengine/$CLI_NAME"
LIB_A="$BUILD_DIR/rtengine/librtengine.a"
RAWENGINE_DLL="$BUILD_DIR/rtengine/$RAWENGINE_LIB_NAME"

if [[ ! -f "$CLI_EXE" ]]; then
    echo "[ERROR] $CLI_EXE not found. Build may have failed."
    exit 1
fi
if [[ ! -f "$LIB_A" ]]; then
    echo "[ERROR] $LIB_A not found. Build may have failed."
    exit 1
fi
if [[ ! -f "$RAWENGINE_DLL" ]]; then
    echo "[ERROR] $RAWENGINE_DLL not found. Build may have failed."
    exit 1
fi

# ---------- 准备输出目录 ----------
echo ""
echo "[collect] Preparing output directories ..."
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/include"
mkdir -p "$OUTPUT_DIR/lib"
mkdir -p "$OUTPUT_DIR/bin"
mkdir -p "$OUTPUT_DIR/pdb"
mkdir -p "$OUTPUT_DIR/resource"

# ---------- 头文件 ----------
echo "[collect] Headers ..."
cp "$RT_DIR/rtengine/raw_engine.h"          "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/rawengine_types.h"     "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/raw_engine_error_def.h" "$OUTPUT_DIR/include/"

# ---------- 静态库 ----------
echo "[collect] Static library ..."
cp "$LIB_A" "$OUTPUT_DIR/lib/"

# ---------- 动态库（DLL/dylib + MinGW 导入库） ----------
echo "[collect] $RAWENGINE_LIB_NAME ..."
cp "$RAWENGINE_DLL" "$OUTPUT_DIR/bin/"
# MinGW 同时生成 libtherapee.dll.a（MinGW/GCC 工程可直接链接）
RAWENGINE_DLL_A="$BUILD_DIR/rtengine/libtherapee.dll.a"
if [[ -f "$RAWENGINE_DLL_A" ]]; then
    cp "$RAWENGINE_DLL_A" "$OUTPUT_DIR/lib/"
fi

# ---------- 为 MSVC 生成导入库（.def + .lib） ----------
# gendef 从 DLL 提取导出表 → MSVC lib.exe 生成 therapee.lib
if [[ "$HOST_OS" != "Darwin" ]]; then
echo "[collect] Generating MSVC import library (gendef + lib.exe) ..."

# lib.exe 路径由 build.bat 通过 RAWENGINE_MSVC_LIB 传入（Windows 路径，可能含空格）
LIB_EXE_WIN="${RAWENGINE_MSVC_LIB:-}"

if ! command -v gendef >/dev/null 2>&1; then
    echo "  [warn] gendef not found in ucrt64; therapee.lib not generated"
    echo "         Install via: pacman -S mingw-w64-ucrt-x86_64-tools"
elif [[ -z "$LIB_EXE_WIN" ]]; then
    echo "  [warn] RAWENGINE_MSVC_LIB not set; therapee.lib not generated"
    echo "         (build.bat should locate MSVC lib.exe via vswhere and pass it in)"
else
    (
        cd "$OUTPUT_DIR/lib"
        cp -f "$OUTPUT_DIR/bin/therapee.dll" ./therapee.dll
        gendef ./therapee.dll
        # 直接用 Windows 路径调用 lib.exe（cygpath 转 MSYS 路径再走 exec，处理空格）
        LIB_EXE_MSYS="$(cygpath -u "$LIB_EXE_WIN" 2>/dev/null || echo "$LIB_EXE_WIN")"
        "$LIB_EXE_MSYS" //def:therapee.def //out:therapee.lib //machine:x64
        rm -f ./therapee.dll ./therapee.exp
    )
    if [[ -f "$OUTPUT_DIR/lib/therapee.lib" ]]; then
        echo "  therapee.def + therapee.lib generated in $OUTPUT_DIR/lib/"
    else
        echo "  [warn] failed to generate therapee.lib (lib.exe call failed)"
    fi
fi
fi

# ---------- CLI 可执行文件 ----------
echo "[collect] $CLI_NAME ..."
cp "$CLI_EXE" "$OUTPUT_DIR/bin/"

# ---------- 运行时 DLL（从 ucrt64/bin 复制依赖） ----------
echo "[collect] Runtime DLLs ..."

# 用 ldd 找出所有来自 ucrt64/bin 的依赖并递归收集
UCRT64_BIN="/ucrt64/bin"
DLL_SEEN="$SCRIPT_DIR/.dlls_seen_$$"
: > "$DLL_SEEN"

collect_dlls_recursive() {
    local f="$1"
    ldd "$f" 2>/dev/null | grep -i "$UCRT64_BIN" | awk '{print $3}' | while IFS= read -r dll_path; do
        [[ -z "$dll_path" || ! -f "$dll_path" ]] && continue
        local dll_name
        dll_name="$(basename "$dll_path")"
        # 跳过已处理
        grep -qxF "$dll_name" "$DLL_SEEN" 2>/dev/null && continue
        echo "$dll_name" >> "$DLL_SEEN"
        cp -f "$dll_path" "$OUTPUT_DIR/bin/"
        # 递归
        collect_dlls_recursive "$dll_path"
    done || true
}

if [[ "$HOST_OS" == "Darwin" ]]; then
    DLL_COUNT=$(find "$OUTPUT_DIR/bin" -name "*.dylib" | wc -l)
    echo "  Copied $DLL_COUNT dylib(s) to output/bin/"
else
    collect_dlls_recursive "$OUTPUT_DIR/bin/$CLI_NAME"
    collect_dlls_recursive "$OUTPUT_DIR/bin/$RAWENGINE_LIB_NAME"
    rm -f "$DLL_SEEN"

    DLL_COUNT=$(find "$OUTPUT_DIR/bin" -name "*.dll" | wc -l)
    echo "  Copied $DLL_COUNT DLL(s) to output/bin/"
fi

# ---------- 调试符号（DWARF → .debug 文件） ----------
echo "[collect] Extracting debug symbols ..."

extract_debug() {
    local src="$1"
    local base
    base="$(basename "$src")"
    local dbg="$OUTPUT_DIR/pdb/${base}.debug"

    if command -v objcopy &>/dev/null; then
        # 提取调试符号
        objcopy --only-keep-debug "$src" "$dbg" 2>/dev/null || true
        # 从可执行文件中剥离调试信息（保留链接信息）
        objcopy --strip-debug --add-gnu-debuglink="$dbg" "$src" 2>/dev/null || true
        echo "  $base → pdb/${base}.debug"
    fi
}

extract_debug "$OUTPUT_DIR/bin/$CLI_NAME"
extract_debug "$OUTPUT_DIR/bin/$RAWENGINE_LIB_NAME"

# 同时为静态库提取符号索引信息（可选）
if command -v nm &>/dev/null; then
    nm --extern-only --defined-only "$OUTPUT_DIR/lib/librtengine.a" \
        > "$OUTPUT_DIR/pdb/librtengine_symbols.txt" 2>/dev/null || true
    echo "  librtengine.a → pdb/librtengine_symbols.txt"
fi

# ---------- 复制 readme ----------
echo "[collect] readme.md ..."
cp "$SCRIPT_DIR/readme.md" "$OUTPUT_DIR/readme.md"

# ---------- 运行时资源（rawengine 加载色彩/相机数据所需） ----------
# 集成方需将 resource/ 目录内容放到与 therapee.dll 同级目录下
# 并在调用 rawengine_init() 前通过 RAWENGINE_DATADIR 环境变量或
# rawengine_set_datadir() 指定资源路径
echo "[collect] Runtime resources ..."
RTDATA_DIR="$RT_DIR/rtdata"

# 相机 DCP 色彩校准文件
if [[ -d "$RTDATA_DIR/dcpprofiles" ]]; then
    cp -r "$RTDATA_DIR/dcpprofiles" "$OUTPUT_DIR/resource/"
fi

# ICC 色彩空间文件（输入/输出）
if [[ -d "$RTDATA_DIR/iccprofiles" ]]; then
    cp -r "$RTDATA_DIR/iccprofiles" "$OUTPUT_DIR/resource/"
fi

# 处理预设（pp3 profiles）
if [[ -d "$RTDATA_DIR/profiles" ]]; then
    cp -r "$RTDATA_DIR/profiles" "$OUTPUT_DIR/resource/"
fi

# 相机参数数据库 JSON
for f in cammatrices.json dcraw.json rt.json; do
    [[ -f "$RTDATA_DIR/$f" ]] && cp "$RTDATA_DIR/$f" "$OUTPUT_DIR/resource/"
done

# external DCP（用于 make+model Standard.dcp 精确命中）
if [[ -d "$RTDATA_DIR/external" ]]; then
    cp -r "$RTDATA_DIR/external" "$OUTPUT_DIR/resource/"
fi

# 编译生成的 camconst.json（包含更完整的相机参数，优先于源码中的版本）
BUILT_CAMCONST="$BUILD_DIR/rtengine/camconst.json"
if [[ -f "$BUILT_CAMCONST" ]]; then
    cp "$BUILT_CAMCONST" "$OUTPUT_DIR/resource/"
elif [[ -f "$RTDATA_DIR/camconst.json" ]]; then
    cp "$RTDATA_DIR/camconst.json" "$OUTPUT_DIR/resource/"
fi


# 默认语言包（rawengine 内部用于格式化消息）
mkdir -p "$OUTPUT_DIR/resource/languages"
if [[ -f "$RTDATA_DIR/languages/default" ]]; then
    cp "$RTDATA_DIR/languages/default" "$OUTPUT_DIR/resource/languages/"
fi

RESOURCE_COUNT=$(find "$OUTPUT_DIR/resource" -type f | wc -l)
echo "  Copied ${RESOURCE_COUNT} resource file(s) to output/resource/"

# ---------- 同步 resource 到 bin（按你的集成要求） ----------
echo "[collect] Mirroring resource/ into bin/ ..."
cp -rf "$OUTPUT_DIR/resource/." "$OUTPUT_DIR/bin/"
RESOURCE_IN_BIN_COUNT=$(find "$OUTPUT_DIR/bin" -type f | wc -l)
echo "  Bin now contains ${RESOURCE_IN_BIN_COUNT} file(s) including mirrored resources"


# ---------- 汇总 ----------
echo ""
echo "============================================================"
echo " Output collected successfully:"
echo "  include/  : $(find "$OUTPUT_DIR/include" -name "*.h" | wc -l) header(s)"
echo "  lib/      : $(find "$OUTPUT_DIR/lib" -name "*.a" | wc -l) static lib(s), $(find "$OUTPUT_DIR/lib" -name "*.lib" -o -name "*.def" -o -name "*.dll.a" 2>/dev/null | wc -l) MSVC/import file(s)"
echo "  bin/      : $(find "$OUTPUT_DIR/bin" -type f \( -name "*.exe" -o -name "rawengine-cli" \) | wc -l) executable(s), $DLL_COUNT runtime lib(s)"
echo "  pdb/      : $(find "$OUTPUT_DIR/pdb" -type f | wc -l) debug file(s)"
echo "  resource/ : ${RESOURCE_COUNT} file(s) (dcpprofiles/iccprofiles/profiles/camconst)"
echo "============================================================"
echo " Output directory: $OUTPUT_DIR"
echo "============================================================"
