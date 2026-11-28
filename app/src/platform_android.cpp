// Android: what MainActivity.java tells native code, and the calls back into it.

#include "platform.hpp"
#include "app_state.hpp"

#include <airlink.h>
#include <android/log.h>
#include <jni.h>

#include <mutex>
#include <filesystem>
#include <string>

namespace airlink::platform {

namespace {

std::mutex mutex;
std::optional<Config> started;
JavaVM* vm = nullptr;
jobject activity = nullptr;  // global ref
std::function<void(std::vector<std::string>)> pending;  // the picker's callback
std::function<void(std::vector<std::string>)> shareHandler;
std::vector<std::string> sharedEarly;  // shared before the handler was set
// AirLinkService and its static onPrompt, looked up on the UI thread: the core's threads, attached
// to the VM later, see only the system's classes through FindClass.
jclass serviceClass = nullptr;
jmethodID onPrompt = nullptr;
jclass listenerClass = nullptr;  // AirLinkNotificationListener
jmethodID listenerGranted = nullptr, listenerOpenSettings = nullptr, listenerAct = nullptr;
jclass smsClass = nullptr, activityClass = nullptr;  // SmsBridge, MainActivity
jmethodID smsHandle = nullptr, smsGranted = nullptr, smsRequestPermissions = nullptr;
jmethodID callsGranted = nullptr, callRequestPermissions = nullptr;
jmethodID screenControlOn = nullptr, screenControlSettings = nullptr;
jmethodID backgroundAutostartNeeded = nullptr, backgroundAutostartSettings = nullptr;
jmethodID startAtBootId = nullptr, setStartAtBootId = nullptr, batteryRestrictedId = nullptr, batteryExemptionId = nullptr;
jmethodID flingAvailableId = nullptr, flingEnabledId = nullptr, setFlingEnabledId = nullptr;
jmethodID flingMediaGrantedId = nullptr, requestFlingMediaId = nullptr;
jmethodID openFileId = nullptr, showInFolderId = nullptr, shareWithAppsId = nullptr, uriReadableId = nullptr;
jclass stagingClass = nullptr;
jmethodID stagingOriginId = nullptr, stagingReleaseId = nullptr;
jclass adbPairingClass = nullptr;
jmethodID adbStart = nullptr, adbDone = nullptr;
jclass appStreamsClass = nullptr;
jmethodID appStreamsHelperStarted = nullptr;
jmethodID appStreamsWatchClipboard = nullptr;
jclass updaterClass = nullptr;
jmethodID updaterInstall = nullptr;

std::string fromJava(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

// The calling thread's JNIEnv (the UI thread is attached already; others are attached here).
JNIEnv* env() {
    if (!vm) return nullptr;
    JNIEnv* e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_EDETACHED) vm->AttachCurrentThread(&e, nullptr);
    return e;
}

}  // namespace

std::optional<Config> config() {
    std::lock_guard lock(mutex);
    return started;
}

void openFolder(const std::string&) {}  // the tray is desktop-only

void pickFiles(std::function<void(std::vector<std::string>)> done) {
    JNIEnv* e = env();
    jobject a;
    {
        std::lock_guard lock(mutex);
        a = activity;
        if (!e || !a) return done({});
        pending = std::move(done);
    }
    jclass cls = e->GetObjectClass(a);
    e->CallVoidMethod(a, e->GetMethodID(cls, "pickFiles", "()V"));
    e->DeleteLocalRef(cls);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

namespace {
jobjectArray toJava(JNIEnv* e, const std::vector<std::string>& list) {
    jclass stringClass = e->FindClass("java/lang/String");
    jobjectArray array = e->NewObjectArray(jsize(list.size()), stringClass, nullptr);
    for (size_t i = 0; i < list.size(); ++i) {
        jstring s = e->NewStringUTF(list[i].c_str());
        e->SetObjectArrayElement(array, jsize(i), s);
        e->DeleteLocalRef(s);
    }
    e->DeleteLocalRef(stringClass);
    return array;
}
}  // namespace

void stageUris(const std::vector<std::string>& uris, std::function<void(std::vector<std::string>)> done) {
    JNIEnv* e = env();
    jobject a;
    {
        std::lock_guard lock(mutex);
        a = activity;
        if (!e || !a) return done({});
        pending = std::move(done);  // answered by nativeFilesPicked, as the picker is
    }
    jobjectArray array = toJava(e, uris);
    jclass cls = e->GetObjectClass(a);
    e->CallVoidMethod(a, e->GetMethodID(cls, "stageUris", "([Ljava/lang/String;)V"), array);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(cls);
    e->DeleteLocalRef(array);
}

void handleLaunch(int, char**) {}  // Android: shares come as intents (MainActivity)

std::optional<std::pair<int, int>> takePanelRequest() { return std::nullopt; }
std::vector<std::pair<std::string, std::string>> takePhoneAppRequests() { return {}; }
std::string updateKind() { return "android"; }
std::string applyUpdate(const std::string&, const std::vector<std::string>& files) {
    // (on the update's own thread: attached to the VM for the call)
    JNIEnv* e = nullptr;
    bool attached = false;
    if (!vm || !updaterClass || !updaterInstall || files.empty()) return "unsupported";
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return "no JNI";
        attached = true;
    }
    jstring path = e->NewStringUTF(files.front().c_str());
    auto result = static_cast<jstring>(e->CallStaticObjectMethod(updaterClass, updaterInstall, path));
    std::string error = e->ExceptionCheck() ? (e->ExceptionClear(), std::string("installer failed")) : result ? fromJava(e, result) : std::string();
    e->DeleteLocalRef(path);
    if (result) e->DeleteLocalRef(result);
    if (attached) vm->DetachCurrentThread();
    return error;
}
bool updateRestarts(const std::string&) { return false; }
void notifyUpdate(const std::string&) {}
bool canMakeAppShortcuts() { return false; }
std::string makePhoneAppShortcut(const std::string&, const std::string&, const std::string&, bool) { return "unsupported"; }
void publishStatus(const std::string&, const std::string&, int) {}

void setShareHandler(std::function<void(std::vector<std::string>)> handler) {
    std::vector<std::string> early;
    {
        std::lock_guard lock(mutex);
        shareHandler = handler;
        early.swap(sharedEarly);
    }
    if (!early.empty()) handler(std::move(early));
}

namespace {
// The core's prompt events, on its own thread: AirLinkService shows them as notifications while
// the app isn't on screen.
void promptHeard(void*, const char* json) {
    if (!vm || !serviceClass || !onPrompt) return;
    JNIEnv* e = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return;
        attached = true;  // detached again below: the core's threads aren't Java's
    }
    jstring s = e->NewStringUTF(json);
    e->CallStaticVoidMethod(serviceClass, onPrompt, s);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(s);
    if (attached) vm->DetachCurrentThread();
}
}  // namespace

