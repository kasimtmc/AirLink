#!/usr/bin/env python3
"""Package the Windows build with ANGLE for VMs and Remote Desktop sessions."""

import sys
from hashlib import sha256
from pathlib import Path
from urllib.request import urlopen
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[3]
import os
BUILD = Path(os.environ.get("AIRLINK_WINDOWS_BUILD", ROOT / "app/build-windows"))
ANGLE_TAG = "v2.1.28252"
ANGLE_NAME = f"angle-windows-x64-{ANGLE_TAG}.zip"
ANGLE_SHA256 = "e57ac5ce6fb20f24dde931b96ade3ddfbbb7518a52cf17811d996a97de6b7ef9"
ANGLE_URL = f"https://github.com/EdgeFirstAI/angle-package/releases/download/{ANGLE_TAG}/{ANGLE_NAME}"


def angle_archive() -> Path:
    path = BUILD / ANGLE_NAME
    if not path.exists():
        with urlopen(ANGLE_URL, timeout=60) as response:
            (path.with_suffix(path.suffix + ".tmp")).write_bytes(response.read())
        path.with_suffix(path.suffix + ".tmp").replace(path)
    if sha256(path.read_bytes()).hexdigest() != ANGLE_SHA256:
        raise SystemExit(f"ANGLE SHA-256 mismatch: {path}")
    return path


def main() -> None:
    exe = BUILD / "airlink.exe"
    if not exe.is_file():
        raise SystemExit(f"Build airlink.exe first: {exe}")
    BUILD.mkdir(parents=True, exist_ok=True)
    # (a release names it with its version: package.py dist/AirLink-0.2.0-windows-x64.zip)
    out = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else BUILD / "AirLink-Windows-ANGLE.zip"
    out.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(angle_archive()) as angle, ZipFile(out, "w", ZIP_DEFLATED, compresslevel=6) as bundle:
        bundle.write(exe, "AirLink/airlink.exe")
        for folder in ("fonts", "licenses"):
            for file in sorted((BUILD / folder).rglob("*")):
                if file.is_file() and file.name != "ANGLE.txt":
                    bundle.write(file, "AirLink/" + str(file.relative_to(BUILD)))
        bundle.write(ROOT / "app/packaging/licenses/OpenH264.txt", "AirLink/licenses/OpenH264.txt")
        for source, target in (("bin/libEGL.dll", "libEGL.dll"),
                               ("bin/libGLESv2.dll", "libGLESv2.dll"),
                               ("LICENSE", "licenses/ANGLE.txt")):
            bundle.writestr("AirLink/" + target, angle.read(source))
        bundle.write(ROOT / "LICENSE", "AirLink/LICENSE.txt")
        windows = ROOT / "app/packaging/windows"
        for name in ("diagnose.cmd", "install-package.cmd", "uninstall-package.cmd", "diagnose-package.cmd"):
            bundle.write(windows / name, "AirLink/" + name)
        # The packaged install (call audio needs package identity): manifest and logos.
        for file in sorted((windows / "appx").rglob("*")):
            if file.is_file():
                bundle.write(file, "AirLink/" + str(file.relative_to(windows / "appx")))
        bundle.write(ROOT / "docs/windows-test.md", "AirLink/TEST.md")
    with ZipFile(out) as bundle:
        if bad := bundle.testzip():
            raise SystemExit(f"Corrupt package entry: {bad}")
    print(out)


if __name__ == "__main__":
    main()
