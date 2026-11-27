#include "app_state.hpp"

#include <airlink.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "cloud_state.hpp"
#include "json.hpp"
#include "platform.hpp"

namespace airlink {

using kompoz::MutableState;
using kompoz::mutableStateOf;

namespace {

std::string takeString(char* s) {
    if (!s) return {};
    std::string out(s);
    airlink_free_string(s);
    return out;
}

std::string cleanError(std::string e) {
    if (e.find("declined") != std::string::npos) return "Karşı taraf reddetti";
    if (e.find("offline") != std::string::npos) return "Cihaz çevrimdışı";
    if (e.find("not paired") != std::string::npos) return "Cihaz eşleşmemiş";
    return e;
}

std::optional<uint16_t> parseAdbPort(const std::string& text) {
    if (text.empty()) return 0;
    unsigned port = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (error != std::errc() || end != text.data() + text.size() || port == 0 || port > 65535) return std::nullopt;
    return static_cast<uint16_t>(port);
}

enum class OpKind { StartPairing, ConfirmPairing, SendFiles, SendClipboard, SendReply, ScreenStart, AdbPair, AdbTest, AdbHelper, DesktopShare };

std::vector<uint8_t> base64(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        size_t v = chars.find(c);
        if (v == std::string::npos) continue;  // '=', newlines
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(buf >> bits));
        }
    }
    return out;
}

}  // namespace

struct App::Impl {
    AirlinkNode* node = nullptr;
    bool startAttempted = false;

    MutableState<bool> ready = mutableStateOf(false);
    MutableState<bool> sharingPaused = mutableStateOf(false);
    MutableState<bool> recentError = mutableStateOf(false);
    int recentErrorUntilTick = 0;
    MutableState<std::string> startError = mutableStateOf(std::string());
    MutableState<DeviceInfo> me = mutableStateOf(DeviceInfo{});
    MutableState<std::vector<Peer>> peers = mutableStateOf(std::vector<Peer>{});
    MutableState<std::vector<Transfer>> transfers = mutableStateOf(std::vector<Transfer>{});
    MutableState<std::vector<Prompt>> prompts = mutableStateOf(std::vector<Prompt>{});
    MutableState<std::optional<OutgoingPairing>> outgoing = mutableStateOf(std::optional<OutgoingPairing>{});
    MutableState<std::vector<std::string>> share = mutableStateOf(std::vector<std::string>{});
    MutableState<std::vector<PhoneNotification>> phoneNotifications = mutableStateOf(std::vector<PhoneNotification>{});
    MutableState<bool> notificationAccess = mutableStateOf(false);
    MutableState<bool> batteryRestricted = mutableStateOf(false);
    MutableState<std::vector<PhoneBattery>> phoneBatteries = mutableStateOf(std::vector<PhoneBattery>{});
    MutableState<std::vector<SmsThread>> smsThreads = mutableStateOf(std::vector<SmsThread>{});
    MutableState<std::optional<SmsThread>> smsOpen = mutableStateOf(std::optional<SmsThread>{});
    MutableState<std::vector<SmsMessage>> smsMessages = mutableStateOf(std::vector<SmsMessage>{});
    MutableState<std::string> smsError = mutableStateOf(std::string());
    MutableState<bool> smsLoading = mutableStateOf(false);
    MutableState<bool> smsAccess = mutableStateOf(false);
    MutableState<std::vector<ContactMatch>> contactResults = mutableStateOf(std::vector<ContactMatch>{});
    std::string contactQuery;  // answers to older searches are dropped
    MutableState<std::vector<ContactMatch>> phoneContacts = mutableStateOf(std::vector<ContactMatch>{});
    MutableState<bool> phoneContactsLoading = mutableStateOf(false);
    MutableState<bool> phoneContactsHasMore = mutableStateOf(false);
    MutableState<std::string> phoneContactsError = mutableStateOf(std::string());
    std::string phoneContactsQuery;
    std::string phoneContactsPeer;
    int phoneContactsOffset = 0;
    MutableState<std::optional<IncomingCall>> incomingCall = mutableStateOf(std::optional<IncomingCall>{});
    MutableState<std::vector<CallEntry>> callLog = mutableStateOf(std::vector<CallEntry>{});
    MutableState<bool> callLogLoading = mutableStateOf(false);
    MutableState<bool> callLogHasMore = mutableStateOf(false);
    int callLogOffset = 0;
    std::string callLogPeer;
    MutableState<std::string> callsError = mutableStateOf(std::string());
    MutableState<bool> callAccess = mutableStateOf(false);
    MutableState<std::optional<ActiveCall>> activeCall = mutableStateOf(std::optional<ActiveCall>{});
    bool callAudioSupported = platform::canBridgeCallAudio();
    bool mediaAudioSupported = platform::canPlayMediaAudio();
    std::string callAudioPeer;
    std::string callAudioAutoPeer;
    std::string callAudioOnAnswer;  // this call only: bridge its audio once it is answered
    bool callAudioAutoAttempted = false;
    MutableState<bool> phoneAudioAutoConnect = mutableStateOf(false);
    MutableState<bool> phoneCallAudioEnabled = mutableStateOf(false);
    std::string phoneCallAudioPeer;
    MutableState<std::string> callAudioMessage = mutableStateOf(std::string());
    int nextBluetoothRouteTick = 0;
    MutableState<std::string> screenPeer = mutableStateOf(std::string());
    MutableState<bool> mediaAudioEnabled = mutableStateOf(false);
    MutableState<std::string> mediaAudioMessage = mutableStateOf(std::string());
    MutableState<std::string> mediaAudioTarget = mutableStateOf(std::string());
    std::string mediaAudioPeer;
    std::string playerBluetoothPeer;
    std::string playerBluetoothName;
    std::string playerBluetoothReportedStatus;
    std::chrono::steady_clock::time_point playerBluetoothExpires;
    MutableState<bool> screenShareAudio = mutableStateOf(false);
    MutableState<bool> pauseScreenAudioDuringMusic = mutableStateOf(true);
    std::string desktopAudioRecipient;
    std::string screenStopPeer;
    MutableState<std::string> screenStatus = mutableStateOf(std::string());
    MutableState<kompoz::ImageBitmap> screenFrame = mutableStateOf(kompoz::ImageBitmap());
    MutableState<bool> screenControl = mutableStateOf(false);
    MutableState<std::string> desktopPeer = mutableStateOf(std::string());
    MutableState<std::string> desktopStatus = mutableStateOf(std::string());
    MutableState<kompoz::ImageBitmap> desktopFrame = mutableStateOf(kompoz::ImageBitmap());
    MutableState<std::optional<DesktopRequest>> desktopRequest = mutableStateOf(std::optional<DesktopRequest>{});
    MutableState<std::string> desktopViewer = mutableStateOf(std::string());
    MutableState<bool> flingOn = mutableStateOf(false);
    MutableState<bool> flingMedia = mutableStateOf(false);
    std::string lastComputer;  // the computer last sent to or controlled: a fling's target
    MutableState<bool> advancedReady = mutableStateOf(false);
    MutableState<std::string> advancedStatus = mutableStateOf(std::string());
    MutableState<std::string> advancedPort = mutableStateOf(std::string());
    MutableState<std::vector<PhoneApp>> phoneApps = mutableStateOf(std::vector<PhoneApp>{});
    MutableState<bool> phoneAppsLoading = mutableStateOf(false);
    struct Wheel {
        double x = 0, y = 0, h = 0, v = 0;
    };
    std::map<int, Wheel> wheels;  // per app session: the turns not sent yet
    Wheel screenWheel;
    bool screenWheelPending = false;
    int nextScreenWheelTick = 0;
    std::string appsListing;                          // the listing whose parts are arriving
    std::map<int, std::vector<PhoneApp>> appsParts;   // its parts so far
    MutableState<std::vector<AppWindow>> appWindows = mutableStateOf(std::vector<AppWindow>{});
    int nextSession = 1;
    std::map<std::string, AirlinkH264Decoder*> videoDecoders;

    kompoz::ImageBitmap decodeMedia(const std::string& stream, const uint8_t* data, size_t len) {
        if (len >= 4 && std::equal(data, data + 4, "ALH1")) {
            auto& decoder = videoDecoders[stream];
            if (!decoder) decoder = airlink_h264_decoder_new();
            uint8_t* pixels = nullptr;
            size_t bytes = 0;
            int width = 0, height = 0;
            if (decoder && airlink_h264_decode(decoder, data, len, &pixels, &bytes, &width, &height)) {
                std::vector<uint8_t> rgba(pixels, pixels + bytes);
                airlink_free_bytes(pixels, bytes);
                return kompoz::ImageBitmap::fromPixels(width, height, std::move(rgba));
            }
            return {};
        }
        return kompoz::decodeToImageBitmap(std::vector<uint8_t>(data, data + len));
    }

    void removeDecoder(const std::string& stream) {
        auto it = videoDecoders.find(stream);
        if (it == videoDecoders.end()) return;
        airlink_h264_decoder_free(it->second);
        videoDecoders.erase(it);
    }

    void updateWindow(int session, const std::function<void(AppWindow&)>& change) {
        appWindows.update([&](std::vector<AppWindow>& list) {
            for (AppWindow& w : list)
                if (w.session == session) change(w);
        });
    }

    void removeWindow(int session) {
        removeDecoder("app:" + std::to_string(session));
        appWindows.update([&](std::vector<AppWindow>& list) { std::erase_if(list, [&](const AppWindow& w) { return w.session == session; }); });
    }

    std::string adbDir() const {
        auto cfg = platform::config();
        return (cfg && !cfg->dataDir.empty() ? cfg->dataDir : std::string(".")) + "/adb";
    }
    int smsRefreshAt = -1;  // a tick to ask the open conversation again (a sent SMS is recorded late)

