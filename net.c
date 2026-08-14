#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "net.h"
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MINGW32__)
#include <windows.h>
#include <process.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
typedef HANDLE ds_net_thread_t;
typedef CRITICAL_SECTION ds_net_mutex_t;
#define NET_MUTEX_INIT(m) InitializeCriticalSection(m)
#define NET_MUTEX_LOCK(m) EnterCriticalSection(m)
#define NET_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
static int ds_thread_create(ds_net_thread_t *th, void *(*fn)(void *), void *arg) {
    *th = (HANDLE)_beginthreadex(NULL, 0, (unsigned int (__stdcall *)(void *))fn, arg, 0, NULL);
    return (*th == NULL) ? -1 : 0;
}
static void ds_thread_join(ds_net_thread_t th) { if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); } }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t ds_net_thread_t;
typedef pthread_mutex_t ds_net_mutex_t;
#define NET_MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define NET_MUTEX_LOCK(m) pthread_mutex_lock(m)
#define NET_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
static int ds_thread_create(ds_net_thread_t *th, void *(*fn)(void *), void *arg) { return pthread_create(th, NULL, fn, arg); }
static void ds_thread_join(ds_net_thread_t th) { pthread_join(th, NULL); }
#endif

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) do { __android_log_print(ANDROID_LOG_INFO, "DimScriptNet", __VA_ARGS__); ds_console_log(0, __VA_ARGS__); } while (0)
#define LOGERR(...) do { __android_log_print(ANDROID_LOG_ERROR, "DimScriptNet", __VA_ARGS__); ds_console_log(1, __VA_ARGS__); } while (0)
#else
#define LOG(...) do { ds_console_log(0, __VA_ARGS__); } while (0)
#define LOGERR(...) do { ds_console_log(1, __VA_ARGS__); } while (0)
#endif

#define URL 512
#define BODY 1024
#define RESP 4096
#define CHAT_RESP 8192
#define WRITE_TICK 50
#define READ_TICK 50
#define TIMEOUT 5000
#define STALE 12000
#define CHAT_MAX 32
#define CHAT_TEXT_MAX 96

typedef struct { double x, y, a, hp, alive; int online; } Actor;
typedef struct { double x, y, dx, dy, active, shot, tr; } Bullet;
typedef struct { char uid[24]; char text[CHAT_TEXT_MAX]; unsigned long ts; int valid; } ChatMsg;

static struct {
    ds_net_thread_t thread, rthread; ds_net_mutex_t lock;
#ifdef __ANDROID__
    JavaVM *vm;
#endif
    int run, started, slot, status;
    char base[256], room[48], uid[24];
    Actor me; Bullet my_bullet; unsigned long seq, count;
    Actor players[NET_SLOTS]; Bullet bullets[NET_SLOTS];
    ChatMsg chats[CHAT_MAX]; int chat_count;
} net;

static char s_chat_text_ret[CHAT_TEXT_MAX];
static char s_chat_uid_ret[24];

static long long net_now_ms(void) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    return (long long)GetTickCount64();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000 + t.tv_nsec/1000000;
#endif
}

static void net_sleep_ms(int ms) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&t, NULL);
#endif
}

static double safe(double v) { return isnan(v)||isinf(v) ? 0 : v; }
static void lock(void) { NET_MUTEX_LOCK(&net.lock); }
static void unlock(void) { NET_MUTEX_UNLOCK(&net.lock); }
static void status(int s) { lock(); net.status = s; unlock(); }

#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm) { net.vm = vm; }

