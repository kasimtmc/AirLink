#include "update_state.hpp"

#include <airlink.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app_state.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include "platform.hpp"

namespace airlink {

using kompoz::MutableState;
using kompoz::mutableStateOf;

namespace {

std::string take(char* s) {
    if (!s) return {};
    std::string out(s);
    airlink_free_string(s);
    return out;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int64_t FIRST_CHECK_MS = 20'000, CHECK_EVERY_MS = 12LL * 3600 * 1000;

// The release files each kind of install takes, by the end of their names (scripts/release).
std::vector<std::string> filesFor(const std::string& kind) {
    if (kind == "appimage") return {"-x86_64.AppImage", ".plasmoid"};
    if (kind == "deb") return {"_amd64.deb"};
    if (kind == "tar") return {"-linux-x86_64.tar.gz"};
    if (kind == "flatpak") return {".flatpak"};
    if (kind == "windows-installer") return {"-windows-x64-setup.exe"};
    if (kind == "windows-zip") return {"-windows-x64.zip"};
    if (kind == "android") return {"-android-arm64.apk"};
    return {};
}

std::filesystem::path fsPath(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

}  // namespace

struct Updates::Impl {
    AirlinkNode* node = nullptr;
    std::string dir;       // <data dir>/updates
    std::string settingPath;
    MutableState<UpdateInfo> info = mutableStateOf(UpdateInfo{});
    MutableState<bool> autoCheck = mutableStateOf(true);
    int64_t nextCheckAt = 0;
    uint64_t checkOp = 0, downloadOp = 0;
    bool silent = false;
    std::string announced;  // the version a notice was given for
    std::vector<std::pair<std::string, std::string>> assets;  // name, url of the available release
    std::string sums;       // its SHA256SUMS
    std::vector<std::string> pending, downloaded;  // urls to fetch, files fetched
    // The install runs on a thread of its own (pkexec waits for a password): its result, for pump.
    std::mutex mutex;
    std::optional<std::string> applied;

    void set(const std::function<void(UpdateInfo&)>& change) {
        info.update(change);
    }

    void notice(const std::string& text) { App::get().notice(text); }

    void fail(const std::string& error) {
        set([&](UpdateInfo& u) {
            u.status = "error";
            u.error = error;
        });
        notice(trf("Güncellenemedi: {}", error));
    }

    void nextDownload() {
        if (pending.empty()) return apply();
        std::string url = pending.front();
        pending.erase(pending.begin());
        downloadOp = airlink_update_download(node, url.c_str(), sums.c_str(), dir.c_str());
        if (!downloadOp) fail(tr("indirme başlatılamadı"));
    }

    void apply() {
        set([](UpdateInfo& u) { u.status = "installing"; });
        std::string kind = platform::updateKind();
        std::vector<std::string> files = downloaded;
        std::weak_ptr<Impl> weak = Updates::get().impl_;
        std::thread([weak, kind, files] {
            std::string error = platform::applyUpdate(kind, files);
            if (auto s = weak.lock()) {
                std::lock_guard lock(s->mutex);
                s->applied = error;
            }
        }).detach();
    }
};

Updates Updates::get() {
    static Updates updates(std::make_shared<Impl>());
    return updates;
}

void Updates::attach(AirlinkNode* node, const std::string& dataDir) const {
    Impl& s = *impl_;
    s.node = node;
    if (!dataDir.empty()) {
        s.dir = dataDir + "/updates";
        s.settingPath = dataDir + "/update-check";
        std::string saved;
        if (std::ifstream in(fsPath(s.settingPath)); in >> saved) s.autoCheck.set(saved != "0");
        // (what an earlier update left)
        std::error_code ec;
        std::filesystem::remove_all(fsPath(s.dir), ec);
    }
    s.set([](UpdateInfo& u) { u.canInstall = !filesFor(platform::updateKind()).empty() && platform::updateKind() != "flatpak"; });
    s.nextCheckAt = nowMs() + FIRST_CHECK_MS;
}

bool Updates::handle(const json::Value& e) const {
    Impl& s = *impl_;
    std::string kind = e["event"].str();
    uint64_t op = e["op"].u64();
    if (kind == "update_progress" && op == s.downloadOp) {
        s.set([&](UpdateInfo& u) {
            u.done = e["done"].i64();
            u.total = e["total"].i64();
        });
        return true;
    }
    if (kind != "op_result" || op == 0) return false;
    if (op == s.checkOp) {
        s.checkOp = 0;
        const json::Value& v = e["value"];
        if (!e["ok"].boolean()) {
            if (s.silent) s.set([](UpdateInfo& u) { u.status = "idle"; });
            else s.fail(e["error"].str());
        } else if (v.isNull()) {
            s.set([](UpdateInfo& u) { u.status = "uptodate"; });
            if (!s.silent) s.notice(trf("AirLink güncel ({})", AIRLINK_VERSION));
        } else {
            s.assets.clear();
            s.sums.clear();
            for (const auto& a : v["assets"].array()) {
                if (a["name"].str() == "SHA256SUMS") s.sums = a["url"].str();
                else s.assets.emplace_back(a["name"].str(), a["url"].str());
            }
            std::string version = v["version"].str();
            s.set([&](UpdateInfo& u) {
                u.status = "available";
                u.version = version;
                u.page = v["page"].str();
                u.notes = v["notes"].str();
                u.error.clear();
            });
            if (s.announced != version) {
                s.announced = version;
                s.notice(trf("AirLink {} çıktı: Ayarlar'dan güncelleyebilirsiniz", version));
                platform::notifyUpdate(version);
            }
        }
        return true;
    }
    if (op == s.downloadOp) {
        s.downloadOp = 0;
        if (!e["ok"].boolean()) s.fail(e["error"].str());
        else {
            s.downloaded.push_back(e["value"].str());
            s.nextDownload();
        }
        return true;
    }
    return false;
}

void Updates::pump() const {
    Impl& s = *impl_;
    if (!s.node) return;
    std::optional<std::string> applied;
    {
        std::lock_guard lock(s.mutex);
        applied.swap(s.applied);
    }
    if (applied) {
        if (!applied->empty()) s.fail(*applied);
        else if (platform::updateRestarts(platform::updateKind())) kompoz::exitApplication();  // (the new one starts as this ends)
        else s.set([](UpdateInfo& u) { u.status = "available"; });
    }
    auto status = s.info.peek().status;
    if (s.autoCheck.peek() && nowMs() >= s.nextCheckAt && status != "checking" && status != "downloading" && status != "installing") {
        s.nextCheckAt = nowMs() + CHECK_EVERY_MS;
        s.silent = true;
        s.checkOp = airlink_update_check(s.node);
        if (s.checkOp && status != "available") s.set([](UpdateInfo& u) { u.status = "checking"; });
    }
}

UpdateInfo Updates::info() const { return impl_->info.get(); }
bool Updates::autoCheck() const { return impl_->autoCheck.get(); }

void Updates::setAutoCheck(bool on) const {
    Impl& s = *impl_;
    s.autoCheck.set(on);
    if (!s.settingPath.empty()) std::ofstream(fsPath(s.settingPath)) << (on ? "1" : "0");
    if (on) s.nextCheckAt = nowMs();
}

void Updates::check() const {
    Impl& s = *impl_;
    if (!s.node || s.checkOp) return;
    s.silent = false;
    s.checkOp = airlink_update_check(s.node);
    if (s.checkOp) s.set([](UpdateInfo& u) {
        u.status = "checking";
        u.error.clear();
    });
}

void Updates::install() const {
    Impl& s = *impl_;
    UpdateInfo u = s.info.peek();
    if (!s.node || u.status != "available") return;
    std::string kind = platform::updateKind();
    std::vector<std::string> wanted = filesFor(kind);
    if (wanted.empty()) {
        // (a build from the source tree: its release page)
        if (!u.page.empty()) platform::openUrl(u.page);
        return;
    }
    s.pending.clear();
    s.downloaded.clear();
    for (const std::string& end : wanted)
        for (const auto& [name, url] : s.assets)
            if (name.ends_with(end)) s.pending.push_back(url);
    if (s.pending.empty()) {
        if (!u.page.empty()) platform::openUrl(u.page);
        return s.notice(tr("Bu sürümde bu kurulum için dosya yok: sürüm sayfası açıldı"));
    }
    std::error_code ec;
    std::filesystem::remove_all(fsPath(s.dir), ec);
    s.set([](UpdateInfo& v) {
        v.status = "downloading";
        v.done = v.total = 0;
        v.error.clear();
    });
    s.nextDownload();
}

}  // namespace airlink