    std::vector<std::string> notices;
    std::vector<std::string> incomingClipboard;
    MutableState<std::vector<ClipboardItem>> clipboardHistory = mutableStateOf(std::vector<ClipboardItem>{});
    MutableState<PhoneMedia> phoneMedia = mutableStateOf(PhoneMedia{});
    MutableState<int> receivedCount = mutableStateOf(0);
    // The Files tab's list, newest first, in <data dir>/file-history.json.
    MutableState<std::vector<FileRecord>> fileHistory = mutableStateOf(std::vector<FileRecord>{});
    int64_t nextFileId = 1;
    MutableState<int> filesOnDisk = mutableStateOf(0);  // bumped as AirLink deletes one: the cards look at the disk again
    std::string fileHistoryPath() const {
        auto dir = me.peek().dataDir;
        return dir.empty() ? std::string() : dir + "/file-history.json";
    }
    void loadFileHistory() {
        std::string path = fileHistoryPath();
        if (path.empty()) return;
        std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())));
        if (!in) return;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        json::Value parsed = json::Value::parse(text);
        std::vector<FileRecord> list;
        for (const auto& f : parsed.array())
            list.push_back(FileRecord{.id = nextFileId++, .peerId = f["peer_id"].str(), .peerName = f["peer_name"].str(),
                                      .path = f["path"].str(), .size = f["size"].i64(), .time = f["time"].i64(),
                                      .incoming = f["incoming"].boolean(), .displayName = f["name"].str()});
        fileHistory.set(std::move(list));
    }
    void saveFileHistory() {
        std::string path = fileHistoryPath();
        if (path.empty()) return;
        std::string out = "[";
        for (const FileRecord& f : fileHistory.peek()) {
            if (out.size() > 1) out += ",\n";
            out += "{\"peer_id\":" + json::quote(f.peerId) + ",\"peer_name\":" + json::quote(f.peerName) +
                   ",\"path\":" + json::quote(f.path) + ",\"size\":" + std::to_string(f.size) +
                   ",\"time\":" + std::to_string(f.time) + ",\"incoming\":" + (f.incoming ? "true" : "false") +
                   (f.displayName.empty() ? std::string() : ",\"name\":" + json::quote(f.displayName)) + "}";
        }
        out += "]\n";
        auto file = std::filesystem::path(std::u8string(path.begin(), path.end()));
        auto temp = file;
        temp += ".tmp";
        { std::ofstream(temp, std::ios::binary) << out; }
        std::error_code ec;
        std::filesystem::rename(temp, file, ec);
    }
    void recordFiles(const std::string& peerId, const std::vector<std::string>& paths, bool incoming) {
        if (paths.empty()) return;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string name = peerNameOf(peerId);
        fileHistory.update([&](std::vector<FileRecord>& list) {
            for (const std::string& p : paths) {
                std::error_code ec;
                auto fs = std::filesystem::path(std::u8string(p.begin(), p.end()));
                int64_t size = std::filesystem::is_regular_file(fs, ec) ? int64_t(std::filesystem::file_size(fs, ec)) : 0;
                FileRecord record{.id = nextFileId++, .peerId = peerId, .peerName = name, .path = p, .size = size, .time = now,
                                  .incoming = incoming};
                // A copy made for sending (Android): the original is what the list keeps; the copy goes.
                if (!incoming) {
                    auto [uri, original] = platform::takeSentOriginal(p);
                    if (!uri.empty()) record.path = uri, record.displayName = original;
                }
                list.insert(list.begin(), record);
            }
            if (list.size() > 500) list.resize(500);
        });
        saveFileHistory();
    }
    MutableState<App::PanelRequest> panelRequest = mutableStateOf(App::PanelRequest{});
    std::string lastClipboard;   // this device's clipboard as last seen or set
    bool lastClipboardSensitive = false;
    MutableState<bool> passwordPrivacy = mutableStateOf(true);
    MutableState<bool> passwordPending = mutableStateOf(false);
    std::string pendingPassword;  // waiting for the user's yes (password privacy)
    std::string settingPath(const char* name) const {
        auto dir = me.peek().dataDir;
        return dir.empty() ? std::string() : dir + "/" + name;
    }
    void sendToTrusted(const std::string& text, bool sensitive) {
        for (const Peer& p : peers.peek()) {
            if (!p.paired || !p.online || !p.autoAccept) continue;
            track(airlink_send_clipboard_marked(node, p.id.c_str(), text.c_str(), sensitive), OpKind::SendClipboard);
            rememberClipboard(p.id, text, false, sensitive);
        }
    }
    bool clipboardSeen = false;  // the first reading is the clipboard AirLink found, not a copy
    bool trusts(const std::string& peerId) const {
        const auto& list = peers.peek();
        return std::any_of(list.begin(), list.end(), [&](const Peer& p) { return p.id == peerId && p.paired && p.autoAccept; });
    }
    int64_t nextClipboardId = 1;
    void rememberClipboard(const std::string& peerId, const std::string& text, bool incoming, bool sensitive = false) {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        clipboardHistory.update([&](std::vector<ClipboardItem>& list) {
            list.insert(list.begin(), ClipboardItem{.id = nextClipboardId++, .peerId = peerId, .text = text,
                                                    .time = now, .incoming = incoming, .sensitive = sensitive});
            if (list.size() > 100) list.resize(100);
        });
    }
    std::map<uint64_t, OpKind> ops;
    int ticks = 0;

    // Filled by the platform's file picker on any thread, applied in pump().
    std::mutex pickedMutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> picked;
    std::vector<std::string> shared;  // from platform::setShareHandler, any thread
    std::vector<std::string> onlinePaired;  // for sends from any thread (under pickedMutex)

    ~Impl() {
        for (auto& [_, decoder] : videoDecoders) airlink_h264_decoder_free(decoder);
        if (node) airlink_stop(node);
    }

    void start() {
        startAttempted = true;
        auto cfg = *platform::config();
        if (!cfg.dataDir.empty()) {
            std::ifstream saved(cfg.dataDir + "/adb/connect-port");
            std::string port;
            if (std::getline(saved, port) && parseAdbPort(port)) advancedPort.set(port);
            std::ifstream target(cfg.dataDir + "/fling-target");
            std::getline(target, lastComputer);
#if defined(__ANDROID__)
            std::ifstream audioAuto(cfg.dataDir + "/audio-auto-connect");
            bool enabled = false;
            if (audioAuto >> enabled) phoneAudioAutoConnect.set(enabled);
            std::ifstream pauseAudio(cfg.dataDir + "/pause-screen-audio-during-music");
            if (pauseAudio >> enabled) pauseScreenAudioDuringMusic.set(enabled);
            platform::setPauseScreenAudioDuringMusic(pauseScreenAudioDuringMusic.peek());
#endif
        }
        flingOn.set(platform::flingEnabled());
        flingMedia.set(platform::flingMediaAccess());
        std::string json = "{";
        auto field = [&](const char* key, const std::string& value) {
            if (value.empty()) return;
            if (json.size() > 1) json += ",";
            json += json::quote(key) + ":" + json::quote(value);
        };
        field("data_dir", cfg.dataDir);
        field("download_dir", cfg.downloadDir);
        field("fallback_name", cfg.fallbackName);
        json += "}";
        char* err = nullptr;
        node = airlink_start(json.c_str(), &err);
        if (!node) {
            std::string message = takeString(err);
            startError.set(message);
            platform::log("core start failed: " + message);
            return;
        }
        auto info = json::Value::parse(takeString(airlink_info(node)));
        me.set(DeviceInfo{.id = info["id"].str(),
                          .name = info["name"].str(),
                          .platform = info["platform"].str(),
                          .port = int(info["port"].i64()),
                          .downloadDir = info["download_dir"].str(),
                          .dataDir = info["data_dir"].str()});
        loadFileHistory();
        Drives::get().attach(node, me.peek().downloadDir, me.peek().dataDir);
        if (auto path = settingPath("password-privacy"); !path.empty()) {
            std::ifstream in(path);
            int on = 1;
            if (in >> on) passwordPrivacy.set(on != 0);
        }
        refreshPeers();
        startError.set({});
        recentError.set(false);
        ready.set(true);
        platform::nodeStarted(node);
    }

    void refreshPeers() {
        // Keep the parsed value in a variable: a range-for over parse(...).array() would iterate
        // the array of a temporary already destroyed (C++20 doesn't extend its lifetime).
        json::Value parsed = json::Value::parse(takeString(airlink_peers(node)));
        std::vector<Peer> list;
        for (const auto& p : parsed.array())
            list.push_back(Peer{.id = p["id"].str(),
                                .name = p["name"].str(),
                                .platform = p["platform"].str(),
                                .paired = p["paired"].boolean(),
                                .online = p["online"].boolean(),
                                .autoAccept = p["auto_accept"].boolean()});
        std::vector<std::string> wasOnline;
        {
            std::lock_guard lock(pickedMutex);
            wasOnline.swap(onlinePaired);
            for (const Peer& p : list)
                if (p.paired && p.online) onlinePaired.push_back(p.id);
        }
        // A phone that just came online: its battery (it tells the changes itself afterwards).
        for (const Peer& p : list)
            if (p.paired && p.online && p.platform == "android" && std::find(wasOnline.begin(), wasOnline.end(), p.id) == wasOnline.end())
                airlink_send_message(node, p.id.c_str(), "phone.battery_request", "{}");
        // An offline phone's battery is old news; its ringing too.
        auto gone = [&](const PhoneBattery& b) {
            return std::none_of(list.begin(), list.end(), [&](const Peer& p) { return p.id == b.peerId && p.online; });
        };
        if (auto batteries = phoneBatteries.peek(); std::any_of(batteries.begin(), batteries.end(), gone))
            phoneBatteries.update([&](std::vector<PhoneBattery>& all) { std::erase_if(all, gone); });
        auto online = [&](const std::string& id) {
            return std::any_of(list.begin(), list.end(), [&](const Peer& p) { return p.id == id && p.paired && p.online; });
        };
        if (!desktopViewer.peek().empty() && !online(desktopViewer.peek())) {
            airlink_desktop_stop(node, desktopViewer.peek().c_str(), "viewer_lost");
            desktopViewer.set({});
            screenShareAudio.set(false);
            stopDesktopAudio();
        }
        if (!callAudioAutoPeer.empty() && !online(callAudioAutoPeer)) {
            callAudioAutoPeer.clear();
            callAudioAutoAttempted = false;
            platform::stopCallAudio();
            callAudioPeer.clear();
        }
#if defined(__ANDROID__)
        for (const auto& previous : peers.peek())
            if (previous.paired && previous.online && !online(previous.id))
                platform::stopPhoneScreenForPeer(previous.id);
        if (!mediaAudioPeer.empty() && !online(mediaAudioPeer)) {
            if (std::none_of(list.begin(), list.end(), [&](const Peer& p) {
                    return p.id == mediaAudioPeer && p.paired;
                })) {
                App::get().setMediaAudioEnabled(false);
            } else {
                mediaAudioMessage.set("Bilgisayarın bağlanması bekleniyor");
            }
        }
        if (!phoneCallAudioPeer.empty() && !online(phoneCallAudioPeer)) {
            phoneCallAudioPeer.clear();
            phoneCallAudioEnabled.set(false);
        }
#endif
        peers.set(std::move(list));
    }

    void updateTransfer(const std::string& id, const std::string& peerId, bool incoming,
                        const std::function<void(Transfer&)>& change) {
        transfers.update([&](std::vector<Transfer>& list) {
            auto it = std::find_if(list.begin(), list.end(), [&](const Transfer& t) { return t.id == id; });
            if (it == list.end()) {
                list.insert(list.begin(), Transfer{.id = id, .peerId = peerId, .incoming = incoming});
                it = list.begin();
            }
            change(*it);
        });
    }

    void markRecentError() {
        recentError.set(true);
        recentErrorUntilTick = ticks + 600;  // 30 seconds at the 50 ms pump interval
    }

    void removePrompt(uint64_t requestId) {
        prompts.update([&](std::vector<Prompt>& list) {
            std::erase_if(list, [&](const Prompt& p) { return p.requestId == requestId; });
        });
    }

    void handle(const json::Value& e) {
        if (Drives::get().handle(e)) return;
        std::string kind = e["event"].str();
        if (kind == "peer_discovered" || kind == "peer_lost") {
            refreshPeers();
        } else if (kind == "pairing_request") {
            prompts.update([&](std::vector<Prompt>& list) {
                list.push_back(Prompt{.requestId = e["request_id"].u64(),
                                      .isPairing = true,
                                      .peerName = e["peer"]["name"].str(),
                                      .code = e["code"].str()});
            });
        } else if (kind == "file_offer") {
            Prompt p{.requestId = e["request_id"].u64(), .peerName = e["peer"]["name"].str(), .totalSize = e["total_size"].i64()};
            for (const auto& f : e["files"].array()) p.files.push_back({f["path"].str(), f["size"].i64()});
            prompts.update([&](std::vector<Prompt>& list) { list.push_back(std::move(p)); });
        } else if (kind == "request_answered") {
            removePrompt(e["request_id"].u64());  // answered elsewhere (a notification's button)
        } else if (kind == "request_cancelled") {
            uint64_t id = e["request_id"].u64();
            bool open = std::any_of(prompts.peek().begin(), prompts.peek().end(),
                                    [&](const Prompt& p) { return p.requestId == id; });
            removePrompt(id);
            if (open) notices.push_back("İstek karşı taraftan iptal edildi");
        } else if (kind == "pairing_completed") {
            refreshPeers();
            if (e["success"].boolean()) notices.push_back(e["peer"]["name"].str() + " ile eşleşildi");
        } else if (kind == "transfer_pending") {
            updateTransfer(e["transfer_id"].str(), e["peer_id"].str(), false, [&](Transfer& t) {
                t.waiting = true;
                t.total = e["bytes_total"].i64();
                t.fileCount = int(e["file_count"].i64());
            });
        } else if (kind == "transfer_progress") {
            updateTransfer(e["transfer_id"].str(), e["peer_id"].str(), e["direction"].str() == "incoming", [&](Transfer& t) {
                t.waiting = false;
                t.done = e["bytes_done"].i64();
                t.total = e["bytes_total"].i64();
            });
        } else if (kind == "transfer_completed") {
            bool incoming = e["direction"].str() == "incoming";
            std::vector<std::string> files;
            for (const auto& f : e["files"].array()) files.push_back(f.str());
            updateTransfer(e["transfer_id"].str(), e["peer_id"].str(), incoming, [&](Transfer& t) {
                t.completed = true;
                t.waiting = false;
                t.done = t.total;
                t.fileCount = int(files.size());
            });
            notices.push_back(std::to_string(files.size()) + (incoming ? " dosya alındı" : " dosya gönderildi"));
            recordFiles(e["peer_id"].str(), files, incoming);
            if (incoming) {
                platform::filesReceived(files);
                platform::notifyReceived(peerNameOf(e["peer_id"].str()), files, {});
                receivedCount.set(receivedCount.peek() + 1);
            }
        } else if (kind == "transfer_failed") {
            markRecentError();
            updateTransfer(e["transfer_id"].str(), e["peer_id"].str(), e["direction"].str() == "incoming", [&](Transfer& t) {
                t.waiting = false;
                t.error = cleanError(e["error"].str());
            });
        } else if (kind == "media_started") {
            if (e["name"].str() == "screen" && e["peer_id"].str() == screenPeer.peek()) screenStatus.set("live");
        } else if (kind == "media_stopped") {
            std::string name = e["name"].str();
            removeDecoder(name);
            if (name == "screen_audio" && e["peer_id"].str() == screenPeer.peek()) platform::stopMediaAudio();
            if (name == "desktop_audio" && e["peer_id"].str() == desktopPeer.peek()) platform::stopMediaAudio();
            if (name == "screen" && e["peer_id"].str() == screenPeer.peek()) closeScreen();
            if (name == "desktop" && e["peer_id"].str() == desktopPeer.peek()) closeDesktop();
        } else if (kind == "desktop_started") {
            desktopViewer.set(e["peer_id"].str());
            updateDesktopAudio();
            notices.push_back("Ekran " + peerNameOf(e["peer_id"].str()) + " ile paylaşılıyor");
        } else if (kind == "desktop_stopped") {
            if (desktopViewer.peek() == e["peer_id"].str()) {
                desktopViewer.set({});
                screenShareAudio.set(false);
                stopDesktopAudio();
            }
            std::string error = e["error"].str();
            if (!error.empty()) notices.push_back("Ekran paylaşılamadı: " + error);
        } else if (kind == "app_message") {
            handleAppMessage(e["peer"], e["kind"].str(), e["body"]);
        } else if (kind == "clipboard_received") {
            bool secret = e["sensitive"].boolean();
            incomingClipboard.push_back(e["text"].str());
            if (secret) lastClipboardSensitive = true;
            rememberClipboard(e["peer"]["id"].str(), e["text"].str(), true, secret);
            notices.push_back(e["peer"]["name"].str() + (secret ? " bir şifre gönderdi" : " panosunu paylaştı"));
            platform::notifyReceived(e["peer"]["name"].str(), {}, e["text"].str(), secret);
            receivedCount.set(receivedCount.peek() + 1);
        } else if (kind == "pairing_code") {
            ops.erase(e["op"].u64());
            outgoing.set(OutgoingPairing{.pairing = e["pairing"].u64(), .peerName = e["peer"]["name"].str(), .code = e["code"].str()});
        } else if (kind == "op_result") {
            uint64_t op = e["op"].u64();
            auto it = ops.find(op);
            if (it == ops.end()) return;
            OpKind what = it->second;
            ops.erase(it);
            bool ok = e["ok"].boolean();
            if (!ok) markRecentError();
            std::string err = cleanError(e["error"].str());
            switch (what) {
                case OpKind::StartPairing:
                    if (!ok) notices.push_back("Eşleşme başlatılamadı: " + err);
                    break;
                case OpKind::ConfirmPairing:
                    outgoing.set(std::nullopt);
                    if (!ok) notices.push_back("Eşleşme başarısız: " + err);
                    else if (!e["value"].boolean()) notices.push_back("Eşleşme reddedildi");
                    refreshPeers();
                    break;
                case OpKind::SendFiles:
                    break;  // shown by the transfer's own events
                case OpKind::SendClipboard:
                    notices.push_back(ok ? "Pano gönderildi" : "Pano gönderilemedi: " + err);
                    break;
                case OpKind::ScreenStart:
                    if (!ok) {
                        closeScreen();
                        notices.push_back("Ekran açılamadı: " + err);
                    }
                    break;
                case OpKind::AdbPair:
                    if (ok) {
                        advancedStatus.set("Eşleşildi, kabuk yetkisi deneniyor…");
                        auto port = parseAdbPort(advancedPort.peek());
                        track(airlink_adb_shell_port(node, adbDir().c_str(), "id", port.value_or(0)), OpKind::AdbTest);
                    } else {
                        advancedStatus.set("Eşleşme olmadı: " + err);
                        platform::advancedSetupResult(false, err);
                    }
                    break;
                case OpKind::AdbTest: {
                    std::string out = e["value"].str();
                    bool shell = ok && out.find("uid=2000") != std::string::npos;
                    advancedReady.set(shell);
                    advancedStatus.set(shell ? "Hazır: kabuk yetkisiyle çalışıyor (" + out.substr(0, out.find(' ')) + ")"
                                             : ok ? "Beklenmeyen yanıt: " + out : "Bağlanılamadı: " + err);
                    platform::advancedSetupResult(shell, shell ? "Gelişmiş mod hazır" : (ok ? out : err));
                    break;
                }
                case OpKind::AdbHelper:
                    platform::helperStarted(ok, err);
                    break;
                case OpKind::DesktopShare:
                    break;  // shown by desktop_started / desktop_stopped
                case OpKind::SendReply:
                    notices.push_back(ok ? "Yanıt gönderildi" : "Yanıt gönderilemedi: " + err);
                    break;
            }
        }
    }

    // What a computer asks of this phone (answered by the platform: SmsBridge, CallBridge).
    static bool isPhoneRequest(const std::string& kind) {
        static const char* const kinds[] = {"sms.threads_request", "sms.thread_request", "sms.send", "contacts.search", "contacts.list",
                                            "calls.log_request", "call.answer", "call.reject", "call.dial", "call.hangup",
                                            "call.hold", "call.mute", "call.route", "call.volume", "call.status_request", "call.dtmf",
                                            "media.state_request", "media.control",
                                            "phone.battery_request", "phone.find", "phone.find_stop",
                                            "screen.start", "screen.stop", "screen.input",
                                            "apps.list", "apps.open", "apps.close", "apps.input"};
        return std::find(std::begin(kinds), std::end(kinds), kind) != std::end(kinds);
    }

    void handleAppMessage(const json::Value& peer, const std::string& kind, const json::Value& body) {
        std::string peerId = peer["id"].str();
        if (kind == "notification.posted") {
            PhoneNotification n{.peerId = peerId,
                                .key = body["key"].str(),
                                .app = body["app"].str(),
                                .title = body["title"].str(),
                                .text = body["text"].str(),
                                .time = body["time"].i64(),
                                .canReply = body["can_reply"].boolean()};
            bool fresh = true;
            phoneNotifications.update([&](std::vector<PhoneNotification>& list) {
                auto same = [&](const PhoneNotification& o) { return o.peerId == n.peerId && o.key == n.key; };
                fresh = std::none_of(list.begin(), list.end(), same);
                std::erase_if(list, same);
                list.insert(list.begin(), n);
                if (list.size() > 100) list.resize(100);
            });
            // An update of one already shown (a progress bar, a typing message) doesn't pop up again.
            if (fresh) platform::showDesktopNotification(n.app.empty() ? n.title : n.app + ": " + n.title, n.text);
        } else if (kind == "notification.removed") {
            std::string key = body["key"].str();
            phoneNotifications.update([&](std::vector<PhoneNotification>& list) {
                std::erase_if(list, [&](const PhoneNotification& o) { return o.peerId == peerId && o.key == key; });
            });
        } else if (kind == "notification.dismiss" || kind == "notification.reply") {
            platform::notificationAction(kind, body["key"].str(), body["text"].str());
        } else if (kind == "desktop.start") {
            // A paired device asks for this computer's screen: the user decides.
            if (!airlink_desktop_supported()) {
                sendDesktop(peerId, "desktop.stopped", "{\"reason\":\"unsupported\"}");
            } else if (desktopViewer.peek() == peerId) {
                track(airlink_desktop_start(node, peerId.c_str(), body.dump().c_str()), OpKind::DesktopShare);
            } else if (trusts(peerId)) {
                desktopRequest.set(DesktopRequest{.peerId = peerId, .peerName = peer["name"].str(), .options = body.dump()});
                App::get().answerDesktop(true);  // a trusted device: no question
            } else {
                desktopRequest.set(DesktopRequest{.peerId = peerId, .peerName = peer["name"].str(), .options = body.dump()});
                platform::showDesktopNotification(peer["name"].str() + " bu bilgisayarı kontrol etmek istiyor", "AirLink'te onaylayın");
            }
        } else if (kind == "desktop.stop") {
            if (auto r = desktopRequest.peek(); r && r->peerId == peerId) desktopRequest.set(std::nullopt);
            airlink_desktop_stop(node, peerId.c_str(), "viewer");
        } else if (kind == "desktop.input") {
            airlink_desktop_input(node, peerId.c_str(), body.dump().c_str());
        } else if (kind == "desktop.started") {
            if (peerId == desktopPeer.peek()) desktopStatus.set("live");
        } else if (kind == "desktop.stopped") {
            if (peerId != desktopPeer.peek()) return;
            std::string why = body["reason"].str();
            if (why != "viewer") notices.push_back(stopReason(why));
            closeDesktop();
        } else if (isPhoneRequest(kind)) {
            platform::smsRequest(peerId, kind, body.dump());  // this phone answers
        } else if (kind == "sms.threads") {
            std::vector<SmsThread> list;
            for (const auto& t : body["threads"].array())
                list.push_back(SmsThread{.threadId = t["thread_id"].i64(), .address = t["address"].str(), .name = t["name"].str(),
                                         .snippet = t["snippet"].str(), .date = t["date"].i64(), .unread = int(t["unread"].i64())});
            smsThreads.set(std::move(list));
            smsLoading.set(false);
            smsError.set({});
        } else if (kind == "sms.messages") {
            auto open = smsOpen.peek();
            if (!open || open->threadId != body["thread_id"].i64()) return;
            std::vector<SmsMessage> list;
            for (const auto& m : body["messages"].array())
                list.push_back(SmsMessage{.id = m["id"].i64(), .body = m["body"].str(), .date = m["date"].i64(),
                                          .outgoing = m["outgoing"].boolean(), .failed = m["failed"].boolean()});
            smsMessages.set(std::move(list));
            smsLoading.set(false);
        } else if (kind == "sms.sent") {
            if (!body["ok"].boolean()) notices.push_back("SMS gönderilemedi: " + body["error"].str());
            smsRefreshAt = ticks + 30;  // ~1.5 s: the phone records it in the conversation meanwhile
        } else if (kind == "sms.received") {
            auto open = smsOpen.peek();
            if (open && (open->threadId == body["thread_id"].i64() || open->address == body["address"].str())) smsRefreshAt = ticks + 4;
            else if (!smsThreads.peek().empty()) smsRefreshAt = ticks + 4;
        } else if (kind == "apps.list_result") {
            // In numbered parts, in any order: the list is shown when all of a listing's are here.
            std::string list = body["list"].str();
            if (list != appsListing) {
                appsListing = list;
                appsParts.clear();
            }
            int parts = int(body["parts"].i64()), part = int(body["part"].i64());
            std::vector<PhoneApp> got;
            for (const auto& a : body["apps"].array())
                got.push_back(PhoneApp{.label = a["label"].str(), .component = a["component"].str(),
                                       .icon = kompoz::decodeToImageBitmap(base64(a["icon"].str()))});
            appsParts[part] = std::move(got);
            if (int(appsParts.size()) == parts) {
                std::vector<PhoneApp> all;
                for (auto& [i, p] : appsParts) all.insert(all.end(), p.begin(), p.end());
                phoneApps.set(std::move(all));
                phoneAppsLoading.set(false);
                appsParts.clear();
            }
        } else if (kind == "apps.event") {
            int session = int(body["session"].i64());
            std::string ev = body["event"].str();
            if (ev == "opened") updateWindow(session, [](AppWindow& w) { w.status = "live"; });
            else if (ev == "closed") removeWindow(session);
            else if (ev == "error") {
                std::string message = body["message"].str();
                notices.push_back(message.empty() ? "Uygulama açılamadı" : message);
                removeWindow(session);
            }
        } else if (kind == "screen.started") {
            if (peerId == screenPeer.peek()) screenStatus.set("live");
        } else if (kind == "player.audio") {
#if (defined(__linux__) || defined(_WIN32)) && !defined(__ANDROID__)
            if (peer["platform"].str() != "android") return;
            if (body["playing"].boolean()) {
                if (!playerBluetoothPeer.empty() && playerBluetoothPeer != peerId &&
                    std::chrono::steady_clock::now() < playerBluetoothExpires) return;
                playerBluetoothPeer = peerId;
                playerBluetoothName = peer["name"].str();
                playerBluetoothExpires = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            } else if (playerBluetoothPeer == peerId) {
                playerBluetoothPeer.clear();
            }
#endif
        } else if (kind == "phone.battery" || kind == "phone.find_state") {
            phoneBatteries.update([&](std::vector<PhoneBattery>& list) {
                auto it = std::find_if(list.begin(), list.end(), [&](const PhoneBattery& b) { return b.peerId == peerId; });
                if (it == list.end()) it = list.insert(list.end(), PhoneBattery{.peerId = peerId});
                if (kind == "phone.battery") {
                    it->level = int(body["level"].i64());
                    it->charging = body["charging"].boolean();
                } else {
                    it->ringing = body["ringing"].boolean();
                }
            });
        } else if (kind == "media.state") {
            PhoneMedia m{.peerId = peerId, .active = body["active"].boolean(), .app = body["app"].str(), .title = body["title"].str(),
                         .artist = body["artist"].str(), .album = body["album"].str(), .playing = body["playing"].boolean(),
                         .position = body["position"].i64(), .positionAt = body["position_at"].i64(), .duration = body["duration"].i64(),
                         .canNext = body["can_next"].boolean(), .canPrevious = body["can_previous"].boolean()};
            std::string art = body["art"].str();
            auto previous = phoneMedia.peek();
            if (!art.empty()) m.art = kompoz::decodeToImageBitmap(base64(art));
            else if (previous.peerId == peerId && previous.title == m.title && previous.artist == m.artist) m.art = previous.art;
            phoneMedia.set(std::move(m));
        } else if (kind == "player.audio.status") {
#if defined(__ANDROID__)
            if (peerId == mediaAudioPeer) App::get().setPlayerAudioStatus(body["status"].str());
#endif
        } else if (kind == "screen.stopped") {
            bool locallyClosed = peerId == screenStopPeer;
            if (peerId != screenPeer.peek() && !locallyClosed) return;
            screenStopPeer.clear();
            std::string why = body["reason"].str();
            if (why == "declined") notices.push_back("Ekran paylaşımı telefonda reddedildi");
            else if (why == "user") notices.push_back("Ekran paylaşımı telefonda durduruldu");
            else if (why == "busy") notices.push_back("Telefon başka bir paylaşım isteğiyle meşgul");
            closeScreen();
        } else if (kind == "call.ringing") {
            IncomingCall call{.peerId = peerId, .number = body["number"].str(), .name = body["name"].str()};
            incomingCall.set(call);
            platform::showDesktopNotification("📞 " + call.title() + " arıyor", peer["name"].str());
        } else if (kind == "call.status") {
            std::string state = body["state"].str();
            if (state == "ended" || state == "ringing" || state.empty()) {
                if (state == "ended") { activeCall.set(std::nullopt); callAudioAutoAttempted = false; callAudioOnAnswer.clear(); }
                return;
            }
            ActiveCall c{.peerId = peerId, .number = body["number"].str(), .name = body["name"].str(), .state = state,
                         .held = body["held"].boolean(), .canHold = body["can_hold"].boolean(), .muted = body["muted"].boolean(),
                         .route = body["route"].str(), .volume = int(body["volume"].i64()), .volumeMax = int(body["volume_max"].i64()),
                         .since = body["since"].i64(), .fullControl = body["full_control"].boolean()};
            for (const auto& r : body["routes"].array()) c.routes.push_back(r.str());
            if (auto prev = activeCall.peek(); prev && c.since == 0) c.since = prev->since;
            if (c.since == 0) c.since = int64_t(std::time(nullptr)) * 1000;
            activeCall.set(c);
            incomingCall.set(std::nullopt);
            if (state == "active" && callAudioOnAnswer == peerId && callAudioSupported) {
                callAudioOnAnswer.clear();
                callAudioAutoAttempted = true;
                App::get().startCallAudio();
            } else if (state == "active" && callAudioAutoPeer == peerId &&
                !callAudioAutoAttempted && callAudioSupported) {
                callAudioAutoAttempted = true;
                App::get().startCallAudio();
            }
        } else if (kind == "call.audio_share") {
#if !defined(__ANDROID__)
            if (body["on"].boolean()) {
                const bool once = body["once"].boolean();
                if (!once && callAudioAutoPeer != peerId) {
                    callAudioAutoPeer = peerId;
                    callAudioAutoAttempted = false;
                }
                if (auto c = activeCall.peek(); c && c->peerId == peerId && c->state == "active" &&
                    callAudioSupported && callAudioPeer != peerId && (once || !callAudioAutoAttempted)) {
                    if (!once) callAudioAutoAttempted = true;
                    App::get().startCallAudio();
                }
            } else {
                if (callAudioAutoPeer == peerId) {
                    callAudioAutoPeer.clear();
                    callAudioAutoAttempted = false;
                }
                if (callAudioPeer == peerId) App::get().stopCallAudio();
            }
#endif
        } else if (kind == "call.state") {
            incomingCall.set(std::nullopt);  // answered or hung up
            if (body["state"].str() == "idle") {  // the log has a new entry
                activeCall.set(std::nullopt);
                callAudioAutoAttempted = false;
                callLogStale = true;
                smsRefreshAt = ticks + 20;
            }
        } else if (kind == "calls.log") {
            if (peerId != callLogPeer || int(body["offset"].i64()) != callLogOffset) return;
            std::vector<CallEntry> list;
            for (const auto& c : body["calls"].array())
                list.push_back(CallEntry{.number = c["number"].str(), .name = c["name"].str(), .type = c["type"].str(),
                                         .date = c["date"].i64(), .duration = c["duration"].i64()});
            callLog.update([&](std::vector<CallEntry>& all) { all.insert(all.end(), list.begin(), list.end()); });
            callLogOffset = int(body["next_offset"].i64());
            callLogHasMore.set(body["has_more"].boolean());
            callLogLoading.set(false);
            callsError.set({});
        } else if (kind == "calls.error") {
            callLogLoading.set(false);
            callLogHasMore.set(false);
            callsError.set(body["error"].str() == "no_permission" ? "Telefonda AirLink'e arama izinleri verilmemiş. Telefondaki AirLink'ten izin verin."
                                                                  : body["error"].str());
        } else if (kind == "call.dial_result") {
            if (body["ok"].boolean()) notices.push_back(body["number"].str() + " telefondan aranıyor");
            else notices.push_back(body["error"].str() == "no_permission" ? "Telefonda arama izni yok" : "Aranamadı");
        } else if (kind == "contacts.results") {
            if (body["query"].str() != contactQuery) return;
            std::vector<ContactMatch> list;
            for (const auto& c : body["contacts"].array())
                list.push_back(ContactMatch{.name = c["name"].str(), .number = c["number"].str(), .label = c["label"].str()});
            contactResults.set(std::move(list));
        } else if (kind == "contacts.page") {
            if (peerId != phoneContactsPeer || body["query"].str() != phoneContactsQuery ||
                int(body["offset"].i64()) != phoneContactsOffset) return;
            phoneContactsLoading.set(false);
            if (body["error"].str() == "no_permission") {
                phoneContactsError.set("Telefonda AirLink'e rehber izni verilmemiş.");
                phoneContactsHasMore.set(false);
                return;
            }
            phoneContactsError.set({});
            std::vector<ContactMatch> page;
            for (const auto& c : body["contacts"].array())
                page.push_back(ContactMatch{.name = c["name"].str(), .number = c["number"].str(), .label = c["label"].str()});
            phoneContacts.update([&](std::vector<ContactMatch>& list) { list.insert(list.end(), page.begin(), page.end()); });
            phoneContactsOffset = int(body["next_offset"].i64());
            phoneContactsHasMore.set(body["has_more"].boolean());
        } else if (kind == "sms.error") {
            smsLoading.set(false);
            smsError.set(body["error"].str() == "no_permission" ? "Telefonda AirLink'e SMS izni verilmemiş. Telefondaki AirLink'ten izin verin."
                                                               : body["error"].str());
        }
    }

    std::string smsPhone() const {
        for (const Peer& p : peers.peek())
            if (p.paired && p.online && p.platform == "android") return p.id;
        return {};
    }

    // A request to the phone used for SMS and calls.
    void phoneAsk(const char* kind, const std::string& body) {
        std::string phone = smsPhone();
        if (node && !phone.empty()) airlink_send_message(node, phone.c_str(), kind, body.c_str());
    }

    void smsAsk(const char* kind, const std::string& body) {
        if (smsPhone().empty()) return;
        smsLoading.set(true);
        phoneAsk(kind, body);
    }

    void stopDesktopAudio() {
        platform::stopDesktopAudioCapture();
        if (node && !desktopAudioRecipient.empty())
            airlink_media_stop(node, desktopAudioRecipient.c_str(), "desktop_audio");
        desktopAudioRecipient.clear();
    }

    void updateDesktopAudio() {
        const std::string target = screenShareAudio.peek() ? desktopViewer.peek() : std::string();
        if (target == desktopAudioRecipient) return;
        stopDesktopAudio();
        if (!target.empty()) {
            desktopAudioRecipient = target;
            platform::startDesktopAudioCapture();
        }
    }

    void closeDesktop() {
#if defined(__ANDROID__)
        if (!desktopPeer.peek().empty()) platform::stopMediaAudio();
#endif
        removeDecoder("desktop");
        desktopPeer.set({});
        desktopStatus.set({});
        desktopFrame.set(kompoz::ImageBitmap());
    }

    static std::string stopReason(const std::string& why) {
        if (why == "declined") return "Bilgisayarda reddedildi";
        if (why == "user") return "Bilgisayarda durduruldu";
        if (why == "replaced") return "Başka bir cihaz bilgisayarı kontrol etmeye başladı";
        if (why == "unsupported") return "Bu bilgisayar ekranını paylaşamıyor";
        if (why == "failed") return "Ekran paylaşılamadı";
        return "Bilgisayar ekranı kapandı";
    }

    void closeScreen() {
#if !defined(__ANDROID__)
        if (!screenPeer.peek().empty()) platform::stopMediaAudio();
#endif
        platform::stopMediaAudio();
        removeDecoder("screen");
        screenWheel = {};
        screenWheelPending = false;
        screenPeer.set({});
        screenStatus.set({});
        screenFrame.set(kompoz::ImageBitmap());
    }

    bool callLogStale = false;  // asked again on the next refresh

    void requestCallLog() {
        callLogPeer = smsPhone();
        callLogOffset = 0;
        callLog.set({});
        callLogHasMore.set(false);
        callsError.set({});
        callLogLoading.set(!callLogPeer.empty());
        if (node && !callLogPeer.empty())
            airlink_send_message(node, callLogPeer.c_str(), "calls.log_request", "{\"offset\":0}");
    }

    void loadMoreCallLog() {
        if (!node || callLogPeer.empty() || callLogLoading.peek() || !callLogHasMore.peek()) return;
        callLogLoading.set(true);
        std::string body = "{\"offset\":" + std::to_string(callLogOffset) + "}";
        airlink_send_message(node, callLogPeer.c_str(), "calls.log_request", body.c_str());
    }

    void smsRefresh() {
        if (callLogStale && !callLog.peek().empty()) {
            callLogStale = false;
            requestCallLog();
        }
        if (auto open = smsOpen.peek()) smsAsk("sms.thread_request", "{\"thread_id\":" + std::to_string(open->threadId) + "}");
        smsAsk("sms.threads_request", "{}");
    }

    // A computer this phone used: kept (data dir) as the fling's target.
    void noteComputer(const std::string& id) {
        for (const Peer& p : peers.peek())
            if (p.id == id && p.platform != "android" && lastComputer != id) {
                lastComputer = id;
                if (auto cfg = platform::config(); cfg && !cfg->dataDir.empty()) std::ofstream(cfg->dataDir + "/fling-target") << id;
            }
    }

    // The fling's target: the last computer used if it is online, else the only online one.
    std::string flingTarget() const {
        std::vector<std::string> online;
        for (const Peer& p : peers.peek())
            if (p.paired && p.online && p.platform != "android") {
                if (p.id == lastComputer) return p.id;
                online.push_back(p.id);
            }
        return online.size() == 1 ? online.front() : std::string();
    }

    std::string peerNameOf(const std::string& id) const {
        for (const Peer& p : peers.peek())
            if (p.id == id) return p.name;
        return id.substr(0, 8);
    }

    void sendDesktop(const std::string& peerId, const char* kind, const char* body) {
        if (node) airlink_send_message(node, peerId.c_str(), kind, body);
    }

    void track(uint64_t op, OpKind kind) {
        if (op) ops[op] = kind;
    }
};