static int http_ex(const char *method, const char *url, const char *body, char *out, size_t cap,
                   const char *header, const char *value, char *etag, size_t etag_cap) {
    JNIEnv *env = NULL; jobject conn = NULL, stream = NULL, urlobj = NULL;
    jclass urlc, connc, streamc; jbyteArray buf; jstring ju, jm;
    int code = 0, attached = 0, ok = 0; size_t total = 0;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!net.vm) return 0;
    if ((*net.vm)->GetEnv(net.vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*net.vm)->AttachCurrentThread(net.vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    }
    if ((*env)->PushLocalFrame(env, 32) != 0) goto done;
    urlc = (*env)->FindClass(env, "java/net/URL");
    connc = (*env)->FindClass(env, "java/net/HttpURLConnection");
    if (!urlc || !connc) goto done;
    ju = (*env)->NewStringUTF(env, url);
    urlobj = (*env)->NewObject(env, urlc, (*env)->GetMethodID(env, urlc, "<init>", "(Ljava/lang/String;)V"), ju);
    if (!urlobj || (*env)->ExceptionCheck(env)) goto done;
    conn = (*env)->CallObjectMethod(env, urlobj, (*env)->GetMethodID(env, urlc, "openConnection", "()Ljava/net/URLConnection;"));
    if (!conn || (*env)->ExceptionCheck(env)) goto done;
    jm = (*env)->NewStringUTF(env, method);
    (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "setRequestMethod", "(Ljava/lang/String;)V"), jm);
    (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "setConnectTimeout", "(I)V"), 5000);
    (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "setReadTimeout", "(I)V"), 5000);
    (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "setUseCaches", "(Z)V"), JNI_FALSE);
    {
        jmethodID set_h = (*env)->GetMethodID(env, connc, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
        (*env)->CallVoidMethod(env, conn, set_h, (*env)->NewStringUTF(env, "Content-Type"), (*env)->NewStringUTF(env, "application/json"));
        if (header && value) (*env)->CallVoidMethod(env, conn, set_h, (*env)->NewStringUTF(env, header), (*env)->NewStringUTF(env, value));
    }
    if (body && *body) {
        jobject os; jsize len = (jsize)strlen(body);
        (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "setDoOutput", "(Z)V"), JNI_TRUE);
        os = (*env)->CallObjectMethod(env, conn, (*env)->GetMethodID(env, connc, "getOutputStream", "()Ljava/io/OutputStream;"));
        if (os && !(*env)->ExceptionCheck(env)) {
            jbyteArray data = (*env)->NewByteArray(env, len);
            (*env)->SetByteArrayRegion(env, data, 0, len, (const jbyte*)body);
            (*env)->CallVoidMethod(env, os, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, os), "write", "([B)V"), data);
            (*env)->CallVoidMethod(env, os, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, os), "close", "()V"));
        }
    }
    code = (int)(*env)->CallIntMethod(env, conn, (*env)->GetMethodID(env, connc, "getResponseCode", "()I"));
    if ((*env)->ExceptionCheck(env)) { code = 0; goto done; }
    if (etag && etag_cap) {
        jstring val = (jstring)(*env)->CallObjectMethod(env, conn, (*env)->GetMethodID(env, connc, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;"), (*env)->NewStringUTF(env, "ETag"));
        if (val && !(*env)->ExceptionCheck(env)) { const char *s = (*env)->GetStringUTFChars(env, val, NULL); if (s) { snprintf(etag, etag_cap, "%s", s); (*env)->ReleaseStringUTFChars(env, val, s); } }
    }
    stream = (*env)->CallObjectMethod(env, conn, (*env)->GetMethodID(env, connc, code >= 400 ? "getErrorStream" : "getInputStream", "()Ljava/io/InputStream;"));
    if (stream && !(*env)->ExceptionCheck(env)) {
        streamc = (*env)->GetObjectClass(env, stream); buf = (*env)->NewByteArray(env, 2048);
        for (;;) {
            jint n = (*env)->CallIntMethod(env, stream, (*env)->GetMethodID(env, streamc, "read", "([B)I"), buf);
            if ((*env)->ExceptionCheck(env) || n <= 0) break;
            if (out && cap && total + (size_t)n < cap) { (*env)->GetByteArrayRegion(env, buf, 0, n, (jbyte*)(out + total)); total += (size_t)n; out[total] = '\0'; }
        }
        (*env)->CallVoidMethod(env, stream, (*env)->GetMethodID(env, streamc, "close", "()V"));
    }
    (*env)->CallVoidMethod(env, conn, (*env)->GetMethodID(env, connc, "disconnect", "()V"));
    ok = 1;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*net.vm)->DetachCurrentThread(net.vm);
    return ok ? code : 0;
}
#elif defined(_WIN32)
#include <windows.h>
#include <wininet.h>
static HINTERNET g_hInternet = NULL;
static int http_ex(const char *method, const char *url, const char *body, char *out, size_t cap,
                   const char *header, const char *value, char *etag, size_t etag_cap) {
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!url || !*url) return 0;
    if (!g_hInternet) g_hInternet = InternetOpenA("DimScriptNet/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!g_hInternet) return 0;
    char scheme[16] = {0}, host[128] = {0}, path[512] = {0};
    URL_COMPONENTSA uc = { sizeof(uc), scheme, sizeof(scheme), 0, host, sizeof(host), 0, NULL, 0, path, sizeof(path), NULL, 0 };
    if (!InternetCrackUrlA(url, (DWORD)strlen(url), 0, &uc)) return 0;
    int is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    HINTERNET hConn = InternetConnectA(g_hInternet, host, uc.nPort ? uc.nPort : (is_https ? 443 : 80), NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) return 0;
    DWORD flags = (is_https ? INTERNET_FLAG_SECURE : 0) | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_DONT_CACHE;
    HINTERNET hReq = HttpOpenRequestA(hConn, method, path, NULL, NULL, NULL, flags, 0);
    if (!hReq) { InternetCloseHandle(hConn); return 0; }
    char hdrs[512] = "Content-Type: application/json\r\n";
    if (header && value && *header && *value) snprintf(hdrs + strlen(hdrs), sizeof(hdrs) - strlen(hdrs), "%s: %s\r\n", header, value);
    BOOL sent = HttpSendRequestA(hReq, hdrs, (DWORD)strlen(hdrs), (LPVOID)body, body ? (DWORD)strlen(body) : 0);
    if (!sent) { InternetCloseHandle(hReq); InternetCloseHandle(hConn); return 0; }
    DWORD status_code = 0, code_sz = sizeof(status_code);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status_code, &code_sz, NULL);
    if (etag && etag_cap) { DWORD el = (DWORD)etag_cap; if (!HttpQueryInfoA(hReq, HTTP_QUERY_ETAG, etag, &el, NULL)) etag[0] = '\0'; }
    if (out && cap) {
        size_t tot = 0; DWORD rd = 0; char buf[1024];
        while (InternetReadFile(hReq, buf, sizeof(buf), &rd) && rd > 0) {
            if (tot + rd < cap) { memcpy(out + tot, buf, rd); tot += rd; out[tot] = '\0'; }
        }
    }
    InternetCloseHandle(hReq); InternetCloseHandle(hConn);
    return (int)status_code;
}
#else
static int http_ex(const char *m, const char *u, const char *b, char *o, size_t c, const char *h, const char *v, char *e, size_t ec) {
    (void)m; (void)u; (void)b; (void)h; (void)v; if (o && c) o[0] = '\0'; if (e && ec) e[0] = '\0'; return 0;
}
#endif

