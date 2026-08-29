#include <jni.h>
#include <windows.h>
#include <stdio.h>

// Fungsi JNI untuk memuat plugin libfdk_aac.dll secara dinamis di Windows
JNIEXPORT jboolean JNICALL
Java_com_ffmpeg_compressor_engine_FFmpegNative_loadFdkPlugin(JNIEnv *env, jobject thiz, jstring pluginPath) {
    const char *path = (*env)->GetStringUTFChars(env, pluginPath, 0);

    // Memuat DLL eksternal ke RAM Windows secara runtime
    HMODULE hModule = LoadLibraryA(path);
    (*env)->ReleaseStringUTFChars(env, pluginPath, path);

    if (!hModule) {
        printf("Gagal memuat libfdk_aac.dll! Error Code: %lu\n", GetLastError());
        return JNI_FALSE;
    }

    printf("Berhasil memuat libfdk_aac.dll ke RAM Windows!\n");
    return JNI_TRUE;
}
