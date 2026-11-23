#!/bin/sh
# Installs AirLink's desktop integration for the current user (no root):
#   - an application menu entry (and "Open with" for any file)
#   - "AirLink ile gönder" in Dolphin's (KDE) right-click menu
#   - a "AirLink ile gönder" script in Files' (GNOME Nautilus) Scripts menu
#   - the icon
# Usage: install.sh [/path/to/airlink]     (default: the build next to this file)
#        install.sh --uninstall
set -eu

here=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
desktop_file=$data/applications/dev.airlink.AirLink.desktop
service_menu=$data/kio/servicemenus/airlink-send.desktop
nautilus_script="$data/nautilus/scripts/AirLink ile gönder"
icon=$data/icons/hicolor/scalable/apps/airlink.svg

refresh() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$data/applications" || true
    command -v kbuildsycoca6 >/dev/null 2>&1 && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$desktop_file" "$service_menu" "$nautilus_script" "$icon"
    refresh
    echo "AirLink desktop integration removed."
    exit 0
fi

bin=${1:-$here/../../build/airlink}
bin=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")
[ -x "$bin" ] || { echo "AirLink binary not found: $bin (build it, or pass its path)" >&2; exit 1; }

mkdir -p "$(dirname "$desktop_file")" "$(dirname "$service_menu")" "$(dirname "$nautilus_script")" "$(dirname "$icon")"
cp "$here/airlink.svg" "$icon"

cat > "$desktop_file" <<DESKTOP
[Desktop Entry]
Type=Application
Name=AirLink
GenericName=Dosya paylaşımı
Comment=Cihazlarınız arasında dosya ve pano paylaşın
Exec="$bin" --send %F
Icon=airlink
Terminal=false
Categories=Network;FileTransfer;
MimeType=application/octet-stream;inode/directory;
StartupWMClass=airlink
DESKTOP

# Dolphin: Plasma 6 runs service menus only when the file is executable.
cat > "$service_menu" <<MENU
[Desktop Entry]
Type=Service
MimeType=all/allfiles;inode/directory;
Actions=sendWithAirLink
X-KDE-Priority=TopLevel

[Desktop Action sendWithAirLink]
Name=AirLink ile gönder
Icon=airlink
Exec="$bin" --send %F
MENU
chmod +x "$service_menu"

cat > "$nautilus_script" <<SCRIPT
#!/bin/sh
# Nautilus gives the selection one path per line.
IFS='
'
exec "$bin" --send \$NAUTILUS_SCRIPT_SELECTED_FILE_PATHS
SCRIPT
chmod +x "$nautilus_script"

refresh
echo "AirLink desktop integration installed for $bin"
echo "  Dolphin: right-click a file > AirLink ile gönder"
echo "  Files (Nautilus): right-click > Scripts > AirLink ile gönder"
