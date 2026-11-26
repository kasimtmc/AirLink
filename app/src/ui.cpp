// AirLink's UI. The @Kompozable functions are Kotlin, turned into C++ by kompozc; the rest is C++.

#include <kompoz/kompoz.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app_icons.hpp"
#include "app_state.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "tray_icon.hpp"

using namespace kompoz;
using namespace airlink;

// ---- C++ helpers the composables call ------------------------------------------------------

static App appState() { return App::get(); }
static void openDownloads(const std::string& path) { airlink::platform::openFolder(path); }
static bool needsBackgroundAutostartSettings() { return airlink::platform::needsBackgroundAutostartSettings(); }
static void openBackgroundAutostartSettings() { airlink::platform::openBackgroundAutostartSettings(); }

enum class TrayIconStatus { Starting, Connected, NoDevices, Paused, Error };

// Preserve the white link mark and the disc's soft edge while tinting its blue background.
static ImageBitmap statusTrayIcon(TrayIconStatus status) {
    ImageBitmap base = decodeToImageBitmap(trayIconPng());
    if (base.empty() || !base.pixels()) return base;
    std::array<int, 3> color{37, 99, 235};  // starting: original blue
    switch (status) {
    case TrayIconStatus::Connected: color = {22, 163, 74}; break;
    case TrayIconStatus::NoDevices: color = {217, 119, 6}; break;
    case TrayIconStatus::Paused: color = {107, 114, 128}; break;
    case TrayIconStatus::Error: color = {220, 38, 38}; break;
    case TrayIconStatus::Starting: break;
    }
    std::vector<uint8_t> rgba(base.pixels(), base.pixels() + size_t(base.width()) * size_t(base.height()) * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        if (!rgba[i + 3]) continue;
        int white = std::clamp((int(rgba[i]) - 37) * 255 / 218, 0, 255);
        for (int channel = 0; channel < 3; ++channel)
            rgba[i + size_t(channel)] = uint8_t((color[size_t(channel)] * (255 - white) + 255 * white + 127) / 255);
    }
    return ImageBitmap::fromPixels(base.width(), base.height(), std::move(rgba));
}

static std::string shortId(const std::string& id) { return id.substr(0, 8); }

static std::vector<std::string> droppedFiles(const DragAndDropEvent& e) { return e.data ? e.data->files : std::vector<std::string>{}; }

// Whether a drag may carry files. While it is over the window only its MIME types are known (the
// files are read at the drop): a list of URIs, or anything that isn't text (Android's drags from
// other apps carry a file's own type, image/jpeg, with a content:// URI).
static bool carriesFiles(const DragAndDropEvent& e) {
    if (!e.data) return false;
    if (!e.data->files.empty()) return true;
    for (const std::string& type : e.data->mimeTypes)
        if (type == "text/uri-list" || type == "x-special/gnome-copied-files" || !type.starts_with("text/")) return true;
    return false;
}

// URIs the drop carries that aren't local paths (Android's content:// ones).
static std::vector<std::string> droppedUris(const DragAndDropEvent& e) {
    std::vector<std::string> uris;
    if (!e.data) return uris;
    for (const auto& item : e.data->toClipEntry().items)
        if (item.uri && !item.uri->starts_with("file://")) uris.push_back(*item.uri);
    return uris;
}

static std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "14:05" for a time in ms since the epoch, in the local time zone.
static std::string clockTime(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", &local);
    return buf;
}

// The first letter of a name, for an avatar (a whole UTF-8 character).
static std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    size_t n = 1;
    unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    return name.substr(0, n);
}

// "14:05" today, "23.11" another day.
static std::string shortDate(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000), now = std::time(nullptr);
    std::tm a{}, b{};
#if defined(_WIN32)
    localtime_s(&a, &t);
    localtime_s(&b, &now);
#else
    localtime_r(&t, &a);
    localtime_r(&now, &b);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M" : "%d.%m", &a);
    return buf;
}

// "1 dk 05 sn", "42 sn".
static std::string duration(int64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + " sn";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld dk %02lld sn", (long long)(seconds / 60), (long long)(seconds % 60));
    return buf;
}

// Where a point on the shown screen is on the phone's, as fractions (0..1) of it: the image is
// fitted into `box` (letterboxed), keeping its aspect.
static bool toScreen(Offset p, Size box, int iw, int ih, double& x, double& y) {
    if (box.width <= 0 || box.height <= 0 || iw <= 0 || ih <= 0) return false;
    double scale = std::min(double(box.width) / iw, double(box.height) / ih);
    double w = iw * scale, h = ih * scale, left = (box.width - w) / 2, top = (box.height - h) / 2;
    x = (p.x - left) / w;
    y = (p.y - top) / h;
    return x >= 0 && x <= 1 && y >= 0 && y <= 1;
}

static std::string touchBody(const std::string& type, Offset p, Size box, int iw, int ih) {
    double x, y;
    if (!toScreen(p, box, iw, ih, x, y)) return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"x\":%.4f,\"y\":%.4f}", type.c_str(), x, y);
    return buf;
}

static std::string swipeBody(Offset from, Offset to, Size box, int iw, int ih) {
    double x1, y1, x2, y2;
    toScreen(from, box, iw, ih, x1, y1);
    toScreen(to, box, iw, ih, x2, y2);
    auto clamp = [](double v) { return std::clamp(v, 0.0, 1.0); };
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"type\":\"swipe\",\"x\":%.4f,\"y\":%.4f,\"x2\":%.4f,\"y2\":%.4f,\"ms\":250}", clamp(x1),
                  clamp(y1), clamp(x2), clamp(y2));
    return buf;
}

// A touch on an app's display: fractions of it, clamped (a drag may leave the image).
static std::string appTouch(const std::string& action, Offset p, Size box, int iw, int ih) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"cmd\":\"touch\",\"action\":\"%s\",\"x\":%.4f,\"y\":%.4f}", action.c_str(),
                  std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
    return buf;
}

// Whether an app's label has the search text in it, letters' case aside (Turkish İ/ı, Ş, Ç...).
static std::string foldCase(const std::string& s) {
    static const std::pair<const char*, const char*> pairs[] = {
        {"İ", "i"}, {"I", "ı"}, {"Ş", "ş"}, {"Ç", "ç"}, {"Ğ", "ğ"}, {"Ö", "ö"}, {"Ü", "ü"}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool done = false;
        for (auto [from, to] : pairs) {
            size_t n = std::strlen(from);
            if (s.compare(i, n, from) == 0) {
                out += to;
                i += n;
                done = true;
                break;
            }
        }
        if (done) continue;
        char c = s[i++];
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
    return out;
}
static bool matchesQuery(const std::string& label, const std::string& query) {
    std::string l = foldCase(label), q = foldCase(query);
    if (l.find(q) != std::string::npos) return true;
    // "i" also finds "ı" and the other way round (people type either)
    auto plain = [](std::string s) {
        for (size_t p; (p = s.find("ı")) != std::string::npos;) s.replace(p, std::strlen("ı"), "i");
        return s;
    };
    return plain(l).find(plain(q)) != std::string::npos;
}

static std::string textBody(const std::string& text) { return "{\"cmd\":\"text\",\"text\":" + json::quote(text) + "}"; }

// A wheel turn over an app's display (notches; Kompoz: down and right positive, Android: up and left).
static void appScroll(const App& app, int session, Offset p, Size box, int iw, int ih, Offset delta) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    app.appWheel(session, std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), -double(delta.x), -double(delta.y));
}

// A wheel turn over the phone's shared screen; false outside the image.
static bool screenScroll(const App& app, Offset p, Size box, int iw, int ih, Offset delta) {
    double x = 0, y = 0;
    if (!toScreen(p, box, iw, ih, x, y)) return false;
    app.screenWheel(x, y, delta.x, delta.y);
    return true;
}

// A key pressed over an app's window, as Android's KeyEvent code and meta state ("" when it has none).
static std::string appKey(const KeyEvent& e) {
    using K = Key;
    int code = 0;
    K k = e.key;
    if (k >= K::A && k <= K::Z) code = 29 + (int(k) - int(K::A));             // KEYCODE_A..Z
    else if (k >= K::Zero && k <= K::Nine) code = 7 + (int(k) - int(K::Zero));  // KEYCODE_0..9
    else if (k >= K::F1 && k <= K::F12) code = 131 + (int(k) - int(K::F1));    // KEYCODE_F1..F12
    else switch (k) {
        case K::Enter: code = 66; break;
        case K::Escape: code = 4; break;  // Back
        case K::Tab: code = 61; break;
        case K::Backspace: code = 67; break;
        case K::Delete: code = 112; break;
        case K::Space: code = 62; break;
        case K::Left: code = 21; break;
        case K::Right: code = 22; break;
        case K::Up: code = 19; break;
        case K::Down: code = 20; break;
        case K::Home: code = 122; break;
        case K::End: code = 123; break;
        case K::PageUp: code = 92; break;
        case K::PageDown: code = 93; break;
        default: return {};
    }
    int meta = 0;
    if (e.modifiers.shift) meta |= 0x1 | 0x40;      // META_SHIFT_ON | META_SHIFT_LEFT_ON
    if (e.modifiers.ctrl) meta |= 0x1000 | 0x2000;  // META_CTRL_ON | META_CTRL_LEFT_ON
    if (e.modifiers.alt) meta |= 0x2 | 0x10;        // META_ALT_ON | META_ALT_LEFT_ON
    // Printable keys are delivered separately as UTF-8 by Kompoz.
    if (e.modifiers.ctrl == e.modifiers.alt &&
        ((k >= K::A && k <= K::Z) || (k >= K::Zero && k <= K::Nine) || k == K::Space)) return {};
    return "{\"cmd\":\"key\",\"code\":" + std::to_string(code) + ",\"meta\":" + std::to_string(meta) + "}";
}

// The viewer's input for a computer's screen (desktop.input): a point as fractions of it.
static std::string desktopPoint(const std::string& type, Offset p, Size box, int iw, int ih, const std::string& extra) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"x\":%.4f,\"y\":%.4f%s}", type.c_str(), std::clamp(x, 0.0, 1.0),
                  std::clamp(y, 0.0, 1.0), extra.c_str());
    return buf;
}

static std::string desktopKey(const std::string& key, const std::string& modifier) {
    std::string body = "{\"type\":\"key\",\"key\":" + json::quote(key);
    if (!modifier.empty()) body += ",\"modifiers\":[" + json::quote(modifier) + "]";
    return body + "}";
}

static std::string desktopText(const std::string& text) { return "{\"type\":\"text\",\"text\":" + json::quote(text) + "}"; }

// Wheel notches from a two-finger drag on the phone: 40 px a notch, content follows the fingers.
static std::string desktopScroll(Offset p, Size box, int iw, int ih, Offset pan) {
    char extra[64];
    std::snprintf(extra, sizeof extra, ",\"dx\":%.2f,\"dy\":%.2f", -pan.x / 40.0, -pan.y / 40.0);
    return desktopPoint("scroll", p, box, iw, ih, extra);
}

// The running transfers and the last `finished` finished ones, in their order.
static std::vector<Transfer> recentTransfers(const std::vector<Transfer>& all, size_t finished) {
    size_t done = std::count_if(all.begin(), all.end(), [](const Transfer& t) { return !t.active(); });
    std::vector<Transfer> out;
    for (const Transfer& t : all) {
        if (t.active() || done-- <= finished) out.push_back(t);
    }
    return out;
}

// The first http(s) link in a text, or "".
static std::string firstLink(const std::string& text) {
    size_t at = text.find("https://");
    if (at == std::string::npos) at = text.find("http://");
    if (at == std::string::npos) return {};
    size_t end = at;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
    return text.substr(at, end - at);
}

