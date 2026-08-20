#include "net.h"
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <wininet.h>
#else
#include <pthread.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#endif
#ifdef __ANDROID__
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
/* Бой обновляется часто, чат — отдельно раз в секунду. Раньше каждый цикл
 * скачивал всю комнату вместе со всей историей чата, а потом ещё раз чат.
 * По мере роста истории это и создавало основную часть сетевых лагов. */
#define WRITE_TICK 60
#define READ_TICK 60
#define CHAT_TICK 1000
/* Ивент (диско) читаем реже боя — это крошечный узел, реагировать на него с
 * задержкой в полсекунды для мигания более чем достаточно. */
#define EVENT_TICK 500
/* Таймауты HTTP: 4 секунды вместо 2 — на мобильном интернете короткие
 * всплески задержки (DNS, переподключение к вышке) раньше выглядели как
 * «Нет соединения» у второго игрока. */
#define TIMEOUT 4000
/* Через сколько секунд молчания чужой слот считается «призрачным» (приложение
 * убили без выхода из онлайна) и занимается. Раньше 12 с — друг, перезашедший
 * после краша, видел «Подключение.../Нет соединения» почти полминуты. */
#define STALE 6000
#define CHAT_MAX 32
#define CHAT_TEXT_MAX 96
#define LOGIN_NICK_MAX 16
#define SESSION_FILE "auth.dat"
#define PROGRESS_FILE "progress.dat"
#ifdef _WIN32
typedef SRWLOCK DSMutex;
#define DS_MUTEX_INIT SRWLOCK_INIT
#define ds_mutex_lock(m) AcquireSRWLockExclusive(&(m))
#define ds_mutex_unlock(m) ReleaseSRWLockExclusive(&(m))
typedef HANDLE DSThread;
static DSThread ds_thread_start(unsigned (__stdcall *fn)(void *), void *arg) {
    return (DSThread)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
}
static void ds_thread_join(DSThread t) { if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
static void ds_thread_detach(DSThread t) { if (t) CloseHandle(t); }
#else
typedef pthread_mutex_t DSMutex;
#define DS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define ds_mutex_lock(m) pthread_mutex_lock(&(m))
#define ds_mutex_unlock(m) pthread_mutex_unlock(&(m))
typedef pthread_t DSThread;
static DSThread ds_thread_start(void *(*fn)(void *), void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return (pthread_t)0;
    return t;
}
static void ds_thread_join(DSThread t) { if (t) pthread_join(t, NULL); }
static void ds_thread_detach(DSThread t) { if (t) pthread_detach(t); }
#endif
/* Удар хранится рядом с игроком. Один маленький GET /players.json теперь
 * содержит всё, что нужно бою; история чата в этот ответ не попадает. */
typedef struct {
    double x,y,a,hp,alive;
    double punch_x,punch_y,punch_dx,punch_dy,punch;
    double cls;
    int online;
    char nick[24];
} Actor;
typedef struct { char uid[24]; char nick[24]; char text[CHAT_TEXT_MAX]; int valid; } ChatMsg;
static struct {
    DSThread thread, rthread; DSMutex lock;
#ifdef __ANDROID__
    JavaVM *vm;
#endif
    int run, started, slot, status;
    char base[256], room[48], uid[24];
    Actor me; unsigned long seq, count;
    Actor players[NET_SLOTS];
    ChatMsg chats[CHAT_MAX]; int chat_count;
    /* Ивент комнаты: маленький узел rooms/<room>/event. 1 = «диско» режим
     * включён, 0 = никакого ивента. Читается отдельно от боя, как чат. */
    int event;
} net = { .lock = DS_MUTEX_INIT };
/* Пока идёт отключение (net_disconnect), HTTP-запросы используют короткий
 * таймаут, чтобы игра не замирала на секунды, если сеть «мертва».
 * Читается сетевыми потоками, пишется игровым — volatile sig_atomic_t:
 * один флаг, чтение/запись int на всех целевых платформах атомарны,
 * и это работает в режиме C99 (MSVC без /std:c11 не знает <stdatomic.h>). */
static volatile sig_atomic_t net_fast = 0;
static char s_chat_text_ret[CHAT_TEXT_MAX];
static char s_chat_uid_ret[24];
static char s_nick_ret[24];
static char s_lg_nick_ret[24];
static long long now_ms(void);
static void lock(void);
static void unlock(void);

/* Вход в онлайн — только ник: ни пароля, ни запросов в сеть, ни потоков.
 * Ник хранится в session_nick, сохраняется в файл и читается при старте. */
typedef struct {
    DSMutex lock;
    int status;
    char session_nick[LOGIN_NICK_MAX+1];
    char path[256];
} Login;
static Login lg = { .lock = DS_MUTEX_INIT };
static void lg_lock(void) { ds_mutex_lock(lg.lock); }
static void lg_unlock(void) { ds_mutex_unlock(lg.lock); }

/* Кубки и класс живут рядом с ником: тот же каталог, отдельный файл, чтобы
 * смена ника не обнуляла прогресс. pending_cls нужен, потому что net_connect
 * обнуляет net.me уже после выбора класса в меню. */
typedef struct {
    DSMutex lock;
    int loaded, cups, cls, azum;
} Progress;
static Progress pg = { .lock = DS_MUTEX_INIT };
static int pending_cls = 0;
static void pg_lock(void) { ds_mutex_lock(pg.lock); }
static void pg_unlock(void) { ds_mutex_unlock(pg.lock); }
static void data_file_path(char *path, size_t cap, const char *name) {
    lg_lock();
    if (lg.path[0]) snprintf(path, cap, "%s/%s", lg.path, name);
    else snprintf(path, cap, "%s", name);
    lg_unlock();
}
static void progress_write(int cups, int cls, int azum) {
    char path[320]; FILE *f;
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "w");
    if (f) { fprintf(f, "%d %d %d\n", cups, cls, azum); fclose(f); }
}
static void progress_read(void) {
    char path[320]; FILE *f; int cups=0, cls=0, azum=0;
    pg_lock();
    if (pg.loaded) { pg_unlock(); return; }
    pg_unlock();
    data_file_path(path, sizeof(path), PROGRESS_FILE);
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d %d %d", &cups, &cls, &azum) < 1) { cups=0; cls=0; azum=0; }
        fclose(f);
    }
    if (cups < 0) cups = 0;
    if (cls != 1) cls = 0;
    azum = azum ? 1 : 0;
    if (cls == 1 && !azum) cls = 0;
    pg_lock(); pg.cups = cups; pg.cls = cls; pg.azum = azum; pg.loaded = 1; pg_unlock();
}
double net_load_cups(void) { double v; progress_read(); pg_lock(); v = (double)pg.cups; pg_unlock(); return v; }
double net_load_class(void) { double v; progress_read(); pg_lock(); v = (double)pg.cls; pg_unlock(); return v; }
double net_load_azum(void) { double v; progress_read(); pg_lock(); v = (double)pg.azum; pg_unlock(); return v; }
void net_save_progress(double cups, double cls, double azum) {
    int c = (int)cups, k = ((int)cls == 1) ? 1 : 0, a = azum ? 1 : 0;
    if (c < 0) c = 0;
    if (k == 1 && !a) k = 0;
    pg_lock(); pg.cups = c; pg.cls = k; pg.azum = a; pg.loaded = 1; pg_unlock();
    pending_cls = k;
    progress_write(c, k, a);
}
void net_set_class(double cls) {
    int k = ((int)cls == 1) ? 1 : 0;
    pending_cls = k;
    lock(); net.me.cls = (double)k;
    if (net.slot >= 0) net.players[net.slot].cls = (double)k;
    unlock();
}

