@echo off
:: =============================================================================
:: build_gui.bat — 在 Windows 上启动 MSYS2 ucrt64 环境并编译 RawTherapee GUI
::
:: 【推荐运行方式（Windows 首选）】
::   直接双击本文件，或在 CMD / PowerShell 中运行：
::     > build_gui.bat
::     > build_gui.bat --jobs 4
::     > build_gui.bat --skip-build
::     > build_gui.bat --clean
::     > build_gui.bat --run
::
::   本脚本会自动：
::     1. 定位 MSYS2 安装目录（C:\msys64）
::     2. 设置 ucrt64 工具链 PATH（/ucrt64/bin）
::     3. 调用 build_gui.sh 完成编译和资源收集
::
:: 【常用参数】
::   --jobs N        并行编译线程数（默认 2，内存不足时改为 1）
::   --skip-build    跳过编译，仅重新收集产物到 output_gui/
::   --clean         清除 build 目录后重新全量编译
::   --run           构建完成后自动启动 rawtherapee.exe
::
:: 【注意事项】
::   - 若 ninja/cmake 报错找不到，说明 build_gui.sh 中 PATH 未包含 /ucrt64/bin，
::     本脚本已通过 "export PATH=/ucrt64/bin:/usr/bin:$PATH" 解决此问题。
::   - 不要直接在 MSYS2 默认（MSYS）终端运行 build_gui.sh，
::     必须使用 MSYS2 UCRT64 终端或通过本 bat 文件调用。
::
:: 【前提条件】
::   已安装 MSYS2，默认路径 C:\msys64（如不同请修改 MSYS2_ROOT）
::   ucrt64 工具链已安装相关依赖（cmake / ninja / gtk3 / exiv2 等）
::
:: 【产物位置】
::   installer\output_gui\rawtherapee.exe
:: =============================================================================

setlocal

:: ---------- 查找 MSYS2 ----------
set MSYS2_ROOT=C:\msys64
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
    echo [ERROR] MSYS2 not found at %MSYS2_ROOT%
    echo Please install MSYS2 from https://www.msys2.org/
    pause
    exit /b 1
)

:: ---------- 当前脚本目录 ----------
set SCRIPT_DIR=%~dp0
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%

:: ---------- 将 Windows 路径转为 MSYS2 路径 ----------
set MSYS_SCRIPT_DIR=%SCRIPT_DIR:\=/%
set DRIVE=%MSYS_SCRIPT_DIR:~0,1%
for %%i in (a b c d e f g h i j k l m n o p q r s t u v w x y z) do (
    if /I "%DRIVE%"=="%%i" set DRIVE=%%i
)
set MSYS_SCRIPT_DIR=/%DRIVE%/%MSYS_SCRIPT_DIR:~3%

:: ---------- 拼接传递给 build_gui.sh 的参数 ----------
set EXTRA_ARGS=%*

:: ---------- 执行 ----------
echo Starting RawTherapee GUI build via MSYS2 ucrt64 ...
echo Script: %SCRIPT_DIR%\build_gui.sh
echo.

"%MSYS2_ROOT%\usr\bin\bash.exe" -l -c ^
    "export PATH=/ucrt64/bin:/usr/bin:$PATH; bash '%MSYS_SCRIPT_DIR%/build_gui.sh' %EXTRA_ARGS%"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build returned error %ERRORLEVEL%
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Output is at: %SCRIPT_DIR%\output_gui\
echo Run: %SCRIPT_DIR%\output_gui\rawtherapee.exe
pause
endlocal