// Files copied in a file manager (the clipboard's file:// items), as paths.
static std::vector<std::string> copiedFiles(const Clipboard& clip) {
    std::vector<std::string> out;
    auto entry = clip.getClipEntry().value;
    if (!entry) return out;
    for (const auto& item : entry->items) {
        if (!item.uri || !item.uri->starts_with("file://")) continue;
        std::string path;
        const std::string& u = *item.uri;
        for (size_t i = 7; i < u.size(); ++i) {
            if (u[i] == '%' && i + 2 < u.size() && std::isxdigit(static_cast<unsigned char>(u[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(u[i + 2]))) {
                path += char(std::stoi(u.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else path += u[i];
        }
#if defined(_WIN32)
        if (path.size() > 2 && path[0] == '/' && path[2] == ':') path.erase(0, 1);  // file:///C:/…
#endif
        out.push_back(path);
    }
    return out;
}

static std::string callTypeLabel(const std::string& type) {
    if (type == "incoming") return "Gelen";
    if (type == "outgoing") return "Giden";
    if (type == "missed") return "Cevapsız";
    if (type == "rejected") return "Reddedildi";
    return "Arama";
}

static std::string desktopRequestText() {
    std::string text = "Ekranınızı görecek, fare ve klavyeyi kullanabilecek. İstediğiniz zaman buradan durdurabilirsiniz.";
#if defined(__linux__) && !defined(__ANDROID__)
    text += "\n\nMasaüstü ilk seferde hangi ekranın paylaşılacağını da soracak.";
#endif
    return text;
}

static const std::vector<std::string>& desktopModifiers() {
    static const std::vector<std::string> list = {"Control", "Alt", "Shift", "Super"};
    return list;
}

static const std::vector<std::string>& desktopKeys() {
    static const std::vector<std::string> list = {"Escape", "Tab", "Enter", "Backspace", "Left", "Up", "Down", "Right"};
    return list;
}

static int64_t nowMs() { return int64_t(std::time(nullptr)) * 1000; }

// "03:25" (or "1:03:25") since `since` (ms).
static std::string elapsed(int64_t since, int64_t now) {
    int64_t s = std::max<int64_t>(0, (now - since) / 1000);
    char buf[16];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

static std::string routeLabel(const std::string& route) {
    if (route == "speaker") return "Hoparlör";
    if (route == "bluetooth") return "Bluetooth";
    if (route == "wired") return "Kablolu kulaklık";
    if (route == "streaming") return "Akış";
    return "Ahize";
}

static std::string routeBody(const std::string& route) { return "{\"route\":\"" + route + "\"}"; }

static std::string callStateLabel(const ActiveCall& c) {
    if (c.held || c.state == "held") return "Beklemede";
    if (c.state == "dialing") return "Aranıyor";
    return "Görüşmede";
}

// Up to two letters of a contact's name for its avatar; "" for a bare number (a person icon).
static std::string initials(const std::string& title) {
    std::string out;
    bool start = true;
    for (size_t i = 0; i < title.size() && out.size() < 8;) {
        unsigned char c = static_cast<unsigned char>(title[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (c == ' ') { start = true; ++i; continue; }
        if (start) {
            if (out.empty() && (std::isdigit(c) || c == '+' || c == '(')) return {};
            out += title.substr(i, len);
            start = false;
            if (std::count_if(out.begin(), out.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }) == 2) break;
        }
        i += len;
    }
    // Upper case for ASCII and the Turkish letters a name starts with.
    std::string upper;
    for (size_t i = 0; i < out.size();) {
        unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 'i') { upper += "İ"; ++i; continue; }  // Turkish: i → İ
        if (c < 0x80) { upper += char(std::toupper(c)); ++i; continue; }
        size_t len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        std::string ch = out.substr(i, len);
        if (ch == "ç") ch = "Ç"; else if (ch == "ğ") ch = "Ğ"; else if (ch == "ı") ch = "I"; else if (ch == "ö") ch = "Ö";
        else if (ch == "ş") ch = "Ş"; else if (ch == "ü") ch = "Ü";
        upper += ch;
        i += len;
    }
    return upper;
}

static std::vector<std::vector<std::string>> keypadRows() {
    return {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"*", "0", "#"}};
}

static std::vector<std::string> quickReplies() {
    return {"Şu an konuşamıyorum, sonra ararım.", "Toplantıdayım, birazdan dönerim.", "Yoldayım, sonra ararım.",
            "Mesaj atar mısın?"};
}

static std::string spacedCode(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

static bool isDesktop() {
#if defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

// ---- Screens ---------------------------------------------------------------------------------

@Kompozable
fun AirLinkApp() {
    val dark = isSystemInDarkTheme()
    val scheme = if (isDynamicColorAvailable()) {
        if (dark) dynamicDarkColorScheme() else dynamicLightColorScheme()
    } else {
        if (dark) darkColorScheme() else lightColorScheme()
    }
    MaterialTheme(colorScheme = scheme) {
        Home()
        if (isDesktop()) PhoneAppWindows(appState())
        if (isDesktop()) AirLinkTray(appState())
    }
}

// The tray icon on a computer: AirLink goes on in the background when its window is closed (it
// keeps the device reachable), and comes back from here.
@Kompozable
fun AirLinkTray(app: App) {
    val clipboard = clipboardManager()
    val paused = app.sharingPaused()
    val online = app.pairedPeers().filter { it.online }
    val status = if (paused) TrayIconStatus.Paused else if (app.startError().isNotEmpty() || app.recentError()) TrayIconStatus.Error
                 else if (!app.ready()) TrayIconStatus.Starting else if (online.isEmpty()) TrayIconStatus.NoDevices
                 else TrayIconStatus.Connected
    val icon = remember(status) { statusTrayIcon(status) }
    val tip = if (paused) "AirLink · paylaşım duraklatıldı" else if (status == TrayIconStatus.Error) "AirLink · işlem hatası"
              else if (status == TrayIconStatus.Starting) "AirLink · başlatılıyor"
              else if (online.isEmpty()) "AirLink · bağlı cihaz yok" else "AirLink · ${online.size} cihaz bağlı"
    // A click on the icon opens the panel there; a click while it is open closes it (the panel loses
    // the focus first, then the click comes: one right after the panel closed is that click).
    var panel by remember { mutableStateOf<WindowPopup?>(null) }
    var panelClosedAt by remember { mutableStateOf(0L) }
    Tray(icon = icon, tooltip = tip, onAction = { at ->
            if (nowMs() - panelClosedAt > 400) panel = WindowPopup(anchor = at)
        }) {
        Item("AirLink'i aç", onClick = { setAppWindowVisible(true) })
        val paired = app.pairedPeers()
        if (paired.isNotEmpty()) {
            Separator()
            for (peer in paired) {
                Menu(peer.name, enabled = app.ready() && !paused && peer.online) {
                    Item("Dosya gönder…", onClick = { app.pickAndSend(peer.id) })
                    Item("Panoyu gönder", onClick = { app.sendClipboard(peer.id, clipboard.getText()) })
                    if (peer.platform == "android") {
                        Separator()
                        Item("Ekranı göster", onClick = { app.startScreen(peer.id); setAppWindowVisible(true) })
                    }
                }
            }
        }
        Separator()
        Item("Gelen dosyaları aç", onClick = { openDownloads(app.me().downloadDir) })
        Item(if (paused) "Paylaşımı sürdür" else if (!app.ready()) "Bağlantıyı yeniden dene" else "Paylaşımı duraklat",
             onClick = { app.setSharingPaused(!paused && app.ready()) })
        Separator()
        Item("Çık", onClick = { exitApplication() })
    }
    // The Plasma panel widget's click (`airlink --panel X Y`): the panel at that point, as a tray click.
    val request = app.panelRequest()
    LaunchedEffect(request.serial) {
        if (request.serial > 0 && nowMs() - panelClosedAt > 400) {
            panel = WindowPopup(anchor = if (request.x == 0 && request.y == 0) null else IntOffset(request.x, request.y))
        }
    }
    val open = panel
    if (open != null) {
        Window(onCloseRequest = {
                   panel = null
                   panelClosedAt = nowMs()
               }, popup = open!!, title = "AirLink", state = rememberWindowState(size = DpSize(380.dp, 600.dp))) {
            TrayPanel(app, onOpenApp = {
                panel = null
                panelClosedAt = nowMs()
                setAppWindowVisible(true)
            })
        }
    }
}

// Home's destinations.
constexpr int TAB_DEVICES = 0, TAB_MESSAGES = 1, TAB_PHONE = 2, TAB_APPS = 3, TAB_SETTINGS = 4, TAB_CLIPBOARD = 5;

static std::string tabTitle(int tab) {
    switch (tab) {
        case TAB_MESSAGES: return "Mesajlar";
        case TAB_PHONE: return "Telefon";
        case TAB_APPS: return "Uygulamalar";
        case TAB_SETTINGS: return "Ayarlar";
        case TAB_CLIPBOARD: return "Pano";
        default: return "AirLink";
    }
}

@Kompozable
fun Home() {
    val app = appState()
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val clipboard = clipboardManager()
    var tab by remember { mutableStateOf(0) }

    // The core's events, applied on the UI thread.
    LaunchedEffect(Unit) {
        var clipboardTicks = 0
        while (true) {
            app.pump()
            for (text in app.takeIncomingClipboard()) clipboard.setText(text)
            if (++clipboardTicks % 20 == 0) app.syncClipboard(clipboard.getText())
            for (message in app.takeNotices()) scope.launch { snackbar.showSnackbar(message) }
            delay(50)
        }
    }

    // Destinations: a computer also has a paired phone's messages, calls and apps.
    val desktopTabs = isDesktop() && app.ready()
    val activeCall = app.activeCall()
    var minimizedCall by remember { mutableStateOf(0L) }
    val callOpen = activeCall != null && isDesktop() && minimizedCall != activeCall!!.since
    val fullView = app.ready() && (app.desktopPeer().isNotEmpty() || app.screenPeer().isNotEmpty() || callOpen)
    if (!desktopTabs && tab != TAB_SETTINGS && tab != TAB_DEVICES && tab != TAB_CLIPBOARD) tab = TAB_DEVICES
    val navType = if (!app.ready() || fullView) NavigationSuiteType.None
                  else NavigationSuiteScaffoldDefaults.navigationSuiteType(currentWindowAdaptiveInfo())

    NavigationSuiteScaffold(
        navigationItems = {
            NavigationSuiteItem(selected = tab == TAB_DEVICES, onClick = { tab = TAB_DEVICES },
                                icon = { Icon(AppIcons.Devices, contentDescription = null) }, label = { Text("Cihazlar") })
            NavigationSuiteItem(selected = tab == TAB_CLIPBOARD, onClick = { tab = TAB_CLIPBOARD },
                                icon = { Icon(AppIcons.ContentPaste, contentDescription = null) }, label = { Text("Pano") })
            if (desktopTabs) {
                NavigationSuiteItem(selected = tab == TAB_MESSAGES, onClick = { tab = TAB_MESSAGES },
                                    icon = { Icon(AppIcons.Sms, contentDescription = null) }, label = { Text("Mesajlar") })
                NavigationSuiteItem(selected = tab == TAB_PHONE, onClick = { tab = TAB_PHONE },
                                    icon = { Icon(AppIcons.Phone, contentDescription = null) }, label = { Text("Telefon") })
                NavigationSuiteItem(selected = tab == TAB_APPS, onClick = { tab = TAB_APPS },
                                    icon = { Icon(AppIcons.Apps, contentDescription = null) }, label = { Text("Uygulamalar") })
            }
            NavigationSuiteItem(selected = tab == TAB_SETTINGS, onClick = { tab = TAB_SETTINGS },
                                icon = { Icon(Icons.Filled.Settings, contentDescription = null) }, label = { Text("Ayarlar") })
        },
        navigationSuiteType = navType,
        modifier = Modifier.fillMaxSize(),
    ) {
        Scaffold(
            topBar = {
                if (!callOpen) TopAppBar(
                    title = { Text(tabTitle(tab)) },
                    actions = {
                        if (tab == TAB_DEVICES && app.transfers().any { !it.active() }) {
                            TextButton(onClick = { app.clearFinishedTransfers() }) { Text("Temizle") }
                        }
                        if (tab == TAB_CLIPBOARD && app.clipboardHistory().isNotEmpty()) {
                            TextButton(onClick = { app.clearClipboardHistory() }) { Text("Temizle") }
                        }
                    },
                )
            },
            snackbarHost = { SnackbarHost(snackbar) },
        ) { padding ->
            Column(modifier = Modifier.fillMaxSize().padding(padding)) {
                if (activeCall != null && isDesktop() && !callOpen) ActiveCallBar(app, activeCall!!, onOpen = { minimizedCall = 0L })
                if (app.desktopViewer().isNotEmpty()) DesktopShareBar(app)
                Box(modifier = Modifier.fillMaxSize()) {
                    if (callOpen) {
                        CallScreen(app, activeCall!!, onMinimize = { minimizedCall = activeCall!!.since })
                    } else if (app.ready() && app.desktopPeer().isNotEmpty()) {
                        DesktopView(app)
                    } else if (app.ready() && app.screenPeer().isNotEmpty()) {
                        ScreenView(app)
                    } else if (app.ready()) {
                        if (tab == TAB_MESSAGES) MessagesScreen(app)
                        else if (tab == TAB_PHONE) PhoneScreen(app)
                        else if (tab == TAB_APPS) AppsScreen(app)
                        else if (tab == TAB_SETTINGS) SettingsScreen(app)
                        else if (tab == TAB_CLIPBOARD) ClipboardScreen(app, clipboard)
                        else DeviceList(app, clipboard, onOpenSettings = { tab = TAB_SETTINGS })
                    } else if (app.sharingPaused()) {
                        Column(modifier = Modifier.align(Alignment.Center), horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Paylaşım duraklatıldı")
                            TextButton(onClick = { app.setSharingPaused(false) }) { Text("Paylaşımı sürdür") }
                        }
                    } else if (app.startError().isNotEmpty()) {
                        Text("Başlatılamadı:\n${app.startError()}", modifier = Modifier.align(Alignment.Center).padding(24.dp))
                    } else {
                        CircularProgressIndicator(modifier = Modifier.align(Alignment.Center))
                    }
                }
            }
        }
    }

    IncomingPrompt(app)
    OutgoingPairingDialog(app)
    ShareDialog(app)
    IncomingCallDialog(app)
    DesktopRequestDialog(app)
}

// A paired phone's call in progress: who, for how long, and its controls.
@Kompozable
fun ActiveCallBar(app: App, call: ActiveCall, onOpen: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var now by remember { mutableStateOf(nowMs()) }
    LaunchedEffect(call.since) {
        while (true) {
            now = nowMs()
            delay(1000)
        }
    }
    Card(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp).clickable(onClick = onOpen),
         colors = CardDefaults.cardColors(containerColor = cs.tertiaryContainer)) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), verticalAlignment = Alignment.CenterVertically) {
            CallAvatar(call.title(), 40)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(call.title(), style = MaterialTheme.typography.titleMedium, color = cs.onTertiaryContainer,
                     maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${callStateLabel(call)} · ${elapsed(call.since, now)}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onTertiaryContainer)
            }
            IconButton(onClick = { app.callControl("call.mute", if (call.muted) "{\"on\":false}" else "{\"on\":true}") }) {
                Icon(if (call.muted) AppIcons.MicOff else AppIcons.Mic, contentDescription = if (call.muted) "Mikrofonu aç" else "Sessize al",
                     tint = cs.onTertiaryContainer)
            }
            TextButton(onClick = onOpen) { Text("Aç") }
            Box(modifier = Modifier.size(40.dp).background(cs.error, CircleShape).clickable(onClick = { app.callControl("call.hangup", "{}") }),
                contentAlignment = Alignment.Center) {
                Icon(AppIcons.CallEnd, contentDescription = "Kapat", tint = cs.onError, modifier = Modifier.size(20.dp))
            }
        }
    }
}

// A round control of the call screen, its label under it (as a phone's dialer has them).
@Kompozable
fun RoundAction(icon: ImageVector, label: String, selected: Boolean, enabled: Boolean, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val container = if (selected) cs.primary else cs.surfaceVariant
    val content = if (!enabled) cs.onSurface.copy(alpha = 0.38f) else if (selected) cs.onPrimary else cs.onSurfaceVariant
    Column(modifier = Modifier.width(96.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(modifier = (if (enabled) Modifier.clickable(onClick = onClick) else Modifier).size(64.dp)
                           .background(if (enabled) container else cs.surfaceVariant.copy(alpha = 0.5f), CircleShape),
            contentAlignment = Alignment.Center) {
            Icon(icon, contentDescription = label, tint = content)
        }
        Spacer(modifier = Modifier.height(6.dp))
        Text(label, style = MaterialTheme.typography.labelMedium, textAlign = TextAlign.Center,
             color = if (enabled) cs.onSurface else cs.onSurface.copy(alpha = 0.38f), maxLines = 2)
    }
}

@Kompozable
fun CallAvatar(title: String, size: Int) {
    val cs = MaterialTheme.colorScheme
    val letters = initials(title)
    Box(modifier = Modifier.size(size.dp).background(cs.primaryContainer, CircleShape), contentAlignment = Alignment.Center) {
        if (letters.isEmpty()) Icon(Icons.Filled.Person, contentDescription = null, tint = cs.onPrimaryContainer,
                                    modifier = Modifier.size((size / 2).dp))
        else Text(letters, color = cs.onPrimaryContainer,
                  style = if (size >= 80) MaterialTheme.typography.displaySmall else MaterialTheme.typography.titleMedium)
    }
}

// The call in progress, as a phone's in-call screen: who, how long, the controls, hang up.
@Kompozable
fun CallScreen(app: App, call: ActiveCall, onMinimize: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var now by remember { mutableStateOf(nowMs()) }
    var keypad by remember { mutableStateOf(false) }
    var typed by remember { mutableStateOf("") }
    var routes by remember { mutableStateOf(false) }
    LaunchedEffect(call.since) {
        while (true) {
            now = nowMs()
            delay(1000)
        }
    }
    Column(modifier = Modifier.fillMaxSize().background(cs.surfaceContainer), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onMinimize) { Icon(Icons.Filled.KeyboardArrowDown, contentDescription = "Küçült") }
            Text("Küçült", style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f))
            Text(app.peerName(call.peerId), style = MaterialTheme.typography.labelLarge, color = cs.onSurfaceVariant,
                 modifier = Modifier.padding(end = 12.dp))
        }
        Column(modifier = Modifier.weight(1f).widthIn(0.dp, 420.dp).padding(horizontal = 24.dp),
               horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            CallAvatar(call.title(), 112)
            Spacer(modifier = Modifier.height(20.dp))
            Text(call.title(), style = MaterialTheme.typography.headlineMedium, textAlign = TextAlign.Center, maxLines = 2)
            if (call.name.isNotEmpty() && call.number.isNotEmpty()) {
                Text(call.number, style = MaterialTheme.typography.bodyLarge, color = cs.onSurfaceVariant)
            }
            Spacer(modifier = Modifier.height(8.dp))
            Text(if (call.state == "dialing") "Aranıyor…" else "${callStateLabel(call)} · ${elapsed(call.since, now)}",
                 style = MaterialTheme.typography.titleMedium, color = if (call.held) cs.tertiary else cs.primary)
            Spacer(modifier = Modifier.height(4.dp))
            Text(if (app.callAudioOn()) app.callAudioStatus() else "Ses: ${routeLabel(call.route)} (telefon)",
                 style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, textAlign = TextAlign.Center)
            Spacer(modifier = Modifier.height(28.dp))
            if (keypad) {
                Text(if (typed.isEmpty()) " " else typed, style = MaterialTheme.typography.headlineSmall)
                Spacer(modifier = Modifier.height(8.dp))
                for (row in keypadRows()) {
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        for (key in row) {
                            Box(modifier = Modifier.clickable(onClick = {
                                    typed = typed + key
                                    app.callControl("call.dtmf", "{\"digit\":\"" + key + "\"}")
                                }).size(64.dp).background(cs.surfaceVariant, CircleShape),
                                contentAlignment = Alignment.Center) {
                                Text(key, style = MaterialTheme.typography.headlineSmall)
                            }
                        }
                    }
                    Spacer(modifier = Modifier.height(12.dp))
                }
                TextButton(onClick = { keypad = false }) { Text("Tuş takımını gizle") }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RoundAction(if (call.muted) AppIcons.MicOff else AppIcons.Mic, if (call.muted) "Sessizde" else "Sessiz",
                                call.muted, true, { app.callControl("call.mute", if (call.muted) "{\"on\":false}" else "{\"on\":true}") })
                    RoundAction(AppIcons.Dialpad, "Tuş takımı", false, call.fullControl, { keypad = true })
                    RoundAction(if (call.held) Icons.Filled.PlayArrow else Icons.Filled.Pause, if (call.held) "Sürdür" else "Beklet",
                                call.held, call.fullControl && call.canHold,
                                { app.callControl("call.hold", if (call.held) "{\"on\":false}" else "{\"on\":true}") })
                }
                Spacer(modifier = Modifier.height(16.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box {
                        RoundAction(if (call.route == "bluetooth") AppIcons.Bluetooth else AppIcons.VolumeUp,
                                    routeLabel(call.route), call.route == "speaker", call.routes.size > 1, { routes = true })
                        DropdownMenu(expanded = routes, onDismissRequest = { routes = false }) {
                            for (r in call.routes) {
                                DropdownMenuItem(text = { Text(routeLabel(r)) },
                                                 onClick = {
                                                     routes = false
                                                     app.callControl("call.route", routeBody(r))
                                                 },
                                                 leadingIcon = { if (call.route == r) Icon(Icons.Filled.Check, contentDescription = null) })
                            }
                        }
                    }
                    RoundAction(AppIcons.Computer, if (app.callAudioOn()) "Bilgisayarda" else "Bilgisayardan konuş",
                                app.callAudioOn(), app.canBridgeCallAudio(),
                                { if (app.callAudioOn()) app.stopCallAudio() else app.startCallAudio() })
                    RoundAction(AppIcons.Smartphone, "Telefona al", false, app.callAudioOn(), { app.stopCallAudio() })
                }
                Spacer(modifier = Modifier.height(16.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = { app.callControl("call.volume", "{\"delta\":-1}") }) {
                        Icon(AppIcons.VolumeDown, contentDescription = "Sesi azalt")
                    }
                    LinearProgressIndicator(progress = { if (call.volumeMax > 0) call.volume.toFloat() / call.volumeMax else 0f },
                                            modifier = Modifier.width(160.dp))
                    IconButton(onClick = { app.callControl("call.volume", "{\"delta\":1}") }) {
                        Icon(AppIcons.VolumeUp, contentDescription = "Sesi artır")
                    }
                }
            }
        }
        Box(modifier = Modifier.clickable(onClick = { app.callControl("call.hangup", "{}") }).size(72.dp).background(cs.error, CircleShape),
            contentAlignment = Alignment.Center) {
            Icon(AppIcons.CallEnd, contentDescription = "Kapat", tint = cs.onError, modifier = Modifier.size(32.dp))
        }
        Spacer(modifier = Modifier.height(12.dp))
        if (!call.fullControl) {
            Text("Bekletme ve tuş takımı için telefonda AirLink'in arama yardımcısı olarak bağlanması gerekiyor.",
                 style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant, textAlign = TextAlign.Center,
                 modifier = Modifier.padding(horizontal = 24.dp))
        }
        Spacer(modifier = Modifier.height(20.dp))
    }
}

