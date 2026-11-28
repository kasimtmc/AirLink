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
    std::string dataDir;  // the core's: identity, pairings, AirLink's own files
    bool operator==(const DeviceInfo&) const = default;
};

struct Peer {
    std::string id;
    std::string name;        // as the user renamed it here, else its own
    std::string deviceName;  // the name the device gives itself
    std::string platform;
    bool paired = false;
    bool online = false;
    bool autoAccept = false;  // "Güvenilir": no prompts, the clipboard is shared with it
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

// A file that went to or came from a paired device (the Files tab); kept across restarts.
struct FileRecord {
    int64_t id = 0;
    std::string peerId;
    std::string peerName;  // as it was then (the device may be forgotten since)
    std::string path;      // received: where it was saved; sent: what was sent
    int64_t size = 0;
    int64_t time = 0;  // ms since the epoch
    bool incoming = true;
    std::string displayName;  // a content:// path's (Android: a file sent from another app)
    std::string name() const {
        if (!displayName.empty()) return displayName;
        auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    bool isUri() const { return path.starts_with("content://"); }
    bool operator==(const FileRecord&) const = default;
};

// What plays on a paired phone (its media.state), for the tray panel.
struct PhoneMedia {
    std::string peerId;
    bool active = false;
    std::string app, title, artist, album;
    bool playing = false;
    int64_t position = 0, positionAt = 0, duration = 0;  // ms; positionAt: ms since the epoch
    bool canNext = false, canPrevious = false;
    kompoz::ImageBitmap art;
    bool operator==(const PhoneMedia&) const = default;
};

// A paired phone's battery (`phone.battery`), and whether "find my phone" rings it now.
struct PhoneBattery {
    std::string peerId;
    int level = -1;  // percent; -1: not known
    bool charging = false;
    bool ringing = false;
    bool operator==(const PhoneBattery&) const = default;
};

// Text that went through the clipboard share, newest first (kept while AirLink runs).
struct ClipboardItem {
    int64_t id = 0;
    std::string peerId;
    std::string text;
    int64_t time = 0;  // ms since the epoch
    bool incoming = true;
    bool sensitive = false;  // a password: shown masked, gone once copied or the panel closes
    bool operator==(const ClipboardItem&) const = default;
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

// An app of a paired phone (advanced mode), for its launcher on the computer.
struct PhoneApp {
    std::string label;
    std::string component;  // "package/.Activity"
    kompoz::ImageBitmap icon;
    std::string iconPng;  // the icon as the phone sent it (for a shortcut's)
    bool operator==(const PhoneApp&) const = default;
};

// A phone app open on the computer, on a display of its own on the phone (advanced mode).
struct AppWindow {
    int session = 0;
    std::string label;
    std::string component;
    std::string status;  // "starting", "live", or an error
    int frontRequests = 0;
    kompoz::ImageBitmap frame;
    bool operator==(const AppWindow&) const = default;
};

// A paired device asks to see and control this computer's screen.
struct DesktopRequest {
    std::string peerId;
    std::string peerName;
    std::string options;  // the viewer's {max_width, fps}
    bool operator==(const DesktopRequest&) const = default;
};

// A call ringing on a paired phone.
struct IncomingCall {
    std::string peerId;
    std::string number;
    std::string name;
    std::string title() const { return !name.empty() ? name : !number.empty() ? number : "Bilinmeyen numara"; }
    bool operator==(const IncomingCall&) const = default;
};

// A paired phone's call in progress, and what can be done with it from here.
struct ActiveCall {
    std::string peerId;
    std::string number;
    std::string name;
    std::string state;  // dialing, active, held
    bool held = false;
    bool canHold = false;
    bool muted = false;
    std::string route;                // earpiece, speaker, bluetooth, wired
    std::vector<std::string> routes;  // the ones available
    int volume = 0;
    int volumeMax = 0;
    int64_t since = 0;         // ms since the epoch, for the timer
    bool fullControl = false;  // the phone's companion InCallService is bound (hold, routes)
    std::string title() const { return !name.empty() ? name : !number.empty() ? number : "Arama"; }
    bool operator==(const ActiveCall&) const = default;
};

// An entry of a phone's call log.
struct CallEntry {
    std::string number;
    std::string name;
    std::string type;  // incoming, outgoing, missed, rejected, blocked, other
    int64_t date = 0;
    int64_t duration = 0;  // seconds
    std::string title() const { return name.empty() ? number : name; }
    bool operator==(const CallEntry&) const = default;
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
    bool sharingPaused() const;
    bool recentError() const;  // a failed operation briefly marks the desktop tray
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
    bool batteryRestricted() const;  // Android: not exempt from the battery saver (checked now and then)

