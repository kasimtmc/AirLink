#include "app_state.hpp"

#include <airlink.h>

#include <algorithm>
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

enum class OpKind { StartPairing, ConfirmPairing, SendFiles, SendClipboard };

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

    std::vector<std::string> notices;
    std::vector<std::string> incomingClipboard;
    std::map<uint64_t, OpKind> ops;
    int ticks = 0;

    // Filled by the platform's file picker on any thread, applied in pump().
    std::mutex pickedMutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> picked;
    std::vector<std::string> shared;  // from platform::setShareHandler, any thread

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
            }
        }
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

    // Reachability changes without an event (a paired device's connection drops): refresh now and then.
    if (++s.ticks % 40 == 0) s.refreshPeers();
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
