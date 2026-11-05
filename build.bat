@echo off
rem Double-click to build everything (PC host + Switch app). Runs build.sh with bash.
setlocal
cd /d "%~dp0"

rem Windows can't overwrite a running exe: close the bridge programs first.
for %%E in (ebridge_host_cli.exe eglantine_bridge.exe) do (
    tasklist /FI "IMAGENAME eq %%E" | find /I "%%E" >nul
    if not errorlevel 1 (
        echo Closing %%E so it can be rebuilt...
        taskkill /IM %%E /F >nul
        ping -n 2 127.0.0.1 >nul
    )
)

set "BASH=C:\Program Files\Git\bin\bash.exe"
if not exist "%BASH%" set "BASH=C:\msys64\usr\bin\bash.exe"
if not exist "%BASH%" (
    echo ERROR: bash not found. Install Git for Windows or MSYS2 in C:\msys64.
    pause
    exit /b 1
)

"%BASH%" ./build.sh
if errorlevel 1 (
    echo.
    echo *** Build FAILED: see the error above. ***
) else (
    echo.
    echo *** Done. Copy switch-client\eglantine-bridge.nro to the SD card, or build the NSP with make_nsp.sh. ***
)
pause
