// Exercise the production JNI bridge with deterministic JNI and input callbacks.
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "front.h"
typedef void *jobject;
typedef jobject jclass;
typedef jobject jbyteArray;
typedef void *jmethodID;
typedef signed char jbyte;
typedef int jsize;
typedef int jint;
typedef unsigned char jboolean;
typedef const struct JNITable *JNIEnv;
typedef const struct VMTable *JavaVM;
#define JNI_VERSION_1_6 0x10006
#define JNI_EDETACHED -2
#define JNI_OK 0
struct JNITable {
    jobject (*NewLocalRef)(JNIEnv *, jobject);
    jint (*PushLocalFrame)(JNIEnv *, jint);
    jclass (*GetObjectClass)(JNIEnv *, jobject);
    jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
    jboolean (*ExceptionCheck)(JNIEnv *);
    void (*ExceptionClear)(JNIEnv *);
    jbyteArray (*NewByteArray)(JNIEnv *, jsize);
    void (*SetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, const jbyte *);
    jbyteArray (*CallObjectMethod)(JNIEnv *, jobject, jmethodID, jbyteArray, jbyteArray, jbyteArray, jbyteArray, int, int);
    jsize (*GetArrayLength)(JNIEnv *, jbyteArray);
    void (*GetByteArrayRegion)(JNIEnv *, jbyteArray, jsize, jsize, jbyte *);
    jobject (*PopLocalFrame)(JNIEnv *, jobject);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    jobject (*NewGlobalRef)(JNIEnv *, jobject);
    void (*DeleteGlobalRef)(JNIEnv *, jobject);
    jboolean (*IsSameObject)(JNIEnv *, jobject, jobject);
};
struct VMTable {
    jint (*GetEnv)(JavaVM *, void **, jint);
    jint (*AttachCurrentThread)(JavaVM *, JNIEnv **, void *);
    jint (*DetachCurrentThread)(JavaVM *);
};
typedef struct { JavaVM *vm; jobject clazz; } ANativeActivity;
static ANativeActivity *g_act;
static void *g_win = (void *)1, *g_external_win;
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_keyboard_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_ime_on;
static uint64_t g_touch_btn;
_Thread_local int g_swkbd_min_chars = -1;
static pthread_t guest_thread;
static _Thread_local int jni_attached;
static int lookups, pending, clears, refs, attached, detached, call_error, logs, fallback;
static int resets, touch_resets, holds, frame_error;
static char log_text[256];
#define LOG(...) do { ++logs; snprintf(log_text, sizeof log_text, __VA_ARGS__); } while (0)
static void hold_sync(void) { ++holds; }
static void dm_input_set(int a, int b, int c, int d, int e) { assert(!(a|b|c|d|e)); ++resets; }
void dm_touch_set(const dm_touch *p, int n) { assert(!p && !n); ++touch_resets; }
static jobject ref(JNIEnv *e, jobject o) { (void)e; return o; }
static jint frame(JNIEnv *e, jint n) { (void)e; assert(n >= 12); return frame_error ? (pending=1, -1) : 0; }
static jmethodID method(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e; ++lookups;
    assert(jni_attached && !pthread_equal(pthread_self(), guest_thread));
    assert(!strcmp(name,"readKeyboard") && !strcmp(sig,"([B[B[B[BII)[B"));
    if (c == (void *)1) { pending=1; return NULL; }
    return (void *)3;
}
static jboolean exception(JNIEnv *e) { (void)e; return pending; }
static void clear(JNIEnv *e) { (void)e; pending=0; ++clears; }
static jbyteArray bytes(JNIEnv *e, jsize n) { (void)e; (void)n; static char data[4][128]; static int next; return data[next++ % 4]; }
static void set(JNIEnv *e, jbyteArray a, jsize off, jsize n, const jbyte *p) { (void)e; memcpy((char *)a+off,p,n); }
static jbyteArray call(JNIEnv *e, jobject a, jmethodID m, jbyteArray t, jbyteArray s, jbyteArray g, jbyteArray i, int min, int max) {
    (void)e; (void)a; (void)m; (void)t; (void)s; (void)g; (void)i;
    assert(jni_attached && !pthread_equal(pthread_self(), guest_thread));
    assert(atomic_load(&g_ime_on) && !g_touch_btn && min==2 && max==6);
    g_touch_btn=99; if (call_error) { pending=1; return NULL; }
    return (void *)"A\xf0\x9f\x9a\x80" "B";
}
static jsize length(JNIEnv *e,jbyteArray a) { (void)e; return (jsize)strlen(a); }
static void get(JNIEnv *e,jbyteArray a,jsize off,jsize n,jbyte *p) { (void)e; memcpy(p,(char *)a+off,n); }
static void unref(JNIEnv *e,jobject o) { (void)e; (void)o; }
static jobject global(JNIEnv *e,jobject o) { (void)e; ++refs; return o; }
static void unglobal(JNIEnv *e,jobject o) { (void)e; assert(o); --refs; }
static jboolean same(JNIEnv *e,jobject a,jobject b) { (void)e; return a==b; }
static const struct JNITable jni={ref,frame,ref,method,exception,clear,bytes,set,call,length,get,ref,unref,global,unglobal,same};
static JNIEnv env=&jni;
static jint get_env(JavaVM *v, void **p, jint ver) {
    (void)v; (void)p;
    assert(!pthread_equal(pthread_self(), guest_thread) && "JNI GetEnv on guest caller");
    assert(ver == JNI_VERSION_1_6 && !jni_attached);
    return JNI_EDETACHED;
}
static jint attach(JavaVM *v, JNIEnv **p, void *a) {
    (void)v; (void)a;
    assert(!pthread_equal(pthread_self(), guest_thread) && "JNI attach on guest caller");
    assert(!jni_attached);
    jni_attached = 1;
    ++attached;
    *p = &env;
    return JNI_OK;
}
static jint detach(JavaVM *v) {
    (void)v;
    assert(!pthread_equal(pthread_self(), guest_thread) && jni_attached);
    jni_attached = 0;
    ++detached;
    return JNI_OK;
}
static const struct VMTable vmt={get_env,attach,detach};
static JavaVM vm=&vmt;
#include "keyboard_android.inc"
static int overlay_text_input(const char *t,const char *s,const char *g,const char *i,int m,char *o,int n) {
    (void)t;(void)s;(void)g;(void)i;(void)m;(void)o;(void)n;
    assert(pthread_equal(pthread_self(), guest_thread) && !jni_attached);
    ++fallback; return 0;
}
#include "keyboard_dispatch.inc"
int main(void) {
    guest_thread = pthread_self();
    ANativeActivity activity={&vm,(void *)1}; g_act=&activity;
    char out[9]="before";
    for(int i=0;i<3;i++) assert(front_text_input("T","S","G","I",6,out,sizeof out)==0);
    assert(lookups==1 && clears==1 && !pending && fallback==3 && refs==1);
    assert(attached==3 && detached==3);
    activity.clazz=(void *)2; g_swkbd_min_chars=2; g_touch_btn=42;
    for(int i=0;i<3;i++) {
        memset(out,0x7b,sizeof out);
        assert(front_text_input("T","S","G","I",6,out,5)==1);
        assert(!strcmp(out,"A") && out[5]==0x7b && !g_touch_btn && !atomic_load(&g_ime_on));
    }
    assert(lookups==2 && refs==1 && attached==6 && detached==6 && resets==6 && touch_resets==6 && holds==6);
    call_error=1; strcpy(out,"before");
    assert(front_text_input("T","S","G","I",6,out,sizeof out)==-1);
    assert(!pending && !atomic_load(&g_ime_on) && !g_touch_btn && !strcmp(out,"before") && fallback==3);
    frame_error=1;
    assert(front_text_input("T","S","G","I",6,out,sizeof out)==-1);
    assert(!pending && attached==detached && lookups==2);
    pthread_mutex_lock(&g_keyboard_mu);
    assert(front_text_input("T","S","G","I",6,out,sizeof out)==0);
    pthread_mutex_unlock(&g_keyboard_mu);
    assert(logs==1 && strstr(log_text,"cancel") && strstr(log_text,"request"));
    assert(front_text_input("T","S","G","I",6,NULL,0)==-1);
}
