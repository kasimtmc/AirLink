// The clipboard on Wayland while AirLink's window isn't focused (closed to the tray, or behind
// another): a Wayland client sees the selection only while it has the keyboard's focus, so the
// copies made in other apps never reached Kompoz's ClipboardManager then. The data-control
// protocol (ext-data-control-v1: KDE, GNOME, wlroots compositors), made for clipboard managers,
// tells of every selection with or without the focus, and sets one without it too (the text a
// paired device shares, put on this computer's clipboard while the window is hidden). This works
// on a connection and a thread of its own; where the compositor has no such protocol nothing changes.
//
// libwayland-client is reached through Kompoz's run-time loaded functions (wayland_dynamic.hpp),
// as Kompoz's own Wayland platform does: no link to it, so X11-only desktops still run.

#include "clipboard_watch.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// clang-format off
#include "wayland_dynamic.hpp"  // (first: the protocol headers' inline functions call its pointers)
#include "wayland-client-protocol.h"
#include "wayland/ext-data-control-v1-client-protocol.h"
// clang-format on

namespace airlink::platform {

namespace {

namespace wl = kompoz::platform::wayland;

struct Watch {
    std::mutex mutex;
    WatchedClipboard latest;
    std::atomic<bool> running{false};

    wl_display* display = nullptr;
    wl_seat* seat = nullptr;
    ext_data_control_manager_v1* manager = nullptr;
    ext_data_control_device_v1* device = nullptr;
    std::map<ext_data_control_offer_v1*, std::set<std::string>> offers;  // each offer's types
    ext_data_control_offer_v1* selection = nullptr;
    ext_data_control_source_v1* source = nullptr;  // the text we put there (under `mutex`)
    std::string ownText;
};

// Our own selection's extra type: seen coming back, it is read from memory, not through a pipe
// (which this thread, blocked reading, couldn't feed itself).
constexpr const char* kOwnType = "application/x-airlink-clipboard";

Watch& watch() {
    static Watch w;
    return w;
}

void forget(ext_data_control_offer_v1* offer) {
    if (!offer) return;
    watch().offers.erase(offer);
    ext_data_control_offer_v1_destroy(offer);
}

// The offer's text, as the app that copied it writes it into a pipe (at most 4 MB, 3 s).
std::string receive(ext_data_control_offer_v1* offer, const char* type) {
    int fds[2];
    if (pipe(fds) != 0) return {};
    ext_data_control_offer_v1_receive(offer, type, fds[1]);
    close(fds[1]);
    wl_display_flush(watch().display);
    std::string text;
    char buf[8192];
    pollfd p{fds[0], POLLIN, 0};
    while (text.size() < (4u << 20) && poll(&p, 1, 3000) > 0) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        text.append(buf, size_t(n));
    }
    close(fds[0]);
    return text;
}

void onSelection(ext_data_control_offer_v1* offer) {
    Watch& w = watch();
    if (w.selection && w.selection != offer) forget(w.selection);
    w.selection = offer;
    if (!offer) return;  // (emptied: the last text stays what it was)
    const auto& types = w.offers[offer];
    const char* type = nullptr;
    for (const char* t : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"})
        if (types.count(t)) {
            type = t;
            break;
        }
    if (types.count(kOwnType)) {
        std::lock_guard lock(w.mutex);
        w.latest = WatchedClipboard{.text = w.ownText, .sensitive = types.count("x-kde-passwordManagerHint") > 0, .serial = w.latest.serial + 1};
        return;
    }
    if (!type) return;  // files, an image: Kompoz's clipboard reads those as the window shows
    // A password manager marks its copies (KDE's hint; Windows' and macOS' ones as KeePassXC sets them).
    bool sensitive = types.count("x-kde-passwordManagerHint") || types.count("ExcludeClipboardContentFromMonitorProcessing") ||
                     types.count("org.nspasteboard.ConcealedType");
    std::string text = receive(offer, type);
    std::lock_guard lock(w.mutex);
    w.latest = WatchedClipboard{.text = std::move(text), .sensitive = sensitive, .serial = w.latest.serial + 1};
}

const ext_data_control_offer_v1_listener offerListener = {
    .offer = [](void*, ext_data_control_offer_v1* offer, const char* type) { watch().offers[offer].insert(type); },
};

const ext_data_control_device_v1_listener deviceListener = {
    .data_offer = [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
        watch().offers[offer];
        ext_data_control_offer_v1_add_listener(offer, &offerListener, nullptr);
    },
    .selection = [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) { onSelection(offer); },
    .finished = [](void*, ext_data_control_device_v1*) { watch().running = false; },
    .primary_selection = [](void*, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
        // (the middle-click selection: not shared)
        if (offer && offer != watch().selection) forget(offer);
    },
};

const ext_data_control_source_v1_listener sourceListener = {
    .send = [](void*, ext_data_control_source_v1*, const char* type, int32_t fd) {
        Watch& w = watch();
        std::string text;
        {
            std::lock_guard lock(w.mutex);
            text = std::strcmp(type, "x-kde-passwordManagerHint") == 0 ? std::string("secret") : w.ownText;
        }
        size_t done = 0;
        while (done < text.size()) {
            ssize_t n = write(fd, text.data() + done, text.size() - done);
            if (n <= 0) break;
            done += size_t(n);
        }
        close(fd);
    },
    .cancelled = [](void*, ext_data_control_source_v1* source) {
        // Another copy took its place (ours too: a newer text of ours).
        Watch& w = watch();
        {
            std::lock_guard lock(w.mutex);
            if (w.source == source) w.source = nullptr;
        }
        ext_data_control_source_v1_destroy(source);
    },
};

const wl_registry_listener registryListener = {
    .global = [](void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
        Watch& w = watch();
        if (!w.seat && std::strcmp(interface, wl_seat_interface.name) == 0)
            w.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        else if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
            w.manager = static_cast<ext_data_control_manager_v1*>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

void run() {
    Watch& w = watch();
    int fd = wl_display_get_fd(w.display);
    while (w.running) {
        while (wl_display_prepare_read(w.display) != 0)
            if (wl_display_dispatch_pending(w.display) < 0) return;
        wl_display_flush(w.display);
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, -1) <= 0) {
            wl_display_cancel_read(w.display);
            continue;
        }
        if (wl_display_read_events(w.display) < 0 || wl_display_dispatch_pending(w.display) < 0) return;
    }
}

}  // namespace

bool startClipboardWatch() {
    Watch& w = watch();
    if (w.running) return true;
    if (!std::getenv("WAYLAND_DISPLAY") || !wl::load()) return false;
    w.display = wl_display_connect(nullptr);
    if (!w.display) return false;
    wl_registry* registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(registry, &registryListener, nullptr);
    wl_display_roundtrip(w.display);
    if (!w.seat || !w.manager) {
        wl_display_disconnect(w.display);
        w.display = nullptr;
        return false;
    }
    w.device = ext_data_control_manager_v1_get_data_device(w.manager, w.seat);
    ext_data_control_device_v1_add_listener(w.device, &deviceListener, nullptr);
    w.running = true;
    std::thread(run).detach();  // (for the app's life)
    return true;
}

bool writeClipboard(const std::string& text, bool sensitive) {
    Watch& w = watch();
    if (!w.running) return false;
    std::lock_guard lock(w.mutex);
    w.ownText = text;
    // (the one before goes as the compositor cancels it, on the watch's thread)
    w.source = ext_data_control_manager_v1_create_data_source(w.manager);
    ext_data_control_source_v1_add_listener(w.source, &sourceListener, nullptr);
    for (const char* type : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT", kOwnType})
        ext_data_control_source_v1_offer(w.source, type);
    // A password: clipboard managers (Klipper) leave it out of their history.
    if (sensitive) ext_data_control_source_v1_offer(w.source, "x-kde-passwordManagerHint");
    ext_data_control_device_v1_set_selection(w.device, w.source);
    wl_display_flush(w.display);
    return true;
}

std::optional<WatchedClipboard> watchedClipboard() {
    Watch& w = watch();
    if (!w.running) return std::nullopt;
    std::lock_guard lock(w.mutex);
    if (w.latest.serial == 0) return std::nullopt;
    return w.latest;
}

}  // namespace airlink::platform