// A paired phone rings: who, and answer (on the phone) or decline from here.
// Previews (KOMPOZ_PREVIEW=CallScreenPreview): the call screen with a made-up call.
@Preview(widthDp = 880, heightDp = 820)
@Kompozable
fun CallScreenPreview() {
    MaterialTheme {
        CallScreen(appState(), ActiveCall(peerId = "", number = "+90 555 123 45 67", name = "Ayşe Yılmaz", state = "active",
                   canHold = true, route = "earpiece", routes = listOf("earpiece", "speaker", "bluetooth"), volume = 4, volumeMax = 7,
                   since = nowMs() - 83000, fullControl = true), onMinimize = {})
    }
}

@Preview(widthDp = 420, heightDp = 620)
@Kompozable
fun IncomingCallPreview() {
    MaterialTheme {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            IncomingCallCard(appState(), IncomingCall(peerId = "", number = "+90 555 123 45 67", name = "Ayşe Yılmaz"))
        }
    }
}

// A paired phone rings: answer (here, or on the phone), decline, or decline with a message.
@Kompozable
fun IncomingCallDialog(app: App) {
    val c = app.incomingCall()
    if (c != null) {
        Dialog(onDismissRequest = { app.dismissCall() }) { IncomingCallCard(app, c!!) }
    }
}

@Kompozable
fun IncomingCallCard(app: App, call: IncomingCall) {
    val cs = MaterialTheme.colorScheme
    var replies by remember { mutableStateOf(false) }
            Card(modifier = Modifier.widthIn(0.dp, 380.dp)) {
                Column(modifier = Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("Gelen arama · ${app.peerName(call.peerId)}", style = MaterialTheme.typography.labelLarge, color = cs.primary)
                    Spacer(modifier = Modifier.height(20.dp))
                    CallAvatar(call.title(), 96)
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(call.title(), style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center, maxLines = 2)
                    if (call.name.isNotEmpty() && call.number.isNotEmpty()) {
                        Text(call.number, style = MaterialTheme.typography.bodyLarge, color = cs.onSurfaceVariant)
                    }
                    Spacer(modifier = Modifier.height(28.dp))
                    if (replies) {
                        for (text in quickReplies()) {
                            OutlinedButton(onClick = {
                                    app.rejectCall()
                                    if (call.number.isNotEmpty()) app.sendSms(call.number, text)
                                }, modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) { Text(text) }
                        }
                        TextButton(onClick = { replies = false }) { Text("Geri") }
                    } else {
                        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Box(modifier = Modifier.clickable(onClick = { app.rejectCall() }).size(68.dp).background(cs.error, CircleShape),
                                    contentAlignment = Alignment.Center) {
                                    Icon(AppIcons.CallEnd, contentDescription = "Reddet", tint = cs.onError, modifier = Modifier.size(30.dp))
                                }
                                Spacer(modifier = Modifier.height(6.dp))
                                Text("Reddet", style = MaterialTheme.typography.labelMedium)
                            }
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Box(modifier = Modifier.clickable(onClick = { app.answerCall() }).size(68.dp).background(Color(0xFF2E7D32), CircleShape),
                                    contentAlignment = Alignment.Center) {
                                    Icon(AppIcons.Phone, contentDescription = "Yanıtla", tint = Color.White, modifier = Modifier.size(30.dp))
                                }
                                Spacer(modifier = Modifier.height(6.dp))
                                Text("Yanıtla", style = MaterialTheme.typography.labelMedium)
                            }
                        }
                        Spacer(modifier = Modifier.height(20.dp))
                        if (app.canBridgeCallAudio()) {
                            FilledTonalButton(onClick = { app.answerCallOnComputer() }, modifier = Modifier.fillMaxWidth()) {
                                Icon(AppIcons.Computer, contentDescription = null, modifier = Modifier.size(18.dp))
                                Spacer(modifier = Modifier.width(8.dp))
                                Text("Yanıtla, bilgisayardan konuş")
                            }
                        }
                        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            if (call.number.isNotEmpty()) {
                                TextButton(onClick = { replies = true }) {
                                    Icon(AppIcons.Sms, contentDescription = null, modifier = Modifier.size(18.dp))
                                    Spacer(modifier = Modifier.width(6.dp))
                                    Text("Mesajla reddet")
                                }
                            }
                            TextButton(onClick = { app.dismissCall() }) { Text("Gizle") }
                        }
                    }
                }
            }
}

