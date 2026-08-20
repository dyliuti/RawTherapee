#!/usr/bin/env bash
# Cross-compile RawEngine's third-party dependencies for iPhoneOS and the
# iPhoneSimulator. Each slice is installed into an isolated static prefix.
#
# Usage:
#   bash build_ios_deps.sh
#   bash build_ios_deps.sh --slice ios-arm64
#   bash build_ios_deps.sh --slice ios-arm64-simulator --clean
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_ROOT="${IOS_DEPS_WORK_ROOT:-$SCRIPT_DIR/.ios_deps_work}"
SOURCE_ROOT="$WORK_ROOT/sources"
DOWNLOAD_ROOT="$WORK_ROOT/downloads"
BUILD_ROOT="$WORK_ROOT/build"
OUTPUT_ROOT="${IOS_DEPS_ROOT:-$SCRIPT_DIR/output_ios_deps}"
DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"
SELECTED_SLICE=""
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --slice) SELECTED_SLICE="$2"; shift 2 ;;
        --clean) DO_CLEAN=1; shift ;;
        *) echo "[ERROR] Unknown option: $1"; exit 1 ;;
    esac
done

case "$SELECTED_SLICE" in
    ""|ios-arm64|ios-arm64-simulator) ;;
    *) echo "[ERROR] Unsupported slice: $SELECTED_SLICE"; exit 1 ;;
esac

for tool in cmake ninja pkg-config curl tar xcrun xcodebuild make python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] Required host tool not found: $tool"
        echo "        Install host tools with: brew install cmake ninja pkg-config"
        exit 1
    fi
done

mkdir -p "$SOURCE_ROOT" "$DOWNLOAD_ROOT" "$BUILD_ROOT" "$OUTPUT_ROOT"

if ! command -v meson >/dev/null 2>&1; then
    MESON_VENV="$WORK_ROOT/meson-venv"
    if [[ ! -x "$MESON_VENV/bin/meson" ]]; then
        echo "[bootstrap] meson"
        python3 -m venv "$MESON_VENV"
        "$MESON_VENV/bin/python" -m pip install --disable-pip-version-check \
            "meson==1.5.2" "packaging"
    fi
    if ! "$MESON_VENV/bin/python" -c 'import packaging' >/dev/null 2>&1; then
        "$MESON_VENV/bin/python" -m pip install --disable-pip-version-check "packaging"
    fi
    export PATH="$MESON_VENV/bin:$PATH"
fi

# Pinned source releases. Override an individual URL from the environment when
# an internal mirror is required.
ZLIB_URL="${ZLIB_URL:-https://zlib.net/fossils/zlib-1.3.1.tar.gz}"
JPEG_URL="${JPEG_URL:-https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.3.tar.gz}"
PNG_URL="${PNG_URL:-https://download.sourceforge.net/libpng/libpng-1.6.43.tar.xz}"
TIFF_URL="${TIFF_URL:-https://download.osgeo.org/libtiff/tiff-4.7.0.tar.xz}"
EXPAT_URL="${EXPAT_URL:-https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.xz}"
LCMS_URL="${LCMS_URL:-https://github.com/mm2/Little-CMS/releases/download/lcms2.16/lcms2-2.16.tar.gz}"
PCRE2_URL="${PCRE2_URL:-https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz}"
LIBFFI_URL="${LIBFFI_URL:-https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz}"
SIGC_URL="${SIGC_URL:-https://download.gnome.org/sources/libsigc++/2.12/libsigc++-2.12.1.tar.xz}"
GLIB_URL="${GLIB_URL:-https://download.gnome.org/sources/glib/2.80/glib-2.80.4.tar.xz}"
GLIBMM_URL="${GLIBMM_URL:-https://download.gnome.org/sources/glibmm/2.66/glibmm-2.66.7.tar.xz}"
PIXMAN_URL="${PIXMAN_URL:-https://cairographics.org/releases/pixman-0.43.4.tar.gz}"
CAIRO_URL="${CAIRO_URL:-https://cairographics.org/releases/cairo-1.18.0.tar.xz}"
CAIROMM_URL="${CAIROMM_URL:-https://cairographics.org/releases/cairomm-1.14.5.tar.xz}"
EXIV2_URL="${EXIV2_URL:-https://github.com/Exiv2/exiv2/archive/refs/tags/v0.28.3.tar.gz}"
LENSFUN_URL="${LENSFUN_URL:-https://github.com/lensfun/lensfun/archive/refs/tags/v0.3.4.tar.gz}"
FFTW_URL="${FFTW_URL:-https://www.fftw.org/fftw-3.3.10.tar.gz}"
IPTCDATA_URL="${IPTCDATA_URL:-https://http.kali.org/pool/main/libi/libiptcdata/libiptcdata_1.0.5.orig.tar.gz}"
FMT_URL="${FMT_URL:-https://github.com/fmtlib/fmt/archive/refs/tags/12.0.0.tar.gz}"