App App::get() {
    static App app = [] {
        auto impl = std::make_shared<Impl>();
        std::weak_ptr<Impl> weak = impl;
        platform::setShareHandler([weak](std::vector<std::string> paths) {
            if (auto s = weak.lock()) {
                std::lock_guard lock(s->pickedMutex);
                s->shared.insert(s->shared.end(), paths.begin(), paths.end());
            }
        });
        return App(impl);
    }();
    return app;
}

void App::pump() const {
    Impl& s = *impl_;
    if (auto at = platform::takePanelRequest()) {
        int serial = s.panelRequest.peek().serial + 1;
        s.panelRequest.set(PanelRequest{.serial = serial, .x = at->first, .y = at->second});
    }
    if (!s.node) {
        if (!s.startAttempted && platform::config()) s.start();
        return;
    }
    while (char* raw = airlink_poll_event(s.node)) s.handle(json::Value::parse(takeString(raw)));
    Drives::get().pump();

    std::vector<std::pair<std::string, std::vector<std::string>>> picked;
    std::vector<std::string> shared;
    {
        std::lock_guard lock(s.pickedMutex);
        picked.swap(s.picked);
        shared.swap(s.shared);
    }
    if (!shared.empty())
        s.share.update([&](std::vector<std::string>& list) { list.insert(list.end(), shared.begin(), shared.end()); });
    for (auto& [peer, paths] : picked)
        if (!paths.empty()) sendFiles(peer, paths);
    // Reachability changes without an event (a paired device's connection drops), and the user may
    // have granted notification access in Settings meanwhile: look now and then.
    if (++s.ticks >= s.recentErrorUntilTick && s.recentError.peek()) s.recentError.set(false);
    if (s.ticks % 40 == 0) {
        s.refreshPeers();
        s.notificationAccess.set(platform::notificationAccessGranted());
        s.batteryRestricted.set(platform::batteryRestricted());
        s.smsAccess.set(platform::smsAccessGranted());
        s.callAccess.set(platform::callAccessGranted());
        s.screenControl.set(platform::screenControlEnabled());
        s.flingMedia.set(platform::flingMediaAccess());
    }
    if (!s.callAudioPeer.empty()) {
        auto call = s.activeCall.peek();
        if (!call || call->peerId != s.callAudioPeer) {
            platform::stopCallAudio();
            s.callAudioPeer.clear();
        } else if (platform::callAudioPhase() == platform::CallAudioPhase::Error) {
            s.callAudioPeer.clear();
        } else if (platform::callAudioPhase() == platform::CallAudioPhase::WaitingForRoute &&
                   call->route != "bluetooth" && s.ticks >= s.nextBluetoothRouteTick) {
            sendTo(s.callAudioPeer, "call.route", "{\"route\":\"bluetooth\"}");
            s.nextBluetoothRouteTick = s.ticks + 40;
        }
    }
#if (defined(__linux__) || defined(_WIN32)) && !defined(__ANDROID__)
    if (!s.playerBluetoothPeer.empty() &&
        std::chrono::steady_clock::now() >= s.playerBluetoothExpires) {
        s.playerBluetoothPeer.clear();
    }
    const bool playerCanConnect = !s.playerBluetoothPeer.empty() && s.callAudioPeer.empty();
    platform::setPlayerBluetoothPhone(playerCanConnect ? s.playerBluetoothName : std::string());
    if (s.playerBluetoothPeer.empty()) s.playerBluetoothReportedStatus.clear();
    else {
        auto status = playerCanConnect ? platform::playerBluetoothStatus() : "Görüşme bitince medya devam edecek";
        if (status != s.playerBluetoothReportedStatus) {
            s.playerBluetoothReportedStatus = status;
            sendTo(s.playerBluetoothPeer, "player.audio.status", "{\"status\":" + json::quote(status) + "}");
        }
    }
#endif
    if (s.ticks % 10 == 0) s.callAudioMessage.set(platform::callAudioStatus());
    // The wheel turns since the last pump, one message per window.
    for (auto& [session, w] : s.wheels) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "{\"session\":%d,\"cmd\":\"scroll\",\"x\":%.4f,\"y\":%.4f,\"v\":%.2f,\"h\":%.2f}", session,
                      w.x, w.y, w.v, w.h);
        s.phoneAsk("apps.input", buf);
    }
    s.wheels.clear();
    // Accessibility gestures are short swipes. Group wheel ticks until the previous gesture
    // finishes, so rapid wheel events do not cancel each other on Android.
    if (s.screenWheelPending && s.ticks >= s.nextScreenWheelTick) {
        auto w = s.screenWheel;
        s.screenWheel = {};
        s.screenWheelPending = false;
        s.nextScreenWheelTick = s.ticks + 4;  // 200 ms at the 50 ms pump interval
        double dx = std::clamp(w.h * 0.12, -0.42, 0.42);
        double dy = std::clamp(w.v * 0.12, -0.42, 0.42);
        double cx = std::clamp(w.x, 0.05 + std::abs(dx) / 2, 0.95 - std::abs(dx) / 2);
        double cy = std::clamp(w.y, 0.05 + std::abs(dy) / 2, 0.95 - std::abs(dy) / 2);
        char buf[192];
        std::snprintf(buf, sizeof buf, "{\"type\":\"swipe\",\"x\":%.4f,\"y\":%.4f,\"x2\":%.4f,\"y2\":%.4f,\"ms\":130}",
                      cx + dx / 2, cy + dy / 2, cx - dx / 2, cy - dy / 2);
        screenInput(buf);
    }
    // Open phone apps' newest frames.
    if (!s.appWindows.peek().empty()) {
        std::string phone = s.smsPhone();
        for (const AppWindow& w : s.appWindows.peek()) {
            uint8_t* data = nullptr;
            size_t len = 0;
            if (phone.empty()) continue;
            std::string name = "app:" + std::to_string(w.session);
            if (airlink_media_take(s.node, phone.c_str(), name.c_str(), &data, &len)) {
                kompoz::ImageBitmap frame = s.decodeMedia(name, data, len);
                airlink_free_bytes(data, len);
                if (!frame.empty()) s.updateWindow(w.session, [&](AppWindow& x) { x.frame = frame; x.status = "live"; });
            }
        }
    }
    // The shared screen's newest frame.
    if (const std::string& from = s.screenPeer.peek(); !from.empty()) {
        uint8_t* data = nullptr;
        size_t len = 0;
        if (airlink_media_take(s.node, from.c_str(), "screen", &data, &len)) {
            kompoz::ImageBitmap frame = s.decodeMedia("screen", data, len);
            airlink_free_bytes(data, len);
            if (!frame.empty()) {
                s.screenFrame.set(frame);
                s.screenStatus.set("live");
            }
        }
    }
    auto playSharedAudio = [&](const std::string& from, const char* stream) {
        if (from.empty()) return;
        uint8_t* data = nullptr;
        size_t len = 0;
        if (airlink_media_take(s.node, from.c_str(), stream, &data, &len)) {
            if (data && len <= 19200 && len % 2 == 0)
                platform::playMediaAudio(std::vector<uint8_t>(data, data + len));
            airlink_free_bytes(data, len);
        }
    };
