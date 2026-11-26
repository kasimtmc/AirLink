#!/bin/sh
# The released panel widget's click: `launch.sh --panel X Y` hands the point to the running
# AirLink, or starts it. AirLink records its own binary (an AppImage's file, say) each time it
# starts; before that, the .deb's or a PATH `airlink`, then the Flatpak.
state="${XDG_STATE_HOME:-$HOME/.local/state}/airlink/launcher"
if [ -r "$state" ]; then
    bin=$(head -n 1 "$state")
    [ -n "$bin" ] && [ -x "$bin" ] && exec "$bin" "$@"
fi
command -v airlink >/dev/null 2>&1 && exec airlink "$@"
if command -v flatpak >/dev/null 2>&1 && flatpak info com.plyg.AirLink >/dev/null 2>&1; then
    exec flatpak run com.plyg.AirLink "$@"
fi
command -v notify-send >/dev/null 2>&1 &&
    notify-send -a AirLink -i airlink "AirLink bulunamadı" "AirLink'i bir kez kendiniz başlatın; panel simgesi sonra onu açar."
exit 1
