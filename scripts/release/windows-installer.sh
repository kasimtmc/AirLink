#!/usr/bin/env bash
# The per-user Windows installer from the portable zip (build-windows.sh): needs makensis (NSIS).
#   windows-installer.sh dist/AirLink-<version>-windows-x64.zip
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
version="$(cat "$root/VERSION")"
zip="$(realpath "${1:-$root/dist/AirLink-$version-windows-x64.zip}")"
# Absolute: makensis works in the .nsi's folder.
out="$(dirname "$zip")/AirLink-$version-windows-x64-setup.exe"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
python3 -m zipfile -e "$zip" "$stage"
makensis -V2 -DVERSION="$version" -DSOURCE="$stage/AirLink" -DOUTFILE="$out" "$root/app/packaging/windows/installer.nsi"
ls -la "$out"
