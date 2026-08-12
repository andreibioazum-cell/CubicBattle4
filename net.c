#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "net.h"
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DimScriptNet", __VA_ARGS__)
#define URL 384
#define BODY 768
#define RESP 4096
#define TICK 100
#define TIMEOUT 3000
#define STALE 12000

typedef struct { double x,y,a,hp,alive; int online; } Actor;
typedef struct { double x,y,dx,dy,active,shot; } Bullet;

static struct {
    pthread_t thread; pthread_mutex_t lock;
    JavaVM *vm; int run, started, slot, status;
    char base[220], room[48], uid[24];
    Actor me; Bullet my_bullet; unsigned long seq, count;
    Actor players[NET_SLOTS]; Bullet bullets[NET_SLOTS];
} net;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void sleep_ms(int ms) {
    struct timespec t = { ms/1000, (long)(ms%1000)*1000000L };
    nanosleep(&t, NULL);
}
static double safe(double v) { return isnan(v)||isinf(v) ? 0 : v; }
static void lock(void) { pthread_mutex_lock(&net.lock); }
static void unlock(void) { pthread_mutex_unlock(&net.lock); }
static void status(int s) { lock(); net.status=s; unlock(); }

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
    if ((*env)->PushLocalFrame(env,24)!=0) goto done;
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
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setConnectTimeout","(I)V"),4000);
    (*env)->CallVoidMethod(env,conn,(*env)->GetMethodID(env,connc,"setReadTimeout","(I)V"),4000);
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
    buf=(*env)->NewByteArray(env,1024);
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
static int http(const char *method,const char *url,const char *body,char *out,size_t cap) {
    return http_ex(method,url,body,out,cap,NULL,NULL,NULL,0);
}

