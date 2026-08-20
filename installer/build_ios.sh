#!/usr/bin/env bash
# Build RawEngine as a static iOS XCFramework.
#
# Third-party dependencies must already be cross-compiled for iOS. By default
# they are read from installer/output_ios_deps/<slice>, as produced by
# build_ios_deps.sh. This script never links macOS Homebrew libraries.
#
# Usage:
#   bash build_ios.sh
#   IOS_DEPS_ROOT=/path/to/ios/deps bash build_ios.sh --clean
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_ROOT="$RT_DIR/build_ios"
OUTPUT_DIR="$SCRIPT_DIR/output_ios"
XCFRAMEWORK_DIR="$OUTPUT_DIR/RawEngine.xcframework"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"
IOS_DEPS_ROOT="${IOS_DEPS_ROOT:-$SCRIPT_DIR/output_ios_deps}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
RAWENGINE_INSTALL_DIR="${RAWENGINE_INSTALL_DIR:-}"
DO_CLEAN=0
DEVICE_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) DO_CLEAN=1; shift ;;
        --device-only) DEVICE_ONLY=1; shift ;;
        *) echo "[ERROR] Unknown option: $1"; exit 1 ;;
    esac
done

if [[ ! -d "$IOS_DEPS_ROOT/ios-arm64" ]]; then
    echo "[ERROR] iOS dependency slices were not found below:"
    echo "        $IOS_DEPS_ROOT"
    echo "        Run installer/build_ios_deps.sh first."
    exit 1
fi
if [[ "$DEVICE_ONLY" -eq 0 &&
      ! -d "$IOS_DEPS_ROOT/ios-arm64-simulator" ]]; then
    echo "[ERROR] iOS Simulator dependency slice was not found below:"
    echo "        $IOS_DEPS_ROOT/ios-arm64-simulator"
    echo "        Run installer/build_ios_deps.sh --slice ios-arm64-simulator first,"
    echo "        or pass --device-only."
    exit 1
fi

CMAKE_BIN="${CMAKE_BIN:-$(command -v cmake || true)}"
NINJA_BIN="${NINJA_BIN:-$(command -v ninja || true)}"
if [[ -z "$CMAKE_BIN" || ! -x "$CMAKE_BIN" ]]; then
    echo "[ERROR] cmake is required."
    exit 1
fi
if [[ -z "$NINJA_BIN" || ! -x "$NINJA_BIN" ]]; then
    echo "[ERROR] ninja is required."
    exit 1
fi
if ! command -v xcodebuild >/dev/null 2>&1; then
    echo "[ERROR] xcodebuild is required to create the XCFramework."
    exit 1
fi

if [[ "$DO_CLEAN" -eq 1 ]]; then
    rm -rf "$BUILD_ROOT" "$OUTPUT_DIR"
fi

