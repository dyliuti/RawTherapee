@echo off

echo ===== Starting LibRAWEngine Build =====

set MSYS2_PATH=C:\msys64
set PATH=%MSYS2_PATH%\mingw64\bin;%MSYS2_PATH%\usr\bin;%PATH%

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR:~0,-1%
set BUILD_DIR=%PROJECT_DIR%/build

echo Project Directory: %PROJECT_DIR%
echo Build Directory: %BUILD_DIR%

if exist "%BUILD_DIR%" (
    echo Cleaning build directory...
    rmdir /s /q "%BUILD_DIR%"
)

mkdir "%BUILD_DIR%"
if not errorlevel 1 (
    echo Build directory created
) else (
    echo ERROR: Failed to create build directory
    exit /b 1
)

echo Configuring project with CMake...
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_C_COMPILER=%MSYS2_PATH%/mingw64/bin/gcc.exe ^
    -DCMAKE_CXX_COMPILER=%MSYS2_PATH%/mingw64/bin/g++.exe ^
    -S "%PROJECT_DIR%" ^
    -B "%BUILD_DIR%" ^
    -G Ninja

if not errorlevel 1 (
    echo CMake configuration successful
) else (
    echo ERROR: CMake configuration failed
    exit /b 1
)

echo Building project...
cmake --build "%BUILD_DIR%" --config RelWithDebInfo  

if not errorlevel 1 (
    echo Build successful
) else (
    echo ERROR: Build failed
    exit /b 1
)

echo ===== Build completed successfully =====

echo Creating output directory and copying files...

set OUTPUT_DIR=%PROJECT_DIR%\output
if exist "%OUTPUT_DIR%" rmdir /s /q "%OUTPUT_DIR%"
mkdir "%OUTPUT_DIR%"
echo Copying files...
if exist "%PROJECT_DIR%\resources" xcopy "%PROJECT_DIR%\resources" "%OUTPUT_DIR%\resources" /E /I /Y

mkdir "%OUTPUT_DIR%\bin\"
if exist "%BUILD_DIR%\RAWEngine\libRAWEngine.dll" copy "%BUILD_DIR%\RAWEngine\libRAWEngine.dll" "%OUTPUT_DIR%\bin\" /Y

:: 复制 mingw64 运行时 DLL（libRAWEngine.dll 的依赖）
set MSYS2_BIN=%MSYS2_PATH%\mingw64\bin
echo Copying mingw64 runtime DLLs...
for %%F in (
    libcairomm-1.0-1.dll
    libexpat-1.dll
    libfftw3f-3.dll
    libfftw3f_omp-3.dll
    libgcc_s_seh-1.dll
    libgio-2.0-0.dll
    libgiomm-2.4-1.dll
    libglib-2.0-0.dll
    libglibmm-2.4-1.dll
    libgobject-2.0-0.dll
    libgomp-1.dll
    liblcms2-2.dll
    liblensfun.dll
    libwinpthread-1.dll
    libsigc-2.0-0.dll
    libstdc++-6.dll
    libexiv2-27.dll
    libjpeg-8.dll
    libpng16-16.dll
    libtiff-6.dll
    zlib1.dll
    libcairo-2.dll
    libfontconfig-1.dll
    libfreetype-6.dll
    libgmodule-2.0-0.dll
    libgthread-2.0-0.dll
    libharfbuzz-0.dll
    libiconv-2.dll
    libintl-8.dll
    libpango-1.0-0.dll
    libpangocairo-1.0-0.dll
    libpangowin32-1.0-0.dll
    libpangoft2-1.0-0.dll
    libpcre2-8-0.dll
    libpixman-1-0.dll
    libffi-8.dll
    libbz2-1.dll
    libbrotlidec.dll
    libbrotlicommon.dll
    libgraphite2.dll
    libdeflate.dll
    libjbig-0.dll
    libLerc.dll
    liblzma-5.dll
    libwebp-7.dll
    libsharpyuv-0.dll
    libzstd.dll
    libinih-0.dll
    libINIReader-0.dll
    libcurl-4.dll
    libcrypto-3-x64.dll
    libssl-3-x64.dll
    libssh2-1.dll
    libidn2-0.dll
    libnghttp2-14.dll
    libnghttp3-9.dll
    libngtcp2-16.dll
    libngtcp2_crypto_ossl-0.dll
    libpsl-5.dll
    libsystre-0.dll
    libtre-5.dll
    libunistring-5.dll
) do (
    if exist "%MSYS2_BIN%\%%F" (
        copy /Y "%MSYS2_BIN%\%%F" "%OUTPUT_DIR%\bin\" >nul 2>&1
    )
)

mkdir "%OUTPUT_DIR%\lib\"
:: 生成 .lib 文件
echo Generating .lib file from .dll...
cd "%BUILD_DIR%\RAWEngine"
gendef .\libRAWEngine.dll
lib /def:libRAWEngine.def /out:libRAWEngine.lib /machine:x64
if exist "%BUILD_DIR%\RAWEngine\libRAWEngine.lib" copy "%BUILD_DIR%\RAWEngine\libRAWEngine.lib" "%OUTPUT_DIR%\lib\" /Y
cd "%PROJECT_DIR%"

mkdir "%OUTPUT_DIR%\static\"
copy "%BUILD_DIR%\RAWEngine\libraw\lib\.libs\libraw_r.a" "%OUTPUT_DIR%\static\" /Y

mkdir "%OUTPUT_DIR%\include\"
copy "%PROJECT_DIR%\RAWEngine\raw_engine.h" "%OUTPUT_DIR%\include\" /Y
copy "%PROJECT_DIR%\RAWEngine\rawengine_types.h" "%OUTPUT_DIR%\include\" /Y
xcopy "%BUILD_DIR%\RAWEngine\libraw\libraw" "%OUTPUT_DIR%\include\libraw" /E /I /Y

mkdir "%OUTPUT_DIR%\pdb\"
%MSYS2_PATH%\cv2pdb\cv2pdb.exe "%BUILD_DIR%\RAWEngine\libRAWEngine.dll"
if exist "%BUILD_DIR%\RAWEngine\libRAWEngine.pdb" copy "%BUILD_DIR%\RAWEngine\libRAWEngine.pdb" "%OUTPUT_DIR%\pdb\" /Y

echo ===== All operations completed =====
echo Output directory: %OUTPUT_DIR%
