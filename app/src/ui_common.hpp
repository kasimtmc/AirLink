#pragma once

// What AirLink's UI files (ui.cpp, ui_*.cpp) share: the C++ helpers their composables call.

#include <kompoz/kompoz.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ctime>

#include "app_icons.hpp"
#include "app_state.hpp"
#include "clipboard_watch.hpp"
#include "i18n.hpp"
#include "cloud_state.hpp"
#include "update_state.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "tray_icon.hpp"

using namespace kompoz;
using namespace airlink;

// ---- C++ helpers the composables call ------------------------------------------------------

inline App appState() { return App::get(); }

// Windows' notifications go through the tray icon: the ones waiting, sent to its state.
inline void showTrayNotifications(const TrayState& tray) {
    for (auto& [title, text] : airlink::platform::takeTrayNotifications())
        tray.sendNotification(Notification(title, text, Notification::Type::Info));
}
inline Drives drivesState() { return Drives::get(); }

// "27.11.2026 14:05" in the local time zone, for an RFC 3339 time from a cloud drive ("" stays).
inline std::string cloudDate(const std::string& rfc3339) {
    std::tm utc{};
    if (rfc3339.size() < 19 || std::sscanf(rfc3339.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
                                           &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6)
        return {};
    utc.tm_year -= 1900;
    utc.tm_mon -= 1;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&utc);
#else
    std::time_t t = timegm(&utc);
#endif
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[24];
    std::strftime(buf, sizeof buf, "%d.%m.%Y %H:%M", &local);
    return buf;
}

// A file's kind by its extension (or a Google document's type), for its icon and color. Images
// and videos show their own picture (cloud_state's isPicture); kinds with none get the plain file.
enum class FileKindId { Other, Image, Video, Pdf, Document, Sheet, Slides, Archive, Audio, Code, Text, Android, Program, Disc, Book };