void nodeStarted(void* node) {
    airlink_set_prompt_listener(static_cast<AirlinkNode*>(node), promptHeard, nullptr);
}

void log(const std::string& line) { __android_log_print(ANDROID_LOG_DEBUG, "AirLink", "%s", line.c_str()); }

bool canShareNotifications() { return true; }

bool notificationAccessGranted() {
    JNIEnv* e = env();
    if (!e || !listenerClass) return false;
    bool granted = e->CallStaticBooleanMethod(listenerClass, listenerGranted);
    if (e->ExceptionCheck()) e->ExceptionClear(), granted = false;
    return granted;
}

void openNotificationAccessSettings() {
    JNIEnv* e = env();
    if (!e || !listenerClass) return;
    e->CallStaticVoidMethod(listenerClass, listenerOpenSettings);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void notificationAction(const std::string& kind, const std::string& key, const std::string& text) {
    JNIEnv* e = env();
    if (!e || !listenerClass) return;
    jstring k = e->NewStringUTF(kind.c_str()), id = e->NewStringUTF(key.c_str()), t = e->NewStringUTF(text.c_str());
    e->CallStaticVoidMethod(listenerClass, listenerAct, k, id, t);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(k), e->DeleteLocalRef(id), e->DeleteLocalRef(t);
}

void showDesktopNotification(const std::string&, const std::string&) {}  // a phone shows its own

bool canUseAdvancedMode() { return true; }
bool canBridgeCallAudio() { return false; }
void startCallAudio(const std::string&) {}
void stopCallAudio() {}
CallAudioPhase callAudioPhase() { return CallAudioPhase::Off; }
std::string callAudioStatus() { return {}; }
bool canPlayMediaAudio() { return true; }
void playMediaAudio(const std::vector<uint8_t>& pcm) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "playDesktopAudio", "([B)V");
    if (method) {
        jbyteArray bytes = e->NewByteArray(static_cast<jsize>(pcm.size()));
        if (bytes) {
            e->SetByteArrayRegion(bytes, 0, static_cast<jsize>(pcm.size()),
                                  reinterpret_cast<const jbyte*>(pcm.data()));
            e->CallStaticVoidMethod(activityClass, method, bytes);
            e->DeleteLocalRef(bytes);
        }
    }
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void stopMediaAudio() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "stopDesktopAudio", "()V");
    if (method) e->CallStaticVoidMethod(activityClass, method);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void startDesktopAudioCapture() {}