#if defined(__ANDROID__)
    if (s.desktopStatus.peek() == "live") playSharedAudio(s.desktopPeer.peek(), "desktop_audio");
#else
    if (s.screenStatus.peek() == "live") playSharedAudio(s.screenPeer.peek(), "screen_audio");
    if (!s.desktopAudioRecipient.empty()) {
        if (platform::desktopAudioCaptureFailed()) {
            setScreenShareAudio(false);
            s.notices.push_back("Bilgisayarın ses çıkışı yakalanamadı");
        } else if (auto pcm = platform::takeDesktopAudio(); !pcm.empty()) {
            sendMedia(s.desktopAudioRecipient, "desktop_audio", pcm);
        }
    }
#endif
    // A computer's screen (this phone is the viewer).
    if (const std::string& from = s.desktopPeer.peek(); !from.empty()) {
        uint8_t* data = nullptr;
        size_t len = 0;
        if (airlink_media_take(s.node, from.c_str(), "desktop", &data, &len)) {
            kompoz::ImageBitmap frame = s.decodeMedia("desktop", data, len);
            airlink_free_bytes(data, len);
            if (!frame.empty()) {
                s.desktopFrame.set(frame);
                s.desktopStatus.set("live");
            }
        }
    }
    if (s.smsRefreshAt >= 0 && s.ticks >= s.smsRefreshAt) {
        s.smsRefreshAt = -1;
        s.smsRefresh();
    }
}