inline FileKindId fileKindOf(const std::string& name, const std::string& googleKind = {}) {
    if (!googleKind.empty()) {
        if (googleKind.ends_with(".document")) return FileKindId::Document;
        if (googleKind.ends_with(".spreadsheet")) return FileKindId::Sheet;
        if (googleKind.ends_with(".presentation")) return FileKindId::Slides;
        if (googleKind.ends_with(".drawing")) return FileKindId::Image;
        return FileKindId::Other;
    }
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return FileKindId::Other;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<FileKindId, std::vector<std::string>> table[] = {
        {FileKindId::Video, {"mp4", "m4v", "mov", "mkv", "webm", "avi", "3gp", "wmv", "flv", "mpg", "mpeg", "ts"}},
        {FileKindId::Image, {"jpg", "jpeg", "png", "gif", "webp", "bmp", "tif", "tiff", "heic", "heif", "avif", "svg", "ico", "raw", "dng", "psd"}},
        {FileKindId::Pdf, {"pdf"}},
        {FileKindId::Document, {"doc", "docx", "odt", "rtf", "pages"}},
        {FileKindId::Sheet, {"xls", "xlsx", "ods", "csv", "tsv", "numbers"}},
        {FileKindId::Slides, {"ppt", "pptx", "odp", "key"}},
        {FileKindId::Archive, {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "zst", "cab"}},
        {FileKindId::Audio, {"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "mid", "midi"}},
        {FileKindId::Code, {"c", "cc", "cpp", "h", "hpp", "rs", "py", "js", "ts", "tsx", "jsx", "java", "kt", "go", "cs", "php", "rb", "swift",
                            "sh", "html", "css", "json", "xml", "yaml", "yml", "toml", "sql", "lua", "dart", "ino"}},
        {FileKindId::Text, {"txt", "md", "log", "ini", "cfg", "conf", "srt"}},
        {FileKindId::Android, {"apk", "aab", "xapk", "apks"}},
        {FileKindId::Program, {"exe", "msi", "msix", "appx", "deb", "rpm", "appimage", "flatpak", "dmg", "pkg", "bat", "cmd", "jar"}},
        {FileKindId::Disc, {"iso", "img", "vhd", "vhdx", "vmdk", "qcow2"}},
        {FileKindId::Book, {"epub", "mobi", "azw3", "djvu", "fb2"}},
    };
    for (const auto& [kind, exts] : table)
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) return kind;
    return FileKindId::Other;
}

inline const kompoz::ImageVector& fileKindIcon(const std::string& name, const std::string& googleKind) {
    switch (fileKindOf(name, googleKind)) {
        case FileKindId::Image: return AppIcons::Image;
        case FileKindId::Video: return AppIcons::Movie;
        case FileKindId::Pdf: return AppIcons::PictureAsPdf;
        case FileKindId::Document: return AppIcons::Description;
        case FileKindId::Sheet: return AppIcons::TableChart;
        case FileKindId::Slides: return AppIcons::Slideshow;
        case FileKindId::Archive: return AppIcons::Archive;
        case FileKindId::Audio: return AppIcons::MusicNote;
        case FileKindId::Code: return AppIcons::Code;
        case FileKindId::Text: return AppIcons::Article;
        case FileKindId::Android: return AppIcons::Android;
        case FileKindId::Program: return AppIcons::Terminal;
        case FileKindId::Disc: return AppIcons::Album;
        case FileKindId::Book: return AppIcons::Book;
        default: return AppIcons::InsertDriveFile;
    }
}

inline std::string previewImagePath() {
    const char* path = std::getenv("AIRLINK_PREVIEW_IMAGE");
    return path ? path : "/tmp/airlink-preview.jpg";
}

inline std::string fileName(const std::string& path);
inline Updates updatesState() { return Updates::get(); }

// The update card's line: what is going on.
inline std::string updateLine(const UpdateInfo& u) {
    if (u.status == "checking") return tr("Denetleniyor…");
    if (u.status == "uptodate") return tr("AirLink güncel");
    if (u.status == "available") return trf("AirLink {} çıktı", u.version);
    if (u.status == "downloading")
        return u.total > 0 ? trf("İndiriliyor · {} / {}", humanSize(u.done), humanSize(u.total)) : tr("İndiriliyor…");
    if (u.status == "installing") return tr("Kuruluyor… AirLink yeniden başlayacak");
    if (u.status == "error") return u.error;
    return {};
}

inline float updateFraction(const UpdateInfo& u) { return u.total > 0 ? float(double(u.done) / double(u.total)) : 0.f; }
// The button for an available release: this copy updates itself, a Flatpak's bundle goes to the
// software center, a source build gets the release page.
inline std::string updateAction() {
    std::string kind = airlink::platform::updateKind();
    return kind.empty() ? tr("Sürüm sayfasını aç") : kind == "flatpak" ? tr("İndir ve aç") : tr("Güncelle");
}
inline bool updateBusy(const UpdateInfo& u) { return u.status == "checking" || u.status == "downloading" || u.status == "installing"; }

// "X gönderilsin" / "3 dosya gönderilsin".
inline std::string shareTitle(const std::vector<std::string>& files) {
    return files.size() == 1 ? trf("{} gönderilsin", fileName(files.front())) : trf("{} dosya gönderilsin", files.size());
}

// A phone app's right-click menu in the Apps tab.
inline std::vector<kompoz::ContextMenuItem> appMenu(const App& app, const PhoneApp& a) {
    std::vector<kompoz::ContextMenuItem> items;
    items.push_back(kompoz::ContextMenuItem{tr("Aç"), [app, a] { app.openPhoneApp(a); }});
    if (app.canMakeAppShortcuts()) {
        items.push_back(kompoz::ContextMenuItem{tr("Uygulama menüsüne kısayol ekle"), [app, a] { app.makeAppShortcut(a, false); }});
        items.push_back(kompoz::ContextMenuItem{tr("Masaüstüne kısayol ekle"), [app, a] { app.makeAppShortcut(a, true); }});
    }
    return items;
}

// A local image AsyncImage can show small (JPEG, PNG, WebP, BMP).
inline bool showsOwnPicture(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "webp" || ext == "bmp";
}

// The kind's own color (as file managers and the drives' web pages color them); false: the theme's.
inline bool hasFileKindColor(const std::string& name, const std::string& googleKind) {
    auto k = fileKindOf(name, googleKind);
    return k != FileKindId::Other && k != FileKindId::Image && k != FileKindId::Video;
}
inline Color fileKindColor(const std::string& name, const std::string& googleKind) {
    switch (fileKindOf(name, googleKind)) {
        case FileKindId::Pdf: return Color::argb(0xFFE53935);
        case FileKindId::Document: return Color::argb(0xFF1E88E5);
        case FileKindId::Sheet: return Color::argb(0xFF43A047);
        case FileKindId::Slides: return Color::argb(0xFFFB8C00);
        case FileKindId::Archive: return Color::argb(0xFF8D6E63);
        case FileKindId::Audio: return Color::argb(0xFF8E24AA);
        case FileKindId::Code: return Color::argb(0xFF00897B);
        case FileKindId::Text: return Color::argb(0xFF78909C);
        case FileKindId::Android: return Color::argb(0xFF7CB342);
        case FileKindId::Program: return Color::argb(0xFF546E7A);
        case FileKindId::Disc: return Color::argb(0xFF6D4C41);
        case FileKindId::Book: return Color::argb(0xFFD81B60);
        default: return Color::argb(0xFF757575);
    }
}

// A drive entry's second line: "3.4 MB · 27.11.2026 14:05", a folder's date alone.
inline std::string cloudEntryLine(const CloudEntry& e) {
    std::string date = cloudDate(e.modified);
    std::string size = e.folder ? std::string() : !e.kind.empty() ? std::string(tr("Google dokümanı")) : humanSize(e.size);
    if (size.empty()) return date;
    return date.empty() ? size : size + " · " + date;
}

// "12.3 GB / 15 GB kullanılıyor", or the used space alone when there is no limit.
inline std::string quotaText(const CloudView& v) {
    if (v.used <= 0 && v.total <= 0) return {};
    return v.total > 0 ? trf("{} / {} kullanılıyor", humanSize(v.used), humanSize(v.total)) : trf("{} kullanılıyor", humanSize(v.used));
}

inline float quotaFraction(const CloudView& v) { return v.total > 0 ? float(double(v.used) / double(v.total)) : 0.f; }

// A tab's label: the service, with the account when it has more than one tab.
inline std::string cloudTabLabel(const std::vector<CloudAccount>& all, const CloudAccount& a) {
    int same = 0;
    for (const auto& other : all) same += other.provider == a.provider;
    if (same < 2) return providerTitle(a.provider);
    std::string who = a.email.empty() ? a.name : a.email.substr(0, a.email.find('@'));
    return providerTitle(a.provider) + " · " + who;
}

// The tray panel's tabs under the devices: the phone's (when one is online) and a drive's each.
constexpr int TRAY_NOTIFICATIONS = 0, TRAY_MESSAGES = 1, TRAY_CALLS = 2, TRAY_CONTACTS = 3, TRAY_DRIVE = 100;

inline std::vector<int> trayTabs(bool phone, bool notifications, int drives) {
    std::vector<int> tabs;
    if (phone || notifications) tabs.push_back(TRAY_NOTIFICATIONS);
    if (phone) {
        tabs.push_back(TRAY_MESSAGES);
        tabs.push_back(TRAY_CALLS);
        tabs.push_back(TRAY_CONTACTS);
    }
    for (int i = 0; i < drives; ++i) tabs.push_back(TRAY_DRIVE + i);
    return tabs;
}

inline const kompoz::ImageVector& trayTabIcon(int tab) {
    switch (tab) {
        case TRAY_NOTIFICATIONS: return kompoz::Icons::Filled::Notifications;
        case TRAY_MESSAGES: return AppIcons::Sms;
        case TRAY_CALLS: return AppIcons::Phone;
        case TRAY_CONTACTS: return AppIcons::Contacts;
        default: return AppIcons::Cloud;
    }
}

inline std::string trayTabTitle(int tab, const std::vector<CloudAccount>& accounts) {
    switch (tab) {
        case TRAY_NOTIFICATIONS: return tr("Bildirimler");
        case TRAY_MESSAGES: return tr("Son mesajlar");
        case TRAY_CALLS: return tr("Son çağrılar");
        case TRAY_CONTACTS: return tr("Rehber");
        default:
            if (tab >= TRAY_DRIVE && size_t(tab - TRAY_DRIVE) < accounts.size()) return cloudTabLabel(accounts, accounts[size_t(tab - TRAY_DRIVE)]);
            return {};
    }
}

inline std::string cloudTitle(const CloudAccount& a) {
    std::string who = a.email.empty() ? a.name : a.email;
    return who.empty() ? providerTitle(a.provider) : providerTitle(a.provider) + " · " + who;
}

inline std::string cloudJobLine(const CloudJob& j) {
    if (!j.error.empty()) return j.error;
    if (j.finished) return j.upload ? tr("Yüklendi") : !j.sendTo.empty() ? tr("İndirildi, cihaza gönderiliyor") : tr("İndirildi");
    std::string done = humanSize(j.done) + (j.total > 0 ? " / " + humanSize(j.total) : std::string());
    return j.upload ? trf("Yükleniyor · {}", done) : trf("İndiriliyor · {}", done);
}
inline void openDownloads(const std::string& path) { airlink::platform::openFolder(path); }
inline bool needsBackgroundAutostartSettings() { return airlink::platform::needsBackgroundAutostartSettings(); }
inline bool canStartAtLogin() { return airlink::platform::canStartAtLogin(); }
inline bool startAtLogin() { return airlink::platform::startAtLogin(); }
inline void setStartAtLogin(bool on) { airlink::platform::setStartAtLogin(on); }
// "%85" / "%85 · şarj oluyor"; empty when not known.
inline std::string batteryText(const PhoneBattery& b) {
    if (b.level < 0) return {};
    return "%" + std::to_string(b.level) + (b.charging ? tr(" · şarj oluyor") : "");
}

inline std::string appVersion() { return AIRLINK_VERSION; }

inline void requestBatteryExemption() { airlink::platform::requestBatteryExemption(); }
inline bool launchedAtLogin() { return airlink::platform::launchedAtLogin(); }
inline void openBackgroundAutostartSettings() { airlink::platform::openBackgroundAutostartSettings(); }

enum class TrayIconStatus { Starting, Connected, NoDevices, Paused, Error, Received };

// Preserve the white link mark and the disc's soft edge while tinting its blue background.
inline ImageBitmap statusTrayIcon(TrayIconStatus status) {
    ImageBitmap base = decodeToImageBitmap(trayIconPng());
    if (base.empty() || !base.pixels()) return base;
    std::array<int, 3> color{37, 99, 235};  // starting: original blue
    switch (status) {
    case TrayIconStatus::Connected: color = {22, 163, 74}; break;
    case TrayIconStatus::NoDevices: color = {217, 119, 6}; break;
    case TrayIconStatus::Paused: color = {107, 114, 128}; break;
    case TrayIconStatus::Error: color = {220, 38, 38}; break;
    case TrayIconStatus::Received: color = {147, 51, 234}; break;  // a blink: something came
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

inline std::string shortId(const std::string& id) { return id.substr(0, 8); }

inline std::vector<std::string> droppedFiles(const DragAndDropEvent& e) { return e.data ? e.data->files : std::vector<std::string>{}; }

// Whether a drag may carry files. While it is over the window only its MIME types are known (the
// files are read at the drop): a list of URIs, or anything that isn't text (Android's drags from
// other apps carry a file's own type, image/jpeg, with a content:// URI).
inline bool carriesFiles(const DragAndDropEvent& e) {
    if (!e.data) return false;
    if (!e.data->files.empty()) return true;
    for (const std::string& type : e.data->mimeTypes)
        if (type == "text/uri-list" || type == "x-special/gnome-copied-files" || !type.starts_with("text/")) return true;
    return false;
}

// URIs the drop carries that aren't local paths (Android's content:// ones).
inline std::vector<std::string> droppedUris(const DragAndDropEvent& e) {
    std::vector<std::string> uris;
    if (!e.data) return uris;
    for (const auto& item : e.data->toClipEntry().items)
        if (item.uri && !item.uri->starts_with("file://")) uris.push_back(*item.uri);
    return uris;
}

inline std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "14:05" for a time in ms since the epoch, in the local time zone.
inline std::string clockTime(int64_t ms) {
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
inline std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    size_t n = 1;
    unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    return name.substr(0, n);
}

// "14:05" today, "23.11" another day.
inline std::string shortDate(int64_t ms) {
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

// A history line: "Yüklendi · 3.4 MB · 14:05", a failure's reason in place of the size.
inline std::string cloudRecordLine(const CloudRecord& r, const std::string& peerName) {
    std::string what = !r.error.empty() ? r.error
                       : r.upload       ? std::string(tr("Yüklendi"))
                       : !r.sendTo.empty() ? trf("{} cihazına gönderildi", peerName)
                                           : std::string(tr("İndirildi"));
    std::string size = r.error.empty() && r.size > 0 ? " · " + humanSize(r.size) : std::string();
    return what + size + " · " + shortDate(r.time);
}

// "1 dk 05 sn", "42 sn".
inline std::string duration(int64_t seconds) {
    if (seconds < 60) return trf("{} sn", seconds);
    char rest[8];
    std::snprintf(rest, sizeof rest, "%02lld", (long long)(seconds % 60));
    return trf("{} dk {} sn", seconds / 60, std::string(rest));
}

// Where a point on the shown screen is on the phone's, as fractions (0..1) of it: the image is
// fitted into `box` (letterboxed), keeping its aspect.
inline bool toScreen(Offset p, Size box, int iw, int ih, double& x, double& y) {
    if (box.width <= 0 || box.height <= 0 || iw <= 0 || ih <= 0) return false;
    double scale = std::min(double(box.width) / iw, double(box.height) / ih);
    double w = iw * scale, h = ih * scale, left = (box.width - w) / 2, top = (box.height - h) / 2;
    x = (p.x - left) / w;
    y = (p.y - top) / h;
    return x >= 0 && x <= 1 && y >= 0 && y <= 1;
}

inline std::string touchBody(const std::string& type, Offset p, Size box, int iw, int ih) {
    double x, y;
    if (!toScreen(p, box, iw, ih, x, y)) return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"x\":%.4f,\"y\":%.4f}", type.c_str(), x, y);
    return buf;
}

inline std::string swipeBody(Offset from, Offset to, Size box, int iw, int ih) {
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
inline std::string appTouch(const std::string& action, Offset p, Size box, int iw, int ih) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"cmd\":\"touch\",\"action\":\"%s\",\"x\":%.4f,\"y\":%.4f}", action.c_str(),
                  std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
    return buf;
}

// Whether an app's label has the search text in it, letters' case aside (Turkish İ/ı, Ş, Ç...).
inline std::string foldCase(const std::string& s) {
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
inline bool matchesQuery(const std::string& label, const std::string& query) {
    std::string l = foldCase(label), q = foldCase(query);
    if (l.find(q) != std::string::npos) return true;
    // "i" also finds "ı" and the other way round (people type either)
    auto plain = [](std::string s) {
        for (size_t p; (p = s.find("ı")) != std::string::npos;) s.replace(p, std::strlen("ı"), "i");
        return s;
    };
    return plain(l).find(plain(q)) != std::string::npos;
}

inline std::string textBody(const std::string& text) { return "{\"cmd\":\"text\",\"text\":" + json::quote(text) + "}"; }

// A wheel turn over an app's display (notches; Kompoz: down and right positive, Android: up and left).
inline void appScroll(const App& app, int session, Offset p, Size box, int iw, int ih, Offset delta) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    app.appWheel(session, std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), -double(delta.x), -double(delta.y));
}

// A wheel turn over the phone's shared screen; false outside the image.
inline bool screenScroll(const App& app, Offset p, Size box, int iw, int ih, Offset delta) {
    double x = 0, y = 0;
    if (!toScreen(p, box, iw, ih, x, y)) return false;
    app.screenWheel(x, y, delta.x, delta.y);
    return true;
}

// A key pressed over an app's window, as Android's KeyEvent code and meta state ("" when it has none).
inline std::string appKey(const KeyEvent& e) {
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
inline std::string desktopPoint(const std::string& type, Offset p, Size box, int iw, int ih, const std::string& extra) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"x\":%.4f,\"y\":%.4f%s}", type.c_str(), std::clamp(x, 0.0, 1.0),
                  std::clamp(y, 0.0, 1.0), extra.c_str());
    return buf;
}

inline std::string desktopKey(const std::string& key, const std::string& modifier) {
    std::string body = "{\"type\":\"key\",\"key\":" + json::quote(key);
    if (!modifier.empty()) body += ",\"modifiers\":[" + json::quote(modifier) + "]";
    return body + "}";
}

inline std::string desktopText(const std::string& text) { return "{\"type\":\"text\",\"text\":" + json::quote(text) + "}"; }

// Wheel notches from a two-finger drag on the phone: 40 px a notch, content follows the fingers.
inline std::string desktopScroll(Offset p, Size box, int iw, int ih, Offset pan) {
    char extra[64];
    std::snprintf(extra, sizeof extra, ",\"dx\":%.2f,\"dy\":%.2f", -pan.x / 40.0, -pan.y / 40.0);
    return desktopPoint("scroll", p, box, iw, ih, extra);
}

// The running transfers and the last `finished` finished ones, in their order.
inline std::vector<Transfer> recentTransfers(const std::vector<Transfer>& all, size_t finished) {
    size_t done = std::count_if(all.begin(), all.end(), [](const Transfer& t) { return !t.active(); });
    std::vector<Transfer> out;
    for (const Transfer& t : all) {
        if (t.active() || done-- <= finished) out.push_back(t);
    }
    return out;
}

// The first http(s) link in a text, or "".
inline std::string firstLink(const std::string& text) {
    size_t at = text.find("https://");
    if (at == std::string::npos) at = text.find("http://");
    if (at == std::string::npos) return {};
    size_t end = at;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
    return text.substr(at, end - at);
}

// Files copied in a file manager (the clipboard's file:// items), as paths.
inline std::vector<std::string> copiedFiles(const Clipboard& clip) {
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

inline std::string callTypeLabel(const std::string& type) {
    if (type == "incoming") return tr("Gelen");
    if (type == "outgoing") return tr("Giden");
    if (type == "missed") return tr("Cevapsız");
    if (type == "rejected") return tr("Reddedildi");
    return tr("Arama");
}

inline bool fileExists(const std::string& path) { return airlink::platform::fileExists(path); }

inline void openFile(const std::string& path) { airlink::platform::openFile(path); }
inline Clipboard systemClipboard() { return kompoz::clipboard(); }  // (Home's `clipboard` is the text manager)
// The clipboard holds a password: Kompoz's metadata (Linux: a password manager's hint) or the platform's own mark.
inline bool clipSensitive(const Clipboard& clip) {
    auto entry = clip.getClipEntry().value;
    return (entry && entry->clipMetadata.isSensitive) || airlink::platform::clipboardMarkedSensitive();
}
// What is on the clipboard now: the watch's latest copy where there is one (Wayland, also without
// the focus), else what the window reads.
inline std::string clipboardText(const ClipboardManager& manager) {
    if (auto w = airlink::platform::watchedClipboard()) return w->text;
    return manager.getText();
}
// A paired device's text onto the clipboard: through the watch where there is one (Wayland: also
// with the window hidden), else the window's own clipboard.
inline void putOnClipboard(const ClipboardManager& manager, const std::string& text, bool sensitive) {
    if (!airlink::platform::writeClipboard(text, sensitive)) manager.setText(text);
}
inline bool clipboardSensitive(const Clipboard& clip) {
    if (auto w = airlink::platform::watchedClipboard()) return w->sensitive;
    return clipSensitive(clip);
}
inline void setAppForeground(bool focused) { airlink::platform::setAppForeground(focused); }
inline void publishStatus(const std::string& status, const std::string& tip, int received) {
    airlink::platform::publishStatus(status, tip, received);
}
inline void showInFolder(const std::string& path) { airlink::platform::showInFolder(path); }
inline void shareWithApps(const std::string& path) { airlink::platform::shareWithApps(path); }

// "cachyPC cihazından · 12,3 MB · 26 Kas 07:58", or that the file is gone.
inline std::string fileLine(const FileRecord& f, bool exists) {
    std::time_t t = std::time_t(f.time / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    static const char* const months[] = {"Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"};
    static const char* const monthsEnglish[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char when[32];
    std::snprintf(when, sizeof when, "%d %s %02d:%02d", local.tm_mday, (english() ? monthsEnglish : months)[local.tm_mon], local.tm_hour, local.tm_min);
    std::string who = f.peerName.empty() ? tr("Bir cihaz") : f.peerName;
    std::string line = (f.incoming ? trf("{} cihazından", who) : trf("{} cihazına", who)) + " · " + when;
    if (!exists) return line + " · " + (f.isUri() ? tr("dosyaya artık erişilemiyor") : tr("dosya artık yok"));
    return f.size > 0 ? line + " · " + humanSize(f.size) : line;
}

inline std::string desktopRequestText() {
    std::string text = tr("Ekranınızı görecek, fare ve klavyeyi kullanabilecek. İstediğiniz zaman buradan durdurabilirsiniz.");
#if defined(__linux__) && !defined(__ANDROID__)
    text += tr("\n\nMasaüstü ilk seferde hangi ekranın paylaşılacağını da soracak.");
#endif
    return text;
}

inline const std::vector<std::string>& desktopModifiers() {
    static const std::vector<std::string> list = {"Control", "Alt", "Shift", "Super"};
    return list;
}

inline const std::vector<std::string>& desktopKeys() {
    static const std::vector<std::string> list = {"Escape", "Tab", "Enter", "Backspace", "Left", "Up", "Down", "Right"};
    return list;
}

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "03:25" (or "1:03:25") since `since` (ms).
inline std::string elapsed(int64_t since, int64_t now) {
    int64_t s = std::max<int64_t>(0, (now - since) / 1000);
    char buf[16];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

inline std::string routeLabel(const std::string& route) {
    if (route == "speaker") return tr("Hoparlör");
    if (route == "bluetooth") return "Bluetooth";
    if (route == "wired") return tr("Kablolu kulaklık");
    if (route == "streaming") return tr("Akış");
    return tr("Ahize");
}

inline std::string routeBody(const std::string& route) { return "{\"route\":\"" + route + "\"}"; }

inline std::string callStateLabel(const ActiveCall& c) {
    if (c.held || c.state == "held") return tr("Beklemede");
    if (c.state == "dialing") return tr("Aranıyor");
    return tr("Görüşmede");
}

// Up to two letters of a contact's name for its avatar; "" for a bare number (a person icon).
inline std::string initials(const std::string& title) {
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

inline std::vector<std::vector<std::string>> keypadRows() {
    return {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {"*", "0", "#"}};
}

inline std::vector<std::string> quickReplies() {
    return {tr("Şu an konuşamıyorum, sonra ararım."), tr("Toplantıdayım, birazdan dönerim."), tr("Yoldayım, sonra ararım."),
            tr("Mesaj atar mısın?")};
}

inline std::string spacedCode(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

inline bool isDesktop() {
#if defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

// ---- Screens ---------------------------------------------------------------------------------