    // ---- SMS: on a computer, a paired phone's; on a phone, whether it shares them ----
    std::string smsPhone() const;  // the online paired phone used for SMS (empty: none)
    std::vector<SmsThread> smsThreads() const;
    std::optional<SmsThread> smsOpenThread() const;
    std::vector<SmsMessage> smsMessages() const;  // the open thread's, oldest first
    std::string smsError() const;
    bool smsLoading() const;
    std::vector<ContactMatch> contactResults() const;  // for the latest searchContacts
    std::vector<ContactMatch> phoneContacts() const;
    bool phoneContactsLoading() const;
    bool phoneContactsHasMore() const;
    std::string phoneContactsError() const;
    std::optional<IncomingCall> incomingCall() const;
    std::vector<CallEntry> callLog() const;
    bool callLogLoading() const;
    bool callLogHasMore() const;
    std::optional<ActiveCall> activeCall() const;

    // ---- A paired phone's screen on this computer ----
    std::string screenPeer() const;    // whose screen is open (empty: none)
    std::string screenStatus() const;  // "asking" (the phone's user must agree), "live"
    kompoz::ImageBitmap screenFrame() const;
    bool canShareScreen() const;       // this phone can share its screen
    bool screenControl() const;        // and carry out a computer's touches

    // ---- Phase 4: a paired computer's screen on this phone, and this computer's own share ----
    // On the phone (viewer): the computer whose screen is open, "asking" / "live", its frame.
    std::string desktopPeer() const;
    std::string desktopStatus() const;
    kompoz::ImageBitmap desktopFrame() const;
    // On the computer (shared): the device asking, the one watching (empty: none).
    std::optional<DesktopRequest> desktopRequest() const;
    std::string desktopViewer() const;
    bool canShareDesktop() const;  // this computer can share its screen (Linux, Windows)

    // ---- Fling to send (phase 4B, a phone): push the phone toward a computer ----
    bool canFling() const;
    bool flingEnabled() const;
    bool flingMediaAccess() const;
    void requestFlingMediaAccess() const;
    std::string flingTarget() const;  // the computer a fling goes to (empty: none online)

    // ---- Advanced mode (this phone's own wireless debugging) ----
    bool canUseAdvancedMode() const;
    bool advancedReady() const;        // paired, and a shell command ran as the shell user
    std::string advancedStatus() const;
    std::string advancedPort() const;   // optional Wireless debugging TCP port when mDNS is unavailable
    // On a computer: the phone's apps, and the ones open here.
    std::vector<PhoneApp> phoneApps() const;
    bool phoneAppsLoading() const;
    std::vector<AppWindow> appWindows() const;
    std::string callsError() const;
    bool canShareCalls() const;
    bool callAccess() const;
    bool canShareSms() const;
    bool smsAccess() const;
    std::string peerName(const std::string& id) const;

    void notice(const std::string& text) const;  // a message for the snackbar
    // Messages for a snackbar, and clipboard texts from other devices, since the last call.
    std::vector<std::string> takeNotices() const;
    std::vector<std::string> takeIncomingClipboard() const;
    bool incomingClipboardSecret() const;  // the latest of them is a password
    std::vector<ClipboardItem> clipboardHistory() const;
    // A paired device's name on this device (empty, or its own name: back to that); kept in
    // <data dir>/device-names.json.
    void renamePeer(const std::string& peerId, const std::string& name) const;
    std::vector<FileRecord> fileHistory() const;
    void forgetFileRecord(int64_t id) const;  // from the list (the "x")
    bool deleteFile(int64_t id) const;        // the file from the disk; its card stays, marked deleted
    void clearFileHistory() const;
    bool filesGrid() const;  // the Files tab as a grid (else a list); kept as a setting
    void setFilesGrid(bool grid) const;
    PhoneMedia phoneMedia() const;
    PhoneBattery phoneBattery(const std::string& peerId) const;  // level -1 when not known
    void findPhone(const std::string& peerId, bool ring) const;  // ring it (through silent mode), or stop
    // Files or clipboard text came from a device: a new count each time (the tray icon blinks).
    int receivedCount() const;
    // The tray panel asked for from outside (the Plasma panel widget): a new serial each time.
    struct PanelRequest { int serial = 0, x = 0, y = 0; bool operator==(const PanelRequest&) const = default; };
    PanelRequest panelRequest() const;
    void requestPhoneMedia() const;
    void phoneMediaControl(const std::string& action) const;  // play, pause, toggle, next, previous
    // This device's clipboard as the UI reads it (about once a second): a change goes to the
    // trusted devices that are online.
    // `sensitive`: the clipboard holds a password (its source marked it).
    void syncClipboard(const std::string& text, bool sensitive) const;
    // "Şifre gizliliği" (on by default): a copied password goes to trusted devices only after a yes.
    // Copied text goes to the trusted devices by itself (else only by hand: Pano buttons, Ctrl+V
    // in the panel); kept in <data dir>/clipboard-auto-send.
    bool clipboardAutoSend() const;
    void setClipboardAutoSend(bool on) const;
    bool passwordPrivacy() const;
    void setPasswordPrivacy(bool on) const;
    bool passwordPending() const;            // a copied password waits for that yes
    void answerPasswordSend(bool send) const;
    void dropPasswords() const;              // the history's passwords (the panel closed)
    void copiedClipboardItem(int64_t id) const;  // copied again from the list: a password then goes
    void setTrusted(const std::string& peerId, bool on) const;
    void removeClipboardItem(int64_t id) const;
    void clearClipboardHistory() const;