mkdir -p "$BUILD_ROOT" "$OUTPUT_DIR/include" "$OUTPUT_DIR/resource"
cp "$RT_DIR/rtengine/raw_engine.h" "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/rawengine_types.h" "$OUTPUT_DIR/include/"
cp "$RT_DIR/rtengine/raw_engine_error_def.h" "$OUTPUT_DIR/include/"
mkdir -p "$OUTPUT_DIR/include/libraw"
cp "$RT_DIR/rtengine/libraw/libraw"/*.h "$OUTPUT_DIR/include/libraw/"

build_slice() {
    local sdk="$1"
    local arch="$2"
    local slice="$3"
    local build_dir="$BUILD_ROOT/$slice"
    local deps_prefix="$IOS_DEPS_ROOT/$slice"
    local sdk_path
    local archive="$build_dir/rtengine/libRawEngine.a"
    sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"

    echo "[cmake] Building $slice ($sdk/$arch)"
    PKG_CONFIG_PATH= \
    PKG_CONFIG_LIBDIR="$deps_prefix/lib/pkgconfig:$deps_prefix/share/pkgconfig" \
    "$CMAKE_BIN" -S "$RT_DIR" -B "$build_dir" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sdk_path" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$deps_prefix" \
        -DCMAKE_FIND_ROOT_PATH="$deps_prefix" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_MAKE_PROGRAM="$NINJA_BIN" \
        -DFETCHCONTENT_SOURCE_DIR_FMT="$deps_prefix/share/rawengine/fmt-src" \
        -DRAWENGINE_ONLY=ON \
        -DRAWENGINE_STATIC=ON \
        -DOPTION_OMP=OFF \
        -DWITH_JXL=OFF \
        -DSVG_BACKEND=lunasvg \
        -DWITH_SYSTEM_LIBRAW=OFF
    "$CMAKE_BIN" --build "$build_dir" -j"$JOBS" --target rawengine

    local engine_archive="$build_dir/rtengine/librtengine.a"
    local libraw_archive="$build_dir/rtengine/libraw/lib/.libs/libraw_r.a"
    if [[ ! -f "$archive" || ! -f "$engine_archive" ]]; then
        echo "[ERROR] Static library not found: $archive"
        exit 1
    fi
    if [[ ! -f "$libraw_archive" ]]; then
        echo "[ERROR] RawTherapee LibRaw archive not found: $libraw_archive"
        exit 1
    fi

    # A static library does not absorb its static dependencies at link time.
    # Merge the public wrapper, rtengine, and all direct-prefix static
    # dependencies into one consumer-facing archive. This keeps the integration
    # package small and avoids exposing the dependency build prefix to users.
    local dependency_libs=()
    local dependency_lib
    for dependency_lib in "$deps_prefix"/lib/*.a; do
        [[ -f "$dependency_lib" ]] || continue
        # RawTherapee uses the regular JPEG API. TurboJPEG ships a second
        # copy of the same jpeg_* core objects; ImgDecodeThirdParty owns the
        # TurboJPEG API separately, so do not duplicate that core here.
        [[ "$(basename "$dependency_lib")" == "libturbojpeg.a" ]] && continue
        dependency_libs+=("$dependency_lib")
    done
    local merged_archive="$build_dir/rtengine/libRawEngine.merged.a"
    libtool -static -o "$merged_archive" \
        "$archive" "$engine_archive" "$libraw_archive" "${dependency_libs[@]}"
    mv "$merged_archive" "$archive"
    printf '%s\n' "$archive"
}

build_slice iphoneos arm64 ios-arm64
DEVICE_LIB="$BUILD_ROOT/ios-arm64/rtengine/libRawEngine.a"

if [[ "$DEVICE_ONLY" -eq 0 ]]; then
    build_slice iphonesimulator arm64 ios-arm64-simulator
    SIMULATOR_LIB="$BUILD_ROOT/ios-arm64-simulator/rtengine/libRawEngine.a"
fi

rm -rf "$XCFRAMEWORK_DIR"
if [[ "$DEVICE_ONLY" -eq 1 ]]; then
    xcodebuild -create-xcframework \
        -library "$DEVICE_LIB" -headers "$OUTPUT_DIR/include" \
        -output "$XCFRAMEWORK_DIR"
else
    xcodebuild -create-xcframework \
        -library "$DEVICE_LIB" -headers "$OUTPUT_DIR/include" \
        -library "$SIMULATOR_LIB" -headers "$OUTPUT_DIR/include" \
        -output "$XCFRAMEWORK_DIR"
fi

if [[ -n "$RAWENGINE_INSTALL_DIR" ]]; then
    INSTALL_XCFRAMEWORK="$RAWENGINE_INSTALL_DIR/RawEngine.xcframework"
    mkdir -p "$RAWENGINE_INSTALL_DIR/include"
    rm -rf "$INSTALL_XCFRAMEWORK"
    if [[ "$DEVICE_ONLY" -eq 1 ]]; then
        xcodebuild -create-xcframework \
            -library "$DEVICE_LIB" -headers "$OUTPUT_DIR/include" \
            -output "$INSTALL_XCFRAMEWORK"
    else
        xcodebuild -create-xcframework \
            -library "$DEVICE_LIB" -headers "$OUTPUT_DIR/include" \
            -library "$SIMULATOR_LIB" -headers "$OUTPUT_DIR/include" \
            -output "$INSTALL_XCFRAMEWORK"
    fi
    cp "$OUTPUT_DIR/include/"*.h "$RAWENGINE_INSTALL_DIR/include/"
    echo "Installed RawEngine integration package:"
    echo "  $RAWENGINE_INSTALL_DIR"
fi

RTDATA_DIR="$RT_DIR/rtdata"
for dir in dcpprofiles iccprofiles profiles; do
    [[ -d "$RTDATA_DIR/$dir" ]] && cp -R "$RTDATA_DIR/$dir" "$OUTPUT_DIR/resource/"
done
for file in cammatrices.json dcraw.json rt.json; do
    [[ -f "$RTDATA_DIR/$file" ]] && cp "$RTDATA_DIR/$file" "$OUTPUT_DIR/resource/"
done
for build_dir in "$BUILD_ROOT/ios-arm64" "$BUILD_ROOT/ios-arm64-simulator"; do
    if [[ -f "$build_dir/rtengine/camconst.json" ]]; then
        cp "$build_dir/rtengine/camconst.json" "$OUTPUT_DIR/resource/"
        break
    fi
done
[[ -d "$RTDATA_DIR/languages" ]] && cp -R "$RTDATA_DIR/languages" "$OUTPUT_DIR/resource/"

echo ""
echo "Static RawEngine XCFramework created:"
echo "  $XCFRAMEWORK_DIR"
echo "Resources:"
echo "  $OUTPUT_DIR/resource"
