@echo off
premake5.exe vs2022
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Project generation failed!
    pause
    exit /b %ERRORLEVEL%
)
echo.
echo Projects generated successfully!
