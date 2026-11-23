#include "app_state.hpp"

#include <airlink.h>

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

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

enum class OpKind { StartPairing, ConfirmPairing, SendFiles, SendClipboard, SendReply, AdbPair, AdbTest, AdbHelper };

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
    MutableState<std::string> startError = mutableStateOf(std::string());
    MutableState<DeviceInfo> me = mutableStateOf(DeviceInfo{});
    MutableState<std::vector<Peer>> peers = mutableStateOf(std::vector<Peer>{});
    MutableState<std::vector<Transfer>> transfers = mutableStateOf(std::vector<Transfer>{});
    MutableState<std::vector<Prompt>> prompts = mutableStateOf(std::vector<Prompt>{});
    MutableState<std::optional<OutgoingPairing>> outgoing = mutableStateOf(std::optional<OutgoingPairing>{});
    MutableState<std::vector<std::string>> share = mutableStateOf(std::vector<std::string>{});
    MutableState<std::vector<PhoneNotification>> phoneNotifications = mutableStateOf(std::vector<PhoneNotification>{});
    MutableState<bool> notificationAccess = mutableStateOf(false);
    MutableState<std::vector<SmsThread>> smsThreads = mutableStateOf(std::vector<SmsThread>{});
    MutableState<std::optional<SmsThread>> smsOpen = mutableStateOf(std::optional<SmsThread>{});
    MutableState<std::vector<SmsMessage>> smsMessages = mutableStateOf(std::vector<SmsMessage>{});
    MutableState<std::string> smsError = mutableStateOf(std::string());
    MutableState<bool> smsLoading = mutableStateOf(false);
    MutableState<bool> smsAccess = mutableStateOf(false);
    MutableState<std::vector<ContactMatch>> contactResults = mutableStateOf(std::vector<ContactMatch>{});
    std::string contactQuery;  // answers to older searches are dropped
    MutableState<std::optional<IncomingCall>> incomingCall = mutableStateOf(std::optional<IncomingCall>{});
    MutableState<std::vector<CallEntry>> callLog = mutableStateOf(std::vector<CallEntry>{});
    MutableState<std::string> callsError = mutableStateOf(std::string());
    MutableState<bool> callAccess = mutableStateOf(false);
    MutableState<std::optional<ActiveCall>> activeCall = mutableStateOf(std::optional<ActiveCall>{});
    MutableState<std::string> screenPeer = mutableStateOf(std::string());
    MutableState<std::string> screenStatus = mutableStateOf(std::string());
    MutableState<kompoz::ImageBitmap> screenFrame = mutableStateOf(kompoz::ImageBitmap());
    MutableState<bool> screenControl = mutableStateOf(false);
    MutableState<bool> advancedReady = mutableStateOf(false);
    MutableState<std::string> advancedStatus = mutableStateOf(std::string());
    MutableState<std::vector<PhoneApp>> phoneApps = mutableStateOf(std::vector<PhoneApp>{});
    MutableState<bool> phoneAppsLoading = mutableStateOf(false);
    MutableState<std::vector<AppWindow>> appWindows = mutableStateOf(std::vector<AppWindow>{});
    MutableState<int> activeWindow = mutableStateOf(0);
    int nextSession = 1;

    void updateWindow(int session, const std::function<void(AppWindow&)>& change) {
        appWindows.update([&](std::vector<AppWindow>& list) {
            for (AppWindow& w : list)
                if (w.session == session) change(w);
        });
    }

    void removeWindow(int session) {
        appWindows.update([&](std::vector<AppWindow>& list) { std::erase_if(list, [&](const AppWindow& w) { return w.session == session; }); });
        if (activeWindow.peek() == session) activeWindow.set(0);
    }

    std::string adbDir() const {
        auto cfg = platform::config();
        return (cfg && !cfg->dataDir.empty() ? cfg->dataDir : std::string(".")) + "/adb";
    }
    int smsRefreshAt = -1;  // a tick to ask the open conversation again (a sent SMS is recorded late)

    std::vector<std::string> notices;
    std::vector<std::string> incomingClipboard;
    std::map<uint64_t, OpKind> ops;
    int ticks = 0;

    // Filled by the platform's file picker on any thread, applied in pump().
    std::mutex pickedMutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> picked;
    std::vector<std::string> shared;  // from platform::setShareHandler, any thread
    std::vector<std::string> onlinePaired;  // for sends from any thread (under pickedMutex)

    ~Impl() {
        if (node) airlink_stop(node);
    }

    void start() {
        startAttempted = true;
        auto cfg = *platform::config();
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
            startError.set(takeString(err));
            return;
        }
        auto info = json::Value::parse(takeString(airlink_info(node)));
        me.set(DeviceInfo{.id = info["id"].str(),
                          .name = info["name"].str(),
                          .platform = info["platform"].str(),
                          .port = int(info["port"].i64()),
                          .downloadDir = info["download_dir"].str()});
        refreshPeers();
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
                                .online = p["online"].boolean()});
        {
            std::lock_guard lock(pickedMutex);
            onlinePaired.clear();
            for (const Peer& p : list)
                if (p.paired && p.online) onlinePaired.push_back(p.id);
        }
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

    void removePrompt(uint64_t requestId) {
        prompts.update([&](std::vector<Prompt>& list) {
            std::erase_if(list, [&](const Prompt& p) { return p.requestId == requestId; });
        });
    }

    void handle(const json::Value& e) {
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
            if (incoming) platform::filesReceived(files);
        } else if (kind == "transfer_failed") {
            updateTransfer(e["transfer_id"].str(), e["peer_id"].str(), e["direction"].str() == "incoming", [&](Transfer& t) {
                t.waiting = false;
                t.error = cleanError(e["error"].str());
            });
        } else if (kind == "media_started") {
            if (e["name"].str() == "screen" && e["peer_id"].str() == screenPeer.peek()) screenStatus.set("live");
        } else if (kind == "media_stopped") {
            if (e["name"].str() == "screen" && e["peer_id"].str() == screenPeer.peek()) closeScreen();
        } else if (kind == "app_message") {
            handleAppMessage(e["peer"], e["kind"].str(), e["body"]);
        } else if (kind == "clipboard_received") {
            incomingClipboard.push_back(e["text"].str());
            notices.push_back(e["peer"]["name"].str() + " panosunu paylaştı");
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
                case OpKind::AdbPair:
                    if (ok) {
                        advancedStatus.set("Eşleşildi, kabuk yetkisi deneniyor…");
                        track(airlink_adb_shell(node, adbDir().c_str(), "id"), OpKind::AdbTest);
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
                case OpKind::SendReply:
                    notices.push_back(ok ? "Yanıt gönderildi" : "Yanıt gönderilemedi: " + err);
                    break;
            }
        }
    }

    // What a computer asks of this phone (answered by the platform: SmsBridge, CallBridge).
    static bool isPhoneRequest(const std::string& kind) {
        static const char* const kinds[] = {"sms.threads_request", "sms.thread_request", "sms.send", "contacts.search",
                                            "calls.log_request", "call.answer", "call.reject", "call.dial", "call.hangup",
                                            "call.hold", "call.mute", "call.route", "call.volume", "call.status_request",
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
            // In parts: the first replaces the list, the last ends the loading.
            std::vector<PhoneApp> list = body["first"].boolean() ? std::vector<PhoneApp>{} : phoneApps.peek();
            for (const auto& a : body["apps"].array())
                list.push_back(PhoneApp{.label = a["label"].str(), .component = a["component"].str(),
                                        .icon = kompoz::decodeToImageBitmap(base64(a["icon"].str()))});
            phoneApps.set(std::move(list));
            if (body["done"].boolean()) phoneAppsLoading.set(false);
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
        } else if (kind == "screen.stopped") {
            if (peerId != screenPeer.peek()) return;
            std::string why = body["reason"].str();
            if (why == "declined") notices.push_back("Ekran paylaşımı telefonda reddedildi");
            else if (why == "user") notices.push_back("Ekran paylaşımı telefonda durduruldu");
            closeScreen();
        } else if (kind == "call.ringing") {
            IncomingCall call{.peerId = peerId, .number = body["number"].str(), .name = body["name"].str()};
            incomingCall.set(call);
            platform::showDesktopNotification("📞 " + call.title() + " arıyor", peer["name"].str());
        } else if (kind == "call.status") {
            std::string state = body["state"].str();
            if (state == "ended" || state == "ringing" || state.empty()) {
                if (state == "ended") activeCall.set(std::nullopt);
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
        } else if (kind == "call.state") {
            incomingCall.set(std::nullopt);  // answered or hung up
            if (body["state"].str() == "idle") activeCall.set(std::nullopt);
            if (body["state"].str() == "idle") {  // the log has a new entry
                callLogStale = true;
                smsRefreshAt = ticks + 20;
            }
        } else if (kind == "calls.log") {
            std::vector<CallEntry> list;
            for (const auto& c : body["calls"].array())
                list.push_back(CallEntry{.number = c["number"].str(), .name = c["name"].str(), .type = c["type"].str(),
                                         .date = c["date"].i64(), .duration = c["duration"].i64()});
            callLog.set(std::move(list));
            callsError.set({});
        } else if (kind == "calls.error") {
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

    void closeScreen() {
        screenPeer.set({});
        screenStatus.set({});
        screenFrame.set(kompoz::ImageBitmap());
    }

    bool callLogStale = false;  // asked again on the next refresh

    void smsRefresh() {
        if (callLogStale && !callLog.peek().empty()) {
            callLogStale = false;
            phoneAsk("calls.log_request", "{}");
        }
        if (auto open = smsOpen.peek()) smsAsk("sms.thread_request", "{\"thread_id\":" + std::to_string(open->threadId) + "}");
        smsAsk("sms.threads_request", "{}");
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
    if (!s.node) {
        if (!s.startAttempted && platform::config()) s.start();
        return;
    }
    while (char* raw = airlink_poll_event(s.node)) s.handle(json::Value::parse(takeString(raw)));

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
    if (++s.ticks % 40 == 0) {
        s.refreshPeers();
        s.notificationAccess.set(platform::notificationAccessGranted());
        s.smsAccess.set(platform::smsAccessGranted());
        s.callAccess.set(platform::callAccessGranted());
        s.screenControl.set(platform::screenControlEnabled());
    }
    // Open phone apps' newest frames.
    if (!s.appWindows.peek().empty()) {
        std::string phone = s.smsPhone();
        for (const AppWindow& w : s.appWindows.peek()) {
            uint8_t* data = nullptr;
            size_t len = 0;
            if (phone.empty() || w.minimized) continue;
            std::string name = "app:" + std::to_string(w.session);
            if (airlink_media_take(s.node, phone.c_str(), name.c_str(), &data, &len)) {
                kompoz::ImageBitmap frame = kompoz::decodeToImageBitmap(std::vector<uint8_t>(data, data + len));
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
            kompoz::ImageBitmap frame = kompoz::decodeToImageBitmap(std::vector<uint8_t>(data, data + len));
            airlink_free_bytes(data, len);
            if (!frame.empty()) {
                s.screenFrame.set(frame);
                s.screenStatus.set("live");
            }
        }
    }
    if (s.smsRefreshAt >= 0 && s.ticks >= s.smsRefreshAt) {
        s.smsRefreshAt = -1;
        s.smsRefresh();
    }
}

bool App::ready() const { return impl_->ready.get(); }
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

std::optional<IncomingCall> App::incomingCall() const { return impl_->incomingCall.get(); }
std::vector<CallEntry> App::callLog() const { return impl_->callLog.get(); }
std::string App::callsError() const { return impl_->callsError.get(); }
bool App::canShareCalls() const { return platform::canShareCalls(); }
bool App::callAccess() const { return impl_->callAccess.get(); }
void App::requestCallAccess() const { platform::requestCallAccess(); }
void App::requestCallLog() const { impl_->phoneAsk("calls.log_request", "{}"); }

void App::answerCall() const {
    if (auto call = impl_->incomingCall.peek()) sendTo(call->peerId, "call.answer", "{}");
    impl_->incomingCall.set(std::nullopt);
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

bool App::canUseAdvancedMode() const { return platform::canUseAdvancedMode(); }
bool App::advancedReady() const { return impl_->advancedReady.get(); }
std::string App::advancedStatus() const { return impl_->advancedStatus.get(); }

void App::startAdvancedSetup() const {
    impl_->advancedStatus.set("Bildirimdeki alana eşleme kodunu yazın…");
    platform::startAdvancedSetup();
}

void App::advancedPair(const std::string& code) const {
    if (!impl_->node) return;
    impl_->advancedStatus.set("Eşleniyor…");
    impl_->track(airlink_adb_pair(impl_->node, impl_->adbDir().c_str(), code.c_str()), OpKind::AdbPair);
}

void App::startHelper(const std::string& command) const {
    if (!impl_->node) return platform::helperStarted(false, "not started");
    impl_->track(airlink_adb_shell(impl_->node, impl_->adbDir().c_str(), command.c_str()), OpKind::AdbHelper);
}

std::vector<PhoneApp> App::phoneApps() const { return impl_->phoneApps.get(); }
bool App::phoneAppsLoading() const { return impl_->phoneAppsLoading.get(); }
std::vector<AppWindow> App::appWindows() const { return impl_->appWindows.get(); }
int App::activeWindow() const { return impl_->activeWindow.get(); }

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
    impl_->activeWindow.set(session);
    impl_->phoneAsk("apps.open", "{\"session\":" + std::to_string(session) + ",\"component\":" + json::quote(app.component) +
                                     ",\"width\":720,\"height\":1560,\"dpi\":320,\"fps\":24,\"quality\":70}");
}

void App::closeAppWindow(int session) const {
    impl_->phoneAsk("apps.close", "{\"session\":" + std::to_string(session) + "}");
    impl_->removeWindow(session);
}

void App::showAppWindow(int session) const {
    impl_->updateWindow(session, [](AppWindow& w) { w.minimized = false; });
    impl_->activeWindow.set(session);
}

void App::minimizeAppWindow(int session) const {
    impl_->updateWindow(session, [](AppWindow& w) { w.minimized = true; });
    if (impl_->activeWindow.peek() == session) impl_->activeWindow.set(0);
}

void App::appInput(int session, const std::string& bodyJson) const {
    if (bodyJson.empty()) return;
    // {"cmd":...} + the session
    std::string body = "{\"session\":" + std::to_string(session) + "," + bodyJson.substr(1);
    impl_->phoneAsk("apps.input", body);
}

void App::advancedTest() const {
    if (!impl_->node) return;
    impl_->advancedStatus.set("Kablosuz hata ayıklamaya bağlanılıyor…");
    impl_->track(airlink_adb_shell(impl_->node, impl_->adbDir().c_str(), "id"), OpKind::AdbTest);
}

void App::startScreen(const std::string& peerId) const {
    impl_->screenPeer.set(peerId);
    impl_->screenStatus.set("asking");
    impl_->screenFrame.set(kompoz::ImageBitmap());
    sendTo(peerId, "screen.start", "{\"quality\":60}");
}

void App::stopScreen() const {
    std::string peer = impl_->screenPeer.peek();
    if (!peer.empty()) sendTo(peer, "screen.stop", "{}");
    impl_->closeScreen();
}

void App::screenInput(const std::string& bodyJson) const {
    platform::log("screen input: " + (bodyJson.empty() ? std::string("(outside the image)") : bodyJson));
    std::string peer = impl_->screenPeer.peek();
    if (!peer.empty() && !bodyJson.empty()) sendTo(peer, "screen.input", bodyJson);
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
std::vector<std::string> App::takeIncomingClipboard() const { return std::exchange(impl_->incomingClipboard, {}); }

void App::pair(const std::string& peerId) const {
    impl_->track(airlink_start_pairing(impl_->node, peerId.c_str()), OpKind::StartPairing);
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
    std::string list = "[";
    for (const auto& p : paths) list += (list.size() > 1 ? "," : "") + json::quote(p);
    list += "]";
    impl_->track(airlink_send_files(impl_->node, peerId.c_str(), list.c_str()), OpKind::SendFiles);
}

void App::sendClipboard(const std::string& peerId, const std::string& text) const {
    if (text.empty()) {
        impl_->notices.push_back("Pano boş");
        return;
    }
    impl_->track(airlink_send_clipboard(impl_->node, peerId.c_str(), text.c_str()), OpKind::SendClipboard);
}

void App::forget(const std::string& peerId) const {
    airlink_forget(impl_->node, peerId.c_str());
    impl_->refreshPeers();
}

void App::clearFinishedTransfers() const {
    impl_->transfers.update([](std::vector<Transfer>& list) { std::erase_if(list, [](const Transfer& t) { return !t.active(); }); });
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