static int nick_valid(const char *n) {
    /* Ник может содержать русские буквы в UTF-8, латиницу, цифры и _. */
    size_t i=0, bytes=n ? strlen(n) : 0, chars=0;
    if (bytes < 3 || bytes > LOGIN_NICK_MAX) return 0;
    while (i < bytes) {
        unsigned char c=(unsigned char)n[i]; unsigned long cp=0; size_t need=0;
        if (c<0x80) { cp=c; need=1; }
        else if ((c&0xE0)==0xC0) { cp=c&0x1F; need=2; }
        else if ((c&0xF0)==0xE0) { cp=c&0x0F; need=3; }
        else if ((c&0xF8)==0xF0) { cp=c&0x07; need=4; }
        else return 0;
        if (i+need>bytes) return 0;
        size_t k; for(k=1;k<need;k++) { unsigned char q=(unsigned char)n[i+k]; if((q&0xC0)!=0x80) return 0; cp=(cp<<6)|(q&0x3F); }
        if ((need==2&&cp<0x80)||(need==3&&cp<0x800)||(need==4&&cp<0x10000)||cp>0x10FFFF) return 0;
        if ((cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||(cp>='0'&&cp<='9')||cp=='_'||(cp>=0x0400&&cp<=0x04FF)) chars++;
        else return 0;
        i+=need;
    }
    return chars>=3 && chars<=16;
}
static void session_save(const char *nick) {
    char path[320]; FILE *f;
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", nick); fclose(f); }
}
static long long now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
#endif
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L };
    nanosleep(&t, NULL);
#endif
}
static double safe(double v) { if (v != v) return 0; if (v > 1e300 || v < -1e300) return 0; return v; }
static void lock(void) { ds_mutex_lock(net.lock); }
static void unlock(void) { ds_mutex_unlock(net.lock); }
static void status(int s) { lock(); net.status=s; unlock(); }
/* Не пишем сетевые ошибки в консоль чаще раза в 2 секунды,
 * иначе при «мёртвой» сети консоль зальёт тысячами строк. */
static DSMutex net_log_lock = DS_MUTEX_INIT;
static int net_log_ok(void) {
    /* Вызывается из нескольких потоков (writer/reader/login) — под своим
     * мьютексом, иначе гонка по static long long это формально UB.
     * (net.lock здесь специально НЕ берём: http_ex вызывает лог, держа чужие
     * контексты; отдельный мьютекс исключает вложенность.) */
    static long long last = 0;
    long long now = now_ms();
    int ok = 0;
    ds_mutex_lock(net_log_lock);
    if (now - last >= 2000) { last = now; ok = 1; }
    ds_mutex_unlock(net_log_lock);
    return ok;
}
#ifdef __ANDROID__
void net_set_java_vm(JavaVM *vm) { net.vm=vm; }
#endif
#ifdef _WIN32
static int parse_http_url(const char *url, char *host, size_t host_cap, int *port, const char **path, int *secure) {
    const char *p = url;
    *secure = 0; *port = 80;
    if (strncmp(p, "https://", 8) == 0) { *secure = 1; *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return 0;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (!hl || hl >= host_cap) return 0;
    memcpy(host, p, hl); host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; *port = atoi(colon + 1); if (*port <= 0) *port = *secure ? 443 : 80; }
    *path = slash ? slash : "/";
    return 1;
}
#ifdef _WIN32
/* Понятная диагностика сетевых ошибок для консоли игры: код WinINet
 * (12007 = нет DNS, 12029 = не могу соединиться, 12038 = сертификат,
 * 12002 = таймаут...) плюс ответ сервера, если он был. */
