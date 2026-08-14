#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MINGW32__)
static CRITICAL_SECTION ds_console_lock;
static int ds_console_lock_inited = 0;
static void console_lock_init(void) {
    if (!ds_console_lock_inited) { InitializeCriticalSection(&ds_console_lock); ds_console_lock_inited = 1; }
}
#define CONSOLE_LOCK() do { console_lock_init(); EnterCriticalSection(&ds_console_lock); } while (0)
#define CONSOLE_UNLOCK() LeaveCriticalSection(&ds_console_lock)
#else
#include <pthread.h>
static pthread_mutex_t ds_console_lock = PTHREAD_MUTEX_INITIALIZER;
#define CONSOLE_LOCK() pthread_mutex_lock(&ds_console_lock)
#define CONSOLE_UNLOCK() pthread_mutex_unlock(&ds_console_lock)
#endif

#define DS_CONSOLE_MAX 256
#define DS_CONSOLE_LINE_MAX 192
static char ds_console_buf[DS_CONSOLE_MAX][DS_CONSOLE_LINE_MAX];
static int ds_console_type_buf[DS_CONSOLE_MAX];
static int ds_console_head = 0, ds_console_count = 0;
static char ds_console_read[8][DS_CONSOLE_LINE_MAX];
static int ds_console_read_pos = 0;

static void console_add(const char *line, int is_error) {
    if (!line) return;
    char tmp[DS_CONSOLE_LINE_MAX]; size_t n = strlen(line), w = 0;
    for (size_t i = 0; i < n && w + 1 < sizeof(tmp); i++) tmp[w++] = (line[i] == '\n' || line[i] == '\r') ? ' ' : line[i];
    tmp[w] = '\0';
    CONSOLE_LOCK();
    snprintf(ds_console_buf[ds_console_head], DS_CONSOLE_LINE_MAX, "%s", tmp);
    ds_console_type_buf[ds_console_head] = is_error ? 1 : 0;
    ds_console_head = (ds_console_head + 1) % DS_CONSOLE_MAX;
    if (ds_console_count < DS_CONSOLE_MAX) ds_console_count++;
    CONSOLE_UNLOCK();
}

int console_count(void) { return ds_console_count; }
int console_type(int index) {
    int t = 0;
    CONSOLE_LOCK();
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        t = ds_console_type_buf[pos < 0 ? pos + DS_CONSOLE_MAX : pos];
    }
    CONSOLE_UNLOCK();
    return t;
}
const char *console_line(int index) {
    CONSOLE_LOCK();
    char *slot = ds_console_read[ds_console_read_pos];
    ds_console_read_pos = (ds_console_read_pos + 1) % 8;
    if (index >= 0 && index < ds_console_count) {
        int pos = (ds_console_head - ds_console_count + index) % DS_CONSOLE_MAX;
        snprintf(slot, DS_CONSOLE_LINE_MAX, "%s", ds_console_buf[pos < 0 ? pos + DS_CONSOLE_MAX : pos]);
    } else slot[0] = '\0';
    CONSOLE_UNLOCK();
    return slot;
}
void console_clear(void) { CONSOLE_LOCK(); ds_console_count = ds_console_head = 0; CONSOLE_UNLOCK(); }

Joy joy = {0};
int screen_w = 0, screen_h = 0;
double dt = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0, ds_has_error = 0, ds_restart_requested = 0;
static char ds_last_error[1024] = {0};

typedef struct DSStringNode { struct DSStringNode *next; char *string; } DSStringNode;
static DSStringNode *ds_strings = NULL;

void ds_log(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX]; va_list args, copy;
    va_start(args, format); va_copy(copy, args);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "DimScript", format, copy);
#else
    printf("[DimScript] "); vprintf(format, copy); printf("\n"); fflush(stdout);
#endif
    va_end(copy); vsnprintf(tmp, sizeof(tmp), format, args); va_end(args);
    console_add(tmp, 0);
}

