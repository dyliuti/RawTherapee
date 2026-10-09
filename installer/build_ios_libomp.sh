#!/usr/bin/env bash
# Cross-compile the LLVM OpenMP runtime (libomp) as a STATIC lib for iOS and
# install it into the RawEngine iOS deps prefixes so build_ios.sh can enable
# OpenMP (multi-threaded demosaic). Device + simulator slices (both arm64).
#
# Output per slice (consumed by build_ios.sh):
#   $IOS_DEPS_ROOT/<slice>/lib/libomp.a
#   $IOS_DEPS_ROOT/<slice>/include/omp.h
#
# Requires: git (network, to fetch LLVM openmp), cmake, ninja, Xcode.
# Usage:
#   bash build_ios_libomp.sh
#   LLVM_TAG=llvmorg-17.0.6 bash build_ios_libomp.sh --device-only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_DEPS_ROOT="${IOS_DEPS_ROOT:-$SCRIPT_DIR/output_ios_deps}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
LLVM_TAG="${LLVM_TAG:-llvmorg-17.0.6}"
WORK="${WORK:-$SCRIPT_DIR/build_ios_libomp}"
# 允许用外部已下载好的 LLVM 源码目录（含 openmp 子目录）；默认用 $WORK/llvm-project。
SRC="${LLVM_SRC:-$WORK/llvm-project}"
DEVICE_ONLY=0
[[ "${1:-}" == "--device-only" ]] && DEVICE_ONLY=1

command -v git >/dev/null   || { echo "[ERROR] git required"; exit 1; }
command -v cmake >/dev/null || { echo "[ERROR] cmake required"; exit 1; }
command -v ninja >/dev/null || { echo "[ERROR] ninja required"; exit 1; }
command -v xcrun >/dev/null || { echo "[ERROR] Xcode/xcrun required"; exit 1; }

mkdir -p "$WORK"
if [[ ! -d "$SRC/openmp" ]]; then
    echo "[libomp] fetching LLVM openmp ($LLVM_TAG) ..."
    git clone --depth 1 --branch "$LLVM_TAG" \
        --filter=blob:none --sparse https://github.com/llvm/llvm-project.git "$SRC"
    git -C "$SRC" sparse-checkout set openmp cmake llvm/cmake runtimes
fi

build_slice() {
    local sdk="$1" slice="$2"
    local sdk_path build_dir prefix
    sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
    build_dir="$WORK/$slice"
    prefix="$IOS_DEPS_ROOT/$slice"
    [[ -d "$prefix" ]] || { echo "[ERROR] missing deps prefix: $prefix (run build_ios_deps.sh)"; exit 1; }

    echo "[libomp] configuring $slice ($sdk/arm64)"
    # NOTE: libomp's CMake rejects CMAKE_SYSTEM_NAME=iOS, so we present Darwin +
    # explicit sysroot/arch/arch-override, which produces an iOS arm64 static lib.
    cmake -S "$SRC/openmp" -B "$build_dir" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Darwin \
        -DCMAKE_OSX_SYSROOT="$sdk_path" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBOMP_ARCH=aarch64 \
        -DLIBOMP_ENABLE_SHARED=OFF \
        -DLIBOMP_OMPT_SUPPORT=OFF \
        -DLIBOMP_USE_HWLOC=OFF \
        -DLIBOMP_FORTRAN_MODULES=OFF \
        -DOPENMP_ENABLE_LIBOMPTARGET=OFF \
        -DOPENMP_ENABLE_OMPT_TOOLS=OFF

    cmake --build "$build_dir" --target omp -j"$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

    local lib omp_h
    lib="$(find "$build_dir" -name 'libomp.a' | head -1)"
    omp_h="$(find "$build_dir" -name 'omp.h' | head -1)"
    [[ -f "$lib" && -f "$omp_h" ]] || { echo "[ERROR] libomp.a/omp.h not produced for $slice"; exit 1; }
    mkdir -p "$prefix/lib" "$prefix/include"
    cp "$lib" "$prefix/lib/libomp.a"
    cp "$omp_h" "$prefix/include/omp.h"
    echo "[libomp] installed -> $prefix/lib/libomp.a"

    # 兼容垫片：较新 Apple clang 会对 dynamic/guided OMP 循环发射 __kmpc_dispatch_deinit，
    # 旧版 libomp（如 17.0.6）无此符号 → 链接 undefined。它只是循环结束后的 dispatch 缓冲清理钩子，
    # 提供 no-op 定义即可安全放行（并行与解码结果不受影响，缓冲在 team 结束时统一回收）。
    # 仅当 libomp 确实缺该符号时补；weak 定义，日后换上含该符号的新 libomp 可自动让位。
    if ! nm "$prefix/lib/libomp.a" 2>/dev/null | grep -q 'T ___kmpc_dispatch_deinit'; then
        local shim_c="$build_dir/omp_compat_shim.c"
        cat > "$shim_c" <<'EOF'
/* no-op compat shim for Apple clang > bundled libomp; see build_ios_libomp.sh */
__attribute__((weak)) void __kmpc_dispatch_deinit(void *loc, int gtid) { (void)loc; (void)gtid; }
EOF
        local minflag="-miphoneos-version-min=$IOS_DEPLOYMENT_TARGET"
        [[ "$sdk" == "iphonesimulator" ]] && minflag="-mios-simulator-version-min=$IOS_DEPLOYMENT_TARGET"
        xcrun --sdk "$sdk" clang -arch arm64 "$minflag" -isysroot "$sdk_path" -O2 \
            -c "$shim_c" -o "$build_dir/omp_compat_shim.o"
        libtool -static -o "$prefix/lib/libompcompat.a" "$build_dir/omp_compat_shim.o"
        echo "[libomp] added compat shim (__kmpc_dispatch_deinit) -> $prefix/lib/libompcompat.a"
    fi
}

build_slice iphoneos ios-arm64
[[ "$DEVICE_ONLY" -eq 0 ]] && build_slice iphonesimulator ios-arm64-simulator
echo "[libomp] done. Now run: bash build_ios.sh"