static void log_win32_net_error(const char *what, int code, int stage) {
    if (!net_log_ok()) return;
    DWORD err = GetLastError();
    char msg[256] = "";
    if (err) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, 0, msg, sizeof(msg) - 1, NULL);
        size_t mlen = strlen(msg);
        while (mlen && (msg[mlen-1]=='\r'||msg[mlen-1]=='\n'||msg[mlen-1]==' '||msg[mlen-1]=='.')) msg[--mlen] = '\0';
    }
    if (stage == 2) {
        char srv[512] = "";
        DWORD slen = sizeof(srv) - 1, sidx = 0;
        if (InternetGetLastResponseInfoA(&sidx, srv, &slen) && slen > 0) {
            srv[slen] = '\0';
            LOGERR("http %s: HTTP %d, wininet err %lu (%s); server: %s",
                   what, code, err, msg[0] ? msg : "unknown", srv);
            return;
        }
    }
    LOGERR("http %s: HTTP %d, wininet err %lu (%s)", what, code, err, msg[0] ? msg : "unknown");
}
#endif
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    char host[256]; int port = 80, secure = 0; const char *path = NULL;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!url || !parse_http_url(url, host, sizeof(host), &port, &path, &secure)) return 0;
    /* Сначала системные настройки (IE/прокси). Если транспорт не сработал —
     * пробуем прямое соединение: сломанный системный прокси/антивирусный
     * фильтр не должен давать вечное «нет соединения». */
    int modes[2] = { INTERNET_OPEN_TYPE_PRECONFIG, INTERNET_OPEN_TYPE_DIRECT };
    int last_code = 0, last_stage = 1;
    for (int mi = 0; mi < 2; mi++) {
        HINTERNET inet = InternetOpenA("CubicBattle/1.0", (DWORD)modes[mi], NULL, NULL, 0);
        if (!inet) continue;
        HINTERNET conn = InternetConnectA(inet, host, (INTERNET_PORT)port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (!conn) { InternetCloseHandle(inet); continue; }
        {
            DWORD tmo = (DWORD)(net_fast ? 1200 : TIMEOUT);
            InternetSetOptionA(conn, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
            InternetSetOptionA(conn, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));
            InternetSetOptionA(conn, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
        }
        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
        if (secure) flags |= INTERNET_FLAG_SECURE;
        HINTERNET req = HttpOpenRequestA(conn, method, path, NULL, NULL, NULL, flags, 0);
        if (!req) { InternetCloseHandle(conn); InternetCloseHandle(inet); continue; }
        char hdrs[512]; int hl = 0;
        hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "Content-Type: application/json\r\n");
        if (header && value) hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "%s: %s\r\n", header, value);
        if (hl < 0 || (size_t)hl >= sizeof(hdrs)) hl = (int)sizeof(hdrs) - 1;
        hdrs[hl] = '\0';
        BOOL ok = HttpSendRequestA(req, hl ? hdrs : NULL, (DWORD)hl, (LPVOID)body, (DWORD)(body ? strlen(body) : 0));
        int code = 0;
        if (ok) {
            DWORD len = sizeof(code), idx = 0;
            if (HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, &idx)) last_code = code;
        }
        if (etag && etag_cap) {
            DWORD len = 0, idx = 0;
            if (HttpQueryInfoA(req, HTTP_QUERY_ETAG, NULL, &len, &idx) || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                if (len && len < etag_cap) {
                    DWORD got = len;
                    if (HttpQueryInfoA(req, HTTP_QUERY_ETAG, etag, &got, &idx) && got < etag_cap) etag[got] = '\0';
                }
            }
        }
        if (ok && out && cap) {
            size_t total = 0;
            for (;;) {
                DWORD rd = 0;
                if (!InternetReadFile(req, out + total, (DWORD)(cap - 1 - total), &rd) || rd == 0) break;
                total += (size_t)rd;
                out[total] = '\0';
                if (total + 1 >= cap) break;
            }
        }
        InternetCloseHandle(req);
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        if (ok) return code;
        if (code) { last_code = code; last_stage = 2; break; } /* сервер ответил — прокси ни при чём */
        last_code = 0; last_stage = 1;
    }
    log_win32_net_error(method, last_code, last_stage);
    return 0;
}
#elif defined(__ANDROID__)
/* JNI HTTP-клиент с полной проверкой исключений. Если после JNI-вызова
 * остаётся «висящее» исключение и мы делаем следующий JNI-вызов — Android
 * (CheckJNI) убивает процесс: это и был «вылет при создании аккаунта»,
 * когда из-за пропавшего setRequestMethod PUT уходил как GET и Java
 * бросала ProtocolException. Теперь каждый вызов проверяется, а исключение
 * очищается, и вместо вылета игра просто показывает причину в консоли. */
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    JNIEnv *env=NULL; jobject conn=NULL, stream=NULL, urlobj=NULL;
    jclass urlc, connc, streamc; jbyteArray buf; jstring ju, jm;
    int code=0, attached=0, ok=0; size_t total=0;
    if(out&&cap)out[0]='\0';
    if(etag&&etag_cap)etag[0]='\0';
    if (!net.vm) return 0;
    if ((*net.vm)->GetEnv(net.vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK) {
        if ((*net.vm)->AttachCurrentThread(net.vm,&env,NULL)!=JNI_OK) return 0;
        attached=1;
    }
#define JNI_CHECK() do { if ((*env)->ExceptionCheck(env)) goto done; } while (0)
    if ((*env)->PushLocalFrame(env,32)!=0) goto done;
    urlc=(*env)->FindClass(env,"java/net/URL"); JNI_CHECK();
    connc=(*env)->FindClass(env,"java/net/HttpURLConnection"); JNI_CHECK();
    ju=(*env)->NewStringUTF(env,url); JNI_CHECK();
    urlobj=(*env)->NewObject(env,urlc,(*env)->GetMethodID(env,urlc,"<init>","(Ljava/lang/String;)V"),ju); JNI_CHECK();
    conn=(*env)->CallObjectMethod(env,urlobj,(*env)->GetMethodID(env,urlc,"openConnection","()Ljava/net/URLConnection;")); JNI_CHECK();
    jm=(*env)->NewStringUTF(env,method); JNI_CHECK();
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setRequestMethod","(Ljava/lang/String;)V"),jm); JNI_CHECK();
    {
        int tmo = net_fast ? 1200 : TIMEOUT;
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setConnectTimeout","(I)V"),tmo); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setReadTimeout","(I)V"),tmo); JNI_CHECK();
    }
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setUseCaches","(Z)V"),JNI_FALSE); JNI_CHECK();
    {
        jmethodID set_header=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
        jstring k=(*env)->NewStringUTF(env,"Content-Type"), v=(*env)->NewStringUTF(env,"application/json");
        (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK();
        if(header&&value) { k=(*env)->NewStringUTF(env,header); v=(*env)->NewStringUTF(env,value); (*env)->CallVoidMethod(env,conn,set_header,k,v); JNI_CHECK(); }
    }
    if (body&&*body) {
        jobject os=NULL; jbyteArray data; jsize len=(jsize)strlen(body);
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setDoOutput","(Z)V"),JNI_TRUE); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setFixedLengthStreamingMode","(I)V"),len); JNI_CHECK();
        os=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getOutputStream","()Ljava/io/OutputStream;")); JNI_CHECK();
        if(!os) goto done;
        data=(*env)->NewByteArray(env,len); JNI_CHECK();
        (*env)->SetByteArrayRegion(env,data,0,len,(const jbyte*)body);
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"write","([B)V"),data); JNI_CHECK();
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"close","()V")); JNI_CHECK();
    }
    code=(int)(*env)->CallIntMethod(env,conn,(*env)->GetMethodID(env,connc,"getResponseCode","()I")); JNI_CHECK();
    if(etag&&etag_cap) {
        jstring key=(*env)->NewStringUTF(env,"ETag"); JNI_CHECK();
        jstring val=(jstring)(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;"),key); JNI_CHECK();
        if(val) { const char *s=(*env)->GetStringUTFChars(env,val,NULL); if(s){snprintf(etag,etag_cap,"%s",s);(*env)->ReleaseStringUTFChars(env,val,s);} }
    }
    stream=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,code>=400?"getErrorStream":"getInputStream","()Ljava/io/InputStream;")); JNI_CHECK();
    if(!stream) goto closeconn;
    streamc=(*env)->GetObjectClass(env,stream); JNI_CHECK();
    buf=(*env)->NewByteArray(env,2048); JNI_CHECK();
    for (;;) {
        jint n=(*env)->CallIntMethod(env,stream,(*env)->GetMethodID(env,streamc,"read","([B)I"),buf); JNI_CHECK();
        if (n<=0) break;
        if (out&&cap&&total+(size_t)n<cap) { (*env)->GetByteArrayRegion(env,buf,0,n,(jbyte*)(out+total)); total+=(size_t)n; out[total]='\0'; }
    }
    (*env)->CallVoidMethod(env,stream,(*env)->GetMethodID(env,streamc,"close","()V")); JNI_CHECK();
