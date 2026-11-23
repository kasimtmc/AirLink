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

enum class OpKind { StartPairing, ConfirmPairing, SendFiles, SendClipboard, SendReply };

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
                case OpKind::SendReply:
                    notices.push_back(ok ? "Yanıt gönderildi" : "Yanıt gönderilemedi: " + err);
                    break;
            }
        }
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
        } else if (kind == "sms.threads_request" || kind == "sms.thread_request" || kind == "sms.send" || kind == "contacts.search") {
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

    void smsAsk(const char* kind, const std::string& body) {
        std::string phone = smsPhone();
        if (!node || phone.empty()) return;
        smsLoading.set(true);
        airlink_send_message(node, phone.c_str(), kind, body.c_str());
    }

    void smsRefresh() {
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
