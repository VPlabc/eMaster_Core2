@echo off
setlocal

rem eMaster Gateway startup launcher.
rem Place a shortcut to this file in shell:startup to start the gateway at
rem Windows sign-in. It supports both an extracted release and this checkout.

set "ROOT=%~dp0"
set "GATEWAY=%ROOT%bin\hsf_gateway.exe"

if not exist "%GATEWAY%" set "GATEWAY=%ROOT%build-release-x86\Release\hsf_gateway.exe"
if not exist "%GATEWAY%" set "GATEWAY=%ROOT%build-win\Release\hsf_gateway.exe"

if not exist "%GATEWAY%" (
    echo [autostart] ERROR: hsf_gateway.exe was not found.
    exit /b 1
)

if not exist "%ROOT%config\config.db" (
    echo [autostart] ERROR: config\config.db was not found.
    exit /b 1
)

rem /d ensures all relative config, log, and script paths resolve from the
rem gateway installation rather than Windows' Startup directory.
start "eMaster Gateway" /min /d "%ROOT%" "%GATEWAY%" "%ROOT%config\config.db"

endlocal
