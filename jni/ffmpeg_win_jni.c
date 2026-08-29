#include <jni.h>
#include <windows.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     com_ffmpeg_compressor_engine_FFmpegNative
 * Method:    loadFdkPlugin
 * Signature: (Ljava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL
Java_com_ffmpeg_compressor_engine_FFmpegNative_loadFdkPlugin(JNIEnv *env, jobject thiz, jstring pluginPath) {
    if (pluginPath == NULL) {
        return JNI_FALSE;
    }

    const char *path = (*env)->GetStringUTFChars(env, pluginPath, NULL);
    if (path == NULL) {
        return JNI_FALSE; // Out of memory
    }

    // Load DLL plugin secara dinamis ke dalam process RAM Windows
    HMODULE hModule = LoadLibraryA(path);
    
    // Bebaskan memory string JNI
    (*env)->ReleaseStringUTFChars(env, pluginPath, path);

    if (!hModule) {
        DWORD errCode = GetLastError();
        char errBuff[256];
        snprintf(errBuff, sizeof(errBuff), "[FFmpegJNI Error] Gagal load DLL. Code: %lu\n", errCode);
        OutputDebugStringA(errBuff); // Muncul di Debugger Windows / Visual Studio Log
        return JNI_FALSE;
    }

    OutputDebugStringA("[FFmpegJNI Success] Berhasil memuat libfdk_aac.dll!\n");
    return JNI_TRUE;
}

#ifdef __cplusplus
}
#endif
