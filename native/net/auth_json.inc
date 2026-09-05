/* Публичный API достижений (биты ACH_FLAG_* описаны в net.h). Само хранение
 * живёт в state_storage.inc: achievements.dat на устройстве и поле ach в
 * облачной записи игрока — награда больше не привязана к одному устройству. */
double net_load_achievement_flags(void) {
    double value;
    achievements_read();
    ds_mutex_lock(ach_lock); value = (double)ach_flags; ds_mutex_unlock(ach_lock);
    return value;
}

/* Заменить маску целиком (локально и в облаке). */
void net_save_achievement_flags(double flags) {
    unsigned int f = (unsigned int)flags;
    ds_mutex_lock(ach_lock);
    ach_flags = f;
    ach_loaded = 1;
    ds_mutex_unlock(ach_lock);
    achievements_write(f);
    progress_push_cloud_current();
}

double net_has_achievement_flag(double flag) {
    double result;
    achievements_read();
    ds_mutex_lock(ach_lock);
    result = (ach_flags & (unsigned int)flag) ? 1.0 : 0.0;
    ds_mutex_unlock(ach_lock);
    return result;
}

/* Выдать награду: бит добавляется к маске, маска пишется в файл и сразу уходит
 * в облако вместе с профилем (progress_push_cloud_current). */
void net_mark_achievement_flag(double flag) {
    unsigned int f;
    achievements_read();
    ds_mutex_lock(ach_lock);
    ach_flags |= (unsigned int)flag;
    f = ach_flags;
    ds_mutex_unlock(ach_lock);
    achievements_write(f);
    progress_push_cloud_current();
}

void net_set_class(double cls) {
    int k = (int)cls;
    if (k != 1 && k != 2 && k != 3) k = 0;
    pending_cls = k;
    lock(); net.me.cls = (double)k;
    if (net.slot >= 0) net.players[net.slot].cls = (double)k;
    unlock();
}
void net_set_level(double level) {
    int lv = (int)level;
    if (lv < 0) lv = 0;
    if (lv > LEVEL_MAX) lv = LEVEL_MAX;
    pending_level = lv;
    lock(); net.me.level = (double)lv;
    if (net.slot >= 0) net.players[net.slot].level = (double)lv;
    unlock();
}
void net_set_skin(double skin) {
    int sk = (int)skin;
    if (sk < 0) sk = 0;
    if (sk > SKIN_MAX) sk = SKIN_MAX;
    pending_skin = sk;
    lock(); net.me.skin = (double)sk;
    if (net.slot >= 0) net.players[net.slot].skin = (double)sk;
    unlock();
}

static int nick_valid(const char *n) {
    size_t i=0, bytes=n ? strlen(n) : 0, chars=0;
    if (bytes < 1 || bytes > LOGIN_NICK_MAX) return 0;
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
    return chars>=1 && chars<=16;
}

static int pass_valid(const char *p) {
    if (!p) return 0;
    size_t len = strlen(p);
    if (len < 1 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7F || c == '\"' || c == '\\' || c == ' ') return 0;
    }
    return 1;
}

static void session_save(const char *nick, const char *pass) {
    char path[320]; FILE *f;
    if (lg.path[0]) snprintf(path, sizeof(path), "%s/%s", lg.path, SESSION_FILE);
    else snprintf(path, sizeof(path), "%s", SESSION_FILE);
    f = fopen(path, "w");
    if (f) {
        if (pass && *pass) fprintf(f, "%s %s\n", nick, pass);
        else fprintf(f, "%s\n", nick);
        fclose(f);
    }
}
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void sleep_ms(int ms) {
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L };
    nanosleep(&t, NULL);
}
static double safe(double v) { if (v != v) return 0; if (v > 1e300 || v < -1e300) return 0; return v; }
static void lock(void) { ds_mutex_lock(net.lock); }
static void unlock(void) { ds_mutex_unlock(net.lock); }
static void status(int s) { lock(); net.status=s; unlock(); }

static DSMutex net_log_lock = DS_MUTEX_INIT;
static int net_log_ok(void) {
    static long long last = 0;
    long long now = now_ms();
    int ok = 0;
    ds_mutex_lock(net_log_lock);
    if (now - last >= 2000) { last = now; ok = 1; }
    ds_mutex_unlock(net_log_lock);
    return ok;
}
void net_set_java_vm(JavaVM *vm) { net.vm=vm; }
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
    {
        /* HttpURLConnection бросает ProtocolException на PATCH, а сохранение
         * прогресса и банов идёт именно им. Firebase умеет принимать такой
         * запрос как POST с заголовком X-HTTP-Method-Override. */
        const char *send_method = method;
        const char *override = NULL;
        if (!strcmp(method, "PATCH")) { send_method = "POST"; override = "PATCH"; }
        jm=(*env)->NewStringUTF(env,send_method); JNI_CHECK();
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setRequestMethod","(Ljava/lang/String;)V"),jm); JNI_CHECK();
        if (override) {
            jmethodID set_override=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
            jstring ok=(*env)->NewStringUTF(env,"X-HTTP-Method-Override"), ov=(*env)->NewStringUTF(env,override);
            (*env)->CallVoidMethod(env,conn,set_override,ok,ov); JNI_CHECK();
        }
    }
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

