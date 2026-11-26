#!/usr/bin/env bash
# The packages a Linux release build needs on Ubuntu 24.04 (the CI runner, or a container of it).
# Older bases lack GCC 14, which Kompoz needs; a build here runs on glibc 2.39 and newer.
set -euo pipefail
sudo=""; [[ $(id -u) -ne 0 ]] && sudo=sudo
$sudo apt-get update
$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential g++-14 cmake ninja-build pkg-config git curl ca-certificates file patchelf desktop-file-utils \
    libwayland-dev libxkbcommon-dev libx11-dev libxi-dev libxcursor-dev libxrandr-dev libvulkan-dev \
    libturbojpeg0-dev libwebp-dev libcurl4-openssl-dev libdbus-1-dev \
    libpipewire-0.3-dev libpulse-dev libssl-dev libclang-dev dpkg-dev fakeroot
