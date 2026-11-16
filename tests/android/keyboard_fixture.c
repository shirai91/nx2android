// Synthetic JNI/guest contract probe. No game files or assets.
#include <android/native_activity.h>
#include <android/log.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define LOG(...) ((void)0)
#define RES_OK 0
#define Result uint32_t
#include "swkbd_core.inc"
static ANativeActivity *g_act;
static ANativeWindow *g_win, *g_external_win;
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_ime_on, resumed;
static _Atomic uint64_t g_touch_btn;
static void hold_sync(void) {}
static void dm_input_set(int a,int b,int c,int d,int e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
static void dm_touch_set(void *p,int n) { (void)p;(void)n; }
#include "keyboard_android.inc"
#define REQUIRE(x) do { if (!(x)) { __android_log_print(ANDROID_LOG_ERROR,"nx2a-keyboard-test","FAIL line %d",__LINE__); abort(); } } while(0)
static void *exercise(void *unused) {
    (void)unused;
#ifdef KEYBOARD_NO_JAVA
    while (!atomic_load(&resumed) || !g_win) usleep(10000);
    char unchanged[8]="before";
    REQUIRE(android_text_input("Fallback","","","",5,unchanged,sizeof unchanged)==-2);
    REQUIRE(!strcmp(unchanged,"before") && !atomic_load(&g_ime_on));
    __android_log_print(ANDROID_LOG_INFO,"nx2a-keyboard-test","ALL PASSED: native-only fallback");
    return NULL;
#endif
    uint16_t initial[]={'A',0xd83d,0xde80,'e',0x301,'Z','Q',0};
    const char utf8[]="A\xf0\x9f\x9a\x80" "e\xcc\x81Z";
    const char *titles[]={"Unicode","UTF16","Minimum","Cancel","Background","Repeat"};
    for (int t=0;t<6;t++) {
        while (!atomic_load(&resumed) || !g_win) usleep(10000);
        usleep(300000);
        _Alignas(8) uint8_t arg[0x500]={0}, buffer[64];
        struct { void *data; uint64_t size; } out={buffer,sizeof buffer};
        memset(buffer,0x7b,sizeof buffer);
        for(int i=0;titles[t][i];i++) arg[0x24+i*2]=titles[t][i];
        arg[0x3ac]=6; arg[0x3b0]=t==2 ? 2:0; arg[0x3bd]=t!=1;
        if(t<=1 || t==5) { const void *work=initial; memcpy(arg+0x4d0,&work,sizeof work); arg[0x3c4]=7; }
        uint32_t result=swkbd_show(&out,arg,android_text_input);
        REQUIRE(g_swkbd_min_chars==-1 && atomic_load(&g_ime_on)==0);
        if(t==3 || t==4) REQUIRE(result==0x29f && buffer[0]==0x7b);
        else {
            REQUIRE(result==0);
            if(t==1) REQUIRE(!memcmp(buffer,initial,12) && !buffer[12] && !buffer[13]);
            else REQUIRE(!strcmp((char*)buffer,t==2?"ab":utf8));
        }
        __android_log_print(ANDROID_LOG_INFO,"nx2a-keyboard-test","PASS %s",titles[t]);
    }
    char small[7]; memset(small,0x7b,sizeof small);
    REQUIRE(android_text_input("Bounded", "", "", "A\xf0\x9f\x9a\x80" "B", 6, small, 5)==1);
    REQUIRE(!strcmp(small,"A") && small[5]==0x7b && small[6]==0x7b);
    __android_log_print(ANDROID_LOG_INFO,"nx2a-keyboard-test","ALL PASSED");
    return NULL;
}
static void resume(ANativeActivity *a) { (void)a; atomic_store(&resumed,1); }
static void pause_app(ANativeActivity *a) { (void)a; atomic_store(&resumed,0); }
static void window(ANativeActivity *a,ANativeWindow *w) { (void)a; g_win=w; }
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *a,void *saved,size_t n) {
    (void)saved;(void)n;g_act=a;
    a->callbacks->onResume=resume;a->callbacks->onPause=pause_app;a->callbacks->onNativeWindowCreated=window;
    pthread_t th; REQUIRE(!pthread_create(&th,NULL,exercise,NULL));pthread_detach(th);
}
