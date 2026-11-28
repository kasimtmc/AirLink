#pragma once

// AirLink's own updates, from its GitHub releases: looked for at start and twice a day (or by
// hand), and installed the way this copy was installed (platform::updateKind): the AppImage
// replaced, the .deb installed through pkexec, the tar.gz's folder refreshed (with its
// install.sh: menus, file-manager entries, the Plasma widget), the Windows installer run, the APK
// handed to Android's installer. A Flatpak, or a build from the source tree, is told about it only.
// Like Drives, `Updates` is a cheap handle onto one shared state the UI reads.

#include <cstdint>
#include <memory>
#include <string>

#include <kompoz/kompoz.hpp>

struct AirlinkNode;
namespace json { class Value; }

namespace airlink {

struct UpdateInfo {
    // idle, checking, uptodate, available, downloading, installing, error
    std::string status = "idle";
    std::string version;  // the newer release's
    std::string page;     // its page on GitHub
    std::string notes;
    int64_t done = 0, total = 0;  // the download
    std::string error;
    bool canInstall = false;      // this copy updates itself (else the page is shown)
    bool operator==(const UpdateInfo&) const = default;
};

class Updates {
public:
    static Updates get();

    void attach(AirlinkNode* node, const std::string& dataDir) const;
    bool handle(const json::Value& event) const;  // update_progress, its op_results
    void pump() const;                            // the automatic checks

    UpdateInfo info() const;
    bool autoCheck() const;
    void setAutoCheck(bool on) const;
    void check() const;    // now (a notice says the result)
    void install() const;  // the available release

    bool operator==(const Updates& other) const { return impl_ == other.impl_; }

    struct Impl;

private:
    explicit Updates(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

}  // namespace airlink