closeconn:
    if (conn) { (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"disconnect","()V")); (*env)->ExceptionClear(env); }
    ok=1;
done:
    if ((*env)->ExceptionCheck(env)) {
        jthrowable ex = (*env)->ExceptionOccurred(env);
        (*env)->ExceptionClear(env);
        if (net_log_ok() && ex) {
            jclass tcls = (*env)->GetObjectClass(env, ex);
            jmethodID gm = tcls ? (*env)->GetMethodID(env, tcls, "getMessage", "()Ljava/lang/String;") : NULL;
            if (gm) {
                jstring jmsg = (jstring)(*env)->CallObjectMethod(env, ex, gm);
                if (jmsg && !(*env)->ExceptionCheck(env)) {
                    const char *s = (*env)->GetStringUTFChars(env, jmsg, NULL);
                    if (s) { LOGERR("http %s: %s", method, s); (*env)->ReleaseStringUTFChars(env, jmsg, s); }
                }
            }
        }
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env,NULL);
    if (attached) (*net.vm)->DetachCurrentThread(net.vm);
    return ok ? code : 0;
#undef JNI_CHECK
}
#else
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    (void)method; (void)url; (void)body; (void)header; (void)value;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    return 0;
}
#endif
static int http(const char *method,const char *url,const char *body,char *out,size_t cap) {
    return http_ex(method,url,body,out,cap,NULL,NULL,NULL,0);
}
static const char *skip_ws(const char *p) { while(p&&*p&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; return p; }
static const char *skip_str(const char *p) { if(!p||*p!='\"') return NULL; for(p++;*p;p++){ if(*p=='\\'&&p[1]){p++;continue;} if(*p=='\"') return p+1; } return NULL; }
static const char *skip_box(const char *p,char open,char close) { int d=0; if(!p||*p!=open) return NULL; for(;*p;p++){ if(*p=='\"'){p=skip_str(p); if(!p)return NULL; p--; continue;} if(*p==open)d++; else if(*p==close&&--d==0)return p+1;} return NULL; }
static const char *skip_val(const char *p) { p=skip_ws(p); if(!p||!*p)return NULL; if(*p=='\"')return skip_str(p); if(*p=='{')return skip_box(p,'{','}'); if(*p=='[')return skip_box(p,'[',']'); while(*p&&*p!=','&&*p!='}'&&*p!=']')p++; return p; }
static const char *member(const char *o,const char *key) {
    size_t n=strlen(key); const char *p=skip_ws(o);
    if(!p||*p!='{') return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!='}';) {
        const char *name=p,*end=skip_str(p); if(!end)return NULL; p=skip_ws(end); if(*p!=':')return NULL; p=skip_ws(p+1);
        if((size_t)(end-name-2)==n&&strncmp(name+1,key,n)==0) return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1);
    }
    return NULL;
}
static const char *element(const char *a,size_t wanted) {
    const char *p=skip_ws(a); size_t index=0;
    if(!p||*p!='[')return NULL;
    for(p=skip_ws(p+1);p&&*p&&*p!=']';index++) {
        if(index==wanted)return p;
        p=skip_val(p); p=skip_ws(p); if(p&&*p==',')p=skip_ws(p+1); else break;
    }
    return NULL;
}
static const char *path_val(const char *json,const char *path) {
    char part[48],*end; const char *v=json;
    while(path&&*path&&v) {
        const char *slash=strchr(path,'/'); size_t n=slash?(size_t)(slash-path):strlen(path),index;
        if(!n||n>=sizeof(part))return NULL;
        memcpy(part,path,n); part[n]=0; v=skip_ws(v);
        if(*v=='[') { index=strtoul(part,&end,10); if(!*part||*end)return NULL; v=element(v,index); }
        else v=member(v,part);
        path=slash?slash+1:path+n;
    }
    return v;
}
static double num(const char *json,const char *path,double fb) { const char *v=path_val(json,path); if(!v||!strncmp(v,"null",4))return fb; if(*v=='t')return 1; if(*v=='f')return 0; if(*v=='\"')v++; return atof(v); }
static void strv(const char *json,const char *path,char *out,size_t cap) {
    const char *v=path_val(json,path); size_t i=0; if(cap)out[0]=0; if(!v||*v!='\"')return;
    for(v++;*v&&*v!='\"'&&i+1<cap;v++){ if(*v=='\\'&&v[1])v++; out[i++]=*v; } out[i]=0;
}
static void json_escape(const char *src, char *dst, size_t cap){
    size_t o=0;
    if(cap==0) return;
    for(size_t i=0; src[i] && o+2<cap; i++){
        char c=src[i];
        if(c=='\"'){ dst[o++]='\\'; dst[o++]='\"'; }
        else if(c=='\\'){ dst[o++]='\\'; dst[o++]='\\'; }
        else if(c=='\n'){ dst[o++]='\\'; dst[o++]='n'; }
        else if(c=='\r'){ dst[o++]='\\'; dst[o++]='r'; }
        else if((unsigned char)c<0x20){  }
        else { dst[o++]=c; }
    }
    dst[o]='\0';
}
static int push_state(void) {
    Actor a; int slot; unsigned long seq; char url[URL], body[BODY], enick[64];
    lock(); a=net.me; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    json_escape(a.nick,enick,sizeof(enick));
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
    /* Пять знаков у координат дают субпиксельную точность. Старые %.2f
     * округляли движение до скачков примерно по 13 px на широком экране. */
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":%.5f,\"y\":%.5f,\"angle\":%.5f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu,\"px\":%.5f,\"py\":%.5f,\"pdx\":%.5f,\"pdy\":%.5f,\"punch\":%.0f,\"cls\":%.0f}",
        net.uid,enick,safe(a.x),safe(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        safe(a.punch_x),safe(a.punch_y),safe(a.punch_dx),safe(a.punch_dy),safe(a.punch),safe(a.cls));
    int c = http("PUT",url,body,NULL,0);
    if (c != 200 && net_log_ok()) LOGERR("push state: HTTP %d (room write denied? check Firebase rules)", c);
    return c == 200;
}