/* ────────────────────────────────────────────────────────────────────────────
 * Firebase Auth: базу могут читать и писать только запросы с ?auth=<idToken>.
 *
 * Раньше правила базы были открыты («users: .read true»), а URL лежал в
 * открытом репозитории — любой мог выкачать чужие пароли (они хранились в
 * открытом виде), затереть чат, слоты или баны и просто завалить базу
 * запросами. Теперь клиент логинится в Firebase Auth по e-mail/паролю
 * <ник>@cb4.game (пароль = игровой пароль + суффикс), получает idToken и
 * подписывает им каждый запрос. API-ключ ограничен нашим Android-пакетом
 * com.cb4 в Google Cloud Console, поэтому токен можно получить только из
 * нашего приложения, а правила firebase.rules.json пускают одних лишь
 * authenticated-запросов. Пароли из базы исчезают совсем.
 *
 * Если FB-ключ не задан (config.ds: FB_KEY=""), клиент работает по-старому —
 * это переходный режим для уже собранных версий.
 * ──────────────────────────────────────────────────────────────────────────── */
#define FB_EMAIL_DOMAIN "@cb4.game"
/* Пароль Firebase = игровой + суффикс: у Firebase минимум 6 символов, а игра
 * разрешает короткие пароли; суффикс добивает длину и не даёт использовать
 * базу паролей Firebase как самостоятельный вход. */
#define FB_PASS_SUFFIX "|cb4v1"
typedef struct {
    DSMutex lock;
    char key[128];
    char id_token[FB_ID_MAX];
    char refresh_token[FB_REFRESH_MAX];
    long long expires_at;   /* мс; обновляем за минуту до истечения */
} FirebaseAuth;
static FirebaseAuth fb = { .lock = DS_MUTEX_INIT };

void net_set_firebase_key(const char *key) {
    size_t n;
    if (!key) key = "";
    while (*key == ' ' || *key == '\t') key++;
    ds_mutex_lock(fb.lock);
    snprintf(fb.key, sizeof(fb.key), "%s", key);
    n = strlen(fb.key);
    while (n && (fb.key[n-1]==' ' || fb.key[n-1]=='\t' || fb.key[n-1]=='\r' || fb.key[n-1]=='\n'))
        fb.key[--n] = 0;
    ds_mutex_unlock(fb.lock);
    if (fb.key[0]) LOG("firebase auth enabled (key set)");
}
static int fb_enabled(void) { return fb.key[0] != 0; }

static void fb_store_tokens(const char *id_token, const char *refresh_token, double expires_in_sec) {
    ds_mutex_lock(fb.lock);
    snprintf(fb.id_token, sizeof(fb.id_token), "%s", id_token ? id_token : "");
    snprintf(fb.refresh_token, sizeof(fb.refresh_token), "%s", refresh_token ? refresh_token : "");
    if (expires_in_sec < 60) expires_in_sec = 60;
    fb.expires_at = now_ms() + (long long)(expires_in_sec * 1000.0) - 60000;
    ds_mutex_unlock(fb.lock);
}

/* URL auth-эндпоинтов. Боевой путь — googleapis.com; если же базой указан
 * локальный http:// тестовый сервер, auth-эндпоинты тоже ищутся на нём —
 * так тестовый прогон проходит ровно тот же код, что и бой. */
static void fb_auth_endpoint(char *dst, size_t cap, const char *path) {
    if (net.base[0] && !strncmp(net.base, "http://", 7)) {
        snprintf(dst, cap, "%s%s?key=%s", net.base, path, fb.key);
        return;
    }
    if (!strcmp(path, "/v1/token"))
        snprintf(dst, cap, "https://securetoken.googleapis.com/v1/token?key=%s", fb.key);
    else
        /* path уже содержит /v1/... (например /v1/accounts:signInWithPassword),
         * поэтому второй раз «v1» подставлять нельзя — иначе получится
         * /v1/v1/... и Firebase Auth ответит ошибкой, а клиент покажет
         * «No connection». */
        snprintf(dst, cap, "https://identitytoolkit.googleapis.com%s?key=%s", path, fb.key);
}
