#ifdef __ANDROID__
#include <android/native_activity.h>
#include <android/keycodes.h>
#include <jni.h>
#define KB_BUF 384
static ANativeActivity *kb_activity = NULL;
static char kb_text[KB_BUF] = {0};
static char kb_raw_snapshot[KB_BUF] = {0};
static int kb_len = 0;
static int kb_show = 0;
static int kb_enter = 0;
static pthread_mutex_t kb_mutex = PTHREAD_MUTEX_INITIALIZER;

static JNIEnv *kb_get_env(int *attached) {
    JNIEnv *env = NULL;
    JavaVM *vm;
    if (attached) *attached = 0;
    if (!kb_activity || !kb_activity->vm) return NULL;
    vm = kb_activity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
        if (attached) *attached = 1;
    }
    return env;
}

static void kb_release_env(int attached) {
    if (attached && kb_activity && kb_activity->vm)
        (*kb_activity->vm)->DetachCurrentThread(kb_activity->vm);
}

static jstring kb_new_java_string(JNIEnv *env, const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    jchar chars[KB_BUF];
    jsize n = 0;
    while (*p && n < KB_BUF-1) {
        unsigned int cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p&0xE0)==0xC0 && (p[1]&0xC0)==0x80) {
            cp=((p[0]&0x1F)<<6)|(p[1]&0x3F); p+=2;
        } else if ((*p&0xF0)==0xE0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) {
            cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3;
        } else if ((*p&0xF8)==0xF0 && (p[1]&0xC0)==0x80 &&
                   (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) {
            cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4;
        } else { cp=0xFFFD; p++; }
        if (cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF)) cp=0xFFFD;
        if (cp<0x10000) chars[n++]=(jchar)cp;
        else if (n+2<=KB_BUF-1) {
            cp-=0x10000;
            chars[n++]=(jchar)(0xD800+(cp>>10));
            chars[n++]=(jchar)(0xDC00+(cp&0x3FF));
        } else break;
    }
    return (*env)->NewString(env, chars, n);
}

static int kb_call_text_method(const char *name, const char *value) {
    int attached = 0, ok = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 8) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "(Ljava/lang/String;)V") : NULL;
    if (method) {
        jstring text = kb_new_java_string(env, value);
        if (text) {
            (*env)->CallVoidMethod(env, kb_activity->clazz, method, text);
            ok = !(*env)->ExceptionCheck(env);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return ok;
}

static int kb_call_bool_method(const char *name) {
    int attached = 0, result = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 4) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "()Z") : NULL;
    if (method) {
        jboolean value = (*env)->CallBooleanMethod(env, kb_activity->clazz, method);
        if (!(*env)->ExceptionCheck(env)) result = value ? 1 : 0;
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return result;
}

static int kb_call_void_method(const char *name) {
    int attached = 0, ok = 0;
    JNIEnv *env = kb_get_env(&attached);
    if (!env || !kb_activity || !kb_activity->clazz) return 0;
    if ((*env)->PushLocalFrame(env, 4) != 0) { kb_release_env(attached); return 0; }
    jclass cls = (*env)->GetObjectClass(env, kb_activity->clazz);
    jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, "()V") : NULL;
    if (method) {
        (*env)->CallVoidMethod(env, kb_activity->clazz, method);
        ok = !(*env)->ExceptionCheck(env);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    kb_release_env(attached);
    return ok;
}

void ds_set_activity(void *act) { kb_activity = (ANativeActivity *)act; }

/* Единственный источник правды для текста — системный редактор в Java.
 * Любое изменение буфера на стороне игры немедленно зеркалим обратно в него,
 * иначе редактор помнит уже стёртые символы и дописывает их к новому вводу
 * ("Q" + "qwerty" -> "Qqwerty"). */
static void kb_sync_editor(void) {
    char copy[KB_BUF];
    pthread_mutex_lock(&kb_mutex);
    snprintf(copy, sizeof(copy), "%s", kb_text);
    pthread_mutex_unlock(&kb_mutex);
    (void)kb_call_text_method("setGameKeyboardText", copy);
}

/* 1, если текст сейчас ведёт системный EditText (он и получает клавиши). */
int keyboard_uses_editor(void) { return kb_call_bool_method("gameKeyboardActive"); }

