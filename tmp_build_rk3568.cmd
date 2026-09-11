@echo off
docker volume create hsf-gateway-ubuntu-build-linux-arm64 >nul
docker run --rm -t --platform linux/arm64 -v "%CD%:/src" -v hsf-gateway-ubuntu-build-linux-arm64:/work -e VCPKG_DEFAULT_BINARY_CACHE=/work/vcpkg-cache -e VCPKG_FORCE_SYSTEM_BINARIES=1 -e DEBIAN_FRONTEND=noninteractive -w /src ubuntu:18.04 bash -lc "set -e; mkdir -p /work/vcpkg-cache; VCPKG_ROOT=/work/vcpkg scripts/build-ubuntu.sh --build-dir /work/build --jobs 4 --no-tests; mkdir -p /src/build-ubuntu-arm64; cp /work/build/hsf_gateway /src/build-ubuntu-arm64/; cp /work/build/BUILD_INFO.txt /src/build-ubuntu-arm64/ 2>/dev/null || true"
exit /b %ERRORLEVEL%
