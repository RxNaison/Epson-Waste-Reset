// The Kotlin side's way into EWR's C ABI (include/ewr/ewr_c.h). Nothing here
// decides anything: it carries strings across, wires the C callbacks to the
// Kotlin listener of the call in progress, and packs every answer as
//     {"status": n, "status_name": "...", "error": "...", "data": {...} | null}
// so Kotlin reads one shape. Callbacks arrive on the thread that made the
// call (the C ABI's promise), so that call's JNIEnv is the one to use.
#include <jni.h>

#include "ewr/ewr_c.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

    struct CallContext
    {
        JNIEnv* env = nullptr;
        jobject callbacks = nullptr; // the Kotlin EwrCallbacks, or null
    };

    std::string FromJava(JNIEnv* env, jstring text)
    {
        if (!text)
            return {};
        const char* chars = env->GetStringUTFChars(text, nullptr);
        std::string out = chars ? chars : "";
        env->ReleaseStringUTFChars(text, chars);
        return out;
    }

    jstring ToJava(JNIEnv* env, const std::string& text)
    {
        return env->NewStringUTF(text.c_str());
    }

    // A Kotlin exception in a callback is a "no", never a crash mid-write.
    bool Cleared(JNIEnv* env)
    {
        if (!env->ExceptionCheck())
            return false;
        env->ExceptionClear();
        return true;
    }

    void OnEvent(const ewr_event* event, void* user)
    {
        auto* ctx = static_cast<CallContext*>(user);
        if (!ctx || !ctx->callbacks || !event)
            return;

        JNIEnv* env = ctx->env;
        jclass type = env->GetObjectClass(ctx->callbacks);
        jmethodID method = env->GetMethodID(type, "onEvent",
                                            "(Ljava/lang/String;Ljava/lang/String;IIIILjava/lang/String;)V");
        env->DeleteLocalRef(type);
        if (!method)
        {
            Cleared(env);
            return;
        }

        jstring code = ToJava(env, event->code ? event->code : "");
        jstring message = ToJava(env, event->message ? event->message : "");
        jstring fields = ToJava(env, event->fields_json ? event->fields_json : "{}");
        env->CallVoidMethod(ctx->callbacks, method, code, message, event->level, event->stage,
                            event->index, event->total, fields);
        Cleared(env);
        env->DeleteLocalRef(code);
        env->DeleteLocalRef(message);
        env->DeleteLocalRef(fields);
    }

    int Ask(const char* name, const char* json, void* user)
    {
        auto* ctx = static_cast<CallContext*>(user);
        if (!ctx || !ctx->callbacks)
            return 0;

        JNIEnv* env = ctx->env;
        jclass type = env->GetObjectClass(ctx->callbacks);
        jmethodID method = env->GetMethodID(type, name, "(Ljava/lang/String;)Z");
        env->DeleteLocalRef(type);
        if (!method)
        {
            Cleared(env);
            return 0;
        }

        jstring text = ToJava(env, json ? json : "{}");
        const jboolean answer = env->CallBooleanMethod(ctx->callbacks, method, text);
        env->DeleteLocalRef(text);
        if (Cleared(env))
            return 0;
        return answer ? 1 : 0;
    }

    int OnBlocker(const char* json, void* user) { return Ask("decideBlocker", json, user); }
    int OnConfirm(const char* json, void* user) { return Ask("confirm", json, user); }
    int OnBackup(const char* json, void* user) { return Ask("saveBackup", json, user); }

    std::string Envelope(int status, const std::string& error, char* json)
    {
        nlohmann::json out;
        out["status"] = status;
        out["status_name"] = ewr_status_name(status);
        out["error"] = error;
        out["data"] = nullptr;
        if (json)
        {
            out["data"] = nlohmann::json::parse(json, nullptr, false);
            ewr_string_free(json);
        }
        return out.dump();
    }

    // One C ABI call on `handle`, with the callbacks wired to `callbacks` for
    // its duration only.
    template <typename Call>
    jstring Run(JNIEnv* env, jlong handle, jobject callbacks, Call call)
    {
        auto* session = reinterpret_cast<ewr_session*>(handle);
        if (!session)
            return ToJava(env, Envelope(EWR_ERR_INVALID_ARGUMENT, "No session.", nullptr));

        CallContext ctx{ env, callbacks };
        ewr_session_set_event_callback(session, callbacks ? OnEvent : nullptr, &ctx);
        ewr_session_set_blocker_callback(session, callbacks ? OnBlocker : nullptr, &ctx);
        ewr_session_set_confirm_callback(session, callbacks ? OnConfirm : nullptr, &ctx);
        ewr_session_set_backup_callback(session, callbacks ? OnBackup : nullptr, &ctx);

        char* json = nullptr;
        const int status = call(session, &json);
        const std::string error = ewr_session_last_error(session);

        // Nothing may call back into a listener that is gone.
        ewr_session_set_event_callback(session, nullptr, nullptr);
        ewr_session_set_blocker_callback(session, nullptr, nullptr);
        ewr_session_set_confirm_callback(session, nullptr, nullptr);
        ewr_session_set_backup_callback(session, nullptr, nullptr);

        return ToJava(env, Envelope(status, error, json));
    }

    const char* OrNull(const std::string& text) { return text.empty() ? nullptr : text.c_str(); }

} // namespace

#define EWR_JNI(name) JNICALL Java_io_github_rxnaison_ewr_EwrNative_##name