void stopDesktopAudioCapture() {}
std::vector<uint8_t> takeDesktopAudio() { return {}; }
bool desktopAudioCaptureFailed() { return false; }

void stopPhoneScreenForPeer(const std::string& peerId) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "stopPhoneScreenForPeer", "(Ljava/lang/String;)V");
    jstring peer = e->NewStringUTF(peerId.c_str());
    if (method) e->CallStaticVoidMethod(activityClass, method, peer);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(peer);
}

void setPauseScreenAudioDuringMusic(bool enabled) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "setPauseScreenAudioDuringMusic", "(Z)V");
    if (method) e->CallStaticVoidMethod(activityClass, method, jboolean(enabled));
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void setScreenShareAudio(bool enabled) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "setScreenShareAudio", "(Z)V");
    if (method) e->CallStaticVoidMethod(activityClass, method, jboolean(enabled));
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void chooseMediaAudioPlayers() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jmethodID method = e->GetStaticMethodID(activityClass, "chooseMediaAudioPlayers", "()V");
    if (method) e->CallStaticVoidMethod(activityClass, method);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void setPlayerAudioTarget(const std::string& peerId) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    jstring peer = peerId.empty() ? nullptr : e->NewStringUTF(peerId.c_str());
    jmethodID method = e->GetStaticMethodID(activityClass, "setPlayerAudioTarget", "(Ljava/lang/String;)V");
    if (method) e->CallStaticVoidMethod(activityClass, method, peer);
    if (e->ExceptionCheck()) e->ExceptionClear();
    if (peer) e->DeleteLocalRef(peer);
}

void setPlayerBluetoothPhone(const std::string&) {}
std::string playerBluetoothStatus() { return {}; }

bool canStartAtLogin() { return true; }

bool startAtLogin() {
    JNIEnv* e = env();
    if (!e || !activityClass || !startAtBootId) return false;
    bool on = e->CallStaticBooleanMethod(activityClass, startAtBootId);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}

void setStartAtLogin(bool on) {
    JNIEnv* e = env();
    if (!e || !activityClass || !setStartAtBootId) return;
    e->CallStaticVoidMethod(activityClass, setStartAtBootId, jboolean(on));
    if (e->ExceptionCheck()) e->ExceptionClear();
}

bool launchedAtLogin() { return false; }

bool batteryRestricted() {
    JNIEnv* e = env();
    if (!e || !activityClass || !batteryRestrictedId) return false;
    bool restricted = e->CallStaticBooleanMethod(activityClass, batteryRestrictedId);
    if (e->ExceptionCheck()) e->ExceptionClear(), restricted = false;
    return restricted;
}