static int http(const char *m, const char *u, const char *b, char *o, size_t c) { return http_ex(m, u, b, o, c, NULL, NULL, NULL, 0); }

static const char *skip_ws(const char *p) { while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *skip_str(const char *p) { if (!p || *p != '"') return NULL; for (p++; *p; p++) { if (*p == '\\' && p[1]) { p++; continue; } if (*p == '"') return p + 1; } return NULL; }
static const char *skip_box(const char *p, char o, char c) { int d = 0; if (!p || *p != o) return NULL; for (; *p; p++) { if (*p == '"') { p = skip_str(p); if (!p) return NULL; p--; continue; } if (*p == o) d++; else if (*p == c && --d == 0) return p + 1; } return NULL; }
static const char *skip_val(const char *p) { p = skip_ws(p); if (!p || !*p) return NULL; if (*p == '"') return skip_str(p); if (*p == '{') return skip_box(p, '{', '}'); if (*p == '[') return skip_box(p, '[', ']'); while (*p && *p != ',' && *p != '}' && *p != ']') p++; return p; }

static const char *member(const char *o, const char *key) {
    size_t n = strlen(key); const char *p = skip_ws(o); if (!p || *p != '{') return NULL;
    for (p = skip_ws(p + 1); p && *p && *p != '}';) {
        const char *name = p, *end = skip_str(p); if (!end) return NULL; p = skip_ws(end); if (*p != ':') return NULL; p = skip_ws(p + 1);
        if ((size_t)(end - name - 2) == n && strncmp(name + 1, key, n) == 0) return p;
        p = skip_val(p); p = skip_ws(p); if (p && *p == ',') p = skip_ws(p + 1);
    }
    return NULL;
}
static const char *element(const char *a, size_t wanted) {
    const char *p = skip_ws(a); size_t idx = 0; if (!p || *p != '[') return NULL;
    for (p = skip_ws(p + 1); p && *p && *p != ']'; idx++) {
        if (idx == wanted) return p;
        p = skip_val(p); p = skip_ws(p); if (p && *p == ',') p = skip_ws(p + 1); else break;
    }
    return NULL;
}
static const char *path_val(const char *json, const char *path) {
    char part[48], *end; const char *v = json;
    while (path && *path && v) {
        const char *slash = strchr(path, '/'); size_t n = slash ? (size_t)(slash - path) : strlen(path);
        if (!n || n >= sizeof(part)) return NULL;
        memcpy(part, path, n); part[n] = 0; v = skip_ws(v);
        if (*v == '[') { size_t idx = strtoul(part, &end, 10); if (!*part || *end) return NULL; v = element(v, idx); }
        else v = member(v, part);
        path = slash ? slash + 1 : path + n;
    }
    return v;
}
static double num(const char *json, const char *path, double fb) { const char *v = path_val(json, path); if (!v || !strncmp(v, "null", 4)) return fb; if (*v == 't') return 1; if (*v == 'f') return 0; if (*v == '"') v++; return atof(v); }
static void strv(const char *json, const char *path, char *out, size_t cap) {
    const char *v = path_val(json, path); size_t i = 0; if (cap) out[0] = 0; if (!v || *v != '"') return;
    for (v++; *v && *v != '"' && i + 1 < cap; v++) { if (*v == '\\' && v[1]) v++; out[i++] = *v; } out[i] = 0;
}

static void json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0; if (!cap) return;
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if ((unsigned char)c >= 0x20) dst[o++] = c;
    }
    dst[o] = '\0';
}

