@echo off
setlocal

rem Build the gateway and native plugins with the same Win32 architecture.
rem The default build.bat configuration is Win32 because the optional ZK
rem PullSDK is 32-bit; this prevents a plugin DLL from being incompatible with
rem the gateway that loads it.
set "ROOT=%~dp0.."
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

call "%ROOT%\build.bat" %CFG%
if errorlevel 1 exit /b 1

cmake --build "%ROOT%\build-win" --config %CFG% --target hsf_driver_modbus hsf_driver_c3protocol
if errorlevel 1 exit /b 1

if not exist "%ROOT%\build-win\plugins\modbus" mkdir "%ROOT%\build-win\plugins\modbus"
if not exist "%ROOT%\build-win\plugins\c3protocol" mkdir "%ROOT%\build-win\plugins\c3protocol"
copy /Y "%ROOT%\build-win\plugins\modbus\%CFG%\plugin.dll" "%ROOT%\build-win\plugins\modbus\plugin.dll" >nul
copy /Y "%ROOT%\plugins\modbus\manifest.json" "%ROOT%\build-win\plugins\modbus\manifest.json" >nul
copy /Y "%ROOT%\build-win\plugins\c3protocol\%CFG%\plugin.dll" "%ROOT%\build-win\plugins\c3protocol\plugin.dll" >nul
copy /Y "%ROOT%\plugins\c3protocol\manifest.json" "%ROOT%\build-win\plugins\c3protocol\manifest.json" >nul
echo [plugins] staged under %ROOT%\build-win\plugins
endlocal