    // ---- Actions ----
    void pair(const std::string& peerId) const;
    void pairAddress(const std::string& address) const;
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
    void cancelTransfer(const std::string& transferId) const;  // either way; both sides see it end
    void setSharingPaused(bool paused) const;
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
    void answerCall() const;
    // Answers and moves the call's audio to this computer once it is active (Bluetooth HFP).
    void answerCallOnComputer() const;
    void rejectCall() const;
    void dismissCall() const;  // hides the alert; the phone goes on ringing
    void dial(const std::string& number) const;
    // To the phone of the call in progress: "call.hangup", "call.hold" {on}, "call.mute" {on},
    // "call.route" {route}, "call.volume" {delta}.
    void callControl(const std::string& kind, const std::string& bodyJson) const;
    bool canBridgeCallAudio() const;
    bool callAudioOn() const;
    std::string callAudioStatus() const;
    void startCallAudio() const;
    void stopCallAudio() const;
    bool audioAutoConnect() const;
    void setAudioAutoConnect(bool on) const;
    void shareCurrentCallAudio() const;
    void setPlayerAudioStatus(const std::string& status) const;
    bool mediaAudioEnabled() const;
    std::string mediaAudioStatus() const;
    std::string mediaAudioTarget() const;
    void chooseMediaAudioPlayers() const;
    void setMediaAudioTarget(const std::string& peerId) const;
    bool canPlayMediaAudio() const;
    void setMediaAudioEnabled(bool on) const;
    void startScreen(const std::string& peerId) const;
    void stopScreen() const;
    // A touch on the shared screen: {"type":"tap"|"long_press"|"swipe"|"back"|"home"|"recents", ...}
    void screenInput(const std::string& bodyJson) const;
    void screenWheel(double x, double y, double h, double v) const;  // wheel deltas over the fitted screen
    void openScreenControlSettings() const;
    // The viewer: asks a computer for its screen, stops, and sends a pointer or key
    // ({"type":"click","x":..,"y":..} and the like; docs/protocol.md desktop.input).
    void startDesktop(const std::string& peerId) const;
    void stopDesktop() const;
    void desktopInput(const std::string& bodyJson) const;
    // The shared computer: answers a request, or ends the share.
    void answerDesktop(bool accept) const;
    void endDesktopShare() const;
    bool screenShareAudio() const;
    bool pauseScreenAudioDuringMusic() const;
    void setPauseScreenAudioDuringMusic(bool on) const;
    void setScreenShareAudio(bool on) const;
    void screenShareAudioFailed() const;
    void setFlingEnabled(bool on) const;
    // A fling happened: files waiting to be shared go to flingTarget(), else the clipboard's text.
    bool fling(const std::string& clipboardText) const;
    // Fling from the background (phase 4B+): what was found on screen goes to the fling target.
    // Each returns the message the phone shows ("" when there was nothing to do).
    std::string flingPendingShare() const;
    std::string flingFiles(const std::vector<std::string>& paths) const;
    std::string flingText(const std::string& text) const;
    void startAdvancedSetup() const;
    void advancedPair(const std::string& code) const;  // the code typed into the setup notification
    void advancedTest() const;                         // runs `id` over wireless debugging
    void setAdvancedPort(const std::string& port) const;
    void startHelper(const std::string& command) const;  // the phone: starts the helper (AppStreams)
    void requestPhoneApps() const;
    void openPhoneApp(const PhoneApp& app) const;
    // A shortcut that opens the app in its own window, AirLink's staying hidden (the applications
    // menu, or the desktop); its result shows as a notice.
    bool canMakeAppShortcuts() const;
    void makeAppShortcut(const PhoneApp& app, bool onDesktop) const;
    void closeAppWindow(int session) const;
    void showAppWindow(int session) const;
    // Input for an open app: {"cmd":"touch","action":"down","x":..,"y":..} and the like (helper/Server.java).
    void appInput(int session, const std::string& bodyJson) const;
    // A wheel turn over an app's window (x, y: fractions; h, v: notches, Android's signs). Turns add
    // up and go to the phone once per pump, not one message each.
    void appWheel(int session, double x, double y, double h, double v) const;
    // This phone's frames (from any thread).
    void sendMedia(const std::string& peerId, const std::string& name, const std::vector<uint8_t>& frame) const;
    void stopMedia(const std::string& peerId, const std::string& name) const;
    void requestCallLog() const;
    void loadMoreCallLog() const;
    void requestContacts(const std::string& query) const;
    void loadMoreContacts() const;
    void requestCallAccess() const;
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
