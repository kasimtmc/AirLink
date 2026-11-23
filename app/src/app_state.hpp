#pragma once

// The app's state over the Rust core: turns the core's events into Kompoz states the UI reads,
// and the UI's actions into core calls. `App` is a cheap copyable handle (copies share one state),
// so composables and callbacks can hold it by value.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <kompoz/kompoz.hpp>

namespace airlink {

struct DeviceInfo {
    std::string id;
    std::string name;
    std::string platform;
    int port = 0;
    std::string downloadDir;
    bool operator==(const DeviceInfo&) const = default;
};

struct Peer {
    std::string id;
    std::string name;
    std::string platform;
    bool paired = false;
    bool online = false;
    bool operator==(const Peer&) const = default;
};

struct OfferedFile {
    std::string path;
    int64_t size = 0;
    bool operator==(const OfferedFile&) const = default;
};

// Something the user must answer: an incoming pairing (isPairing) or file offer.
struct Prompt {
    uint64_t requestId = 0;
    bool isPairing = false;
    std::string peerName;
    std::string code;                // pairing
    std::vector<OfferedFile> files;  // file offer
    int64_t totalSize = 0;
    bool operator==(const Prompt&) const = default;
};

// Our own pairing attempt, waiting for the user to compare the code.
struct OutgoingPairing {
    uint64_t pairing = 0;
    std::string peerName;
    std::string code;
    bool waitingForPeer = false;  // confirmed here, the other side has not answered yet
    bool operator==(const OutgoingPairing&) const = default;
};

struct Transfer {
    std::string id;
    std::string peerId;
    bool incoming = false;
    bool waiting = false;  // outgoing offer not accepted yet
    bool completed = false;
    int64_t done = 0;
    int64_t total = 0;
    int fileCount = 0;
    std::string error;
    float fraction() const { return total > 0 ? float(double(done) / double(total)) : (completed ? 1.f : 0.f); }
    bool active() const { return !completed && error.empty(); }
    bool operator==(const Transfer&) const = default;
};

// A notification of a paired phone, mirrored on this computer.
struct PhoneNotification {
    std::string peerId;
    std::string key;  // the phone's own key for it
    std::string app;
    std::string title;
    std::string text;
    int64_t time = 0;  // ms since the epoch
    bool canReply = false;
    bool operator==(const PhoneNotification&) const = default;
};

// A phone's SMS conversation, as the computer shows it.
struct SmsThread {
    int64_t threadId = 0;
    std::string address;
    std::string name;  // the contact's, or empty
    std::string snippet;
    int64_t date = 0;
    int unread = 0;
    std::string title() const { return name.empty() ? address : name; }
    bool operator==(const SmsThread&) const = default;
};

struct SmsMessage {
    int64_t id = 0;
    std::string body;
    int64_t date = 0;
    bool outgoing = false;
    bool failed = false;
    bool operator==(const SmsMessage&) const = default;
};

// A contact of the phone, found by a search from the computer.
struct ContactMatch {
    std::string name;
    std::string number;
    std::string label;  // "Mobil", "Ev"...
    bool operator==(const ContactMatch&) const = default;
};

class App {
public:
    // The one app state of the process.
    static App get();

    // Starts the core once the platform's configuration is known, then applies pending
    // events. Called from the UI loop every few milliseconds.
    void pump() const;

    // ---- What the UI reads (each read subscribes the composable) ----
    bool ready() const;
    std::string startError() const;
    DeviceInfo me() const;
    std::vector<Peer> peers() const;
    std::vector<Peer> pairedPeers() const;
    std::vector<Peer> nearbyPeers() const;
    std::vector<Transfer> transfers() const;  // newest first
    std::optional<Prompt> prompt() const;      // the oldest unanswered one
    std::optional<OutgoingPairing> outgoing() const;
    // Files handed to the app from outside, waiting for the user to pick a device (empty: none).
    std::vector<std::string> share() const;
    // Phones' notifications, newest first.
    std::vector<PhoneNotification> phoneNotifications() const;
    // This phone: may it share its notifications, and is it allowed to.
    bool canShareNotifications() const;
    bool notificationAccess() const;

    // ---- SMS: on a computer, a paired phone's; on a phone, whether it shares them ----
    std::string smsPhone() const;  // the online paired phone used for SMS (empty: none)
    std::vector<SmsThread> smsThreads() const;
    std::optional<SmsThread> smsOpenThread() const;
    std::vector<SmsMessage> smsMessages() const;  // the open thread's, oldest first
    std::string smsError() const;
    bool smsLoading() const;
    std::vector<ContactMatch> contactResults() const;  // for the latest searchContacts
    bool canShareSms() const;
    bool smsAccess() const;
    std::string peerName(const std::string& id) const;

    // Messages for a snackbar, and clipboard texts from other devices, since the last call.
    std::vector<std::string> takeNotices() const;
    std::vector<std::string> takeIncomingClipboard() const;

    // ---- Actions ----
    void pair(const std::string& peerId) const;
    void confirmOutgoing(bool accept) const;
    void respond(uint64_t requestId, bool accept) const;
    void pickAndSend(const std::string& peerId) const;
    void sendFiles(const std::string& peerId, const std::vector<std::string>& paths) const;
    // A drop: local paths, or else URIs the platform first copies to local files (Android).
    void sendDropped(const std::string& peerId, const std::vector<std::string>& paths,
                     const std::vector<std::string>& uris) const;
    void sendClipboard(const std::string& peerId, const std::string& text) const;
    void forget(const std::string& peerId) const;
    void clearFinishedTransfers() const;
    void shareTo(const std::string& peerId) const;
    void cancelShare() const;
    // A phone's notification: dismiss it there, or answer it (messaging apps).
    void dismissPhoneNotification(const PhoneNotification& n) const;
    void replyPhoneNotification(const PhoneNotification& n, const std::string& text) const;
    void openNotificationAccessSettings() const;
    void requestSmsThreads() const;
    void openSmsThread(const SmsThread& thread) const;
    void closeSmsThread() const;
    void sendSms(const std::string& address, const std::string& text) const;
    void requestSmsAccess() const;
    void searchContacts(const std::string& query) const;
    // To one paired device, from any thread (answers this phone gives a computer).
    void sendTo(const std::string& peerId, const std::string& kind, const std::string& bodyJson) const;
    // To every reachable paired device, from any thread (this phone's notifications, new SMS).
    void broadcast(const std::string& kind, const std::string& bodyJson) const;

    bool operator==(const App& other) const { return impl_ == other.impl_; }

    struct Impl;

private:
    explicit App(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<Impl> impl_;
};

// A size as people read it: "3.4 MB".
std::string humanSize(int64_t bytes);

}  // namespace airlink