fetch_source() {
    local name="$1"
    local url="$2"
    local source_dir="$SOURCE_ROOT/$name"
    local archive="$DOWNLOAD_ROOT/${url##*/}"

    if [[ ! -d "$source_dir" ]]; then
        if [[ -f "$archive" ]] && ! tar -tf "$archive" >/dev/null 2>&1; then
            echo "[download] Removing incomplete archive: $archive"
            rm -f "$archive"
        fi
        if [[ ! -f "$archive" ]]; then
            echo "[download] $name"
            local partial="$archive.partial"
            curl --fail --http1.1 --location \
                --connect-timeout 30 --max-time 600 \
                --retry 8 --retry-all-errors --retry-delay 3 \
                --continue-at - \
                --output "$partial" "$url"
            mv "$partial" "$archive"
        fi
        local unpack_dir="$SOURCE_ROOT/.unpack-$name"
        rm -rf "$unpack_dir"
        mkdir -p "$unpack_dir"
        tar -xf "$archive" -C "$unpack_dir"
        local entries=("$unpack_dir"/*)
        if [[ ${#entries[@]} -ne 1 || ! -d "${entries[0]}" ]]; then
            echo "[ERROR] Unexpected archive layout for $name"
            exit 1
        fi
        mv "${entries[0]}" "$source_dir"
        rm -rf "$unpack_dir"
    fi
    printf '%s\n' "$source_dir"
}

prepare_sources() {
    fetch_source zlib "$ZLIB_URL" >/dev/null
    fetch_source jpeg "$JPEG_URL" >/dev/null
    fetch_source png "$PNG_URL" >/dev/null
    fetch_source tiff "$TIFF_URL" >/dev/null
    fetch_source expat "$EXPAT_URL" >/dev/null
    fetch_source lcms "$LCMS_URL" >/dev/null
    fetch_source pcre2 "$PCRE2_URL" >/dev/null
    fetch_source libffi "$LIBFFI_URL" >/dev/null
    fetch_source sigc "$SIGC_URL" >/dev/null
    fetch_source glib "$GLIB_URL" >/dev/null
    fetch_source glibmm "$GLIBMM_URL" >/dev/null
    fetch_source pixman "$PIXMAN_URL" >/dev/null
    fetch_source cairo "$CAIRO_URL" >/dev/null
    fetch_source cairomm "$CAIROMM_URL" >/dev/null
    fetch_source exiv2 "$EXIV2_URL" >/dev/null
    fetch_source lensfun "$LENSFUN_URL" >/dev/null
    fetch_source fftw "$FFTW_URL" >/dev/null
    fetch_source iptcdata "$IPTCDATA_URL" >/dev/null
    fetch_source fmt "$FMT_URL" >/dev/null
}

setup_slice() {
    SLICE="$1"
    if [[ "$SLICE" == "ios-arm64" ]]; then
        SDK="iphoneos"
        MIN_FLAG="-miphoneos-version-min=$DEPLOYMENT_TARGET"
    else
        SDK="iphonesimulator"
        MIN_FLAG="-mios-simulator-version-min=$DEPLOYMENT_TARGET"
    fi
    ARCH="arm64"
    HOST_TRIPLET="aarch64-apple-darwin"
    PREFIX="$OUTPUT_ROOT/$SLICE"
    SLICE_BUILD_ROOT="$BUILD_ROOT/$SLICE"
    SYSROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
    CC="$(xcrun --sdk "$SDK" --find clang)"
    CXX="$(xcrun --sdk "$SDK" --find clang++)"
    AR="$(xcrun --sdk "$SDK" --find ar)"
    RANLIB="$(xcrun --sdk "$SDK" --find ranlib)"
    STRIP="$(xcrun --sdk "$SDK" --find strip)"
    CFLAGS="-arch $ARCH -isysroot $SYSROOT $MIN_FLAG -fPIC"
    CXXFLAGS="$CFLAGS -stdlib=libc++"
    LDFLAGS="-arch $ARCH -isysroot $SYSROOT $MIN_FLAG"
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
    export SDKROOT="$SYSROOT" CC CXX AR RANLIB STRIP CFLAGS CXXFLAGS LDFLAGS
    export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR
    mkdir -p "$PREFIX" "$SLICE_BUILD_ROOT"
    IOS_TOOLCHAIN_FILE="$SLICE_BUILD_ROOT/ios-$SLICE.toolchain.cmake"
    cat > "$IOS_TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR "$ARCH" CACHE STRING "" FORCE)
set(CMAKE_OSX_SYSROOT "$SDK" CACHE STRING "" FORCE)
set(CMAKE_OSX_ARCHITECTURES "$ARCH" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "$DEPLOYMENT_TARGET" CACHE STRING "" FORCE)
EOF
}

cmake_build() {
    local name="$1"
    local source_dir="$2"
    shift 2
    local build_dir="$SLICE_BUILD_ROOT/$name"
    echo "[build][$SLICE] $name"
    if [[ -f "$build_dir/CMakeCache.txt" ]] &&
       ! grep -q '^CMAKE_TOOLCHAIN_FILE:FILEPATH=' "$build_dir/CMakeCache.txt"; then
        echo "[cmake][$SLICE] Recreating $name cache with iOS toolchain"
        rm -rf "$build_dir"
    fi
    cmake -S "$source_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$IOS_TOOLCHAIN_FILE" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
        -DCMAKE_OSX_SYSROOT="$SDK" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_LIBRARY_SUFFIXES=.a \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    cmake --build "$build_dir" -j"$JOBS"
    cmake --install "$build_dir"
    if [[ "$name" == "zlib" ]]; then
        rm -f "$PREFIX"/lib/libz*.dylib
    fi
}

autotools_build() {
    local name="$1"
    local source_dir="$2"
    shift 2
    local build_dir="$SLICE_BUILD_ROOT/$name"
    echo "[build][$SLICE] $name"
    if [[ ! -x "$source_dir/configure" && -x "$source_dir/autogen.sh" ]]; then
        echo "[autogen][$SLICE] $name"
        (cd "$source_dir" && ./autogen.sh)
    fi
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    (
        cd "$build_dir"
        "$source_dir/configure" \
            --host="$HOST_TRIPLET" \
            --prefix="$PREFIX" \
            --disable-shared \
            --enable-static \
            "$@"
        make -j"$JOBS"
        make install
    )
}

write_meson_cross_file() {
    MESON_CROSS_FILE="$SLICE_BUILD_ROOT/meson-$SLICE.ini"
    cat > "$MESON_CROSS_FILE" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$(command -v pkg-config)'

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PKG_CONFIG_LIBDIR'

[built-in options]
c_args = ['-arch', '$ARCH', '-isysroot', '$SYSROOT', '$MIN_FLAG', '-fPIC']
cpp_args = ['-arch', '$ARCH', '-isysroot', '$SYSROOT', '$MIN_FLAG', '-fPIC', '-stdlib=libc++']
c_link_args = ['-arch', '$ARCH', '-isysroot', '$SYSROOT', '$MIN_FLAG']
cpp_link_args = ['-arch', '$ARCH', '-isysroot', '$SYSROOT', '$MIN_FLAG', '-stdlib=libc++']
default_library = 'static'
EOF
}

meson_build() {
    local name="$1"
    local source_dir="$2"
    shift 2
    local build_dir="$SLICE_BUILD_ROOT/$name"
    echo "[build][$SLICE] $name"
    if [[ -d "$build_dir" ]]; then
        rm -rf "$build_dir"
    fi
    meson setup "$build_dir" "$source_dir" \
        --cross-file "$MESON_CROSS_FILE" \
        --prefix "$PREFIX" \
        --libdir lib \
        --buildtype release \
        --default-library static \
        "$@"
    if [[ "$name" == "glib" && "$SLICE" == "ios-arm64-simulator" ]]; then
        # The simulator SDK headers do not expose pipe2(), but GLib's
        # cross-compile probe can incorrectly infer it from the host.
        sed -i '' 's/^#define HAVE_PIPE2 1$/#undef HAVE_PIPE2/' \
            "$build_dir/config.h"
    fi
    meson compile -C "$build_dir" -j "$JOBS"
    meson install -C "$build_dir"
}

build_slice() {
    setup_slice "$1"
    write_meson_cross_file

    cmake_build zlib "$SOURCE_ROOT/zlib" \
        -DZLIB_BUILD_EXAMPLES=OFF
    cmake_build fmt "$SOURCE_ROOT/fmt" \
        -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_INSTALL=OFF
    mkdir -p "$PREFIX/share/rawengine"
    rm -rf "$PREFIX/share/rawengine/fmt-src"
    cp -R "$SOURCE_ROOT/fmt" "$PREFIX/share/rawengine/fmt-src"
    cmake_build jpeg "$SOURCE_ROOT/jpeg" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
        -DWITH_JPEG8=ON -DWITH_TURBOJPEG=OFF \
        -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
    cmake_build png "$SOURCE_ROOT/png" \
        -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_FRAMEWORK=OFF \
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
    cmake_build tiff "$SOURCE_ROOT/tiff" \
        -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF \
        -Dtiff-docs=OFF -Dwebp=OFF -Dzstd=OFF -Dlerc=OFF -Djbig=OFF
    cmake_build expat "$SOURCE_ROOT/expat" \
        -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF \
        -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF
    autotools_build lcms "$SOURCE_ROOT/lcms" \
        --without-zlib --without-jpeg --without-tiff --without-python
    cmake_build pcre2 "$SOURCE_ROOT/pcre2" \
        -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF \
        -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_BUILD_TESTS=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF

    autotools_build libffi "$SOURCE_ROOT/libffi" \
        --disable-docs --disable-multi-os-directory \
        gcc_cv_as_cfi_pseudo_op=no
    meson_build sigc "$SOURCE_ROOT/sigc" \
        -Dbuild-tests=false -Dbuild-examples=false
    meson_build glib "$SOURCE_ROOT/glib" \
        -Dtests=false -Dinstalled_tests=false -Dnls=disabled \
        -Dlibmount=disabled -Dselinux=disabled -Dxattr=false \
        -Dman-pages=disabled -Ddtrace=false -Dsystemtap=false \
        -Dsysprof=disabled
    meson_build glibmm "$SOURCE_ROOT/glibmm" \
        -Dbuild-examples=false \
        -Dbuild-documentation=false

    meson_build pixman "$SOURCE_ROOT/pixman" \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
        -Dlibpng=enabled
    meson_build cairo "$SOURCE_ROOT/cairo" \
        -Dtests=disabled -Dtee=disabled \
        -Dpng=enabled -Dzlib=enabled -Dfreetype=disabled \
        -Dfontconfig=disabled -Dxlib=disabled -Dxcb=disabled \
        -Dquartz=disabled
    meson_build cairomm "$SOURCE_ROOT/cairomm" \
        -Dbuild-tests=false -Dboost-shared=false \
        -Dbuild-examples=false \
        -Dbuild-documentation=false

    cmake_build exiv2 "$SOURCE_ROOT/exiv2" \
        -DEXIV2_BUILD_SAMPLES=OFF -DEXIV2_BUILD_EXIV2_COMMAND=OFF \
        -DEXIV2_BUILD_UNIT_TESTS=OFF -DEXIV2_BUILD_FUZZ_TESTS=OFF \
        -DEXIV2_ENABLE_XMP=ON -DEXIV2_ENABLE_PNG=ON \
        -DEXIV2_ENABLE_BMFF=ON -DEXIV2_ENABLE_BROTLI=OFF \
        -DEXIV2_ENABLE_INIH=OFF -DEXIV2_ENABLE_VIDEO=OFF \
        -DEXIV2_ENABLE_WEBREADY=OFF
    cmake_build lensfun "$SOURCE_ROOT/lensfun" \
        -DBUILD_STATIC=ON -DBUILD_TESTS=OFF -DBUILD_LENSTOOL=OFF \
        -DBUILD_FOR_SSE=OFF -DBUILD_FOR_SSE2=OFF -DBUILD_DOC=OFF \
        -DINSTALL_PYTHON_MODULE=OFF -DINSTALL_HELPER_SCRIPTS=OFF \
        -DPYTHON=PYTHON-NOTFOUND \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    autotools_build fftw "$SOURCE_ROOT/fftw" \
        --enable-float --disable-fortran --disable-threads \
        --disable-openmp
    autotools_build iptcdata "$SOURCE_ROOT/iptcdata" \
        --disable-nls --disable-python

    echo "[done] $SLICE -> $PREFIX"
}

if [[ $DO_CLEAN -eq 1 ]]; then
    if [[ -n "$SELECTED_SLICE" ]]; then
        rm -rf "$BUILD_ROOT/$SELECTED_SLICE" "$OUTPUT_ROOT/$SELECTED_SLICE"
    else
        rm -rf "$BUILD_ROOT" "$OUTPUT_ROOT"
    fi
fi

prepare_sources
if [[ -z "$SELECTED_SLICE" || "$SELECTED_SLICE" == "ios-arm64" ]]; then
    build_slice ios-arm64
fi
if [[ -z "$SELECTED_SLICE" || "$SELECTED_SLICE" == "ios-arm64-simulator" ]]; then
    build_slice ios-arm64-simulator
fi

echo ""
echo "iOS dependency prefixes are ready:"
echo "  $OUTPUT_ROOT/ios-arm64"
echo "  $OUTPUT_ROOT/ios-arm64-simulator"