static int push_state(void) {
    Actor a; Bullet b; int slot; unsigned long seq; char url[URL], body[BODY];
    lock(); a = net.me; b = net.my_bullet; slot = net.slot; seq = ++net.seq; unlock();
    if (slot < 0) return 0;
    snprintf(url, sizeof(url), "%s/rooms/%s.json", net.base, net.room);
    snprintf(body, sizeof(body), "{\"players/%d\":{\"uid\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"angle\":%.4f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu},\"bullets/%d\":{\"x\":%.2f,\"y\":%.2f,\"dx\":%.4f,\"dy\":%.4f,\"active\":%.0f,\"shot\":%.0f,\"tr\":%.1f}}",
        slot, net.uid, safe(a.x), safe(a.y), safe(a.a), safe(a.hp), safe(a.alive), seq,
        slot, safe(b.x), safe(b.y), safe(b.dx), safe(b.dy), safe(b.active), safe(b.shot), safe(b.tr));
    return http("PATCH", url, body, NULL, 0) == 200;
}
static int push_bullet_only(void) {
    Bullet b; int slot; char url[URL], body[BODY];
    lock(); b = net.my_bullet; slot = net.slot; unlock();
    if (slot < 0) return 0;
    snprintf(url, sizeof(url), "%s/rooms/%s/bullets/%d.json", net.base, net.room, slot);
    snprintf(body, sizeof(body), "{\"x\":%.2f,\"y\":%.2f,\"dx\":%.4f,\"dy\":%.4f,\"active\":%.0f,\"shot\":%.0f,\"tr\":%.1f}",
        safe(b.x), safe(b.y), safe(b.dx), safe(b.dy), safe(b.active), safe(b.shot), safe(b.tr));
    return http("PUT", url, body, NULL, 0) == 200;
}
static int pull_state(char *resp, size_t cap) { char url[URL]; snprintf(url, sizeof(url), "%s/rooms/%s.json", net.base, net.room); return http("GET", url, NULL, resp, cap) == 200; }
static void release_slot(void) {
    int slot; char url[URL]; lock(); slot = net.slot; net.slot = -1; unlock(); if (slot < 0) return;
    snprintf(url, sizeof(url), "%s/rooms/%s/players/%d.json", net.base, net.room, slot); http("DELETE", url, NULL, NULL, 0);
    snprintf(url, sizeof(url), "%s/rooms/%s/bullets/%d.json", net.base, net.room, slot); http("DELETE", url, NULL, NULL, 0);
}
static int claim_slot(void) {
    static unsigned long seen_seq[NET_SLOTS]; static long long seen_at[NET_SLOTS]; static char seen_uid[NET_SLOTS][24];
    char resp[RESP], url[URL], body[BODY], uid[24], etag[96]; long long t = net_now_ms();
    for (int slot = 0; slot < NET_SLOTS; slot++) {
        snprintf(url, sizeof(url), "%s/rooms/%s/players/%d.json", net.base, net.room, slot);
        if (http_ex("GET", url, NULL, resp, sizeof(resp), "X-Firebase-ETag", "true", etag, sizeof(etag)) != 200 || !etag[0]) continue;
        strv(resp, "uid", uid, sizeof(uid));
        if (uid[0] && !strcmp(uid, net.uid)) return slot;
        unsigned long seq = (unsigned long)num(resp, "seq", 0); int claim = 0;
        if (!uid[0]) claim = 1;
        else if (strcmp(uid, seen_uid[slot]) || seq != seen_seq[slot]) { snprintf(seen_uid[slot], sizeof(seen_uid[slot]), "%s", uid); seen_seq[slot] = seq; seen_at[slot] = t; }
        else if (t - seen_at[slot] >= STALE) claim = 1;
        if (!claim) continue;
        snprintf(body, sizeof(body), "{\"uid\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0}", net.uid);
        if (http_ex("PUT", url, body, NULL, 0, "if-match", etag, NULL, 0) == 200) {
            seen_uid[slot][0] = 0; seen_seq[slot] = seen_at[slot] = 0; return slot;
        }
    }
    return -1;
}
static void read_players(const char *resp) {
    static unsigned long lseq[NET_SLOTS]; static long long lch[NET_SLOTS];
    Actor ps[NET_SLOTS]; Bullet bs[NET_SLOTS]; long long t = net_now_ms(); int local, count = 0;
    memset(ps, 0, sizeof(ps)); memset(bs, 0, sizeof(bs));
    lock(); local = net.slot; unlock();
    for (int slot = 0; slot < NET_SLOTS; slot++) {
        char bp[24], p[40], uid[24];
        if (slot == local) { lock(); ps[slot] = net.me; bs[slot] = net.my_bullet; ps[slot].online = local >= 0; unlock(); if (local >= 0) count++; continue; }
        snprintf(bp, sizeof(bp), "players/%d", slot); snprintf(p, sizeof(p), "%s/uid", bp); strv(resp, p, uid, sizeof(uid));
        if (!uid[0] || !strcmp(uid, net.uid)) { lseq[slot] = lch[slot] = 0; continue; }
        snprintf(p, sizeof(p), "%s/seq", bp); unsigned long sq = (unsigned long)num(resp, p, 0);
        if (sq != lseq[slot]) { lseq[slot] = sq; lch[slot] = t; } else if (!lch[slot]) lch[slot] = t;
        if (t - lch[slot] >= TIMEOUT) continue;
        snprintf(p, sizeof(p), "%s/x", bp); ps[slot].x = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/y", bp); ps[slot].y = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/angle", bp); ps[slot].a = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/hp", bp); ps[slot].hp = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/alive", bp); ps[slot].alive = num(resp, p, 0);
        ps[slot].online = 1;
        snprintf(bp, sizeof(bp), "bullets/%d", slot);
        snprintf(p, sizeof(p), "%s/x", bp); bs[slot].x = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/y", bp); bs[slot].y = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/dx", bp); bs[slot].dx = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/dy", bp); bs[slot].dy = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/active", bp); bs[slot].active = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/shot", bp); bs[slot].shot = num(resp, p, 0);
        snprintf(p, sizeof(p), "%s/tr", bp); bs[slot].tr = num(resp, p, 0);
        count++;
    }
    lock(); memcpy(net.players, ps, sizeof(ps)); memcpy(net.bullets, bs, sizeof(bs)); net.count = count; unlock();
}