void keyboard_show(void) {
    char current[KB_BUF];
    pthread_mutex_lock(&kb_mutex);
    snprintf(current, sizeof(current), "%s", kb_text);
    kb_show = 1;
    pthread_mutex_unlock(&kb_mutex);
    if (!kb_call_text_method("showGameKeyboard", current) && kb_activity)
        ANativeActivity_showSoftInput(kb_activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
}

void keyboard_hide(void) {
    pthread_mutex_lock(&kb_mutex); kb_show = 0; pthread_mutex_unlock(&kb_mutex);
    if (!kb_call_void_method("hideGameKeyboard") && kb_activity)
        ANativeActivity_hideSoftInput(kb_activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_IMPLICIT_ONLY);
}

const char *keyboard_get_text(void) {
    char copy[KB_BUF];
    pthread_mutex_lock(&kb_mutex); snprintf(copy, sizeof(copy), "%s", kb_text); pthread_mutex_unlock(&kb_mutex);
    return ds_track_string(ds_strdup(copy));
}

const char *keyboard_get_raw(void) {
    pthread_mutex_lock(&kb_mutex);
    snprintf(kb_raw_snapshot, sizeof(kb_raw_snapshot), "%s", kb_text);
    pthread_mutex_unlock(&kb_mutex);
    return kb_raw_snapshot;
}

void keyboard_clear(void) {
    pthread_mutex_lock(&kb_mutex);
    kb_text[0] = '\0'; kb_len = 0; kb_enter = 0;
    pthread_mutex_unlock(&kb_mutex);
    (void)kb_call_text_method("setGameKeyboardText", "");
}

int keyboard_visible(void) {
    int visible;
    pthread_mutex_lock(&kb_mutex); visible = kb_show; pthread_mutex_unlock(&kb_mutex);
    return visible;
}

int keyboard_enter_pressed(void) {
    int enter;
    pthread_mutex_lock(&kb_mutex); enter = kb_enter; kb_enter = 0; pthread_mutex_unlock(&kb_mutex);
    return enter;
}

static void kb_append_cp_locked(unsigned int cp) {
    char u[5]; int n = 0;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0]=(char)(0xC0|(cp>>6)); u[1]=(char)(0x80|(cp&0x3F)); n=2; }
    else if (cp < 0x10000) { u[0]=(char)(0xE0|(cp>>12)); u[1]=(char)(0x80|((cp>>6)&0x3F)); u[2]=(char)(0x80|(cp&0x3F)); n=3; }
    else if (cp <= 0x10FFFF) { u[0]=(char)(0xF0|(cp>>18)); u[1]=(char)(0x80|((cp>>12)&0x3F)); u[2]=(char)(0x80|((cp>>6)&0x3F)); u[3]=(char)(0x80|(cp&0x3F)); n=4; }
    if (n > 0 && kb_len+n < KB_BUF) {
        memcpy(kb_text+kb_len, u, (size_t)n); kb_len += n; kb_text[kb_len] = '\0';
    }
}

void keyboard_type(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!p) return;
    pthread_mutex_lock(&kb_mutex);
    while (*p) {
        unsigned int cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p&0xE0)==0xC0 && (p[1]&0xC0)==0x80) { cp=((p[0]&0x1F)<<6)|(p[1]&0x3F); p+=2; }
        else if ((*p&0xF0)==0xE0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80) { cp=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; }
        else if ((*p&0xF8)==0xF0 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80) { cp=((p[0]&7)<<18)|((p[1]&63)<<12)|((p[2]&63)<<6)|(p[3]&63); p+=4; }
        else { p++; continue; }
        if (cp=='\n' || cp=='\r') kb_enter=1; else if (cp>=0x20) kb_append_cp_locked(cp);
    }
    pthread_mutex_unlock(&kb_mutex);
    kb_sync_editor();
}

/* Стираем целый UTF-8 символ, а не один байт.
 * Зажатый Backspace: подряд идущие вызовы с малым интервалом — это автоповтор
 * клавиши; после шести таких подряд стираем всё разом (см. тот же приём в
 * GameActivity.java для экранной клавиатуры). */
#define KB_DEL_STREAK_GAP_MS 150
#define KB_DEL_STREAK_CLEAR 6
static long long kb_del_last_ms = 0;
static int kb_del_streak = 0;
void keyboard_backspace(void) {
    struct timespec now;
    long long now_ms;
    pthread_mutex_lock(&kb_mutex);
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        now_ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    else
        now_ms = kb_del_last_ms + KB_DEL_STREAK_GAP_MS + 1;
    if (now_ms - kb_del_last_ms <= KB_DEL_STREAK_GAP_MS) kb_del_streak++;
    else kb_del_streak = 1;
    kb_del_last_ms = now_ms;
    if (kb_del_streak >= KB_DEL_STREAK_CLEAR) {
        kb_text[0] = '\0'; kb_len = 0;
    } else {
        while (kb_len > 0) {
            unsigned char c = (unsigned char)kb_text[--kb_len];
            kb_text[kb_len] = '\0';
            if ((c & 0xC0) != 0x80) break;
        }
    }
    pthread_mutex_unlock(&kb_mutex);
    kb_sync_editor();
}

