# LibRaw has its own configuration script and uses make to build. Here we add
# the LibRaw configuration commands so they run during the CMake configuration.
# Also, add a target which always runs make.

set(LIBRAW_DIR "${CMAKE_CURRENT_BINARY_DIR}/libraw")
set(LIBRAW_LIB_DIR "${LIBRAW_DIR}/lib")
set(LIBRAW_PHANTOM_FILE "${LIBRAW_LIB_DIR}/phantom_file")
if(DEFINED ENV{SHELL})
    set(SHELL "$ENV{SHELL}")
else()
    set(SHELL "sh")
endif()

if(CMAKE_HOST_WIN32)
    # MSYS2 has a broken setup and apparently requires to run shells as login
    # shell in order to get its emulation layers set up properly so that
    # auto(re)conf works properly
    set(SHELLARGS "-l")
else()
    # let's not import login stuff to not pollute nor slow down the build
    set(SHELLARGS "")
endif()

add_custom_target(
    LibRaw ALL
    DEPENDS ${LIBRAW_PHANTOM_FILE} # Ensures target always executes.
)

# Configuration flags.
set(CONFIGURE_FLAGS "--disable-examples")
set(LIBRAW_CXX_FLAGS "${CXX_FLAGS} -std=gnu++11 -Wno-error=unknown-pragmas")
set(LIBRAW_C_FLAGS "${C_FLAGS}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    foreach(arch ${CMAKE_OSX_ARCHITECTURES})
        string(APPEND LIBRAW_CXX_FLAGS " -arch ${arch}")
        string(APPEND LIBRAW_C_FLAGS " -arch ${arch}")
    endforeach()
endif()
# Let the configure script handle OpenMP flags.
string(REPLACE "${OpenMP_CXX_FLAGS}" "" LIBRAW_CXX_FLAGS "${LIBRAW_CXX_FLAGS}")
if(OPTION_OMP)
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} --enable-openmp")
else()
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} --disable-openmp")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(CMAKE_OSX_SYSROOT MATCHES "iPhoneSimulator")
        set(IOS_MIN_FLAG "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(IOS_MIN_FLAG "-miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    string(APPEND LIBRAW_C_FLAGS " -isysroot ${CMAKE_OSX_SYSROOT} ${IOS_MIN_FLAG}")
    string(APPEND LIBRAW_CXX_FLAGS " -isysroot ${CMAKE_OSX_SYSROOT} ${IOS_MIN_FLAG}")
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} --host=aarch64-apple-darwin --disable-shared --enable-static")
endif()
if(ANDROID)
    # Cross build the bundled LibRaw for Android arm64 via the NDK clang.
    # Inject the Android target triple and sysroot so autotools' configure
    # compiles for the device rather than the Windows host, and force a
    # static archive (libraw_r.a) that is linked into libtherapee.so.
    string(APPEND LIBRAW_C_FLAGS " --target=aarch64-linux-android${ANDROID_PLATFORM_LEVEL} --sysroot=${CMAKE_SYSROOT} -fPIC")
    string(APPEND LIBRAW_CXX_FLAGS " --target=aarch64-linux-android${ANDROID_PLATFORM_LEVEL} --sysroot=${CMAKE_SYSROOT} -fPIC")
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} --host=aarch64-linux-android --disable-shared --enable-static")
    # pkg-config on this Windows host resolves zlib/lcms2/jpeg to the msys2
    # mingw64 prefix, which drags in Win32-only headers (_mingw.h) and breaks
    # the Android compile. LibRaw's use of these is optional, and RawTherapee
    # links its own arm64 lcms2/zlib, so disable them in the bundled LibRaw.
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} --disable-lcms --disable-zlib --disable-jpeg --disable-jasper")
    # libtool's configure needs the binutils; point them at the NDK's llvm
    # tools (plain `nm`/`ar` are absent in the msys2 shell used here).
    get_filename_component(_ndk_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} NM=${_ndk_bin}/llvm-nm AR=${_ndk_bin}/llvm-ar RANLIB=${_ndk_bin}/llvm-ranlib STRIP=${_ndk_bin}/llvm-strip")
endif()
set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} CC=\"${CMAKE_C_COMPILER}\"")
set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} CXX=\"${CMAKE_CXX_COMPILER}\"")
set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} CFLAGS=\"${LIBRAW_C_FLAGS}\"")
set(CONFIGURE_FLAGS "${CONFIGURE_FLAGS} CXXFLAGS=\"${LIBRAW_CXX_FLAGS} -DCMS_NO_REGISTER_KEYWORD=1\"")

# Configuration commands.
message(STATUS "Configuring LibRaw")
execute_process(
    COMMAND cp -p -R "${CMAKE_CURRENT_SOURCE_DIR}/libraw" .
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE PROCESS_RESULT
    COMMAND_ECHO STDOUT
)
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not copy LibRaw files into build directory")
endif()