// Files handed to AirLink from outside (a file manager's "Send with AirLink", Android's share
// sheet): which device gets them.
@Kompozable
fun ShareDialog(app: App) {
    val files = app.share()
    if (files.isNotEmpty() && app.ready()) {
        val paired = app.pairedPeers()
        AlertDialog(
            onDismissRequest = { app.cancelShare() },
            icon = { Icon(AppIcons.Upload, contentDescription = null) },
            title = { Text(if (files.size == 1) "${fileName(files.first())} gönderilsin" else "${files.size} dosya gönderilsin") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (paired.isEmpty()) {
                        Text("Henüz eşleşmiş cihaz yok. Önce bir cihazla eşleşin.")
                    } else {
                        Text("Hangi cihaza?", color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (app.canFling() && app.flingEnabled() && app.flingTarget().isNotEmpty()) {
                            Text("Ya da telefonu ${app.peerName(app.flingTarget())} cihazına doğru fırlatın.",
                                 style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
                        }
                        for (peer in paired) {
                            OutlinedButton(onClick = { app.shareTo(peer.id) }, modifier = Modifier.fillMaxWidth(), enabled = peer.online) {
                                Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(18.dp))
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(if (peer.online) peer.name else "${peer.name} (çevrimdışı)", modifier = Modifier.weight(1f))
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { app.cancelShare() }) { Text("Vazgeç") } },
        )
    }
}

@Kompozable
fun DeviceList(app: App, clipboard: ClipboardManager, onOpenSettings: () -> Unit) {
    val transfers = app.transfers()
    val paired = app.pairedPeers()
    val nearby = app.nearbyPeers()
    val notifications = app.phoneNotifications()
    var forgetting by remember { mutableStateOf("") }
    var replyTo by remember { mutableStateOf(PhoneNotification()) }
    var replyText by remember { mutableStateOf("") }
    var manualAddress by remember { mutableStateOf("") }
    var pairByAddress by remember { mutableStateOf(false) }
    var allTransfers by remember { mutableStateOf(false) }

    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        val missing = missingPermissions(app)
        if (missing > 0) {
            item { MissingPermissionsBanner(missing, onOpenSettings) }
        }

        item { SectionTitle("Eşleşmiş cihazlar") }
        if (paired.isEmpty()) {
            item { NoPairedDevices() }
        } else {
            items(paired) { peer ->
                PairedPeerCard(
                    peer = peer,
                    onSendFiles = { app.pickAndSend(peer.id) },
                    onDropFiles = { paths, uris -> app.sendDropped(peer.id, paths, uris) },
                    onSendClipboard = { app.sendClipboard(peer.id, clipboard.getText()) },
                    onForget = { forgetting = peer.id },
                    onShowScreen = { app.startScreen(peer.id) },
                    onControl = { app.startDesktop(peer.id) },
                    onTrust = { app.setTrusted(peer.id, it) },
                )
            }
        }

        // The running ones and the latest few finished ones; the rest on request.
        if (transfers.isNotEmpty()) {
            val shown = if (allTransfers) transfers else recentTransfers(transfers, 3)
            item { SectionTitle("Aktarımlar") }
            items(shown) { t -> TransferRow(t, app.peerName(t.peerId)) }
            if (shown.size < transfers.size) {
                item {
                    TextButton(onClick = { allTransfers = true }) { Text("Tümünü göster (${transfers.size})") }
                }
            }
        }

        if (notifications.isNotEmpty()) {
            item { SectionTitle("Telefon bildirimleri") }
            items(notifications) { n ->
                PhoneNotificationCard(n, app.peerName(n.peerId),
                    onDismiss = { app.dismissPhoneNotification(n) },
                    onReply = { replyTo = n })
            }
        }

        item { SectionTitle("Yakındaki cihazlar") }
        if (nearby.isEmpty()) {
            item { Hint("Aranıyor… Diğer cihazda AirLink açık ve aynı ağda olmalı.", loading = true) }
        } else {
            items(nearby) { peer -> NearbyPeerCard(peer, onPair = { app.pair(peer.id) }) }
        }
        item {
            if (pairByAddress) {
                OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("IP adresiyle eşleştir", style = MaterialTheme.typography.titleSmall)
                        Text("Ağ keşfi çalışmıyorsa diğer cihazın IP adresini yazın. Varsayılan bağlantı noktası 47820.",
                             style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = manualAddress, onValueChange = { manualAddress = it }, singleLine = true,
                                              label = "IP adresi veya IP:port", modifier = Modifier.weight(1f))
                            Spacer(modifier = Modifier.width(8.dp))
                            Button(onClick = { app.pairAddress(manualAddress) }, enabled = manualAddress.isNotBlank()) { Text("Eşleştir") }
                        }
                    }
                }
            } else {
                TextButton(onClick = { pairByAddress = true }) { Text("Cihaz görünmüyor mu? IP adresiyle eşleştir") }
            }
        }
    }

    if (replyTo.key.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { replyTo = PhoneNotification(); replyText = "" },
            title = { Text("${replyTo.title} yanıtla") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(replyTo.text, style = MaterialTheme.typography.bodySmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
                    OutlinedTextField(value = replyText, onValueChange = { replyText = it }, modifier = Modifier.fillMaxWidth(), label = "Yanıt")
                }
            },
            confirmButton = {
                TextButton(onClick = { app.replyPhoneNotification(replyTo, replyText); replyTo = PhoneNotification(); replyText = "" }) { Text("Gönder") }
            },
            dismissButton = { TextButton(onClick = { replyTo = PhoneNotification(); replyText = "" }) { Text("Vazgeç") } },
        )
    }

    if (forgetting.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { forgetting = "" },
            title = { Text("${app.peerName(forgetting)} unutulsun mu?") },
            text = { Text("Tekrar dosya paylaşmak için yeniden eşleşmeniz gerekecek.") },
            confirmButton = { TextButton(onClick = { app.forget(forgetting); forgetting = "" }) { Text("Unut") } },
            dismissButton = { TextButton(onClick = { forgetting = "" }) { Text("Vazgeç") } },
        )
    }
}

// Permissions this device still needs for what it can share (the phone's, mostly).
@Kompozable
fun missingPermissions(app: App): Int {
    var n = 0
    if (app.canShareNotifications() && !app.notificationAccess()) n++
    if (app.canShareSms() && !app.smsAccess()) n++
    if (app.canShareCalls() && !app.callAccess()) n++
    if (app.canShareScreen() && !app.screenControl()) n++
    return n
}

// What went through the clipboard share: copy it again, open its link, or remove it.
@Kompozable
fun ClipboardScreen(app: App, clipboard: ClipboardManager) {
    val items = app.clipboardHistory()
    if (items.isEmpty()) {
        EmptyState(AppIcons.ContentPaste, "Pano geçmişi boş",
                   "Eşleşmiş cihazlarla paylaşılan metinler ve bağlantılar burada görünür.")
        return
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(items) { item -> ClipboardRow(app, item, clipboard) }
    }
}

@Kompozable
fun ClipboardRow(app: App, item: ClipboardItem, clipboard: ClipboardManager) {
    val cs = MaterialTheme.colorScheme
    val links = uriHandler()
    val link = firstLink(item.text)
    val peer = app.peerName(item.peerId)
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, end = 8.dp, bottom = 4.dp)) {
            Text(if (item.incoming) "$peer cihazından · ${clockTime(item.time)}" else "$peer cihazına · ${clockTime(item.time)}",
                 style = MaterialTheme.typography.labelMedium, color = cs.primary)
            Spacer(modifier = Modifier.height(4.dp))
            Text(item.text, style = MaterialTheme.typography.bodyMedium, maxLines = 6, overflow = TextOverflow.Ellipsis,
                 modifier = Modifier.padding(end = 8.dp))
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (link.isNotEmpty()) TextButton(onClick = { links.openUri(link) }) { Text("Bağlantıyı aç") }
                TextButton(onClick = { clipboard.setText(item.text) }) { Text("Kopyala") }
                TextButton(onClick = { app.removeClipboardItem(item.id) }) { Text("Sil") }
            }
        }
    }
}

@Kompozable
fun MissingPermissionsBanner(count: Int, onOpenSettings: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.secondaryContainer), modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(if (count == 1) "Bir izin eksik; bazı özellikler kapalı." else "$count izin eksik; bazı özellikler kapalı.",
                 modifier = Modifier.weight(1f), color = cs.onSecondaryContainer)
            TextButton(onClick = onOpenSettings) { Text("Ayarlar") }
        }
    }
}

// This device, its permissions and its features.
@Kompozable
fun SettingsScreen(app: App) {
    val paired = app.pairedPeers()
    val permissions = app.canShareNotifications() || app.canShareSms() || app.canShareCalls() || app.canShareScreen() ||
                      needsBackgroundAutostartSettings()
    val phoneAudio = !isDesktop() && app.canPlayMediaAudio() && paired.any { it.online && it.platform != "android" }
    val features = phoneAudio || app.canFling() || app.canUseAdvancedMode()
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { ThisDeviceCard(app.me()) }
        if (permissions) {
            item { SectionTitle("İzinler") }
            if (needsBackgroundAutostartSettings()) item { BackgroundAutostartCard() }
            if (app.canShareNotifications()) item { NotificationAccessCard(app) }
            if (app.canShareSms()) item { SmsAccessCard(app) }
            if (app.canShareCalls()) item { CallAccessCard(app) }
            if (app.canShareScreen()) item { ScreenControlCard(app) }
        }
        if (features) {
            item { SectionTitle("Özellikler") }
            if (phoneAudio) item { PhoneAudioCard(app) }
            if (app.canFling()) item { FlingCard(app) }
            if (app.canUseAdvancedMode()) item { AdvancedModeCard(app) }
        }
    }
}

@Kompozable
fun ThisDeviceCard(me: DeviceInfo) {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.primaryContainer)) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(52.dp).background(cs.primary, CircleShape), contentAlignment = Alignment.Center) {
                Icon(AppIcons.forPlatform(me.platform), contentDescription = null, tint = cs.onPrimary)
            }
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(me.name, style = MaterialTheme.typography.titleLarge, color = cs.onPrimaryContainer)
                Text("Bu cihaz · ${shortId(me.id)} · port ${me.port}", color = cs.onPrimaryContainer)
                Text("Gelen dosyalar: ${me.downloadDir}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onPrimaryContainer, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Kompozable
fun BackgroundAutostartCard() {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Arka planda bağlı kal", style = MaterialTheme.typography.titleMedium)
            Text("Xiaomi, uygulama süreci kapandığında AirLink'in yeniden başlamasını engelleyebilir. Ayarlar'da AirLink için arka planda otomatik başlatmayı açın.",
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            FilledTonalButton(onClick = { openBackgroundAutostartSettings() }) { Text("Otomatik başlatma ayarları") }
        }
    }
}