extern "C" {

JNIEXPORT jint EWR_JNI(abiVersion)(JNIEnv*, jclass) { return ewr_abi_version(); }

JNIEXPORT jstring EWR_JNI(version)(JNIEnv* env, jclass) { return ToJava(env, ewr_version()); }

JNIEXPORT void EWR_JNI(setDataDir)(JNIEnv* env, jclass, jstring dir)
{
    ewr_set_data_dir(FromJava(env, dir).c_str());
}

JNIEXPORT void EWR_JNI(setUsbFds)(JNIEnv* env, jclass, jintArray fds)
{
    std::vector<int> list;
    if (fds)
    {
        const jsize count = env->GetArrayLength(fds);
        list.resize(static_cast<size_t>(count));
        env->GetIntArrayRegion(fds, 0, count, reinterpret_cast<jint*>(list.data()));
    }
    ewr_android_set_usb_fds(list.data(), list.size());
}

JNIEXPORT jstring EWR_JNI(databaseCheck)(JNIEnv* env, jclass, jstring path)
{
    char* json = nullptr;
    const int status = ewr_database_check(FromJava(env, path).c_str(), &json);
    return ToJava(env, Envelope(status, status == EWR_OK ? "" : "Not a database this EWR can load.", json));
}

// {status, handle}: a handle is a pointer, and on Android a tagged one, so
// it never travels through JSON, whose numbers cannot hold it exactly.
JNIEXPORT jlongArray EWR_JNI(open)(JNIEnv* env, jclass, jstring databasePath, jstring host)
{
    const std::string path = FromJava(env, databasePath);
    const std::string where = FromJava(env, host);

    ewr_session* session = nullptr;
    const int status = where.empty() ? ewr_session_open(path.c_str(), &session)
                                     : ewr_session_open_network(path.c_str(), where.c_str(), &session);

    jlong values[2] = { status, reinterpret_cast<jlong>(session) };
    jlongArray out = env->NewLongArray(2);
    env->SetLongArrayRegion(out, 0, 2, values);
    return out;
}

JNIEXPORT void EWR_JNI(close)(JNIEnv*, jclass, jlong handle)
{
    ewr_session_close(reinterpret_cast<ewr_session*>(handle));
}

JNIEXPORT jstring EWR_JNI(lastError)(JNIEnv* env, jclass, jlong handle)
{
    return ToJava(env, ewr_session_last_error(reinterpret_cast<ewr_session*>(handle)));
}

JNIEXPORT jstring EWR_JNI(listModels)(JNIEnv* env, jclass, jlong handle)
{
    return Run(env, handle, nullptr, [](ewr_session* s, char** out) { return ewr_list_models(s, out); });
}

JNIEXPORT jstring EWR_JNI(detectModel)(JNIEnv* env, jclass, jlong handle, jobject callbacks)
{
    return Run(env, handle, callbacks, [](ewr_session* s, char** out) { return ewr_detect_model(s, out); });
}

JNIEXPORT jstring EWR_JNI(discoverNetwork)(JNIEnv* env, jclass, jlong handle, jobject callbacks)
{
    return Run(env, handle, callbacks, [](ewr_session* s, char** out) { return ewr_discover_network(s, out); });
}

JNIEXPORT jstring EWR_JNI(readStatus)(JNIEnv* env, jclass, jlong handle, jstring model, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    return Run(env, handle, callbacks, [&](ewr_session* s, char** out) { return ewr_read_status(s, name.c_str(), out); });
}

JNIEXPORT jstring EWR_JNI(plan)(JNIEnv* env, jclass, jlong handle, jstring model, jboolean ink)
{
    const std::string name = FromJava(env, model);
    return Run(env, handle, nullptr, [&](ewr_session* s, char** out) { return ewr_plan(s, name.c_str(), ink ? 1 : 0, out); });
}

JNIEXPORT jstring EWR_JNI(reset)(JNIEnv* env, jclass, jlong handle, jstring model, jboolean ink, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    return Run(env, handle, callbacks, [&](ewr_session* s, char** out) { return ewr_reset(s, name.c_str(), ink ? 1 : 0, out); });
}

JNIEXPORT jstring EWR_JNI(restore)(JNIEnv* env, jclass, jlong handle, jstring model, jstring contents, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    const std::string text = FromJava(env, contents);
    return Run(env, handle, callbacks,
               [&](ewr_session* s, char** out) { return ewr_restore(s, OrNull(name), text.c_str(), out); });
}

JNIEXPORT jstring EWR_JNI(headClean)(JNIEnv* env, jclass, jlong handle, jstring model, jstring level, jint group,
                                     jboolean power, jboolean printData, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    const std::string strength = FromJava(env, level);
    return Run(env, handle, callbacks, [&](ewr_session* s, char** out) {
        return ewr_head_clean(s, OrNull(name), OrNull(strength), group, power ? 1 : 0, printData ? 1 : 0, out);
    });
}

JNIEXPORT jstring EWR_JNI(nozzleCheck)(JNIEnv* env, jclass, jlong handle, jstring model, jint variant, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    return Run(env, handle, callbacks,
               [&](ewr_session* s, char** out) { return ewr_nozzle_check(s, OrNull(name), variant, out); });
}

JNIEXPORT jstring EWR_JNI(readInfo)(JNIEnv* env, jclass, jlong handle, jstring model, jobject callbacks)
{
    const std::string name = FromJava(env, model);
    return Run(env, handle, callbacks, [&](ewr_session* s, char** out) { return ewr_read_info(s, OrNull(name), out); });
}

} // extern "C"
