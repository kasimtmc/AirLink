#!/usr/bin/env bash
# Cross-compiles the Windows release with MinGW-w64 (GCC 14 or newer: Kompoz doesn't build with 13)
# and packages it as the portable zip in dist/ (with ANGLE, and the packaged install's files for
# call audio); windows-installer.sh makes the installer from it. Kompoz at ../kompoz with scripts/fetch-deps.sh run.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/app/build-windows-release}"
version="$(cat "$root/VERSION")"
dist="$root/dist"
mkdir -p "$dist"
cmake -S "$root/app" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$root/../kompoz/cmake/mingw-w64-x86_64.cmake"
cmake --build "$build"
x86_64-w64-mingw32-strip "$build/airlink.exe"

zip="$dist/AirLink-$version-windows-x64.zip"
AIRLINK_WINDOWS_BUILD="$build" python3 "$root/app/packaging/windows/package.py" "$zip"

ls -la "$zip"
