// No clipboard watch here (Windows and X11 give the clipboard to any window; Android reads it in
// front): the window's own clipboard is read. See clipboard_watch.hpp.

#include "clipboard_watch.hpp"

namespace airlink::platform {

bool startClipboardWatch() { return false; }
bool writeClipboard(const std::string&, bool) { return false; }
std::optional<WatchedClipboard> watchedClipboard() { return std::nullopt; }

}  // namespace airlink::platform
