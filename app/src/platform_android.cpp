// Android: what MainActivity.java tells native code, and the calls back into it.

#include "platform.hpp"

#include <jni.h>

#include <mutex>
#include <string>

namespace airlink::platform {

namespace {

std::mutex mutex;
std::optional<Config> started;
JavaVM* vm = nullptr;
jobject activity = nullptr;  // global ref
std::function<void(std::vector<std::string>)> pending;  // the picker's callback

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

extern "C" JNIEXPORT void JNICALL Java_dev_airlink_airlink_MainActivity_nativeInit(
    JNIEnv* e, jclass, jobject activityObject, jstring filesDir, jstring downloadDir, jstring deviceName) {
    std::lock_guard lock(mutex);
    e->GetJavaVM(&vm);
    if (activity) e->DeleteGlobalRef(activity);
    activity = e->NewGlobalRef(activityObject);
    started = Config{.dataDir = fromJava(e, filesDir), .downloadDir = fromJava(e, downloadDir), .fallbackName = fromJava(e, deviceName)};
}

extern "C" JNIEXPORT void JNICALL Java_dev_airlink_airlink_MainActivity_nativeFilesPicked(JNIEnv* e, jclass, jobjectArray paths) {
    std::vector<std::string> list;
    jsize n = paths ? e->GetArrayLength(paths) : 0;
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(e->GetObjectArrayElement(paths, i));
        list.push_back(fromJava(e, s));
        e->DeleteLocalRef(s);
    }
    std::function<void(std::vector<std::string>)> done;
    {
        std::lock_guard lock(mutex);
        done = std::move(pending);
        pending = nullptr;
    }
    if (done) done(std::move(list));
}
