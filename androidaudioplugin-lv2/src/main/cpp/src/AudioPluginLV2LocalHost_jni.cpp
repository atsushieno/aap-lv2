#include <unistd.h>
#include <dlfcn.h>
#include <cmath>
#include <cstring>
#include <vector>

#if ANDROID

#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#endif

#include <aap/core/host/audio-plugin-host.h>

#if ANDROID

extern AAssetManager *current_asset_manager;

namespace aaplv2 {

void set_io_context(AAssetManager *am) {
    current_asset_manager = am;
}

void cleanup() {
    set_io_context(nullptr);
}
}

extern "C" {

JNIEXPORT void JNICALL
Java_org_androidaudioplugin_lv2_AudioPluginLV2ServiceExtension_initialize(JNIEnv *env, jobject obj,
                                                                          jstring lv2PathString,
                                                                          jobject assets) {
    aaplv2::set_io_context(assets ? AAssetManager_fromJava(env, assets) : nullptr);

    jboolean isCopy = JNI_TRUE;
    auto s = env->GetStringUTFChars(lv2PathString, &isCopy);
    setenv("LV2_PATH", s, true);
    env->ReleaseStringUTFChars(lv2PathString, s);
}

JNIEXPORT void JNICALL
Java_org_androidaudioplugin_lv2_AudioPluginLV2ServiceExtension_cleanup(JNIEnv *env, jobject obj) {
    aaplv2::cleanup();
}

} // extern "C"
#endif
