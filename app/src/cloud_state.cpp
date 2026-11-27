#include "cloud_state.hpp"

#include <airlink.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>

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

CloudEntry entryFrom(const json::Value& v) {
    return CloudEntry{.id = v["id"].str(),
                      .name = v["name"].str(),
                      .folder = v["folder"].boolean(),
                      .size = v["size"].i64(),
                      .modified = v["modified"].str(),
                      .kind = v["kind"].str(),
                      .json = v.dump()};
}

std::vector<CloudEntry> entriesFrom(const json::Value& v) {
    std::vector<CloudEntry> out;
    for (const auto& e : v.array()) out.push_back(entryFrom(e));
    return out;
}

CloudAccount accountFrom(const json::Value& v) {
    return CloudAccount{.id = v["id"].str(), .provider = v["provider"].str(), .name = v["name"].str(), .email = v["email"].str()};
}

// The core's messages, as people read them.
std::string clean(const std::string& error) {
    if (error.find("error sending request") != std::string::npos || error.find("dns error") != std::string::npos)
        return tr("Bağlanılamadı: internet bağlantısını denetleyin");
    return error;
}

enum class Kind { Connect, Disconnect, List, Search, Quota, Download, Upload, CreateFolder, Rename, Delete };

struct Op {
    Kind kind;
    std::string account;
    std::string folder;  // List: the folder asked for; Upload/CreateFolder/Rename/Delete: the folder to reload
};

}  // namespace

struct Drives::Impl {
    AirlinkNode* node = nullptr;
    std::string downloadDir;
    std::string cacheDir;
    MutableState<std::vector<std::string>> providers = mutableStateOf(std::vector<std::string>{});
    MutableState<std::vector<CloudAccount>> accounts = mutableStateOf(std::vector<CloudAccount>{});
    MutableState<std::string> connecting = mutableStateOf(std::string());
    uint64_t connectOp = 0;
    MutableState<std::map<std::string, CloudView>> views = mutableStateOf(std::map<std::string, CloudView>{});
    MutableState<std::vector<CloudJob>> jobs = mutableStateOf(std::vector<CloudJob>{});
    std::map<uint64_t, Op> ops;
    std::map<std::string, uint64_t> listing;  // an account's latest list or search: older answers are dropped
    int cacheFolders = 0;
    std::mutex pickedMutex;
    std::vector<std::pair<std::string, std::vector<std::string>>> picked;

    void notice(const std::string& text) { App::get().notice(text); }

    void loadState() {
        json::Value state = json::Value::parse(take(airlink_cloud_state(node)));
        std::vector<std::string> list;
        for (const auto& p : state["providers"].array()) list.push_back(p.str());
        providers.set(list);
        std::vector<CloudAccount> all;
        for (const auto& a : state["accounts"].array()) all.push_back(accountFrom(a));
        accounts.set(all);
    }

    void track(uint64_t op, Op what) {
        if (op) ops[op] = std::move(what);
        else notice(tr("Bulut sürücüsü işlemi başlatılamadı"));
    }

    void edit(const std::string& account, const std::function<void(CloudView&)>& change) {
        views.update([&](std::map<std::string, CloudView>& all) {
            CloudView& v = all[account];
            if (v.path.empty()) v.path.push_back(CloudFolder{});
            change(v);
        });
    }

    bool searching(const std::string& account) {
        const auto& all = views.peek();
        auto it = all.find(account);
        return it != all.end() && !it->second.search.empty();
    }

    std::string openFolder(const std::string& account) {
        const auto& all = views.peek();
        auto it = all.find(account);
        return it == all.end() || it->second.path.empty() ? std::string() : it->second.path.back().id;
    }

    void list(const std::string& account) {
        std::string folder = openFolder(account);
        edit(account, [](CloudView& v) {
            v.loading = true;
            v.error.clear();
            v.search.clear();
        });
        uint64_t op = airlink_cloud_list(node, account.c_str(), folder.c_str());
        listing[account] = op;
        track(op, Op{Kind::List, account, folder});
        track(airlink_cloud_quota(node, account.c_str()), Op{Kind::Quota, account, {}});
    }