bool App::ready() const { return impl_->ready.get(); }
bool App::sharingPaused() const { return impl_->sharingPaused.get(); }
bool App::recentError() const { return impl_->recentError.get(); }
std::string App::startError() const { return impl_->startError.get(); }
DeviceInfo App::me() const { return impl_->me.get(); }
std::vector<Peer> App::peers() const { return impl_->peers.get(); }

std::vector<Peer> App::pairedPeers() const {
    std::vector<Peer> out;
    for (const Peer& p : impl_->peers.get())
        if (p.paired) out.push_back(p);
    return out;
}

std::vector<Peer> App::nearbyPeers() const {
    std::vector<Peer> out;
    for (const Peer& p : impl_->peers.get())
        if (!p.paired && p.online) out.push_back(p);
    return out;
}

std::vector<Transfer> App::transfers() const { return impl_->transfers.get(); }

std::optional<Prompt> App::prompt() const {
    const auto& list = impl_->prompts.get();
    if (list.empty()) return std::nullopt;
    return list.front();
}

std::optional<OutgoingPairing> App::outgoing() const { return impl_->outgoing.get(); }
std::vector<std::string> App::share() const { return impl_->share.get(); }
std::vector<PhoneNotification> App::phoneNotifications() const { return impl_->phoneNotifications.get(); }
bool App::canShareNotifications() const { return platform::canShareNotifications(); }
bool App::notificationAccess() const { return impl_->notificationAccess.get(); }
bool App::batteryRestricted() const { return impl_->batteryRestricted.get(); }