@Kompozable
fun NotificationAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Bildirimleri bilgisayarda göster", style = MaterialTheme.typography.titleMedium)
            if (app.notificationAccess()) {
                Text("Bu telefonun bildirimleri eşleşmiş bilgisayarlara gidiyor; oradan kapatılabilir ya da yanıtlanabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Bilgisayarlarınız bu telefonun bildirimlerini görsün ve mesajları oradan yanıtlayın. Ayarlar'da AirLink'e bildirim erişimi verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openNotificationAccessSettings() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun SmsAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("SMS'leri bilgisayardan kullan", style = MaterialTheme.typography.titleMedium)
            if (app.smsAccess()) {
                Text("Eşleşmiş bilgisayarlar bu telefonun SMS'lerini görebilir ve gönderebilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Bilgisayarınızdan mesajlarınızı okuyun ve SMS gönderin. AirLink'e SMS (ve isimler için kişiler) izni verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestSmsAccess() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun CallAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Aramaları bilgisayarda göster", style = MaterialTheme.typography.titleMedium)
            if (app.callAccess()) {
                Text("Telefon çalınca eşleşmiş bilgisayarlar arayanı görür; oradan yanıtlanabilir, reddedilebilir ya da numara aratılabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Kim aradığını bilgisayarda görün, aramayı oradan reddedin ve arama kaydınıza ulaşın. AirLink'e telefon ve arama izinleri verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestCallAccess() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun ScreenControlCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Ekranı bilgisayardan kontrol et", style = MaterialTheme.typography.titleMedium)
            if (app.screenControl()) {
                Text("Bilgisayar ekranınızı görmek istediğinde onayınız sorulur; paylaşım sürerken oradan dokunabilir ve kaydırabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Ekranınız bilgisayarda görünebilir (her seferinde onayınızla). Oradan dokunmak için Erişilebilirlik'te \"AirLink ekran kontrolü\"nü açın.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openScreenControlSettings() }) { Text("Erişilebilirliği aç") }
            }
        }
    }
}

@Kompozable
fun PhoneAudioCard(app: App) {
    val computers = app.pairedPeers().filter { it.online && it.platform != "android" }
    val target = if (app.mediaAudioTarget().isEmpty()) app.flingTarget() else app.mediaAudioTarget()
    val bluetoothTarget = computers.any { it.id == target && (it.platform == "linux" || it.platform == "windows") }
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Telefon sesini paylaş", style = MaterialTheme.typography.titleMedium)
            Text("Hedef bilgisayar", style = MaterialTheme.typography.bodySmall)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (computer in computers) {
                    FilterChip(selected = target == computer.id, onClick = { app.setMediaAudioTarget(computer.id) },
                               label = { Text(computer.name) })
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Otomatik bağlan", modifier = Modifier.weight(1f))
                Switch(checked = app.audioAutoConnect(), onCheckedChange = { app.setAudioAutoConnect(it) })
            }
            if (app.audioAutoConnect()) {
                Text("Arama sesi açıksa görüşme başladığında Bluetooth bağlantısı kurulur.",
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (bluetoothTarget) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Arama sesini paylaş", modifier = Modifier.weight(1f))
                    Switch(checked = app.callAudioOn(), onCheckedChange = { if (it) app.startCallAudio() else app.stopCallAudio() })
                }
                if (app.callAudioOn() && !app.audioAutoConnect()) {
                    OutlinedButton(onClick = { app.shareCurrentCallAudio() }) { Text("Süren görüşmenin sesini aktar") }
                }
            } else {
                Text("Arama sesi paylaşımı Linux ve Windows bilgisayarda kullanılabilir.",
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Ekran paylaşımı sesi", modifier = Modifier.weight(1f))
                Switch(checked = app.screenShareAudio(), onCheckedChange = { app.setScreenShareAudio(it) })
            }
            Text("Tek uygulama ve tam ekran paylaşımında telefonun medya çıkışını gönderir.", style = MaterialTheme.typography.bodySmall,
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Müzik oynatılırken ekran sesini durdur", modifier = Modifier.weight(1f))
                Switch(checked = app.pauseScreenAudioDuringMusic(), onCheckedChange = { app.setPauseScreenAudioDuringMusic(it) })
            }
            if (app.pauseScreenAudioDuringMusic() && !app.notificationAccess() && !app.mediaAudioEnabled()) {
                OutlinedButton(onClick = { app.openNotificationAccessSettings() }) { Text("Oynatmayı algılamak için bildirim erişimini aç") }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Oynatıcı sesini aktar", modifier = Modifier.weight(1f))
                Switch(checked = app.mediaAudioEnabled(), enabled = bluetoothTarget,
                       onCheckedChange = { app.setMediaAudioEnabled(it) })
            }
            if (app.mediaAudioEnabled()) {
                Text(app.mediaAudioStatus(), style = MaterialTheme.typography.bodySmall,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (!app.notificationAccess()) {
                    OutlinedButton(onClick = { app.openNotificationAccessSettings() }) { Text("Bildirim erişimini aç") }
                }
            }
            OutlinedButton(onClick = { app.chooseMediaAudioPlayers() }) { Text("Oynatıcıları seç") }
            Text("Seçili bir oynatıcı çalarken eşleşmiş bilgisayara Bluetooth medya sesi gider.",
                 style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

// ---- The tray panel (a click on the tray icon) -------------------------------------------------

// What AirLink offers without its window: the devices and their quick actions, the phone's media,
// messages, notifications, calls and contacts.
@Kompozable
fun TrayPanel(app: App, onOpenApp: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val clip = clipboard()
    val text = clipboardManager()
    val online = app.pairedPeers().filter { it.online }
    val phone = online.any { it.platform == "android" }
    var copied by remember { mutableStateOf(copiedFiles(clip)) }
    var query by remember { mutableStateOf("") }
    var replyThread by remember { mutableStateOf("") }
    var replyText by remember { mutableStateOf("") }
    // Ctrl+V sends what is copied (files, else text) to the chosen device: the panel closes when
    // anything else is clicked, so dragging onto it can't work.
    var chosen by remember { mutableStateOf("") }
    var pasted by remember { mutableStateOf("") }
    val target = if (online.any { it.id == chosen }) chosen
                 else if (online.any { it.platform == "android" }) online.first { it.platform == "android" }.id
                 else if (online.isNotEmpty()) online.first().id else ""
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    LaunchedEffect(phone) {
        if (phone) {
            app.requestPhoneMedia()
            app.requestSmsThreads()
            app.requestCallLog()
        }
    }
    LaunchedEffect(Unit) {
        while (true) {
            copied = copiedFiles(clip)
            delay(1500)
        }
    }
    LazyColumn(modifier = Modifier.fillMaxSize().background(cs.surfaceContainer)
                   .onKeyEvent { e ->
                       if (e.type != KeyEventType.KeyDown || !e.isCtrlPressed || e.isAltPressed || e.key != Key.V) false
                       else if (target.isEmpty()) {
                           pasted = "Gönderilecek çevrimiçi cihaz yok"
                           true
                       } else {
                           val files = copiedFiles(clip)
                           val copiedText = text.getText()
                           if (files.isNotEmpty()) {
                               app.sendFiles(target, files)
                               pasted = if (files.size == 1) "1 dosya ${app.peerName(target)} cihazına gönderildi"
                                        else "${files.size} dosya ${app.peerName(target)} cihazına gönderildi"
                           } else if (copiedText.isNotEmpty()) {
                               app.sendClipboard(target, copiedText)
                               pasted = "Metin ${app.peerName(target)} cihazının panosuna gönderildi"
                           } else {
                               pasted = "Panoda gönderilecek dosya ya da metin yok"
                           }
                           true
                       }
                   }
                   .focusRequester(focus)
                   .focusable(),
               contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        item {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("AirLink", style = MaterialTheme.typography.titleMedium)
                    Text(if (online.isEmpty()) "Bağlı cihaz yok" else "${online.size} cihaz bağlı",
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                }
                TextButton(onClick = onOpenApp) { Text("AirLink'i aç") }
            }
        }
        if (target.isNotEmpty()) {
            item {
                Text(if (pasted.isNotEmpty()) pasted
                     else "Ctrl+V: kopyaladığınız dosyalar ya da metin ${app.peerName(target)} cihazına gider",
                     style = MaterialTheme.typography.bodySmall, color = if (pasted.isNotEmpty()) cs.primary else cs.onSurfaceVariant,
                     modifier = Modifier.padding(horizontal = 4.dp))
            }
        }
        // Devices
        if (online.isEmpty()) {
            item {
                Text("Eşleşmiş cihazlarınızdan biri çevrimiçi olunca burada görünür.", style = MaterialTheme.typography.bodySmall,
                     color = cs.onSurfaceVariant, modifier = Modifier.padding(4.dp))
            }
        }
        for (peer in online) {
            item { TrayDevice(app, peer, copied, text, peer.id == target && online.size > 1, onChoose = { chosen = peer.id }) }
        }
        // Media
        val media = app.phoneMedia()
        if (media.active && phone) {
            item { TrayMedia(app, media) }
        }
        // Messages
        val threads = app.smsThreads()
        if (phone && threads.isNotEmpty()) {
            item { TraySection("Son mesajlar") }
            items(threads.take(3)) { t ->
                Column(modifier = Modifier.fillMaxWidth()) {
                    Row(modifier = Modifier.fillMaxWidth().clickable(onClick = {
                            replyThread = if (replyThread == t.address) "" else t.address
                            replyText = ""
                        }).padding(4.dp, 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        CallAvatar(t.title(), 32)
                        Spacer(modifier = Modifier.width(10.dp))
                        Column(modifier = Modifier.weight(1f)) {
                            Text(t.title(), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(t.snippet, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 1,
                                 overflow = TextOverflow.Ellipsis)
                        }
                        Text(clockTime(t.date), style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant)
                    }
                    if (replyThread == t.address) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = replyText, onValueChange = { replyText = it }, singleLine = true, label = "Yanıt",
                                              modifier = Modifier.weight(1f))
                            IconButton(onClick = {
                                    app.sendSms(t.address, replyText)
                                    replyThread = ""
                                    replyText = ""
                                }, enabled = replyText.isNotBlank()) { Icon(Icons.Filled.Send, contentDescription = "Gönder") }
                        }
                    }
                }
            }
        }
        // Notifications
        val notifications = app.phoneNotifications()
        if (notifications.isNotEmpty()) {
            item { TraySection("Bildirimler") }
            items(notifications.take(4)) { n ->
                Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("${n.app} · ${clockTime(n.time)}", style = MaterialTheme.typography.labelSmall, color = cs.primary)
                        Text(if (n.title.isEmpty()) n.text else n.title, style = MaterialTheme.typography.labelLarge, maxLines = 1,
                             overflow = TextOverflow.Ellipsis)
                        if (n.title.isNotEmpty() && n.text.isNotEmpty()) {
                            Text(n.text, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2,
                                 overflow = TextOverflow.Ellipsis)
                        }
                    }
                    IconButton(onClick = { app.dismissPhoneNotification(n) }) { Icon(Icons.Filled.Close, contentDescription = "Kapat") }
                }
            }
        }
        // Calls
        val calls = app.callLog()
        if (phone && calls.isNotEmpty()) {
            item { TraySection("Son çağrılar") }
            items(calls.take(4)) { c ->
                Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    CallAvatar(c.title(), 32)
                    Spacer(modifier = Modifier.width(10.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(c.title(), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis,
                             color = if (c.type == "missed") cs.error else cs.onSurface)
                        Text("${callTypeLabel(c.type)} · ${clockTime(c.date)}", style = MaterialTheme.typography.bodySmall,
                             color = cs.onSurfaceVariant)
                    }
                    IconButton(onClick = { app.dial(c.number) }) { Icon(AppIcons.Phone, contentDescription = "Ara") }
                }
            }
        }
        // Contacts
        if (phone) {
            item { TraySection("Kişiler") }
            item {
                OutlinedTextField(value = query, onValueChange = {
                        query = it
                        if (it.length >= 2) app.searchContacts(it)
                    }, singleLine = true, label = "Kişi ya da numara ara", modifier = Modifier.fillMaxWidth())
            }
            if (query.length >= 2) {
                items(app.contactResults().take(5)) { c ->
                    Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                        CallAvatar(c.name, 32)
                        Spacer(modifier = Modifier.width(10.dp))
                        Column(modifier = Modifier.weight(1f)) {
                            Text(c.name, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}", style = MaterialTheme.typography.bodySmall,
                                 color = cs.onSurfaceVariant)
                        }
                        IconButton(onClick = { app.dial(c.number) }) { Icon(AppIcons.Phone, contentDescription = "Ara") }
                        IconButton(onClick = { replyThread = c.number; replyText = "" }) { Icon(AppIcons.Sms, contentDescription = "Mesaj") }
                    }
                    if (replyThread == c.number) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = replyText, onValueChange = { replyText = it }, singleLine = true, label = "Mesaj",
                                              modifier = Modifier.weight(1f))
                            IconButton(onClick = {
                                    app.sendSms(c.number, replyText)
                                    replyThread = ""
                                    replyText = ""
                                }, enabled = replyText.isNotBlank()) { Icon(Icons.Filled.Send, contentDescription = "Gönder") }
                        }
                    }
                }
                if (query.any { it.isDigit() }) {
                    item {
                        TextButton(onClick = { app.dial(query) }) { Text("$query numarasını ara") }
                    }
                }
            }
        }
    }
}

@Preview(widthDp = 380, heightDp = 560)
@Kompozable
fun TrayPanelPreview() {
    MaterialTheme { TrayPanel(appState(), onOpenApp = {}) }
}

@Kompozable
fun TraySection(title: String) {
    Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
         modifier = Modifier.padding(start = 4.dp, top = 10.dp, bottom = 2.dp))
}

// A connected device in the panel: its actions, and a place to drop files on.
@Kompozable
fun TrayDevice(app: App, peer: Peer, copied: List<String>, text: ClipboardManager, chosen: Boolean, onChoose: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var dragging by remember { mutableStateOf(false) }
    val drop = Modifier.dragAndDropTarget(
        shouldStartDragAndDrop = { e -> carriesFiles(e) },
        target = DragAndDropTarget(
            onDrop = { e ->
                dragging = false
                app.sendDropped(peer.id, droppedFiles(e), droppedUris(e))
                true
            },
            onEntered = { dragging = true },
            onExited = { dragging = false },
            onEnded = { dragging = false },
        ),
    )
    // With more than one device, a click on a card makes it the one Ctrl+V sends to (outlined).
    Card(modifier = drop.fillMaxWidth().clickable(onClick = onChoose)
                        .then(if (chosen) Modifier.border(2.dp, cs.primary, RoundedCornerShape(12.dp)) else Modifier),
         colors = CardDefaults.cardColors(containerColor = if (dragging) cs.secondaryContainer else cs.surface)) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box {
                    Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(28.dp))
                    Box(modifier = Modifier.align(Alignment.TopEnd)) { OnlineDot(true) }
                }
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(peer.name, style = MaterialTheme.typography.titleSmall)
                    Text(if (dragging) "Göndermek için bırakın" else if (chosen) "Ctrl+V buraya gönderir" else "Dosyaları buraya sürükleyin",
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                }
                if (peer.platform == "android") {
                    IconButton(onClick = {
                            app.startScreen(peer.id)
                            setAppWindowVisible(true)
                        }) { Icon(AppIcons.Smartphone, contentDescription = "Ekranı göster") }
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(start = 40.dp)) {
                AssistChip(onClick = { app.pickAndSend(peer.id) }, label = { Text("Dosya") },
                           leadingIcon = { Icon(AppIcons.UploadFile, contentDescription = null, modifier = Modifier.size(16.dp)) })
                AssistChip(onClick = { app.sendClipboard(peer.id, text.getText()) }, label = { Text("Pano") },
                           leadingIcon = { Icon(AppIcons.ContentPaste, contentDescription = null, modifier = Modifier.size(16.dp)) })
                if (copied.isNotEmpty()) {
                    AssistChip(onClick = { app.sendFiles(peer.id, copied) },
                               label = { Text(if (copied.size == 1) "Kopyalanan dosya" else "Kopyalanan ${copied.size} dosya") },
                               leadingIcon = { Icon(AppIcons.Upload, contentDescription = null, modifier = Modifier.size(16.dp)) })
                }
            }
        }
    }
}