/* Физическая клавиатура: KeyEvent.getUnicodeChar с учётом раскладки. */
static unsigned int kb_unicode(int keycode, int meta) {
    JNIEnv *env = NULL; JavaVM *vm; jclass cls; jmethodID ctor, get_uni;
    jobject ev; jint uni = 0; int attached = 0;
    if (!kb_activity || !kb_activity->vm) return 0;
    vm = kb_activity->vm;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    if ((*env)->PushLocalFrame(env, 8) != 0) { if (attached) (*vm)->DetachCurrentThread(vm); return 0; }
    cls = (*env)->FindClass(env, "android/view/KeyEvent");
    if (!cls) goto done;
    ctor = (*env)->GetMethodID(env, cls, "<init>", "(II)V");
    get_uni = (*env)->GetMethodID(env, cls, "getUnicodeChar", "(I)I");
    if (!ctor || !get_uni) goto done;
    ev = (*env)->NewObject(env, cls, ctor, (jint)0, (jint)keycode);
    if (!ev || (*env)->ExceptionCheck(env)) goto done;
    uni = (*env)->CallIntMethod(env, ev, get_uni, (jint)meta);
    if ((*env)->ExceptionCheck(env)) uni = 0;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return uni > 0 ? (unsigned int)uni : 0;
}

int keyboard_handle_key(int keycode, int action, int meta) {
    (void)action;
    /* NativeActivity забирает InputQueue окна: KeyEvent от Gboard (латиница
     * без composing) приходят СЮДА, а не в EditText. Если проглотить их из-за
     * wantKeyboard, поле Ник открывает клавиатуру, но буквы не появляются.
     * JNI nativeReplaceText по-прежнему заменяет буфер целиком, если IME
     * всё же доставила символ в редактор — тогда append ниже просто дублирует
     * уже стоящую букву, и мы это пропускаем. */
    if (keycode==AKEYCODE_DEL || keycode==AKEYCODE_FORWARD_DEL) { keyboard_backspace(); return 1; }
    if (keycode==AKEYCODE_ENTER || keycode==AKEYCODE_NUMPAD_ENTER || keycode==AKEYCODE_DPAD_CENTER) {
        pthread_mutex_lock(&kb_mutex); kb_enter=1; pthread_mutex_unlock(&kb_mutex); return 1;
    }
    if (keycode==AKEYCODE_SPACE) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(' '); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    {
        unsigned int cp = kb_unicode(keycode, meta);
        if (cp>=0x20 && cp!=0x7F) {
            pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(cp); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
        }
    }
    if (keycode>=AKEYCODE_A && keycode<=AKEYCODE_Z) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked((unsigned int)('a'+keycode-AKEYCODE_A)); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    if (keycode>=AKEYCODE_0 && keycode<=AKEYCODE_9) {
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked((unsigned int)('0'+keycode-AKEYCODE_0)); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    if (keycode==AKEYCODE_COMMA || keycode==AKEYCODE_PERIOD || keycode==AKEYCODE_MINUS) {
        unsigned int cp = keycode==AKEYCODE_COMMA ? ',' : keycode==AKEYCODE_PERIOD ? '.' : '-';
        pthread_mutex_lock(&kb_mutex); kb_append_cp_locked(cp); pthread_mutex_unlock(&kb_mutex); kb_sync_editor(); return 1;
    }
    return 0;
}

void keyboard_commit_utf8(const char *utf8) { keyboard_type(utf8); }

/* Callbacks from GameActivity's EditText. UTF-16 is converted to valid UTF-8,
 * including surrogate pairs, instead of relying on JNI modified UTF-8. */
JNIEXPORT void JNICALL
Java_com_cb4_GameActivity_nativeReplaceText(JNIEnv *env, jobject self, jstring value) {
    (void)self;
    pthread_mutex_lock(&kb_mutex);
    kb_text[0]='\0'; kb_len=0;
    if (value) {
        jsize count = (*env)->GetStringLength(env, value);
        const jchar *chars = (*env)->GetStringChars(env, value, NULL);
        if (chars) {
            for (jsize i=0; i<count; i++) {
                unsigned int cp = chars[i];
                if (cp>=0xD800 && cp<=0xDBFF && i+1<count && chars[i+1]>=0xDC00 && chars[i+1]<=0xDFFF) {
                    cp = 0x10000 + ((cp-0xD800)<<10) + (chars[++i]-0xDC00);
                }
                if (cp>=0x20 && cp!=0x7F) kb_append_cp_locked(cp);
            }
            (*env)->ReleaseStringChars(env, value, chars);
        }
    }
    pthread_mutex_unlock(&kb_mutex);
}

JNIEXPORT void JNICALL
Java_com_cb4_GameActivity_nativeSubmitText(JNIEnv *env, jobject self) {
    (void)env; (void)self;
    pthread_mutex_lock(&kb_mutex); kb_enter=1; pthread_mutex_unlock(&kb_mutex);
}

JNIEXPORT void JNICALL
Java_com_cb4_GameActivity_nativeKeyboardHidden(JNIEnv *env, jobject self) {
    (void)env; (void)self;
    pthread_mutex_lock(&kb_mutex); kb_show=0; pthread_mutex_unlock(&kb_mutex);
}

#endif