void App::dismissPhoneNotification(const PhoneNotification& n) const {
    std::string body = "{\"key\":" + json::quote(n.key) + "}";
    airlink_send_message(impl_->node, n.peerId.c_str(), "notification.dismiss", body.c_str());
    impl_->phoneNotifications.update([&](std::vector<PhoneNotification>& list) {
        std::erase_if(list, [&](const PhoneNotification& o) { return o.peerId == n.peerId && o.key == n.key; });
    });
}

void App::replyPhoneNotification(const PhoneNotification& n, const std::string& text) const {
    if (text.empty()) return;
    std::string body = "{\"key\":" + json::quote(n.key) + ",\"text\":" + json::quote(text) + "}";
    impl_->track(airlink_send_message(impl_->node, n.peerId.c_str(), "notification.reply", body.c_str()), OpKind::SendReply);
}

void App::openNotificationAccessSettings() const { platform::openNotificationAccessSettings(); }

void App::broadcast(const std::string& kind, const std::string& bodyJson) const {
    if (!impl_->node) return;
    std::vector<std::string> targets;
    {
        std::lock_guard lock(impl_->pickedMutex);
        targets = impl_->onlinePaired;
    }
    for (const std::string& id : targets) airlink_send_message(impl_->node, id.c_str(), kind.c_str(), bodyJson.c_str());
    platform::log("broadcast " + kind + " to " + std::to_string(targets.size()) + " devices");
}

void App::sendTo(const std::string& peerId, const std::string& kind, const std::string& bodyJson) const {
    if (impl_->node) airlink_send_message(impl_->node, peerId.c_str(), kind.c_str(), bodyJson.c_str());
}

std::string App::smsPhone() const {
    for (const Peer& p : impl_->peers.get())
        if (p.paired && p.online && p.platform == "android") return p.id;
    return {};
}
std::vector<SmsThread> App::smsThreads() const { return impl_->smsThreads.get(); }
std::optional<SmsThread> App::smsOpenThread() const { return impl_->smsOpen.get(); }
std::vector<SmsMessage> App::smsMessages() const { return impl_->smsMessages.get(); }
std::string App::smsError() const { return impl_->smsError.get(); }
bool App::smsLoading() const { return impl_->smsLoading.get(); }
bool App::canShareSms() const { return platform::canShareSms(); }
bool App::smsAccess() const { return impl_->smsAccess.get(); }

void App::requestSmsThreads() const { impl_->smsAsk("sms.threads_request", "{}"); }

void App::openSmsThread(const SmsThread& thread) const {
    impl_->smsOpen.set(thread);
    impl_->smsMessages.set({});
    impl_->smsAsk("sms.thread_request", "{\"thread_id\":" + std::to_string(thread.threadId) + "}");
}

void App::closeSmsThread() const {
    impl_->smsOpen.set(std::nullopt);
    impl_->smsMessages.set({});
}

void App::sendSms(const std::string& address, const std::string& text) const {
    if (address.empty() || text.empty()) return;
    impl_->smsAsk("sms.send", "{\"address\":" + json::quote(address) + ",\"text\":" + json::quote(text) + "}");
    // Shown at once; the phone's own record replaces it when the conversation is asked again.
    impl_->smsMessages.update([&](std::vector<SmsMessage>& list) {
        list.push_back(SmsMessage{.id = -1, .body = text, .date = int64_t(std::time(nullptr)) * 1000, .outgoing = true});
    });
}

void App::requestSmsAccess() const { platform::requestSmsAccess(); }

std::vector<ContactMatch> App::contactResults() const { return impl_->contactResults.get(); }
std::vector<ContactMatch> App::phoneContacts() const { return impl_->phoneContacts.get(); }
bool App::phoneContactsLoading() const { return impl_->phoneContactsLoading.get(); }
bool App::phoneContactsHasMore() const { return impl_->phoneContactsHasMore.get(); }
std::string App::phoneContactsError() const { return impl_->phoneContactsError.get(); }

std::optional<IncomingCall> App::incomingCall() const { return impl_->incomingCall.get(); }
std::vector<CallEntry> App::callLog() const { return impl_->callLog.get(); }
bool App::callLogLoading() const { return impl_->callLogLoading.get(); }
bool App::callLogHasMore() const { return impl_->callLogHasMore.get(); }
std::string App::callsError() const { return impl_->callsError.get(); }
bool App::canShareCalls() const { return platform::canShareCalls(); }
bool App::callAccess() const { return impl_->callAccess.get(); }
void App::requestCallAccess() const { platform::requestCallAccess(); }
void App::requestCallLog() const { impl_->requestCallLog(); }
void App::loadMoreCallLog() const { impl_->loadMoreCallLog(); }

void App::requestContacts(const std::string& query) const {
    impl_->phoneContactsQuery = query;
    impl_->phoneContactsPeer = impl_->smsPhone();
    impl_->phoneContactsOffset = 0;
    impl_->phoneContacts.set({});
    impl_->phoneContactsHasMore.set(false);
    impl_->phoneContactsError.set({});
    impl_->phoneContactsLoading.set(!impl_->phoneContactsPeer.empty());
    if (!impl_->phoneContactsPeer.empty()) {
        std::string body = "{\"query\":" + json::quote(query) + ",\"offset\":0}";
        sendTo(impl_->phoneContactsPeer, "contacts.list", body);
    }
}

void App::loadMoreContacts() const {
    if (impl_->phoneContactsPeer.empty() || impl_->phoneContactsLoading.peek() || !impl_->phoneContactsHasMore.peek()) return;
    impl_->phoneContactsLoading.set(true);
    std::string body = "{\"query\":" + json::quote(impl_->phoneContactsQuery) +
                       ",\"offset\":" + std::to_string(impl_->phoneContactsOffset) + "}";
    sendTo(impl_->phoneContactsPeer, "contacts.list", body);
}

void App::answerCall() const {
    if (auto call = impl_->incomingCall.peek()) sendTo(call->peerId, "call.answer", "{}");
    impl_->incomingCall.set(std::nullopt);
}

void App::answerCallOnComputer() const {
    if (auto c = impl_->incomingCall.peek()) impl_->callAudioOnAnswer = c->peerId;
    answerCall();
}

void App::rejectCall() const {
    if (auto call = impl_->incomingCall.peek()) sendTo(call->peerId, "call.reject", "{}");
    impl_->incomingCall.set(std::nullopt);
}

void App::dismissCall() const { impl_->incomingCall.set(std::nullopt); }

std::optional<ActiveCall> App::activeCall() const { return impl_->activeCall.get(); }

std::string App::screenPeer() const { return impl_->screenPeer.get(); }
std::string App::screenStatus() const { return impl_->screenStatus.get(); }
kompoz::ImageBitmap App::screenFrame() const { return impl_->screenFrame.get(); }
bool App::canShareScreen() const { return platform::canShareScreen(); }
bool App::screenControl() const { return impl_->screenControl.get(); }
void App::openScreenControlSettings() const { platform::openScreenControlSettings(); }

std::string App::desktopPeer() const { return impl_->desktopPeer.get(); }
std::string App::desktopStatus() const { return impl_->desktopStatus.get(); }
kompoz::ImageBitmap App::desktopFrame() const { return impl_->desktopFrame.get(); }
std::optional<DesktopRequest> App::desktopRequest() const { return impl_->desktopRequest.get(); }
std::string App::desktopViewer() const { return impl_->desktopViewer.get(); }
bool App::canShareDesktop() const { return airlink_desktop_supported(); }

bool App::screenShareAudio() const { return impl_->screenShareAudio.get(); }

bool App::pauseScreenAudioDuringMusic() const {
    return impl_->pauseScreenAudioDuringMusic.get();
}

void App::setPauseScreenAudioDuringMusic(bool on) const {
    impl_->pauseScreenAudioDuringMusic.set(on);
    if (auto cfg = platform::config(); cfg && !cfg->dataDir.empty())
        std::ofstream(cfg->dataDir + "/pause-screen-audio-during-music") << on;
    platform::setPauseScreenAudioDuringMusic(on);
}

void App::screenShareAudioFailed() const {
    impl_->screenShareAudio.set(false);
    impl_->notices.push_back("Ekran paylaşımı için medya sesi alınamadı");
}

void App::setScreenShareAudio(bool on) const {
    impl_->screenShareAudio.set(on);
#if defined(__ANDROID__)
    platform::setScreenShareAudio(on);
#else
    impl_->updateDesktopAudio();
#endif
}

void App::startDesktop(const std::string& peerId) const {
    if (!impl_->node || peerId.empty()) return;
    impl_->closeDesktop();
    impl_->noteComputer(peerId);
    impl_->desktopPeer.set(peerId);
    impl_->desktopStatus.set("asking");
    // Sized for a phone: its screen is about this wide; fewer frames keep it smooth on Wi-Fi.
    sendTo(peerId, "desktop.start", "{\"max_width\":1600,\"fps\":15}");
}

void App::stopDesktop() const {
    std::string peer = impl_->desktopPeer.peek();
    if (!peer.empty()) sendTo(peer, "desktop.stop", "{}");
    impl_->closeDesktop();
}

void App::desktopInput(const std::string& bodyJson) const {
    std::string peer = impl_->desktopPeer.peek();
    if (!peer.empty() && !bodyJson.empty()) sendTo(peer, "desktop.input", bodyJson);
}

void App::answerDesktop(bool accept) const {
    auto request = impl_->desktopRequest.peek();
    impl_->desktopRequest.set(std::nullopt);
    if (!request || !impl_->node) return;
    if (!accept) return impl_->sendDesktop(request->peerId, "desktop.stopped", "{\"reason\":\"declined\"}");
    impl_->track(airlink_desktop_start(impl_->node, request->peerId.c_str(), request->options.c_str()), OpKind::DesktopShare);
}

void App::endDesktopShare() const {
    setScreenShareAudio(false);
    if (impl_->node) airlink_desktop_stop(impl_->node, nullptr, "user");
    impl_->desktopViewer.set({});
}

bool App::canFling() const { return platform::canFling(); }
bool App::flingEnabled() const { return impl_->flingOn.get(); }
bool App::flingMediaAccess() const { return impl_->flingMedia.get(); }
void App::requestFlingMediaAccess() const { platform::requestFlingMediaAccess(); }

std::string App::flingTarget() const {
    impl_->peers.get();  // subscribes: the target changes as devices come and go
    return impl_->flingTarget();
}

void App::setFlingEnabled(bool on) const {
    platform::setFlingEnabled(on);
    impl_->flingOn.set(on);
}

bool App::fling(const std::string& clipboardText) const {
    std::string target = impl_->flingTarget();
    if (target.empty()) {
        impl_->notices.push_back("Fırlatma için çevrimiçi bir bilgisayar yok");
        return false;
    }
    std::string name = impl_->peerNameOf(target);
    if (auto files = impl_->share.peek(); !files.empty()) {
        impl_->share.set({});
        sendFiles(target, files);
        impl_->notices.push_back(std::to_string(files.size()) + " dosya " + name + " cihazına fırlatıldı");
        return true;
    } else if (!clipboardText.empty()) {
        sendClipboard(target, clipboardText);
        return true;
    } else {
        impl_->notices.push_back("Gönderilecek bir şey yok: dosya paylaşın ya da metin kopyalayın");
        return false;
    }
}

std::string App::flingPendingShare() const {
    auto files = impl_->share.peek();
    if (files.empty()) return {};
    impl_->share.set({});
    return flingFiles(files);
}

std::string App::flingFiles(const std::vector<std::string>& paths) const {
    std::string target = impl_->flingTarget();
    if (target.empty()) return "Fırlatma için çevrimiçi bir bilgisayar yok";
    if (paths.empty()) return "Gönderilecek dosya okunamadı";
    sendFiles(target, paths);
    std::string message = std::to_string(paths.size()) + " dosya " + impl_->peerNameOf(target) + " cihazına fırlatıldı";
    impl_->notices.push_back(message);
    return message;
}

