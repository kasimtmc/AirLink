#pragma once

// What differs between the desktop and Android builds.

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace airlink::platform {

struct Config {
    std::string dataDir;       // empty: the core's per-user default
    std::string downloadDir;   // empty: Downloads/AirLink
    std::string fallbackName;  // used when no device name was saved before
};

// The start configuration, once the platform knows it (on Android the activity tells it).
std::optional<Config> config();

// Lets the user choose files to send. `done` gets local paths (empty if cancelled); it may be
// called on any thread.
void pickFiles(std::function<void(std::vector<std::string>)> done);

// Open a local folder in the platform file manager (used by the desktop tray).
void openFolder(const std::string& path);

// Copies URIs that aren't local files (Android's content://) to local files; `done` gets their
// paths, maybe on another thread.
void stageUris(const std::vector<std::string>& uris, std::function<void(std::vector<std::string>)> done);

// Handles the command line (desktop): `airlink --send FILE...` or `airlink FILE...` from a file
// manager's menu. If AirLink already runs, the files go to it and this process exits; otherwise
// they are kept for the share handler and this instance serves later launches.
void handleLaunch(int argc, char** argv);

// Files handed to the app from outside (a file manager's menu, Android's share sheet, the command
// line). Files that came before a handler was set are given to it at once. May be called on any thread.
void setShareHandler(std::function<void(std::vector<std::string>)> handler);

// The core started: Android hooks its background notifications to it. `node` is an AirlinkNode*.
void nodeStarted(void* node);

// ---- Phone notifications on the computer --------------------------------------------------

// Whether this device can share its notifications (Android), and whether the user allowed it
// (Settings > Notification access).
bool canShareNotifications();
bool notificationAccessGranted();
void openNotificationAccessSettings();

// What a paired computer asked of one of this phone's notifications: kind "notification.dismiss"
// or "notification.reply" (with `text`).
void notificationAction(const std::string& kind, const std::string& key, const std::string& text);

// Shows a notification of this computer's desktop (a phone's notification mirrored here).
void showDesktopNotification(const std::string& title, const std::string& body);

// ---- SMS (a phone's, used from a paired computer) ------------------------------------------

bool canShareSms();
bool smsAccessGranted();
void requestSmsAccess();
// A computer's "sms.*", "contacts.*", "call.*" or "calls.*" request to this phone (body: JSON);
// answered with App::sendTo.
void smsRequest(const std::string& peerId, const std::string& kind, const std::string& bodyJson);

// ---- Calls (a phone's, seen and handled from a paired computer) -----------------------------

bool canShareCalls();
bool callAccessGranted();
void requestCallAccess();

// ---- Screen sharing (a phone's screen on a paired computer) ---------------------------------

bool canShareScreen();
bool screenControlEnabled();  // the phone carries out a computer's touches (Accessibility on)
void openScreenControlSettings();

// OEMs may block the restart of a sticky foreground service after its process dies.
bool needsBackgroundAutostartSettings();
void openBackgroundAutostartSettings();

// ---- Fling to send (phase 4B, Android): push the phone toward the computer -----------------

bool canFling();      // the phone has a linear acceleration sensor
bool flingEnabled();
void setFlingEnabled(bool on);
// Fling from another app: reading the photos, videos and songs (to find what is on screen).
bool flingMediaAccess();
void requestFlingMediaAccess();


// ---- Advanced mode (Android): the phone's own wireless debugging ----------------------------

bool canUseAdvancedMode();
// Opens Developer options and posts the notification the pairing code is typed into.
void startAdvancedSetup();
// The pairing's outcome, for that notification.
void advancedSetupResult(bool ok, const std::string& message);
// The command that starts the helper ran (or not).
void helperStarted(bool ok, const std::string& message);

// Linux HFP Hands-Free audio bridge. Only active for an explicit call and user action.
enum class CallAudioPhase { Off, Connecting, WaitingForRoute, Active, Error };
bool canBridgeCallAudio();
void startCallAudio(const std::string& phoneName);
void stopCallAudio();
CallAudioPhase callAudioPhase();
std::string callAudioStatus();
// PCM media playback is separate from the HFP call path.
bool canPlayMediaAudio();
void playMediaAudio(const std::vector<uint8_t>& pcm);
void stopMediaAudio();
// Android is the source of phone media audio; these control its local projection.
void startDesktopAudioCapture();
void stopDesktopAudioCapture();
std::vector<uint8_t> takeDesktopAudio();
bool desktopAudioCaptureFailed();
void stopPhoneScreenForPeer(const std::string& peerId);
void setPauseScreenAudioDuringMusic(bool enabled);
void setScreenShareAudio(bool enabled);
void chooseMediaAudioPlayers();
void setPlayerAudioTarget(const std::string& peerId);
void setPlayerBluetoothPhone(const std::string& phoneName);
std::string playerBluetoothStatus();

// A line in the platform's log (Android's logcat, stderr on the desktop).
void log(const std::string& line);

// Files that arrived (Android makes them visible to the gallery and file apps).
void filesReceived(const std::vector<std::string>& paths);

}  // namespace airlink::platform