static void parse_and_store_chat(const char *json) {
    if (!json || !*json) return;
    ChatMsg tmp[CHAT_MAX]; int cnt = 0; const char *p = json;
    while (*p && cnt < CHAT_MAX) {
        const char *uk = strstr(p, "\"uid\""); if (!uk) break;
        const char *uc = strchr(uk, ':'); if (!uc) break;
        const char *q1 = strchr(uc, '\"'); if (!q1) break; q1++;
        const char *q2 = strchr(q1, '\"'); if (!q2) break;
        char uid[24] = {0}; size_t ul = q2 - q1; if (ul >= sizeof(uid)) ul = sizeof(uid) - 1;
        memcpy(uid, q1, ul);
        const char *tk = strstr(q2, "\"text\""); if (!tk) break;
        const char *tc = strchr(tk, ':'); if (!tc) { p = tk + 6; continue; }
        const char *tq1 = strchr(tc, '\"'); if (!tq1) { p = tc + 1; continue; } tq1++;
        const char *tq2 = tq1; while (*tq2 && (*tq2 != '\"' || *(tq2 - 1) == '\\')) tq2++;
        if (!*tq2) break;
        char text[CHAT_TEXT_MAX] = {0}; size_t tl = tq2 - tq1, oi = 0;
        if (tl >= CHAT_TEXT_MAX) tl = CHAT_TEXT_MAX - 1;
        for (size_t i = 0; i < tl && oi + 1 < sizeof(text); i++) {
            if (tq1[i] == '\\' && i + 1 < tl) { char esc = tq1[++i]; text[oi++] = (esc == 'n') ? ' ' : esc; }
            else text[oi++] = tq1[i];
        }
        strncpy(tmp[cnt].uid, uid, sizeof(tmp[cnt].uid) - 1);
        strncpy(tmp[cnt].text, text, sizeof(tmp[cnt].text) - 1);
        tmp[cnt].valid = 1; cnt++; p = tq2 + 1;
    }
    lock();
    if (cnt > 0) {
        int st = cnt > CHAT_MAX ? cnt - CHAT_MAX : 0; net.chat_count = 0;
        for (int i = st; i < cnt; i++) net.chats[net.chat_count++] = tmp[i];
    }
    unlock();
}
static int pull_chat(char *resp, size_t cap) {
    char url[URL]; snprintf(url, sizeof(url), "%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=20", net.base, net.room);
    return http("GET", url, NULL, resp, cap) == 200;
}