std::string App::flingText(const std::string& text) const {
    std::string target = impl_->flingTarget();
    if (target.empty()) return "Fırlatma için çevrimiçi bir bilgisayar yok";
    if (text.empty()) return {};
    sendClipboard(target, text);
    return "Metin " + impl_->peerNameOf(target) + " cihazının panosuna gönderildi";
}

bool App::canUseAdvancedMode() const { return platform::canUseAdvancedMode(); }
bool App::advancedReady() const { return impl_->advancedReady.get(); }
std::string App::advancedStatus() const { return impl_->advancedStatus.get(); }
std::string App::advancedPort() const { return impl_->advancedPort.get(); }

void App::setAdvancedPort(const std::string& port) const {
    impl_->advancedPort.set(port);
    impl_->advancedReady.set(false);
    auto cfg = platform::config();
    if (!cfg || cfg->dataDir.empty()) return;
    std::filesystem::path path = std::filesystem::path(cfg->dataDir) / "adb" / "connect-port";
    if (port.empty()) {
        std::error_code error;
        std::filesystem::remove(path, error);
    } else if (parseAdbPort(port)) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!error) std::ofstream(path) << port;
    }
}

void App::startAdvancedSetup() const {
    impl_->advancedStatus.set("Bildirimdeki alana eşleme kodunu yazın…");
    platform::startAdvancedSetup();
}

void App::advancedPair(const std::string& code) const {
    if (!impl_->node) return;
    size_t colon = code.find(':');
    std::string pairingCode = code.substr(0, colon);
    std::string portText = colon == std::string::npos ? std::string() : code.substr(colon + 1);
    auto port = parseAdbPort(portText);
    if (!port || (colon != std::string::npos && *port == 0)) {
        impl_->advancedStatus.set("Eşleme portu 1–65535 arasında olmalı");
        return platform::advancedSetupResult(false, "Eşleme portu geçersiz");
    }
    impl_->advancedStatus.set("Eşleniyor…");
    impl_->track(airlink_adb_pair_port(impl_->node, impl_->adbDir().c_str(), pairingCode.c_str(), *port), OpKind::AdbPair);
}

void App::startHelper(const std::string& command) const {
    if (!impl_->node) return platform::helperStarted(false, "not started");
    auto port = parseAdbPort(impl_->advancedPort.peek());
    if (!port) return platform::helperStarted(false, "Kablosuz hata ayıklama portu geçersiz");
    impl_->track(airlink_adb_shell_port(impl_->node, impl_->adbDir().c_str(), command.c_str(), *port), OpKind::AdbHelper);
}

std::vector<PhoneApp> App::phoneApps() const { return impl_->phoneApps.get(); }
bool App::phoneAppsLoading() const { return impl_->phoneAppsLoading.get(); }
std::vector<AppWindow> App::appWindows() const { return impl_->appWindows.get(); }

void App::requestPhoneApps() const {
    if (impl_->smsPhone().empty()) return;
    impl_->phoneAppsLoading.set(true);
    impl_->phoneAsk("apps.list", "{}");
}

void App::openPhoneApp(const PhoneApp& app) const {
    for (const AppWindow& w : impl_->appWindows.peek())
        if (w.component == app.component) return showAppWindow(w.session);
    int session = impl_->nextSession++;
    impl_->appWindows.update([&](std::vector<AppWindow>& list) {
        list.push_back(AppWindow{.session = session, .label = app.label, .component = app.component, .status = "starting"});
    });
    impl_->phoneAsk("apps.open", "{\"session\":" + std::to_string(session) + ",\"component\":" + json::quote(app.component) +
                                     ",\"width\":720,\"height\":1560,\"dpi\":320,\"fps\":24,\"quality\":70,\"codec\":\"h264\"}");
}

void App::closeAppWindow(int session) const {
    impl_->phoneAsk("apps.close", "{\"session\":" + std::to_string(session) + "}");
    impl_->removeWindow(session);
}

void App::showAppWindow(int session) const {
    impl_->updateWindow(session, [](AppWindow& w) { ++w.frontRequests; });
}

void App::appWheel(int session, double x, double y, double h, double v) const {
    Impl::Wheel& w = impl_->wheels[session];
    w.x = x;
    w.y = y;
    w.h += h;
    w.v += v;
}

void App::appInput(int session, const std::string& bodyJson) const {
    if (bodyJson.empty()) return;
    // {"cmd":...} + the session
    std::string body = "{\"session\":" + std::to_string(session) + "," + bodyJson.substr(1);
    impl_->phoneAsk("apps.input", body);
}

void App::advancedTest() const {
    if (!impl_->node) return;
    auto port = parseAdbPort(impl_->advancedPort.peek());
    if (!port) return impl_->advancedStatus.set("Kablosuz hata ayıklama portu 1–65535 arasında olmalı");
    impl_->advancedStatus.set("Kablosuz hata ayıklamaya bağlanılıyor…");
    impl_->track(airlink_adb_shell_port(impl_->node, impl_->adbDir().c_str(), "id", *port), OpKind::AdbTest);
}

void App::startScreen(const std::string& peerId) const {
    if (!impl_->node) return;
    impl_->screenStopPeer.clear();
    impl_->screenPeer.set(peerId);
    impl_->screenStatus.set("asking");
    impl_->screenFrame.set(kompoz::ImageBitmap());
    const std::string body = R"({"quality":60,"codec":"h264"})";
    impl_->track(airlink_send_message(impl_->node, peerId.c_str(), "screen.start", body.c_str()),
                 OpKind::ScreenStart);
}

bool App::audioAutoConnect() const { return impl_->phoneAudioAutoConnect.get(); }

void App::setAudioAutoConnect(bool on) const {
#if defined(__ANDROID__)
    auto& s = *impl_;
    s.phoneAudioAutoConnect.set(on);
    if (auto cfg = platform::config(); cfg && !cfg->dataDir.empty())
        std::ofstream(cfg->dataDir + "/audio-auto-connect") << on;
    if (s.phoneCallAudioEnabled.peek() && !s.phoneCallAudioPeer.empty())
        sendTo(s.phoneCallAudioPeer, "call.audio_share", on ? R"({"on":true})" : R"({"on":false})");
#else
    (void)on;
#endif
}

void App::shareCurrentCallAudio() const {
#if defined(__ANDROID__)
    if (impl_->phoneCallAudioEnabled.peek() && !impl_->phoneCallAudioPeer.empty())
        sendTo(impl_->phoneCallAudioPeer, "call.audio_share", R"({"on":true,"once":true})");
#endif
}

void App::setPlayerAudioStatus(const std::string& status) const {
    if (impl_->mediaAudioEnabled.peek()) impl_->mediaAudioMessage.set(status);
}

bool App::mediaAudioEnabled() const { return impl_->mediaAudioEnabled.get(); }
std::string App::mediaAudioStatus() const { return impl_->mediaAudioMessage.get(); }
std::string App::mediaAudioTarget() const { return impl_->mediaAudioTarget.get(); }
void App::setMediaAudioTarget(const std::string& peerId) const {
    if (impl_->mediaAudioEnabled.peek()) setMediaAudioEnabled(false);
#if defined(__ANDROID__)
    if (impl_->phoneCallAudioEnabled.peek()) stopCallAudio();
#endif
    impl_->mediaAudioTarget.set(peerId);
}
void App::chooseMediaAudioPlayers() const { platform::chooseMediaAudioPlayers(); }
bool App::canPlayMediaAudio() const { return impl_->mediaAudioSupported; }

void App::setMediaAudioEnabled(bool on) const {
#if defined(__ANDROID__)
    const std::string peer = on ? (impl_->mediaAudioTarget.peek().empty() ? impl_->flingTarget() : impl_->mediaAudioTarget.peek())
                                : impl_->mediaAudioPeer;
    if (on) {
        const auto& peers = impl_->peers.peek();
        if (std::none_of(peers.begin(), peers.end(), [&](const Peer& p) {
                return p.id == peer && p.paired && p.online && (p.platform == "linux" || p.platform == "windows");
            })) {
            impl_->notices.push_back("Bluetooth oynatıcı sesi için çevrimiçi Linux veya Windows bilgisayarı seçin");
            return;
        }
    }
    impl_->mediaAudioEnabled.set(on);
    impl_->mediaAudioPeer = on ? peer : std::string();
    impl_->mediaAudioMessage.set(on ? "Seçili oynatıcıların oynatması bekleniyor" : "");
    platform::setPlayerAudioTarget(impl_->mediaAudioPeer);
#else
    (void)on;
#endif
}

void App::stopScreen() const {
    std::string peer = impl_->screenPeer.peek();
    if (!peer.empty()) {
        impl_->screenStopPeer = peer;
        sendTo(peer, "screen.stop", "{}");
    }
    impl_->closeScreen();
}

void App::screenInput(const std::string& bodyJson) const {
    platform::log("screen input: " + (bodyJson.empty() ? std::string("(outside the image)") : bodyJson));
    std::string peer = impl_->screenPeer.peek();
    if (!peer.empty() && !bodyJson.empty()) sendTo(peer, "screen.input", bodyJson);
}

void App::screenWheel(double x, double y, double h, double v) const {
    if (impl_->screenPeer.peek().empty()) return;
    auto& w = impl_->screenWheel;
    w.x = x;
    w.y = y;
    w.h += h;
    w.v += v;
    impl_->screenWheelPending = true;
}

void App::sendMedia(const std::string& peerId, const std::string& name, const std::vector<uint8_t>& frame) const {
    if (impl_->node) airlink_media_send(impl_->node, peerId.c_str(), name.c_str(), frame.data(), frame.size());
}

void App::stopMedia(const std::string& peerId, const std::string& name) const {
    if (impl_->node) airlink_media_stop(impl_->node, peerId.c_str(), name.c_str());
}

void App::callControl(const std::string& kind, const std::string& bodyJson) const {
    if (auto call = impl_->activeCall.peek()) sendTo(call->peerId, kind, bodyJson);
}

bool App::canBridgeCallAudio() const { return impl_->callAudioSupported; }
bool App::callAudioOn() const {
#if defined(__ANDROID__)
    return impl_->phoneCallAudioEnabled.get();
#else
    return !impl_->callAudioPeer.empty();
#endif
}
std::string App::callAudioStatus() const { return impl_->callAudioMessage.get(); }

void App::startCallAudio() const {
#if defined(__ANDROID__)
    const std::string peer = impl_->mediaAudioTarget.peek().empty() ? impl_->flingTarget() : impl_->mediaAudioTarget.peek();
    if (peer.empty()) return;
    impl_->phoneCallAudioEnabled.set(true);
    impl_->phoneCallAudioPeer = peer;
    if (impl_->phoneAudioAutoConnect.peek())
        sendTo(peer, "call.audio_share", R"({"on":true})");
    return;
#endif
    auto call = impl_->activeCall.peek();
    if (!impl_->callAudioSupported || !call) return;
    impl_->callAudioPeer = call->peerId;
    impl_->nextBluetoothRouteTick = impl_->ticks;
    platform::startCallAudio(impl_->peerNameOf(call->peerId));
    impl_->callAudioMessage.set("Bluetooth telefona bağlanıyor");
}

void App::stopCallAudio() const {
#if defined(__ANDROID__)
    impl_->phoneCallAudioEnabled.set(false);
    const std::string peer = std::exchange(impl_->phoneCallAudioPeer, {});
    if (!peer.empty()) sendTo(peer, "call.audio_share", "{\"on\":false}");
    return;
#endif
    platform::stopCallAudio();
    impl_->callAudioPeer.clear();
    if (auto call = impl_->activeCall.peek()) {
        const bool earpiece = std::find(call->routes.begin(), call->routes.end(), "earpiece") != call->routes.end();
        callControl("call.route", earpiece ? "{\"route\":\"earpiece\"}" : "{\"route\":\"speaker\"}");
    }
}

