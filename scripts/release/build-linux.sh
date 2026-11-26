#!/usr/bin/env bash
# Builds the Linux release binary: build-linux.sh [build dir] (default app/build-release).
# Kompoz must be at ../kompoz (at KOMPOZ_REF) with its vendored dependencies fetched
# (scripts/fetch-deps.sh): FreeType, HarfBuzz, zlib and libpng are linked in statically.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/app/build-release}"
export CC="${CC:-gcc-14}" CXX="${CXX:-g++-14}"
cmake -S "$root/app" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKOMPOZ_VENDORED_DEPS=ON \
    -DKOMPOZ_WITH_JPEG=ON -DKOMPOZ_WITH_WEBP=ON -DKOMPOZ_WITH_CURL=ON
cmake --build "$build"
strip "$build/airlink"
echo "$build/airlink"