void ds_log_err(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX]; va_list args, copy;
    va_start(args, format); va_copy(copy, args);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, copy);
#else
    fprintf(stderr, "[DimScript ERR] "); vfprintf(stderr, format, copy); fprintf(stderr, "\n"); fflush(stderr);
#endif
    va_end(copy); vsnprintf(tmp, sizeof(tmp), format, args); va_end(args);
    console_add(tmp, 1);
}

void ds_console_log(int is_error, const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX]; va_list args, copy;
    va_start(args, format); va_copy(copy, args);
#ifdef __ANDROID__
    __android_log_vprint(is_error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "DimScript", format, copy);
#else
    if (is_error) { fprintf(stderr, "[DimScriptNet ERR] "); vfprintf(stderr, format, copy); fprintf(stderr, "\n"); fflush(stderr); }
    else { printf("[DimScriptNet] "); vprintf(format, copy); printf("\n"); fflush(stdout); }
#endif
    va_end(copy); vsnprintf(tmp, sizeof(tmp), format, args); va_end(args);
    console_add(tmp, is_error ? 1 : 0);
}

void ds_runtime_error(const char *format, ...) {
    char tmp[DS_CONSOLE_LINE_MAX]; va_list args, copy, copy2;
    va_start(args, format); va_copy(copy, args); va_copy(copy2, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy); va_end(copy);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, copy2);
#else
    fprintf(stderr, "[DimScript FATAL] "); vfprintf(stderr, format, copy2); fprintf(stderr, "\n"); fflush(stderr);
#endif
    va_end(copy2); vsnprintf(tmp, sizeof(tmp), format, args); va_end(args);
    console_add(tmp, 1); ds_has_error = 1;
    if (ds_error_handler_active) longjmp(ds_error_jump, 1);
}

int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label) {
    if (!function) { ds_runtime_error("empty hook %s", label ? label : ""); return 0; }
    if (ds_error_handler_active) { function(userdata); return !ds_has_error; }
    ds_error_handler_active = 1;
    if (setjmp(ds_error_jump) == 0) { function(userdata); ds_error_handler_active = 0; return !ds_has_error; }
    ds_error_handler_active = 0;
    if (label && *label && ds_last_error[0] == '\0') snprintf(ds_last_error, sizeof(ds_last_error), "hook '%s' failed", label);
    return 0;
}

const char *ds_runtime_error_message(void) { return ds_last_error[0] ? ds_last_error : "runtime error"; }
int ds_script_has_error(void) { return ds_has_error; }
void ds_clear_runtime_error(void) { ds_has_error = 0; ds_last_error[0] = '\0'; }
void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static char *ds_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1; char *c = (char*)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}
static char *ds_track_string(char *s) {
    if (!s) { ds_runtime_error("OOM string"); return NULL; }
    DSStringNode *node = (DSStringNode*)malloc(sizeof(*node));
    if (!node) { free(s); ds_runtime_error("OOM tracking"); return NULL; }
    node->string = s; node->next = ds_strings; ds_strings = node;
    return s;
}
char *ds_num_to_string(double number) {
    char buf[96]; snprintf(buf, sizeof(buf), "%g", number);
    return ds_track_string(ds_strdup(buf));
}
void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) { DSStringNode *next = node->next; free(node->string); free(node); node = next; }
    ds_strings = NULL;
}
char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0, lb = right ? strlen(right) : 0;
    char *out = (char*)malloc(la + lb + 1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + la, right, lb);
    out[la + lb] = '\0';
    return ds_track_string(out);
}

