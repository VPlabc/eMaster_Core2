@echo off
setlocal EnableDelayedExpansion

rem Reproducible Windows x86 release package for deployment. x86 is required
rem when the target uses ZKTeco's 32-bit PullSDK (plcommpro.dll).
set "ROOT=%~dp0"
set "WINAPPS=%LOCALAPPDATA%\Microsoft\WindowsApps"
rem Prevent vcpkg from selecting the inaccessible WindowsApps pwsh shim.
set "PATH=!PATH:%WINAPPS%;=!"
set "PATH=!PATH:;%WINAPPS%=!"

echo [release] Building and packaging Windows x86 Release...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT%scripts\package.ps1" -Arch x86
if errorlevel 1 (
    echo [release] Build/package failed.
    exit /b 1
)

echo [release] Deployment package is in "%ROOT%dist"
endlocal
