#include "net.h"
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <wininet.h>
#else
#include <pthread.h>
#include <sys/stat.h>
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
#define WRITE_TICK 50
#define READ_TICK 50
#define TIMEOUT 5000
#define STALE 12000
#define CHAT_MAX 32
#define CHAT_TEXT_MAX 96
#define NICK_MAX_CHARS 16
#define NICK_MAX_BYTES 48
#define PASSWORD_MAX_BYTES 192
#define ACCOUNT_KEY_HEX 64
#define ACCOUNT_SALT_HEX 32
#define ACCOUNT_VERIFIER_HEX 64
#define PBKDF2_ROUNDS 60000
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
typedef struct { double x,y,a,hp,alive; int online; char nickname[NICK_MAX_BYTES+1]; } Actor;
typedef struct { double x,y,dx,dy,active,shot,tr; } Bullet;
typedef struct { char uid[24]; char nickname[NICK_MAX_BYTES+1]; char text[CHAT_TEXT_MAX]; unsigned long ts; int valid; } ChatMsg;
typedef struct {
    int status;
    char nickname[NICK_MAX_BYTES+1];
    char key[ACCOUNT_KEY_HEX+1];
    char salt[ACCOUNT_SALT_HEX+1];
    char verifier[ACCOUNT_VERIFIER_HEX+1];
    char error[128];
} Account;
static struct {
    DSThread thread, rthread; DSMutex lock;
#ifdef __ANDROID__
    JavaVM *vm;
#endif
    int run, started, slot, status, writer_created, reader_created;
    char base[256], room[48], uid[24], storage[384];
    Actor me; Bullet my_bullet; unsigned long seq, count;
    Actor players[NET_SLOTS]; Bullet bullets[NET_SLOTS];
    ChatMsg chats[CHAT_MAX]; int chat_count;
    Account account; int account_loaded, account_worker;
} net = { .lock = DS_MUTEX_INIT, .slot = -1 };
static char s_chat_text_ret[CHAT_TEXT_MAX];
static char s_chat_uid_ret[24];
static char s_chat_nick_ret[NICK_MAX_BYTES+1];
static char s_player_nick_ret[NICK_MAX_BYTES+1];
static char s_account_nick_ret[NICK_MAX_BYTES+1];
static char s_account_error_ret[128];
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
static int http_ex(const char *method,const char *url,const char *body,char *out,size_t cap,
                   const char *header,const char *value,char *etag,size_t etag_cap) {
    char host[256]; int port = 80, secure = 0; const char *path = NULL;
    if (out && cap) out[0] = '\0';
    if (etag && etag_cap) etag[0] = '\0';
    if (!url || !parse_http_url(url, host, sizeof(host), &port, &path, &secure)) return 0;
    HINTERNET inet = InternetOpenA("CubicBattle/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return 0;
    HINTERNET conn = InternetConnectA(inet, host, (INTERNET_PORT)port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return 0; }
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
    if (secure) flags |= INTERNET_FLAG_SECURE;
    HINTERNET req = HttpOpenRequestA(conn, method, path, NULL, NULL, NULL, flags, 0);
    if (!req) { InternetCloseHandle(conn); InternetCloseHandle(inet); return 0; }
    char hdrs[512]; int hl = 0;
    hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "Content-Type: application/json\r\n");
    if (header && value) hl += snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "%s: %s\r\n", header, value);
    if (hl < 0 || (size_t)hl >= sizeof(hdrs)) hl = (int)sizeof(hdrs) - 1;
    hdrs[hl] = '\0';
    BOOL ok = HttpSendRequestA(req, hl ? hdrs : NULL, (DWORD)hl, (LPVOID)body, (DWORD)(body ? strlen(body) : 0));
    int code = 0;
    if (ok) {
        DWORD len = sizeof(code), idx = 0;
        HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, &idx);
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
    return code;
}
#elif defined(__ANDROID__)
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
    if ((*env)->PushLocalFrame(env,32)!=0) goto done;
    urlc=(*env)->FindClass(env,"java/net/URL");
    connc=(*env)->FindClass(env,"java/net/HttpURLConnection");
    if (!urlc||!connc) goto done;
    ju=(*env)->NewStringUTF(env,url);
    urlobj=(*env)->NewObject(env,urlc,(*env)->GetMethodID(env,urlc,"<init>","(Ljava/lang/String;)V"),ju);
    if (!urlobj||(*env)->ExceptionCheck(env)) goto done;
    conn=(*env)->CallObjectMethod(env,urlobj,(*env)->GetMethodID(env,urlc,"openConnection","()Ljava/net/URLConnection;"));
    if (!conn||(*env)->ExceptionCheck(env)) goto done;
    jm=(*env)->NewStringUTF(env,method);
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setRequestMethod","(Ljava/lang/String;)V"),jm);
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setConnectTimeout","(I)V"),5000);
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setReadTimeout","(I)V"),5000);
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setUseCaches","(Z)V"),JNI_FALSE);
    {
        jmethodID set_header=(*env)->GetMethodID(env,connc,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
        jstring k=(*env)->NewStringUTF(env,"Content-Type"), v=(*env)->NewStringUTF(env,"application/json");
        (*env)->CallVoidMethod(env,conn,set_header,k,v);
        if(header&&value) { k=(*env)->NewStringUTF(env,header); v=(*env)->NewStringUTF(env,value); (*env)->CallVoidMethod(env,conn,set_header,k,v); }
    }
    if (body&&*body) {
        jobject os; jbyteArray data; jsize len=(jsize)strlen(body);
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setDoOutput","(Z)V"),JNI_TRUE);
        (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setFixedLengthStreamingMode","(I)V"),len);
        os=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getOutputStream","()Ljava/io/OutputStream;"));
        if (!os||(*env)->ExceptionCheck(env)) goto done;
        data=(*env)->NewByteArray(env,len);
        (*env)->SetByteArrayRegion(env,data,0,len,(const jbyte*)body);
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"write","([B)V"),data);
        (*env)->CallVoidMethod(env,os,(*env)->GetMethodID(env,(*env)->GetObjectClass(env,os),"close","()V"));
    }
    code=(int)(*env)->CallIntMethod(env,conn,(*env)->GetMethodID(env,connc,"getResponseCode","()I"));
    if ((*env)->ExceptionCheck(env)) { code=0; goto done; }
    if(etag&&etag_cap) {
        jstring key=(*env)->NewStringUTF(env,"ETag");
        jstring val=(jstring)(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;"),key);
        if(val&&!(*env)->ExceptionCheck(env)) { const char *s=(*env)->GetStringUTFChars(env,val,NULL); if(s){snprintf(etag,etag_cap,"%s",s);(*env)->ReleaseStringUTFChars(env,val,s);} }
    }
    stream=(*env)->CallObjectMethod(env,conn,(*env)->GetMethodID(env,connc,code>=400?"getErrorStream":"getInputStream","()Ljava/io/InputStream;"));
    if (!stream||(*env)->ExceptionCheck(env)) goto closeconn;
    streamc=(*env)->GetObjectClass(env,stream);
    buf=(*env)->NewByteArray(env,2048);
    for (;;) {
        jint n=(*env)->CallIntMethod(env,stream,(*env)->GetMethodID(env,streamc,"read","([B)I"),buf);
        if ((*env)->ExceptionCheck(env)||n<=0) break;
        if (out&&cap&&total+(size_t)n<cap) { (*env)->GetByteArrayRegion(env,buf,0,n,(jbyte*)(out+total)); total+=(size_t)n; out[total]='\0'; }
    }
    (*env)->CallVoidMethod(env,stream,(*env)->GetMethodID(env,streamc,"close","()V"));
closeconn:
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"disconnect","()V"));
    ok=1;
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env,NULL);
    if (attached) (*net.vm)->DetachCurrentThread(net.vm);
    return ok ? code : 0;
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
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char block[64]; size_t used; } Sha256;
static const uint32_t sha_k[64]={
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
static uint32_t rr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void sha_block(Sha256 *s,const unsigned char *p){
    uint32_t w[64],a,b,c,d,e,f,g,h;
    for(int i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for(int i=16;i<64;i++){uint32_t x=w[i-15],y=w[i-2];w[i]=(rr(x,7)^rr(x,18)^(x>>3))+w[i-16]+(rr(y,17)^rr(y,19)^(y>>10))+w[i-7];}
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for(int i=0;i<64;i++){uint32_t t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^((~e)&g))+sha_k[i]+w[i];uint32_t t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(Sha256 *s){static const uint32_t iv[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};memcpy(s->h,iv,sizeof(iv));s->bits=0;s->used=0;}
static void sha_update(Sha256 *s,const void *data,size_t n){const unsigned char *p=data;s->bits+=(uint64_t)n*8;while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){sha_block(s,s->block);s->used=0;}}}
static void sha_final(Sha256 *s,unsigned char out[32]){uint64_t bits=s->bits;s->block[s->used++]=0x80;if(s->used>56){while(s->used<64)s->block[s->used++]=0;sha_block(s,s->block);s->used=0;}while(s->used<56)s->block[s->used++]=0;for(int i=7;i>=0;i--)s->block[s->used++]=(unsigned char)(bits>>(i*8));sha_block(s,s->block);for(int i=0;i<8;i++){out[i*4]=(unsigned char)(s->h[i]>>24);out[i*4+1]=(unsigned char)(s->h[i]>>16);out[i*4+2]=(unsigned char)(s->h[i]>>8);out[i*4+3]=(unsigned char)s->h[i];}}
static void sha256(const void *data,size_t n,unsigned char out[32]){Sha256 s;sha_init(&s);sha_update(&s,data,n);sha_final(&s,out);}
static void hmac_sha256(const unsigned char *key,size_t kn,const unsigned char *data,size_t dn,unsigned char out[32]){
    unsigned char k[64]={0},ipad[64],opad[64],inner[32];Sha256 s;if(kn>64){sha256(key,kn,k);kn=32;}else memcpy(k,key,kn);for(int i=0;i<64;i++){ipad[i]=k[i]^0x36;opad[i]=k[i]^0x5c;}sha_init(&s);sha_update(&s,ipad,64);sha_update(&s,data,dn);sha_final(&s,inner);sha_init(&s);sha_update(&s,opad,64);sha_update(&s,inner,32);sha_final(&s,out);
}
static void pbkdf2(const char *password,const unsigned char salt[16],unsigned char out[32]){
    unsigned char msg[20],u[32],t[32];memcpy(msg,salt,16);msg[16]=0;msg[17]=0;msg[18]=0;msg[19]=1;hmac_sha256((const unsigned char*)password,strlen(password),msg,sizeof(msg),u);memcpy(t,u,32);for(int r=1;r<PBKDF2_ROUNDS;r++){hmac_sha256((const unsigned char*)password,strlen(password),u,32,u);for(int i=0;i<32;i++)t[i]^=u[i];}memcpy(out,t,32);
}
static void to_hex(const unsigned char *in,size_t n,char *out){static const char h[]="0123456789abcdef";for(size_t i=0;i<n;i++){out[i*2]=h[in[i]>>4];out[i*2+1]=h[in[i]&15];}out[n*2]=0;}
static int from_hex(const char *in,unsigned char *out,size_t n){if(!in||strlen(in)!=n*2)return 0;for(size_t i=0;i<n;i++){int a=in[i*2],b=in[i*2+1];a=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:a>='A'&&a<='F'?a-'A'+10:-1;b=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:b>='A'&&b<='F'?b-'A'+10:-1;if(a<0||b<0)return 0;out[i]=(unsigned char)((a<<4)|b);}return 1;}
static int secure_eq(const char *a,const char *b,size_t n){unsigned char d=0;if(!a||!b||strlen(a)!=n||strlen(b)!=n)return 0;for(size_t i=0;i<n;i++)d|=(unsigned char)(a[i]^b[i]);return d==0;}
static void wipe(void *p,size_t n){volatile unsigned char *v=p;while(n--)*v++=0;}
static int utf8_next(const unsigned char **at,const unsigned char *end,uint32_t *cp){
    const unsigned char *p=*at;uint32_t c;if(p>=end)return 0;if(*p<0x80){*cp=*p;*at=p+1;return 1;}if(*p>=0xc2&&*p<=0xdf&&p+1<end&&(p[1]&0xc0)==0x80){c=((uint32_t)(p[0]&31)<<6)|(p[1]&63);*at=p+2;*cp=c;return 1;}if(*p>=0xe0&&*p<=0xef&&p+2<end&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80){c=((uint32_t)(p[0]&15)<<12)|((uint32_t)(p[1]&63)<<6)|(p[2]&63);if(c<0x800||(c>=0xd800&&c<=0xdfff))return 0;*at=p+3;*cp=c;return 1;}if(*p>=0xf0&&*p<=0xf4&&p+3<end&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80&&(p[3]&0xc0)==0x80){c=((uint32_t)(p[0]&7)<<18)|((uint32_t)(p[1]&63)<<12)|((uint32_t)(p[2]&63)<<6)|(p[3]&63);if(c<0x10000||c>0x10ffff)return 0;*at=p+4;*cp=c;return 1;}return 0;
}
static size_t utf8_put(uint32_t cp,char *out,size_t cap){if(cp<0x80){if(cap<1)return 0;out[0]=(char)cp;return 1;}if(cp<0x800){if(cap<2)return 0;out[0]=(char)(0xc0|(cp>>6));out[1]=(char)(0x80|(cp&63));return 2;}if(cap<3)return 0;out[0]=(char)(0xe0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&63));out[2]=(char)(0x80|(cp&63));return 3;}
static int normalize_nickname(const char *input,char shown[NICK_MAX_BYTES+1],char normalized[NICK_MAX_BYTES+1],char key[ACCOUNT_KEY_HEX+1]){
    const unsigned char *start=(const unsigned char*)(input?input:""),*end=start+strlen((const char*)start),*p;size_t oi=0,ni=0;int count=0,word=0;uint32_t cp;unsigned char digest[32];
    while(start<end&&(*start==' '||*start=='\t'||*start=='\n'||*start=='\r'))start++;
    while(end>start&&(end[-1]==' '||end[-1]=='\t'||end[-1]=='\n'||end[-1]=='\r'))end--;
    p=start;
    while(p<end){const unsigned char *before=p;if(!utf8_next(&p,end,&cp))return 0;if(++count>NICK_MAX_CHARS)return 0;if((cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z')||(cp>='0'&&cp<='9'))word=1;else if(cp=='_'||cp=='-'){}else if(cp==0x401||cp==0x451||(cp>=0x410&&cp<=0x44f))word=1;else return 0;size_t raw=(size_t)(p-before);if(oi+raw>NICK_MAX_BYTES)return 0;memcpy(shown+oi,before,raw);oi+=raw;if(cp>='A'&&cp<='Z')cp+=32;else if(cp>=0x410&&cp<=0x42f)cp+=32;else if(cp==0x401)cp=0x451;size_t wrote=utf8_put(cp,normalized+ni,NICK_MAX_BYTES-ni);if(!wrote)return 0;ni+=wrote;}
    if(count<3||!word)return 0;
    shown[oi]=0;normalized[ni]=0;sha256(normalized,ni,digest);to_hex(digest,32,key);return 1;
}
static int valid_password(const char *password){const unsigned char *p=(const unsigned char*)(password?password:""),*end=p+strlen((const char*)p);uint32_t cp;int count=0;if((size_t)(end-p)>PASSWORD_MAX_BYTES)return 0;while(p<end){if(!utf8_next(&p,end,&cp))return 0;if(++count>64)return 0;}return count>=6;}
static int random_bytes(unsigned char *out,size_t n){FILE *f=fopen("/dev/urandom","rb");if(f){size_t got=fread(out,1,n,f);fclose(f);if(got==n)return 1;}uint64_t x=(uint64_t)now_ms()^((uint64_t)(
#ifdef _WIN32
_getpid()
#else
getpid()
#endif
)<<32)^(uintptr_t)out;for(size_t i=0;i<n;i++){x^=x<<13;x^=x>>7;x^=x<<17;out[i]=(unsigned char)x;}return 1;}
static void account_file(char *out,size_t cap,const char *suffix){char root[384];lock();snprintf(root,sizeof(root),"%s",net.storage);unlock();if(root[0])snprintf(out,cap,"%s/%s",root,suffix);else out[0]=0;}
static void account_set(int st,const char *error){lock();net.account.status=st;snprintf(net.account.error,sizeof(net.account.error),"%s",error?error:"");unlock();}
static int account_save(const Account *a){char path[512],tmp[520];account_file(path,sizeof(path),"account.dat");if(!path[0])return 0;snprintf(tmp,sizeof(tmp),"%s.tmp",path);FILE *f=fopen(tmp,"wb");if(!f)return 0;int ok=fprintf(f,"1\n%s\n%s\n%s\n%s\n",a->nickname,a->key,a->salt,a->verifier)>0;if(fclose(f)!=0)ok=0;if(!ok){remove(tmp);return 0;}
#ifndef _WIN32
chmod(tmp,S_IRUSR|S_IWUSR);
#endif
if(rename(tmp,path)!=0){remove(tmp);return 0;}return 1;}
static void chomp(char *s){size_t n=strlen(s);while(n&&(s[n-1]=='\n'||s[n-1]=='\r'))s[--n]=0;}
static void account_load(void){
    char path[512],version[8],nickname[NICK_MAX_BYTES+2],key[ACCOUNT_KEY_HEX+2],salt[ACCOUNT_SALT_HEX+2],verifier[ACCOUNT_VERIFIER_HEX+2],shown[NICK_MAX_BYTES+1],norm[NICK_MAX_BYTES+1],checkkey[ACCOUNT_KEY_HEX+1];Account a;memset(&a,0,sizeof(a));account_file(path,sizeof(path),"account.dat");FILE *f=fopen(path,"rb");if(!f){lock();net.account_loaded=1;unlock();return;}int ok=fgets(version,sizeof(version),f)&&fgets(nickname,sizeof(nickname),f)&&fgets(key,sizeof(key),f)&&fgets(salt,sizeof(salt),f)&&fgets(verifier,sizeof(verifier),f);fclose(f);if(!ok)return;chomp(version);chomp(nickname);chomp(key);chomp(salt);chomp(verifier);unsigned char bin[32];if(strcmp(version,"1")||!normalize_nickname(nickname,shown,norm,checkkey)||strcmp(key,checkkey)||!from_hex(salt,bin,16)||!from_hex(verifier,bin,32))return;snprintf(a.nickname,sizeof(a.nickname),"%s",shown);snprintf(a.key,sizeof(a.key),"%s",key);snprintf(a.salt,sizeof(a.salt),"%s",salt);snprintf(a.verifier,sizeof(a.verifier),"%s",verifier);a.status=NET_ACCOUNT_READY;lock();net.account=a;net.account_loaded=1;unlock();
}
typedef struct { char nickname[NICK_MAX_BYTES+1],key[ACCOUNT_KEY_HEX+1],password[PASSWORD_MAX_BYTES+1],base[256]; } AccountRequest;
static int account_fetch(const char *base,const char *key,char *resp,size_t cap,char *etag,size_t etag_cap){char url[URL];snprintf(url,sizeof(url),"%s/accounts/%s.json",base,key);return http_ex("GET",url,NULL,resp,cap,"X-Firebase-ETag","true",etag,etag_cap);}
static int account_match(const char *resp,const char *password,Account *result,const char *fallback_nick,const char *key){
    char salt[ACCOUNT_SALT_HEX+1],stored[ACCOUNT_VERIFIER_HEX+1],nickname[NICK_MAX_BYTES+1];unsigned char salt_bin[16],derived[32];char verifier[ACCOUNT_VERIFIER_HEX+1];strv(resp,"salt",salt,sizeof(salt));strv(resp,"verifier",stored,sizeof(stored));strv(resp,"nickname",nickname,sizeof(nickname));if(!from_hex(salt,salt_bin,16)||strlen(stored)!=ACCOUNT_VERIFIER_HEX)return -1;pbkdf2(password,salt_bin,derived);to_hex(derived,32,verifier);wipe(derived,sizeof(derived));if(!secure_eq(stored,verifier,ACCOUNT_VERIFIER_HEX))return 0;memset(result,0,sizeof(*result));snprintf(result->nickname,sizeof(result->nickname),"%s",nickname[0]?nickname:fallback_nick);snprintf(result->key,sizeof(result->key),"%s",key);snprintf(result->salt,sizeof(result->salt),"%s",salt);snprintf(result->verifier,sizeof(result->verifier),"%s",stored);result->status=NET_ACCOUNT_READY;return 1;
}
static void account_success(const Account *a){if(!account_save(a))LOGERR("account: could not save private local profile");lock();net.account=*a;net.account_worker=0;unlock();LOG("account '%s' confirmed",a->nickname);}
static void *account_thread(void *arg){
    AccountRequest *r=arg;char resp[2048],etag[96],url[URL],body[BODY],esc[NICK_MAX_BYTES*2+1];Account a;int code=account_fetch(r->base,r->key,resp,sizeof(resp),etag,sizeof(etag));
    if(code!=200){account_set(NET_ACCOUNT_ERROR,"Firebase account registry is unavailable");goto done;}
    if(!strncmp(skip_ws(resp),"null",4)){
        unsigned char salt[16],derived[32];char salthex[ACCOUNT_SALT_HEX+1],verifier[ACCOUNT_VERIFIER_HEX+1];random_bytes(salt,sizeof(salt));pbkdf2(r->password,salt,derived);to_hex(salt,sizeof(salt),salthex);to_hex(derived,sizeof(derived),verifier);wipe(derived,sizeof(derived));json_escape(r->nickname,esc,sizeof(esc));snprintf(url,sizeof(url),"%s/accounts/%s.json",r->base,r->key);snprintf(body,sizeof(body),"{\"nickname\":\"%s\",\"salt\":\"%s\",\"verifier\":\"%s\",\"created\":%lu}",esc,salthex,verifier,(unsigned long)time(NULL));
        if(!etag[0]){account_set(NET_ACCOUNT_ERROR,"Firebase did not return an account ETag");goto done;}
        code=http_ex("PUT",url,body,NULL,0,"if-match",etag,NULL,0);
        if(code==200){memset(&a,0,sizeof(a));snprintf(a.nickname,sizeof(a.nickname),"%s",r->nickname);snprintf(a.key,sizeof(a.key),"%s",r->key);snprintf(a.salt,sizeof(a.salt),"%s",salthex);snprintf(a.verifier,sizeof(a.verifier),"%s",verifier);a.status=NET_ACCOUNT_READY;account_success(&a);goto wipe_done;}
        if(code==412){code=account_fetch(r->base,r->key,resp,sizeof(resp),etag,sizeof(etag));if(code==200&&account_match(resp,r->password,&a,r->nickname,r->key)==1){account_success(&a);goto wipe_done;}account_set(NET_ACCOUNT_WRONG_PASSWORD,"This nickname already has a different password");goto wipe_done;}
        account_set(NET_ACCOUNT_ERROR,"Firebase refused account creation");
wipe_done: wipe(salt,sizeof(salt));
    }else{
        int match=account_match(resp,r->password,&a,r->nickname,r->key);if(match==1)account_success(&a);else if(match==0)account_set(NET_ACCOUNT_WRONG_PASSWORD,"Wrong password for this nickname");else account_set(NET_ACCOUNT_ERROR,"Invalid account record in Firebase");
    }
done:
    wipe(r->password,sizeof(r->password));free(r);lock();net.account_worker=0;unlock();return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_account_thread(void *arg){ account_thread(arg); return 0; }
#endif
void net_set_storage_path(const char *path){lock();snprintf(net.storage,sizeof(net.storage),"%s",path?path:"");unlock();}
void net_account_configure(const char *url,const char *room){size_t n;lock();snprintf(net.base,sizeof(net.base),"%s",url?url:"");n=strlen(net.base);while(n&&net.base[n-1]=='/')net.base[--n]=0;snprintf(net.room,sizeof(net.room),"%s",room&&*room?room:"main");int load=!net.account_loaded;net.started=1;unlock();if(load)account_load();}
void net_account_login(const char *nickname,const char *password){
    char shown[NICK_MAX_BYTES+1],normalized[NICK_MAX_BYTES+1],key[ACCOUNT_KEY_HEX+1],base[256];if(!normalize_nickname(nickname,shown,normalized,key)){account_set(NET_ACCOUNT_INVALID,"Nickname must be 3-16 letters, digits, _ or -");return;}if(!valid_password(password)){account_set(NET_ACCOUNT_INVALID,"Password must contain 6-64 characters");return;}lock();if(net.account_worker){unlock();return;}snprintf(base,sizeof(base),"%s",net.base);net.account.status=NET_ACCOUNT_CHECKING;net.account.error[0]=0;net.account_worker=1;unlock();if(!base[0]){account_set(NET_ACCOUNT_ERROR,"Firebase is not configured");lock();net.account_worker=0;unlock();return;}AccountRequest *r=calloc(1,sizeof(*r));if(!r){account_set(NET_ACCOUNT_ERROR,"Out of memory");lock();net.account_worker=0;unlock();return;}snprintf(r->nickname,sizeof(r->nickname),"%s",shown);snprintf(r->key,sizeof(r->key),"%s",key);snprintf(r->password,sizeof(r->password),"%s",password);snprintf(r->base,sizeof(r->base),"%s",base);
#ifdef _WIN32
    DSThread worker=ds_thread_start(win_account_thread,r);
#else
    DSThread worker=ds_thread_start(account_thread,r);
#endif
if(!worker){wipe(r->password,sizeof(r->password));free(r);account_set(NET_ACCOUNT_ERROR,"Cannot start account check");lock();net.account_worker=0;unlock();return;}ds_thread_detach(worker);
}
double net_account_status(void){double v;lock();v=net.account.status;unlock();return v;}
const char *net_account_nickname(void){lock();snprintf(s_account_nick_ret,sizeof(s_account_nick_ret),"%s",net.account.nickname);unlock();return s_account_nick_ret;}
const char *net_account_error(void){lock();snprintf(s_account_error_ret,sizeof(s_account_error_ret),"%s",net.account.error);unlock();return s_account_error_ret;}
static int verify_saved_account(void){
    Account a;char base[256],resp[2048],etag[8],salt[ACCOUNT_SALT_HEX+1],verifier[ACCOUNT_VERIFIER_HEX+1];lock();a=net.account;snprintf(base,sizeof(base),"%s",net.base);unlock();if(a.status!=NET_ACCOUNT_READY||!a.key[0])return -1;int code=account_fetch(base,a.key,resp,sizeof(resp),etag,sizeof(etag));if(code!=200)return 0;strv(resp,"salt",salt,sizeof(salt));strv(resp,"verifier",verifier,sizeof(verifier));return secure_eq(a.salt,salt,ACCOUNT_SALT_HEX)&&secure_eq(a.verifier,verifier,ACCOUNT_VERIFIER_HEX)?1:-1;
}
static int push_state(void) {
    Actor a; Bullet b; int slot; unsigned long seq; char url[URL], body[BODY], nick[NICK_MAX_BYTES*2+1];
    lock(); a=net.me; b=net.my_bullet; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    json_escape(a.nickname,nick,sizeof(nick));
    snprintf(url,sizeof(url),"%s/rooms/%s.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"players/%d\":{\"uid\":\"%s\",\"nickname\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"angle\":%.4f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu},\"bullets/%d\":{\"x\":%.2f,\"y\":%.2f,\"dx\":%.4f,\"dy\":%.4f,\"active\":%.0f,\"shot\":%.0f,\"tr\":%.1f}}",
        slot,net.uid,nick,safe(a.x),safe(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,
        slot,safe(b.x),safe(b.y),safe(b.dx),safe(b.dy),safe(b.active),safe(b.shot),safe(b.tr));
    return http("PATCH",url,body,NULL,0)==200;
}
static int push_bullet_only(void){
    Bullet b; int slot; char url[URL], body[BODY];
    lock(); b=net.my_bullet; slot=net.slot; unlock();
    if(slot<0) return 0;
    snprintf(url,sizeof(url),"%s/rooms/%s/bullets/%d.json",net.base,net.room,slot);
    snprintf(body,sizeof(body),"{\"x\":%.2f,\"y\":%.2f,\"dx\":%.4f,\"dy\":%.4f,\"active\":%.0f,\"shot\":%.0f,\"tr\":%.1f}",
        safe(b.x),safe(b.y),safe(b.dx),safe(b.dy),safe(b.active),safe(b.shot),safe(b.tr));
    int code=http("PUT",url,body,NULL,0);
    return code==200;
}
static int pull_state(char *resp,size_t cap) { char url[URL]; snprintf(url,sizeof(url),"%s/rooms/%s.json",net.base,net.room); return http("GET",url,NULL,resp,cap)==200; }
static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
    snprintf(url,sizeof(url),"%s/rooms/%s/bullets/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    static unsigned long seen_seq[NET_SLOTS]; static long long seen_at[NET_SLOTS]; static char seen_uid[NET_SLOTS][24];
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96],nickname[NICK_MAX_BYTES*2+1]; long long t=now_ms(); int slot;
    lock(); json_escape(net.account.nickname,nickname,sizeof(nickname)); unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        unsigned long seq; int claim=0,code;
        snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot);
        code=http_ex("GET",url,NULL,resp,sizeof(resp),"X-Firebase-ETag","true",etag,sizeof(etag));
        if(code!=200||!etag[0])continue;
        strv(resp,"uid",uid,sizeof(uid));
        if(uid[0]&&!strcmp(uid,net.uid))return slot;
        seq=(unsigned long)num(resp,"seq",0);
        if(!uid[0])claim=1;
        else if(strcmp(uid,seen_uid[slot])||seq!=seen_seq[slot]) { snprintf(seen_uid[slot],sizeof(seen_uid[slot]),"%s",uid); seen_seq[slot]=seq; seen_at[slot]=t; }
        else if(t-seen_at[slot]>=STALE)claim=1;
        if(!claim)continue;
        snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nickname\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0}",net.uid,nickname);
        code=http_ex("PUT",url,body,NULL,0,"if-match",etag,NULL,0);
        if(code==200) { seen_uid[slot][0]=0; seen_seq[slot]=0; seen_at[slot]=0; LOG("slot %d uid %s",slot,net.uid); return slot; }
    }
    return -1;
}
static void read_players(const char *resp) {
    static unsigned long lseq[NET_SLOTS]; static long long lch[NET_SLOTS];
    Actor ps[NET_SLOTS]; Bullet bs[NET_SLOTS]; long long t=now_ms(); int local,count=0,slot;
    memset(ps,0,sizeof(ps)); memset(bs,0,sizeof(bs));
    lock(); local=net.slot; unlock();
    for(slot=0;slot<NET_SLOTS;slot++) {
        char bp[24],p[40],uid[24]; unsigned long sq; int online;
        if(slot==local) { lock(); ps[slot]=net.me; bs[slot]=net.my_bullet; ps[slot].online=local>=0; unlock(); if(local>=0)count++; continue; }
        snprintf(bp,sizeof(bp),"players/%d",slot); snprintf(p,sizeof(p),"%s/uid",bp); strv(resp,p,uid,sizeof(uid));
        if(!uid[0]||!strcmp(uid,net.uid)){ lseq[slot]=0; lch[slot]=0; continue; }
        snprintf(p,sizeof(p),"%s/seq",bp); sq=(unsigned long)num(resp,p,0);
        if(sq!=lseq[slot]){ lseq[slot]=sq; lch[slot]=t; } else if(!lch[slot]) lch[slot]=t;
        online=t-lch[slot]<TIMEOUT; if(!online)continue;
        snprintf(p,sizeof(p),"%s/x",bp); ps[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); ps[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/angle",bp); ps[slot].a=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/hp",bp); ps[slot].hp=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/alive",bp); ps[slot].alive=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/nickname",bp); strv(resp,p,ps[slot].nickname,sizeof(ps[slot].nickname));
        ps[slot].online=1;
        snprintf(bp,sizeof(bp),"bullets/%d",slot);
        snprintf(p,sizeof(p),"%s/x",bp); bs[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); bs[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dx",bp); bs[slot].dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dy",bp); bs[slot].dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/active",bp); bs[slot].active=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/shot",bp); bs[slot].shot=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/tr",bp); bs[slot].tr=num(resp,p,0);
        count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); memcpy(net.bullets,bs,sizeof(bs)); net.count=count; unlock();
}
static void parse_and_store_chat(const char *json){
    ChatMsg tmp[CHAT_MAX];int tmp_cnt=0;const char *p=skip_ws(json);
    memset(tmp,0,sizeof(tmp));
    if(!p||*p!='{')return;
    p=skip_ws(p+1);
    while(p&&*p&&*p!='}'&&tmp_cnt<CHAT_MAX){
        const char *key_end=skip_str(p),*value,*value_end;
        if(!key_end)break;
        p=skip_ws(key_end);if(*p!=':')break;
        value=skip_ws(p+1);value_end=skip_val(value);if(!value_end)break;
        if(*value=='{'){
            ChatMsg *m=&tmp[tmp_cnt];
            strv(value,"uid",m->uid,sizeof(m->uid));
            strv(value,"nickname",m->nickname,sizeof(m->nickname));
            strv(value,"text",m->text,sizeof(m->text));
            m->ts=(unsigned long)num(value,"ts",0);
            if(m->uid[0]&&m->text[0]){m->valid=1;tmp_cnt++;}
        }
        p=skip_ws(value_end);if(*p==',')p=skip_ws(p+1);else if(*p!='}')break;
    }
    lock();net.chat_count=0;for(int i=0;i<tmp_cnt;i++)net.chats[net.chat_count++]=tmp[i];unlock();
}
static int pull_chat(char *resp, size_t cap){
    char url[URL];
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json?orderBy=%%22$key%%22&limitToLast=20",net.base,net.room);
    return http("GET",url,NULL,resp,cap)==200;
}
static void *thread_main(void *arg) {
    int fails=0; (void)arg;
    int verified=verify_saved_account();
    if(verified<=0){
        if(verified<0){account_set(NET_ACCOUNT_WRONG_PASSWORD,"Saved account no longer matches Firebase");LOGERR("account verification failed; online login denied");}
        else {account_set(NET_ACCOUNT_ERROR,"Could not verify account in Firebase");LOGERR("account verification network error");}
        status(NET_AUTH_ERROR);net.run=0;return NULL;
    }
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            status(NET_CONNECTING); slot=claim_slot();
            if(slot<0){ if(++fails>3){status(NET_ERROR); LOGERR("network error: cannot claim player slot in room '%s'", net.room);} sleep_ms(500); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.bullets[slot]=net.my_bullet; net.players[slot].online=1; unlock(); fails=0;
            LOG("slot %d claimed", (int)net.slot);
        }
        if(!push_state()){ if(++fails>3){status(NET_ERROR); LOGERR("network error: failed to push player state");} sleep_ms(300); continue; }
        fails=0; status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<WRITE_TICK)sleep_ms((int)(WRITE_TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_thread_main(void *arg){ thread_main(arg); return 0; }
#endif
static void *reader_thread(void *arg) {
    char resp[RESP];
    char chat_resp[CHAT_RESP];
    (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0){ sleep_ms(50); continue; }
        if(pull_state(resp,sizeof(resp))) read_players(resp);
        if(pull_chat(chat_resp,sizeof(chat_resp))) parse_and_store_chat(chat_resp);
        long long spent=now_ms()-start; if(spent<READ_TICK)sleep_ms((int)(READ_TICK-spent));
    }
    return NULL;
}
#ifdef _WIN32
static unsigned __stdcall win_reader_thread(void *arg){ reader_thread(arg); return 0; }
#endif
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
    if(net.writer_created||net.reader_created)net_disconnect();
    lock();
    if(net.account.status!=NET_ACCOUNT_READY){net.status=NET_AUTH_ERROR;unlock();LOGERR("online login denied: configure nickname and password first");return;}
    memset(&net.me,0,sizeof(net.me)); memset(&net.my_bullet,0,sizeof(net.my_bullet)); memset(net.players,0,sizeof(net.players)); memset(net.bullets,0,sizeof(net.bullets));
    memset(net.chats,0,sizeof(net.chats));
    snprintf(net.me.nickname,sizeof(net.me.nickname),"%s",net.account.nickname);
    net.count=0; net.chat_count=0; net.slot=-1; net.seq=0;
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    net.started=1;net.status=NET_CONNECTING;net.run=1;unlock();
#ifdef _WIN32
    net.thread=ds_thread_start(win_thread_main,NULL);
#else
    net.thread=ds_thread_start(thread_main,NULL);
#endif
    if(!net.thread){lock();net.run=0;net.status=NET_ERROR;unlock();LOGERR("network error: cannot start writer thread");return;}
    net.writer_created=1;
#ifdef _WIN32
    net.rthread=ds_thread_start(win_reader_thread,NULL);
#else
    net.rthread=ds_thread_start(reader_thread,NULL);
#endif
    if(!net.rthread){net.run=0;ds_thread_join(net.thread);net.writer_created=0;status(NET_ERROR);LOGERR("network error: cannot start reader thread");return;}
    net.reader_created=1;
    LOG("connect %s/%s (write %dms read %dms)",net.base,net.room,WRITE_TICK,READ_TICK);
}
void net_disconnect(void) {
    net.run=0;
    if(net.writer_created){ds_thread_join(net.thread);net.writer_created=0;}
    if(net.reader_created){ds_thread_join(net.rthread);net.reader_created=0;}
    lock();net.status=NET_OFFLINE;net.slot=-1;net.count=0;memset(net.players,0,sizeof(net.players));memset(net.bullets,0,sizeof(net.bullets));unlock();
}
void net_publish(double x,double y,double a,double hp,double alive) {
    if(!net.started) return;
    lock(); net.me.x=x; net.me.y=y; net.me.a=a; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; } unlock();
}
void net_publish_bullet(double x,double y,double dx,double dy,double active,double shot,double tr) {
    if(!net.started) return;
    lock(); net.my_bullet.x=x; net.my_bullet.y=y; net.my_bullet.dx=dx; net.my_bullet.dy=dy; net.my_bullet.active=active; net.my_bullet.shot=shot; net.my_bullet.tr=tr; if(net.slot>=0)net.bullets[net.slot]=net.my_bullet; unlock();
    if(active>0.5){
        push_bullet_only();
    }
}
void net_chat_send(const char *text){
    if(!net.started || !text || !*text) return;
    if(!net.run) return;
    char url[URL], body[BODY*2], esc[CHAT_TEXT_MAX*2], nick[NICK_MAX_BYTES*2+1];
    json_escape(text, esc, sizeof(esc));json_escape(net.account.nickname,nick,sizeof(nick));
    snprintf(url,sizeof(url),"%s/rooms/%s/chat.json",net.base,net.room);
    long long ts=now_ms();
    snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"nickname\":\"%s\",\"text\":\"%s\",\"ts\":%lld}",net.uid,nick,esc,ts);
    http("POST",url,body,NULL,0);
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
        strncpy(s_chat_uid_ret, net.chats[i].uid, sizeof(s_chat_uid_ret)-1);
        s_chat_uid_ret[sizeof(s_chat_uid_ret)-1]='\0';
    }else{
        s_chat_uid_ret[0]='\0';
    }
    unlock();
    return s_chat_uid_ret;
}
const char* net_chat_nickname(double idx){
    int i=(int)idx;
    lock();
    if(i>=0 && i<net.chat_count && net.chats[i].valid){
        strncpy(s_chat_nick_ret, net.chats[i].nickname, sizeof(s_chat_nick_ret)-1);
        s_chat_nick_ret[sizeof(s_chat_nick_ret)-1]='\0';
    }else s_chat_nick_ret[0]='\0';
    unlock();
    return s_chat_nick_ret;
}
double net_chat_time(double idx){
    int i=(int)idx; double v=0;
    lock(); if(i>=0 && i<net.chat_count) v=(double)net.chats[i].ts; unlock();
    return v;
}
static int sidx(double slot){ int i=(int)slot; return i>=0&&i<NET_SLOTS?i:-1; }
double net_status(void){ double v; lock(); v=net.status; unlock(); return v; }
double net_slot(void){ double v; lock(); v=net.slot; unlock(); return v; }
double net_count(void){ double v; lock(); v=net.count; unlock(); return v; }
#define READER(name, field) double name(double slot){ int i=sidx(slot); double v=0; if(i>=0){lock();v=field;unlock();} return v; }
READER(net_player_online, net.players[i].online?1:0)
READER(net_player_x, net.players[i].x)
READER(net_player_y, net.players[i].y)
READER(net_player_angle, net.players[i].a)
READER(net_player_hp, net.players[i].hp)
READER(net_player_alive, net.players[i].alive)
const char *net_player_nickname(double slot){int i=sidx(slot);lock();if(i>=0)snprintf(s_player_nick_ret,sizeof(s_player_nick_ret),"%s",net.players[i].nickname);else s_player_nick_ret[0]=0;unlock();return s_player_nick_ret;}
READER(net_player_bullet_active, net.bullets[i].active)
READER(net_player_bullet_x, net.bullets[i].x)
READER(net_player_bullet_y, net.bullets[i].y)
READER(net_player_bullet_dx, net.bullets[i].dx)
READER(net_player_bullet_dy, net.bullets[i].dy)
READER(net_player_bullet_shot, net.bullets[i].shot)
READER(net_player_bullet_tr, net.bullets[i].tr)
#undef READER
