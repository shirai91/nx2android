// Optional Android keyboard bridge. -2 means the Java activity has no keyboard.
// Shared swkbd owns the guest encodings and UTF-16 length contract. Use byte
// arrays for ordinary UTF-8 across JNI: NewStringUTF uses modified UTF-8.
// Protected by g_keyboard_mu in front_text_input. Keep the class alive for the
// cached method ID; absence is cached too, so code-less APKs throw only once.
static jclass g_keyboard_class;
static jmethodID g_keyboard_method;
static int android_text_input(const char *title, const char *sub, const char *guide, const char *initial,
                     int max_chars, char *out, int out_cap) {
    if (!out || out_cap < 1) return -1;
    pthread_mutex_lock(&g_win_mu);
    ANativeActivity *act = g_win || g_external_win ? g_act : NULL;
    if (!act) { pthread_mutex_unlock(&g_win_mu); return -1; }
    JavaVM *vm = act->vm;
    JNIEnv *env = NULL; int attached = 0;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
            pthread_mutex_unlock(&g_win_mu); return -1;
        }
        attached = 1;
    }
    int result = -1;
    jobject activity = env ? (*env)->NewLocalRef(env, act->clazz) : NULL;
    pthread_mutex_unlock(&g_win_mu);
    if (activity && !(*env)->PushLocalFrame(env, 12)) {
        jclass cls = (*env)->GetObjectClass(env, activity);
        jmethodID method = NULL;
        if (cls && !(*env)->ExceptionCheck(env)) {
            if (!g_keyboard_class || !(*env)->IsSameObject(env, cls, g_keyboard_class)) {
                jclass keep = (*env)->NewGlobalRef(env, cls);
                if (keep) {
                    if (g_keyboard_class) (*env)->DeleteGlobalRef(env, g_keyboard_class);
                    g_keyboard_class = keep;
                    g_keyboard_method = (*env)->GetMethodID(env, cls, "readKeyboard", "([B[B[B[BII)[B");
                    if (!g_keyboard_method && (*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
                }
            }
            if (!(*env)->ExceptionCheck(env)) {
                method = g_keyboard_method;
                if (!method) result = -2;
            }
        }
        const char *args[] = {title, sub, guide, initial};
        jbyteArray bytes[4] = {0};
        for (int i = 0; method && i < 4 && !(*env)->ExceptionCheck(env); i++) {
            jsize n = args[i] ? (jsize)strlen(args[i]) : 0;
            bytes[i] = (*env)->NewByteArray(env, n);
            if (bytes[i] && n) (*env)->SetByteArrayRegion(env, bytes[i], 0, n, (const jbyte *)args[i]);
        }
        if (method && !(*env)->ExceptionCheck(env)) {
            atomic_store(&g_ime_on, 1); hold_sync();
            dm_input_set(0, 0, 0, 0, 0); dm_touch_set(NULL, 0); g_touch_btn = 0;
            jbyteArray answer = (*env)->CallObjectMethod(env, activity, method, bytes[0], bytes[1], bytes[2], bytes[3],
                                                          g_swkbd_min_chars, max_chars);
            atomic_store(&g_ime_on, 0); hold_sync();
            dm_input_set(0, 0, 0, 0, 0); dm_touch_set(NULL, 0); g_touch_btn = 0;
            if (!(*env)->ExceptionCheck(env)) {
                if (!answer) result = 0;
                else {
                    jsize len = (*env)->GetArrayLength(env, answer);
                    jsize n = len < out_cap ? len : out_cap - 1;
                    // Inspect the byte following the cut to avoid returning half a code point.
                    (*env)->GetByteArrayRegion(env, answer, 0, n + (n < len), (jbyte *)out);
                    if (n < len) while (n && ((unsigned char)out[n] & 0xc0) == 0x80) --n;
                    if (!(*env)->ExceptionCheck(env)) { out[n] = 0; result = 1; }
                }
            }
        }
        (*env)->PopLocalFrame(env, NULL);
    }
    if (env && (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env); result = -1;
    }
    if (activity) (*env)->DeleteLocalRef(env, activity);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return result;
}
