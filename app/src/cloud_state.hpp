#pragma once

// Cloud drives on the computer (OneDrive, Dropbox, Google Drive) over the core's airlink_cloud_*
// calls: the connected accounts, each one's open folder, and the downloads and uploads. Like App,
// `Drives` is a cheap handle onto one shared state; the UI reads it, App feeds it the core's events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <kompoz/kompoz.hpp>

struct AirlinkNode;
namespace json { class Value; }

namespace airlink {

struct CloudAccount {
    std::string id;        // "<provider>:<user>"
    std::string provider;  // onedrive, dropbox, gdrive
    std::string name;
    std::string email;
    bool operator==(const CloudAccount&) const = default;
};

struct CloudEntry {
    std::string id;
    std::string name;
    bool folder = false;
    int64_t size = 0;
    std::string modified;  // RFC 3339 or empty
    std::string kind;      // a Google document's type, else empty
    std::string json;      // as the core gave it (a download takes it back)
    bool operator==(const CloudEntry&) const = default;
};

struct CloudFolder {
    std::string id;  // "" is the drive's root
    std::string name;
    bool operator==(const CloudFolder&) const = default;
};

// An account's tab: the folder open in it (or a search), its entries and the drive's space.
struct CloudView {
    std::vector<CloudFolder> path;  // from the root to the open folder
    std::vector<CloudEntry> entries;
    std::string search;             // non-empty: entries are this search's results
    bool loading = false;
    std::string error;
    int64_t used = 0, total = 0;    // bytes; total 0: unknown
    bool operator==(const CloudView&) const = default;
};

// A download or upload.
struct CloudJob {
    uint64_t op = 0;
    std::string accountId;
    std::string name;
    bool upload = false;
    std::string sendTo;  // a download for this paired device: sent there once it is in
    int64_t done = 0, total = 0;
    bool finished = false;
    std::string error;
    std::string path;    // a finished download's file
    int64_t endedAt = 0; // when it finished or failed (ms): the card goes a few seconds later
    float fraction() const { return total > 0 ? float(double(done) / double(total)) : (finished ? 1.f : 0.f); }
    bool active() const { return !finished && error.empty(); }
    bool operator==(const CloudJob&) const = default;
};

// A finished download or upload in an account's history (kept in <data dir>/cloud-history.json).
struct CloudRecord {
    int64_t id = 0;
    std::string accountId;
    std::string name;
    bool upload = false;
    int64_t size = 0;
    int64_t time = 0;     // ms
    std::string path;     // a download's file on this computer
    std::string sendTo;   // a download that went on to this paired device
    std::string error;    // empty: done
    bool operator==(const CloudRecord&) const = default;
};

// How an account's files are shown.
enum class CloudLayout { List, Grid };

class Drives {
public:
    static Drives get();

    // ---- From App ----
    // The core is up: loads the accounts. `downloadDir` takes downloads, `dataDir` a cache for
    // files on their way to another device.
    void attach(AirlinkNode* node, const std::string& downloadDir, const std::string& dataDir) const;
    // A core event: true if it was the drives' (cloud_auth, cloud_progress, their op_results).
    bool handle(const json::Value& event) const;
    void pump() const;  // files picked to upload (the picker answers on its own thread)

    // ---- What the UI reads ----
    bool available() const;                    // this build can sign in to at least one provider
    std::vector<std::string> providers() const;
    std::vector<CloudAccount> accounts() const;
    std::string connecting() const;            // the provider being signed in to (empty: none)
    CloudView view(const std::string& accountId) const;
    std::vector<CloudJob> jobs() const;        // newest first; a finished one only for a few seconds
    std::vector<CloudRecord> history(const std::string& accountId) const;  // newest first
    CloudLayout layout() const;
    // An image's or a video's small picture once it is in (empty until then; see requestThumbnail).
    kompoz::ImageBitmap thumbnail(const std::string& accountId, const CloudEntry& entry) const;

    // ---- Actions ----
    void connect(const std::string& provider) const;
    void cancelConnect() const;
    void disconnect(const std::string& accountId) const;
    void openRoot(const std::string& accountId) const;  // loads the tab the first time it shows
    void open(const std::string& accountId, const CloudEntry& folder) const;
    void goUp(const std::string& accountId, int index) const;  // to path[index]
    void refresh(const std::string& accountId) const;
    void search(const std::string& accountId, const std::string& query) const;  // "" ends it
    void download(const std::string& accountId, const CloudEntry& entry) const;
    void sendToDevice(const std::string& accountId, const CloudEntry& entry, const std::string& peerId) const;
    void pickAndUpload(const std::string& accountId) const;
    void upload(const std::string& accountId, const std::vector<std::string>& paths) const;
    void createFolder(const std::string& accountId, const std::string& name) const;
    void rename(const std::string& accountId, const CloudEntry& entry, const std::string& name) const;
    void trash(const std::string& accountId, const CloudEntry& entry) const;  // to the recycle bin
    void cancelJob(uint64_t op) const;
    void openJobFile(const CloudJob& job) const;
    void setLayout(CloudLayout layout) const;
    void requestThumbnail(const std::string& accountId, const CloudEntry& entry) const;  // an image or a video: fetched once
    void forgetRecord(int64_t id) const;
    void clearHistory(const std::string& accountId) const;

    bool operator==(const Drives& other) const { return impl_ == other.impl_; }

    struct Impl;

private:
    explicit Drives(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

std::string providerTitle(const std::string& provider);
bool isPicture(const std::string& name);  // an image or a video, by its extension
bool isVideo(const std::string& name);

}  // namespace airlink