// What plays on the phone: its cover, title and the transport buttons.
@Kompozable
fun TrayMedia(app: App, media: PhoneMedia) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (media.art.width > 0) {
                Image(media.art, contentDescription = null, modifier = Modifier.size(56.dp).clip(RoundedCornerShape(8.dp)))
            } else {
                Box(modifier = Modifier.size(56.dp).background(cs.primaryContainer, RoundedCornerShape(8.dp)), contentAlignment = Alignment.Center) {
                    Icon(AppIcons.VolumeUp, contentDescription = null, tint = cs.onPrimaryContainer)
                }
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(media.title, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(if (media.artist.isEmpty()) media.app else "${media.artist} · ${media.app}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            IconButton(onClick = { app.phoneMediaControl("previous") }, enabled = media.canPrevious) {
                Icon(Icons.Filled.KeyboardArrowLeft, contentDescription = "Önceki")
            }
            IconButton(onClick = { app.phoneMediaControl("toggle") }) {
                Icon(if (media.playing) Icons.Filled.Pause else Icons.Filled.PlayArrow, contentDescription = if (media.playing) "Duraklat" else "Çal")
            }
            IconButton(onClick = { app.phoneMediaControl("next") }, enabled = media.canNext) {
                Icon(Icons.Filled.KeyboardArrowRight, contentDescription = "Sonraki")
            }
        }
    }
}

// Fling to send (phase 4B): push the phone toward the computer.
@Kompozable
fun FlingCard(app: App) {
    val target = app.flingTarget()
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Fırlatarak gönder", style = MaterialTheme.typography.titleMedium)
                    Text("Telefonu bilgisayara doğru kısa ve hızlı bir hareketle itin. AirLink açıkken paylaşılan dosyalar ya da kopyalanan metin, başka bir uygulama açıkken o uygulamanın gösterdiği içerik gider: galerideki fotoğraf ya da video, galeri ve dosya yöneticisinde seçili dosyalar, çalan müzik.",
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                    Text(if (target.isEmpty()) "Hedef: çevrimiçi bilgisayar yok" else "Hedef: ${app.peerName(target)}",
                         style = MaterialTheme.typography.labelMedium)
                }
                Spacer(modifier = Modifier.width(12.dp))
                Switch(checked = app.flingEnabled(), onCheckedChange = { app.setFlingEnabled(it) })
            }
            if (app.flingEnabled() && !app.screenControl()) {
                Text("Diğer uygulamalardan göndermek için Erişilebilirlik'te \"AirLink ekran kontrolü\" açık olmalı.",
                     style = MaterialTheme.typography.bodySmall, color = cs.primary)
                OutlinedButton(onClick = { app.openScreenControlSettings() }) { Text("Erişilebilirliği aç") }
            }
            if (app.flingEnabled() && !app.flingMediaAccess()) {
                Text("Galerideki fotoğrafı doğrudan bulmak için fotoğraf, video ve müzik erişimi gerekir.",
                     style = MaterialTheme.typography.bodySmall, color = cs.primary)
                OutlinedButton(onClick = { app.requestFlingMediaAccess() }) { Text("Medya erişimi ver") }
            }
        }
    }
}

@Kompozable
fun AdvancedModeCard(app: App) {
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Gelişmiş mod", style = MaterialTheme.typography.titleMedium)
            Text("Uygulamaları bilgisayarda kendi pencerelerinde açın; gerçek dokunuş ve klavye. Kablosuz hata ayıklama ile bir kez kurulur.",
                 color = cs.onSurfaceVariant)
            if (app.advancedStatus().isNotEmpty()) {
                Text(app.advancedStatus(), style = MaterialTheme.typography.bodySmall,
                     color = if (app.advancedReady()) Color(0xFF2E7D32) else cs.primary)
            }
            OutlinedTextField(value = app.advancedPort(), onValueChange = { app.setAdvancedPort(it) },
                              singleLine = true, label = "Kablosuz hata ayıklama portu (isteğe bağlı)",
                              modifier = Modifier.fillMaxWidth())
            Text("Bağlantı bulunamazsa Android > Kablosuz hata ayıklama ekranındaki IP:port değerinin yalnızca portunu girin.",
                 style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = { app.startAdvancedSetup() }) { Text(if (app.advancedReady()) "Yeniden kur" else "Kur") }
                TextButton(onClick = { app.advancedTest() }) { Text("Dene") }
            }
        }
    }
}

// ---- A paired computer's screen (on a phone, phase 4) ----------------------------------------

// The computer shares its screen: who is watching, and a way to end it here.
@Kompozable
fun DesktopShareBar(app: App) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), colors = CardDefaults.cardColors(containerColor = cs.errorContainer)) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(AppIcons.Computer, contentDescription = null, tint = cs.onErrorContainer)
                Spacer(modifier = Modifier.width(12.dp))
                Text("${app.peerName(app.desktopViewer())} bu bilgisayarı görüyor ve kontrol ediyor",
                     style = MaterialTheme.typography.bodyMedium, color = cs.onErrorContainer, modifier = Modifier.weight(1f))
                Button(onClick = { app.endDesktopShare() },
                       colors = ButtonDefaults.buttonColors(containerColor = cs.error, contentColor = cs.onError)) { Text("Durdur") }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Ekran paylaşımı sesi", modifier = Modifier.weight(1f), color = cs.onErrorContainer)
                Switch(checked = app.screenShareAudio(), onCheckedChange = { app.setScreenShareAudio(it) })
            }
            Text("Bilgisayarın varsayılan ses çıkışını gönderir.", style = MaterialTheme.typography.bodySmall,
                 color = cs.onErrorContainer)
        }
    }
}

// A paired device asks for this computer's screen and input: the user decides here.
@Kompozable
fun DesktopRequestDialog(app: App) {
    val r = app.desktopRequest()
    if (r != null) {
        val request = r!!
        AlertDialog(
            onDismissRequest = {},
            icon = { Icon(AppIcons.Computer, contentDescription = null) },
            title = { Text("${request.peerName} bu bilgisayarı kontrol etmek istiyor") },
            text = { Text(desktopRequestText()) },
            confirmButton = { TextButton(onClick = { app.answerDesktop(true) }) { Text("İzin ver") } },
            dismissButton = { TextButton(onClick = { app.answerDesktop(false) }) { Text("Reddet") } },
        )
    }
}

// The computer's screen, fitted, with touch as a mouse: tap clicks, long press right-clicks,
// double tap double-clicks, a drag drags (left button held), two fingers scroll.
@Kompozable
fun DesktopView(app: App) {
    val frame = app.desktopFrame()
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var last by remember { mutableStateOf(Offset(0f, 0f)) }
    var typed by remember { mutableStateOf("") }
    var held by remember { mutableStateOf("") }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(app.peerName(app.desktopPeer()), style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f),
                 maxLines = 1, overflow = TextOverflow.Ellipsis)
            Button(onClick = { app.stopDesktop() }) { Text("Kapat") }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(24.dp)) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(if (app.desktopStatus() == "asking") "Bilgisayarda izin verin…" else "Görüntü bekleniyor…", color = Color.White)
                }
            } else {
                val fw = frame.width()
                val fh = frame.height()
                Image(frame, contentDescription = "Bilgisayar ekranı", contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .pointerInput(Unit) {
                              detectTapGestures(
                                  onTap = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, "")) },
                                  onDoubleTap = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, ",\"count\":2")) },
                                  onLongPress = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, ",\"button\":\"right\"")) },
                              )
                          }
                          .pointerInput(Unit) {
                              detectTransformGestures { centroid, pan, zoom, rotation ->
                                  app.desktopInput(desktopScroll(centroid, box, fw, fh, pan))
                              }
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> last = p; app.desktopInput(desktopPoint("button", p, box, fw, fh, ",\"down\":true")) },
                                  onDrag = { change, amount -> last = last + amount; app.desktopInput(desktopPoint("move", last, box, fw, fh, "")) },
                                  onDragEnd = { app.desktopInput(desktopPoint("button", last, box, fw, fh, ",\"down\":false")) },
                                  onDragCancel = { app.desktopInput(desktopPoint("button", last, box, fw, fh, ",\"down\":false")) },
                              )
                          })
            }
        }
        FlowRow(modifier = Modifier.fillMaxWidth().padding(8.dp, 0.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            for (m in desktopModifiers()) {
                FilterChip(selected = held == m, onClick = { held = if (held == m) "" else m }, label = { Text(m) })
            }
            for (k in desktopKeys()) {
                TextButton(onClick = { app.desktopInput(desktopKey(k, held)); held = "" }) { Text(k) }
            }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true, label = "Bilgisayara yaz",
                              modifier = Modifier.weight(1f))
            TextButton(onClick = {
                app.desktopInput(if (held.isNotEmpty()) desktopKey(typed, held) else desktopText(typed))
                typed = ""
                held = ""
            }, enabled = typed.isNotEmpty()) { Text("Gönder") }
        }
    }
}

// ---- A paired phone's screen (on a computer) ------------------------------------------------

@Kompozable
fun ScreenView(app: App) {
    val frame = app.screenFrame()
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var dragFrom by remember { mutableStateOf(Offset(0f, 0f)) }
    var dragTo by remember { mutableStateOf(Offset(0f, 0f)) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("${app.peerName(app.screenPeer())} ekranı", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { app.screenInput("{\"type\":\"back\"}") }) { Text("Geri") }
            TextButton(onClick = { app.screenInput("{\"type\":\"home\"}") }) { Text("Ana ekran") }
            TextButton(onClick = { app.screenInput("{\"type\":\"recents\"}") }) { Text("Son") }
            Button(onClick = { app.stopScreen() }) { Text("Kapat") }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(if (app.screenStatus() == "asking") "Telefonda ekran paylaşımına izin verin…" else "Görüntü bekleniyor…", color = Color.White)
                }
            } else {
                Image(frame, contentDescription = "Telefon ekranı", contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .onPointerEvent(PointerEventType.Scroll) { e ->
                              val change = e.changes.first()
                              if (screenScroll(app, change.position, box, frame.width(), frame.height(), change.scrollDelta)) change.consume()
                          }
                          .pointerInput(Unit) {
                              detectTapGestures(
                                  onTap = { p -> app.screenInput(touchBody("tap", p, box, frame.width(), frame.height())) },
                                  onLongPress = { p -> app.screenInput(touchBody("long_press", p, box, frame.width(), frame.height())) },
                              )
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> dragFrom = p; dragTo = p },
                                  onDrag = { change, amount -> dragTo = dragTo + amount },
                                  onDragEnd = { app.screenInput(swipeBody(dragFrom, dragTo, box, frame.width(), frame.height())) },
                              )
                          })
            }
        }
    }
}

// ---- A paired phone's apps, each on a display of its own (advanced mode, on a computer) -----

@Kompozable
fun AppsScreen(app: App) {
    val phone = app.smsPhone()
    val apps = app.phoneApps()
    val windows = app.appWindows()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty() && apps.isEmpty()) app.requestPhoneApps()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Apps, "Telefon bağlı değil", "Uygulamalar için eşleşmiş bir Android telefon çevrimiçi olmalı.")
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            if (windows.isNotEmpty()) {
                FlowRow(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (w in windows) {
                        TextButton(onClick = { app.showAppWindow(w.session) }) { Text(w.label) }
                    }
                }
            }
            AppGrid(app, apps)
        }
    }
}

@Kompozable
fun AppGrid(app: App, apps: List<PhoneApp>) {
    var query by remember { mutableStateOf("") }
    if (apps.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint(if (app.phoneAppsLoading()) "Uygulamalar telefondan alınıyor…" else "Uygulama yok.", loading = app.phoneAppsLoading())
        }
    } else {
        val shown = if (query.isEmpty()) apps else apps.filter { matchesQuery(it.label, query) }
        Column(modifier = Modifier.fillMaxSize()) {
            Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = "Uygulama ara",
                                  modifier = Modifier.weight(1f))
                Spacer(modifier = Modifier.width(8.dp))
                IconButton(onClick = { app.requestPhoneApps() }) { Icon(Icons.Default.Refresh, contentDescription = "Yenile") }
            }
            Text("${shown.size} uygulama", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                 modifier = Modifier.padding(20.dp, 0.dp))
            LazyVerticalGrid(GridCells.Adaptive(96.dp), modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp),
                             verticalArrangement = Arrangement.spacedBy(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                items(shown) { a ->
                    Column(modifier = Modifier.fillMaxWidth().height(112.dp).clip(RoundedCornerShape(16.dp))
                                         .clickable(onClick = { app.openPhoneApp(a) }).padding(6.dp, 10.dp),
                           horizontalAlignment = Alignment.CenterHorizontally) {
                        if (a.icon.empty()) Box(modifier = Modifier.size(48.dp).background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(12.dp)))
                        else Image(a.icon, contentDescription = null, modifier = Modifier.size(48.dp).clip(RoundedCornerShape(12.dp)))
                        Spacer(modifier = Modifier.height(8.dp))
                        Text(a.label, style = MaterialTheme.typography.bodySmall, maxLines = 2, minLines = 2, overflow = TextOverflow.Ellipsis,
                             textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
                    }
                }
            }
        }
    }
}

@Kompozable
fun PhoneAppWindows(app: App) {
    for (w in app.appWindows()) {
        key(w.session) {
            val state = rememberWindowState(size = DpSize(400.dp, 860.dp))
            LaunchedEffect(w.frontRequests) {
                if (w.frontRequests > 0) state.toFront()
            }
            Window(onCloseRequest = { app.closeAppWindow(w.session) }, state = state, title = w.label) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AppWindowView(app, w, state)
                }
            }
        }
    }
}