# Homebrew on macOS packages libtool's commands with a "g" prefixed, so
# let's try glibtoolize as fallback.
find_program(LIBTOOLIZE libtoolize glibtoolize REQUIRED)

# Git and autotools don't mix well because Git does not preserve timestamp
# relations or relations between source and generated files, so regenerate:
execute_process(
    COMMAND "${SHELL}" ${SHELLARGS} -c "env LIBTOOLIZE=${LIBTOOLIZE} autoreconf --verbose --install --force"
    WORKING_DIRECTORY "${LIBRAW_DIR}"
    RESULT_VARIABLE PROCESS_RESULT
    COMMAND_ECHO STDOUT
)
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate LibRaw configuration script")
endif()

execute_process(
    COMMAND "${SHELL}" ${SHELLARGS} -c "./configure ${CONFIGURE_FLAGS}"
    WORKING_DIRECTORY "${LIBRAW_DIR}"
    RESULT_VARIABLE PROCESS_RESULT
    COMMAND_ECHO STDOUT
)
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    execute_process(
        COMMAND cat config.log
        WORKING_DIRECTORY "${LIBRAW_DIR}"
        COMMAND_ECHO STDOUT
    )
    message(FATAL_ERROR "LibRaw configure failed")
endif()

# Build flags.
set(LIBRAW_MAKE_FLAGS "")
# Set number of jobs equal to logical processors count.
execute_process(
    COMMAND "${SHELL}" ${SHELLARGS} -c "nproc"
    OUTPUT_VARIABLE LOGICAL_PROCESSORS
    RESULT_VARIABLE PROCESS_RESULT
    ERROR_QUIET
)
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    execute_process(
        COMMAND "${SHELL}" ${SHELLARGS} -c "sysctl -n hw.ncpu"
        OUTPUT_VARIABLE LOGICAL_PROCESSORS
        RESULT_VARIABLE PROCESS_RESULT
        ERROR_QUIET
    )
endif()
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    execute_process(
        COMMAND "${SHELL}" ${SHELLARGS} -c "getconf _NPROCESSORS_ONLN"
        OUTPUT_VARIABLE LOGICAL_PROCESSORS
        RESULT_VARIABLE PROCESS_RESULT
        ERROR_QUIET
    )
endif()
if(PROCESS_RESULT AND NOT PROCESS_RESULT EQUAL 0)
    set(LOGICAL_PROCESSORS "1")
endif()
string(STRIP "${LOGICAL_PROCESSORS}" LOGICAL_PROCESSORS)

# $(MAKE) or ${CMAKE_MAKE_PROGRAM} may be unsuitable for automake-
# generated Makefile sets, for instance, if MAKE is nmake.
# It might also be unset, or ninja.

# We can't solve this easily in cmake itself. Specifically, we cannot use
# anything that might leave a $ behind, f.i.$(MAKE),
# because that breaks Ninja due to long-standing bug in cmake,
# unfixed as of 2025-07-28, which fails to escape $ properly for Ninja,
# see <https://gitlab.kitware.com/cmake/cmake/-/issues/18062>.
# Work around this cmake bug by using a shell helper script instead,
# which parses the make's name and the number of processors we pass it,
# and passes remaining flags through (we don't have any at the moment):
set(LIBRAW_MAKE_COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/LibRaw-make.sh")

# Build commands.
add_custom_command(
    OUTPUT "${LIBRAW_PHANTOM_FILE}" "${LIBRAW_LIB_DIR}/.libs/libraw_r.a"
    COMMAND cp -p -R "${CMAKE_CURRENT_SOURCE_DIR}/libraw" ..
    COMMAND "${SHELL}" "${LIBRAW_MAKE_COMMAND}" "${CMAKE_MAKE_PROGRAM}" "${LOGICAL_PROCESSORS}" ${LIBRAW_MAKE_FLAGS}
    COMMENT "Building LibRaw"
    WORKING_DIRECTORY libraw
    VERBATIM
)

# make sure that "make clean-libraw && make" works:
add_dependencies(rtengine LibRaw)

# Add a `make clean-libraw` command because there's no good way to automatically
# clean the LibRaw build with `make clean`.
# Also, strip the recursive-make environment variables because if cmake runs with
# a different make that's incompatible with "make", older MFLAGS or current MAKEFLAGS
# might break the build. GNU make doesn't like BSD's -J option, and BSD
# requires a number after -j and might not cope with --jobserver in MAKEFLAGS.
add_custom_target(
    clean-libraw
    COMMAND cmake -E env --unset=MFLAGS --unset=MAKEFLAGS make clean
    COMMAND rm -rf lib
    WORKING_DIRECTORY libraw
)
