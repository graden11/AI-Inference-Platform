@echo off
setlocal

set ARGS=%*
if "%ARGS%"=="" set ARGS=cpu

echo === Kama-HTTPServer One-Click Deploy ===
echo.

REM Convert Windows path to WSL path (replace \ with / then use wslpath)
set "WINPATH=%CD:\=/%"
for /f "delims=" %%i in ('wsl wslpath -a "%WINPATH%" 2^>nul') do set "WSLDIR=%%i"
if "%WSLDIR%"=="" (
    echo Error: Cannot determine WSL path. Is WSL installed?
    echo Install: wsl --install
    exit /b 1
)

wsl -e bash -c "cd '%WSLDIR%' && bash start.sh %ARGS%"

if %ERRORLEVEL% equ 0 (
    echo.
    echo Opening http://localhost/ ...
    start http://localhost/
)
