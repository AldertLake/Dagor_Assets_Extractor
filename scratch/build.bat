@echo off
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found!
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -property installationPath`) do (
    set "VS_INSTALL_DIR=%%I"
)
if not defined VS_INSTALL_DIR (
    echo Visual Studio installation directory not found!
    exit /b 1
)

set "MSBUILD_EXE=%VS_INSTALL_DIR%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD_EXE%" (
    echo MSBuild not found at: %MSBUILD_EXE%
    exit /b 1
)

echo Building project using %MSBUILD_EXE%...
"%MSBUILD_EXE%" "build\vs2026\DagorAssetsExtractorValidationTests.vcxproj" /p:Configuration=Debug