@Kompozable
fun AppWindowView(app: App, w: AppWindow, state: WindowState) {
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var last by remember { mutableStateOf(Offset(0f, 0f)) }
    var typed by remember { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    val clip = LocalClipboardManager.current
    val fw = if (w.frame.empty()) 720 else w.frame.width()
    val fh = if (w.frame.empty()) 1560 else w.frame.height()
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(w.label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":4}") }) { Text("Geri") }
            TextButton(onClick = { state.setIsMinimized(true) }) { Text("Küçült") }
            Button(onClick = { app.closeAppWindow(w.session) }) { Text("Kapat") }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (w.frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text("${w.label} açılıyor…", color = Color.White)
                }
            } else {
                Image(w.frame, contentDescription = w.label, contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .onPointerEvent(PointerEventType.Scroll) { e ->
                              appScroll(app, w.session, e.changes.first().position, box, fw, fh, e.changes.first().scrollDelta)
                              e.changes.first().consume()
                          }
                          // The keyboard goes to the app while its image has the focus (a click gives it).
                          .onKeyEvent { e ->
                              if (e.type == KeyEventType.Unknown && e.text.isNotEmpty()) {
                                  app.appInput(w.session, textBody(e.text))
                                  true
                              } else if (e.type != KeyEventType.KeyDown) false
                              else if (e.isCtrlPressed && !e.isAltPressed && e.key == Key.V) {
                                  val text = clip.getText()
                                  if (text.isNotEmpty()) app.appInput(w.session, textBody(text))
                                  true
                              } else {
                                  val body = appKey(e)
                                  if (body.isNotEmpty()) app.appInput(w.session, body)
                                  body.isNotEmpty()
                              }
                          }
                          .focusRequester(focus)
                          .focusable()
                          .pointerInput(Unit) {
                              detectTapGestures(onTap = { p ->
                                  focus.requestFocus()
                                  app.appInput(w.session, appTouch("down", p, box, fw, fh))
                                  app.appInput(w.session, appTouch("up", p, box, fw, fh))
                              })
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> focus.requestFocus(); last = p; app.appInput(w.session, appTouch("down", p, box, fw, fh)) },
                                  onDrag = { change, amount -> last = last + amount; app.appInput(w.session, appTouch("move", last, box, fw, fh)) },
                                  onDragEnd = { app.appInput(w.session, appTouch("up", last, box, fw, fh)) },
                              )
                          })
            }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true, label = "Uygulamaya yaz", modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, textBody(typed)); typed = "" }, enabled = typed.isNotEmpty()) { Text("Gönder") }
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":66}") }) { Text("Enter") }
        }
    }
}

// ---- Calls (a paired phone's, on a computer) ------------------------------------------------

@Kompozable
fun PhoneScreen(app: App) {
    var tab by remember { mutableStateOf(0) }
    Column(modifier = Modifier.fillMaxSize()) {
        PrimaryTabRow(selectedTabIndex = tab) {
            Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("Aramalar") })
            Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("Rehber") })
        }
        if (tab == 0) CallsScreen(app) else ContactsScreen(app)
    }
}

@Kompozable
fun CallsScreen(app: App) {
    val phone = app.smsPhone()
    val log = app.callLog()
    val loading = app.callLogLoading()
    val list = rememberLazyListState()
    val nearEnd by remember {
        derivedStateOf {
            val info = list.layoutInfo
            info.visibleItemsInfo.isNotEmpty() && info.visibleItemsInfo.last().index >= info.totalItemsCount - 4
        }
    }
    var number by remember { mutableStateOf("") }
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestCallLog()
    }
    LaunchedEffect(nearEnd, log.size, loading) {
        if (nearEnd && !loading && app.callLogHasMore()) app.loadMoreCallLog()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Phone, "Telefon bağlı değil", "Aramalar için eşleşmiş bir Android telefon çevrimiçi olmalı.")
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = number, onValueChange = { number = it }, singleLine = true, label = "Numara",
                                  modifier = Modifier.weight(1f))
                Spacer(modifier = Modifier.width(8.dp))
                Button(onClick = { app.dial(number) }, enabled = number.isNotBlank()) { Text("Telefondan ara") }
            }
            if (app.callsError().isNotEmpty()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(app.callsError())
                    TextButton(onClick = { app.requestCallLog() }) { Text("Tekrar dene") }
                }
            }
            LazyColumn(modifier = Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(12.dp, 0.dp, 12.dp, 12.dp),
                       verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (log.isEmpty() && app.callsError().isEmpty()) {
                    item { Hint(if (loading) "Arama kaydı telefondan alınıyor…" else "Arama kaydı yok.", loading = loading) }
                }
                items(log) { c -> CallRow(c, onCall = { app.dial(c.number) }) }
                if (loading && log.isNotEmpty()) {
                    item { Hint("Yükleniyor…", loading = true) }
                }
            }
        }
    }
}

@Kompozable
fun ContactsScreen(app: App) {
    val phone = app.smsPhone()
    var query by remember { mutableStateOf("") }
    val list = rememberLazyListState()
    LaunchedEffect(phone + "|" + query) {
        if (phone.isNotEmpty()) {
            list.scrollToItem(0)
            delay(250)
            app.requestContacts(query)
        }
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(Icons.Filled.Person, "Telefon bağlı değil", "Rehber için eşleşmiş bir Android telefon çevrimiçi olmalı.")
        }
    } else {
        val contacts = app.phoneContacts()
        val loading = app.phoneContactsLoading()
        val error = app.phoneContactsError()
        val nearEnd by remember {
            derivedStateOf {
                val info = list.layoutInfo
                info.visibleItemsInfo.isNotEmpty() && info.visibleItemsInfo.last().index >= info.totalItemsCount - 4
            }
        }
        LaunchedEffect(nearEnd, contacts.size, loading) {
            if (nearEnd && !loading && app.phoneContactsHasMore()) app.loadMoreContacts()
        }
        Column(modifier = Modifier.fillMaxSize()) {
            OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = "Rehberde ara",
                              modifier = Modifier.fillMaxWidth().padding(12.dp))
            if (error.isNotEmpty()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(error)
                    TextButton(onClick = { app.requestContacts(query) }) { Text("Tekrar dene") }
                }
            }
            LazyColumn(modifier = Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(12.dp, 0.dp, 12.dp, 12.dp),
                       verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (contacts.isEmpty() && error.isEmpty()) {
                    item { Hint(if (loading) "Rehber telefondan alınıyor…" else "Kişi bulunamadı.", loading = loading) }
                }
                items(contacts) { c -> PhoneContactRow(c, onCall = { app.dial(c.number) }) }
                if (loading && contacts.isNotEmpty()) {
                    item {
                        Hint("Yükleniyor…", loading = true)
                    }
                }
            }
        }
    }
}

@Kompozable
fun PhoneContactRow(c: ContactMatch, onCall: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(if (c.name.isEmpty()) c.number else c.name, style = MaterialTheme.typography.titleSmall)
                Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}",
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            TextButton(onClick = onCall) { Text("Ara") }
        }
    }
}

@Kompozable
fun CallRow(c: CallEntry, onCall: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val missed = c.type == "missed" || c.type == "rejected" || c.type == "blocked"
    val kind = when (c.type) {
        "incoming" -> "↙ Gelen"
        "outgoing" -> "↗ Giden"
        "missed" -> "✕ Cevapsız"
        "rejected" -> "✕ Reddedildi"
        "blocked" -> "⊘ Engellendi"
        else -> "Arama"
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(c.title(), style = MaterialTheme.typography.titleSmall, color = if (missed) cs.error else cs.onSurface)
                Text("$kind · ${shortDate(c.date)}${if (c.duration > 0) " · " + duration(c.duration) else ""}",
                     style = MaterialTheme.typography.bodySmall, color = if (missed) cs.error else cs.onSurfaceVariant)
            }
            TextButton(onClick = onCall) { Text("Ara") }
        }
    }
}

// ---- Messages (a paired phone's SMS, on a computer) -----------------------------------------

@Kompozable
fun MessagesScreen(app: App) {
    val phone = app.smsPhone()
    val open = app.smsOpenThread()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestSmsThreads()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Sms, "Telefon bağlı değil", "Mesajlar için eşleşmiş bir Android telefon çevrimiçi olmalı.")
        }
    } else if (app.smsError().isNotEmpty()) {
        Column(modifier = Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(app.smsError())
            TextButton(onClick = { app.requestSmsThreads() }) { Text("Tekrar dene") }
        }
    } else if (currentWindowAdaptiveInfo().windowSizeClass.windowWidthSizeClass == WindowWidthSizeClass.Expanded) {
        // A wide window: the threads and the open one side by side.
        Row(modifier = Modifier.fillMaxSize()) {
            Box(modifier = Modifier.width(360.dp).fillMaxHeight()) { ThreadList(app) }
            Box(modifier = Modifier.width(1.dp).fillMaxHeight().background(MaterialTheme.colorScheme.outlineVariant))
            Box(modifier = Modifier.weight(1f).fillMaxHeight()) {
                if (open != null) {
                    Conversation(app, open!!, showBack = false)
                } else {
                    EmptyState(AppIcons.Sms, "Bir konuşma seçin", "Soldaki listeden bir konuşma açın ya da yeni mesaj yazın.")
                }
            }
        }
    } else if (open != null) {
        Conversation(app, open!!, showBack = true)
    } else {
        ThreadList(app)
    }
}

@Kompozable
fun ThreadList(app: App) {
    val threads = app.smsThreads()
    var composing by remember { mutableStateOf(false) }
    Box(modifier = Modifier.fillMaxSize()) {
        LazyColumn(modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (threads.isEmpty()) {
                item { Hint(if (app.smsLoading()) "Mesajlar telefondan alınıyor…" else "Mesaj yok.", loading = app.smsLoading()) }
            }
            items(threads) { t -> ThreadRow(t, onClick = { app.openSmsThread(t) }) }
        }
        FloatingActionButton(onClick = { composing = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
            Icon(Icons.Filled.Add, contentDescription = "Yeni mesaj")
        }
    }
    if (composing) NewSmsDialog(app, onClose = { composing = false })
}

@Kompozable
fun ThreadRow(t: SmsThread, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(onClick = onClick, modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(40.dp).background(cs.primaryContainer, CircleShape), contentAlignment = Alignment.Center) {
                Text(initial(t.title()), color = cs.onPrimaryContainer, style = MaterialTheme.typography.titleMedium)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(t.title(), style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(t.snippet, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Spacer(modifier = Modifier.width(8.dp))
            Column(horizontalAlignment = Alignment.End) {
                Text(shortDate(t.date), style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant)
                if (t.unread > 0) {
                    Box(modifier = Modifier.padding(top = 4.dp).size(20.dp).background(cs.primary, CircleShape), contentAlignment = Alignment.Center) {
                        Text("${t.unread}", color = cs.onPrimary, style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }
    }
}

@Kompozable
fun Conversation(app: App, thread: SmsThread, showBack: Boolean) {
    val messages = app.smsMessages()
    val list = rememberLazyListState()
    var text by remember { mutableStateOf("") }
    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) list.scrollToItem(messages.size - 1)
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
            if (showBack) {
                IconButton(onClick = { app.closeSmsThread() }) { Icon(Icons.Filled.ArrowBack, contentDescription = "Geri") }
            } else {
                Spacer(modifier = Modifier.width(12.dp))
            }
            Column(modifier = Modifier.padding(vertical = 6.dp)) {
                Text(thread.title(), style = MaterialTheme.typography.titleMedium)
                if (thread.name.isNotEmpty()) Text(thread.address, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f).fillMaxWidth(), state = list, contentPadding = PaddingValues(12.dp),
                   verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items(messages) { m -> Bubble(m) }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = text, onValueChange = { text = it }, modifier = Modifier.weight(1f), label = "Mesaj")
            IconButton(onClick = { app.sendSms(thread.address, text); text = "" }, enabled = text.isNotBlank()) {
                Icon(Icons.Filled.Send, contentDescription = "Gönder")
            }
        }
    }
}

@Kompozable
fun Bubble(m: SmsMessage) {
    val cs = MaterialTheme.colorScheme
    val color = if (m.outgoing) cs.primaryContainer else cs.surfaceContainerHigh
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = if (m.outgoing) Arrangement.End else Arrangement.Start) {
        Column(modifier = Modifier.widthIn(0.dp, 360.dp).background(color, RoundedCornerShape(16.dp)).padding(12.dp, 8.dp)) {
            Text(m.body, style = MaterialTheme.typography.bodyMedium)
            Text(if (m.failed) "${clockTime(m.date)} · gönderilemedi" else if (m.id < 0) "gönderiliyor…" else clockTime(m.date),
                 style = MaterialTheme.typography.labelSmall, color = if (m.failed) cs.error else cs.onSurfaceVariant)
        }
    }
}

@Kompozable
fun NewSmsDialog(app: App, onClose: () -> Unit) {
    var to by remember { mutableStateOf("") }        // what was typed: a name or a number
    var chosen by remember { mutableStateOf("") }    // the contact picked, its name
    var number by remember { mutableStateOf("") }    // the number it goes to
    var body by remember { mutableStateOf("") }
    // Search the phone's contacts as the user types (once typing pauses).
    LaunchedEffect(to) {
        delay(250)
        if (chosen.isEmpty()) app.searchContacts(to)
    }
    val results = app.contactResults()
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Yeni mesaj") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = to, onValueChange = { to = it; chosen = ""; number = it }, singleLine = true,
                                  label = "Kişi ya da numara", modifier = Modifier.fillMaxWidth())
                if (chosen.isNotEmpty()) {
                    Text("$chosen · $number", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
                } else {
                    for (c in results.take(5)) {
                        ContactRow(c, onClick = { chosen = c.name; number = c.number; to = c.name })
                    }
                }
                OutlinedTextField(value = body, onValueChange = { body = it }, label = "Mesaj", modifier = Modifier.fillMaxWidth())
            }
        },
        confirmButton = {
            TextButton(onClick = { app.sendSms(number, body); app.searchContacts(""); app.requestSmsThreads(); onClose() },
                       enabled = number.isNotBlank() && body.isNotBlank()) { Text("Gönder") }
        },
        dismissButton = { TextButton(onClick = { app.searchContacts(""); onClose() }) { Text("Vazgeç") } },
    )
}

