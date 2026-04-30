@echo off
echo ===== Building RAWEngine Qt Demo (MSVC + Qt6) =====

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR:~0,-1%
set BUILD_DIR=%PROJECT_DIR%\build
set DIST_DIR=%PROJECT_DIR%\dist
set RAWENGINE_BUILD=%PROJECT_DIR%\..\build\RAWEngine
set MSYS2_BIN=C:\msys64\mingw64\bin

:: Qt6 路径
set QT_DIR=C:\Qt\6.5.3\msvc2019_64

echo Project Directory:    %PROJECT_DIR%
echo Build Directory:      %BUILD_DIR%
echo Dist Directory:       %DIST_DIR%
echo RAWEngine Build:      %RAWENGINE_BUILD%
echo Qt Directory:         %QT_DIR%
echo.

:: ====== 1. 编译 ======
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"

echo [1/4] Configuring with CMake (MSVC)...
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    -S "%PROJECT_DIR%" ^
    -B "%BUILD_DIR%"

if errorlevel 1 (
    echo ERROR: CMake configuration failed
    echo 提示: 请从 "x64 Native Tools Command Prompt for VS" 运行此脚本
    exit /b 1
)

echo [2/4] Building...
cmake --build "%BUILD_DIR%" --config RelWithDebInfo

if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

:: ====== 2. 创建 dist 目录 ======
echo.
echo [3/4] Creating dist directory...
if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%"
mkdir "%DIST_DIR%"

:: 复制 exe
set EXE_SRC=%BUILD_DIR%\RelWithDebInfo\rawengine_demo_qt.exe
if not exist "%EXE_SRC%" set EXE_SRC=%BUILD_DIR%\rawengine_demo_qt.exe
copy /Y "%EXE_SRC%" "%DIST_DIR%\"

:: 复制 libRAWEngine.dll
copy /Y "%RAWENGINE_BUILD%\libRAWEngine.dll" "%DIST_DIR%\"

:: 复制 resources（引擎依赖的配置、色彩配置文件、lensfun 数据库等）
echo Copying resources...
xcopy "%RAWENGINE_BUILD%\resources" "%DIST_DIR%\resources" /E /I /Y /Q

:: 复制 mingw64 运行时 DLL（libRAWEngine.dll 的依赖）
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
        copy /Y "%MSYS2_BIN%\%%F" "%DIST_DIR%\" >nul 2>&1
    )
)

:: ====== 3. 部署 Qt DLL ======
echo [4/4] Deploying Qt runtime (windeployqt)...
"%QT_DIR%\bin\windeployqt.exe" "%DIST_DIR%\rawengine_demo_qt.exe" --no-translations --no-opengl-sw

echo.
echo ===== All Done =====
echo.
echo Dist directory: %DIST_DIR%
echo.
echo 运行方式:
echo   cd %DIST_DIR%
echo   rawengine_demo_qt.exe
echo.
echo 或者直接双击: %DIST_DIR%\rawengine_demo_qt.exe