    void updateJob(uint64_t op, const std::function<void(CloudJob&)>& change) {
        jobs.update([&](std::vector<CloudJob>& all) {
            for (CloudJob& j : all)
                if (j.op == op) change(j);
        });
    }

    void startDownload(const std::string& account, const CloudEntry& entry, const std::string& dir, const std::string& sendTo) {
        if (entry.folder) return notice(tr("Klasörler indirilemiyor; içindeki dosyaları seçin"));
        uint64_t op = airlink_cloud_download(node, account.c_str(), entry.json.c_str(), dir.c_str());
        if (!op) return notice(tr("İndirme başlatılamadı"));
        ops[op] = Op{Kind::Download, account, {}};
        jobs.update([&](std::vector<CloudJob>& all) {
            all.insert(all.begin(), CloudJob{.op = op, .accountId = account, .name = entry.name, .sendTo = sendTo, .total = entry.size});
        });
    }

    void result(uint64_t op, const Op& what, bool ok, const json::Value& value, const std::string& error) {
        std::string err = clean(error);
        switch (what.kind) {
            case Kind::Connect:
                if (op != connectOp) return;
                connecting.set({});
                connectOp = 0;
                if (ok) {
                    loadState();
                    CloudAccount a = accountFrom(value);
                    notice(trf("{} bağlandı: {}", providerTitle(a.provider), a.email.empty() ? a.name : a.email));
                } else if (err != "cancelled") {
                    notice(trf("Bağlanılamadı: {}", err));
                }
                break;
            case Kind::Disconnect:
                loadState();
                views.update([&](std::map<std::string, CloudView>& all) { all.erase(what.account); });
                if (!ok) notice(trf("Hesap kaldırılamadı: {}", err));
                break;
            case Kind::List:
            case Kind::Search:
                if (listing[what.account] != op) return;
                edit(what.account, [&](CloudView& v) {
                    v.loading = false;
                    if (ok) v.entries = entriesFrom(value);
                    else v.error = err;
                });
                break;
            case Kind::Quota:
                if (ok) edit(what.account, [&](CloudView& v) {
                    v.used = value["used"].i64();
                    v.total = value["total"].i64();
                });
                break;
            case Kind::Download: {
                std::string path = value.str();
                std::string sendTo;
                updateJob(op, [&](CloudJob& j) {
                    if (ok) {
                        j.finished = true;
                        j.path = path;
                        j.done = std::max(j.done, j.total);
                    } else {
                        j.error = err == "cancelled" ? tr("İptal edildi") : err;
                    }
                    sendTo = j.sendTo;
                });
                if (ok && !sendTo.empty()) App::get().sendFiles(sendTo, {path});
                else if (ok) notice(trf("İndirildi: {}", std::filesystem::path(std::u8string(path.begin(), path.end())).filename().string()));
                else if (err != "cancelled") notice(trf("İndirilemedi: {}", err));
                break;
            }
            case Kind::Upload:
                updateJob(op, [&](CloudJob& j) {
                    if (ok) {
                        j.finished = true;
                        j.done = std::max(j.done, j.total);
                    } else {
                        j.error = err == "cancelled" ? tr("İptal edildi") : err;
                    }
                });
                if (!ok && err != "cancelled") notice(trf("Yüklenemedi: {}", err));
                if (ok && openFolder(what.account) == what.folder && !searching(what.account)) list(what.account);
                break;
            case Kind::CreateFolder:
            case Kind::Rename:
            case Kind::Delete:
                if (!ok) {
                    notice(what.kind == Kind::CreateFolder ? trf("Klasör oluşturulamadı: {}", err)
                           : what.kind == Kind::Rename     ? trf("Yeniden adlandırılamadı: {}", err)
                                                           : trf("Silinemedi: {}", err));
                } else if (what.kind == Kind::Delete) {
                    notice(tr("Çöp kutusuna taşındı"));
                }
                if (openFolder(what.account) == what.folder && !searching(what.account)) list(what.account);
                break;
        }
    }
};

Drives Drives::get() {
    static Drives drives(std::make_shared<Impl>());
    return drives;
}

std::string providerTitle(const std::string& provider) {
    if (provider == "onedrive") return "OneDrive";
    if (provider == "dropbox") return "Dropbox";
    if (provider == "gdrive") return "Google Drive";
    return provider;
}

