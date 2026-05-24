@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD_DIR=%SCRIPT_DIR%\build\vs2022"
set "CMAKE_EXE="
set "GENERATOR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL_DIR="
set "VS_VERSION="
set "VS_MAJOR="

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL_DIR=%%I"
    )

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do (
        set "VS_VERSION=%%I"
    )
)

for /f "tokens=1 delims=." %%I in ("%VS_VERSION%") do (
    set "VS_MAJOR=%%I"
)

if "%VS_MAJOR%"=="18" (
    set "GENERATOR=Visual Studio 18 2026"
    set "BUILD_DIR=%SCRIPT_DIR%\build\vs2026"
)

if "%VS_MAJOR%"=="17" (
    set "GENERATOR=Visual Studio 17 2022"
)

if not defined GENERATOR (
    set "GENERATOR=Visual Studio 17 2022"
)

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

where cmake >nul 2>nul
if not errorlevel 1 (
    set "CMAKE_EXE=cmake"
)

if not defined CMAKE_EXE if defined VS_INSTALL_DIR if exist "%VS_INSTALL_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%VS_INSTALL_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
)

if not defined CMAKE_EXE (
    echo CMake was not found. Install CMake or Visual Studio's C++ CMake tools, then rerun this script.
    exit /b 1
)

if defined VS_INSTALL_DIR (
    "%CMAKE_EXE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -A x64 "-DCMAKE_GENERATOR_INSTANCE:PATH=%VS_INSTALL_DIR%"
) else (
    "%CMAKE_EXE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -A x64
)
if errorlevel 1 (
    exit /b %errorlevel%
)

echo Generated Visual Studio solution:
echo   %BUILD_DIR%\DagorAssetsExtractor.sln

endlocal
