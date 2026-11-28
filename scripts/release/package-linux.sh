#!/usr/bin/env bash
# Packages a Linux release build as a tar.gz, a .deb, an AppImage and the KDE panel widget
# (.plasmoid), in dist/:
#   package-linux.sh [build dir]   (default app/build-release; build-linux.sh makes it)
# The AppImage needs linuxdeploy (downloaded to dist/.tools when missing).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$root/app/build-release}"
version="$(cat "$root/VERSION")"
dist="$root/dist"
tools="$dist/.tools"
linux="$root/app/packaging/linux"
mkdir -p "$dist" "$tools"
[[ -x "$build/airlink" ]] || { echo "Build first: $build/airlink" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# The program and what sits next to it (Kompoz finds fonts/ beside the binary).
put_program() {  # dir
    install -Dm755 "$build/airlink" "$1/airlink"
    cp -r "$build/fonts" "$build/licenses" "$1/"
    install -Dm644 "$root/app/packaging/licenses/OpenH264.txt" "$1/licenses/OpenH264.txt"
    install -Dm644 "$root/LICENSE" "$1/LICENSE"
}

desktop_entry() {  # Exec
    cat <<DESKTOP
[Desktop Entry]
Type=Application
Name=AirLink
GenericName=Device link
Comment=Linux, Windows ve Android cihazlarınız arasında dosya, pano, bildirim ve arama
Exec=$1 %F
Icon=airlink
Terminal=false
Categories=Network;FileTransfer;Utility;
Keywords=phone;android;share;transfer;clipboard;sms;
StartupWMClass=airlink
MimeType=application/octet-stream;
DESKTOP
}

# The KDE panel widget, as a folder: `bin` is the binary it starts, empty for the released
# widget (contents/code/launch.sh then finds AirLink).
put_plasmoid() {  # dir [bin]
    mkdir -p "$1"
    cp -r "$linux/plasmoid/." "$1/"
    sed -i "s|\"Version\": \"[^\"]*\"|\"Version\": \"$version\"|" "$1/metadata.json"
    [[ -z "${2:-}" ]] || sed -i "s|@AIRLINK_BIN@|$2|" "$1/contents/ui/main.qml"
}

# ---- .plasmoid: the panel widget on its own, for any of the packages below ----------------
put_plasmoid "$stage/plasmoid"
rm -f "$dist/AirLink-$version.plasmoid"
(cd "$stage/plasmoid" && python3 -m zipfile -c "$dist/AirLink-$version.plasmoid" metadata.json contents)
echo "$dist/AirLink-$version.plasmoid"

# ---- tar.gz: the binary, its files and install.sh (menus, file-manager entries, plasmoid) ----
tarname="AirLink-$version-linux-x86_64"
put_program "$stage/$tarname"
mkdir -p "$stage/$tarname/packaging"
cp -r "$linux"/. "$stage/$tarname/packaging/"
cp "$root/README.md" "$stage/$tarname/"
cat > "$stage/$tarname/INSTALL.txt" <<TXT
AirLink $version for Linux (x86_64, glibc 2.39+)

Run ./airlink, or install the menu entry, the file-manager menus and the KDE panel widget
for this user:

    ./packaging/install.sh

packaging/install.sh --uninstall removes them. Needs PipeWire and PulseAudio's client
libraries (libpipewire-0.3, libpulse); BlueZ, bluetoothctl and pactl for call audio.
TXT
tar -C "$stage" -czf "$dist/$tarname.tar.gz" "$tarname"
echo "$dist/$tarname.tar.gz"

# ---- .deb: /opt/airlink, a /usr/bin link, the menu entry, icon and panel widget ------------
deb="$stage/deb"
put_program "$deb/opt/airlink"
mkdir -p "$deb/usr/bin" "$deb/usr/share/applications" "$deb/usr/share/icons/hicolor/scalable/apps" "$deb/DEBIAN"
ln -s /opt/airlink/airlink "$deb/usr/bin/airlink"
desktop_entry /usr/bin/airlink > "$deb/usr/share/applications/com.plyg.AirLink.desktop"
cp "$linux/airlink.svg" "$deb/usr/share/icons/hicolor/scalable/apps/airlink.svg"
mkdir -p "$deb/usr/share/kio/servicemenus"
cat > "$deb/usr/share/kio/servicemenus/airlink-send.desktop" <<DESKTOP
[Desktop Entry]
Type=Service
MimeType=all/all;
Actions=airlinkSend;
X-KDE-Priority=TopLevel

[Desktop Action airlinkSend]
Name=AirLink ile gönder
Icon=airlink
Exec=/usr/bin/airlink --send %F
DESKTOP
put_plasmoid "$deb/usr/share/plasma/plasmoids/dev.airlink.panel" /usr/bin/airlink
# The libraries it links: dpkg-shlibdeps reads them from the binary.
mkdir -p "$stage/shlibs/debian"
echo "Source: airlink" > "$stage/shlibs/debian/control"
depends="$(cd "$stage/shlibs" && dpkg-shlibdeps -O -e "$deb/opt/airlink/airlink" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
size="$(du -sk "$deb" | cut -f1)"
cat > "$deb/DEBIAN/control" <<CONTROL
Package: airlink
Version: $version
Architecture: amd64
Maintainer: Kasim <kasimtmc@users.noreply.github.com>
Installed-Size: $size
Depends: $depends
Recommends: bluez, pulseaudio-utils, libnotify-bin
Section: net
Priority: optional
Homepage: https://github.com/kasimtmc/AirLink
Description: Seamless link between Linux, Windows and Android devices
 Files, clipboard, notifications, SMS, calls, phone screen and phone audio
 between paired devices on your own network, with no account and no cloud.
CONTROL
fakeroot dpkg-deb --build -Zxz "$deb" "$dist/airlink_${version}_amd64.deb" >/dev/null
echo "$dist/airlink_${version}_amd64.deb"

# ---- AppImage: linuxdeploy bundles the libraries a distro may not have --------------------
linuxdeploy="$tools/linuxdeploy-x86_64.AppImage"
if [[ ! -x "$linuxdeploy" ]]; then
    curl -fL --retry 3 -o "$linuxdeploy" \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x "$linuxdeploy"
fi
appdir="$stage/AppDir"
put_program "$appdir/usr/bin"
desktop_entry airlink > "$stage/com.plyg.AirLink.desktop"
# The sound server's, D-Bus', systemd's and the display server's client libraries (and what they
# link) go with the system's own: not bundled. curl, libjpeg-turbo and libwebp (and curl's TLS) are.
export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 LINUXDEPLOY_OUTPUT_VERSION="$version" NO_STRIP=1
(cd "$dist" && "$linuxdeploy" --appdir "$appdir" \
    --executable "$appdir/usr/bin/airlink" \
    --desktop-file "$stage/com.plyg.AirLink.desktop" \
    --icon-file "$linux/airlink.svg" \
    --exclude-library 'libpipewire-0.3.so*' --exclude-library 'libpulse*.so*' --exclude-library 'libdbus-1.so*' \
    --exclude-library 'libsystemd.so*' --exclude-library 'libvulkan.so*' --exclude-library 'libwayland-*.so*' \
    --exclude-library 'libxkbcommon.so*' --exclude-library 'libX*.so*' --exclude-library 'libxcb*.so*' \
    --exclude-library 'libsndfile.so*' --exclude-library 'libFLAC.so*' --exclude-library 'libogg.so*' \
    --exclude-library 'libvorbis*.so*' --exclude-library 'libopus.so*' --exclude-library 'libmpg123.so*' \
    --exclude-library 'libmp3lame.so*' --exclude-library 'libasyncns.so*' --exclude-library 'libapparmor.so*' \
    --exclude-library 'libcap.so*' --exclude-library 'libgcrypt.so*' --exclude-library 'liblz4.so*' \
    --exclude-library 'liblzma.so*' --exclude-library 'libbsd.so*' --exclude-library 'libmd.so*' \
    --exclude-library 'libffi.so*' \
    --output appimage >/dev/null)
mv "$dist"/AirLink-"$version"-x86_64.AppImage "$dist/AirLink-$version-x86_64.AppImage" 2>/dev/null || true
ls "$dist"/*.AppImage
