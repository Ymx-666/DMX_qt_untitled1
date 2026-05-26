@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DMX_qt MSVC 2013 Build Script
:: ============================================================

:: 1. Define Paths
set "PROJECT_ROOT=%~dp0.."
if not defined QT_BIN set "QT_BIN="

if not "%QT_BIN%"=="" goto :qt_ok
if exist "E:\.trae\Qt\5.4\msvc2013_64\bin\qmake.exe" set "QT_BIN=E:\.trae\Qt\5.4\msvc2013_64\bin"
if exist "E:\qt5.4\5.4\msvc2013_64\bin\qmake.exe" set "QT_BIN=E:\qt5.4\5.4\msvc2013_64\bin"
if exist "E:\Qt\5.4\msvc2013_64\bin\qmake.exe" set "QT_BIN=E:\Qt\5.4\msvc2013_64\bin"
if exist "C:\Qt\Qt5.4.2\5.4\msvc2013_64\bin\qmake.exe" set "QT_BIN=C:\Qt\Qt5.4.2\5.4\msvc2013_64\bin"

:qt_ok
if "%QT_BIN%"=="" (
    echo [ERROR] Qt 5.4 MSVC 2013 x64 kit not found.
    echo Please install Qt 5.4 MSVC2013 64-bit kit, then set QT_BIN to its bin folder.
    echo Example: set QT_BIN=E:\Qt\5.4\msvc2013_64\bin
    pause
    exit /b 1
)
echo [INFO] QT_BIN=%QT_BIN%

:: 2. Find MSVC 2013 vcvarsall.bat
set "VS2013_PATH_1=E:\.trae\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
set "VS2013_PATH_2=C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
set "VS2013_PATH_3=D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"

if exist "%VS2013_PATH_1%" (
    set "VCVARS=%VS2013_PATH_1%"
) else if exist "%VS2013_PATH_2%" (
    set "VCVARS=%VS2013_PATH_2%"
) else if exist "%VS2013_PATH_3%" (
    set "VCVARS=%VS2013_PATH_3%"
) else (
    echo [ERROR] MSVC 2013 not found.
    echo Please install Visual Studio 2013 or MSVC 2013 Build Tools.
    echo See win_setup_test\README_WIN.md for download links.
    pause
    exit /b 1
)
echo [INFO] VCVARS=%VCVARS%

:: 3. Initialize MSVC 2013 x64 environment
echo [INFO] Initializing MSVC 2013 x64 environment...
call "%VCVARS%" amd64
set "PATH=%QT_BIN%;%PATH%"
if not defined OPENCV_INSTALL (
    if exist "%PROJECT_ROOT%\win_setup_test\3rdparty\opencv\build\include" set "OPENCV_INSTALL=%PROJECT_ROOT%\win_setup_test\3rdparty\opencv\build"
)
if not defined FFMPEG_PATH (
    if exist "%PROJECT_ROOT%\win_setup_test\3rdparty\ffmpeg\include" set "FFMPEG_PATH=%PROJECT_ROOT%\win_setup_test\3rdparty\ffmpeg"
)

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
