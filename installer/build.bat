@echo off
:: =============================================================================
:: build.bat — 在 Windows 上启动 MSYS2 ucrt64 环境并执行 build.sh
::
:: 用法（直接双击或在 cmd 中运行）:
::   build.bat [--jobs N] [--skip-build] [--clean]
::
:: 前提条件:
::   已安装 MSYS2，默认路径 C:\msys64
::   ucrt64 工具链已安装相关依赖（运行一次后会自动检查）
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
:: 去掉末尾反斜杠
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%

:: ---------- 将 Windows 路径转为 MSYS2 路径 ----------
:: 例: C:\project\raw\RawTherapee\installer -> /c/project/raw/RawTherapee/installer
set MSYS_SCRIPT_DIR=%SCRIPT_DIR:\=/%
set DRIVE=%MSYS_SCRIPT_DIR:~0,1%
:: 小写盘符
for %%i in (a b c d e f g h i j k l m n o p q r s t u v w x y z) do (
    if /I "%DRIVE%"=="%%i" set DRIVE=%%i
)
set MSYS_SCRIPT_DIR=/%DRIVE%/%MSYS_SCRIPT_DIR:~3%

:: ---------- 拼接传递给 build.sh 的参数 ----------
set EXTRA_ARGS=%*

:: ---------- 执行 ----------
echo Starting build via MSYS2 ucrt64 ...
echo Script: %SCRIPT_DIR%\build.sh
echo.

"%MSYS2_ROOT%\usr\bin\bash.exe" -l -c ^
    "export PATH=/ucrt64/bin:/usr/bin:$PATH; bash '%MSYS_SCRIPT_DIR%/build.sh' %EXTRA_ARGS%"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Build or collect step returned error %ERRORLEVEL%
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo [SUCCESS] Output is at: %SCRIPT_DIR%\output\
pause
endlocal