static const char *skip_ws(const char *p) { while(p&&*p&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; return p; }
static const char *skip_str(const char *p) { if(!p||*p!='"') return NULL; for(p++;*p;p++){ if(*p=='\\'&&p[1]){p++;continue;} if(*p=='"') return p+1; } return NULL; }
static const char *skip_box(const char *p,char open,char close) { int d=0; if(!p||*p!=open) return NULL; for(;*p;p++){ if(*p=='"'){p=skip_str(p); if(!p)return NULL; p--; continue;} if(*p==open)d++; else if(*p==close&&--d==0)return p+1;} return NULL; }
static const char *skip_val(const char *p) { p=skip_ws(p); if(!p||!*p)return NULL; if(*p=='"')return skip_str(p); if(*p=='{')return skip_box(p,'{','}'); if(*p=='[')return skip_box(p,'[',']'); while(*p&&*p!=','&&*p!='}'&&*p!=']')p++; return p; }
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
/* Firebase serializes dense numeric children (players/0, players/1, ...)
 * as JSON arrays.  Treat an array index like an object path component. */
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
static double num(const char *json,const char *path,double fb) { const char *v=path_val(json,path); if(!v||!strncmp(v,"null",4))return fb; if(*v=='t')return 1; if(*v=='f')return 0; if(*v=='"')v++; return atof(v); }
static void strv(const char *json,const char *path,char *out,size_t cap) {
    const char *v=path_val(json,path); size_t i=0; if(cap)out[0]=0; if(!v||*v!='"')return;
    for(v++;*v&&*v!='"'&&i+1<cap;v++){ if(*v=='\\'&&v[1])v++; out[i++]=*v; } out[i]=0;
}

static int push_state(void) {
    Actor a; Bullet b; int slot; unsigned long seq; char url[URL], body[BODY];
    lock(); a=net.me; b=net.my_bullet; slot=net.slot; seq=++net.seq; unlock();
    if(slot<0) return 0;
    snprintf(url,sizeof(url),"%s/rooms/%s.json",net.base,net.room);
    snprintf(body,sizeof(body),"{\"players/%d\":{\"uid\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"angle\":%.4f,\"hp\":%.0f,\"alive\":%.0f,\"seq\":%lu},\"bullets/%d\":{\"x\":%.1f,\"y\":%.1f,\"dx\":%.4f,\"dy\":%.4f,\"active\":%.0f,\"shot\":%.0f}}",slot,net.uid,safe(a.x),safe(a.y),safe(a.a),safe(a.hp),safe(a.alive),seq,slot,safe(b.x),safe(b.y),safe(b.dx),safe(b.dy),safe(b.active),safe(b.shot));
    return http("PATCH",url,body,NULL,0)==200;
}
static int pull_state(char *resp,size_t cap) { char url[URL]; snprintf(url,sizeof(url),"%s/rooms/%s.json",net.base,net.room); return http("GET",url,NULL,resp,cap)==200; }
static void release_slot(void) {
    int slot; char url[URL]; lock(); slot=net.slot; net.slot=-1; unlock(); if(slot<0)return;
    snprintf(url,sizeof(url),"%s/rooms/%s/players/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
    snprintf(url,sizeof(url),"%s/rooms/%s/bullets/%d.json",net.base,net.room,slot); http("DELETE",url,NULL,NULL,0);
}
static int claim_slot(void) {
    static unsigned long seen_seq[NET_SLOTS]; static long long seen_at[NET_SLOTS]; static char seen_uid[NET_SLOTS][24];
    char resp[RESP],url[URL],body[BODY],uid[24],etag[96]; long long t=now_ms(); int slot;
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
        snprintf(body,sizeof(body),"{\"uid\":\"%s\",\"x\":0,\"y\":0,\"angle\":0,\"hp\":0,\"alive\":0,\"seq\":0}",net.uid);
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
        ps[slot].online=1;
        snprintf(bp,sizeof(bp),"bullets/%d",slot);
        snprintf(p,sizeof(p),"%s/x",bp); bs[slot].x=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/y",bp); bs[slot].y=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dx",bp); bs[slot].dx=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/dy",bp); bs[slot].dy=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/active",bp); bs[slot].active=num(resp,p,0);
        snprintf(p,sizeof(p),"%s/shot",bp); bs[slot].shot=num(resp,p,0);
        count++;
    }
    lock(); memcpy(net.players,ps,sizeof(ps)); memcpy(net.bullets,bs,sizeof(bs)); net.count=count; unlock();
}
static void *thread_main(void *arg) {
    char resp[RESP]; int fails=0; (void)arg;
    while(net.run) {
        long long start=now_ms(); int slot;
        lock(); slot=net.slot; unlock();
        if(slot<0) {
            status(NET_CONNECTING); slot=claim_slot();
            if(slot<0){ if(++fails>3)status(NET_ERROR); sleep_ms(500); continue; }
            lock(); net.slot=slot; net.seq=0; net.players[slot]=net.me; net.bullets[slot]=net.my_bullet; net.players[slot].online=1; unlock(); fails=0;
        }
        if(!push_state()||!pull_state(resp,sizeof(resp))){ if(++fails>3)status(NET_ERROR); sleep_ms(300); continue; }
        fails=0; read_players(resp); status(NET_PLAYING);
        long long spent=now_ms()-start; if(spent<TICK)sleep_ms((int)(TICK-spent));
    }
    release_slot(); status(NET_OFFLINE); return NULL;
}
static void make_uid(void) {
    struct timespec t; unsigned long a,b; int local=0;
    clock_gettime(CLOCK_MONOTONIC,&t);
    a=(unsigned long)time(NULL)^((unsigned long)t.tv_nsec<<8);
    b=(unsigned long)getpid()^(unsigned long)(uintptr_t)&local;
    snprintf(net.uid,sizeof(net.uid),"%08lx%08lx",a&0xfffffffful,b&0xfffffffful);
}
void net_connect(const char *url,const char *room) {
    size_t n;
    if(net.run||!url||!*url)return;
    memset(&net.me,0,sizeof(net.me)); memset(&net.my_bullet,0,sizeof(net.my_bullet)); memset(net.players,0,sizeof(net.players)); memset(net.bullets,0,sizeof(net.bullets));
    net.count=0; net.slot=-1; net.seq=0;
    snprintf(net.base,sizeof(net.base),"%s",url); n=strlen(net.base); while(n&&net.base[n-1]=='/')net.base[--n]=0;
    snprintf(net.room,sizeof(net.room),"%s",(room&&*room)?room:"main");
    if(!net.uid[0])make_uid();
    if(!net.started){ pthread_mutex_init(&net.lock,NULL); net.started=1; }
    net.status=NET_CONNECTING; net.run=1;
    if(pthread_create(&net.thread,NULL,thread_main,NULL)){ net.run=0; net.status=NET_ERROR; return; }
    LOG("connect %s/%s",net.base,net.room);
}
void net_disconnect(void) { if(!net.run)return; net.run=0; pthread_join(net.thread,NULL); net.status=NET_OFFLINE; net.slot=-1; net.count=0; memset(net.players,0,sizeof(net.players)); memset(net.bullets,0,sizeof(net.bullets)); }
void net_shutdown(void) { net_disconnect(); }
void net_publish(double x,double y,double a,double hp,double alive) {
    if(!net.started) return;
    lock(); net.me.x=x; net.me.y=y; net.me.a=a; net.me.hp=hp; net.me.alive=alive;
    if(net.slot>=0){ net.players[net.slot]=net.me; net.players[net.slot].online=1; } unlock();
}
void net_publish_bullet(double x,double y,double dx,double dy,double active,double shot) {
    if(!net.started) return;
    lock(); net.my_bullet.x=x; net.my_bullet.y=y; net.my_bullet.dx=dx; net.my_bullet.dy=dy; net.my_bullet.active=active; net.my_bullet.shot=shot; if(net.slot>=0)net.bullets[net.slot]=net.my_bullet; unlock();
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
READER(net_player_bullet_active, net.bullets[i].active)
READER(net_player_bullet_x, net.bullets[i].x)
READER(net_player_bullet_y, net.bullets[i].y)
READER(net_player_bullet_dx, net.bullets[i].dx)
READER(net_player_bullet_dy, net.bullets[i].dy)
READER(net_player_bullet_shot, net.bullets[i].shot)
#undef READER
