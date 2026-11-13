#include <jni.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef struct { JNIEnv *env; jobject clazz; int sdkVersion; } ANativeActivity;
#define LOG(...) ((void)0)
static int mode, received, pending, fail, calls, pushed, popped, resumed, g_quit;
static int bad(void) { if (++calls == fail) { pending=1; return 1; } return 0; }
static jint push(JNIEnv *e, jint n) { (void)e; assert(n>=4); pushed++; return bad() ? -1 : 0; }
static jobject pop(JNIEnv *e, jobject o) { (void)e; popped++; return o; }
static jboolean check(JNIEnv *e) { (void)e; return pending; }
static void clear(JNIEnv *e) { (void)e; pending=0; }
static jclass cls(JNIEnv *e, jobject o) { (void)e; assert(o); return bad() ? NULL : (jclass)2; }
static jstring string(JNIEnv *e, const char *s) { (void)e; assert(!strcmp(s,"game")); return bad() ? NULL : (jstring)3; }
static jmethodID method(JNIEnv *e, jclass c, const char *name, const char *sig) {
    (void)e; assert(c);
    assert((!strcmp(name,"getSystemService") && !strcmp(sig,"(Ljava/lang/String;)Ljava/lang/Object;")) ||
           (!strcmp(name,"getGameMode") && !strcmp(sig,"()I")));
    return bad() ? NULL : (jmethodID)4;
}
static jobject object(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; assert(o&&m); if (bad()) return NULL; return fail==-1 ? NULL : (jobject)5; }
static jint integer(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; assert(o&&m); return bad() ? 0 : mode; }
static void dm_perf_game_mode(int m) { received=m; }
static void go_foreground(void) { assert(received>=0); resumed++; }
static void act_finish(ANativeActivity *a) { (void)a; assert(g_quit); }
/* PRODUCTION */
int main(void) {
    struct JNINativeInterface_ table={0}; table.PushLocalFrame=push; table.PopLocalFrame=pop;
    table.ExceptionCheck=check; table.ExceptionClear=clear; table.GetObjectClass=cls;
    table.NewStringUTF=string; table.GetMethodID=method; table.CallObjectMethod=object; table.CallIntMethod=integer;
    JNIEnv env=&table; ANativeActivity a={&env,(jobject)1,30};
    received=-1; on_resume(&a); assert(received==0 && !calls && resumed==1);
    a.sdkVersion=31;
    for (int m=1;m<=3;m++) { received=-1; mode=m; calls=pushed=popped=0; on_resume(&a); assert(received==m && pushed==1 && popped==1 && !pending); }
    int total=calls;
    for (fail=-1;fail<=total;fail++) {
        if (!fail) continue;
        received=-1; calls=pushed=popped=pending=0; on_resume(&a);
        assert(received==0 && !pending && pushed==1 && popped==(fail==1 ? 0 : 1));
    }
    g_quit=1; calls=0; on_resume(&a); assert(!calls);
    return 0;
}