static void *thread_main(void *arg) {
    int fails = 0; (void)arg;
    while (net.run) {
        long long st = net_now_ms(); int slot; lock(); slot = net.slot; unlock();
        if (slot < 0) {
            status(NET_CONNECTING); slot = claim_slot();
            if (slot < 0) { if (++fails > 3) status(NET_ERROR); net_sleep_ms(500); continue; }
            lock(); net.slot = slot; net.seq = 0; net.players[slot] = net.me; net.bullets[slot] = net.my_bullet; net.players[slot].online = 1; unlock(); fails = 0;
        }
        if (!push_state()) { if (++fails > 3) status(NET_ERROR); net_sleep_ms(300); continue; }
        fails = 0; status(NET_PLAYING);
        long long sp = net_now_ms() - st; if (sp < WRITE_TICK) net_sleep_ms((int)(WRITE_TICK - sp));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP]; (void)arg;
    while (net.run) {
        long long st = net_now_ms(); int slot; lock(); slot = net.slot; unlock();
        if (slot >= 0) {
            if (pull_state(resp, sizeof(resp))) read_players(resp);
            if (pull_chat(chat_resp, sizeof(chat_resp))) parse_and_store_chat(chat_resp);
        }
        long long sp = net_now_ms() - st; if (sp < READ_TICK) net_sleep_ms((int)(READ_TICK - sp));
    }
    return NULL;
}

