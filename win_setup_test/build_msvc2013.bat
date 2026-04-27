@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DMX_qt MSVC 2013 Build Script
:: ============================================================

:: 1. Define Paths
set "QT_BIN=E:\.trae\qt5.4.2\5.4\msvc2013_64_opengl\bin"
set "PROJECT_ROOT=%~dp0.."

:: 2. Find MSVC 2013 vcvarsall.bat
set "VS2013_PATH_1=C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
set "VS2013_PATH_2=D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"

if exist "%VS2013_PATH_1%" (
    set "VCVARS=%VS2013_PATH_1%"
) else if exist "%VS2013_PATH_2%" (
    set "VCVARS=%VS2013_PATH_2%"
) else (
    echo [ERROR] MSVC 2013 not found.
    echo Please install Visual Studio 2013 or MSVC 2013 Build Tools.
    echo See win_setup_test\README_WIN.md for download links.
    pause
    exit /b 1
)

:: 3. Initialize MSVC 2013 x64 environment
echo [INFO] Initializing MSVC 2013 x64 environment...
call "%VCVARS%" amd64

:: 4. Run qmake
echo [INFO] Running qmake...
cd /d "%PROJECT_ROOT%"
"%QT_BIN%\qmake.exe" -spec win32-msvc2013 untitled1.pro

if %ERRORLEVEL% neq 0 (
    echo [ERROR] qmake failed.
    pause
    exit /b %ERRORLEVEL%
)

:: 5. Run nmake
echo [INFO] Running nmake...
nmake

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    pause
    exit /b %ERRORLEVEL%
)

echo [SUCCESS] Project built successfully!
echo The executable should be in the 'release' or 'debug' folder.
pause
