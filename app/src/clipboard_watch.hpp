#pragma once

// The system clipboard's text as other apps copy it, and setting it, also while AirLink's window
// doesn't have the focus (Wayland gives the clipboard only to the focused window: see
// clipboard_watch_wayland.cpp).
// Elsewhere (X11, Windows, a compositor without data-control) there is no watch, and the window's
// own clipboard is read as before.

#include <cstdint>
#include <optional>
#include <string>

namespace airlink::platform {

struct WatchedClipboard {
    std::string text;
    bool sensitive = false;  // marked as a password by the app that copied it
    uint64_t serial = 0;     // one more for each copy seen
};

// Starts watching where it can (once); false where it can't.
bool startClipboardWatch();
// Puts text on the clipboard (also without the window's focus); false where there is no watch.
bool writeClipboard(const std::string& text, bool sensitive);
// The latest copy seen; none before the first, or where there is no watch.
std::optional<WatchedClipboard> watchedClipboard();

}  // namespace airlink::platform
