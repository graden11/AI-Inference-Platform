@echo off
setlocal

set MODE=%1
if "%MODE%"=="" set MODE=cpu

echo === Stopping Kama-HTTPServer (%MODE%) ===

set "WINPATH=%CD:\=/%"
for /f "delims=" %%i in ('wsl wslpath -a "%WINPATH%" 2^>nul') do set "WSLDIR=%%i"
if "%WSLDIR%"=="" (
    echo Error: Cannot determine WSL path.
    exit /b 1
)

wsl -e bash -c "cd '%WSLDIR%' && bash stop.sh %MODE%"
echo Done.