void net_connect(const char *url, const char *room) {
    if (net.run || !url || !*url) return;
    memset(&net.me, 0, sizeof(net.me)); memset(net.players, 0, sizeof(net.players)); memset(net.chats, 0, sizeof(net.chats));
    net.count = net.chat_count = 0; net.slot = -1; net.seq = 0;
    snprintf(net.base, sizeof(net.base), "%s", url); size_t n = strlen(net.base); while (n && net.base[n-1] == '/') net.base[--n] = 0;
    snprintf(net.room, sizeof(net.room), "%s", (room && *room) ? room : "main");
    if (!net.uid[0]) snprintf(net.uid, sizeof(net.uid), "%08lx%08lx", (unsigned long)time(NULL), (unsigned long)net_now_ms());
    if (!net.started) { NET_MUTEX_INIT(&net.lock); net.started = 1; }
    net.status = NET_CONNECTING; net.run = 1;
    if (ds_thread_create(&net.thread, thread_main, NULL)) { net.run = 0; net.status = NET_ERROR; return; }
    if (ds_thread_create(&net.rthread, reader_thread, NULL)) { net.run = 0; ds_thread_join(net.thread); net.status = NET_ERROR; return; }
}
void net_disconnect(void) { if (!net.run) return; net.run = 0; ds_thread_join(net.thread); ds_thread_join(net.rthread); net.status = NET_OFFLINE; net.slot = -1; net.count = 0; }
void net_publish(double x, double y, double a, double hp, double alive) {
    if (!net.started) return;
    lock(); net.me.x = x; net.me.y = y; net.me.a = a; net.me.hp = hp; net.me.alive = alive;
    if (net.slot >= 0) { net.players[net.slot] = net.me; net.players[net.slot].online = 1; } unlock();
}
void net_publish_bullet(double x, double y, double dx, double dy, double active, double shot, double tr) {
    if (!net.started) return;
    lock(); net.my_bullet.x = x; net.my_bullet.y = y; net.my_bullet.dx = dx; net.my_bullet.dy = dy; net.my_bullet.active = active; net.my_bullet.shot = shot; net.my_bullet.tr = tr;
    if (net.slot >= 0) net.bullets[net.slot] = net.my_bullet; unlock();
    if (active > 0.5) push_bullet_only();
}

void net_chat_send(const char *text) {
    if (!net.started || !net.run || !text || !*text) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2];
    json_escape(text, esc, sizeof(esc));
    snprintf(url, sizeof(url), "%s/rooms/%s/chat.json", net.base, net.room);
    snprintf(body, sizeof(body), "{\"uid\":\"%s\",\"text\":\"%s\",\"ts\":%lld}", net.uid, esc, net_now_ms());
    http("POST", url, body, NULL, 0);
}
double net_chat_count(void) { double v; lock(); v = net.chat_count; unlock(); return v; }
const char* net_chat_text(double idx) {
    int i = (int)idx; lock();
    if (i >= 0 && i < net.chat_count && net.chats[i].valid) { snprintf(s_chat_text_ret, sizeof(s_chat_text_ret), "%s", net.chats[i].text); }
    else s_chat_text_ret[0] = '\0';
    unlock(); return s_chat_text_ret;
}
const char* net_chat_uid(double idx) {
    int i = (int)idx; lock();
    if (i >= 0 && i < net.chat_count && net.chats[i].valid) { snprintf(s_chat_uid_ret, sizeof(s_chat_uid_ret), "%s", net.chats[i].uid); }
    else s_chat_uid_ret[0] = '\0';
    unlock(); return s_chat_uid_ret;
}
double net_chat_time(double idx) { int i = (int)idx; double v = 0; lock(); if (i >= 0 && i < net.chat_count) v = (double)net.chats[i].ts; unlock(); return v; }

static int sidx(double slot) { int i = (int)slot; return i >= 0 && i < NET_SLOTS ? i : -1; }
double net_status(void) { double v; lock(); v = net.status; unlock(); return v; }
double net_slot(void) { double v; lock(); v = net.slot; unlock(); return v; }
double net_count(void) { double v; lock(); v = net.count; unlock(); return v; }

#define READER(name, field) double name(double slot) { int i = sidx(slot); double v = 0; if (i >= 0) { lock(); v = field; unlock(); } return v; }
READER(net_player_online, net.players[i].online ? 1 : 0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
READER(net_player_bullet_active, net.bullets[i].active)
READER(net_player_bullet_x, net.bullets[i].x)
READER(net_player_bullet_y, net.bullets[i].y)
READER(net_player_bullet_dx, net.bullets[i].dx)
READER(net_player_bullet_dy, net.bullets[i].dy)
READER(net_player_bullet_shot, net.bullets[i].shot)
READER(net_player_bullet_tr, net.bullets[i].tr)
#undef READER