void Drives::attach(AirlinkNode* node, const std::string& downloadDir, const std::string& dataDir) const {
    Impl& s = *impl_;
    s.node = node;
    s.downloadDir = downloadDir;
    std::u8string data(dataDir.begin(), dataDir.end());
    auto cache = std::filesystem::path(data) / "cloud-cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);  // what an earlier run sent on (or left half-done)
    auto u8 = cache.u8string();
    s.cacheDir = std::string(u8.begin(), u8.end());
    s.loadState();
}

bool Drives::handle(const json::Value& e) const {
    Impl& s = *impl_;
    std::string kind = e["event"].str();
    uint64_t op = e["op"].u64();
    if (kind == "cloud_auth") {
        if (op == s.connectOp) platform::openUrl(e["url"].str());
        return true;
    }
    if (kind == "cloud_progress") {
        s.updateJob(op, [&](CloudJob& j) {
            j.done = e["done"].i64();
            j.total = e["total"].i64();
        });
        return true;
    }
    if (kind != "op_result") return false;
    auto it = s.ops.find(op);
    if (it == s.ops.end()) return false;
    Op what = it->second;
    s.ops.erase(it);
    s.result(op, what, e["ok"].boolean(), e["value"], e["error"].str());
    return true;
}

void Drives::pump() const {
    std::vector<std::pair<std::string, std::vector<std::string>>> picked;
    {
        std::lock_guard lock(impl_->pickedMutex);
        picked.swap(impl_->picked);
    }
    for (auto& [account, paths] : picked) upload(account, paths);
}

bool Drives::available() const { return !impl_->providers.get().empty(); }
std::vector<std::string> Drives::providers() const { return impl_->providers.get(); }
std::vector<CloudAccount> Drives::accounts() const { return impl_->accounts.get(); }
std::string Drives::connecting() const { return impl_->connecting.get(); }

CloudView Drives::view(const std::string& accountId) const {
    auto all = impl_->views.get();
    auto it = all.find(accountId);
    if (it != all.end()) return it->second;
    CloudView fresh;
    fresh.path.push_back(CloudFolder{});
    return fresh;
}

std::vector<CloudJob> Drives::jobs() const { return impl_->jobs.get(); }

void Drives::connect(const std::string& provider) const {
    Impl& s = *impl_;
    if (!s.node) return;
    if (s.connectOp) airlink_cloud_cancel(s.node, s.connectOp);
    s.connectOp = airlink_cloud_connect(s.node, provider.c_str());
    if (!s.connectOp) return s.notice(tr("Giriş başlatılamadı"));
    s.ops[s.connectOp] = Op{Kind::Connect, {}, {}};
    s.connecting.set(provider);
}

void Drives::cancelConnect() const {
    Impl& s = *impl_;
    if (s.connectOp) airlink_cloud_cancel(s.node, s.connectOp);
}

void Drives::disconnect(const std::string& accountId) const {
    Impl& s = *impl_;
    s.track(airlink_cloud_disconnect(s.node, accountId.c_str()), Op{Kind::Disconnect, accountId, {}});
}

void Drives::openRoot(const std::string& accountId) const {
    Impl& s = *impl_;
    auto all = s.views.peek();
    if (all.find(accountId) == all.end()) s.list(accountId);
}

void Drives::open(const std::string& accountId, const CloudEntry& folder) const {
    if (!folder.folder) return;
    impl_->edit(accountId, [&](CloudView& v) {
        // From a search, the folder is opened from the root (its parents aren't known).
        if (!v.search.empty()) v.path.resize(1);
        v.path.push_back(CloudFolder{.id = folder.id, .name = folder.name});
        v.entries.clear();
    });
    impl_->list(accountId);
}

void Drives::goUp(const std::string& accountId, int index) const {
    impl_->edit(accountId, [&](CloudView& v) {
        if (index >= 0 && size_t(index) < v.path.size()) v.path.resize(size_t(index) + 1);
        v.entries.clear();
    });
    impl_->list(accountId);
}

void Drives::refresh(const std::string& accountId) const {
    auto query = view(accountId).search;
    if (query.empty()) impl_->list(accountId);
    else search(accountId, query);
}

