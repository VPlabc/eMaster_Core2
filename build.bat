@echo off
setlocal enabledelayedexpansion

rem Auto build script for Windows (vcpkg + CMake + MSVC).
rem Usage: build.bat [Debug|Release]   (default: Release)

set "ROOT=%~dp0"
set "VCPKG_DIR=%ROOT%build\vcpkg"
set "BUILD_DIR=%ROOT%build-win"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

rem WindowsApps contains execution-alias shims (notably pwsh.exe) that can
rem fail under vcpkg's child-process detection. Remove that shim directory
rem before CMake/vcpkg choose host tools; vcpkg can then use its cached tools
rem or download the pinned PowerShell Core version normally.
set "WINAPPS=%LOCALAPPDATA%\Microsoft\WindowsApps"
set "PATH=!PATH:%WINAPPS%;=!"
set "PATH=!PATH:;%WINAPPS%=!"

rem Capture %ProgramFiles(x86)% into a plain var *before* any parenthesized
rem block below -- its own literal "(x86)" breaks cmd's paren-block parsing
rem if referenced directly inside an if/else (...) block.
set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=%ProgramFiles%"

rem --- Locate cmake: prefer PATH, else fall back to the copy bundled with VS Build Tools ---
where cmake >nul 2>nul
if %ERRORLEVEL%==0 (
    set "CMAKE=cmake"
) else (
    set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [build.bat] ERROR: cmake not found on PATH, and vswhere.exe not found to locate a bundled copy.
        echo [build.bat] Install CMake ^(cmake.org^), or install Visual Studio Build Tools with the C++ workload.
        exit /b 1
    )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo [build.bat] ERROR: No Visual Studio installation with the C++ ^(VC.Tools.x86.x64^) workload was found.
        echo [build.bat] Install "Desktop development with C++" in Visual Studio / Build Tools.
        exit /b 1
    )
    set "CMAKE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE!" (
        echo [build.bat] ERROR: Expected bundled cmake.exe not found at "!CMAKE!".
        exit /b 1
    )
)

echo [build.bat] Using cmake: !CMAKE!

rem --- Bootstrap vcpkg if needed ---
if not exist "%VCPKG_DIR%\vcpkg.exe" (
    if not exist "%VCPKG_DIR%\bootstrap-vcpkg.bat" (
        echo [build.bat] ERROR: "%VCPKG_DIR%" doesn't look like a vcpkg checkout ^(no bootstrap-vcpkg.bat^).
        echo [build.bat] Clone it first: git clone https://github.com/microsoft/vcpkg "%VCPKG_DIR%"
        exit /b 1
    )
    echo [build.bat] Bootstrapping vcpkg...
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 exit /b 1
)

rem --- Configure ---
rem x86, not x64: src/zk_controller links plcommpro.dll, a 32-bit-only DLL
rem (request/HSF_Machine_ZK_Controller_Lua_Integration.md section 12) --
rem CMakeLists.txt's own guard will FATAL_ERROR if this ever drifts back to x64.
echo [build.bat] Configuring into "%BUILD_DIR%" ^(first run installs vcpkg deps from source - can take 15-30+ min^)...
rem WindowsApps may expose a broken pwsh.exe shim to vcpkg. Pin the
rem PowerShell implementation used by the toolchain to the system binary.
set "VCPKG_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
"!CMAKE!" --fresh -S "%ROOT%." -B "%BUILD_DIR%" -A Win32 -DVCPKG_TARGET_TRIPLET=x86-windows -DZ_VCPKG_POWERSHELL_PATH="%VCPKG_POWERSHELL%" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake"
if errorlevel 1 (
    echo [build.bat] Configure failed.
    exit /b 1
)

rem --- Build ---
echo [build.bat] Building ^(%CFG%^)...
"!CMAKE!" --build "%BUILD_DIR%" --config %CFG%
if errorlevel 1 (
    echo [build.bat] Build failed.
    exit /b 1
)

rem --- Build the additive Rust Core workspace -------------------------------
rem The Rust crates are currently a standalone workspace; this produces
rem target\release artifacts without replacing the native gateway executable.
where cargo >nul 2>nul
if errorlevel 1 (
    echo [build.bat] ERROR: cargo not found on PATH. Install Rust via rustup.
    exit /b 1
)
echo [build.bat] Building Rust Core workspace (Release)...
pushd "%ROOT%"
cargo build --workspace --release
if errorlevel 1 (
    popd
    echo [build.bat] Rust Core build failed.
    exit /b 1
)
popd
echo [build.bat] Rust Core build succeeded: %ROOT%target\release

echo [build.bat] Build succeeded: %BUILD_DIR%\%CFG%\hsf_gateway.exe
endlocal
