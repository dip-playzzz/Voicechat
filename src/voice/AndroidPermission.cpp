#include "voice/AndroidPermission.h"

#include <dlfcn.h>
#include <jni.h>

namespace voicechat {

namespace {

JavaVM *g_vm = nullptr;
constexpr const char *kMic = "android.permission.RECORD_AUDIO";

JavaVM *findVm() {
    if (g_vm) return g_vm; // set by JNI_OnLoad if the loader calls it
    using Fn = jint (*)(JavaVM **, jsize, jsize *);
    Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn) {
        if (void *h = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD))
            fn = reinterpret_cast<Fn>(dlsym(h, "JNI_GetCreatedJavaVMs"));
    }
    if (fn) {
        JavaVM *vm = nullptr;
        jsize n = 0;
        if (fn(&vm, 1, &n) == JNI_OK && n > 0) g_vm = vm;
    }
    return g_vm;
}

struct ScopedEnv {
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;
    bool attached = false;
    ScopedEnv() {
        vm = findVm();
        if (!vm) return;
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return;
        env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
        else env = nullptr;
    }
    ~ScopedEnv() { if (attached) vm->DetachCurrentThread(); }
};

// Returns true (and clears it) if a Java exception is pending.
bool pending(JNIEnv *env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

std::string toStd(JNIEnv *env, jstring s) {
    if (!s) return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

jobject currentApplication(JNIEnv *env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    if (!at || pending(env)) return nullptr;
    jmethodID m = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
    if (!m || pending(env)) return nullptr;
    jobject app = env->CallStaticObjectMethod(at, m);
    if (pending(env)) return nullptr;
    return app;
}

} // namespace

MicStatus queryMicPermission() {
    MicStatus st;
    ScopedEnv se;
    JNIEnv *env = se.env;
    if (!env) { st.error = "no JavaVM available"; return st; }
    env->PushLocalFrame(64);

    jobject app = currentApplication(env);
    if (!app) { st.error = "Application not available yet"; env->PopLocalFrame(nullptr); return st; }

    jclass ctx = env->FindClass("android/content/Context");
    jmethodID getPkg = env->GetMethodID(ctx, "getPackageName", "()Ljava/lang/String;");
    jmethodID check = env->GetMethodID(ctx, "checkSelfPermission", "(Ljava/lang/String;)I");
    jmethodID getPm = env->GetMethodID(ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    if (pending(env) || !getPkg || !check || !getPm) {
        st.error = "Context methods not found";
        env->PopLocalFrame(nullptr);
        return st;
    }

    jstring pkg = static_cast<jstring>(env->CallObjectMethod(app, getPkg));
    if (!pending(env)) st.package = toStd(env, pkg);

    jstring perm = env->NewStringUTF(kMic);
    const jint r = env->CallIntMethod(app, check, perm);
    st.granted = !pending(env) && r == 0; // PackageManager.PERMISSION_GRANTED == 0

    // Is RECORD_AUDIO listed in the app's manifest? (GET_PERMISSIONS == 0x1000)
    jobject pm = env->CallObjectMethod(app, getPm);
    if (!pending(env) && pm && pkg) {
        jclass pmc = env->FindClass("android/content/pm/PackageManager");
        jmethodID getPi = env->GetMethodID(pmc, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
        if (!pending(env) && getPi) {
            jobject pi = env->CallObjectMethod(pm, getPi, pkg, static_cast<jint>(0x1000));
            if (!pending(env) && pi) {
                jclass pic = env->FindClass("android/content/pm/PackageInfo");
                jfieldID f = env->GetFieldID(pic, "requestedPermissions", "[Ljava/lang/String;");
                if (!pending(env) && f) {
                    auto arr = static_cast<jobjectArray>(env->GetObjectField(pi, f));
                    if (arr) {
                        const jsize n = env->GetArrayLength(arr);
                        for (jsize i = 0; i < n && !st.declared; i++) {
                            auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
                            if (toStd(env, s) == kMic) st.declared = true;
                            env->DeleteLocalRef(s);
                        }
                    }
                }
            }
        }
    }

    st.ok = true;
    env->PopLocalFrame(nullptr);
    return st;
}

std::string requestMicPermission() {
    ScopedEnv se;
    JNIEnv *env = se.env;
    if (!env) return "no JavaVM";
    env->PushLocalFrame(64);
    auto fail = [&](const char *why) { env->PopLocalFrame(nullptr); return std::string(why); };

    // Find the foreground Activity through ActivityThread.mActivities (hidden API; may be blocked).
    jclass atc = env->FindClass("android/app/ActivityThread");
    if (!atc || pending(env)) return fail("ActivityThread not found");
    jmethodID cur = env->GetStaticMethodID(atc, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!cur || pending(env)) return fail("currentActivityThread not found");
    jobject at = env->CallStaticObjectMethod(atc, cur);
    if (pending(env) || !at) return fail("no ActivityThread instance");

    jfieldID fMap = env->GetFieldID(atc, "mActivities", "Landroid/util/ArrayMap;");
    if (!fMap || pending(env)) return fail("mActivities not accessible (hidden API blocked?)");
    jobject map = env->GetObjectField(at, fMap);
    if (!map) return fail("no activities");

    jclass mapc = env->FindClass("android/util/ArrayMap");
    jmethodID sizeM = env->GetMethodID(mapc, "size", "()I");
    jmethodID valueAt = env->GetMethodID(mapc, "valueAt", "(I)Ljava/lang/Object;");
    jclass recc = env->FindClass("android/app/ActivityThread$ActivityClientRecord");
    if (pending(env) || !sizeM || !valueAt || !recc) return fail("ArrayMap/ActivityClientRecord not found");
    jfieldID fAct = env->GetFieldID(recc, "activity", "Landroid/app/Activity;");
    jfieldID fPaused = env->GetFieldID(recc, "paused", "Z");
    if (pending(env) || !fAct || !fPaused) return fail("ActivityClientRecord fields not accessible");

    jobject activity = nullptr;
    const jint n = env->CallIntMethod(map, sizeM);
    for (jint i = 0; i < n && !pending(env); i++) {
        jobject rec = env->CallObjectMethod(map, valueAt, i);
        if (pending(env) || !rec) continue;
        jobject a = env->GetObjectField(rec, fAct);
        if (!a) continue;
        if (!activity) activity = a; // fallback: first activity
        if (!env->GetBooleanField(rec, fPaused)) { activity = a; break; } // prefer the resumed one
    }
    if (!activity) return fail("no activity found");

    jclass actc = env->FindClass("android/app/Activity");
    jmethodID req = env->GetMethodID(actc, "requestPermissions", "([Ljava/lang/String;I)V");
    if (!req || pending(env)) return fail("Activity.requestPermissions not found");
    jclass strc = env->FindClass("java/lang/String");
    jobjectArray perms = env->NewObjectArray(1, strc, env->NewStringUTF(kMic));
    env->CallVoidMethod(activity, req, perms, static_cast<jint>(7331));
    if (pending(env)) return fail("requestPermissions threw an exception");

    env->PopLocalFrame(nullptr);
    return "requested - look for the Android microphone prompt";
}

} // namespace voicechat

// Optional: lets us keep the JavaVM if the mod loader runs JNI_OnLoad for us.
extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM *vm, void *) {
    // g_vm lives in voicechat's anonymous namespace
    voicechat::g_vm = vm;
    return JNI_VERSION_1_6;
}