/* Arrays */
struct DSArray { double *data; size_t len, cap; };
DSArray* arr_new(void) {
    DSArray *a = (DSArray*)calloc(1, sizeof(*a));
    if (a) { a->cap = 8; a->data = (double*)malloc(a->cap * sizeof(double)); }
    return a;
}
void arr_push(DSArray* a, double v) {
    if (!a) return;
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->data = (double*)realloc(a->data, a->cap * sizeof(double));
    }
    a->data[a->len++] = v;
}
double arr_pop(DSArray* a) { return (a && a->len) ? a->data[--a->len] : 0; }
double arr_get(DSArray* a, double idx) { long i = (long)idx; return (a && i >= 0 && (size_t)i < a->len) ? a->data[i] : 0; }
void arr_set(DSArray* a, double idx, double v) {
    long i = (long)idx; if (!a || i < 0) return;
    while (a->len <= (size_t)i) arr_push(a, 0);
    a->data[i] = v;
}
double arr_len(DSArray* a) { return a ? (double)a->len : 0; }
void arr_clear(DSArray* a) { if (a) a->len = 0; }
void arr_free(DSArray* a) { if (a) { free(a->data); free(a); } }

/* Dicts */
typedef struct DSDictEntry { char *key; double val; struct DSDictEntry *next; } DSDictEntry;
struct DSDict { DSDictEntry *head; };
DSDict* dict_new(void) { return (DSDict*)calloc(1, sizeof(DSDict)); }
void dict_set(DSDict* d, const char* key, double val) {
    if (!d || !key) return;
    for (DSDictEntry *e = d->head; e; e = e->next) if (strcmp(e->key, key) == 0) { e->val = val; return; }
    DSDictEntry *e = (DSDictEntry*)malloc(sizeof(*e));
    if (e) { e->key = ds_strdup(key); e->val = val; e->next = d->head; d->head = e; }
}
double dict_get(DSDict* d, const char* key) {
    if (d && key) for (DSDictEntry *e = d->head; e; e = e->next) if (strcmp(e->key, key) == 0) return e->val;
    return 0;
}
int dict_has(DSDict* d, const char* key) {
    if (d && key) for (DSDictEntry *e = d->head; e; e = e->next) if (strcmp(e->key, key) == 0) return 1;
    return 0;
}
void dict_del(DSDict* d, const char* key) {
    if (!d || !key) return;
    for (DSDictEntry **pp = &d->head; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->key, key) == 0) { DSDictEntry *t = *pp; *pp = t->next; free(t->key); free(t); return; }
    }
}
void dict_free(DSDict* d) {
    if (!d) return;
    for (DSDictEntry *e = d->head; e;) { DSDictEntry *n = e->next; free(e->key); free(e); e = n; }
    free(d);
}

/* Timers */
struct DSTimer { long long start_ms; };
static long long now_ms(void) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (long long)GetTickCount64();
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    return (long long)time(NULL) * 1000;
#endif
}
DSTimer* timer_new(void) { DSTimer *t = (DSTimer*)malloc(sizeof(*t)); if (t) t->start_ms = now_ms(); return t; }
void timer_start(DSTimer* t) { if (t) t->start_ms = now_ms(); }
double timer_elapsed(DSTimer* t) { return t ? (now_ms() - t->start_ms) / 1000.0 : 0; }
void timer_reset(DSTimer* t) { if (t) t->start_ms = now_ms(); }
void timer_free(DSTimer* t) { free(t); }

