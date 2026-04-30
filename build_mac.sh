set -e  # 遇到错误立即退出

# 项目配置 - 动态获取当前目录
PROJECT_NAME="LibRAWEngine"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="Release"

# 编译器配置
if [ "$1" == "arm" ]; then
    CMAKE_BINARY="/opt/homebrew/bin/cmake"
    echo "build mac arm ... "
else    
    CMAKE_BINARY="/usr/local/bin/cmake"
    echo "build mac intel ... "
fi

C_COMPILER="/usr/bin/clang"
CXX_COMPILER="/usr/bin/clang++"

# 显示构建信息
echo "=========================================="
echo "项目名称: ${PROJECT_NAME}"
echo "项目根目录: ${PROJECT_ROOT}"
echo "构建目录: ${BUILD_DIR}"
echo "构建类型: ${BUILD_TYPE}"
echo "=========================================="

# 清理构建目录
echo "[driver] 清理构建缓存..."
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

mkdir -p "${BUILD_DIR}"

# 配置项目
echo "[main] 正在配置项目: ${PROJECT_NAME}"

${CMAKE_BINARY} \
    -DCMAKE_BUILD_TYPE:STRING="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=TRUE \
    -DCMAKE_C_COMPILER:FILEPATH="${C_COMPILER}" \
    -DCMAKE_CXX_COMPILER:FILEPATH="${CXX_COMPILER}" \
    -DOSX_DEV_BUILD=OFF \
    --no-warn-unused-cli \
    -S"${PROJECT_ROOT}" \
    -B"${BUILD_DIR}" \
    -G Ninja

# 构建项目
echo "[main] 正在构建项目: ${PROJECT_NAME}"
${CMAKE_BINARY} --build "${BUILD_DIR}" --config "${BUILD_TYPE}"

# Create output directory
OUTPUT_DIR="${PROJECT_ROOT}/output"
if [ -d "$OUTPUT_DIR" ]; then
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p "${OUTPUT_DIR}/lib"

# Copy dylib files
cp -f "${BUILD_DIR}/RAWEngine/lib/"*.dylib "${OUTPUT_DIR}/lib/"
cp -f "${BUILD_DIR}/RAWEngine/"*.dylib "${OUTPUT_DIR}/lib/"


mkdir -p "${OUTPUT_DIR}/static"
cp -f "${BUILD_DIR}/RAWEngine/libraw/lib/.libs/libraw_r.a" "${OUTPUT_DIR}/static/"

mkdir -p "${OUTPUT_DIR}/include"
cp -f "${PROJECT_ROOT}/RAWEngine/raw_engine.h" "${OUTPUT_DIR}/include/"
cp -f "${PROJECT_ROOT}/RAWEngine/rawengine_types.h" "${OUTPUT_DIR}/include/"
cp -r "${BUILD_DIR}/RAWEngine/libraw/libraw" "${OUTPUT_DIR}/include/"

# Copy resources directory
cp -R "${PROJECT_ROOT}/resources" "${OUTPUT_DIR}/"

echo "resources copy end ...."

# 修改dylib ID路径
LIB_DIR="${OUTPUT_DIR}/lib"
echo "Fixing dylib IDs..."
find "${LIB_DIR}" -name "*.dylib" | while read lib; do
    # 修改ID为@rpath格式
    libname=$(basename "$lib")
    install_name_tool -id "@rpath/${libname}" "$lib"
done

for lib in "${LIB_DIR}"/*.dylib; do
    # 更新依赖的其他库路径
    for dep in $(otool -L "$lib" | awk '{print $1}' | grep -v "@rpath" | grep -v "/usr/lib" | grep -v "/System"); do
        if [ -n "$dep" ]; then
            libname=$(basename "$dep")
            install_name_tool -change "$dep" "@rpath/${libname}" "$lib"
        fi
        if ! otool -l "$lib" | grep -q "@loader_path/../Frameworks"; then
            install_name_tool -add_rpath "@loader_path/../Frameworks" "$lib"
        fi
    done
done

echo "change rpath end ...."