void Drives::search(const std::string& accountId, const std::string& query) const {
    Impl& s = *impl_;
    if (query.empty()) return s.list(accountId);
    s.edit(accountId, [&](CloudView& v) {
        v.search = query;
        v.loading = true;
        v.error.clear();
    });
    uint64_t op = airlink_cloud_search(s.node, accountId.c_str(), query.c_str());
    s.listing[accountId] = op;
    s.track(op, Op{Kind::Search, accountId, {}});
}

void Drives::download(const std::string& accountId, const CloudEntry& entry) const {
    impl_->startDownload(accountId, entry, impl_->downloadDir, {});
}

void Drives::sendToDevice(const std::string& accountId, const CloudEntry& entry, const std::string& peerId) const {
    Impl& s = *impl_;
    // A folder of its own per file, so names never clash and each goes when sent.
    std::u8string base(s.cacheDir.begin(), s.cacheDir.end());
    auto dir = (std::filesystem::path(base) / std::to_string(++s.cacheFolders) / "").u8string();
    s.startDownload(accountId, entry, std::string(dir.begin(), dir.end()), peerId);
}

void Drives::pickAndUpload(const std::string& accountId) const {
    std::weak_ptr<Impl> weak = impl_;
    platform::pickFiles([weak, accountId](std::vector<std::string> paths) {
        if (auto s = weak.lock()) {
            std::lock_guard lock(s->pickedMutex);
            s->picked.emplace_back(accountId, std::move(paths));
        }
    });
}

void Drives::upload(const std::string& accountId, const std::vector<std::string>& paths) const {
    Impl& s = *impl_;
    std::string folder = s.openFolder(accountId);
    for (const std::string& path : paths) {
        std::error_code ec;
        std::filesystem::path p(std::u8string(path.begin(), path.end()));
        uint64_t op = airlink_cloud_upload(s.node, accountId.c_str(), folder.c_str(), path.c_str());
        if (!op) {
            s.notice(tr("Yükleme başlatılamadı"));
            continue;
        }
        s.ops[op] = Op{Kind::Upload, accountId, folder};
        auto name = p.filename().u8string();
        // (a folder: all its files, as the core counts its progress)
        int64_t size = 0;
        if (std::filesystem::is_directory(p, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(p, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec) && !it->is_symlink(ec)) size += int64_t(it->file_size(ec));
            ec.clear();
        } else {
            size = int64_t(std::filesystem::file_size(p, ec));
        }
        s.jobs.update([&](std::vector<CloudJob>& all) {
            all.insert(all.begin(), CloudJob{.op = op, .accountId = accountId, .name = std::string(name.begin(), name.end()),
                                             .upload = true, .total = ec ? 0 : size});
        });
    }
}

void Drives::createFolder(const std::string& accountId, const std::string& name) const {
    Impl& s = *impl_;
    if (name.empty()) return;
    std::string folder = s.openFolder(accountId);
    s.track(airlink_cloud_create_folder(s.node, accountId.c_str(), folder.c_str(), name.c_str()), Op{Kind::CreateFolder, accountId, folder});
}

void Drives::rename(const std::string& accountId, const CloudEntry& entry, const std::string& name) const {
    Impl& s = *impl_;
    if (name.empty() || name == entry.name) return;
    s.track(airlink_cloud_rename(s.node, accountId.c_str(), entry.id.c_str(), name.c_str()), Op{Kind::Rename, accountId, s.openFolder(accountId)});
}

void Drives::trash(const std::string& accountId, const CloudEntry& entry) const {
    Impl& s = *impl_;
    s.track(airlink_cloud_delete(s.node, accountId.c_str(), entry.id.c_str()), Op{Kind::Delete, accountId, s.openFolder(accountId)});
}

void Drives::cancelJob(uint64_t op) const { airlink_cloud_cancel(impl_->node, op); }

void Drives::clearFinishedJobs() const {
    impl_->jobs.update([](std::vector<CloudJob>& all) { std::erase_if(all, [](const CloudJob& j) { return !j.active(); }); });
}

void Drives::openJobFile(const CloudJob& job) const {
    if (!job.path.empty()) platform::openFile(job.path);
}

}  // namespace airlink