void requestBatteryExemption() {
    JNIEnv* e = env();
    if (!e || !activityClass || !batteryExemptionId) return;
    e->CallStaticVoidMethod(activityClass, batteryExemptionId);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

bool needsBackgroundAutostartSettings() {
    JNIEnv* e = env();
    if (!e || !activityClass || !backgroundAutostartNeeded) return false;
    bool needed = e->CallStaticBooleanMethod(activityClass, backgroundAutostartNeeded);
    if (e->ExceptionCheck()) e->ExceptionClear(), needed = false;
    return needed;
}

void openBackgroundAutostartSettings() {
    JNIEnv* e = env();
    if (!e || !activityClass || !backgroundAutostartSettings) return;
    e->CallStaticVoidMethod(activityClass, backgroundAutostartSettings);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

bool canFling() {
    JNIEnv* e = env();
    if (!e || !activityClass || !flingAvailableId) return false;
    bool on = e->CallStaticBooleanMethod(activityClass, flingAvailableId);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}

bool flingEnabled() {
    JNIEnv* e = env();
    if (!e || !activityClass || !flingEnabledId) return false;
    bool on = e->CallStaticBooleanMethod(activityClass, flingEnabledId);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}

namespace {
void callWithPath(jmethodID method, const std::string& path) {
    JNIEnv* e = env();
    if (!e || !activityClass || !method) return;
    jstring p = e->NewStringUTF(path.c_str());
    e->CallStaticVoidMethod(activityClass, method, p);
    e->DeleteLocalRef(p);
    if (e->ExceptionCheck()) e->ExceptionClear();
}
}  // namespace

void openFile(const std::string& path) { callWithPath(openFileId, path); }
void openUrl(const std::string& url) { callWithPath(openFileId, url); }

std::string systemLanguage() {
    // java.util.Locale.getDefault().getLanguage(): "tr", "en"…
    JNIEnv* e = env();
    if (!e) return {};
    std::string language;
    jclass locale = e->FindClass("java/util/Locale");
    jmethodID getDefault = locale ? e->GetStaticMethodID(locale, "getDefault", "()Ljava/util/Locale;") : nullptr;
    jmethodID getLanguage = locale ? e->GetMethodID(locale, "getLanguage", "()Ljava/lang/String;") : nullptr;
    if (getDefault && getLanguage) {
        jobject current = e->CallStaticObjectMethod(locale, getDefault);
        if (current) {
            auto code = static_cast<jstring>(e->CallObjectMethod(current, getLanguage));
            if (code) language = fromJava(e, code);
        }
    }
    if (e->ExceptionCheck()) e->ExceptionClear();
    return language;
}

void setAppForeground(bool) {}  // (Android: MainActivity knows)

bool clipboardMarkedSensitive() {
    JNIEnv* e = env();
    if (!e || !activityClass) return false;
    static jmethodID method = e->GetStaticMethodID(activityClass, "clipboardSensitive", "()Z");
    if (!method) {
        if (e->ExceptionCheck()) e->ExceptionClear();
        return false;
    }
    bool on = e->CallStaticBooleanMethod(activityClass, method);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}

void askSendPassword(std::function<void(bool)>) {
    // (the app's dialog and this notification answer through App::answerPasswordSend)
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    static jmethodID method = e->GetStaticMethodID(activityClass, "askSendPassword", "()V");
    if (method) e->CallStaticVoidMethod(activityClass, method);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void passwordAnswered() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    static jmethodID method = e->GetStaticMethodID(activityClass, "passwordAnswered", "()V");
    if (method) e->CallStaticVoidMethod(activityClass, method);
    if (e->ExceptionCheck()) e->ExceptionClear();
}  // (the app's dialog asks: Android reads the clipboard only in front)

std::vector<std::pair<std::string, std::string>> takeTrayNotifications() { return {}; }

void notifyReceived(const std::string& from, const std::vector<std::string>& files, const std::string& text, bool secret) {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    static jmethodID method = e->GetStaticMethodID(activityClass, "notifyReceived", "(Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;Z)V");
    if (!method) {
        if (e->ExceptionCheck()) e->ExceptionClear();
        return;
    }
    jobjectArray list = toJava(e, files);
    jstring f = e->NewStringUTF(from.c_str()), t = e->NewStringUTF(text.c_str());
    e->CallStaticVoidMethod(activityClass, method, f, list, t, jboolean(secret));
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(f), e->DeleteLocalRef(t), e->DeleteLocalRef(list);
}

bool fileExists(const std::string& path) {
    if (!path.starts_with("content://")) {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::path(path), ec);
    }
    JNIEnv* e = env();
    if (!e || !activityClass || !uriReadableId) return false;
    jstring p = e->NewStringUTF(path.c_str());
    bool ok = e->CallStaticBooleanMethod(activityClass, uriReadableId, p);
    e->DeleteLocalRef(p);
    if (e->ExceptionCheck()) e->ExceptionClear(), ok = false;
    return ok;
}

std::pair<std::string, std::string> takeSentOriginal(const std::string& path) {
    JNIEnv* e = env();
    if (!e || !stagingClass || !stagingOriginId || !stagingReleaseId) return {};
    jstring p = e->NewStringUTF(path.c_str());
    auto origin = static_cast<jstring>(e->CallStaticObjectMethod(stagingClass, stagingOriginId, p));
    std::string text = origin ? fromJava(e, origin) : std::string();
    if (origin) e->DeleteLocalRef(origin);
    if (e->ExceptionCheck()) e->ExceptionClear();
    if (!text.empty()) e->CallStaticVoidMethod(stagingClass, stagingReleaseId, p);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(p);
    auto newline = text.find('\n');
    if (newline == std::string::npos) return {};
    return {text.substr(0, newline), text.substr(newline + 1)};
}
void showInFolder(const std::string& path) { callWithPath(showInFolderId, path); }
bool shareWithApps(const std::string& path) {
    JNIEnv* e = env();
    if (!e || !activityClass || !shareWithAppsId) return false;
    jstring p = e->NewStringUTF(path.c_str());
    bool ok = e->CallStaticBooleanMethod(activityClass, shareWithAppsId, p);
    e->DeleteLocalRef(p);
    if (e->ExceptionCheck()) e->ExceptionClear(), ok = false;
    return ok;
}

bool flingMediaAccess() {
    JNIEnv* e = env();
    if (!e || !activityClass || !flingMediaGrantedId) return false;
    bool on = e->CallStaticBooleanMethod(activityClass, flingMediaGrantedId);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}
void requestFlingMediaAccess() {
    JNIEnv* e = env();
    if (!e || !activityClass || !requestFlingMediaId) return;
    e->CallStaticVoidMethod(activityClass, requestFlingMediaId);
    if (e->ExceptionCheck()) e->ExceptionClear();
}
void setFlingEnabled(bool on) {
    JNIEnv* e = env();
    if (!e || !activityClass || !setFlingEnabledId) return;
    e->CallStaticVoidMethod(activityClass, setFlingEnabledId, jboolean(on));
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void startAdvancedSetup() {
    JNIEnv* e = env();
    if (!e || !adbPairingClass) return;
    e->CallStaticVoidMethod(adbPairingClass, adbStart);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void advancedSetupResult(bool ok, const std::string& message) {
    JNIEnv* e = env();
    if (!e || !adbPairingClass) return;
    jstring m = e->NewStringUTF(message.c_str());
    e->CallStaticVoidMethod(adbPairingClass, adbDone, jboolean(ok), m);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(m);
}

void watchClipboardInBackground() {
    JNIEnv* e = env();
    if (!e || !appStreamsClass || !appStreamsWatchClipboard) return;
    e->CallStaticVoidMethod(appStreamsClass, appStreamsWatchClipboard);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void helperStarted(bool ok, const std::string& message) {
    JNIEnv* e = env();
    if (!e || !appStreamsClass) return;
    jstring m = e->NewStringUTF(message.c_str());
    e->CallStaticVoidMethod(appStreamsClass, appStreamsHelperStarted, jboolean(ok), m);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(m);
}

bool canShareScreen() { return true; }

bool screenControlEnabled() {
    JNIEnv* e = env();
    if (!e || !activityClass) return false;
    bool on = e->CallStaticBooleanMethod(activityClass, screenControlOn);
    if (e->ExceptionCheck()) e->ExceptionClear(), on = false;
    return on;
}

void openScreenControlSettings() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    e->CallStaticVoidMethod(activityClass, screenControlSettings);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

bool canShareCalls() { return true; }

bool callAccessGranted() {
    JNIEnv* e = env();
    if (!e || !activityClass) return false;
    bool granted = e->CallStaticBooleanMethod(activityClass, callsGranted);
    if (e->ExceptionCheck()) e->ExceptionClear(), granted = false;
    return granted;
}

void requestCallAccess() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    e->CallStaticVoidMethod(activityClass, callRequestPermissions);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

bool canShareSms() { return true; }

bool smsAccessGranted() {
    JNIEnv* e = env();
    if (!e || !activityClass) return false;
    bool granted = e->CallStaticBooleanMethod(activityClass, smsGranted);
    if (e->ExceptionCheck()) e->ExceptionClear(), granted = false;
    return granted;
}

void requestSmsAccess() {
    JNIEnv* e = env();
    if (!e || !activityClass) return;
    e->CallStaticVoidMethod(activityClass, smsRequestPermissions);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void smsRequest(const std::string& peerId, const std::string& kind, const std::string& bodyJson) {
    JNIEnv* e = env();
    if (!e || !smsClass) return;
    jstring p = e->NewStringUTF(peerId.c_str()), k = e->NewStringUTF(kind.c_str()), b = e->NewStringUTF(bodyJson.c_str());
    e->CallStaticVoidMethod(smsClass, smsHandle, p, k, b);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(p), e->DeleteLocalRef(k), e->DeleteLocalRef(b);
}

void filesReceived(const std::vector<std::string>& paths) {
    JNIEnv* e = env();
    jobject a;
    {
        std::lock_guard lock(mutex);
        a = activity;
    }
    if (!e || !a || paths.empty()) return;
    jclass stringClass = e->FindClass("java/lang/String");
    jobjectArray array = e->NewObjectArray(jsize(paths.size()), stringClass, nullptr);
    for (size_t i = 0; i < paths.size(); ++i) {
        jstring s = e->NewStringUTF(paths[i].c_str());
        e->SetObjectArrayElement(array, jsize(i), s);
        e->DeleteLocalRef(s);
    }
    jclass cls = e->GetObjectClass(a);
    e->CallVoidMethod(a, e->GetMethodID(cls, "scanFiles", "([Ljava/lang/String;)V"), array);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(cls);
    e->DeleteLocalRef(array);
    e->DeleteLocalRef(stringClass);
}

}  // namespace airlink::platform

using namespace airlink::platform;

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeInit(
    JNIEnv* e, jclass, jobject activityObject, jstring filesDir, jstring downloadDir, jstring deviceName) {
    std::lock_guard lock(mutex);
    e->GetJavaVM(&vm);
    if (activityObject) {
        if (activity) e->DeleteGlobalRef(activity);
        activity = e->NewGlobalRef(activityObject);
    }
    started = Config{.dataDir = fromJava(e, filesDir), .downloadDir = fromJava(e, downloadDir), .fallbackName = fromJava(e, deviceName)};
    if (!serviceClass) {
        jclass local = e->FindClass("com/plyg/airlink/AirLinkService");
        serviceClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        onPrompt = e->GetStaticMethodID(serviceClass, "onPrompt", "(Ljava/lang/String;)V");
    }
    if (!listenerClass) {
        jclass local = e->FindClass("com/plyg/airlink/AirLinkNotificationListener");
        listenerClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        listenerGranted = e->GetStaticMethodID(listenerClass, "accessGranted", "()Z");
        listenerOpenSettings = e->GetStaticMethodID(listenerClass, "openAccessSettings", "()V");
        listenerAct = e->GetStaticMethodID(listenerClass, "act", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    }
    if (!smsClass) {
        jclass local = e->FindClass("com/plyg/airlink/SmsBridge");
        smsClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        smsHandle = e->GetStaticMethodID(smsClass, "handle", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        local = e->FindClass("com/plyg/airlink/MainActivity");
        activityClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        smsGranted = e->GetStaticMethodID(activityClass, "smsGranted", "()Z");
        smsRequestPermissions = e->GetStaticMethodID(activityClass, "requestSmsPermissions", "()V");
        flingAvailableId = e->GetStaticMethodID(activityClass, "flingAvailable", "()Z");
        flingEnabledId = e->GetStaticMethodID(activityClass, "flingEnabled", "()Z");
        setFlingEnabledId = e->GetStaticMethodID(activityClass, "setFlingEnabled", "(Z)V");
        flingMediaGrantedId = e->GetStaticMethodID(activityClass, "flingMediaGranted", "()Z");
        requestFlingMediaId = e->GetStaticMethodID(activityClass, "requestFlingMedia", "()V");
        openFileId = e->GetStaticMethodID(activityClass, "openFile", "(Ljava/lang/String;)V");
        showInFolderId = e->GetStaticMethodID(activityClass, "showInFolder", "(Ljava/lang/String;)V");
        shareWithAppsId = e->GetStaticMethodID(activityClass, "shareWithApps", "(Ljava/lang/String;)Z");
        uriReadableId = e->GetStaticMethodID(activityClass, "uriReadable", "(Ljava/lang/String;)Z");
        local = e->FindClass("com/plyg/airlink/Staging");
        stagingClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        stagingOriginId = e->GetStaticMethodID(stagingClass, "originOf", "(Ljava/lang/String;)Ljava/lang/String;");
        stagingReleaseId = e->GetStaticMethodID(stagingClass, "release", "(Ljava/lang/String;)V");
        callsGranted = e->GetStaticMethodID(activityClass, "callsGranted", "()Z");
        callRequestPermissions = e->GetStaticMethodID(activityClass, "requestCallPermissions", "()V");
        screenControlOn = e->GetStaticMethodID(activityClass, "screenControlEnabled", "()Z");
        screenControlSettings = e->GetStaticMethodID(activityClass, "openScreenControlSettings", "()V");
        backgroundAutostartNeeded = e->GetStaticMethodID(activityClass, "needsBackgroundAutostartSettings", "()Z");
        backgroundAutostartSettings = e->GetStaticMethodID(activityClass, "openBackgroundAutostartSettings", "()V");
        startAtBootId = e->GetStaticMethodID(activityClass, "startAtBoot", "()Z");
        setStartAtBootId = e->GetStaticMethodID(activityClass, "setStartAtBoot", "(Z)V");
        batteryRestrictedId = e->GetStaticMethodID(activityClass, "batteryRestricted", "()Z");
        batteryExemptionId = e->GetStaticMethodID(activityClass, "requestBatteryExemption", "()V");
        local = e->FindClass("com/plyg/airlink/AdbPairing");
        adbPairingClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        adbStart = e->GetStaticMethodID(adbPairingClass, "start", "()V");
        adbDone = e->GetStaticMethodID(adbPairingClass, "done", "(ZLjava/lang/String;)V");
        local = e->FindClass("com/plyg/airlink/AppStreams");
        appStreamsClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        appStreamsHelperStarted = e->GetStaticMethodID(appStreamsClass, "helperStarted", "(ZLjava/lang/String;)V");
        appStreamsWatchClipboard = e->GetStaticMethodID(appStreamsClass, "watchClipboard", "()V");
        local = e->FindClass("com/plyg/airlink/Updater");
        updaterClass = static_cast<jclass>(e->NewGlobalRef(local));
        e->DeleteLocalRef(local);
        updaterInstall = e->GetStaticMethodID(updaterClass, "install", "(Ljava/lang/String;)Ljava/lang/String;");
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeDetachActivity(JNIEnv* e, jclass, jobject oldActivity) {
    std::function<void(std::vector<std::string>)> done;
    {
        std::lock_guard lock(mutex);
        if (activity && e->IsSameObject(activity, oldActivity)) {
            e->DeleteGlobalRef(activity);
            activity = nullptr;
            done = std::move(pending);
            pending = {};
        }
    }
    if (done) done({});
}

namespace {
std::vector<std::string> pathsOf(JNIEnv* e, jobjectArray paths) {
    std::vector<std::string> list;
    jsize n = paths ? e->GetArrayLength(paths) : 0;
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(e->GetObjectArrayElement(paths, i));
        list.push_back(fromJava(e, s));
        e->DeleteLocalRef(s);
    }
    return list;
}
}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeShared(JNIEnv* e, jclass, jobjectArray paths) {
    std::vector<std::string> list = pathsOf(e, paths);
    if (list.empty()) return;
    std::function<void(std::vector<std::string>)> handler;
    {
        std::lock_guard lock(mutex);
        if (!shareHandler) {
            sharedEarly.insert(sharedEarly.end(), list.begin(), list.end());
            return;
        }
        handler = shareHandler;
    }
    handler(std::move(list));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_AirLinkNotificationListener_nativeBroadcast(JNIEnv* e, jclass, jstring kind,
                                                                                                    jstring body) {
    airlink::App::get().broadcast(fromJava(e, kind), fromJava(e, body));
}

// A copy the shell helper saw (on the main thread): sent on as one read in front would be.
extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_AppStreams_nativeClipboard(JNIEnv* e, jclass, jstring text, jboolean sensitive) {
    airlink::App::get().syncClipboard(fromJava(e, text), sensitive);
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeStartHelper(JNIEnv* e, jclass, jstring command) {
    airlink::App::get().startHelper(fromJava(e, command));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_AppStreams_nativeSendFrame(JNIEnv* e, jclass, jstring peer, jstring name,
                                                                                     jbyteArray jpeg) {
    jsize n = e->GetArrayLength(jpeg);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    e->GetByteArrayRegion(jpeg, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    airlink::App::get().sendMedia(fromJava(e, peer), fromJava(e, name), bytes);
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_AppStreams_nativeStopFrames(JNIEnv* e, jclass, jstring peer, jstring name) {
    airlink::App::get().stopMedia(fromJava(e, peer), fromJava(e, name));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeAdbPair(JNIEnv* e, jclass, jstring code) {
    airlink::App::get().advancedPair(fromJava(e, code));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativePump(JNIEnv*, jclass) {
    airlink::App::get().pump();  // on the main (UI) thread, as the UI loop pumps
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_ScreenShare_nativeSendFrame(JNIEnv* e, jclass, jstring peer, jbyteArray jpeg) {
    jsize n = e->GetArrayLength(jpeg);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    e->GetByteArrayRegion(jpeg, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    airlink::App::get().sendMedia(fromJava(e, peer), "screen", bytes);
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_ScreenShare_nativeStopFrames(JNIEnv* e, jclass, jstring peer) {
    airlink::App::get().stopMedia(fromJava(e, peer), "screen");
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_ScreenShare_nativeSendAudio(JNIEnv* e, jclass, jstring peer, jbyteArray pcm) {
    jsize n = e->GetArrayLength(pcm);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    e->GetByteArrayRegion(pcm, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    airlink::App::get().sendMedia(fromJava(e, peer), "screen_audio", bytes);
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_ScreenShare_nativeStopAudio(JNIEnv* e, jclass, jstring peer) {
    airlink::App::get().stopMedia(fromJava(e, peer), "screen_audio");
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_ScreenShare_nativeScreenAudioFailed(JNIEnv*, jclass) {
    airlink::App::get().screenShareAudioFailed();
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_PlayerBluetoothAudio_nativeStatus(JNIEnv* e, jclass, jstring status) {
    airlink::App::get().setPlayerAudioStatus(fromJava(e, status));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_SmsBridge_nativeSendTo(JNIEnv* e, jclass, jstring peer, jstring kind, jstring body) {
    airlink::App::get().sendTo(fromJava(e, peer), fromJava(e, kind), fromJava(e, body));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_SmsBridge_nativeBroadcast(JNIEnv* e, jclass, jstring kind, jstring body) {
    airlink::App::get().broadcast(fromJava(e, kind), fromJava(e, body));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeAnswerPassword(JNIEnv*, jclass, jboolean send) {
    airlink::App::get().answerPasswordSend(send);  // (a notification's action: on the main thread)
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeRespond(JNIEnv*, jclass, jlong requestId, jboolean accept) {
    airlink::App::get().respond(uint64_t(requestId), accept);
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_MainActivity_nativeFilesPicked(JNIEnv* e, jclass, jobjectArray paths) {
    std::vector<std::string> list = pathsOf(e, paths);
    std::function<void(std::vector<std::string>)> done;
    {
        std::lock_guard lock(mutex);
        done = std::move(pending);
        pending = nullptr;
    }
    if (done) done(std::move(list));
}

extern "C" JNIEXPORT void JNICALL Java_com_plyg_airlink_FlingDetector_nativeFlung(JNIEnv* e, jclass, jstring clipboardText) {
    airlink::App::get().fling(fromJava(e, clipboardText));  // on the main thread (the sensor's)
}

// Background fling (ForegroundContent): on the main thread; each returns the message to show.
extern "C" JNIEXPORT jstring JNICALL Java_com_plyg_airlink_ForegroundContent_nativeFlingPendingShare(JNIEnv* e, jclass) {
    return e->NewStringUTF(airlink::App::get().flingPendingShare().c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_plyg_airlink_ForegroundContent_nativeFlingFiles(JNIEnv* e, jclass, jobjectArray paths) {
    return e->NewStringUTF(airlink::App::get().flingFiles(pathsOf(e, paths)).c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_plyg_airlink_ForegroundContent_nativeFlingText(JNIEnv* e, jclass, jstring text) {
    return e->NewStringUTF(airlink::App::get().flingText(fromJava(e, text)).c_str());
}
