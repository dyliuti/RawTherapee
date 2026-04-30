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
if ! command -v cmake &>/dev/null; then
    echo "[ERROR] cmake not found. Please run in MSYS2 ucrt64 shell."
    exit 1
fi
if ! command -v ninja &>/dev/null; then
    echo "[ERROR] ninja not found. Install: pacman -S mingw-w64-ucrt-x86_64-ninja"
    exit 1
fi

echo "============================================================"
echo " rawengine build & collect"
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
    echo "[ninja] Building rawengine-cli (jobs=$JOBS) ..."
    ninja -C "$BUILD_DIR" -j"$JOBS" rawengine-cli
else
    echo "[skip-build] Skipping build, using existing artifacts in $BUILD_DIR"
fi

# ---------- 验证产物存在 ----------
CLI_EXE="$BUILD_DIR/rtengine/rawengine-cli.exe"
LIB_A="$BUILD_DIR/rtengine/librtengine.a"
RAWENGINE_DLL="$BUILD_DIR/rtengine/rawengine.dll"

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

# ---------- 动态库（DLL + MinGW 导入库） ----------
echo "[collect] rawengine.dll ..."
cp "$RAWENGINE_DLL" "$OUTPUT_DIR/bin/"
# MinGW 同时生成 librawengine.dll.a（MinGW/GCC 工程可直接链接）
RAWENGINE_DLL_A="$BUILD_DIR/rtengine/librawengine.dll.a"
if [[ -f "$RAWENGINE_DLL_A" ]]; then
    cp "$RAWENGINE_DLL_A" "$OUTPUT_DIR/lib/"
fi

# ---------- 为 MSVC 生成导入库（.def + .lib） ----------
echo "[collect] Generating MSVC import library ..."
RAWENGINE_DEF="$OUTPUT_DIR/lib/rawengine.def"
RAWENGINE_LIB="$OUTPUT_DIR/lib/rawengine.lib"

# 直接从 DLL 导出表提取函数名，生成 .def 文件
{
    echo "LIBRARY rawengine.dll"
    echo "EXPORTS"
    objdump -x "$OUTPUT_DIR/bin/rawengine.dll" 2>/dev/null \
        | awk '/\[Name Pointer\/Ordinal\]/{found=1} found && /\+base\[/{
            # lines like:  [  74] +base[  75]  004a rawengine_decode
            match($0, /[0-9a-f]{4} ([A-Za-z_][A-Za-z0-9_]+)$/, m)
            if (m[1] != "" && substr(m[1],1,10) == "rawengine_") print "    " m[1]
        }'
} > "$RAWENGINE_DEF"

# 用 llvm-dlltool 生成 MSVC 兼容的 .lib（比 GNU dlltool 生成的格式更兼容）
if command -v llvm-dlltool &>/dev/null; then
    llvm-dlltool -m i386:x86-64 -D rawengine.dll -d "$RAWENGINE_DEF" -l "$RAWENGINE_LIB" 2>/dev/null || true
    if [[ -f "$RAWENGINE_LIB" ]]; then
        EXPORTED_COUNT=$(grep -c '    ' "$RAWENGINE_DEF" 2>/dev/null || echo 0)
        echo "  rawengine.def (${EXPORTED_COUNT} exports) + rawengine.lib generated"
    else
        echo "  [warn] llvm-dlltool failed to generate rawengine.lib"
    fi
else
    echo "  [warn] llvm-dlltool not found; rawengine.lib not generated"
    echo "         Install: pacman -S mingw-w64-ucrt-x86_64-llvm"
    echo "         Or on MSVC machine: lib /DEF:rawengine.def /MACHINE:X64 /OUT:rawengine.lib"
fi

# ---------- CLI 可执行文件 ----------
echo "[collect] rawengine-cli.exe ..."
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

collect_dlls_recursive "$OUTPUT_DIR/bin/rawengine-cli.exe"
collect_dlls_recursive "$OUTPUT_DIR/bin/rawengine.dll"
rm -f "$DLL_SEEN"

DLL_COUNT=$(find "$OUTPUT_DIR/bin" -name "*.dll" | wc -l)
echo "  Copied $DLL_COUNT DLL(s) to output/bin/"

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

extract_debug "$OUTPUT_DIR/bin/rawengine-cli.exe"
extract_debug "$OUTPUT_DIR/bin/rawengine.dll"

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
# 集成方需将 resource/ 目录内容放到与 rawengine.dll 同级目录下
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

# ---------- 汇总 ----------
echo ""
echo "============================================================"
echo " Output collected successfully:"
echo "  include/  : $(find "$OUTPUT_DIR/include" -name "*.h" | wc -l) header(s)"
echo "  lib/      : $(find "$OUTPUT_DIR/lib" -name "*.a" | wc -l) static lib(s), $(find "$OUTPUT_DIR/lib" -name "*.lib" -o -name "*.def" -o -name "*.dll.a" 2>/dev/null | wc -l) MSVC/import file(s)"
echo "  bin/      : $(find "$OUTPUT_DIR/bin" -name "*.exe" | wc -l) exe(s), $DLL_COUNT DLL(s)"
echo "  pdb/      : $(find "$OUTPUT_DIR/pdb" -type f | wc -l) debug file(s)"
echo "  resource/ : ${RESOURCE_COUNT} file(s) (dcpprofiles/iccprofiles/profiles/camconst)"
echo "============================================================"
echo " Output directory: $OUTPUT_DIR"
echo "============================================================"
