// Android port: local play over the LAN (env.txt's DM_LAN=1, hle_ldn.c / hle_socket.c). Many Wi-Fi drivers drop broadcast and multicast frames to save power unless an app
// holds a WifiManager.MulticastLock: without it the device never sees the scans of the other players (ldn_mitm broadcasts them to UDP 11452), nor the game's own broadcasts.
// The lock is taken once, through JNI on a thread of its own, and kept while the app runs (the system drops it when the process ends).
#include "dm.h"
#include <jni.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdlib.h>

static JavaVM *g_vm; static jobject g_actobj;
static void *lan_thread(void *a) {
    (void)a; JNIEnv *E;
    if ((*g_vm)->AttachCurrentThread(g_vm, &E, NULL) != JNI_OK) { LOG("[RED] [lan] cannot attach to the Java VM"); return NULL; }
    jclass cCtx = (*E)->FindClass(E, "android/content/Context"), cWifi = (*E)->FindClass(E, "android/net/wifi/WifiManager");
    jclass cLock = (*E)->FindClass(E, "android/net/wifi/WifiManager$MulticastLock");
    if ((*E)->ExceptionCheck(E) || !cCtx || !cWifi || !cLock) { (*E)->ExceptionClear(E); LOG("[RED] [lan] WifiManager classes missing"); goto out; }
    jmethodID mApp = (*E)->GetMethodID(E, cCtx, "getApplicationContext", "()Landroid/content/Context;");
    jmethodID mSvc = (*E)->GetMethodID(E, cCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jmethodID mMake = (*E)->GetMethodID(E, cWifi, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    jmethodID mRef = (*E)->GetMethodID(E, cLock, "setReferenceCounted", "(Z)V"), mAcq = (*E)->GetMethodID(E, cLock, "acquire", "()V");
    if ((*E)->ExceptionCheck(E)) { (*E)->ExceptionClear(E); LOG("[RED] [lan] WifiManager methods missing"); goto out; }
    jobject app = (*E)->CallObjectMethod(E, g_actobj, mApp);
    jstring name = (*E)->NewStringUTF(E, "wifi");
    jobject wifi = app ? (*E)->CallObjectMethod(E, app, mSvc, name) : NULL;
    if ((*E)->ExceptionCheck(E) || !wifi) { (*E)->ExceptionClear(E); LOG("[RED] [lan] no WifiManager: broadcasts from other players may not arrive"); goto out; }
    jstring tag = (*E)->NewStringUTF(E, "nx2android local play");
    jobject lock = (*E)->CallObjectMethod(E, wifi, mMake, tag);
    if (!(*E)->ExceptionCheck(E) && lock) { (*E)->CallVoidMethod(E, lock, mRef, JNI_FALSE); (*E)->CallVoidMethod(E, lock, mAcq); }
    if ((*E)->ExceptionCheck(E) || !lock) { (*E)->ExceptionClear(E); LOG("[RED] [lan] the multicast lock was refused: broadcasts from other players may not arrive"); goto out; }
    (*E)->NewGlobalRef(E, lock);   // kept for the life of the process
    LOG("[lan] Wi-Fi multicast lock held: broadcasts from other players arrive");
out:
    (*g_vm)->DetachCurrentThread(g_vm);
    return NULL;
}
void dm_lan_android_start(ANativeActivity *act) {
    const char *e = getenv("DM_LAN"); if (!e || *e != '1') return;
    g_vm = act->vm; g_actobj = act->clazz;
    pthread_t t; if (!pthread_create(&t, NULL, lan_thread, NULL)) pthread_detach(t);
}