void App::dial(const std::string& number) const {
    if (number.empty()) return;
    impl_->phoneAsk("call.dial", "{\"number\":" + json::quote(number) + "}");
}

void App::searchContacts(const std::string& query) const {
    impl_->contactQuery = query;
    if (query.empty()) return impl_->contactResults.set({});
    std::string phone = impl_->smsPhone();
    if (!impl_->node || phone.empty()) return;
    std::string body = "{\"query\":" + json::quote(query) + "}";
    airlink_send_message(impl_->node, phone.c_str(), "contacts.search", body.c_str());
}

void App::shareTo(const std::string& peerId) const {
    auto paths = impl_->share.peek();
    impl_->share.set({});
    if (!paths.empty()) sendFiles(peerId, paths);
}

void App::cancelShare() const { impl_->share.set({}); }

std::string App::peerName(const std::string& id) const {
    for (const Peer& p : impl_->peers.get())
        if (p.id == id) return p.name;
    return id.substr(0, 8);
}

std::vector<std::string> App::takeNotices() const { return std::exchange(impl_->notices, {}); }
std::vector<std::string> App::takeIncomingClipboard() const {
    auto list = std::exchange(impl_->incomingClipboard, {});
    if (!list.empty()) impl_->lastClipboard = list.back();  // set here: not to be sent back
    return list;
}

void App::syncClipboard(const std::string& text, bool sensitive) const {
    auto& s = *impl_;
    if (!s.node || text.empty() || text == s.lastClipboard) return;
    s.lastClipboard = text;
    s.lastClipboardSensitive = sensitive;
    // The first reading is what was there already, not a copy: not sent. A password still asks: on
    // Android the clipboard is read only with the app in front, so a fresh copy can be the first.
    bool first = !std::exchange(s.clipboardSeen, true);
    if (first && !(sensitive && s.passwordPrivacy.peek())) return;
    if (sensitive && s.passwordPrivacy.peek()) {
        // A password: to the trusted devices only after a yes.
        bool anyTrusted = std::any_of(s.peers.peek().begin(), s.peers.peek().end(),
                                      [](const Peer& p) { return p.paired && p.online && p.autoAccept; });
        if (!anyTrusted) return;
        s.pendingPassword = text;
        s.passwordPending.set(true);
        platform::askSendPassword([](bool send) { App::get().answerPasswordSend(send); });
        return;
    }
    s.sendToTrusted(text, sensitive);
}

bool App::passwordPrivacy() const { return impl_->passwordPrivacy.get(); }
void App::setPasswordPrivacy(bool on) const {
    impl_->passwordPrivacy.set(on);
    if (auto path = impl_->settingPath("password-privacy"); !path.empty()) std::ofstream(path) << (on ? 1 : 0);
}
bool App::passwordPending() const { return impl_->passwordPending.get(); }
void App::answerPasswordSend(bool send) const {
    auto& s = *impl_;
    if (!s.passwordPending.peek()) return;  // (answered already: the dialog or the notification)
    s.passwordPending.set(false);
    platform::passwordAnswered();
    std::string text = std::exchange(s.pendingPassword, {});
    if (send && !text.empty()) s.sendToTrusted(text, true);
}
void App::dropPasswords() const {
    impl_->clipboardHistory.update([](std::vector<ClipboardItem>& list) {
        std::erase_if(list, [](const ClipboardItem& item) { return item.sensitive; });
    });
}
void App::copiedClipboardItem(int64_t id) const {
    impl_->clipboardHistory.update([&](std::vector<ClipboardItem>& list) {
        std::erase_if(list, [&](const ClipboardItem& item) { return item.id == id && item.sensitive; });
    });
}

void App::setTrusted(const std::string& peerId, bool on) const {
    if (!impl_->node) return;
    if (airlink_set_auto_accept(impl_->node, peerId.c_str(), on)) impl_->refreshPeers();
}

void App::pair(const std::string& peerId) const {
    impl_->track(airlink_start_pairing(impl_->node, peerId.c_str()), OpKind::StartPairing);
}

void App::pairAddress(const std::string& address) const {
    if (impl_->node && !address.empty())
        impl_->track(airlink_start_pairing_address(impl_->node, address.c_str()), OpKind::StartPairing);
}

void App::confirmOutgoing(bool accept) const {
    auto current = impl_->outgoing.peek();
    if (!current) return;
    impl_->track(airlink_confirm_pairing(impl_->node, current->pairing, accept), OpKind::ConfirmPairing);
    if (accept) {
        current->waitingForPeer = true;
        impl_->outgoing.set(current);
    } else {
        impl_->outgoing.set(std::nullopt);
    }
}

void App::notice(const std::string& text) const { impl_->notices.push_back(text); }

void App::respond(uint64_t requestId, bool accept) const {
    airlink_respond(impl_->node, requestId, accept);
    impl_->removePrompt(requestId);
}

void App::pickAndSend(const std::string& peerId) const {
    std::weak_ptr<Impl> weak = impl_;
    platform::pickFiles([weak, peerId](std::vector<std::string> paths) {
        if (auto s = weak.lock()) {
            std::lock_guard lock(s->pickedMutex);
            s->picked.emplace_back(peerId, std::move(paths));
        }
    });
}

void App::sendDropped(const std::string& peerId, const std::vector<std::string>& paths,
                      const std::vector<std::string>& uris) const {
    if (!paths.empty()) return sendFiles(peerId, paths);
    if (uris.empty()) {
        impl_->notices.push_back("Bırakılan öğede dosya yok");
        return;
    }
    std::weak_ptr<Impl> weak = impl_;
    platform::stageUris(uris, [weak, peerId](std::vector<std::string> staged) {
        if (auto s = weak.lock()) {
            std::lock_guard lock(s->pickedMutex);
            s->picked.emplace_back(peerId, std::move(staged));
        }
    });
}

void App::sendFiles(const std::string& peerId, const std::vector<std::string>& paths) const {
    impl_->noteComputer(peerId);
    std::string list = "[";
    for (const auto& p : paths) list += (list.size() > 1 ? "," : "") + json::quote(p);
    list += "]";
    impl_->track(airlink_send_files(impl_->node, peerId.c_str(), list.c_str()), OpKind::SendFiles);
}

void App::sendClipboard(const std::string& peerId, const std::string& text) const {
    impl_->noteComputer(peerId);
    if (text.empty()) {
        impl_->notices.push_back("Pano boş");
        return;
    }
    // Sent by hand: a password goes, marked so.
    bool secret = text == impl_->lastClipboard && impl_->lastClipboardSensitive;
    impl_->track(airlink_send_clipboard_marked(impl_->node, peerId.c_str(), text.c_str(), secret), OpKind::SendClipboard);
    impl_->rememberClipboard(peerId, text, false, secret);
}

std::vector<FileRecord> App::fileHistory() const {
    impl_->filesOnDisk.get();
    return impl_->fileHistory.get();
}
void App::forgetFileRecord(int64_t id) const {
    impl_->fileHistory.update([&](std::vector<FileRecord>& list) {
        std::erase_if(list, [&](const FileRecord& f) { return f.id == id; });
    });
    impl_->saveFileHistory();
}
bool App::deleteFile(int64_t id) const {
    for (const FileRecord& f : impl_->fileHistory.peek()) {
        if (f.id != id) continue;
        std::error_code ec;
        auto path = std::filesystem::path(std::u8string(f.path.begin(), f.path.end()));
        bool gone = !std::filesystem::exists(path, ec) || std::filesystem::remove_all(path, ec) > 0;
        if (!gone) {
            impl_->notices.push_back("Dosya silinemedi: " + ec.message());
            return false;
        }
        impl_->filesOnDisk.set(impl_->filesOnDisk.peek() + 1);  // (its card redrawn as deleted)
        return true;
    }
    return false;
}
void App::clearFileHistory() const {
    impl_->fileHistory.set({});
    impl_->saveFileHistory();
}
PhoneMedia App::phoneMedia() const { return impl_->phoneMedia.get(); }

PhoneBattery App::phoneBattery(const std::string& peerId) const {
    for (const PhoneBattery& b : impl_->phoneBatteries.get())
        if (b.peerId == peerId) return b;
    return PhoneBattery{.peerId = peerId};
}

void App::findPhone(const std::string& peerId, bool ring) const {
    if (impl_->node) airlink_send_message(impl_->node, peerId.c_str(), ring ? "phone.find" : "phone.find_stop", "{}");
}
int App::receivedCount() const { return impl_->receivedCount.get(); }
App::PanelRequest App::panelRequest() const { return impl_->panelRequest.get(); }
void App::requestPhoneMedia() const { impl_->phoneAsk("media.state_request", "{}"); }
void App::phoneMediaControl(const std::string& action) const {
    impl_->phoneAsk("media.control", "{\"action\":" + json::quote(action) + "}");
}
std::vector<ClipboardItem> App::clipboardHistory() const { return impl_->clipboardHistory.get(); }
void App::removeClipboardItem(int64_t id) const {
    impl_->clipboardHistory.update([&](std::vector<ClipboardItem>& list) {
        std::erase_if(list, [&](const ClipboardItem& item) { return item.id == id; });
    });
}
void App::clearClipboardHistory() const { impl_->clipboardHistory.set({}); }

void App::forget(const std::string& peerId) const {
    airlink_forget(impl_->node, peerId.c_str());
    impl_->refreshPeers();
}

void App::clearFinishedTransfers() const {
    impl_->transfers.update([](std::vector<Transfer>& list) { std::erase_if(list, [](const Transfer& t) { return !t.active(); }); });
}

void App::setSharingPaused(bool paused) const {
    Impl& s = *impl_;
    if (s.sharingPaused.peek() == paused && (paused || s.node)) return;
    if (paused) {
        // An in-flight operation no longer has a running node to report its result.
        if (!s.mediaAudioPeer.empty()) platform::setPlayerAudioTarget({});
        s.playerBluetoothPeer.clear();
        s.playerBluetoothReportedStatus.clear();
        platform::setPlayerBluetoothPhone({});
        s.mediaAudioPeer.clear();
        s.mediaAudioEnabled.set(false);
        s.mediaAudioMessage.set({});
        platform::stopMediaAudio();
        setScreenShareAudio(false);
        platform::stopCallAudio();
        s.callAudioPeer.clear();
        s.callAudioAutoPeer.clear();
        s.callAudioAutoAttempted = false;
        s.phoneCallAudioPeer.clear();
        s.phoneCallAudioEnabled.set(false);
        s.ready.set(false);
        s.sharingPaused.set(true);
        s.transfers.update([](std::vector<Transfer>& list) {
            for (Transfer& t : list)
                if (t.active()) t.error = "Paylaşım duraklatıldı";
        });
        s.prompts.set({});
        s.outgoing.set(std::nullopt);
        s.ops.clear();
        s.closeScreen();
        s.closeDesktop();
        s.desktopViewer.set({});
        s.appWindows.set({});
        s.phoneApps.set({});
        s.phoneAppsLoading.set(false);
        s.incomingCall.set(std::nullopt);
        s.activeCall.set(std::nullopt);
        s.smsLoading.set(false);
        s.phoneContactsLoading.set(false);
        s.callLogLoading.set(false);
        s.peers.update([](std::vector<Peer>& list) {
            for (Peer& p : list) p.online = false;
        });
        {
            std::lock_guard lock(s.pickedMutex);
            s.onlinePaired.clear();
        }
        AirlinkNode* node = std::exchange(s.node, nullptr);
        if (node) airlink_stop(node);
    } else {
        s.sharingPaused.set(false);
        s.start();
    }
}

std::string humanSize(int64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int i = 0;
    while (v >= 1024 && i < 4) v /= 1024, ++i;
    char buf[32];
    if (i == 0) std::snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[i]);
    return buf;
}

}  // namespace airlink