@Kompozable
fun ContactRow(c: ContactMatch, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Row(modifier = Modifier.fillMaxWidth().clickable(onClick = onClick).padding(4.dp, 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(modifier = Modifier.size(32.dp).background(cs.secondaryContainer, CircleShape), contentAlignment = Alignment.Center) {
            Text(initial(c.name), color = cs.onSecondaryContainer, style = MaterialTheme.typography.labelLarge)
        }
        Spacer(modifier = Modifier.width(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(c.name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}", style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
        }
    }
}

@Kompozable
fun PhoneNotificationCard(n: PhoneNotification, peerName: String, onDismiss: () -> Unit, onReply: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp)) {
            Text("${n.app} · $peerName · ${clockTime(n.time)}", style = MaterialTheme.typography.labelMedium, color = cs.primary)
            if (n.title.isNotEmpty()) Text(n.title, style = MaterialTheme.typography.titleSmall)
            if (n.text.isNotEmpty()) Text(n.text, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, maxLines = 4, overflow = TextOverflow.Ellipsis)
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (n.canReply) TextButton(onClick = onReply) { Text("Yanıtla") }
                TextButton(onClick = onDismiss) { Text("Kapat") }
            }
        }
    }
}

@Kompozable
fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 16.dp, bottom = 4.dp))
}

// A screen with nothing to show yet: an icon, what is missing and what to do, in the middle.
@Kompozable
fun EmptyState(icon: ImageVector, title: String, text: String) {
    val cs = MaterialTheme.colorScheme
    Column(modifier = Modifier.fillMaxSize().padding(32.dp), horizontalAlignment = Alignment.CenterHorizontally,
           verticalArrangement = Arrangement.Center) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(48.dp), tint = cs.primary)
        Spacer(modifier = Modifier.height(16.dp))
        Text(title, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
        Spacer(modifier = Modifier.height(4.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, textAlign = TextAlign.Center)
    }
}

@Kompozable
fun Hint(text: String, loading: Boolean) {
    Row(modifier = Modifier.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        if (loading) {
            CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
            Spacer(modifier = Modifier.width(12.dp))
        }
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Kompozable
fun OnlineDot(online: Boolean) {
    val color = if (online) Color(0xFF2E7D32) else MaterialTheme.colorScheme.outline
    Box(modifier = Modifier.size(10.dp).background(color, CircleShape))
}

@Kompozable
fun PairedPeerCard(
    peer: Peer,
    onSendFiles: () -> Unit,
    onDropFiles: (List<String>, List<String>) -> Unit,
    onSendClipboard: () -> Unit,
    onForget: () -> Unit,
    onShowScreen: () -> Unit,
    onControl: () -> Unit,
    onTrust: (Boolean) -> Unit,
) {
    var dragging by remember { mutableStateOf(false) }
    val cs = MaterialTheme.colorScheme
    val status = when {
        dragging -> "Göndermek için bırakın"
        !peer.online -> if (peer.autoAccept) "Çevrimdışı · Güvenilir" else "Çevrimdışı"
        peer.autoAccept -> "Çevrimiçi · Güvenilir"
        isDesktop() -> "Çevrimiçi · dosyaları buraya sürükleyin"
        else -> "Çevrimiçi"
    }
    var confirmTrust by remember { mutableStateOf(false) }
    val drop = Modifier.dragAndDropTarget(
        shouldStartDragAndDrop = { e -> carriesFiles(e) },
        target = DragAndDropTarget(
            onDrop = { e ->
                dragging = false
                onDropFiles(droppedFiles(e), droppedUris(e))
                true
            },
            onEntered = { dragging = true },
            onExited = { dragging = false },
            onEnded = { dragging = false },
        ),
    )
    var menu by remember { mutableStateOf(false) }
    OutlinedCard(
        modifier = drop.fillMaxWidth(),
        colors = CardDefaults.outlinedCardColors(containerColor = if (dragging) cs.secondaryContainer else cs.surface),
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, end = 4.dp, bottom = 12.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box {
                    Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(32.dp))
                    Box(modifier = Modifier.align(Alignment.TopEnd)) { OnlineDot(peer.online) }
                }
                Spacer(modifier = Modifier.width(16.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(peer.name, style = MaterialTheme.typography.titleMedium)
                    Text(status, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
                }
                IconButton(onClick = { if (peer.autoAccept) onTrust(false) else confirmTrust = true }) {
                    if (peer.autoAccept) Icon(AppIcons.VerifiedUser, contentDescription = "Güvenilir: geri al", tint = cs.primary)
                    else Icon(AppIcons.Shield, contentDescription = "Güvenilir yap", tint = cs.onSurfaceVariant)
                }
                Box {
                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = "Diğer") }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        DropdownMenuItem(
                            text = { Text("Cihazı unut") },
                            onClick = {
                                menu = false
                                onForget()
                            },
                            leadingIcon = { Icon(Icons.Filled.Delete, contentDescription = null) })
                    }
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(start = 48.dp, end = 12.dp)) {
                AssistChip(onClick = onSendFiles, enabled = peer.online, label = { Text("Dosya gönder") },
                           leadingIcon = { Icon(AppIcons.UploadFile, contentDescription = null, modifier = Modifier.size(18.dp)) })
                AssistChip(onClick = onSendClipboard, enabled = peer.online, label = { Text("Pano") },
                           leadingIcon = { Icon(AppIcons.ContentPaste, contentDescription = null, modifier = Modifier.size(18.dp)) })
                if (isDesktop() && peer.platform == "android") {
                    AssistChip(onClick = onShowScreen, enabled = peer.online, label = { Text("Ekranı göster") },
                               leadingIcon = { Icon(AppIcons.Smartphone, contentDescription = null, modifier = Modifier.size(18.dp)) })
                }
                if (!isDesktop() && peer.platform != "android") {
                    AssistChip(onClick = onControl, enabled = peer.online, label = { Text("Kontrol et") },
                               leadingIcon = { Icon(AppIcons.Computer, contentDescription = null, modifier = Modifier.size(18.dp)) })
                }
            }
        }
    }
    if (confirmTrust) {
        AlertDialog(
            onDismissRequest = { confirmTrust = false },
            icon = { Icon(AppIcons.VerifiedUser, contentDescription = null) },
            title = { Text("${peer.name} güvenilir olsun mu?") },
            text = {
                Text("Bu cihazdan gelen dosyalar sorulmadan kaydedilir, bilgisayarı kontrol etme istekleri sorulmadan kabul edilir ve kopyaladığınız metinler ona otomatik gider. Kalkan simgesine yeniden dokunarak istediğiniz zaman geri alabilirsiniz.")
            },
            confirmButton = { TextButton(onClick = { confirmTrust = false; onTrust(true) }) { Text("Güvenilir yap") } },
            dismissButton = { TextButton(onClick = { confirmTrust = false }) { Text("Vazgeç") } },
        )
    }
}

fun platformName(platform: String): String = when (platform) {
    "android" -> "Android"
    "linux" -> "Linux"
    "windows" -> "Windows"
    "macos" -> "macOS"
    else -> platform
}

// No paired device yet: what pairing gives and how to start.
@Kompozable
fun NoPairedDevices() {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.surfaceVariant), modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.Devices, contentDescription = null, modifier = Modifier.size(36.dp), tint = cs.primary)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text("Henüz eşleşmiş cihaz yok", style = MaterialTheme.typography.titleSmall)
                Text("Diğer cihazda da AirLink'i açın; aşağıda görününce Eşleş'e basıp iki ekrandaki kodu karşılaştırın.",
                     style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
            }
        }
    }
}

@Kompozable
fun NearbyPeerCard(peer: Peer, onPair: () -> Unit) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(peer.name, style = MaterialTheme.typography.titleMedium)
                Text("${platformName(peer.platform)} · ${shortId(peer.id)}", style = MaterialTheme.typography.bodyMedium,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            FilledTonalButton(onClick = onPair) {
                Icon(AppIcons.Link, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(modifier = Modifier.width(8.dp))
                Text("Eşleş")
            }
        }
    }
}

@Kompozable
fun TransferRow(t: Transfer, peerName: String) {
    val cs = MaterialTheme.colorScheme
    val title = if (t.incoming) "$peerName → bu cihaz" else "Bu cihaz → $peerName"
    val status = when {
        t.error.isNotEmpty() -> "Hata: ${t.error}"
        t.completed -> "Tamamlandı · ${humanSize(t.total)}"
        t.waiting -> "Karşı tarafın onayı bekleniyor… (${humanSize(t.total)})"
        else -> "${humanSize(t.done)} / ${humanSize(t.total)}"
    }
    val icon = when {
        t.error.isNotEmpty() -> AppIcons.Error
        t.completed -> Icons.Filled.CheckCircle
        t.incoming -> AppIcons.Download
        else -> AppIcons.Upload
    }
    val tint = when {
        t.error.isNotEmpty() -> cs.error
        t.completed -> Color(0xFF2E7D32)
        else -> cs.primary
    }
    Card {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, contentDescription = null, tint = tint)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall)
                if (t.active()) {
                    Spacer(modifier = Modifier.height(6.dp))
                    if (t.waiting) {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    } else {
                        LinearProgressIndicator(progress = { t.fraction() }, modifier = Modifier.fillMaxWidth())
                    }
                }
                Spacer(modifier = Modifier.height(4.dp))
                Text(status, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2)
            }
        }
    }
}

@Kompozable
fun PairingCode(code: String, hint: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
        Text(spacedCode(code), style = MaterialTheme.typography.displaySmall)
        Spacer(modifier = Modifier.height(12.dp))
        Text(hint)
    }
}

@Kompozable
fun IncomingPrompt(app: App) {
    val p = app.prompt()
    if (p != null) {
        val prompt = p!!
        if (prompt.isPairing) {
            AlertDialog(
                onDismissRequest = {},
                icon = { Icon(AppIcons.Link, contentDescription = null) },
                title = { Text("${prompt.peerName} eşleşmek istiyor") },
                text = { PairingCode(prompt.code, "Diğer cihazda da aynı kod görünüyorsa onaylayın.") },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text("Kodlar aynı") } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text("Reddet") } },
            )
        } else {
            AlertDialog(
                onDismissRequest = {},
                icon = { Icon(AppIcons.Download, contentDescription = null) },
                title = { Text("${prompt.peerName} dosya göndermek istiyor") },
                text = {
                    Column {
                        Text("${prompt.files.size} dosya, toplam ${humanSize(prompt.totalSize)}")
                        Spacer(modifier = Modifier.height(8.dp))
                        for (f in prompt.files.take(8)) {
                            Text("${f.path}  ·  ${humanSize(f.size)}", style = MaterialTheme.typography.bodySmall,
                                 maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        if (prompt.files.size > 8) Text("… ve ${prompt.files.size - 8} dosya daha", style = MaterialTheme.typography.bodySmall)
                    }
                },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text("Kabul et") } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text("Reddet") } },
            )
        }
    }
}

@Kompozable
fun OutgoingPairingDialog(app: App) {
    val o = app.outgoing()
    if (o != null) {
        val pairing = o!!
        AlertDialog(
            onDismissRequest = {},
            icon = { Icon(AppIcons.Link, contentDescription = null) },
            title = { Text("${pairing.peerName} ile eşleş") },
            text = {
                PairingCode(pairing.code,
                    if (pairing.waitingForPeer) "Diğer cihazın onayı bekleniyor…" else "Bu kod diğer cihazda da görünüyor mu?")
            },
            confirmButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(true) }) { Text("Kodlar aynı") }
            },
            dismissButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(false) }) { Text("İptal") }
            },
        )
    }
}

kompoz::Application kompoz::createApplication(int argc, char** argv) {
#if defined(_WIN32)
    // A GUI-subsystem process has no console in WinBoat's app view. Keep startup diagnostics
    // when requested by the bundled diagnostic launcher, including Kompoz's GPU errors.
    if (const char* path = std::getenv("AIRLINK_DIAG_LOG")) {
        if (std::freopen(path, "w", stderr)) {
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::fprintf(stderr, "airlink: diagnostic launch\n");
        }
    }
#endif
    // `airlink --send FILE...`: to the running instance if there is one (this process then exits).
    airlink::platform::handleLaunch(argc, argv);
    // Create the app's states here, outside any composition's snapshot.
    App::get();
    // On a computer, closing the window keeps AirLink running in the tray (where there is one).
    // Wide enough for the side rail and a readable list (Kompoz's default is phone-sized).
    return {.title = "AirLink", .width = 880, .height = 820, .content = [] { AirLinkApp(); }, .onCloseRequest = [] {
                if (kompoz::isTraySupported()) kompoz::setAppWindowVisible(false);
                else kompoz::exitApplication();
            }};
}