/* Только чат отправляется отдельной фоновой задачей. Удар больше не создаёт
 * по потоку на каждый тап и не может перезаписать новое состояние старым. */
typedef struct { char url[URL]; char body[BODY*2]; } HttpJob;
static void *http_post_job(void *arg) {
    HttpJob *j = (HttpJob*)arg;
    if (j) { http("POST", j->url, j->body, NULL, 0); free(j); }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_http_post(void *arg) { http_post_job(arg); return 0; }
#endif
static void http_post_async(const char *url, const char *body) {
    HttpJob *j = (HttpJob*)malloc(sizeof(*j));
    DSThread t;
    if (!j) return;
    snprintf(j->url, sizeof(j->url), "%s", url);
    snprintf(j->body, sizeof(j->body), "%s", body);
#ifdef _WIN32
    t = ds_thread_start(win_http_post, j);
#else
    t = ds_thread_start(http_post_job, j);
#endif
    if (t) { ds_thread_detach(t); return; }
    free(j);
}
static int pull_state(char *resp,size_t cap) {
    char url[URL];
    /* Важно: читаем только players, а не всю комнату с растущим чатом. */
    snprintf(url,sizeof(url),"%s/rooms/%s/players.json",net.base,net.room);
    int c = http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull state: HTTP %d", c);
    return c == 200;
}
/* Узел event — число 1/0 или true/false прямо в корне базы (не внутри комнаты).
 * Если узла нет (null), num() вернёт запасное значение 0: никакого ивента. */
static int pull_event(char *resp,size_t cap) {
    char url[URL];
    snprintf(url,sizeof(url),"%s/event.json",net.base);
    int c = http("GET",url,NULL,resp,cap);
    if (c != 200 && net_log_ok()) LOGERR("pull event: HTTP %d", c);
    return c == 200;
}

void net_set_data_path(const char *path) {
    lg_lock();
    if (path && *path) snprintf(lg.path, sizeof(lg.path), "%s", path);
    else lg.path[0] = 0;
    lg_unlock();
}
void net_autologin(const char *url) {
    /* Вход без пароля: просто читаем сохранённый ник из файла сессии. */
    char path[320], nick[LOGIN_NICK_MAX+2];
    FILE *f;
    (void)url;
    lg_lock();
    if (lg.status == NET_LOGIN_OK) { lg_unlock(); return; }
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    lg_unlock();
    f = fopen(path, "r");
    if (!f) return;
    if (fscanf(f, "%17s", nick) != 1) { fclose(f); return; }
    fclose(f);
    if (!nick_valid(nick)) return;
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    LOG("autologin: nick '%s'", nick);
}
double net_set_nick(const char *nick) {
    /* Никакой регистрации: проверяем формат, сохраняем ник и пускаем в онлайн. */
    if (!nick || !nick_valid(nick)) return 0.0;
    lg_lock();
    snprintf(lg.session_nick, sizeof(lg.session_nick), "%s", nick);
    lg.status = NET_LOGIN_OK;
    lg_unlock();
    session_save(nick);
    LOG("nick set: '%s'", nick);
    return 1.0;
}
double net_login_status(void) {
    double v;
    lg_lock(); v = (double)lg.status; lg_unlock();
    return v;
}
const char *net_login_nick(void) {
    lg_lock();
    if (lg.status == NET_LOGIN_OK && lg.session_nick[0])
        snprintf(s_lg_nick_ret, sizeof(s_lg_nick_ret), "%s", lg.session_nick);
    else s_lg_nick_ret[0] = 0;
    lg_unlock();
    return s_lg_nick_ret;
}
static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    static unsigned long seen_seq[NET_SLOTS]; static long long seen_at[NET_SLOTS]; static char seen_uid[NET_SLOTS][24];
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        if (!net.run) return -1; /* отключение: не ждём все слоты */
        snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
        code=http_ex("GET",url,NULL,resp,sizeof(resp),"X-Firebase-ETag","true",etag,sizeof(etag));
        if(code!=200) { if(net_log_ok()) LOGERR("claim slot %d: HTTP %d", slot, code); continue; }
        strv(resp,"uid",uid,sizeof(uid));
        if(uid[0]&&!strcmp(uid,net.uid))return slot;
        seq=(unsigned long)num(resp,"seq",0);
        if(!uid[0])claim=1;
        else if(etag[0]&&(strcmp(uid,seen_uid[slot])||seq!=seen_seq[slot])) { snprintf(seen_uid[slot],sizeof(seen_uid[slot]),"%s",uid); seen_seq[slot]=seq; seen_at[slot]=t; }
        else if(etag[0]&&t-seen_at[slot]>=STALE)claim=1;
        if(!claim)continue;
        if(!etag[0]) {
            /* Сервер/клиент не отдали ETag (например, WinINet на ПК): занимаем
             * только гарантированно пустой слот, без if-match. Раньше такой
             * слот просто пропускался — и на ПК получалось вечное
             * «нет соединения». */
            if(net_log_ok()) LOG("claim slot %d: no ETag, claiming empty slot without if-match", slot);
        }
        {
            char enick[64];
            json_escape(net.me.nick,enick,sizeof(enick));
            snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0,\"px\":0,\"py\":0,\"pdx\":0,\"pdy\":0,\"punch\":0,\"cls\":%.0f}",net.uid,enick,safe(net.me.cls));
        }
        code=http_ex("PUT",url,body,NULL,0, etag[0] ? "if-match" : NULL, etag[0] ? etag : NULL, NULL, 0);
        if(code==200) { seen_uid[slot][0]=0; seen_seq[slot]=0; seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
        if(net_log_ok()) LOGERR("claim slot %d: PUT failed, HTTP %d", slot, code);
    }
    return -1;
}
static void read_players(const char *resp) {
    static unsigned long lseq[NET_SLOTS]; static long long lch[NET_SLOTS];
    Actor ps[NET_SLOTS]; long long t=now_ms(); int local,count=0,slot;
    memset(ps,0,sizeof(ps));
    lock(); local=net.slot; unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        char bp[24],p[40],uid[24]; unsigned long sq; int online;
        if(slot==local) { lock(); ps[slot]=net.me; ps[slot].online=local>=0; unlock(); if(local>=0)count++; continue; }
        /* Ответ уже начинается с players, поэтому путь короче: "1/x", а не
         * "players/1/x". Это также не даёт чату попасть в боевой ответ. */
        snprintf(bp,sizeof(bp),"%d",slot); snprintf(p,sizeof(p),"%s/uid",bp); strv(resp,p,uid,sizeof(uid));
        if(!uid[0]||!strcmp(uid,net.uid)){ lseq[slot]=0; lch[slot]=0; continue; }
        snprintf(p,sizeof(p),"%s/seq",bp); sq=(unsigned long)num(resp,p,0);
        if(sq!=lseq[slot]){ lseq[slot]=sq; lch[slot]=t; } else if(!lch[slot]) lch[slot]=t;
        /* Игрок, который вышел (или молчит дольше TIMEOUT), удаляется целиком:
         * ник тоже не переносится, иначе его имя оставалось бы «призраком». */
        online=t-lch[slot]<TIMEOUT; if(!online)continue;
        snprintf(p,sizeof(p),"%s/nick",bp); strv(resp,p,ps[slot].nick,sizeof(ps[slot].nick));
        snprintf(p,sizeof(p),"%s/x",bp); ps[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); ps[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/angle",bp); ps[slot].a=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/hp",bp); ps[slot].hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/alive",bp); ps[slot].alive=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/px",bp); ps[slot].punch_x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/py",bp); ps[slot].punch_y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdx",bp); ps[slot].punch_dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/pdy",bp); ps[slot].punch_dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/punch",bp); ps[slot].punch=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/cls",bp); ps[slot].cls=num(resp,p,0);
        ps[slot].online=1; count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); net.count=count; unlock();
}
static void parse_and_store_chat(const char *json){
    if(!json || !*json) return;
    ChatMsg tmp[CHAT_MAX];
    int tmp_cnt=0;
    const char *p=json;
    while(*p && tmp_cnt<CHAT_MAX){
        const char *uid_key=strstr(p, "\"uid\"");
        if(!uid_key) break;
        const char *uid_colon=strchr(uid_key, ':');
        if(!uid_colon) break;
        const char *q1=strchr(uid_colon, '\"');
        if(!q1) break; q1++;
        const char *q2=strchr(q1, '\"');
        if(!q2) break;
        char uid[24]={0};
        size_t ul=q2-q1; if(ul>=sizeof(uid)) ul=sizeof(uid)-1;
        memcpy(uid,q1,ul); uid[ul]='\0';
        const char *next_uid=strstr(uid_key+1, "\"uid\"");
        const char *text_key=strstr(q2, "\"text\"");
        if(!text_key) break;
        if(next_uid && text_key>next_uid){
            p=q2+1; continue;
        }
        char nick[24]={0};
        {
            const char *nk=strstr(q2, "\"nick\"");
            if(nk && (!next_uid || nk<next_uid) && nk<text_key){
                const char *nc=strchr(nk, ':');
                if(nc){
                    const char *nq1=strchr(nc, '"');
                    if(nq1){
                        nq1++;
                        const char *nq2=strchr(nq1, '"');
                        if(nq2){
                            size_t nl=nq2-nq1;
                            if(nl>=sizeof(nick)) nl=sizeof(nick)-1;
                            memcpy(nick,nq1,nl); nick[nl]=0;
                        }
                    }
                }
            }
        }
        const char *t_colon=strchr(text_key, ':');
        if(!t_colon){ p=text_key+6; continue; }
        const char *tq1=strchr(t_colon, '\"');
        if(!tq1){ p=t_colon+1; continue; } tq1++;
        const char *tq2=tq1;
        while(*tq2){
            if(*tq2=='\"' && *(tq2-1)!='\\') break;
            tq2++;
        }
        if(!*tq2) break;
        size_t tl=tq2-tq1;
        if(tl>=CHAT_TEXT_MAX) tl=CHAT_TEXT_MAX-1;
        char raw[CHAT_TEXT_MAX*2];
        size_t rl=tl; if(rl>=sizeof(raw)) rl=sizeof(raw)-1;
        memcpy(raw,tq1,rl); raw[rl]='\0';
        char text[CHAT_TEXT_MAX]={0};
        size_t oi=0;
        for(size_t i=0;i<rl && oi+1<sizeof(text);i++){
            if(raw[i]=='\\' && i+1<rl){
                char esc=raw[i+1];
                if(esc=='\"'){ text[oi++]='\"'; i++; }
                else if(esc=='\\'){ text[oi++]='\\'; i++; }
                else if(esc=='n'){ text[oi++]=' '; i++; }
                else if(esc=='r'){ i++; }
                else if(esc=='/'){ text[oi++]='/'; i++; }
                else { text[oi++]=esc; i++; }
            }else{
                text[oi++]=raw[i];
            }
        }
        text[oi]='\0';
        strncpy(tmp[tmp_cnt].uid, uid, sizeof(tmp[tmp_cnt].uid)-1);
        strncpy(tmp[tmp_cnt].nick, nick, sizeof(tmp[tmp_cnt].nick)-1);
        strncpy(tmp[tmp_cnt].text, text, sizeof(tmp[tmp_cnt].text)-1);
        tmp[tmp_cnt].valid=1;
        tmp_cnt++;
        p=tq2+1;
    }
    lock();
    if(tmp_cnt>0){
        int start=0;
        if(tmp_cnt>CHAT_MAX) start=tmp_cnt-CHAT_MAX;
        net.chat_count=0;
        for(int i=start;i<tmp_cnt;i++){
            net.chats[net.chat_count++]=tmp[i];
        }
    }
    unlock();
}
static int pull_chat(char *resp, size_t cap){
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=20",net.base,net.room);
    return http("GET",url,NULL,resp,cap)==200;
}
#ifdef _WIN32
static void *thread_main(void *arg);
static void *reader_thread(void *arg);
static unsigned __stdcall win_thread_main(void *arg){ thread_main(arg); return 0; }
static unsigned __stdcall win_reader_thread(void *arg){ reader_thread(arg); return 0; }
#endif
static void *thread_main(void *arg) {
    int fails=0; (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            status(NET_CONNECTING); slot=claim_slot();
            /* Ошибка показывается только после 6 неудач подряд (раньше 3): на
             * мобильном интернете 2-3 случайных таймаута больше не рисуют
             * «Нет соединения», подключение просто продолжается. */
            if(slot<0){ if(++fails>6){status(NET_ERROR); LOGERR("network error: cannot claim player slot in room '%s'", net.room);} sleep_ms(500); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.players[slot].online=1; unlock(); fails=0;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>6){status(NET_ERROR); LOGERR("network error: failed to push player state");} sleep_ms(300); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void *reader_thread(void *arg) {
    char resp[RESP], chat_resp[CHAT_RESP], event_resp[RESP];
    long long next_chat=0, next_event=0;
    (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0){ sleep_ms(50); continue; }
        if(pull_state(resp,sizeof(resp))) read_players(resp);
        /* Сообщения не влияют на бой, поэтому нет смысла скачивать их 16 раз
         * в секунду. Так редкий запрос чата не забивает канал позиций. */
        if(start>=next_chat) {
            if(pull_chat(chat_resp,sizeof(chat_resp))) parse_and_store_chat(chat_resp);
            next_chat=now_ms()+CHAT_TICK;
        }
        /* Ивент (диско) читаем отдельно и реже: значение 1 включает мигание
         * у всех, кто сейчас в комнате. Узел крошечный, но всё равно не нужно
         * дёргать его 16 раз в секунду вместе с позициями. */
        if(start>=next_event) {
            if(pull_event(event_resp,sizeof(event_resp)))
                lock(); net.event = num(event_resp,"",0)!=0 ? 1 : 0; unlock();
            next_event=now_ms()+EVENT_TICK;
        }
        long long spent=now_ms()-start; if(spent<READ_TICK)sleep_ms((int)(READ_TICK-spent));
    }
    return NULL;
}
static void make_uid(void) {
    unsigned long a,b; int local=0;
#ifdef _WIN32
    a=(unsigned long)time(NULL)^((unsigned long)GetTickCount64()<<8);
    b=(unsigned long)_getpid()^(unsigned long)(uintptr_t)&local;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    a=(unsigned long)time(NULL)^((unsigned long)t.tv_nsec<<8);
    b=(unsigned long)getpid()^(unsigned long)(uintptr_t)&local;
#endif
    snprintf(net.uid,sizeof(net.uid),"%08lx%08lx",a&0xfffffffful,b&0xfffffffful);
}
void net_connect(const char *url,const char *room) {
    size_t n;
    if(net.run||!url||!*url)return;
    memset(&net.me,0,sizeof(net.me)); memset(net.players,0,sizeof(net.players));
    memset(net.chats,0,sizeof(net.chats));
    net.count=0; net.chat_count=0; net.slot=-1; net.seq=0;
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    {
        char snick[LOGIN_NICK_MAX+1];
        lg_lock(); if (lg.status == NET_LOGIN_OK) snprintf(snick,sizeof(snick),"%s",lg.session_nick); else snick[0]=0; lg_unlock();
        if (snick[0]) snprintf(net.me.nick,sizeof(net.me.nick),"%s",snick);
    }
    net.me.cls = (double)pending_cls;
    net.started=1;
    net.status=NET_CONNECTING; net.run=1;
#ifdef _WIN32
    net.thread=ds_thread_start(win_thread_main,NULL);
#else
    net.thread=ds_thread_start(thread_main,NULL);
#endif
    if(!net.thread){ net.run=0; net.status=NET_ERROR; LOGERR("network error: cannot start writer thread"); return; }
#ifdef _WIN32
    net.rthread=ds_thread_start(win_reader_thread,NULL);
#else
    net.rthread=ds_thread_start(reader_thread,NULL);
#endif
    if(!net.rthread){ net.run=0; ds_thread_join(net.thread); net.status=NET_ERROR; LOGERR("network error: cannot start reader thread"); return; }
    LOG("connect %s/%s (write %dms read %dms)",net.base,net.room,WRITE_TICK,READ_TICK);
}
void net_disconnect(void) {
    if(!net.run)return;
    /* Короткие таймауты HTTP: если сеть «мертва», не блокируем игровой поток
     * на секунды (иначе при выходе из онлайна игра зависала и могла «вылететь»). */
    net_fast = 1;
    net.run=0; ds_thread_join(net.thread); ds_thread_join(net.rthread);
    net_fast = 0;
    net.status=NET_OFFLINE; net.slot=-1; net.count=0; memset(net.players,0,sizeof(net.players));
}
void net_publish(double x,double y,double a,double hp,double alive) {
    if(!net.started) return;
    lock(); net.me.x=x; net.me.y=y; net.me.a=a; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; } unlock();
}
void net_publish_punch(double x,double y,double dx,double dy,double punch) {
    if(!net.started) return;
    /* punch только растёт. Получателю достаточно заметить новое число: даже
     * медленный GET не пропустит короткий флаг active=1, как было раньше. */
    lock();
    net.me.punch_x=x; net.me.punch_y=y;
    net.me.punch_dx=dx; net.me.punch_dy=dy; net.me.punch=punch;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; }
    unlock();
}
void net_chat_send(const char *text){
    if(!net.started || !text || !*text) return;
    if(!net.run) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2], enick[64];
    const char *nick;
    json_escape(text, esc, sizeof(esc));
    lock(); nick = net.me.nick[0] ? net.me.nick : net.uid; json_escape(nick, enick, sizeof(enick)); unlock();
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nick\":\"%s\",\"text\":\"%s\"}",net.uid,enick,esc);
    http_post_async(url, body);
    LOG("chat send %s",text);
}
double net_chat_count(void){ double v; lock(); v=net.chat_count; unlock(); return v; }
const char* net_chat_text(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        strncpy(s_chat_text_ret, net.chats[i].text, sizeof(s_chat_text_ret)-1);
        s_chat_text_ret[sizeof(s_chat_text_ret)-1]='\0';
    }else{
        s_chat_text_ret[0]='\0';
    }
    unlock();
    return s_chat_text_ret;
}
const char* net_chat_uid(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        const char *src = net.chats[i].nick[0] ? net.chats[i].nick : net.chats[i].uid;
        strncpy(s_chat_uid_ret, src, sizeof(s_chat_uid_ret)-1);
        s_chat_uid_ret[sizeof(s_chat_uid_ret)-1]='\0';
    }else{
        s_chat_uid_ret[0]='\0';
    }
    unlock();
    return s_chat_uid_ret;
}
static int sidx(double slot){ int i=(int)slot; return i>=0&&i<NET_SLOTS?i:-1; }
double net_status(void){ double v; lock(); v=net.status; unlock(); return v; }
double net_slot(void){ double v; lock(); v=net.slot; unlock(); return v; }
double net_event(void){ double v; lock(); v=net.event; unlock(); return v; }
double net_count(void){ double v; lock(); v=net.count; unlock(); return v; }
const char* net_player_nick(double slot){
    int i=sidx(slot);
    lock();
    if(i>=0 && net.players[i].nick[0]){
        strncpy(s_nick_ret, net.players[i].nick, sizeof(s_nick_ret)-1);
        s_nick_ret[sizeof(s_nick_ret)-1]='\0';
    }else{
        s_nick_ret[0]='\0';
    }
    unlock();
    return s_nick_ret;
}
#define READER(name, field) double name(double slot){ int i=sidx(slot); double v=0; if(i>=0){lock();v=field;unlock();} return v; }
READER(net_player_online, net.players[i].online?1:0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
READER(net_player_punch_x, net.players[i].punch_x)
READER(net_player_punch_y, net.players[i].punch_y)
READER(net_player_punch_dx, net.players[i].punch_dx)
READER(net_player_punch_dy, net.players[i].punch_dy)
READER(net_player_punch, net.players[i].punch)
READER(net_player_class, net.players[i].cls)
#undef READER
