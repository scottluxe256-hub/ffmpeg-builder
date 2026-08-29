#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>

#define LOG_TAG "FFmpegJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jboolean JNICALL
Java_com_ffmpeg_compressor_engine_FFmpegNative_loadFdkPlugin(JNIEnv *env, jobject thiz, jstring pluginPath) {
    if (pluginPath == NULL) {
        LOGE("Path plugin bernilai null!");
        return JNI_FALSE;
    }

    const char *path = (*env)->GetStringUTFChars(env, pluginPath, 0);
    if (path == NULL) {
        LOGE("Gagal mengonversi string JNI.");
        return JNI_FALSE;
    }

    // Memuat library ke RAM secara runtime
    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    (*env)->ReleaseStringUTFChars(env, pluginPath, path);

    if (!handle) {
        LOGE("Gagal memuat libfdk_aac.so: %s", dlerror());
        return JNI_FALSE;
    }

    LOGI("Berhasil memuat libfdk_aac.so ke RAM!");
    return JNI_TRUE;
}

#ifdef __cplusplus
}
#endif