/* Files */
const char* file_read(const char* path) {
    if (!path) return ds_track_string(ds_strdup(""));
    FILE *f = fopen(path, "rb");
    if (!f) { char b[512]; snprintf(b, sizeof(b), "game/assets/%s", path); f = fopen(b, "rb"); if (!f) { snprintf(b, sizeof(b), "assets/%s", path); f = fopen(b, "rb"); } }
    if (!f) return ds_track_string(ds_strdup(""));
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0; if (sz > 10*1024*1024) sz = 10*1024*1024;
    char *d = (char*)malloc(sz + 1);
    if (!d) { fclose(f); return ds_track_string(ds_strdup("")); }
    size_t r = fread(d, 1, sz, f); fclose(f); d[r] = '\0';
    return ds_track_string(d);
}
int file_write(const char* path, const char* content) {
    if (!path) return 0;
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    size_t len = content ? strlen(content) : 0, w = fwrite(content ? content : "", 1, len, f);
    fclose(f); return w == len;
}
int file_exists(const char* path) {
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    char b[512]; snprintf(b, sizeof(b), "game/assets/%s", path); f = fopen(b, "rb");
    if (f) { fclose(f); return 1; }
    snprintf(b, sizeof(b), "assets/%s", path); f = fopen(b, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}
int file_del(const char* path) { return path ? (remove(path) == 0) : 0; }

/* JSON */
static const char* skip_ws(const char* p) { while (p && *p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; return p; }
double json_get_num(const char* json, const char* path) {
    if (!json || !path) return 0;
    const char *key = strrchr(path, '/'); key = key ? key + 1 : path;
    char pat[128]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(json, pat); if (!pos) return 0;
    pos = strchr(pos, ':'); if (!pos) return 0;
    return strtod(skip_ws(pos + 1), NULL);
}
const char* json_get_str(const char* json, const char* path) {
    if (!json || !path) return ds_track_string(ds_strdup(""));
    const char *key = strrchr(path, '/'); key = key ? key + 1 : path;
    char pat[128]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(json, pat); if (!pos) return ds_track_string(ds_strdup(""));
    pos = strchr(pos, ':'); if (!pos) return ds_track_string(ds_strdup(""));
    pos = skip_ws(pos + 1); if (*pos != '"') return ds_track_string(ds_strdup("")); pos++;
    const char *end = strchr(pos, '"'); if (!end) return ds_track_string(ds_strdup(""));
    size_t len = end - pos; char *out = (char*)malloc(len + 1);
    if (!out) return ds_track_string(ds_strdup(""));
    memcpy(out, pos, len); out[len] = '\0';
    return ds_track_string(out);
}
int json_get_bool(const char* json, const char* path) {
    if (!json || !path) return 0;
    const char *key = strrchr(path, '/'); key = key ? key + 1 : path;
    char pat[128]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *pos = strstr(json, pat); if (!pos) return 0;
    pos = strchr(pos, ':'); if (!pos) return 0;
    pos = skip_ws(pos + 1);
    return strncmp(pos, "true", 4) == 0;
}

const char* http_get(const char* url) {
    if (!url) return ds_track_string(ds_strdup(""));
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return file_read(url);
    return ds_track_string(ds_strdup(""));
}
const char* http_post(const char* url, const char* body) { (void)url; (void)body; return ds_track_string(ds_strdup("")); }

double clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double dist(double x1, double y1, double x2, double y2) { double dx = x2 - x1, dy = y2 - y1; return sqrt(dx * dx + dy * dy); }
double now(void) { return now_ms() / 1000.0; }
double str_len(const char *s) { return s ? (double)strlen(s) : 0; }
int str_eq(const char *a, const char *b) { return (a == b) || (a && b && strcmp(a, b) == 0); }

/* Keyboard */
#ifdef __ANDROID__
#include <android/native_activity.h>
#include <android/keycodes.h>
#include <jni.h>
static ANativeActivity *kb_activity = NULL;
#endif

#define KB_BUF 256
static char kb_text[KB_BUF] = {0};
static int kb_len = 0, kb_show = 0, kb_enter = 0;

void ds_set_activity(void *act) {
#ifdef __ANDROID__
    kb_activity = (ANativeActivity*)act;
#else
    (void)act;
#endif
}
void keyboard_show(void) {
#ifdef __ANDROID__
    if (kb_activity) ANativeActivity_showSoftInput(kb_activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
#endif
    kb_show = 1;
}
void keyboard_hide(void) {
#ifdef __ANDROID__
    if (kb_activity) ANativeActivity_hideSoftInput(kb_activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_IMPLICIT_ONLY);
#endif
    kb_show = 0;
}
const char* keyboard_get_text(void) { return ds_track_string(ds_strdup(kb_text)); }
const char* keyboard_get_raw(void) { return kb_text; }
void keyboard_clear(void) { kb_text[0] = '\0'; kb_len = kb_enter = 0; }
int keyboard_visible(void) { return kb_show; }
int keyboard_enter_pressed(void) { int e = kb_enter; kb_enter = 0; return e; }
void keyboard_type(const char *text) {
    if (!text) return;
    for (size_t i = 0; text[i] && kb_len + 1 < KB_BUF - 1; i++) {
        if (text[i] == '\n' || text[i] == '\r') kb_enter = 1;
        else kb_text[kb_len++] = text[i];
    }
    kb_text[kb_len] = '\0';
}
void keyboard_backspace(void) {
    while (kb_len > 0) {
        unsigned char c = (unsigned char)kb_text[--kb_len];
        kb_text[kb_len] = '\0';
        if ((c & 0xC0) != 0x80) break;
    }
}

static void kb_append_cp(unsigned int cp) {
    char u[5] = {0}; int n = 0;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (kb_len + n < KB_BUF - 1) { memcpy(kb_text + kb_len, u, n); kb_len += n; kb_text[kb_len] = '\0'; }
}

#ifdef __ANDROID__
static unsigned int kb_unicode(int keycode, int meta) {
    JNIEnv *env = NULL; JavaVM *vm; jclass cls; jmethodID ctor, get_uni;
    if (!kb_activity || !kb_activity->vm) return 0;
    vm = kb_activity->vm;
    int attached = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    jint uni = 0;
    if ((*env)->PushLocalFrame(env, 8) == 0) {
        cls = (*env)->FindClass(env, "android/view/KeyEvent");
        if (cls) {
            ctor = (*env)->GetMethodID(env, cls, "<init>", "(II)V");
            get_uni = (*env)->GetMethodID(env, cls, "getUnicodeChar", "(I)I");
            if (ctor && get_uni) {
                jobject ev = (*env)->NewObject(env, cls, ctor, (jint)0, (jint)keycode);
                if (ev && !(*env)->ExceptionCheck(env)) uni = (*env)->CallIntMethod(env, ev, get_uni, (jint)meta);
            }
        }
        (*env)->PopLocalFrame(env, NULL);
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
    return uni > 0 ? (unsigned int)uni : 0;
}
#endif

int keyboard_handle_key(int keycode, int action, int meta) {
    (void)action;
#ifdef __ANDROID__
    if (keycode == AKEYCODE_DEL || keycode == AKEYCODE_FORWARD_DEL) { keyboard_backspace(); return 1; }
    if (keycode == AKEYCODE_ENTER || keycode == AKEYCODE_NUMPAD_ENTER || keycode == AKEYCODE_DPAD_CENTER) { kb_enter = 1; return 1; }
    if (keycode == AKEYCODE_SPACE) { kb_append_cp(' '); return 1; }
    unsigned int cp = kb_unicode(keycode, meta);
    if (cp >= 0x20 && cp != 0x7F) { kb_append_cp(cp); return 1; }
    if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z) { kb_append_cp((unsigned int)('a' + (keycode - AKEYCODE_A))); return 1; }
    if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9) { kb_append_cp((unsigned int)('0' + (keycode - AKEYCODE_0))); return 1; }
#else
    (void)meta;
    if (keycode == '\b' || keycode == 8 || keycode == 127) { keyboard_backspace(); return 1; }
    if (keycode == '\r' || keycode == '\n' || keycode == 13) { kb_enter = 1; return 1; }
    if (keycode >= 32) { kb_append_cp((unsigned int)keycode); return 1; }
#endif
    return 0;
}

void keyboard_commit_utf8(const char *utf8) {
    if (utf8) for (size_t i = 0; utf8[i] && kb_len + 1 < KB_BUF - 1; i++) {
        if (utf8[i] == '\n' || utf8[i] == '\r') kb_enter = 1;
        else kb_text[kb_len++] = utf8[i];
    }
    kb_text[kb_len] = '\0';
}
